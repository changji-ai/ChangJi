#include "http/readonly.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "media/assemble.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "stages/storyboard.hpp"
#include "util/fs_time.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

/// 这部电影自己的帧数格子。
///
/// `stages::video_limits()` 是进程里那**一份**全局的，只有出片和拆分镜
/// 那两条路会按项目重算（见 config::apply_video_limits）。这几个只读接口
/// 从来不设它，读到的于是是「上一次跑的是哪部电影」——同一张分镜表，出片
/// 前后报出来的时长能差三倍：本机那份上限是 124 帧，一个 15 秒的镜头在
/// 镜头墙上写着 5.2 秒，整章时长跟着一起错，成片页那条跳转条也偏。
/// 2026-09-16 实测：hulian-test 的 ep06_sh019 计划 15 秒，接口报 5.2 秒。
///
/// 这个接口跑得勤（出片时每 6 秒一次），所以按 changji.toml 的修改时间
/// 缓存——没改就不重读 TOML。
const stages::VideoLimits& limits_for_project(const std::filesystem::path& root) {
    struct Entry {
        std::filesystem::file_time_type stamp{};
        stages::VideoLimits limits;
    };
    static std::mutex mu;
    static std::map<std::string, Entry> cache;

    const std::filesystem::path toml = root / "changji.toml";
    std::filesystem::file_time_type stamp{};
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(toml, ec);
    if (!ec) stamp = t;

    const std::string key = paths::to_utf8(root);
    std::lock_guard lg(mu);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.stamp == stamp) return it->second.limits;

    Entry e;
    e.stamp = stamp;
    // 读不出来（目录刚没了、TOML 语法坏了）不能让只读接口整个塌掉：
    // 退回进程里那一份，和改之前一样，至少还能看。
    try {
        e.limits = config::video_limits_for(config::load_settings(root));
    } catch (const std::exception&) {
        e.limits = stages::video_limits();
    }
    return cache.insert_or_assign(key, std::move(e)).first->second.limits;
}

}  // namespace


namespace {

using namespace changji::models;

/// Python 的 round(x, 1)。银行家舍入，不是 std::round。
/// 详见 models/hardware.cpp 里的同名函数和它上面那段注释。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

/// 对应 Python 的 _store(path)：路径为空是 400 不是 500。
ProjectStore store_for(const std::string& path) {
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

/// 同上，外加**一定得是个项目**。
///
/// `store_for` 只是把路径包起来，不问那儿有没有东西。`get_project` 和
/// `get_shots` 紧接着会 `load_project()`，所以路径不对时它们照实抛；而
/// `get_assets` 只 `load_assets()`——文件不在就回一个空库，**于是随便指一个
/// 目录，它都回 200 加「没有角色、没有场景」**。三个接口对"什么算项目"说的
/// 不是同一句话。
///
/// 后果在界面上：桌面端设定那一格看到 200 + 空库，就照实写「还没有人物和
/// 场景。说一句「把人物和场景提出来」就行」——而那个目录根本不是项目
/// （2026-09-21 收"读不出来要说出来"那一族时撞见的）。
///
/// 那句话只写在一处（`ProjectStore::load_project`），所以这儿靠它抛，
/// 不另写一遍。
ProjectStore project_for(const std::string& path) {
    ProjectStore store = store_for(path);
    if (store.exists()) return store;
    // 必抛，消息由 `load_project` 给（那句话只写在一处）。**转成 400**：
    // 让原始的 runtime_error 漏出去的话，`guard` 会按"没预料到的异常"处理
    // ——500，而且前面挂一句「服务端出错：」。路径填错不是服务端的错，而
    // `/api/project` 对同一件事回的正是 400（实测过两边）。
    try {
        store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
    return store;   // 走不到
}

/// 三个档位的参数，/api/hardware 和 /api/settings 都要。
json tiers_json(const std::map<Tier, TierSpec>& tiers, bool with_seconds) {
    json out = json::object();
    for (Tier t : all_tiers()) {
        const auto it = tiers.find(t);
        if (it == tiers.end()) continue;
        const TierSpec& s = it->second;
        json entry = {
            {"width", s.width},
            {"height", s.height},
            {"steps", s.steps},
        };
        if (with_seconds) {
            entry["seconds"] = s.measured_seconds.has_value()
                                   ? json(*s.measured_seconds)
                                   : json(nullptr);
        }
        out[to_string(t)] = entry;
    }
    return out;
}

/// std::optional<std::string> 转 JSON：无值是 null，对应 Python 的 None。
json opt(const std::optional<std::string>& v) {
    return v.has_value() ? json(*v) : json(nullptr);
}

}  // namespace

ApiResult get_hardware(const config::Settings& settings) {
    return get_hardware(settings,
                        HardwareProfile::detect(settings.vram_gb_override));
}

ApiResult get_hardware(const config::Settings& settings,
                       const HardwareProfile& p) {
    // **单镜最长能出多久，要看得见。** 2026-09-16 撞到：用户说「分镜时间改到
    // 最大 15 秒」，而这个数是四道夹子连乘出来的——模型能出多少帧、这张卡的
    // 显存、内核的像素×帧上限、这部电影的 [video].max_shot_s，再加上帧数必须
    // 落在 17k+5 的格子上。中间任何一道把它压回 5 秒，界面上一个字都没有，
    // 只能靠翻源码倒推。摆出来之后，「为什么排不出长镜头」是一眼的事。
    const auto& limits = stages::video_limits();
    const int fps = stages::effective_fps(limits, settings.assembly.fps);
    json slots = json::array();
    for (const double s : limits.duration_slots(fps)) slots.push_back(s);
    return {200, {
        {"gpu", p.gpu.has_value() ? json(p.gpu->name) : json(nullptr)},
        {"vram_gb", round1(p.vram_gb)},
        {"detected", p.detected},
        {"tiers", tiers_json(p.tiers, /*with_seconds=*/true)},
        {"shot", {
            {"max_frames", limits.max_frames},
            // 落在格子上、真正生成得出来的那个帧数。和 max_frames 差一截是
            // 常态（360 → 345），差的那一截正是「排了却出不来」的来源。
            {"max_frames_on_grid", limits.max_frames_on_grid()},
            {"frame_grid", std::to_string(limits.frame_step) + "k+" +
                               std::to_string(limits.frame_base)},
            {"fps", fps},
            {"max_shot_s", round1(limits.max_duration_s(fps))},
            {"duration_slots", slots},
        }},
    }};
}

ApiResult get_settings(const config::Settings& s) {
    return get_settings(s, HardwareProfile::detect(s.vram_gb_override));
}

ApiResult get_settings(const config::Settings& s, const HardwareProfile& p) {
    return {200, {
        {"tiers", tiers_json(p.tiers, /*with_seconds=*/false)},
        {"assembly", {
            {"fps", s.assembly.fps},
            {"crf", s.assembly.crf},
            {"subtitle_font", s.assembly.subtitle_font},
            {"subtitle_max_chars_per_line", s.assembly.subtitle_max_chars_per_line},
            {"subtitle_max_lines", s.assembly.subtitle_max_lines},
            {"scene_transition_s", s.assembly.scene_transition_s},
        }},
        {"gates", {
            {"enabled", s.gates.enabled},
            {"max_attempts_per_shot", s.gates.max_attempts_per_shot},
            {"min_pixel_std", s.gates.min_pixel_std},
            {"min_frame_similarity", s.gates.min_frame_similarity},
            {"max_audio_drift_s", s.gates.max_audio_drift_s},
            {"target_lufs", s.gates.target_lufs},
            {"fallback_on_exhausted", s.gates.fallback_on_exhausted},
        }},
        // **这里原来有 tts 那一组（tolerance_s / max_tempo_shift）。**
        // 2026-09-13 两项都删了——有校验、有持久化、设置页能改，但引擎里
        // 没有任何一个阶段读过。删完这一组一项不剩，整个键跟着去掉：
        // 留一个空对象的话 nlohmann 会把它序列化成 null，前端拿到
        // `settings.tts.xxx` 就是一次 undefined。
        // backend / base_url 不在这儿，它们在 /api/connections。
    }};
}

ApiResult get_project(const std::string& path) {
    ProjectStore store = store_for(path);
    Project p;
    AssetLibrary assets;
    try {
        p = store.load_project();
        assets = store.load_assets();
    } catch (const std::exception& e) {
        // Python 那边 catch 的是 FileNotFoundError 和 ValueError，都转 400
        throw ApiError(400, e.what());
    }

    json characters = json::array();
    for (const auto& kv : assets.characters) {
        characters.push_back({{"char_id", kv.second.char_id},
                              {"name", kv.second.name}});
    }
    json locations = json::array();
    for (const auto& kv : assets.locations) {
        locations.push_back({{"location_id", kv.second.location_id},
                             {"name", kv.second.name}});
    }
    json episodes = json::array();
    for (const auto& e : p.episodes) {
        // **和下面那个 `shots` 数的是同一批镜头。** 两个数在同一份回包里，
        // 界面拿它们相减算「还差几镜出片」（EpShots 的 projectPending）——
        // 一个数全部、一个数能用的，就会算出负数或者漏算。空壳镜头
        //（shot_id 是空串）两处一起不算，理由见下面那段。
        json status = json::object();
        for (const auto& sh : e.shots) {
            if (text::strip_ws(sh.shot_id).empty()) continue;
            const std::string k = models::to_string(sh.status);
            status[k] = status.value(k, 0) + 1;
        }
        episodes.push_back({
            {"episode_id", e.episode_id},
            {"title", e.title},
            {"synopsis", e.synopsis},
            // **数的是"能用的镜头"，不是数组长度。**
            //
            // 空壳镜头（shot_id 是空串）指不到任何文件、进不了任何一步。
            // 而这个数是好几处的判据：顶栏那几个步骤对勾（http/flow.cpp 的
            // has_shots）、镜头页那颗按钮上的「还差 N 章分镜」、下拉里的
            // 「（17 镜）」。报数组长度的话，一章空壳在这三处都显示成
            // "已经有分镜了"。2026-09-17 实见 ep07 就是这样。
            //
            // 正常流程产不出空壳（那个 bug 当天修了，见 stages/render.cpp
            // 的 Done::ran），但存盘被截断、手工改坏一样留得下——而**"有
            // 几镜"的答案本来就不是数组长度**。批量补分镜那条判据同日改成
            // 一样的口径（http/batch.cpp）。
            {"shots", static_cast<int>(std::count_if(
                          e.shots.begin(), e.shots.end(), [](const auto& s) {
                              return !text::strip_ws(s.shot_id).empty();
                          }))},
            // **挂在哪一章。** 界面靠它分辨"两条看起来一样的章节记录"：一章
            // 一条是现在的规矩，而规矩立起来之前留下的重复（两条挂同一章）
            // 在下拉里长得一模一样——2026-09-16 实见 ep08 和 ep09 都写着
            // 「了结（17 镜）」，选哪个全靠猜。
            {"chapter_refs", e.chapter_refs},
            // 「理解过没有」的判据之一：每一章的剧本写了没有。只报字数，
            // 正文另走 /api/script——这份回包每一页都在拉。
            {"script_chars", static_cast<int>(text::utf8_len(e.script))},
            // **界面上这个数标着"时长"，那它就得是成片的长度。**
            //
            // 不是 planned_duration_s()——那是分镜表里那串名义值的和，而
            // 模型只能按格子出帧（见 stages::real_total_s）。两者能差
            // 10%：walk_c ep01 名义 58.0 秒，成片 61.8 秒。
            //
            // 副作用是这个数**跟着这台机器变**：卡小的时候 VideoLimits 被
            // 夹低，同一个项目报出来的时长会短一些。那是实话——在这台机器
            // 上渲出来就是那么长——但别拿它当项目的固有属性。要看人当初
            // 要的是多长，看 target_duration_s。
            {"duration_s",
             round1(stages::real_total_s(
                 e.shots, limits_for_project(store.root()), 24))},
            {"status", status},
        });
    }

    return {200, {
        {"project_id", p.project_id},
        {"title", p.title},
        {"style_line", to_string(p.style_line)},
        // 这部电影讲什么。剧本页拿它回填梗概框，不用凭记忆重打。
        {"premise", p.premise},
        {"root", paths::to_utf8(store.root())},
        {"characters", characters},
        {"locations", locations},
        {"episodes", episodes},
    }};
}

ApiResult get_shots(const std::string& path, const std::string& episode_id) {
    ProjectStore store = store_for(path);
    const Project p = store.load_project();
    const Episode* ep = p.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));

    json shots = json::array();
    for (const auto& s : ep->sorted_shots()) {
        json char_ids = json::array();
        for (const auto& c : s.characters) char_ids.push_back(c.char_id);

        json dialogue = json::array();
        json audio_paths = json::array();
        for (const auto& d : s.dialogue) {
            dialogue.push_back({
                {"char_id", opt(d.char_id)},
                {"text", d.text},
                {"duration_s", d.actual_duration_s.has_value()
                                   ? json(*d.actual_duration_s)
                                   : json(nullptr)},
            });
            if (d.audio_path.has_value() && !d.audio_path->empty()) {
                audio_paths.push_back(*d.audio_path);
            }
        }

        shots.push_back({
            {"shot_id", s.shot_id},
            {"order", s.order},
            {"scene_id", s.scene_id},
            // 这一章用到哪几个场景，界面靠它算。缺了的话设定页的
            // 「场景」那一格只能把全片的场景一股脑列出来，看不出跟
            // 本章的关系。
            {"location_id", opt(s.location_id)},
            {"char_ids", char_ids},
            {"shot_size", to_string(s.shot_size)},
            {"camera_angle", to_string(s.camera_angle)},
            {"camera_move", to_string(s.camera_move)},
            // 编辑器要回填这些，缺了的话打开是空的，
            // 用户以为本来就没内容，一保存就把原文清空了
            {"first_frame_prompt", s.first_frame_prompt},
            {"motion_prompt", s.motion_prompt},
            {"negative_prompt", s.negative_prompt},
            {"subtitle_text", s.subtitle_text},
            {"transition_in", to_string(s.transition_in)},
            {"transition_dur_s", s.transition_dur_s},
            {"continuity_notes", s.continuity_notes},
            {"duration_s", s.duration_s},
            {"duration_locked", s.duration_locked},
            {"needs_lipsync", s.needs_lipsync},
            {"status", to_string(s.status)},
            {"attempts", s.attempts},
            {"visual_desc", s.visual_desc},
            {"dialogue", dialogue},
            {"gate_notes", s.gate_notes},
            {"has_video", s.video_path.has_value() && !s.video_path->empty()},
            // 审片要用。只给相对路径，取文件走 /api/media，
            // 那里做了越界检查，不会读到项目目录之外。
            {"frame_path", opt(s.frame_path)},
            {"video_path", opt(s.video_path)},
            {"audio_paths", audio_paths},
            {"beat", s.beat},
            // **这一镜真正会出多长。** `duration_s` 是名义值（档位表里挑的
            // 整数，编辑器改的也是它），而模型只能按格子出帧——名义 4 秒
            // 出来是 107 帧 = 4.458 秒。
            //
            // 界面上凡是给人看的时长都该用这个。前端原来是自己 reduce 累加
            // `duration_s`，于是标题写「18 镜 · 58 秒」而成片是 61.8 秒
            // （ffprobe 量的）——**格子规则不能在前端复刻一份**，那是第三份
            // 副本，模型一换就全错。
            //
            // ⚠️ **这儿按 24 fps 算**（`real_duration_s` 的默认参数），而装
            // 配那边用的是 `config.fps`（media/assemble.cpp）。今天两边一定
            // 相等：帧率跟着出片模型走，当前这个（MiniMax-H3）硬是 24，配置
            // 里写别的会被 normalize_fps_for_model 纠回去。
            //
            // 哪天接一个 native_fps 不是 24 的模型、或者接一个不报 native_fps
            // 的模型而用户把 [assembly].fps 填成 30——这两个数就会对不上，
            // 表现是成片页那条跳转条点哪一镜都偏、镜头页那句"这一章多长"也
            // 偏，而全程不报错。那时候要把项目的 fps 传进来（注意这个接口
            // 跑得很勤：出片时每 6 秒一次，别顺手在里面读 TOML；而且对拍
            // 语料按现在这样钉着）。
            {"real_duration_s",
             round1(limits_for_project(store.root()).real_duration_s(
                 s.duration_s))},
        });
    }
    // 整章多长。前端别自己加：单镜是四舍五入过的，逐镜加会带累积误差。
    return {200,
            {{"shots", shots},
             // **这张表是不是照改之前那版剧本拆的。** 见 Episode::shots_from：
             // 「理解故事」和「批量改编」会照新正文把剧本重写一遍，而那两条
             // 路都不重拆分镜（那是有意的，重拆会覆盖整张表）。不带这一条
             // 出去的话，镜头页上那 151 镜看着和昨天一模一样，而它们照的是
             // 一份已经不存在的剧本。
             //
             // 挂在信封上、不挂在每一镜上：整张表是一次拆出来的，一镜一个
             // 答案是假的精度，而且 151 份一模一样的 true 要多发一大截。
             {"shots_stale", ep->shots_stale()},
             {"duration_s",
              round1(stages::real_total_s(
                  ep->shots, limits_for_project(store.root()), 24))}}};
}

namespace {

/// 一条起点。`why` 是给人看的那一行小字。
json dir_entry(const fs::path& p, std::string why = {}) {
    json j{{"path", paths::to_utf8(p)},
           {"name", paths::to_utf8(p.filename().empty() ? p : p.filename())}};
    if (!why.empty()) j["why"] = std::move(why);
    return j;
}

}  // namespace

ApiResult get_dirs(const std::string& path, const config::Settings& settings) {
    std::error_code ec;
    json roots = json::array();

    // 没给路径：给几个起点。**空手起步是这条接口存在的理由**——人要是
    // 知道路径，他就直接敲进那个框了。
    const fs::path home = paths::home_dir();
    const fs::path ws = settings.workspace_path();
    const fs::path models = settings.models.dir_path(ws);
    if (fs::is_directory(home, ec)) {
        roots.push_back(dir_entry(home, SAY("用户目录")));
    }
    if (fs::is_directory(ws, ec) && ws != home) {
        roots.push_back(dir_entry(ws, SAY("项目库")));
    }
    if (fs::is_directory(models, ec) && models != ws && models != home) {
        roots.push_back(dir_entry(models, SAY("现在的模型目录")));
    }
    // **每一块盘也是一个起点。** 只有上面三条的话，它们全在系统盘上——从用户目录
    // 一级级往上翻能翻到 `C:\`，却翻不到 `D:\`，模型就只能放在最挤的那块盘上
    // （用户 2026-09-24：「模型目录要可以选择到别的地方」）。盘符自己就说清了是什么，
    // 不另配一行小字。
    for (const auto& v : paths::volume_roots()) roots.push_back(dir_entry(v));

    if (text::strip_ws(path).empty()) {
        return {200, {{"path", ""}, {"parent", nullptr},
                      {"entries", json::array()}, {"roots", roots}}};
    }

    const fs::path here = fs::absolute(paths::expand_user(path), ec);
    if (!fs::is_directory(here, ec)) {
        // **说清是"不在"还是"不是目录"**：前者多半是敲错了，后者是选了
        // 一个文件，两种要做的事不一样。
        throw ApiError(
            400, fs::exists(here, ec)
                     ? SAYF("这不是一个目录：%1", paths::to_utf8(here))
                     : SAYF("这个目录不在：%1", paths::to_utf8(here)));
    }

    json entries = json::array();
    std::vector<fs::path> children;
    for (const auto& e : fs::directory_iterator(
             here, fs::directory_options::skip_permission_denied, ec)) {
        children.push_back(e.path());
    }
    std::sort(children.begin(), children.end());
    for (const auto& child : children) {
        if (!fs::is_directory(child, ec)) continue;
        const std::string name = paths::to_utf8(child.filename());
        // 点开头的一律不列：`.git` `.venv` 这些在模型目录旁边到处都是，
        // 而没有人会把模型放进去。
        if (name.empty() || name.front() == '.') continue;
        entries.push_back(dir_entry(child));
    }

    const fs::path up = here.parent_path();
    return {200,
            {{"path", paths::to_utf8(here)},
             // 到根了就没有上一级。别回一个指向自己的 parent——界面上那个
             // 「上一级」会变成点了没反应。
             {"parent", up.empty() || up == here ? json(nullptr)
                                                 : json(paths::to_utf8(up))},
             {"entries", entries},
             {"roots", roots}}};
}

ApiResult get_assets(const std::string& path) {
    ProjectStore store = project_for(path);
    AssetLibrary assets;
    try {
        assets = store.load_assets();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }

    // 参考图一定会**原样交给出图模型**（见 sd_image.cpp 里的 ref_images）。
    // 但只有**图像编辑类**模型真的照着画——纯文生图的收下之后一声不吭地
    // 忽略，画面一点不变。不说清楚的话，用户传了图、镜头也退回重跑了，
    // 画面却一模一样，而且没有任何报错。
    //
    // 判据是模型文件名：Qwen-Image-Edit、Flux Kontext 这一类名字里都带
    // edit 或 kontext。认错了**宁可往"多提醒一句"那边错**——说"不看"而其实
    // 在看，用户至多多读一行字；反过来那一半才是前面说的那种查不出来的
    // 白跑。
    //
    // （这一段原来判的是项目里有没有 workflows/image.json。那是 ComfyUI
    // 时代的东西，这个二进制里一行都不剩了，于是它永远为假——页面上那句
    // "把工作流存成 workflows/image.json" 指向一个不存在的做法。）
    const config::Settings img_settings = config::load_settings(store.root());
    std::string image_model =
        paths::to_utf8(paths::from_utf8(img_settings.models.image).filename());
    for (char& ch : image_model) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    const bool refs_honored = image_model.find("edit") != std::string::npos ||
                              image_model.find("kontext") != std::string::npos;

    const StyleLine line = assets.style.style_line;

    // 「这一条有没有参考图」。
    //
    // **判据是那个文件在不在盘上**，不是"路径这一栏填没填"
    // （CLAUDE.md 第四条）。设定是先写好路径、之后才去出图的，所以路径那一
    // 栏几乎总是满的——照它判的话所有人永远都"有图"。
    //
    // 为什么由引擎说：图在引擎那台机器上。桌面端可能在另一台，`stat` 不到；
    // 而它拿到的 404 分不清"还没出"和"读坏了"——前者是下一步该干什么，
    // 后者是出事了。**一处算，两处用**（第八条）：代理那句摘要和桌面端那
    // 一格读的是同一个字段。
    const auto has_file = [&store](const std::optional<std::string>& rel) {
        if (!rel.has_value() || rel->empty()) return false;
        std::error_code ec;
        return fs::is_regular_file(store.root() / paths::from_utf8(*rel), ec);
    };

    json characters = json::array();
    for (const auto& kv : assets.characters) {
        const Character& c = kv.second;
        characters.push_back({
            {"char_id", c.char_id},
            {"name", c.name},
            {"identity", c.appearance.identity},
            {"body", c.appearance.body},
            {"face", c.appearance.face},
            {"attire", c.appearance.attire},
            {"style", c.appearance.style},
            {"voice_id", opt(c.voice_id)},
            // 猜出来的性别。音色留空时按它挑，猜错了用户得看得见
            {"voice_gender", c.voice_gender},
            {"ref_front", opt(c.ref_front)},
            {"ref_three_quarter", opt(c.ref_three_quarter)},
            {"ref_back", opt(c.ref_back)},
            {"lora_trigger", opt(c.lora_trigger)},
            {"lora_strength", c.lora_strength},
            {"rendered", c.render_prompt(line)},
            // 正面那张在不在。**三张里只认正面**：界面和代理说"有图"的时候
            // 指的都是"能看见他长什么样"，而那是正面那张。
            {"has_ref", has_file(c.ref_front)},
        });
    }

    json locations = json::array();
    for (const auto& kv : assets.locations) {
        const Location& l = kv.second;
        locations.push_back({
            {"location_id", l.location_id},
            {"name", l.name},
            {"space", l.space},
            {"lighting", l.lighting},
            {"palette", l.palette},
            // 空景图是场景一致性的锚点。以前没回传，界面上就没法
            // 显示也没法换，等于这个字段只有命令行够得着。
            {"ref_empty", opt(l.ref_empty)},
            {"rendered", l.render_prompt(line)},
            {"has_ref", has_file(l.ref_empty)},
        });
    }

    return {200, {
        {"reference_images_used", refs_honored},
        // ⚠️ **这句话两次指错路。**
        //
        // 最早写的是「去项目页「模型」那一行换一个图像编辑模型
        // （Qwen-Image-Edit、**Flux Kontext** 这类）」——Flux Kontext 的
        // 文件名是 `flux1-kontext-…`，`accepts_reference_images` 认的是名字
        // 里的 "edit"，认不出来；照那句话换过去参考图照样一张都不传。
        //
        // 后来改成「模型下载那一组里没有这一档（它那一组是文生图），要自己
        // 下好再填路径」——**2026-09-15 起也不成立了**：首帧那一组整族换成了
        // Qwen-Image-Edit 2509，视觉塔（image_text_encoder_vision）也跟着一
        // 起下，弹窗里挑一档点保存就齐了。再教人去手填路径是把人支去绕远路。
        //
        // 所以现在只剩一种走到这儿的情形：**配置里还指着一个非 edit 的权重**
        // ——老装机留下的基础版 Qwen-Image，或者自己手填过。指回那个弹窗即可。
        //
        // （为什么基础模型一律不传：sd.cpp 见到 ref_images 就走 EDIT mode，
        // 基础版出来的是参考图的翻版——settings.hpp 那段记着实见的那一镜。）
        {"reference_hint", refs_honored ? std::string() :
            SAY(
            "参考图会传给出图模型，但当前这个是纯文生图的，它不会照着画——"
            "画面靠的是下面那段拼出来的提示词。要让参考图真生效，"
            // 项目页上那是**一行**（`line__k` 写着「模型」，后面四个名字
            // 各是一个按钮），不是一节；而且要换的是四个里的哪一个也得说
            // 出来——参考图归首帧那一组。只说"那一节"的人会在项目页上找
            // 一个不存在的小标题。
            "去项目页「模型」那一行点开首帧那一个，挑一档 Qwen-Image-Edit "
            "2509 下下来（视觉塔会跟着一起下）。⚠️ 引擎是按**文件名**认的，"
            "名字里带 edit 才算——官方那几份自带，改过名就认不出来。")},
        {"characters", characters},
        {"locations", locations},
        {"style", {
            {"style_line", to_string(assets.style.style_line)},
            {"global_style", assets.style.global_style},
            {"negative_prompt", assets.style.negative_prompt},
            // **报画幅算出来的那个，不报盘上那份拷贝。** 老项目里那份
            // 可能和画幅不一致（2026-09-14 之前它能单独改），而界面上
            // 显示一个跑的时候根本不会用的值，比不显示更糟。
            {"aspect_ratio", img_settings.video.aspect_ratio()},
        }},
    }};
}

ApiResult get_projects(const config::Settings& settings) {
    // 项目库里有哪些项目。
    //
    // 以前只能手打一条绝对路径。在容器里跑的时候那是
    // /data/projects/片名 这种路径，用户根本不知道该填什么。
    const fs::path root = settings.workspace_path();
    std::error_code ec;

    struct Item {
        json body;
        double mtime = 0.0;
    };
    std::vector<Item> items;

    if (fs::is_directory(root, ec)) {
        // Python 那边是 sorted(root.iterdir())，directory_iterator 不保证顺序，
        // 先收齐再排。虽然最后按 mtime 重排，但 mtime 相同的条目要稳定。
        std::vector<fs::path> children;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            children.push_back(e.path());
        }
        std::sort(children.begin(), children.end());

        for (const auto& child : children) {
            if (!fs::is_directory(child, ec)) continue;
            const std::string dir_name = paths::to_utf8(child.filename());
            if (!dir_name.empty() && dir_name.front() == '.') continue;

            ProjectStore store(child);
            if (!store.exists()) continue;

            Project project;
            try {
                project = store.load_project();
            } catch (const std::exception& e) {
                // **mtime 用目录自己的，不是 0.0。** 传 0.0 的话下面那个
                // 降序 stable_sort 会把坏掉的项目永久钉在列表最后一条——
                // 而「出问题的东西」正该是最先看见的。
                items.push_back({json{{"path", paths::to_utf8(child)},
                                      {"dir", dir_name},
                                      {"name", dir_name},
                                      {"broken", e.what()}},
                                 util::file_mtime_unix(child)});
                continue;
            }

            // **正片和预告分开数。** 预告片和正片同住 project.episodes，
            // 混在一起数的话，一部只剪了预告的电影在项目库里显示成「1 章」。
            int episode_count = 0;
            for (const auto& ep : project.episodes) {
                if (is_regular_episode(ep.episode_id)) ++episode_count;
            }

            int shots = 0, done = 0;
            for (const auto& ep : project.episodes) {
                shots += static_cast<int>(ep.shots.size());
                for (const auto& sh : ep.shots) {
                    if (sh.status == ShotStatus::FINAL_DONE ||
                        sh.status == ShotStatus::LOCKED) {
                        ++done;
                    }
                }
            }

            // 成片数。**只认文件名恰好等于某一章正片 id 的那些。**
            //
            // 装配写出去的名字就是 `episode_id + ".mp4"`（见 media/assemble
            // 那一头），所以正着匹配即可。反过来"排除预告"用子串判是错的：
            // 任何一个短 id（`ep`、`e` 这种，POST /api/episode 不拦长度）都会
            // 命中全部 epNN.mp4，一部全出完的电影 outputs 恒为 0。
            //
            // 顺带把分母兜住：这么数出来的 outputs 天然 ≤ 正片章数，不会出现
            // 「删了几章但 output 目录里的 mp4 还在」导致进度条永远满格。
            std::set<std::string> regular_ids;
            for (const auto& ep : project.episodes) {
                if (is_regular_episode(ep.episode_id)) {
                    regular_ids.insert(ep.episode_id);
                }
            }
            //
            // ⚠️ **老项目里带 `_NN` 的那些也得算。** 2026-09-18 之前一章会在
            // 章内切成几段，叫 `ep01_01.mp4` / `ep01_02.mp4`；今天一章只出一个
            // `ep01.mp4`，**再也不产出 `_NN`**（见 media/assemble.hpp 的
            // `episode_of_output`），但老项目盘上还躺着那种名字。原来只认不带
            // `_NN` 的那一种，于是切过的章算 0 章出片——顶栏那句「N/M 章已出片」
            // 把它们全漏了。2026-09-17 实见：ep01、ep06 各切两段，两章都没算进去。
            //
            // **一章算一次**，不是一个文件算一次：切成两段仍然是一章出完了。
            // 判法照上面那条注释的告诫，**不用子串**：要么整个 stem 就是
            // 那个 id，要么是 id + '_' + 两位数字。短 id 不会误伤。
            std::set<std::string> produced;
            const fs::path out_dir = store.paths().output();
            if (fs::is_directory(out_dir, ec)) {
                for (const auto& f : fs::directory_iterator(out_dir, ec)) {
                    if (f.path().extension() != ".mp4") continue;
                    // 判法只有一份，在 media::episode_of_output 上
                    const auto owner =
                        media::episode_of_output(paths::to_utf8(f.path().stem()));
                    if (owner && regular_ids.count(*owner) != 0) {
                        produced.insert(*owner);
                    }
                }
            }
            const int outputs = static_cast<int>(produced.size());

            // Unix 秒。**不能直接用 time_since_epoch()**：
            // file_time_type 的纪元由实现定，MSVC 用的是 1601-01-01，
            // 一个刚写的文件在那上面是一百三十多亿秒。前端的 humanAgo()
            // 按 Unix 秒算，会得出 2381 年、然后一律显示"刚刚"——
            // 每个项目都显示"刚刚"，看起来像是没坏。
            double mtime = 0.0;
            const fs::path pf = store.paths().project_file();
            if (fs::is_regular_file(pf, ec)) {
                mtime = util::file_mtime_unix(pf);
            }

            // 故事那几个数。**挑项目时真正想知道的是「这部电影讲什么、到哪
            // 一步了」**，而不是三个产出计数——刚建的空项目和故事写完还没
            // 分镜的项目，在「出片百分比」上都是 0%，看上去一模一样。
            //
            // 多读一个 story.json 的代价：它可能有几百 KB（章节正文全在
            // 里面）。但这个列表本来就在读 project.json，而那个装着整张
            // 分镜表，通常更大——多这一份不改变量级。
            std::string logline;
            std::string story_broken;
            int chapters = 0;
            int written_chapters = 0;
            int planned_episodes = 0;
            try {
                const auto story = store.load_story();
                logline = story.logline.empty() ? story.premise : story.logline;
                chapters = static_cast<int>(story.chapters.size());
                written_chapters = story.written_chapters();
                planned_episodes = static_cast<int>(story.plan.size());
            } catch (const std::exception& e) {
                // 读不了就当没有。**一个坏掉的 story.json 不该让整个项目库
                // 列不出来**——那时候用户连"去哪个项目修它"都看不见。
                //
                // **但要把「坏了」和「空的」分开。** 2026-09-14 之前这里
                // 一声不吭，于是一个正文全写完、只是 JSON 崩了的项目，在
                // 项目库里和 smoke-tmp 那种空壳长得一模一样（都是「还没写
                // 故事」）——顺手删掉的正是投入最多的那一个。
                story_broken = e.what();
            }

            // 资产库同理。**「坏了」和「空的」也要分开**——上面那段话
            // （一个写完的项目和空壳长得一模一样）对 assets.json 一字不差
            // 地成立：它装着角色、服装和一堆参考图路径，形状歪了就整份读
            // 不出来，而"读不出来"和"这个项目还没定妆"在这条列表上原来是
            // 同一个样子。
            //
            // 只读、不算数：这条列表不显示角色数，要的只是"它是不是坏的"，
            // 好让项目库上那一行能说出来、而不是等人点进去撞一个 400。
            std::string assets_broken;
            try {
                (void)store.load_assets();
            } catch (const std::exception& e) {
                assets_broken = e.what();
            }

            if (logline.empty()) logline = project.premise;

            // **砍短了要说一声。**
            //
            // 这一行是项目页上「这是哪部电影」的全部内容，而它常常不是一句
            // 真正的 logline：没写大纲的项目回落到梗概，而梗概能有两千字。
            // 砍到 80 字不加任何记号的话，屏幕上就是一句从中间断掉的话，
            // 看着像是内容写坏了——而这一页别处没有第二个地方能对出来。
            //
            // 比字节数就够：truncate_utf8 回的是原串的前缀。
            const std::string one_line = text::collapse_ws(logline);
            std::string shown = text::truncate_utf8(one_line, 80);
            if (shown.size() < one_line.size()) shown += "…";

            items.push_back({json{
                {"path", paths::to_utf8(child)},
                // 目录名单独给，前端不该自己去切路径分隔符，
                // 那段代码在 Windows 和容器里得写两套
                {"dir", dir_name},
                {"name", project.title.empty() ? dir_name : project.title},
                // ⚠️ **style_line 2026-09-14 摘掉了。** 引擎每次列项目都算，
                // 而前端全库零引用——画风属于「这一部电影的详情」，归项目页
                // 的「这部电影长什么样」那个弹窗。
                {"episodes", episode_count},
                {"shots", shots},
                {"done_shots", done},
                {"outputs", outputs},
                {"logline", shown},
                {"chapters", chapters},
                {"written_chapters", written_chapters},
                {"planned_episodes", planned_episodes},
                {"story_broken", story_broken},
                {"assets_broken", assets_broken},
                {"mtime", mtime},
            }, mtime});
        }
    }

    // 新的在前。stable_sort 保证 mtime 相同时保持目录名顺序，
    // 对应 Python 那边 sorted() 的稳定性。
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.mtime > b.mtime; });

    json projects = json::array();
    for (auto& it : items) projects.push_back(std::move(it.body));

    return {200, {
        {"workspace", paths::to_utf8(root)},
        {"projects", projects},
    }};
}

}  // namespace changji::http
