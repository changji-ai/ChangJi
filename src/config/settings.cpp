#include "config/settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

// 只为了 `ProjectPaths::assets_file()` 和 `StyleProfile` 那个初值：没写
// `[video]` 的老项目，画幅要从它自己的 assets.json 推（见
// orientation_from_assets）。文件名和初值都从这儿拿，不在这边再抄一份。
#include "config/model_index.hpp"
#include "models/project.hpp"
#include "stages/limits.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::config {

namespace fs = std::filesystem;

// ---- 校验 ----
//
// 每一条都对应 Python 侧 config.py 里的一个 Field 约束或 field_validator。
// 改这里的时候必须同步改那边，反之亦然，直到 Python 删除为止。

namespace {

/// 去掉首尾空白和末尾斜杠。对应 Python 的 _strip_slash。
std::string strip_trailing_slash(std::string v) {
    size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = v.find_last_not_of(" \t\r\n");
    v = v.substr(b, e - b + 1);
    while (!v.empty() && v.back() == '/') v.pop_back();
    return v;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// 把一个数写成字符串。**照 `ostringstream <<` 的样子**，不是
/// `std::to_string`——后者固定六位小数，`0` 会变成 `0.000000`，那几句
/// 报错读起来就成了另一回事。这几条以前就是直接 `os << v` 的。
std::string num(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

/// ⚠️ **先判有没有限，再比大小。** NaN 和任何数比都是假，所以
/// `v < lo || v > hi` 对它一句话都不说——`nan` 是 toml 认的字面量
/// （toml++ 的 parser 直接给回 quiet_NaN），于是 `call_log_max_mb = nan`
/// 能静默过关，一路传到记录器那边算出上限 0，**刚写下的正文当场被删光**，
/// 而开关明明开着、index.jsonl 还在一行一行地长。症状和「配置填错了」
/// 完全对不上。`inf` 被 `> hi` 挡得住，`nan` 挡不住，所以要单独判。
void check_range(std::vector<std::string>& errs, const char* name,
                 double v, double lo, double hi) {
    if (!std::isfinite(v) || v < lo || v > hi) {
        errs.push_back(SAYF("%1 必须在 %2 到 %3 之间，当前是 %4", name, num(lo),
                            num(hi), num(v)));
    }
}

/// 同上：NaN 过不去这道门（`v <= lo` 对它也是假）。
void check_gt(std::vector<std::string>& errs, const char* name, double v, double lo) {
    if (!std::isfinite(v) || v <= lo) {
        errs.push_back(SAYF("%1 必须大于 %2，当前是 %3", name, num(lo), num(v)));
    }
}

/// 同上。**三道门要一起判**，不然 NaN 只是换一个字段溜进来。
void check_ge(std::vector<std::string>& errs, const char* name, double v, double lo) {
    if (!std::isfinite(v) || v < lo) {
        errs.push_back(SAYF("%1 不能小于 %2，当前是 %3", name, num(lo), num(v)));
    }
}

}  // namespace

std::vector<std::string> VideoConfig::validate() const {
    std::vector<std::string> errs;
    if (orientation != "portrait" && orientation != "landscape") {
        errs.push_back(SAYF("video.orientation 只能是 portrait 或 landscape，"
                            "现在是 %1",
                            orientation));
    }
    if (quality != "720p" && quality != "hd" && quality != "2k") {
        errs.push_back(SAYF("video.quality 只能是 720p、hd 或 2k，现在是 %1",
                            quality));
    }
    // 0 = 自己定。填了就得是个能排出镜头的数：最短的档位是 2 秒。
    if (max_shot_s < 0.0 || (max_shot_s > 0.0 && max_shot_s < 2.0)) {
        errs.push_back(SAYF("video.max_shot_s 要么是 0（按模型和显卡自己定），"
                            "要么至少 2 秒，现在是 %1",
                            std::to_string(max_shot_s)));
    }
    // 上限 4：每多一条就是多一镜的显卡时间，关键镜头两三条够挑了。
    check_range(errs, "video.hero_takes", hero_takes, 1, 4);
    return errs;
}

std::vector<std::string> PreviewConfig::validate() const {
    std::vector<std::string> errs;
    // 下限 5 秒：比一镜还短的"前 n 分钟"挑出来就一镜，没什么可看的。
    // 上限一小时：再长就是整章，那条路是「出片」，不是这一条。
    if (seconds < 5.0 || seconds > 3600.0) {
        errs.push_back(SAYF("preview.seconds 要在 5 秒到 1 小时之间，现在是 %1",
                            std::to_string(seconds)));
    }
    return errs;
}

std::pair<int, int> VideoConfig::size() const {
    // **两边都取 32 的倍数**——Wan 那一族的潜空间要求，不对齐的话
    // 出图直接失败，而日志里指不到这儿。
    //
    // 标准档 **544×928**（2026-09-10 用户定的，原话"将 720p 改成 544*920"）。
    // 544 = 32 × 17；**920 除不尽（÷32 = 28.75），取最近的 928 = 32 × 29**。
    // 差的那 8 个像素在手机上看不出来，而对齐这件事是硬约束——第一版这里
    // 写的是 720，单元测试当场抓住了，那条用例存在的全部意义就是这个。
    //
    // 换掉之前是 704×1280。像素数从 90 万降到 50 万（约 -44%），出片更快。
    // 名字仍然叫 "720p"：那是**存在每个项目 changji.toml 里的取值**，
    // 改了名老项目就读不出来了。界面上跟着显示真实尺寸，不靠这个名字。
    //
    // 2K 那两个数（1440 = 32 × 45、2560 = 32 × 80）本来就整除。
    //
    // **2K 一张 32 GB 的卡跑不动**：1280×704 时计算缓冲就已经 ~14.6 GB，
    // 2K 是它四倍像素。跑不动时由上层决定怎么办（换大卡，或者出标准档
    // 再 `changji --upscale`）。**在这里悄悄降档是不行的**——
    // 用户选了 2K 却拿到标准档，而且没有任何提示。
    // **三档，全部 32 对齐。**
    //   720p → 544×928   省时间，一镜约 2 分钟
    //   hd   → 704×1280   2026-09-11 加回来的。改成 544×928 之后用户说
    //                     "糊掉、变形"，而同一章里 sh001 是 704×1280、
    //                     sh002 是 544×928，像素 90 万 vs 50 万，差 44%。
    //                     这一档是 9-10 之前一直在用、用户认可过的那个。
    //   2k   → 1440×2560  一张 32 GB 的卡跑不动，见下面
    //
    // **不做成"自己填宽高"**：填出个不是 32 倍数的值，出图直接失败而且
    // 日志里指不到那儿。三个定好的档位挡住了这一整类问题。
    int long_side = 928;
    int short_side = 544;
    if (quality == "2k") {
        long_side = 2560;   // 32 × 80
        short_side = 1440;  // 32 × 45
    } else if (quality == "hd") {
        long_side = 1280;   // 32 × 40
        short_side = 704;   // 32 × 22
    }
    return orientation == "landscape"
               ? std::pair<int, int>{long_side, short_side}
               : std::pair<int, int>{short_side, long_side};
}

std::optional<std::string> orientation_of_aspect(const std::string& aspect_ratio) {
    // **照着正向那个函数比**，不另写一份映射：这样这张表只有一处。
    for (const char* o : {"landscape", "portrait"}) {
        VideoConfig probe;
        probe.orientation = o;
        if (probe.aspect_ratio() == aspect_ratio) return std::string(o);
    }
    return std::nullopt;
}

std::vector<std::string> TiersConfig::validate() const {
    std::vector<std::string> errs;
    // **分辨率必须是 32 的倍数**，否则 Wan 那一族的潜空间对不齐。
    // 这一条以前只在接口层查（config_api.cpp），从配置文件进来是绕过的——
    // 而绕过之后的症状是出图直接失败，日志里指不到这儿。
    const std::pair<const char*, int> res[] = {
        {"tiers.draft_width", draft_width}, {"tiers.draft_height", draft_height},
        {"tiers.final_width", final_width}, {"tiers.final_height", final_height},
    };
    for (const auto& [name, v] : res) {
        if (v < 0) errs.push_back(SAYF("%1 不能是负的", name));
        if (v > 0 && v % 32 != 0) {
            errs.push_back(
                SAYF("%1 要是 32 的倍数，现在是 %2", name, std::to_string(v)));
        }
    }
    for (const auto& [name, v] : {std::pair<const char*, int>{"tiers.draft_steps", draft_steps},
                                  {"tiers.final_steps", final_steps}}) {
        if (v < 0) errs.push_back(SAYF("%1 不能是负的", name));
    }
    return errs;
}

std::string LLMConfig::model_for(const std::string& task) const {
    const auto it = task_models.find(task);
    return it == task_models.end() || it->second.empty() ? model : it->second;
}

std::string LLMConfig::effort_for(const std::string& task) const {
    // **空串在这儿是"没点名"，不是"这一步不要发"。** 想让某一步不发这个
    // 字段，把全局那个 `reasoning_effort` 清空——按任务关掉单独一步，和
    // "没配过"在 TOML 里长得一模一样，分不开。
    const auto it = task_efforts.find(task);
    if (it != task_efforts.end() && !it->second.empty()) return it->second;

    // ---- 各步自己的默认，配置没点名时用 ----
    //
    // **写正文默认 low**（2026-09-20 用户拍的，数是这么量的）：
    //
    //   一次 315 秒 → **64 秒**（快 4.9 倍）
    //   思考是正文的 8.2 倍 → **0.4 倍**（少二十倍）
    //   闸门第一轮通过率 50%（high, n=48）→ 67%（low, n=16）——样本小，
    //     只能说**不比 high 差**，不能说更好
    //   采 1000 条训练样本：42 小时 → **10 小时**
    //
    // 思考那部分用户一个字都看不到，而正文这一步占全片输出 token 的九成
    // ——省的就是这一笔。
    //
    // ⚠️ **low 的代价单独治过**：它让模型 JSON 写得糙，2026-09-20 之前
    // 355 次正文调用里 70 次（20%）把对白写成 ASCII 直引号不转义、整章
    // 作废（high 档 355 次只坏 1 次）。那两层修补（json_extract 的
    // `escape_stray_quotes` + chapter_write 的双引号归一）已经进去了，
    // 所以这个代价现在小得多——但**它是先修了洞才改的默认，顺序别倒过来**。
    //
    // 别的步骤照旧走全局那个 high：出大纲、拆分镜都实测过 max 会让模型
    // 在思考里把成品写一遍（见 `reasoning_effort` 上那段），low 那一档
    // 它们没量过，没数就别改。
    static const std::map<std::string, std::string> kPerTask = {
        {"chapter", "low"},
    };
    const auto d = kPerTask.find(task);
    if (d != kPerTask.end()) return d->second;
    return reasoning_effort;
}

double LLMConfig::temperature_for(const std::string& task) const {
    // 要发散的：编东西那几步。往上推一档。
    static const std::set<std::string> kLoose = {"story_outline", "premises",
                                                 "trailer"};
    // 要听话的：把已有的东西转成结构那几步。压到基准的四成上下。
    static const std::set<std::string> kTight = {"bible", "storyboard",
                                                 "story_analysis"};
    if (kLoose.count(task) != 0) return std::min(temperature + 0.25, 1.1);
    if (kTight.count(task) != 0) return temperature * 0.4;
    return temperature;
}

bool LLMConfig::needs_api_key() const {
    if (backend == "local") return false;
    // 只认几个明确的本机/私网写法，别的一律当云。172.16~172.31 是一整段
    // 私网，这里按前缀列——写不全的那几个（172.2x）落到"当云"那一侧，
    // 代价只是多提示一句。
    static const char* kLocal[] = {"//127.0.0.1", "//localhost", "//0.0.0.0",
                                   "//[::1]",     "//192.168.",  "//10.",
                                   "//172.16.",   "//172.17.",   "//172.18.",
                                   "//172.19.",   "//172.30.",   "//172.31."};
    for (const char* m : kLocal) {
        if (base_url.find(m) != std::string::npos) return false;
    }
    return true;
}

std::vector<std::string> LLMConfig::validate() const {
    std::vector<std::string> errs;
    // 三条路：remote（打 API）、local（进程内，2026-09-19 接回来的）、
    // command（本机命令行）。local 要的权重（[models].llm）在不在，由体检
    // 和模型窗那头说，不在这儿拦——拦了老机器起不来，人只看到一行 stderr。
    if (backend != "remote" && backend != "local" && backend != "command") {
        errs.push_back(SAYF("llm.backend 只能是 remote、local 或 command，"
                            "当前是 %1",
                            backend));
    }
    // 选了命令行这条就得说清跑哪个程序。**不给默认值**：默认成 claude 的话，
    // 没装它的机器会在每一次生成时才失败，而这儿本可以一句话说清。
    if (backend == "command" && command.empty()) {
        errs.push_back(
            SAY("llm.backend 是 command，但没填 llm.command（要跑哪个程序，"
                "比如 claude 或 codex）"));
    }
    check_gt(errs, "llm.command_timeout_s", command_timeout_s, 0);
    check_gt(errs, "llm.timeout_s", timeout_s, 0);
    check_range(errs, "llm.temperature", temperature, 0.0, 2.0);
    // 想多久：**认得的档才放行**，拼错一个字母的代价是服务回 400，而那一下
    // 会让所有生成一起挂——在这儿拦住，人看到的是一句能读懂的话。
    // 空串是"不发这个字段"，合法。
    {
        // ⚠️ **这张表只挡拼写错，不决定发什么。**
        //
        // 哪一家的哪个模型认哪几档、字段叫什么，全在 `llm/thinking.hpp`
        // 那一张表里；这儿放宽一点，把那张表里的键（`kThinkTiers`）全收下，
        // 外加 `""`（不发这个字段）和两个老值。收窄的后果是"界面上能选、
        // 存下去被顶回来"，而那句 422 在那条路上没人看，表现成点了没反应
        //（2026-09-21：`off` 那一档就差点这么掉进去）。
        //
        // 守卫 `test_qml_effort.cpp` 钉着「`kThinkTiers` 里的每一个这儿都得有」。
        static const std::set<std::string> kEfforts = {
            "", "minimal", "low", "medium", "high", "max", "off", "ultra"};
        auto check_effort = [&errs](const std::string& where,
                                    const std::string& v) {
            if (kEfforts.count(v) == 0) {
                errs.push_back(
                    SAYF("%1 只能是 off、minimal、low、medium、high、max、"
                         "ultra，或者留空（不发这个字段），当前是 %2",
                         where, v));
            }
        };
        check_effort("llm.reasoning_effort", reasoning_effort);
        for (const auto& [task, v] : task_efforts) {
            check_effort("llm.effort." + task, v);
        }
    }
    // **两头都要夹**，症状是同一个：日志目录永远是空的，而人看到的是开关明明
    // 开着、`index.jsonl` 还在一行一行地长。这个数要乘 1024×1024 变成一个
    // `uintmax_t` 的字节上限（`llm/call_log.cpp` 的 `call_log_options`），
    // 而「超了就剪」紧跟在写完正文之后跑，上限算成 0 就是把刚写下的那一份
    // 当场删掉。
    //   · 下界：0 和负数直接就是 0 字节。
    //   · 上界：`call_log_max_mb = 1e30` 这种（设置页上手滑多按几个 0 也一样，
    //     `/api/connections` 那头只判是不是数）转成 `uintmax_t` 是未定义行为，
    //     实测常得 0 —— 填得越大反而一份都留不住。
    // 判据写成正面的：**这个上限至少要装得下几十次调用，又要小到装得进磁盘**。
    // 一次调用的正文（提示词 + 回复 + 思考）几十 KB，所以下界取 1 MB；想彻底
    // 不留就关 `call_log`，那条路清楚得多，不用让人猜「填多小算关」。
    // 上界取 100 GB：比这份日志在任何一台机器上该占的都大得多，而多按几个零
    // 一定越过它。**别写成 1024*1024**（1 TB）：错误消息里那个数是
    // `ostringstream` 默认精度打的，七位有效数字会变成 `1.04858e+06`，
    // 而这句话是起服务时给人看的。
    check_range(errs, "llm.call_log_max_mb", call_log_max_mb, 1.0, 102400.0);
    // 上限给 8：再多也开不出来（每个上下文一份 KV cache），而写得离谱
    // 会在起服务时白白试八次、每次失败都往 stderr 上打一行。
    check_range(errs, "llm.parallel", static_cast<double>(parallel), 1.0, 8.0);
    // 0 表示"用模型训练时的长度"，别的值至少要放得下一次输出（8192）加上
    // 一条像样的提示词。给 1024 的话它连规则都塞不下，而症状是每次生成都
    // 报"提示词太长"——不如在这儿就说清楚。
    if (context_tokens != 0) {
        check_range(errs, "llm.context_tokens",
                    static_cast<double>(context_tokens), 10240.0, 200000.0);
    }
    return errs;
}

std::vector<std::string> TTSConfig::validate() const {
    std::vector<std::string> errs;
    if (backend != "http" && backend != "local") {
        errs.push_back(
            backend == "comfy"
                ? SAY("tts.backend = \"comfy\" 已经不支持了："
                      "ComfyUI 那条路已拆除。改成 \"local\"（进程内跑，"
                      "要填 [models].tts 和 tts_decoder）或者 "
                      "\"http\"（外部配音服务，要填 [tts].base_url）。")
                : SAYF("tts.backend 只能是 http 或 local，当前是 %1", backend));
    }
    // Python 侧这一条在 doctor 里查而不是在模型里查，这里保持一致，
    // 避免配置加载阶段就因为还没填地址而整个起不来。
    return errs;
}

std::vector<std::string> GateConfig::validate() const {
    std::vector<std::string> errs;
    check_ge(errs, "gates.min_pixel_std", min_pixel_std, 0);
    check_ge(errs, "gates.min_pixel_mean", min_pixel_mean, 0);
    check_ge(errs, "gates.max_pixel_mean", max_pixel_mean, 0);
    check_range(errs, "gates.min_frame_similarity", min_frame_similarity, 0.0, 1.0);
    check_gt(errs, "gates.max_audio_drift_s", max_audio_drift_s, 0);
    check_ge(errs, "gates.max_attempts_per_shot", max_attempts_per_shot, 1);
    return errs;
}

std::vector<std::string> AssemblyConfig::validate() const {
    std::vector<std::string> errs;
    check_range(errs, "assembly.fps", fps, 1, 120);
    check_range(errs, "assembly.crf", crf, 0, 51);
    check_range(errs, "assembly.audio_sample_rate", audio_sample_rate, 8000, 192000);
    check_range(errs, "assembly.audio_channels", audio_channels, 1, 2);
    check_range(errs, "assembly.scene_transition_s", scene_transition_s, 0.0, 2.0);
    check_range(errs, "assembly.subtitle_max_chars_per_line",
                subtitle_max_chars_per_line, 6, 30);
    check_range(errs, "assembly.subtitle_max_lines", subtitle_max_lines, 1, 3);
    return errs;
}

std::vector<std::string> LookConfig::validate() const {
    std::vector<std::string> errs;
    if (preset != "film" && preset != "clean" && preset != "off") {
        errs.push_back(
            SAYF("look.preset 只能是 film / clean / off，现在是 %1", preset));
    }
    check_range(errs, "look.lut_strength", lut_strength, 0.0, 1.0);
    check_range(errs, "look.grain", grain, 0.0, 100.0);
    // 超过 1 像素就真糊了，这一项的意义只是压掉 AI 的微锐化。
    check_range(errs, "look.soften", soften, 0.0, 1.0);
    if (letterbox != 0.0 && (letterbox < 1.34 || letterbox > 3.0)) {
        errs.push_back(
            SAYF("look.letterbox 要么是 0（不遮幅），要么在 1.34～3.0 之间"
                 "（1.85、2.39 这种），现在是 %1",
                 std::to_string(letterbox)));
    }
    return errs;
}

std::vector<std::string> SoundConfig::validate() const {
    std::vector<std::string> errs;
    // 「压在台词下」只能是往下压。填正数等于把环境声顶到台词上面，
    // 而那不会报错，只是成片里听不清人说话。
    check_range(errs, "sound.ambient_db", ambient_db, -60.0, 0.0);
    check_range(errs, "sound.music_db", music_db, -60.0, 0.0);
    check_gt(errs, "sound.music_timeout_s", music_timeout_s, 0.0);
    return errs;
}

std::vector<std::string> UpscaleConfig::validate() const {
    std::vector<std::string> errs;
    check_range(errs, "upscale.scale", scale, 1, 4);
    check_gt(errs, "upscale.timeout_s", timeout_s, 0.0);
    return errs;
}

fs::path ModelsConfig::dir_path(const fs::path& workspace) const {
    if (dir && !dir->empty()) {
        std::error_code ec;
        fs::path p = paths::expand_user(*dir);
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    // 回落到项目库旁边。模型和项目放一起，整个 workspace 拷到另一台机器
    // 就能直接跑——不然还得记得单独拷模型。
    return workspace / "models";
}

fs::path ModelsConfig::resolve(const std::string& entry,
                               const fs::path& workspace) const {
    if (entry.empty()) return {};
    fs::path p = paths::expand_user(entry);
    // 绝对路径原样用。多台机器共享一个网络盘时会这么填。
    if (p.is_absolute()) return p;
    const fs::path root = dir_path(workspace);
    std::error_code ec;
    fs::path abs = fs::absolute(root / p, ec);
    if (ec) abs = root / p;
    // **拼出来的位置上没有，就按文件名在模型目录里找一遍。** 新下的按类型进了
    // 子目录（`image/Qwen_Image-Q8_0.gguf`），配置里记的还是那个名字；老版本
    // 下的平铺在根上；人自己拷进来的放哪儿都有。理由写在 model_index.hpp 上。
    // 哪儿都没有就照旧回拼出来的那个——体检拿它说"缺哪个文件、该放哪儿"。
    std::error_code ex;
    if (!fs::exists(abs, ex)) {
        if (auto hit = find_model(root, paths::to_utf8(p))) return *hit;
    }
    return abs;
}

std::vector<std::string> ModelsConfig::validate() const {
    // **只查 engine，模型文件一个都不查。**
    //
    // engine 是枚举，写错了不是"文件缺了"而是"整条出片的路走岔了"，
    // 而走岔的表现是连不上 ComfyUI 或者报"没有编进出图后端"——
    // 两句话都指不到真正的原因（拼写错误）。
    //
    // 文件这边能查的只有"在不在"，而那件事不该在配置加载阶段做：
    // 模型动辄好几个 G，装好程序还没下模型是常态。那时候如果加载直接失败，
    // 用户连界面都进不去，也就没法在界面里看到到底缺哪个文件。
    // 存在性检查在 doctor 里，报警告，程序照常起来。
    std::vector<std::string> errs;
    if (engine != "sd") {
        // ComfyUI 那条 2026-09-10 拆掉了。**老配置要给出迁移说明**，
        // 只说"只能是 sd"的话用户不知道自己那套工作流该怎么办。
        errs.push_back(
            engine == "comfy"
                ? SAY("models.engine = \"comfy\" 已经不支持了："
                      "ComfyUI 那条路已拆除，出图出片都走进程内的 "
                      "sd.cpp。改成 \"sd\"，并在 [models] 里填 "
                      "image / video 那几个模型文件。")
                : SAYF("models.engine 只能是 sd，当前是 %1", engine));
    }
    // Qwen-Image-Edit 2509 起，编码器要带视觉塔才看得见参考图。没带的话
    // sd.cpp 只打一句 "no vision weights detected, vision disabled"，然后
    // 照常跑——参考图只剩 VAE 潜空间那一半进 DiT，出来的图对不上参考。
    // 上游 docs/qwen_image_edit.md 的 2509 例子带着 --llm_vision，初版
    // Edit 的例子不带。2511 还要 model_args（出图那边自己加，见 sd_image.cpp）。
    if (accepts_reference_images(image) &&
        (image.find("2509") != std::string::npos ||
         image.find("2511") != std::string::npos) &&
        image_text_encoder_vision.empty()) {
        errs.push_back(
            SAY("[models].image 是 Qwen-Image-Edit 2509/2511，要一起填 "
                "image_text_encoder_vision（Qwen2.5-VL-7B 的 mmproj）：不填的话"
                "编码器看不见参考图，sd.cpp 只在日志里说一句 vision disabled 就"
                "照常出图，出来的和参考图对不上"));
    }
    return errs;
}

// 出图出片那几个实测数，**只有这一份**。
//
// 判断"装不装得下"（weights_for / image_weights_for）和回答"要占多少"
// （*_live_vram_gb）用的是同一组数，抄两份迟早只改一处。
namespace {
/// 视频 1280×704 的计算缓冲。5090（32.6 GB）上 H3 18.8 GB 常驻时跑到
/// 第 46/51 段差 788 MB，也就是 32.6 − 18.8 = 13.8 还差一点，它要 ~14.6。
/// **已含驱动余量**（那 788 MB 是连驱动一起差的），所以直接和整卡比，不乘 0.9。
constexpr double kVideoBuffer = 14.6;
/// kVideoBuffer 是在哪个画布上量的。1280 × 704 = 901120 像素。
constexpr double kVideoBufferAnchorPx = 1280.0 * 704.0;
/// VAE 也常驻要再加这么多（放内存的话每镜解码多花 63 秒）。
constexpr double kVideoVae = 5.5;
/// 图像 1280×704 的解码缓冲，实测。
constexpr double kImageDecode = 6.6;
/// 采样缓冲，加上别的上下文的残留——视频上下文卸掉之后 CUDA 还占 1.4 GB。
constexpr double kImageSlack = 4.0;
/// 大模型权重之外还要的那部分**不是常数**：KV 缓存跟着层数和上下文走，
/// 大模型就大。所以按倍数加常数，不是直接加一个数。
/// 锚点是 5090 上量的：Qwen3-14B-Q4_K_M 文件 9.0 GB，载进去 15.4 GB
/// （9×1.25 + 4 = 15.25，对得上）。
constexpr double kLlmWeightFactor = 1.25;
constexpr double kLlmOverhead = 4.0;

/// 这块画布上的计算缓冲。
///
/// **只往上放大，不往下缩小。** 缓冲里有一部分是不随画布变的（CUDA 上下文、
/// 常驻工作区、驱动余量），而我们**只有一个锚点**（1280×704 那次），
/// 分不出固定和可变各占多少。往下缩会把固定那部分一起缩掉，于是在小画布上
/// 低估——低估的后果是"以为装得下"然后 OOM，比高估严重得多。往上放大则是
/// 线性外推：注意力开了 flash-attention（sd.cpp 的 --diffusion-fa），
/// 激活显存随 token 数走，而 token 数随像素走，一次幂不是二次幂。
///
/// 要换成真曲线，得在目标机器上按 704p / 1080p / 2K 各量一次峰值显存。
/// **不能拿 5090 量**——那张卡装不下 2K，量不到那一段。
double video_buffer_gb(double canvas_px) {
    if (canvas_px <= 0.0) return kVideoBuffer;   // 不知道画布，按锚点算
    const double factor = canvas_px / kVideoBufferAnchorPx;
    return kVideoBuffer * (factor > 1.0 ? factor : 1.0);
}
}  // namespace

std::string ModelsConfig::weights_for(double vram_gb, double model_gb,
                                     bool unified, double canvas_px) const {
    // 计算缓冲跟着画布走，见 video_buffer_gb。不传画布就按锚点算。
    const double buffer = video_buffer_gb(canvas_px);
    if (weights != "smart") return weights;
    // **文本编码器永远放内存。** 它每镜只跑一次（H3 的 Qwen3-VL-32B 实测
    // 8 到 9 秒），而它是这一套里最大的一块（18.9 GB）。放显存换来的那几秒
    // 远不如把地方让给扩散模型。
    //
    // 拿不到模型大小按装不下处理：猜错是整章出片失败，放内存只是慢。
    if (model_gb <= 0.0) return "cpu";
    // 缓冲那个数是怎么来的写在 kVideoBuffer / video_buffer_gb 头上。
    if (model_gb + buffer > vram_gb) return "cpu";   // 权重都常驻不下

    // ---- 统一内存（苹果芯片）：装得下就一个都别往内存放 ----
    //
    // ⚠️ **上面那句"文本编码器永远放内存"是独显的算法，在这种机器上是纯亏。**
    //
    // 独显上"放内存"换的是显存：权重待在系统内存里，用到才走一趟 PCIe
    // 搬进去。统一内存上这笔交易的两头都不成立——
    //   - 没有"另一块内存"：CPU 和 GPU 指的是同一片物理内存，把权重挪到
    //     "内存"里并不会让 GPU 多出哪怕一个字节；
    //   - 没有那趟搬运：也就没有"省显存换一点慢"这回事，只剩下把计算
    //     赶去 CPU 跑（UMT5-XXL 在 CPU 上 8 到 9 秒，在 GPU 上一两秒）。
    //
    // 所以顺序反过来：**默认全常驻**，只有真的超过 Metal 那条线
    // （recommendedMaxWorkingSetSize，见 models/hardware.cpp）才开始退让。
    // 退让仍然有意义——ggml 的 CPU 缓冲不算进那条线里，超了系统会开始
    // 压缩换页，那比把编码器放 CPU 慢得多。
    if (unified) {
        const bool all_fits = model_gb + buffer + kVideoVae <= vram_gb;
        return all_fits ? "gpu" : "te=cpu";
    }

    // VAE 也常驻要再加 5.5 GB（放内存每镜解码多花 63 秒）。既要真装得下，
    // 也要过 vae_vram_min_gb 那道门槛。
    const bool vae_fits = model_gb + buffer + kVideoVae <= vram_gb;
    return (vram_gb >= vae_vram_min_gb && vae_fits) ? "te=cpu" : "te=cpu,vae=cpu";
}

std::string ModelsConfig::image_weights_for(double vram_gb, double model_gb,
                                           bool unified) const {
    if (image_weights != "smart") return image_weights;
    // 常驻要放得下：权重本身 + 1280×704 解码缓冲 6.6 GB（实测）+ 采样缓冲和
    // 别的上下文的残留（视频上下文卸掉之后 CUDA 还占 1.4 GB）约 4 GB。
    // 卡按九成算——那一成是驱动和别的程序的。拿不到模型大小按装不下处理：
    // 猜错的代价是六镜首帧全废，而放内存只是慢。
    const double need = model_gb + kImageDecode + kImageSlack;
    if (model_gb <= 0.0) return "cpu";
    // 统一内存上理由同 weights_for：装得下就全常驻，把编码器和 VAE 赶去
    // CPU 换不来任何地方。装不下才退回原来那条阶梯。
    if (unified) return vram_gb * 0.9 >= need ? "gpu" : "te=cpu,vae=cpu";
    // **装得下就放显存——VAE 也是。**
    //
    // 这一支原来返回 `te=cpu,vae=cpu`：扩散常驻，文本编码器和 VAE 都赶去
    // 内存。文本编码器（Qwen2.5-VL bf16 16.5 GB）赶走是对的，它大；
    // **VAE 只有 0.24 GB**，把它也赶走省不出任何东西，却把解码整段
    // 搬到了 CPU 上。
    //
    // 代价实测出来了（2026-09-15，L20 45 GB）：一镜 544×928 要 78 块
    // VAE 解码，这一段在 CPU 上跑，屏幕上是「准备 6/78 → 78/78」，
    // 而 `nvidia-smi` 每秒采样 40 次里 31 次是 0%——卡干等着。一镜墙钟
    // 42 秒，GPU 只动了 9 秒。
    //
    // 判据不变（还是"扩散 + 缓冲装不装得下"）：VAE 那 0.24 GB 和缓冲的
    // 10.6 GB 比可以忽略，装得下扩散就一定装得下它。所以这里不需要多要
    // 一个参数，只是把本来就该留在显存里的那一份留下。
    return vram_gb * 0.9 >= need ? "te=cpu" : "cpu";
}

// 常驻权重：规格里写了 vae=cpu 就只有扩散那份，写了整个 "cpu" 就一份都不常驻。
// 缓冲不管放哪都要，所以它在两个函数里都是无条件加上的。
double ModelsConfig::video_live_vram_gb(const std::string& placement,
                                        double model_gb,
                                        double canvas_px) const {
    // **必须和 weights_for 用同一个画布**，否则会出现"那边说装得下、
    // 这边说占不下"这种自相矛盾。
    const double buffer = video_buffer_gb(canvas_px);
    if (placement == "cpu") return buffer;
    // te=cpu：文本编码器在内存，扩散和 VAE 都常驻。
    if (placement == "te=cpu") return model_gb + buffer + kVideoVae;
    // te=cpu,vae=cpu：只有扩散常驻。
    // auto / 别的自定义规格按"全常驻"算——宁可估高，估高只是多卸一次，
    // 估低是 OOM。
    if (placement == "te=cpu,vae=cpu") return model_gb + buffer;
    return model_gb + buffer + kVideoVae;
}

double ModelsConfig::image_live_vram_gb(const std::string& placement,
                                        double model_gb) const {
    const double buffer = kImageDecode + kImageSlack;
    if (placement == "cpu") return buffer;
    return model_gb + buffer;
}

double ModelsConfig::llm_live_vram_gb(double model_gb) const {
    // 拿不到文件大小就别猜。返回 0 表示"没有老实数"，调用方会退回保守估算——
    // 猜小了是 OOM，而退回保守只是多卸一次。
    if (model_gb <= 0.0) return 0.0;
    return model_gb * kLlmWeightFactor + kLlmOverhead;
}

std::vector<std::string> Settings::validate() const {
    std::vector<std::string> errs;
    auto merge = [&errs](std::vector<std::string> more) {
        errs.insert(errs.end(), std::make_move_iterator(more.begin()),
                    std::make_move_iterator(more.end()));
    };
    merge(llm.validate());
    merge(tiers.validate());
    merge(video.validate());
    merge(preview.validate());
    merge(tts.validate());
    merge(gates.validate());
    merge(assembly.validate());
    merge(look.validate());
    merge(sound.validate());
    merge(upscale.validate());
    merge(models.validate());
    if (vram_gb_override && *vram_gb_override <= 0) {
        errs.push_back(SAY("vram_gb_override 必须大于 0"));
    }
    if (!models.video_high_noise.empty() && models.video.empty()) {
        // 只填高噪声那份是配错了，而症状会是"出的片和以前一样"——
        // 高噪声那份被静默忽略，看不出来。
        errs.push_back(
            SAY("[models].video_high_noise 填了但 [models].video 是空的："
                "双专家要两份，video 填低噪声那份"));
    }
    if (models.video_lora_tiers != "draft" &&
        models.video_lora_tiers != "final" &&
        models.video_lora_tiers != "both") {
        errs.push_back(
            SAYF("[models].video_lora_tiers 只能是 draft / final / both，"
                 "现在是 %1",
                 models.video_lora_tiers));
    }
    if (models.vae_vram_min_gb < 0) {
        errs.push_back(SAY("[models].vae_vram_min_gb 不能是负的"));
    }
    if (models.video_vae_tile < 0) {
        errs.push_back(SAY("[models].video_vae_tile 不能是负的（0 = 用内置值）"));
    }
    // 这三项一错，分镜、渲染、配音、装配会一起走偏，而且都不报错，
    // 所以在配置这一关就拦住。
    // 三项都是 0 = 按模型自己认，所以只拦负数和"只填了一半"。
    if (models.video_max_frames < 0) {
        errs.push_back(
            SAY("[models].video_max_frames 不能是负的（0 = 按模型自己认）"));
    }
    if (models.video_frame_step < 0 || models.video_frame_base < 0) {
        errs.push_back(SAY("[models].video_frame_step / video_frame_base "
                           "不能是负的（0 = 按模型自己认）"));
    }
    if (models.video_frame_base > 0 && models.video_frame_step <= 0) {
        errs.push_back(
            SAY("[models].video_frame_base 填了就要一起填 video_frame_step："
                "帧数的格子是 step*k + base，只给 base 定不出来"));
    }
    if (models.video_rng != "auto" && models.video_rng != "cuda" &&
        models.video_rng != "cpu" && models.video_rng != "std") {
        errs.push_back(
            SAYF("[models].video_rng 只能是 auto / cuda / cpu / std，现在是 %1",
                 models.video_rng));
    }
    if (!models.video_llm.empty() && !models.video_text_encoder.empty()) {
        // 两个参数位只能填一个。都填了的话下面按 llm 走、t5xxl 那份被
        // 静默丢掉——症状是"出的片和提示词没关系"，没有任何报错指到这儿。
        errs.push_back(
            SAY("[models].video_llm 和 video_text_encoder 只能填一个："
                "前者是 llm_path（MiniMax-H3 那类），后者是 t5xxl_path"
                "（Wan 那类）"));
    }
    if (models.video_moe_boundary <= 0 || models.video_moe_boundary >= 1) {
        errs.push_back(
            SAYF("[models].video_moe_boundary 要在 0 和 1 之间，现在是 %1",
                 std::to_string(models.video_moe_boundary)));
    }
    if (models.vram_reserve_gb < 0) {
        errs.push_back(SAY("[models].vram_reserve_gb 不能是负的"));
    }
    if (models.frame_steps < 0) {
        errs.push_back(SAYF("[models].frame_steps 不能是负数，现在是 %1",
                            std::to_string(models.frame_steps)));
    }
    if (models.frame_tier != "draft" && models.frame_tier != "final") {
        errs.push_back(SAYF("[models].frame_tier 只能是 draft 或 final，现在是 %1",
                            models.frame_tier));
    }
    if (models.weights.empty()) {
        errs.push_back(
            SAY("[models].weights 不能是空的（要 cpu / auto / 组件规格）"));
    }
    return errs;
}

fs::path Settings::workspace_path() const {
    if (workspace && !workspace->empty()) {
        std::error_code ec;
        fs::path p = paths::expand_user(*workspace);
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    return paths::user_data_dir(kAppName) / "projects";
}

// ---- 加载 ----

fs::path user_config_path() {
    return paths::user_config_dir(kAppName) / "config.toml";
}

fs::path user_api_key_path() {
    return paths::user_config_dir(kAppName) / "api_key";
}

std::string read_api_key_file() {
    std::error_code ec;
    const fs::path p = user_api_key_path();
    if (!fs::is_regular_file(p, ec)) return {};
    std::ifstream in(p, std::ios::binary);
    if (!in.good()) return {};
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    // 文件里就一行密钥。**把首尾空白全剃掉**：用编辑器存出来的多半带一个
    // 结尾换行，带着它发出去的 Authorization 头会被网关判成非法，报的是
    // 401——而那会把人支去查一个其实填对了的密钥。
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

fs::path write_api_key_file(const std::string& key) {
    const fs::path p = user_api_key_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (key.empty()) {
        fs::remove(p, ec);   // 清空 = 删掉，别留一个空文件在那儿让人猜
        return p;
    }
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << key;
    }
#ifndef _WIN32
    // 只给自己读写。Windows 上没有对应的简单做法，跳过。
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
#endif
    return p;
}


/// 从 base_url 揉一个能当文件名的短名。
///
/// 只留字母数字和横线，别的一律换成横线，再把连着的横线压成一个。
/// `https://open.bigmodel.cn/api/paas/v4` → `open-bigmodel-cn-api-paas-v4`。
/// **协议和结尾的斜杠不进短名**：`http://x/v1` 和 `https://x/v1/` 指的是同
/// 一家，存成两份的话换一次协议密钥就"丢"了。
std::string api_key_slug(const std::string& base_url) {
    std::string u = base_url;
    for (const char* pre : {"https://", "http://"}) {
        const std::string p = pre;
        if (u.rfind(p, 0) == 0) { u = u.substr(p.size()); break; }
    }
    while (!u.empty() && u.back() == '/') u.pop_back();
    std::string out;
    for (const char c : u) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-';
        if (keep) {
            out += c;
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    // 地址空着或者全是符号时不该落到一个叫 "" 的文件上。
    return out.empty() ? std::string("default") : out;
}

fs::path api_key_path_for(const std::string& base_url) {
    return paths::user_config_dir(kAppName) / "api_keys" /
           paths::from_utf8(api_key_slug(base_url));
}

bool has_own_api_key(const std::string& base_url) {
    std::error_code ec;
    const fs::path p = api_key_path_for(base_url);
    if (!fs::is_regular_file(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in.good()) return false;
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    return s.find_first_not_of(" \t\r\n") != std::string::npos;
}

bool any_own_api_key() {
    std::error_code ec;
    const fs::path dir = paths::user_config_dir(kAppName) / "api_keys";
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::ifstream in(e.path(), std::ios::binary);
        if (!in.good()) continue;
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        if (s.find_first_not_of(" \t\r\n") != std::string::npos) return true;
    }
    return false;
}

std::string read_api_key_for(const std::string& base_url) {
    std::error_code ec;
    const fs::path p = api_key_path_for(base_url);
    if (fs::is_regular_file(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        if (in.good()) {
            std::string s((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b != std::string::npos) {
                const auto e = s.find_last_not_of(" \t\r\n");
                return s.substr(b, e - b + 1);
            }
        }
    }

    // ⚠️ **退回老的那个单文件，只在"还没有任何一家单独存过"的时候。**
    //
    // 那个兜底本来是给升级留的路（手里那把钥匙不能因为换了存法就消失）。
    // 可它一直是"问谁都给"——于是**换到一家没配过的服务，发出去的是上一家
    // 的钥匙**。那不是 401，那是**把一家的密钥送到另一家的服务器上**。
    //
    // 2026-09-21 实撞：配好智谱和 DeepSeek 之后切到月之暗面（从没配过），
    // 设置页说「已经填过 sk***a8」——那是 DeepSeek 那把，而请求真带着它
    // 发了出去。
    //
    // 进了"按家存"的时代（`api_keys/` 下但凡有一份）就不再兜底：那时候
    // 空着才是实情，401 也才是实情。升级那条路由 `load_settings` 一次性
    // 搬家接住（见那儿）。
    if (any_own_api_key()) return {};
    return read_api_key_file();
}

fs::path write_api_key_for(const std::string& base_url, const std::string& key) {
    const fs::path p = api_key_path_for(base_url);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (key.empty()) {
        fs::remove(p, ec);
        return p;
    }
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << key;
    }
#ifndef _WIN32
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
#endif
    return p;
}

namespace {

/// 环境变量后缀 -> 配置路径。与 Python 的 _ENV_MAPPING 一一对应。
const std::vector<std::pair<const char*, const char*>>& env_mapping() {
    static const std::vector<std::pair<const char*, const char*>> m = {
        {"LLM_BASE_URL", "llm_base_url"},
        {"LLM_MODEL", "llm_model"},
        {"LLM_API_KEY", "llm_api_key"},
        {"TTS_BASE_URL", "tts_base_url"},
        // C++ 独有。Python 的 TTSConfig.backend 只有 comfy / http，
        // 没有 local（进程内那条路是这边加的），所以它那边也没有
        // 这个环境变量。和 MODELS_* 那九个是同一类扩展。
        // 有它才能不改配置文件就切到进程内配音——跑判据、跑 CI
        // 都要这个，改用户的配置文件是不该干的事。
        {"TTS_BACKEND", "tts_backend"},
        {"WORKSPACE", "workspace"},
        {"VRAM_GB", "vram_gb_override"},
        {"FFMPEG_PATH", "assembly_ffmpeg_path"},
        // 模型这几项是 C++ 独有的，Python 侧没有对应。
        // 容器里最常用的就是 MODELS_DIR——镜像不打包模型，
        // 挂个卷进来然后用它指过去。
        {"MODELS_DIR", "models_dir"},
        {"MODELS_LLM", "models_llm"},
        {"MODELS_VIDEO", "models_video"},
        {"MODELS_VIDEO_VAE", "models_video_vae"},
        {"MODELS_VIDEO_TEXT_ENCODER", "models_video_text_encoder"},
        {"MODELS_IMAGE", "models_image"},
        {"MODELS_IMAGE_BASE", "models_image_base"},
        // **engine 原来漏了。** 别的 [models] 键都有环境变量，
        // 偏偏这个开关没有——而它决定出图出片走进程内还是走 ComfyUI，
        // 正是容器里和对拍时最需要临时翻的一个。
        // 对拍推理层就卡在这上面：两边默认走的不是同一条路，没法比。
        {"MODELS_ENGINE", "models_engine"},
        {"MODELS_TTS", "models_tts"},
        {"MODELS_IMAGE_VAE", "models_image_vae"},
        {"MODELS_IMAGE_TEXT_ENCODER", "models_image_text_encoder"},
        {"MODELS_IMAGE_TEXT_ENCODER_VISION", "models_image_text_encoder_vision"},
        {"MODELS_TTS_DECODER", "models_tts_decoder"},
    };
    return m;
}

/// 从 toml 表里取值，键不存在就保持原样。
///
/// 全部走「存在才覆盖」而不是「取值或默认」，是因为配置是分层合并的：
/// 用户配置里没写的项要留给下一层，不能被默认值顶掉。
template <typename T>
void take(const toml::table* tbl, const char* key, T& dest) {
    if (!tbl) return;
    if (auto node = tbl->get(key)) {
        if (auto v = node->value<T>()) dest = *v;
    }
}

void take_path_str(const toml::table* tbl, const char* key,
                   std::optional<std::string>& dest) {
    if (!tbl) return;
    if (auto node = tbl->get(key)) {
        if (auto v = node->value<std::string>()) dest = *v;
    }
}

void apply_table(const toml::table& doc, Settings& s) {
    if (auto t = doc["video"].as_table()) {
        take(t, "orientation", s.video.orientation);
        take(t, "quality", s.video.quality);
        take(t, "max_shot_s", s.video.max_shot_s);
        take(t, "hero_takes", s.video.hero_takes);
        take(t, "chain_frames", s.video.chain_frames);
    }
    if (auto t = doc["tiers"].as_table()) {
        take(t, "draft_width", s.tiers.draft_width);
        take(t, "draft_height", s.tiers.draft_height);
        take(t, "draft_steps", s.tiers.draft_steps);
        take(t, "final_width", s.tiers.final_width);
        take(t, "final_height", s.tiers.final_height);
        take(t, "final_steps", s.tiers.final_steps);
    }
    if (auto t = doc["llm"].as_table()) {
        take(t, "backend", s.llm.backend);
        take(t, "command", s.llm.command);
        take(t, "command_timeout_s", s.llm.command_timeout_s);
        // 参数是个数组。**写了就整份替换，不合并**——参数的顺序和配对
        // （`--model` 跟着它的值）是一体的，合并出来的组合谁也说不清。
        if (auto arr = (*t)["command_args"].as_array()) {
            s.llm.command_args.clear();
            for (const auto& v : *arr) {
                if (auto sv = v.template value<std::string>()) {
                    s.llm.command_args.push_back(*sv);
                }
            }
        }
        take(t, "base_url", s.llm.base_url);
        take(t, "model", s.llm.model);
        take(t, "api_key", s.llm.api_key);
        take(t, "timeout_s", s.llm.timeout_s);
        take(t, "temperature", s.llm.temperature);
        take(t, "call_log", s.llm.call_log);
        take(t, "call_log_max_mb", s.llm.call_log_max_mb);
        take(t, "parallel", s.llm.parallel);
        take(t, "context_tokens", s.llm.context_tokens);
        take(t, "thinking", s.llm.thinking);
        take(t, "reasoning_effort", s.llm.reasoning_effort);
        // [llm.models] —— 按任务分流。**只覆盖写了的键**，没写的留着默认，
        // 否则用户想单独换一个任务就得把九个键全抄一遍。
        if (auto mt = (*t)["models"].as_table()) {
            for (const auto& [k, v] : *mt) {
                if (auto sv = v.template value<std::string>()) {
                    s.llm.task_models[std::string(k.str())] = *sv;
                }
            }
        }
        // [llm.effort] —— 按任务分档想多久。同上，只覆盖写了的键。
        if (auto et = (*t)["effort"].as_table()) {
            for (const auto& [k, v] : *et) {
                if (auto sv = v.template value<std::string>()) {
                    s.llm.task_efforts[std::string(k.str())] = *sv;
                }
            }
        }
    }
    if (auto t = doc["workers"].as_table()) {
        if (auto v = (*t)["gpu"].value<std::int64_t>()) {
            s.workers.gpu = static_cast<int>(*v);
        }
        if (auto v = (*t)["auto_spawn"].value<bool>()) {
            s.workers.auto_spawn = *v;
        }
        if (auto v = (*t)["base_port"].value<std::int64_t>()) {
            s.workers.base_port = static_cast<int>(*v);
        }
        if (auto arr = (*t)["endpoints"].as_array()) {
            s.workers.endpoints.clear();
            for (const auto& v : *arr) {
                if (auto sv = v.value<std::string>()) {
                    s.workers.endpoints.push_back(*sv);
                }
            }
        }
    }
    if (auto t = doc["peer"].as_table()) {
        take(t, "token", s.peer.token);
        if (auto arr = (*t)["nodes"].as_array()) {
            s.peer.nodes.clear();
            for (const auto& item : *arr) {
                const auto* nt = item.as_table();
                if (nt == nullptr) continue;
                PeerNodeConfig n;
                if (auto v = (*nt)["url"].value<std::string>()) n.url = *v;
                if (auto v = (*nt)["token"].value<std::string>()) n.token = *v;
                if (auto off = (*nt)["off"].as_array()) {
                    for (const auto& o : *off) {
                        if (auto sv = o.value<std::string>()) {
                            n.off.push_back(*sv);
                        }
                    }
                }
                // url 空的条目直接丢——留着的话它会在那张表上显示成一台
                // 永远连不上的机器，而用户根本不知道那是哪来的。
                if (!n.url.empty()) s.peer.nodes.push_back(std::move(n));
            }
        }
    }
    if (auto t = doc["tts"].as_table()) {
        take(t, "backend", s.tts.backend);
        take_path_str(t, "base_url", s.tts.base_url);
    }
    if (auto t = doc["gates"].as_table()) {
        take(t, "enabled", s.gates.enabled);
        take(t, "min_pixel_std", s.gates.min_pixel_std);
        take(t, "min_pixel_mean", s.gates.min_pixel_mean);
        take(t, "max_pixel_mean", s.gates.max_pixel_mean);
        take(t, "min_frame_similarity", s.gates.min_frame_similarity);
        take(t, "max_audio_drift_s", s.gates.max_audio_drift_s);
        take(t, "target_lufs", s.gates.target_lufs);
        take(t, "max_true_peak_db", s.gates.max_true_peak_db);
        take(t, "max_attempts_per_shot", s.gates.max_attempts_per_shot);
        take(t, "fallback_on_exhausted", s.gates.fallback_on_exhausted);
    }
    if (auto t = doc["assembly"].as_table()) {
        take(t, "fps", s.assembly.fps);
        take(t, "pix_fmt", s.assembly.pix_fmt);
        take(t, "video_codec", s.assembly.video_codec);
        take(t, "crf", s.assembly.crf);
        take(t, "audio_codec", s.assembly.audio_codec);
        take(t, "audio_bitrate", s.assembly.audio_bitrate);
        take(t, "audio_sample_rate", s.assembly.audio_sample_rate);
        take(t, "audio_channels", s.assembly.audio_channels);
        take(t, "scene_transition_s", s.assembly.scene_transition_s);
        // 老项目的 changji.toml 里还写着 `episode_s`（成片按每集 N 秒切的
        // 年代留下的）。take 是「键存在才覆盖」，这儿不 take 就是**忽略**它
        // ——不校验、不报错，老项目照常打开。
        take(t, "subtitle_max_chars_per_line", s.assembly.subtitle_max_chars_per_line);
        take(t, "subtitle_max_lines", s.assembly.subtitle_max_lines);
        take(t, "subtitle_font", s.assembly.subtitle_font);
        take(t, "ffmpeg_path", s.assembly.ffmpeg_path);
        take(t, "ffprobe_path", s.assembly.ffprobe_path);
    }
    if (auto t = doc["update"].as_table()) {
        take(t, "auto_check", s.update.auto_check);
        take(t, "channel", s.update.channel);
        take(t, "repo", s.update.repo);
        take(t, "every_hours", s.update.every_hours);
    }
    if (auto t = doc["preview"].as_table()) {
        take(t, "seconds", s.preview.seconds);
    }
    if (auto t = doc["look"].as_table()) {
        take(t, "preset", s.look.preset);
        take(t, "lut", s.look.lut);
        take(t, "lut_strength", s.look.lut_strength);
        take(t, "grain", s.look.grain);
        take(t, "soften", s.look.soften);
        take(t, "letterbox", s.look.letterbox);
    }
    if (auto t = doc["sound"].as_table()) {
        take(t, "ambient", s.sound.ambient);
        take(t, "ambient_db", s.sound.ambient_db);
        take(t, "music", s.sound.music);
        take(t, "music_db", s.sound.music_db);
        take(t, "duck", s.sound.duck);
        take(t, "music_style", s.sound.music_style);
        take(t, "music_command", s.sound.music_command);
        take(t, "music_timeout_s", s.sound.music_timeout_s);
    }
    if (auto t = doc["upscale"].as_table()) {
        take(t, "command", s.upscale.command);
        take(t, "scale", s.upscale.scale);
        take(t, "timeout_s", s.upscale.timeout_s);
    }
    if (auto t = doc["models"].as_table()) {
        take(t, "engine", s.models.engine);
        take_path_str(t, "dir", s.models.dir);
        take(t, "llm", s.models.llm);
        take(t, "video", s.models.video);
        take(t, "video_vae", s.models.video_vae);
        take(t, "video_text_encoder", s.models.video_text_encoder);
        take(t, "image", s.models.image);
        take(t, "image_base", s.models.image_base);
        take(t, "image_vae", s.models.image_vae);
        take(t, "image_text_encoder", s.models.image_text_encoder);
        take(t, "image_text_encoder_vision", s.models.image_text_encoder_vision);
        take(t, "tts", s.models.tts);
        take(t, "tts_decoder", s.models.tts_decoder);

        // `[models.pick]`：这部电影要哪一档。**按键盖，不整份替换。**
        //
        // 这个函数会被调两次（先全局、后项目里那份），而项目多半只写了
        // 一两组。整份替换的话，项目里写一个 image 就等于把全局挑好的
        // llm / tts / video 全清空——而那三组清空之后走的是"从文件名反推"
        // 那条老路，表面上还能跑，直到某一组的文件名恰好不在目录里。
        if (auto pk = (*t)["pick"].as_table()) {
            for (const auto& [k, v] : *pk) {
                if (auto str = v.value<std::string>()) {
                    s.models.pick[std::string(k.str())] = *str;
                }
            }
        }
        take(t, "diffusion_flash_attn", s.models.diffusion_flash_attn);
        take(t, "weights", s.models.weights);
        take(t, "image_weights", s.models.image_weights);
        take(t, "video_cfg", s.models.video_cfg);
        take(t, "video_flow_shift", s.models.video_flow_shift);
        take(t, "image_cfg", s.models.image_cfg);
        take(t, "image_flow_shift", s.models.image_flow_shift);
        take(t, "frame_tier", s.models.frame_tier);
        take(t, "frame_steps", s.models.frame_steps);
        take(t, "vram_reserve_gb", s.models.vram_reserve_gb);
        take(t, "video_high_noise", s.models.video_high_noise);
        take(t, "video_moe_boundary", s.models.video_moe_boundary);
        take(t, "video_llm", s.models.video_llm);
        take(t, "video_llm_vision", s.models.video_llm_vision);
        take(t, "video_audio_vae", s.models.video_audio_vae);
        take(t, "video_rng", s.models.video_rng);
        take(t, "video_lora", s.models.video_lora);
        take(t, "video_lora_strength", s.models.video_lora_strength);
        take(t, "video_lora_tiers", s.models.video_lora_tiers);
        take(t, "video_vae_tile", s.models.video_vae_tile);
        take(t, "video_max_frames", s.models.video_max_frames);
        take(t, "video_frame_step", s.models.video_frame_step);
        take(t, "video_frame_base", s.models.video_frame_base);
        take(t, "vae_vram_min_gb", s.models.vae_vram_min_gb);
    }
    take_path_str(&doc, "workspace", s.workspace);
    if (auto node = doc.get("vram_gb_override")) {
        if (auto v = node->value<double>()) s.vram_gb_override = *v;
    }
}

/// 把一份 toml 叠到 s 上。返回**这份文件里明写了 `[video].orientation` 吗**。
///
/// 那个返回值是 `load_settings` 分「项目自己说了画幅」和「项目没说」用的，
/// 没说的那一支要去 assets.json 里推（见 orientation_from_assets）。
/// **不能拿"读出来的值等于内置默认"当判据**：明写 landscape 和什么都没写，
/// 读完一模一样，而这两件事要走不同的路。
bool read_toml_into(const fs::path& path, Settings& s) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    try {
        auto doc = toml::parse_file(paths::to_utf8(path));
        apply_table(doc, s);
        const auto* video = doc["video"].as_table();
        // 类型不对（写成数字之类）算没说：那时候 take() 也没覆盖，
        // 两边保持同一个口径。
        return video != nullptr &&
               video->get_as<std::string>("orientation") != nullptr;
    } catch (const toml::parse_error& e) {
        // 配置坏了要说清楚是哪个文件。只说「配置解析失败」的话，
        // 用户手上有用户级和项目级两份，只能挨个翻。
        throw std::runtime_error(
            SAYF("配置文件解析失败：%1\n%2（第 %3 行）", paths::to_utf8(path),
                 e.description(), std::to_string(e.source().begin.line)));
    }
}

/// 没写 `[video]` 的老项目：画幅从它自己 assets.json 里那份比例反推。
/// 推不出来回 nullopt，由内置默认接着。
///
/// **为什么要推。** 2026-09-18 内置默认从 portrait 翻成 landscape（用户把
/// 产品定位从短剧改成电影制作平台），当时的理由是「老项目已经把画幅写进自己的
/// changji.toml，不受影响」。那个前提不成立：写 changji.toml 的是
/// `http::post_new_project`，`models::ProjectStore::create` 不写——手工建的、
/// 以及早于项目模板那一批老项目，目录里根本没有 `[video]`，读出来的就是
/// 内置默认。仓库自己的夹具 `tests/golden/项目_雨夜天台/` 就是这一类。
/// 于是翻默认等于把它们全改成横屏，三件事一件比一件难救：
///
///   · 一个已经出了两百镜 544×928 的竖屏项目再补一镜，`run_frames` 走
///     `scaled_to("16:9")` 出的是 928×544——**同一章里两种画幅**，装配时
///     要么被 normalize 成上下黑边、要么直接拼坏，全程不报错；
///   · 用户只要在设置页碰一下「后期 / 画面」，引擎就把
///     `orientation = "landscape"` 永久写进那个竖屏项目——那两条路写盘时
///     用的正是这个函数所在的 `load_settings` 读出来的值。不可逆，没提示；
///   · `/api/assets` 报的比例跟着翻，重出参考图也跟着翻。
///
/// **判据是「这个项目自己说了算」。** 老项目虽然没有 `[video]`，画幅在盘上
/// 仍然是有记录的：`assets.json` 里的 `style.aspect_ratio`。那份值是这个
/// 项目所有参考图和已出镜头的实际比例——出图那两层直接拿它把档位表的宽高
/// 转过来（`stages/frames.cpp` 里 `spec.scaled_to(assets.style.aspect_ratio)`
/// 那一句）——比任何内置默认都可信。
///
/// **那一栏读不出来也是数据，不是"推不出来"。** 读不出来一共四种样子——
/// 缺 `aspect_ratio` 那一栏、整个 `style` 对象缺、`style` 或那一栏类型不对、
/// 文件根本不是合法 JSON——成因是同一个：这份文件比这个字段还老，或者被
/// 手改歪了。而那个年代建的项目全是竖屏的，所以这四种都按 `StyleProfile`
/// 自己那个初值（9:16）算，正好答对。
///
/// 反过来说：**"文件读不出来"推不出"这个项目是横屏"。** 一份连 `style` 都
/// 没有的极老竖屏项目要是在这儿报了 landscape，补一镜就走
/// `scaled_to("16:9")` 出 928×544——同一章两种画幅、全程不报错，正是上面那
/// 三件事里最难救的第一件。
///
/// 还有一条对账：`ProjectStore::load_assets` 读**同一份盘**时，缺 `style`
/// 给的是 9:16（`asset_library_from_json` 里 `def.style` 那一支），类型不对
/// 和坏 JSON 直接抛——**没有一份会答"横屏"**。两个读法对同一份文件给不同的
/// 答案时，谁对谁错没人看得出来：一边是设置页显示的画幅，一边是出图那几层
/// 实际用的比例。
///
/// `nullopt` 只留给**真认不出**的那一种：那一栏在、是字符串、但既不是 16:9
/// 也不是 9:16（手改过，或者哪天加了档而这张表没跟）。那时候硬猜一个是错的
/// ——见 `orientation_of_aspect`。
///
/// **连 assets.json 都没有的目录不算老项目。** `ProjectStore::create` 建目录
/// 那一下就把 assets.json 写下来了（还顺手把当时的画幅钉进去），所以一个连
/// 它都没有的目录不是一份等着被认出来的老项目，是"盘上什么都没说"——那种
/// 交给内置默认答。
///
/// ⚠️ **只读，不写盘。** 出片那条路每跑一章都要经过这儿
/// （`http/run.cpp` 的 `load_settings(store.root())`），在一条读路径上顺手
/// 改用户的项目文件是不行的。落盘的时机是用户在设置页按保存那一下，
/// 见 `http/server.cpp` 的 `/bff/project/video` 和 `/bff/project/finish`
/// ——它们写的就是这儿推出来的值。
std::optional<std::string> orientation_from_assets(const fs::path& project_dir) {
    const fs::path f = models::ProjectPaths(project_dir).assets_file();
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) return std::nullopt;

    // **手改坏的 assets.json 在这儿一声不响地放过去。** 这是加载配置，
    // 不是打开资产库；真去读资产库的 `ProjectStore::load_assets` 会抛，
    // 而在这儿抛的后果是"文件歪了连设置页都打不开"。
    //
    // 放过去之后答的是 `StyleProfile` 的初值，不是内置默认——理由见上面
    // 「读不出来也是数据」那一段。所以下面这一路**没有一个 return
    // nullopt**：打不开、不是 JSON、没有 `style`、那一栏类型不对，全都停在
    // 这个初值上。`nlohmann::json` 的 `find()` 对非对象（含 parse 失败那个
    // discarded 值）回的就是 `end()`，四种情况因此能合成同一条路。
    std::string ratio = models::StyleProfile{}.aspect_ratio;
    if (std::ifstream in(f, std::ios::binary); in) {
        const auto doc = nlohmann::json::parse(in, nullptr, false);
        if (const auto style = doc.find("style");
            style != doc.end() && style->is_object()) {
            if (const auto it = style->find("aspect_ratio");
                it != style->end() && it->is_string()) {
                ratio = it->get<std::string>();
            }
        }
    }
    return orientation_of_aspect(ratio);
}

/// 环境变量覆盖。
///
/// 环境变量都是字符串，数值项要转换。转换失败就忽略这一项而不是报错——
/// 容器里注入了一个格式不对的值，整个服务起不来比用默认值更糟。
void apply_env(Settings& s) {
    auto get = [](const char* suffix) {
        return paths::env((std::string(kEnvPrefix) + suffix).c_str());
    };
    auto as_double = [](const std::string& v, double& dest) {
        try { dest = std::stod(v); } catch (...) {}
    };

    std::string v;
    if (!(v = get("LLM_BASE_URL")).empty()) s.llm.base_url = v;
    if (!(v = get("LLM_MODEL")).empty()) s.llm.model = v;
    if (!(v = get("LLM_API_KEY")).empty()) s.llm.api_key = v;
    if (!(v = get("TTS_BASE_URL")).empty()) s.tts.base_url = v;
    if (!(v = get("TTS_BACKEND")).empty()) s.tts.backend = v;
    if (!(v = get("WORKSPACE")).empty()) s.workspace = v;
    if (!(v = get("VRAM_GB")).empty()) {
        double d = 0;
        as_double(v, d);
        if (d > 0) s.vram_gb_override = d;
    }
    if (!(v = get("FFMPEG_PATH")).empty()) s.assembly.ffmpeg_path = v;
    if (!(v = get("MODELS_DIR")).empty()) s.models.dir = v;
    if (!(v = get("MODELS_LLM")).empty()) s.models.llm = v;
    if (!(v = get("MODELS_VIDEO")).empty()) s.models.video = v;
    if (!(v = get("MODELS_VIDEO_VAE")).empty()) s.models.video_vae = v;
    if (!(v = get("MODELS_VIDEO_TEXT_ENCODER")).empty()) {
        s.models.video_text_encoder = v;
    }
    if (!(v = get("MODELS_IMAGE")).empty()) s.models.image = v;
    if (!(v = get("MODELS_IMAGE_BASE")).empty()) s.models.image_base = v;
    // engine 只认那两个取值。写错了不静默接受——那会让整条出片的路
    // 悄悄走岔，而表现是"连不上 ComfyUI"或者"没编进出图后端"，
    // 两句话都指不到真正的原因（环境变量拼错了）。
    // 这里保持原值，随后 validate() 会拦住它并说清楚。
    if (!(v = get("MODELS_ENGINE")).empty()) s.models.engine = v;
    if (!(v = get("MODELS_IMAGE_VAE")).empty()) s.models.image_vae = v;
    if (!(v = get("MODELS_IMAGE_TEXT_ENCODER")).empty())
        s.models.image_text_encoder = v;
    if (!(v = get("MODELS_IMAGE_TEXT_ENCODER_VISION")).empty())
        s.models.image_text_encoder_vision = v;
    if (!(v = get("MODELS_TTS")).empty()) s.models.tts = v;
    if (!(v = get("MODELS_TTS_DECODER")).empty()) s.models.tts_decoder = v;
}

}  // namespace

Settings load_settings(const std::optional<fs::path>& project_dir) {
    Settings s;  // 内置默认值就是成员初始化器
    read_toml_into(user_config_path(), s);
    if (project_dir) {
        // **项目自己没写画幅时，从它自己的 assets.json 推**，而不是落到
        // 内置默认。优先级照旧「项目 > 全局 > 内置默认」——推出来的这个值
        // 也是项目级的记录，所以它压全局那份。见 orientation_from_assets。
        const bool toml_says_orientation =
            read_toml_into(*project_dir / "changji.toml", s);
        if (!toml_says_orientation) {
            if (const auto o = orientation_from_assets(*project_dir)) {
                s.video.orientation = *o;
            }
        }
    }
    // **密钥单独一个文件，压过 config.toml 里那份。**
    // 老配置里写了 [llm].api_key 的照样认（上面那行已经读进来了），
    // 但只要单独那个文件在，就以它为准——见 user_api_key_path。
    // 环境变量仍然最大，所以 apply_env 排在后面。
    // **按地址取。**
    //
    // ⚠️ **升上来的那把钥匙在这儿一次性搬家。** 老版本只有一个单文件，
    // 而那把钥匙属于**它当时指着的那一家**——也就是这会儿配置里写的这一家。
    // 不搬的话它会一直当"问谁都给"的兜底，而那会把一家的密钥送到另一家的
    // 服务器上（见 read_api_key_for 上那段）。
    //
    // 只在「一家都还没单独存过」时搬，所以一辈子只发生一次。
    if (!any_own_api_key()) {
        if (const std::string legacy = read_api_key_file(); !legacy.empty()) {
            try {
                write_api_key_for(s.llm.base_url, legacy);
            } catch (...) {
                // 搬不动就算了（盘满、只读）：下面照旧退回单文件读得到。
            }
        }
    }
    if (const std::string k = read_api_key_for(s.llm.base_url); !k.empty()) {
        s.llm.api_key = k;
    }
    apply_env(s);

    // 地址类的值统一规整，避免 http://x:8188/ 和 http://x:8188
    // 被当成两个不同的服务
    s.llm.base_url = strip_trailing_slash(s.llm.base_url);
    if (s.tts.base_url) s.tts.base_url = strip_trailing_slash(*s.tts.base_url);

    // 往 stderr 打，不走日志：这一步发生在任何日志接上之前。
    for (const auto& note : migrate_legacy(s)) {
        std::fprintf(stderr, "[配置] %s\n", note.c_str());
    }
    return s;
}

namespace {

/// 模型文件多大（GB）。读不到回 0——上游按"装不下"处理，也就是放内存。
double model_size_gb(const Settings& s, const std::string& entry) {
    std::error_code ec;
    const auto p = s.models.resolve(entry, s.workspace_path());
    const auto bytes = p.empty() ? 0 : fs::file_size(p, ec);
    return (!ec && bytes > 0) ? static_cast<double>(bytes) / (1024.0 * 1024 * 1024)
                              : 0.0;
}

}  // namespace

ModelsConfig::VideoFamily ModelsConfig::video_family() const {
    std::string low;
    for (const char c : video) {
        const unsigned char u = static_cast<unsigned char>(c);
        low += (u >= 'A' && u <= 'Z') ? static_cast<char>(u - 'A' + 'a') : c;
    }
    const auto has = [&low](const char* w) { return low.find(w) != std::string::npos; };
    if (has("minimax") || has("hailuo") || has("h3")) return VideoFamily::MiniMaxH3;
    if (has("wan")) return video_high_noise.empty() ? VideoFamily::Wan5B : VideoFamily::WanA14B;
    if (!video_llm.empty()) return VideoFamily::MiniMaxH3;
    return VideoFamily::Unknown;
}

double ModelsConfig::effective_video_cfg() const {
    if (video_cfg > 0.0) return video_cfg;
    switch (video_family()) {
        case VideoFamily::MiniMaxH3: return 1.0;   // docs/minimax_h3.md：--cfg-scale 1.0
        case VideoFamily::WanA14B: return 3.5;     // docs/wan.md A14B
        case VideoFamily::Wan5B: return 6.0;       // docs/wan.md TI2V-5B
        case VideoFamily::Unknown: break;
    }
    return 6.0;   // 老默认，认不出家族时别乱猜
}

std::string ModelsConfig::effective_video_rng() const {
    if (video_rng != "auto") return video_rng;
    // 上游给 H3 的命令行明写 --rng cpu；sd.cpp 自己的默认是 cuda。
    return video_family() == VideoFamily::MiniMaxH3 ? "cpu" : "cuda";
}

void resolve_model_family_defaults(Settings& s) {
    // 把 0 / auto 落成具体值。读取设置的两条入口都调（见 normalize_fps_for_model
    // 头上那段：load_settings 和 Runtime::replace 互不相通），下游拿到的
    // 永远是具体值，不用每个消费方都记得去问家族。
    // 认不出家族（多半是根本没配视频模型）就不动：留着 0 / auto，
    // 消费方用 effective_* 时照样拿到老默认 6.0 / cuda；而模板解析出来
    // 仍等于内置默认值，那条用例钉的正是这一点。
    if (s.models.video_family() == ModelsConfig::VideoFamily::Unknown) return;
    s.models.video_cfg = s.models.effective_video_cfg();
    s.models.video_rng = s.models.effective_video_rng();
}

bool ModelsConfig::accepts_reference_images(const std::string& image_file) {
    std::string low;
    for (const char c : image_file) {
        const unsigned char u = static_cast<unsigned char>(c);
        low += (u >= 'A' && u <= 'Z') ? static_cast<char>(u - 'A' + 'a') : c;
    }
    // 只看文件名那一段：目录名里带 edit 不算数。
    const std::size_t slash = low.find_last_of("/\\");
    if (slash != std::string::npos) low = low.substr(slash + 1);
    return low.find("edit") != std::string::npos;
}

Settings expand_placement(Settings s, double card_gb, bool unified) {
    // **画布要传进去。** 计算缓冲跟着它走（见 video_buffer_gb）：不传的话
    // 2K 会被当成 1280×704，在大卡上判成"装得下、VAE 也常驻"，然后 OOM。
    const auto [cw, ch] = s.video.size();
    const double canvas_px = static_cast<double>(cw) * ch;
    s.models.weights = s.models.weights_for(
        card_gb, model_size_gb(s, s.models.video), unified, canvas_px);
    s.models.image_weights = s.models.image_weights_for(
        card_gb, model_size_gb(s, s.models.image), unified);
    return s;
}

PlacementInfo video_placement(const Settings& expanded) {
    PlacementInfo p;
    p.weights = expanded.models.weights;
    p.model_gb = model_size_gb(expanded, expanded.models.video);
    const auto [cw, ch] = expanded.video.size();
    p.live_vram_gb = expanded.models.video_live_vram_gb(
        p.weights, p.model_gb, static_cast<double>(cw) * ch);
    // "cpu" = 权重全在内存。别的规格里扩散那份都是常驻的
    // （te=cpu 只把文本编码器放内存，te=cpu,vae=cpu 再加上 VAE）。
    p.resident = p.weights != "cpu";
    return p;
}

PlacementInfo image_placement_of(const Settings& expanded,
                                 const std::string& diffusion_file) {
    PlacementInfo p;
    p.weights = expanded.models.image_weights;
    p.model_gb = model_size_gb(expanded, diffusion_file);
    p.live_vram_gb = expanded.models.image_live_vram_gb(p.weights, p.model_gb);
    p.resident = p.weights != "cpu";
    return p;
}

PlacementInfo image_placement(const Settings& expanded) {
    return image_placement_of(expanded, expanded.models.image);
}

int steps_on_node(const Settings& node, int dispatched_steps, bool steps_pinned) {
    if (steps_pinned || dispatched_steps <= 0) return dispatched_steps;
    const auto eff = effective_spec(node, dispatched_steps);
    // 这台自己的 [tiers].final_steps 不算数：那是它本地跑时的偏好，
    // 派来的活听派活那部电影的。只拿"挂没挂上 Turbo"这一个结论。
    return eff.turbo ? 6 : dispatched_steps;
}

EffectiveSpec effective_spec(const Settings& s, int table_final_steps) {
    EffectiveSpec out;
    const auto [w, h] = s.video.size();
    out.width = w;
    out.height = h;

    // 档位表里的步数假设的是**不带蒸馏 LoRA** 的模型（MiniMax-H3 走 20）。
    // 挂着 Turbo 还跑 20 步不只是慢：资料和实测都说超过 8 步开始过锐，
    // 画面反而变差。
    std::error_code ec;
    const auto lora = s.models.resolve(s.models.video_lora, s.workspace_path());
    out.turbo = !s.models.video_lora.empty() &&
                fs::is_regular_file(lora, ec);
    out.steps_pinned = s.tiers.final_steps != 0;

    out.final_steps = out.steps_pinned ? s.tiers.final_steps : table_final_steps;
    // 显式填了步数的人是有意的，别替他改。
    if (out.turbo && !out.steps_pinned) out.final_steps = 6;

    // **首帧不跟着 Turbo 走。** 那个 LoRA 只挂在视频模型上，出图那一步
    // 没有它；跟着变成 6 步就是裸跑 6 步，出来的首帧糊。而首帧是喂给
    // 出片那一步的起始图，也是跨镜头一致性的锚点——糊了后面每一镜都糊，
    // 而且全程不报错。
    out.frame_steps = s.models.frame_steps > 0 ? s.models.frame_steps
                                               : table_final_steps;
    return out;
}

double workload_scale(int table_width, int table_height, int table_steps,
                      const EffectiveSpec& eff) {
    // 解码那段占的比例，见头文件里为什么不是纯步数比。
    constexpr double kFixedShare = 0.2;

    const double table_px = static_cast<double>(table_width) * table_height;
    if (table_px <= 0.0 || table_steps <= 0) return 1.0;
    if (eff.width <= 0 || eff.height <= 0 || eff.final_steps <= 0) return 1.0;

    const double px_ratio =
        static_cast<double>(eff.width) * eff.height / table_px;
    const double step_ratio =
        static_cast<double>(eff.final_steps) / table_steps;
    return px_ratio * (kFixedShare + (1.0 - kFixedShare) * step_ratio);
}

std::string normalize_fps_for_model(Settings& s) {
    const auto limits =
        stages::guess_video_limits(s.models.video, !s.models.video_llm.empty());
    const int want = stages::effective_fps(limits, s.assembly.fps);
    if (want == s.assembly.fps) return {};
    const int had = s.assembly.fps;
    s.assembly.fps = want;
    // 这一句**界面上看得见**：`/api/settings` 把它跟着回执一起送回去
    // （见 `http/config_api.cpp` 里 notes 那段）。下面 migrate_legacy 的
    // 那两句 comfy 迁移只进 stderr，是日志，所以不包。
    return SAYF("[assembly].fps 填的是 %1，但出片模型只出 %2 fps——sd.cpp 会"
                "自己改掉，只在日志里留一句淹在 CUDA Graph 刷屏里的 LOG_WARN。"
                "这次按 %2 算。不跟着改的话整片变速：裸帧是按 %2fps 的节奏"
                "演的，而帧数、每镜时长、编码都按你填的那个数走。",
                std::to_string(had), std::to_string(want));
}

std::vector<std::string> migrate_legacy(Settings& s) {
    std::vector<std::string> notes;
    // 帧率跟着出片模型走。放在这儿是因为这个函数正是"设置读进来之后
    // 自己纠一遍并大声说一句"的那一处，而出片那条路每跑一章都会经过它
    // （http/run.cpp 的 load_settings(store.root())）。
    if (std::string note = normalize_fps_for_model(s); !note.empty()) {
        notes.push_back(std::move(note));
    }
    // cfg / rng 的 0 / auto 按模型家族落成具体值，同样两条入口都做。
    resolve_model_family_defaults(s);
    // **拆掉一条路之后，老配置不能让程序起不来。**
    //
    // 2026-09-10 拆 ComfyUI 时只在 validate() 里加了迁移说明，
    // 于是升级上来的用户是这个遭遇：程序**直接退出**，只往 stderr 打一句
    // "tts.backend = comfy 已经不支持了"。双击启动的人连那句都看不到，
    // 窗口一闪就没了。而那句话让他去改的 toml，正是他多半不知道在哪的
    // 那个文件——他本来会去设置页改，可设置页是这个进程发的，起不来就打不开。
    //
    // 两处 comfy 都只有一个像样的去处，那就自己换掉、大声说一句。
    // 换错的代价（本机跑而不是走 ComfyUI）远小于起不来。
    if (s.tts.backend == "comfy") {
        s.tts.backend = "local";
        notes.push_back(
            "[tts].backend 原来是 \"comfy\"，ComfyUI 那条路已拆除，"
            "这次按 \"local\"（进程内跑）起。要外接配音服务就改成 "
            "\"http\" 并填 [tts].base_url。");
    }
    if (s.models.engine == "comfy") {
        s.models.engine = "sd";
        notes.push_back(
            "[models].engine 原来是 \"comfy\"，出图出片现在都走进程内的 "
            "sd.cpp，这次按 \"sd\" 起。模型文件在 [models] 的 image / "
            "video 几项。");
    }
    return notes;
}

std::map<std::string, std::string> env_overridden() {
    std::map<std::string, std::string> out;
    for (const auto& [suffix, key] : env_mapping()) {
        std::string name = std::string(kEnvPrefix) + suffix;
        if (!paths::env(name.c_str()).empty()) out[key] = name;
    }
    return out;
}

// ---- 写模板 ----

namespace {

// 与 Python 的 _DEFAULT_TOML 保持一致。注释是给人看的，不能省。
constexpr const char* kDefaultTomlHead = R"(# 场记配置文件
# 优先级：环境变量 > 项目目录下的 changji.toml > 本文件 > 内置默认值

# 项目库根目录。留空则用系统标准数据目录。
# 换机器时把项目目录整个拷走即可，程序装在哪都不影响。
# workspace = "D:/电影项目"

# 显存覆盖。推理服务跑在另一台机器时本机探测不到显卡，用它手动指定。
# vram_gb_override = 16

[video]
# **这一节写在项目目录的 changji.toml 里**，一部电影一份——一台机器上可以
# 同时有横屏的正片和竖版的物料，画幅是这部电影的属性不是这台机器的属性。
#   orientation = "portrait" | "landscape"
#   quality     = "720p" | "hd" | "2k"
# 宽高由这两项算出来（短边 × 长边，横屏时反过来）：
#   720p → 544×928    省时间，一镜约两分钟
#   hd   → 704×1280   像素多 80%，画面明显更实
#   2k   → 1440×2560  一张 32 GB 的卡跑不动
# 三个数都是 32 的倍数，这是硬约束，所以没有"自己填宽高"这一项。
# **2K 一张 32 GB 的卡跑不动**，那时候要么换大卡，要么出 720p 再
# `changji --upscale`。这里不会悄悄降档。
# orientation = "landscape"
# quality = "720p"

[tiers]
# 画质档位。**不填就按显存推**（见 models/hardware.cpp 的档位表），
# 填了就以填的为准，一项一项来。分辨率要是 32 的倍数（Wan 的潜空间对齐）。
#
# **界面上没有这一节了**：画幅（竖屏/横屏、720p/2K）搬到每个项目自己的
# changji.toml 的 [video] 里，出片时会盖掉这里的 final_width/final_height。
# 留在这儿的是专家旋钮，最有用的一项是 final_steps：
#
#   **填了 final_steps，挂 Turbo LoRA 时的 6 步就不再自动生效**
#   （run.cpp 只在它是 0 时才动它）。想固定步数才填，否则别碰——
#   填成档位表推出来的 28，每一镜会慢四倍，而且哪儿都不会提示。
# draft_width = 960
# draft_height = 544
# draft_steps = 6
# final_width = 1280
# final_height = 704
# final_steps = 6

[workers]
# **本机多卡不用填 endpoints**：留空时主进程会按显卡数自己拉起每张卡一个
# 工作进程（auto_spawn，默认开），第 i 张卡监听 base_port + i。
# 也就是说多卡上你启动的仍然只是一个命令。
#
# 跨机部署才填：endpoints = ["http://别的机器:9001", ...]，协议一模一样。
# auto_spawn = true
# base_port = 9001

[peer]
# 别的机器要把活派到这台来时，认的口令。**空 = 不接外来的活。**
#
# 只在对外监听（--host 0.0.0.0 之类）时才要：本机多卡自己拉起的那些
# 工作进程听的是 127.0.0.1，外面连不进来，不受这条影响。
#
# 没设口令却要对外监听的话，服务会当场拒绝启动并说清楚——谁都能派活
# 过来烧这张卡、读走这台有哪些模型，那不该是默认值。
# token = ""

# 别的机器，一台一段。跨机会自动传文件（参考图、首帧过去，产物回来），
# 同机那条路（[workers].endpoints）不受影响、一个字节都不搬。
#
# off 是"不许它干的那几样"：llm / tts / frame / video / assemble。
# **只能关不能开**——能不能干是那台自己量出来的，这儿只做减法。
#
# [[peer.nodes]]
# url = "http://gpu-box:9001"
# token = ""            # 留空就用上面那个
# off = ["llm"]         # 这台的卡留着出片，写文别派给它

[llm]
# 剧本和分镜用的大模型。backend 只有 remote 这一个值（下面说了为什么）：
#   remote —— **默认**，走下面的 base_url，任何兼容 OpenAI 接口的服务都行。
#             默认走智谱（bigmodel.cn，国内直连不用自备网络）：默认挑的
#             glm-4.7-flash 不要钱，但 **api_key 必须自己填**，
#             去 bigmodel.cn 控制台领一把。
#             国外访问把地址换成 https://api.z.ai/api/paas/v4，
#             同一套后端、同一把密钥、同一份模型清单。
#             换 DeepSeek、硅基流动、火山方舟，或者局域网里另一台
#             机器上的 Ollama / vLLM，改这两行就是。
#   local  —— 进程内跑，权重填 [models].llm（模型窗里挑一档下载就自动填上）。
#             不用密钥、不出网、不撞限流；归调度器管，出片要显存时先卸它。
#             写得没云上的好（本地那档最高 59 分对 glm-5.3 的 81.8），
#             想要好的换成 remote。2026-09-14 删过一次、2026-09-19 接回来：
#             这次 schema 和远端一样贴在提示词里，思考自由流、按 <think> 拆开。
#   command —— 跑本机装着的命令行（claude / codex），按订阅算钱。
backend = "remote"
base_url = "https://open.bigmodel.cn/api/paas/v4"
# api_key = "去 bigmodel.cn 控制台领"

# 「先想再写」远端那条 2026-09-14 起不再发开关：**现在的模型都要思考**，
# 关不掉的越来越多（glm-5.3 / 5.3-flash 发关闭值直接回 400），
# 而智谱把思考放在 reasoning_content 里、不混进正文，所以也不用关。
# 本地那条（backend = "local"）能关：thinking = false，省一半时间，
# 代价是小模型更容易把梗概平摊成概述。默认开。远端和命令行不看这一项。
# thinking = true

# **想多久。** 关不掉，但能调档。智谱不说话就是 max，而 max 在这条流水线上
# 是真会出事的：模型会在思考里把整章（或整张分镜表）写完一遍，然后才用 JSON
# 再写一遍——实测撞过一次 977 秒、思考 169394 字、正文 0 字。所以默认 high。
#
#   取值：minimal / low / medium / high / max，留空 = 一个字段都不发。
#   ⚠️ 各家收的不一样：glm-5.3 和 5.3-flash 只收 low / high / max。
#   认不得这个参数的模型一个字段都不多发，填了也不影响。
#
# 这一项在界面上也能改（项目页点编剧模型名弹出来那个窗）。
# reasoning_effort = "high"

# 把每一次给大模型的提示词和它的回答都留下来，放在
#   <数据目录>/llm_log/<项目名>/
# index.jsonl 一行一次调用（给统计用），提示词和回复各自原样存成 .txt
# （想比两次提示词直接 diff，grep 也能直接搜）。字段表和几条现成的 jq
# 在 docs/提示词日志.md 里。
# **密钥不落盘**，整个请求头都不记，只记地址。
# ⚠️ 提示词里是整章正文，拷这个目录给别人之前先想一下。
call_log = true
# 正文文件最多占多少磁盘（MB），超了从最旧的那次开始删。
# index.jsonl 不在此列，它永远不删——统计的依据在它身上。
# 收 1 到 102400（100 GB）之间的数，超出这个区间起服务时就会说。
# 一次调用的正文几十 KB，比 1 MB 还小的上限等于刚写下就被剪光；
# 真不想留就把上面那个 call_log 关掉。
call_log_max_mb = 1024.0

# 兜底模型：下面 [llm.models] 里没点名的任务用它。
# glm-4.7-flash 是这家唯一免费的模型，也是默认——代价是限流很紧
# （撞上回的是 1305「该模型当前访问量过大」）。
model = "glm-4.7-flash"

# **按任务分流：哪一步用哪个模型。** 键是内部的 schema 名。
# 默认整段是注释掉的——免费档只有一个模型，分流无从分起。
#
# 想要好的就把下面这段的注释去掉。两种本事分开买：
#   写得好 —— 正文、梗概、大纲、预告。glm-5.3 在 EQ-Bench 长文创作榜上
#             81.8 分、slop 7.09（全榜第二低），而且八章几乎不降——
#             一章接一章往下写最怕的就是越写越塌。默认那个 glm-4.7-flash
#             在同一个榜上 47.8、slop 48.86。
#   听话   —— 分镜、人物表、剧本四段、分析。分镜那份 schema 有六十多个
#             类型定义和一串枚举，文采在这儿一点用都没有，够听话就行。
#
# 一部 11 章按输入 20 万 / 输出 15 万 token 估，这么配合计约 $0.73。
# ⚠️ glm-5.3-flash 榜上没测过，别拿 5.3 的分替它背书：上一代
#    glm-4.7 → glm-4.7-flash 掉了 18.2 分。选型只能靠真发一次。
# [llm.models]
# chapter        = "glm-5.3"
# premises       = "glm-5.3"
# story_outline  = "glm-5.3"
# story_revision = "glm-5.3"
# trailer        = "glm-5.3"
# storyboard     = "glm-5.3-flash"
# bible          = "glm-5.3-flash"
# script         = "glm-5.3-flash"
# story_analysis = "glm-5.3-flash"

# 按任务分档**想多久**，键和上面那张表同一套。没点名的用 reasoning_effort。
# 用处是省钱：拆分镜、人物表这些"听话"的活可以更低一档。
# [llm.effort]
# chapter    = "high"
# storyboard = "low"

[tts]
# backend 只有两个值（第三个 comfy 2026-09-10 随 ComfyUI 一起拆了，
# 老配置填它会被校验拦下并给出改法）：
#   local —— **进程内配音，不需要装任何外部服务**。要填下面 [models] 里的
#            tts 和 tts_decoder 两个模型文件。想先听听效果的话，不必配也不必
#            建项目，直接：changji --say "雨下了一整夜。" --tts-model <骨干>
#            --tts-decoder <解码器>
#   http  —— 独立的配音服务
backend = "local"

[gates]
# 质量闸门。全自动模式下这些阈值决定废片能不能被拦住。
enabled = true
max_attempts_per_shot = 3
# 重试超限时保留最后那一版（闸门没过，但那一镜的视频在），保证整章能出片而不是卡死。
fallback_on_exhausted = true

[assembly]
fps = 24
crf = 18
# ⚠️ 转场目前不生效：装配是 `-f concat -c copy` 直接拼，全程硬切，
# 引擎里一处 xfade / acrossfade 都没有（见 media/assemble.cpp 里那段）。
# 这个数还收着（改它不报错、也存得住），但改了不会有任何变化。
scene_transition_s = 0.4
# 中文字幕单行上限，全角字符数。
subtitle_max_chars_per_line = 15
subtitle_font = "Source Han Sans SC"

[sound]
# 声音那几层里**机器属性**的那一项：生成一条配乐的命令。别的（环境声、
# 配乐开关、压多少 dB）是这部电影的属性，在项目目录的 changji.toml 里。
# 占位符：{prompt} 描述、{seconds} 时长、{out} 输出 wav。空 = 不生成配乐。
# ACE-Step 1.5 的包装脚本在 cpp/tools/music_ace_step.py（<4 GB 显存，几秒一条）。
# music_command = "python /path/to/changji/cpp/tools/music_ace_step.py --prompt {prompt} --seconds {seconds} --out {out}"
# music_timeout_s = 600
)";

// **模板从这里断成两截，别接回去。** MSVC 的单条字符串字面量上限是 16380
// 字节（C2026），这份模板整个 17 KB；接成一条的表现是 **Windows 上整个编
// 不过**，而 clang/gcc 一点事没有——2026-09-15 在那台 Windows 上实撞，报的
// 是 `settings.cpp(1469): error C2026`，指着模板中间某一行，看不出一点跟
// 长度有关。下面那两条 static_assert 把这件事挪到**每个平台都编不过**：
// 往模板里加内容加到超限时，Mac 上就会当场说清楚是哪一截、差多少。
// （拼完之后那条 65535 的上限离得还远，同一件事见 cpp/tools/gen_webapp.py。）
constexpr const char* kDefaultTomlTail = R"(
[upscale]
# 时序放大（机器属性）。本地 MiniMax-H3 只到 768p，要 1080p 只能放大；
# 逐帧 ESRGAN 会闪，SeedVR2 / RTX VSR 这类时序放大器都在 Python 里，所以
# 做成一条命令，装配时每一镜先过它再调色加颗粒。
# 占位符：{in} {out} {width} {height} {short}（目标短边） {scale}。空 = 不放大。
# command = "/path/to/seedvr2/.venv/bin/python /path/to/seedvr2/inference_cli.py {in} --output {out} --resolution {short} --batch_size 5"
# scale = 2
# timeout_s = 1800

# ⚠️ **迁移期间这一整节默认是注释掉的。**
#
# Python 引擎的 Settings 是 extra="forbid"，只要这份配置里出现 [models]，
# **它整份加载失败、后端根本起不来**，报的是
# "Extra inputs are not permitted"。而迁移期间两个后端共用这一份文件。
#
# 要用进程内推理（sd.cpp / 进程内配音）就把下面这些取消注释——
# 那之后 Python 引擎会起不来，阶段 8 之前请确认你不再需要它。
# 或者把这一节写进项目目录的 changji.toml，只影响那一个项目。
#
# [models]
# 出图出片走哪个引擎。现在只有 sd（进程内 sd.cpp）。
# ComfyUI 那条 2026-09-10 拆了；老配置写 comfy 会被拦下并给出改法。
# engine = "sd"
#
# 进程内推理要用的模型文件。
#
# 相对路径相对下面的 dir 解析，绝对路径原样用（多机共享网络盘时会这么填）。
# dir 留空则用项目库旁边的 models/ 目录。
#
# 这几项**必须自己填**，没有默认文件名。原因是同一个二进制要在配置差很多的
# 机器上跑：24G 显存的机器和 8G 内存的树莓派，能装下的量化档完全不同，
# 猜一个默认值只会让人以为配好了然后在加载时炸掉。
#
# dir = "~/models"
# llm = "Qwen3-14B-Q4_K_M.gguf"
# 进程内配音（[tts] backend = "local"）要这两个。骨干是自回归那半，
# 解码器把 token 变成波形，缺一个都出不了声。
# tts = "Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf"
# tts_decoder = "mmproj-Qwen3-TTS-12Hz-1.7B-Base-f16.gguf"
# video = "Wan2.2-TI2V-5B-Q4_K_M.gguf"
# video_vae = "Wan2.2_VAE.safetensors"
# video_text_encoder = "umt5-xxl-encoder-Q5_K_M.gguf"
# image = "Qwen-Image-Edit-2509-Q6_K.gguf"
# 图像模型自己的 VAE 和文本编码器。**别拿视频那套顶**——
# Wan 的 VAE 和 Qwen-Image 的不是一回事，UMT5-XXL 和 Qwen2.5-VL 更不是
# （在 sd.cpp 里连参数位都不同）。喂错了不报错，只是出来的图和提示词没关系。
# 这两项留空会退回 video_vae / video_text_encoder，只用 Wan 的人不必填。
# image_vae = "qwen_image_vae.safetensors"
# image_text_encoder = "Qwen2.5-VL-7B-Instruct-Q8_0.gguf"
# 2509 及以后的 Qwen-Image-Edit 还要视觉塔，初版可以不填
# image_text_encoder_vision = "Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf"
#
# 权重放哪。cpu（默认）= 放系统内存、用到才搬进显存，小卡上能跑全靠它，
# 代价是每一步都在等 PCIe。auto = 交给 sd.cpp 按这张卡真实的空闲显存决定，
# 装得下的常驻显存——大卡（≥ 24 GB）上用这个，实测出片阶段利用率从 35% 起飞。
# **默认 smart：按视频模型文件多大和这张卡多大算，换卡不用改。**
# 别写死。写死 cpu 的后果：换了 48 GB 的卡还在每一步搬权重，而且没有任何提示。
# 苹果芯片上 smart 会展开成 gpu（一个组件都不往内存放）：那种机器上
# CPU 和 GPU 是同一块内存，"放内存"省不出地方，只是把计算赶去了 CPU。
# weights = "smart"
#
# 图像模型单独一项。**别跟着 weights 一起改成 cpu**：那是给 18 GB 的视频
# 模型准备的，图像模型放内存会慢五倍（5090 上实测采样时 GPU 利用率 18%、
# 一步 6.8 秒；扩散权重常驻是 82%、一步 1.25 秒）。
# smart = 按模型文件大小算装不装得下（权重 + 解码缓冲 6.6 GB + 余量 4 GB
# ≤ 显存的九成）：fp8 20 GB 在 32 GB 卡上装不下→cpu；Q6_K 16 GB 装得下→常驻。
# image_weights = "smart"
#
# 采样旋钮，按角色分开。图像那条路 cfg 太高（比如 7）出来的就是噪点。
#
# flow_shift 的 0（默认）= 自动，让 sd.cpp 按**模型架构**挑：Wan 5、
# HunyuanVideo 7、MiniMax-H3 12、Qwen-Image 3。以前这里写死 3.0（Wan 的数），
# 换成 H3 之后一直在拿 Wan 的 time-shift 跑它，而且不报错。除非你在对某个
# 具体模型调参，否则别填。
# video_cfg = 0.0   # 0 = 按模型家族自动：H3 1.0、Wan A14B 3.5、Wan 5B 6.0
# video_flow_shift = 0.0
# image_cfg = 2.5
# image_flow_shift = 0.0
#
# 首帧按哪个档位出。默认 draft（和 Python 一样）；首帧是跨镜头一致性的锚点，
# 又会当起始图喂给出片那一步，草稿档的首帧配成片档的视频等于把锚点放大两倍
# 再用。显存够就填 final，慢一些但清楚。
# frame_tier = "draft"   # 默认 final；小卡上显存不够才降到 draft
# frame_steps = 0        # 首帧出几步，0 = 跟档位走（Turbo 压的 6 步不算）
#
# 双专家视频模型的高噪声那一份（Wan 2.2 的 A14B 系列）。video 填低噪声那份，
# 这里填高噪声那份，留空就是单模型。高噪声专家跑前几步定构图和运动，
# 低噪声专家跑后几步出细节。
# **换 A14B 之后 video_cfg 要跟着改**：上游给 A14B 的是 3.5，5B 那边是 6.0。
# video_high_noise = "Wan2.2-I2V-A14B-HighNoise-Q8_0.gguf"
#
# 两个专家在哪个 sigma 交班，默认 0.875（sd.cpp 的默认）。
# 调大 = 高噪声专家跑得更久，运动更大、细节更少。
# video_moe_boundary = 0.875
#
# 视频模型的 LLM 类文本编码器。video_text_encoder 走 t5xxl_path（Wan 那一路的
# UMT5-XXL），这一项走 llm_path（MiniMax-H3 那类用大语言模型当编码器的）。
# **两个只能填一个**，填错了不报错，只是出来的片和提示词没关系。
# video_llm = "qwen3vl_32b_minimax_h3-Q4_K_M.gguf"
# video_llm_vision = ""
#
# 音频 VAE。给 MiniMax-H3 这类画面和声音一起生成的模型用；不填的话联合扩散
# 照跑，但出来的片没有解码好的音轨。
# video_audio_vae = "minimax_h3_audio_vae_fp32.safetensors"
#
# 出片用哪种随机数发生器：cuda（默认，Wan 那一路）/ cpu / std。
# 上游给 MiniMax-H3 的命令行是 --rng cpu；发生器不同则同一个种子出的画面不同，
# 而且不报错。只影响出片，出图那条不动。
# video_rng = "auto"   # auto = 按模型家族：H3 cpu、其余 cuda
#
# 出片挂一个 LoRA。Turbo 那类蒸馏适配器能把采样步数压到 6 步左右（约 5 倍）。
# **挂上之后步数要跟着改**，不改的话白挂。认不认这类给 ComfyUI 做的 LoRA
# 要实测：不认时只是加载不上、画面照出，判据得看耗时有没有真降下来。
# video_lora = "loras/minimax_h3_turbo_v4_step600_ema.safetensors"
# video_lora_strength = 1.0
#
# LoRA 挂在哪些档位：draft / final / both（默认）。Turbo 那类蒸馏 LoRA 拿
# 画质换速度，适合只挂草稿档——草稿看叙事和构图，6 步够；成片跑满步数不挂。
# video_lora_tiers = "draft"
#
# 出片时 VAE 解码的分块大小（潜空间格子），0 = 用内置的 16×11。
# 调小换显存：块的计算缓冲小了，VAE 权重才有机会常驻显存。5090 上实测
# VAE 放内存解码要 71 秒、放显存只要 8 秒，而按内置块大小放显存会差 112 MB。
# video_vae_tile = 12
#
# 视频模型单段能出多少帧、帧数要落在什么格子上。
# **一般不用填**：引擎按上面 video 那个文件名自己认（MiniMax-H3 360/17k+5、
# Wan 121/4n+1），换模型就跟着换，不用记着改这里。
#   填了才覆盖它——卡小跑不动长镜头时用得上：
# video_max_frames = 124   # 17*7+5，正好在格子上 = 5.167 秒，回到一镜五秒的排法
# video_frame_step = 17
# video_frame_base = 5
#
# weights = "smart" 时，显存到多少才把 VAE 放显存（GB）。默认 40 是量出来的：
# 5090（32.6 GB）上扩散 17.9 + VAE 5.5 = 23.4 GB 权重，加扩散自己约 9 GB 的
# 计算缓冲就差 112 MB 装不下。VAE 放内存每镜解码 71 秒、放显存 8 秒，
# 所以大卡上一定要放进去。
# vae_vram_min_gb = 40.0
#
# weights = "auto" 时给计算缓冲留多少显存（GB）。auto 的预算是给权重的，
# 而生成时那块计算缓冲比"给驱动留一成"大一个量级：1280×704 的 VAE 解码
# 实测要 6.6 GB。留少了的症状是出图全失败、日志里 decode_first_stage failed。
# 出更大的图要调大它。
# vram_reserve_gb = 6.0
)";

// 见上面那段：超了就在下一个小节处再断一截出来，别把限额调大。
constexpr std::size_t kMsvcLiteralMax = 16380;
static_assert(std::char_traits<char>::length(kDefaultTomlHead) <
                  kMsvcLiteralMax,
              "配置模板的上半截超了 MSVC 的字符串字面量上限，Windows 上会编不过");
static_assert(std::char_traits<char>::length(kDefaultTomlTail) <
                  kMsvcLiteralMax,
              "配置模板的下半截超了 MSVC 的字符串字面量上限，Windows 上会编不过");
}  // namespace

std::string default_config_template() {
    return std::string(kDefaultTomlHead) + kDefaultTomlTail;
}

fs::path write_default_config(const std::optional<fs::path>& path) {
    fs::path target = path ? *path : user_config_path();
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("写不了配置文件：" + paths::to_utf8(target));
    out << kDefaultTomlHead << kDefaultTomlTail;
    return target;
}

// ---- 项目自己那份 ----

namespace {

// **只放这部电影的属性。** 模型文件、显存、端口、大模型地址是机器的属性，
// 在全局配置里；写到这儿的话把项目目录拷到另一台机器就跑不起来。
//
// 明写出来（而不是注释掉）的几项就是"一部电影的标准参数"：新建时按内置
// 默认值落下来，以后全局默认再变也不影响已建的电影——每部电影自己说了算。
// [models] 那一节全是注释：采样旋钮多数时候跟着模型走，按这部电影调才打开。
//
// 两个占位符由 write_project_config 换成真实取值。
constexpr const char* kProjectToml = R"(# 这部电影自己的配置（一个项目一份）。
#
# 只放「这部电影的属性」：画幅、装配、闸门、按这部电影调的采样旋钮。
# 机器的属性（模型文件、显存、端口、大模型地址）在全局配置里，不要写到这儿——
# 否则项目目录拷到另一台机器就跑不起来。
# 优先级：环境变量 > 本文件 > 全局配置 > 内置默认值
#
# 这份是新建项目时按标准参数生成的。每部电影的差异在这里改，别去改全局。

[video]
# 画幅是这部电影的属性，不是这台机器的：同一台机器上可以同时有横屏的正片和竖版的物料。
#   orientation = "portrait" | "landscape"
#   quality     = "720p" | "hd" | "2k"
#   720p → 544×928    hd → 704×1280    2k → 1440×2560（一张 32 GB 的卡跑不动）
#   （**这几个是竖屏的写法，横屏时反过来**：928×544 / 1280×704 / 2560×1440）
orientation = "@ORIENTATION@"
quality = "@QUALITY@"
# 单个镜头最长几秒。**这是这部电影的属性，不是显存的函数**：一镜只保留一个
# 核心动作。
#
# **2026-09-16 从 5.0 改成 15.0。** 原来这儿写着「实测 8 秒的镜头会中途硬切成
# 另一场戏」，所以压到 5 秒。那条实测多半记的是别的病的症状：同一天查出来，
# 中途硬切的根因是 motion_prompt 没盖满整镜时长（4 秒的镜头只写到 [0-2秒]，
# 剩下那截模型自由发挥）和运动描述里写了进画出画（人走进来、开门露出门后）。
# 两条都堵上了，闸门那句「首帧和提示词对不上，模型半路切到了提示词要的画面」
# 说的正是这件事。上限不该替内容做决定——一镜只演一件事由提示词那两句原话
# 管着：分镜那头的「一镜只演一件事」、剧本那头的「每一拍只写一件事」。
# 这里只管机器和这部电影允许多长。
#
# 真跑长镜头之前先看一眼 /api/hardware 的 shot.max_shot_s：它是四道夹子
# （模型帧数、显存、内核像素×帧、这一行）连乘、再落到 17k+5 格子上的结果。
# 0 = 按模型和这张卡自己定。
max_shot_s = 15.0
# 关键镜头（开场钩子、章尾留扣、反转，以及第一镜和最后一镜）多出几条换种子、
# 按闸门的数挑最好的一条。行业做法是关键镜多出 20～30%。1 = 不多出。
hero_takes = 2
# 连续动作的两镜（分镜里标了 continuous_with_prev 的），拿上一镜的最后一帧
# 当下一镜的首帧，动作接得上。
chain_frames = true

[preview]
# 「只做前 n 分钟」那个 n（秒）。分镜页那颗按钮和项目页的「一键成片」读的
# 都是这一个值：先花二十分钟看看这部片子长什么样，别等整章一个钟头跑完。
# 挑出来的是从第一镜起、累计**不少于**这个长度的那几镜（跨过这条线的那一镜
# 也算），装成 output/preview/<章号>.mp4，不占正片的位置。
seconds = 60

[assembly]
# 帧率在全局配置里（[assembly].fps）。**它只对报得出原生帧率的模型才会被
# 纠正**：MiniMax-H3 硬是 24，填别的会被改回去并在保存时说一句。
# Wan 那一族不报原生帧率，填什么就是什么——而 `/api/shots` 回的
# 「这一镜多长」目前写死按 24 算（readonly.cpp 里那段），两个数就对不上了：
# 成片页那条跳转条点哪一镜都偏、镜头页那句「这一章多长」也偏，全程不报错。
# 用 Wan 的话，帧率保持 24 最省事。
crf = 18
# ⚠️ 转场目前不生效：装配是 `-f concat -c copy` 直接拼，全程硬切。
# 这个数还收着，但改了不会有任何变化。见 media/assemble.cpp。
scene_transition_s = 0.4
# 中文字幕单行上限（全角字符数）和字体。
subtitle_max_chars_per_line = 15
subtitle_font = "Source Han Sans SC"

[look]
# 成片的后期链，装配时逐镜加：柔化 → 调色 → 颗粒。这是「电影质感」里最便宜
# 的一段（docs/电影质感方案.md）。
#   preset = "film"   柔化 + 调色 + 颗粒（默认）
#          = "clean"  只柔化和颗粒，不调色
#          = "off"    一个滤镜都不加
# 调色默认用内置曲线（S 形、暗部偏青、亮部偏暖）；有胶片 LUT 就填路径，
# 要 Rec.709 输入的版本（模型直出就是 709）。强度 0.5～0.7 是 AI 素材的区间。
preset = "film"
# lut = "luts/kodak2383_rec709.cube"
lut_strength = 0.6
# 颗粒 0～100（0 关）；柔化是像素，别超过 1。
grain = 10
soften = 0.4
# 横屏项目遮幅到 2.39（宽银幕）。竖屏忽略。0 = 关。
letterbox = 0

[sound]
# 台词之外的三层。环境声来自出片模型自己出的原生音轨（H3 每镜都有），
# 有没有台词都留着，压在台词底下；配乐要全局配了 music_command 才会生成。
ambient = true
ambient_db = -12
music = true
music_db = -20
# 台词处把环境声和配乐再压一道。
duck = true
# 配乐风格提示，空 = 只按剧本的拍子推。
music_style = ""

[gates]
# 质量闸门。全自动模式下这些阈值决定废片能不能被拦住。
enabled = true
max_attempts_per_shot = 3
# 重试超限时保留最后那一版（闸门没过，但那一镜的视频在），保证整章能出片而不是卡死。
fallback_on_exhausted = true

[models]
# **模型文件名不要写在这里**，那是机器的属性——每台的目录和文件名都不一样，
# 写进来的话项目目录拷到另一台就跑不起来。
#
# 但**"这部电影要哪一档"可以**，那是这部电影的属性，而且机器无关：档位 id 来自内置
# 目录，每台都认得，各自去自己的模型目录里找对应的文件。写在下面的
# [models.pick] 里，项目这一份盖全局（按键盖，没写的那几组照旧跟全局走）。
#
#   [models.pick]
#   llm   = "zhipu-free"
#   image = "qwen-image-q4"
#
# 不写 = 没挑过，那时候"选了哪一档"从全局配置里的文件名反推，和以前一样。
#
# 这一节剩下的只放按这部电影调的采样旋钮，
# 全部注释掉 = 跟全局走。flow_shift 的 0 = 自动（按模型架构挑）。
# video_cfg = 1.0
# video_flow_shift = 0.0
# video_lora_strength = 1.0
# image_cfg = 2.5
# image_flow_shift = 0.0
#
# 分档：草稿档挂 Turbo 跑 6 步看叙事，成片档不挂 LoRA 跑满步数（30 是手和
# 纹理的甜点）。默认 both = 两档都挂 Turbo，最快；要成片质量就改成 draft，
# 然后成片档只对留下的镜头跑（单章页上按镜头重跑）。
# video_lora_tiers = "draft"
)";

// 理由同上面那两条：这一份现在才 5 KB，但它也是只增不减的。
static_assert(std::char_traits<char>::length(kProjectToml) < kMsvcLiteralMax,
              "项目模板超了 MSVC 的字符串字面量上限，Windows 上会编不过");

void replace_all_in(std::string& s, const std::string& from,
                    const std::string& to) {
    for (std::size_t pos = s.find(from); pos != std::string::npos;
         pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
}

}  // namespace

std::string project_config_template() {
    std::string s = kProjectToml;
    const VideoConfig v;
    replace_all_in(s, "@ORIENTATION@", v.orientation);
    replace_all_in(s, "@QUALITY@", v.quality);
    return s;
}

bool write_project_config(const fs::path& project_root, const VideoConfig& video) {
    const fs::path target = project_root / paths::from_utf8("changji.toml");
    std::error_code ec;
    if (fs::exists(target, ec)) return false;
    // 先校验再写：写进去一个非法值的话，下一次加载整个项目都打不开。
    if (const auto errs = video.validate(); !errs.empty()) {
        throw std::runtime_error(errs.front());
    }
    std::string s = kProjectToml;
    replace_all_in(s, "@ORIENTATION@", video.orientation);
    replace_all_in(s, "@QUALITY@", video.quality);
    fs::create_directories(project_root, ec);
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("写不了项目配置：" + paths::to_utf8(target));
    out << s;
    return true;
}

}  // namespace changji::config
