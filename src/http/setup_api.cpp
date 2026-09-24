#include "http/setup_api.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "config/model_index.hpp"
#include "config/runtime.hpp"
#include "config/writeback.hpp"
#include "config/model_patch.hpp"
#include "setup/downloader.hpp"
#include "setup/source.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

using setup::catalog;
using setup::Group;
using setup::Item;
using setup::kNoneOption;
using setup::Option;

bool download_in_progress(const fs::path& p) {
    std::error_code ec;
    fs::path ctrl = p;
    ctrl += ".aria2";
    return fs::exists(ctrl, ec);
}

namespace {

std::uint64_t size_of(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

std::string file_name_of(const std::string& rel) {
    const auto slash = rel.find_last_of("/\\");
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

/// 相对模型目录、`/` 分隔的那一串，界面上照着印。
std::string rel_to(const fs::path& dir, const fs::path& full) {
    std::string s = paths::to_utf8(full.lexically_relative(dir));
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/// 清单里的一个文件在盘上是什么样。
struct OnDisk {
    bool present = false;     ///< 下全了（大小恰好对上、没在下）
    std::uint64_t have = 0;   ///< 下了多少（全了就是全长）
    std::string at;           ///< 在哪 / 要落在哪，相对模型目录
};

/// **在整个模型目录里找，不只看一个位置**：新下的在 `<类型>/` 底下，老版本下的
/// 平铺在根上，人自己拷进来的哪儿都有（见 config/model_index.hpp）。
/// 下载器判"已经有了"走的是同一个 `find_model`——两处判据不一样的话，界面说
/// "齐了"、点下去又开始下，或者反过来。
OnDisk on_disk(const fs::path& dir, const std::string& group_key, const setup::FileSpec& f) {
    OnDisk out;
    out.at = setup::download_rel(group_key, f);
    if (f.bytes == 0) return out;
    if (const auto hit = config::find_model(dir, f.name, f.bytes);
        hit && !download_in_progress(*hit)) {
        out.present = true;
        out.have = f.bytes;
        out.at = rel_to(dir, *hit);
        return out;
    }
    // 下了一半的：新位置、老位置的 `.part`，或者更老的版本留在正式名字上的半截。
    // **预分配的那种算不出来**（文件已经是最终大小，真下了多少只有 .aria2 里的
    // 位图知道），那种记 0——进度偏小，比显示"已完成"好。
    for (const std::string& rel : {out.at, f.name}) {
        const fs::path full = dir / paths::from_utf8(rel);
        fs::path part = full;
        part += ".part";
        for (const fs::path& p : {part, full}) {
            const auto n = size_of(p);
            if (n > 0 && n < f.bytes && n > out.have) out.have = n;
        }
    }
    return out;
}



/// 这个目录（或者它最近的一个存在的上级）所在盘还剩多少。
///
/// **要往上找**：模型目录多半还不存在——它正是这一页要建的那个。
/// 对一个不存在的路径调 fs::space 拿到的是错误码，界面上就成了"剩 0 字节"，
/// 而那会让人以为盘满了。
std::pair<std::uint64_t, std::uint64_t> disk_space(fs::path dir) {
    std::error_code ec;
    while (!dir.empty()) {
        if (fs::exists(dir, ec)) {
            const auto s = fs::space(dir, ec);
            if (!ec) {
                return {static_cast<std::uint64_t>(s.available),
                        static_cast<std::uint64_t>(s.capacity)};
            }
        }
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return {0, 0};
}

/// 这一档此刻**按配置**该用哪几份文件。
///
/// 替换档（编码器、VAE）挑的是哪一份，看配置里那个键填的是什么名字——
/// 这样"还没下全 / 已有"算的就是用户真正要用的那几份，而不是默认那几份。
std::map<std::string, std::string> alts_from_config(
    const std::string& group_key, const Option& o, config::ModelsConfig cur) {
    std::map<std::string, std::string> picks;
    for (const auto& alt : o.alts) {
        const std::string* v = config::models_field(cur, alt.role);
        if (v == nullptr || v->empty()) continue;
        for (const auto& c : alt.choices) {
            if (c.name == *v) picks[setup::alt_key(group_key, alt.role)] = *v;
        }
    }
    return picks;
}

// ---- 模型清单上那些话，在这儿换语言 ----
//
// **翻在出口，不翻在表里。** `setup/catalog.cpp` 那张表里的说明（组名、
// 每一档的介绍、每个文件的备注）一律留着中文原话，出到 json 的这几处才
// 过一遍 `SAY()`。三个理由：
//
//   · 那张表是 `catalog()` 里一个**函数局部 static**，第一次调用时建好
//     就不再变。翻在表里的话，语言在第一次调用之前设没设好，决定了整个
//     进程里这张表说哪国话——而"谁先跑"这种事最不该成为翻译对不对的依据。
//   · 表里那些中文原话**同时是翻译表的键**。留着原话，键就只有一处。
//   · 出口一共这么几处，包起来是一眼看得完的；表里是九十多处。
//
// ⚠️ **`o.label` / `o.family` 里有两种东西**：一种是产品名
//（`Qwen-Image · Q8_0`），一种是人话（`不下载 · 之后再说`）。`SAY()` 查不到
// 就原样返回，所以两种一起包没有坏处——而判"这一句要不要翻"的判据是
// **里头有没有汉字**，`test_i18n.cpp` 里那条用例走的就是这个判据。
json option_json(const Option& o, const fs::path& models_dir, double vram_gb,
                 const std::string& group_key,
                 const config::ModelsConfig& cur) {
    json files = json::array();
    std::uint64_t have = 0;
    const std::vector<setup::FileSpec> use =
        setup::effective_files(group_key, o, alts_from_config(group_key, o, cur));
    bool complete = !use.empty();
    std::uint64_t total = 0;
    for (const auto& f : use) {
        // 还在下就一律不算齐，哪怕大小已经对上了（预分配，见 on_disk）。
        const OnDisk d = on_disk(models_dir, group_key, f);
        have += d.have;
        if (!d.present) complete = false;
        total += f.bytes;
        files.push_back({{"name", f.name},
                         {"path", d.at},
                         {"note", SAY(f.note)},
                         {"bytes", f.bytes},
                         {"haveBytes", d.have},
                         {"present", d.present}});
    }
    // 可以换的那几个角色，连同每一份的名字、大小、说明、在不在盘上。
    json alts = json::array();
    for (const auto& alt : o.alts) {
        json choices = json::array();
        for (const auto& c : alt.choices) {
            choices.push_back({{"name", c.name},
                               {"note", SAY(c.note)},
                               {"bytes", c.bytes},
                               {"present", on_disk(models_dir, group_key, c).present}});
        }
        alts.push_back({{"role", alt.role},
                        {"title", SAY(alt.title)},
                        {"choices", choices}});
    }

    return {{"id", o.id},
            {"family", SAY(o.family)},
            {"label", setup::say_label(o.label)},
            {"quant", o.quant},
            {"familyNote", SAY(o.family_note)},
            {"note", SAY(o.note)},
            // **这个数是"权重常驻得下的显存"，不是"跑不起来的下限"。**
            // 界面上必须这么措辞：低于它照样能跑，只是权重放内存、
            // 每一步都在等 PCIe。写成"最低要求"会让 16 GB 的用户
            // 以为自己什么都跑不了。
            {"minVramGb", o.min_vram_gb},
            // fits 只是"推荐不推荐"，**不禁止选**：卡小但内存大的机器
            // 把权重放内存照样跑得动，只是慢。挡住不如把代价说清楚。
            {"fits", o.min_vram_gb <= vram_gb},
            // **按此刻选中的替换档算，不是 o.total_bytes()。** 换成小编码器
            // 之后总量要跟着降，否则界面上那个数和真要下的对不上。
            {"totalBytes", total},
            {"haveBytes", have},
            {"complete", complete},
            {"files", files},
            {"alts", alts}};
}

/// 两个接口地址是不是同一家。只差结尾的斜杠不算两家。
std::string same_service_key(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

}  // namespace

/// 这一组挑的那个 id 认不认得。认不出时回那句给人看的话，认得回空串。
///
/// **认不出不能当成"没挑"。** `[models.pick]` 是手写得到的（项目目录拷来
/// 拷去、或者人直接改 changji.toml），写错一个字母的话，悄悄退回"从文件名
/// 反推"就成了：界面上显示的是另一档、跑的也是另一档，而没有任何一处说过
/// 这件事。这个函数把话交出去，由 get_setup_state 摆到那一组上。
std::string pick_problem(const Group& g, const config::Settings& s) {
    const auto it = s.models.pick.find(g.key);
    if (it == s.models.pick.end() || it->second.empty()) return {};
    if (g.find(it->second) != nullptr) return {};
    // **一句话一个键，不拿 `+` 拼三块。** 理由在 `util/say.hpp` 里
    // `SAYF` 那段：拼出来的碎片翻不了，语序也改不动。
    return SAYF("配置里 [models.pick].%1 写的是「%2」，这一组里没有这一档"
                "——这次按配置里的文件名算",
                g.key, it->second);
}

std::string current_option(const Group& g, const config::Settings& s) {
    // **挑过的优先。** `[models.pick]` 记的是"这部电影要哪一档"，机器无关，
    // 项目里的 changji.toml 能盖全局（用户 2026-09-15 定的：项目优先）。
    // 认不出的 id 不在这儿处理，见 pick_problem——**别悄悄退回去**。
    if (const auto it = s.models.pick.find(g.key); it != s.models.pick.end()) {
        if (!it->second.empty() && g.find(it->second) != nullptr) {
            return it->second;
        }
    }

    // 没挑过（老项目、或者刚装完）就从文件名反推。
    //
    // **远端那条路不按文件认。** 切到云端时我们特意没清 [models].llm
    // （见 catalog.cpp 的 config_patch），所以上次下的那个权重还在配置里；
    // 按文件认的话这一页会选中本地那一档，而实际跑的是云端——界面说的
    // 和真跑的不是一回事，比不显示更糟。
    //
    // ⚠️ **按地址认这一家，不是按模型名。** 2026-09-14 用户报「智谱的
    // 模型名保存不了」，根子就在这儿：原来是拿 `llm.model` 和每一项写死
    // 的那个名字比，而模型名恰恰是用户在设置页自己挑的东西。他把
    // glm-4.7-flash 换成 glm-5.3 那一刻，这一组就"认不出是哪一家"了，
    // 于是
    //   * 这一页显示成「不下载 · 用别的外接服务」——他明明在用智谱；
    //   * 更糟的是下面 post_setup_download 的判据跟着塌：选中项从
    //     `zhipu-free` 变成 `none`，页面上摆着的还是 `zhipu-free`，
    //     一保存就被当成"换了一家"，把那一项写死的 glm-4.7-flash
    //     冲回配置文件。
    // 一家服务 = 一个地址。模型名归用户，这一页不拿它当身份证。
    // **进程内那条按文件认**，和出图出片那三组一样——走下面那段。
    if (g.key == "llm" && s.llm.backend != "local") {
        const auto here = same_service_key(s.llm.base_url);
        for (const auto& o : g.options) {
            for (const auto& [key, value] : o.settings) {
                if (key == "llm.base_url" && value.is_string() &&
                    same_service_key(value.get<std::string>()) == here) {
                    return o.id;
                }
            }
        }
        // 认不出是哪一家（自己填的地址）：那就是"用别的外接服务"那一项。
        return kNoneOption;
    }
    if (g.owned_roles.empty()) return {};
    config::ModelsConfig copy = s.models;
    const std::string* primary = config::models_field(copy, g.owned_roles.front());
    if (primary == nullptr || primary->empty()) return {};
    for (const auto& o : g.options) {
        for (const auto& f : o.files) {
            if (f.role == g.owned_roles.front() && f.name == *primary) return o.id;
        }
    }
    // 配置里写的带了别的目录（`image/…`、人手填的 `D:/models/…`）：按文件名再认
    // 一遍。**先整串、后文件名**——整串对得上的那一档永远优先。
    const std::string mine = file_name_of(*primary);
    for (const auto& o : g.options) {
        for (const auto& f : o.files) {
            if (f.role == g.owned_roles.front() && file_name_of(f.name) == mine) return o.id;
        }
    }
    return {};
}

namespace {

/// 让内存里那份跟上，`to_file` 为真时顺带写回配置文件。
///
/// **两件事要分开。** 设置页顶上那个「写回配置文件」勾（不勾就是"只对
/// 本次进程生效，重启就没了"）原来只管得到那一页的「引擎」「配音」
/// 「装配与闸门」三节——它们走的是 `/api/settings` / `/api/connections`，
/// 那两条一直收 `persist`。而同一页的「模型目录和下载」走的是这儿，
/// 无论勾没勾都往盘上写，回一句「存好了」。勾在那一排的最右边、就在
/// 那颗保存按钮上方，说的却不是同一件事。
void persist(const json& patch, bool to_file = true) {
    if (patch.empty()) return;
    if (to_file) config::save_user_config(patch);
    auto s = config::runtime().snapshot();
    config::apply_setup_patch(s, patch);
    config::runtime().replace(s);
}

}  // namespace


bool group_satisfied(const Group& g, const config::Settings& s) {
    // 编剧和配音有另一条出路：接外面的服务。那时候本机一个文件都没有
    // 也算配齐——不认这一条的话，用云端大模型的人会被永远挡在这一页上。
    //
    // **但云端那条还要有密钥才算配齐。** 2026-09-13 默认改成了远端的
    // glm-4.7-flash，装完就是"backend=remote、api_key 空"这个状态；
    // 只看 backend 的话这一页会说"配好了"放人过去，然后第一次写剧本
    // 401。本机/局域网的服务不要求——Ollama 那些根本不校验。
    // **进程内那条按文件算**（2026-09-19 接回来的）：[models].llm 配了、
    // 文件真在盘上，才算配齐——走下面那段通用的检查。
    if (g.key == "llm" && s.llm.backend != "local") {
        return !s.llm.needs_api_key() || !s.llm.api_key.empty();
    }
    if (g.key == "tts" && s.tts.backend != "local") return true;
    if (g.owned_roles.empty()) return true;

    const fs::path ws = s.workspace_path();
    config::ModelsConfig copy = s.models;
    // 主角色配了、而且文件真的在，才算这一组能用。
    // **只看配没配是不够的**：配了一个不存在的文件名是最常见的情形
    // （手抄配置抄错、模型没下完），而那时候出片会在跑到一半时炸。
    const std::string* primary = config::models_field(copy, g.owned_roles.front());
    if (primary == nullptr || primary->empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(s.models.resolve(*primary, ws), ec);
}


ApiResult get_setup_state(const config::Settings& settings,
                          const models::HardwareProfile& profile) {
    // **探源放在这儿而不是下载那一步。** 探一次要两个 HEAD，最坏 12 秒；
    // 放在下载请求里的话，用户点了「开始下载」之后要愣十几秒才看到动静，
    // 而那时候他不知道程序在干什么。放在这儿是页面打开时顺手探的。
    // 进程内只探一次，见 detect_source()。
    //
    // （这段原来还写着"而且探完的结果要显示出来"——那件事没做成，
    //   见下面 `sourceHow` 那条上的说明：界面一处都没读。）
    const setup::SourceProbe probe = setup::probe_sources();
    // 探测结果单独一个字段，别拼进说明里——拼的话"连不上"后面会跟一个
    // "秒"，成了「连不上 秒」。
    //
    // ⚠️ 这儿原来跟着一句"前端只负责把它排出来"，**而前端一处都没读**
    // （同 `sourceHow`）。下面这个 lambda 格出来的字今天谁也看不见；
    // 留着是因为它不额外花钱，真要显示时形状就是对的。
    const auto probe_label = [&probe](double v) {
        if (probe.how != "probed") return std::string();
        if (v < 0) return SAY("连不上");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return SAYF("%1 秒", buf);
    };
    const std::string probe_ms = probe_label(probe.modelscope_s);
    const std::string probe_hf = probe_label(probe.huggingface_s);

    const fs::path ws = settings.workspace_path();
    const fs::path models_dir = settings.models.dir_path(ws);
    // **按物理显存挑，不按 vram_gb_override。** 那个数是拿来挑画质档位的，
    // 和这张卡实际有多少显存是两回事——2026-09-10 混用它的后果是
    // fp8 图像模型在第 34/62 段 OOM。
    const double vram_gb =
        profile.gpu.has_value() ? profile.gpu->vram_gb() : profile.vram_gb;

    const auto recommended = setup::recommend(vram_gb);

    json groups = json::array();
    json selected = json::object();
    bool needed = false;
    for (const auto& g : catalog()) {
        const bool ok = group_satisfied(g, settings);
        if (g.required && !ok) needed = true;

        json options = json::array();
        for (const auto& o : g.options) {
            options.push_back(
                option_json(o, models_dir, vram_gb, g.key, settings.models));
        }

        // 界面上先选中的那一项：已经配着的优先，没有就用推荐的。
        // **顺序不能反**——反过来的话，用户上次特意挑了小一档的模型，
        // 这一页会把它换回推荐的那档，而他多半不会注意到。
        const std::string current = current_option(g, settings);
        const auto rec = recommended.find(g.key);
        // **还没配、但盘上已经有一整档的，先选它。** 人把模型拷进目录（或者按了
        // 「索引」）之后，这一页该指着他已经有的那一份，而不是推荐另一档让他再下
        // 几十 GB。几档都齐就挑 rank 最高的。
        std::string on_hand;
        if (current.empty()) {
            int best = -1;
            for (std::size_t i = 0; i < g.options.size(); ++i) {
                const Option& o = g.options[i];
                if (o.files.empty() || !options[i].value("complete", false)) continue;
                if (o.rank > best) {
                    best = o.rank;
                    on_hand = o.id;
                }
            }
        }
        const std::string pick =
            !current.empty()
                ? current
                : !on_hand.empty()
                      ? on_hand
                      : (rec == recommended.end() ? std::string() : rec->second);
        selected[g.key] = pick;

        // **替换档（编码器、VAE）也要回填。**
        //
        // 这张表是界面上那几个下拉的初值（models.js 的 `picks`）。原来这儿
        // 只放组一级那一个 id，于是弹窗一打开，编码器和 VAE 两个下拉读不到
        // 自己那个键，回落到 `alt.choices[0]` —— 也就是默认那档。
        //
        // 落地就是用户 2026-09-18 报的那件事：出片模型里把文本编码器换成
        // Q2_K_M、视频 VAE 换成 int8_convrot，存下，再打开**两个都弹回默认**。
        // 更难查的是当时那一屏是**自相矛盾**的：精度那一列的体积按存下的
        // 小编码器算（45.5 GB，走的是 option_json → alts_from_config），
        // 两个下拉却显示着大的那份，底下「下载」按钮又按大的算成 53.0 GB。
        // 同一屏三个数三种说法，而没有一处报错。
        //
        // 真正伤人的是接着那一下：这时候再点「保存」，发出去的是下拉里那份
        // **默认值**——用户特意挑的小编码器就这么被自己的一次保存覆盖掉了。
        //
        // 判据和算体积那条共用 `alts_from_config`（它按配置里填的文件名反查
        // 是哪一档）。两处各写一份的话，迟早又回到"体积按这份算、下拉显示
        // 那份"。
        if (const Option* o = g.find(pick); o != nullptr) {
            for (const auto& [k, v] : alts_from_config(g.key, *o, settings.models)) {
                selected[k] = v;
            }
        }

        json grp{{"key", g.key},
                 {"title", SAY(g.title)},
                 {"purpose", SAY(g.purpose)},
                 {"required", g.required},
                 {"satisfied", ok},
                 {"options", options}};
        // 挑的那个 id 认不出来时**要说话**，别悄悄退回按文件名反推——
        // 那样界面上显示的和跑的是另一档，而没有一处提过这件事。
        if (const auto why = pick_problem(g, settings); !why.empty()) {
            grp["pickProblem"] = why;
        }
        groups.push_back(std::move(grp));
    }

    const auto [free_bytes, total_bytes] = disk_space(models_dir);

    json gpu = nullptr;
    if (profile.gpu.has_value()) {
        gpu = {{"name", profile.gpu->name},
               {"vramGb", profile.gpu->vram_gb()},
               {"count", profile.gpu->count}};
        // **统一内存的机器上，整机多大也要告诉界面。**
        // 这一页上所有的门槛都是拿 vramGb 比的（"≥ 81 GB 可常驻"），而在
        // 苹果芯片上那是 Metal 肯给的那一份（128 GB 的机器上 107.5 GB），
        // 不是整机内存。只报前者，用户看到的是"我买的明明是 128"——
        // 而这一页正是他决定要不要下 91 GB 那一档的地方。
        // 不是统一内存时这一项是 null，界面就只显示一个数。
        // 措辞和体检那边对齐（doctor.cpp 那句「整机 X GB，其余留给系统」）：
        // 同一件事在两处说成两样，用户会以为它们讲的是两个数。
        gpu["unifiedGb"] = profile.gpu->unified()
                               ? json(static_cast<double>(profile.gpu->unified_mb) / 1024.0)
                               : json(nullptr);
    }

    return {200,
            {{"needed", needed},
             {"detected", profile.detected},
             {"gpu", gpu},
             {"vramGb", vram_gb},
             {"modelsDir", paths::to_utf8(models_dir)},
             {"configFile", paths::to_utf8(config::user_config_path())},
             {"diskFreeBytes", free_bytes},
             {"diskTotalBytes", total_bytes},
             // 一个下载器都没有的话，界面要在按下载之前就说出来，
             // 而不是让人点完等半天才看到一句"起不来 aria2c"。
             {"tool", setup::pick_tool()},
             // **从哪儿下。国内魔搭、国外 HuggingFace。**
             // 探出来的那个只是默认值，界面上可以改——探测本身会错：
             // 挂了代理的国内机器 HuggingFace 可能更快，而反过来，
             // 公司网络里魔搭也可能被挡。所以探完把依据也一起报出去。
             //
             // ⚠️ **依据那两项今天没人看。** 下面 `sourceHow`（env / default /
             // probed，说明这个默认值是环境变量定的、写死的、还是真探出来的）
             // 和每个源里那个 `probe`（`probe_label` 已经格成 "0.42 秒" /
             // "连不上" 这种现成能印的字），**前端一处都没读**：设置页
             // 「下载源」那个下拉框只印 `label` + `note`（「魔搭 ModelScope ·
             // 国内」），选中的是哪一个看得见，*为什么*是它看不见。
             //
             // 也就是说上面那句"让用户看得见为什么是这个源"，引擎这一侧做完了，
             // 界面那一侧没接。两项都留着——算它们不额外花钱（probe 本来就要
             // 跑，how 是顺手带的一个字符串），而且都已经是能直接印的形状，
             // 真要显示时不用再动这儿。
             {"source", setup::to_string(probe.source)},
             {"sourceHow", probe.how},
             {"sources",
              json::array({json{{"id", "modelscope"},
                                {"label", SAY("魔搭 ModelScope")},
                                {"note", SAY("国内")},
                                {"probe", probe_ms}},
                           json{{"id", "huggingface"},
                                {"label", "HuggingFace"},
                                {"note", SAY("国外")},
                                {"probe", probe_hf}},
                           json{{"id", "hf-mirror"},
                                {"label", SAY("HuggingFace 镜像")},
                                {"note", SAY("魔搭上万一缺某个文件时的退路，不参与自动挑选")},
                                {"probe", ""}}})},
             {"recommended", recommended},
             {"selected", selected},
             {"groups", groups},
             {"download", setup::Downloader::instance().snapshot().to_json()}}};
}

ApiResult post_setup_download(const config::Settings& settings, const json& body) {
    // **只保存配置，不下文件。** 用户 2026-09-14："模型没下载也应该可以
    // 保存，提示用户是否现在下载。"
    //
    // 这两件事本来就该分开：「这一档是我要的」是个决定，「文件到盘上了」
    // 是件体力活。捆在一起的后果是——挑一档 40 GB 的权重，就得先等它下完
    // 才算选上，中途一停配置还回到原样。
    //
    // 默认仍是 true，老的调用方一个字都不用改。
    const bool want_download = [&] {
        const auto it = body.find("download");
        return it == body.end() || !it->is_boolean() || it->get<bool>();
    }();

    // 要不要写回配置文件。设置页顶上那个勾就是它，默认真——项目页那个
    // 模型窗口和别的调用方一个字都不用改。见上面 persist() 那段。
    const bool want_persist = [&] {
        const auto it = body.find("persist");
        return it == body.end() || !it->is_boolean() || it->get<bool>();
    }();
    // **真要下文件就不能"只对本次进程生效"。** 下完那一下 `on_item_done`
    // 会把这一组写回配置文件（它必须写：配置指着老模型而文件是新的，
    // sd.cpp 不报错，只出一段花屏）。收下这个要求再反悔，比当场说清楚糟。
    if (!want_persist && want_download) {
        throw ApiError(400,
                       SAY("要下模型就得写回配置文件：文件下完之后配置得指"
                           "过去，不然出图会拿着老模型跑。先勾上「写回配置"
                           "文件」。"));
    }

    if (setup::Downloader::instance().running()) {
        // 409 而不是静默忽略：用户点了第二次而界面什么都没变的话，
        // 他会以为第一次没点上。
        // **只保存也拦**：下载器正拿着上一套选择在跑，这会儿把配置改成
        // 另一套，下完那一下 on_item_done 又会写回去，两边打架。
        // 按钮上写的是「停下」不是「停止」（ModelDialog 里那颗
        // `stopDownload`）。这句话出现的时候人正卡着，而那颗按钮就在同一个
        // 窗里、进度条旁边——名字差一个字，他会以为要找的是别的东西。
        throw ApiError(409,
                       SAY("已经在下了。要换选择先点进度条旁边那颗"
                           "「停下」。"));
    }
    // **这一条只在真要下的时候问。** 机器上没装 aria2/curl 跟"我想把配置
    // 存下来"毫无关系，而拦在这儿的话，没装下载器的机器连模型都选不了。
    if (want_download && setup::pick_tool().empty()) {
        throw ApiError(400,
                       SAY("这台机器上没找到下载器（aria2c 或 curl）。"
                           "装一个再回来：Debian/Ubuntu 是 apt-get install "
                           "-y aria2，Windows 是 winget install aria2.aria2。"));
    }

    // 从哪儿下。前端不传就用探出来的那个。
    setup::Source source = setup::detect_source();
    if (const auto raw = body.find("source");
        raw != body.end() && raw->is_string() && !raw->get<std::string>().empty()) {
        const auto picked = setup::source_from_string(raw->get<std::string>());
        // **认不出就报错，不静默退回默认值**：用户选了个源而程序偷偷换掉，
        // 他会以为自己选的生效了，然后对着"怎么还是这么慢"发愣。
        if (!picked.has_value()) {
            throw ApiError(400, SAYF("不认识的下载源：%1",
                                     raw->get<std::string>()));
        }
        if (*picked != setup::Source::Auto) source = *picked;
    }

    const auto it = body.find("selections");
    if (it == body.end() || !it->is_object()) {
        throw ApiError(400, SAY("缺 selections"));
    }
    std::map<std::string, std::string> selections;
    for (const auto& [key, value] : it->items()) {
        if (value.is_string()) selections[key] = value.get<std::string>();
    }
    // **一组都没选，但只是存设置，那是合法的。**
    //
    // 2026-09-14 起「模型目录」和「下载源」归设置页管（用户：「模型的路径
    // 放到设置里，这个全局统一的」），而它们和"挑哪一档模型"是两件事——
    // 改目录时一组都不用选。真要下却一组没选才是错：那时候没有任何文件
    // 可下，静默返回的话用户会对着一个不动的进度条等。
    if (selections.empty() && want_download) {
        throw ApiError(400, SAY("一组都没选"));
    }

    // 模型放哪。用户填了就以填的为准，**而且要先写进配置再开下**——
    // 否则下完之后配置里的相对路径仍然相对老目录解析，文件在盘上却"找不到"。
    fs::path models_dir = settings.models.dir_path(settings.workspace_path());
    json immediate = json::object();
    if (const auto dir = body.find("dir");
        dir != body.end() && dir->is_string() && !dir->get<std::string>().empty()) {
        const auto raw = dir->get<std::string>();
        models_dir = fs::absolute(paths::expand_user(raw));
        immediate["models"]["dir"] = paths::to_utf8(models_dir);
    }

    // **有没有哪一组真的换了。** 下面用它决定"写哪些组"，理由见那段注释。
    bool any_changed = false;
    for (const auto& g : catalog()) {
        const auto pick = selections.find(g.key);
        if (pick == selections.end()) continue;
        if (current_option(g, settings) != pick->second) {
            any_changed = true;
            break;
        }
    }

    std::vector<Item> items;
    for (const auto& g : catalog()) {
        const auto pick = selections.find(g.key);
        if (pick == selections.end()) continue;
        const Option* opt = g.find(pick->second);
        if (opt == nullptr) {
            throw ApiError(400, SAYF("不认识的选项：%1", pick->second));
        }
        const bool changed = current_option(g, settings) != pick->second;

        // **每一组选中的配置都立刻写，不管它要不要下文件。**
        //
        // 原来只有 `files.empty()` 那几组（云端 API、"不下载"）在这儿写，
        // 要下文件的那些等 on_item_done 在整组下完之后才写。那样一来
        // "选中"和"下完"是同一件事，中途一停就什么都没留下。
        //
        // ⚠️ 下面那个 on_item_done 的老注释说「一组的文件全齐了才写」，
        // 防的是**半组**：video 指着新模型、video_vae 还是上一档，两个
        // 文件都在盘上却不配套，sd.cpp 不报错只出一段花屏。**那个顾虑
        // 和这里不冲突**——这里写的是整组，一个文件都还没下。配置指着
        // 盘上没有的文件是个**说得出口**的状态：体检里那条"配了 N 项、
        // 其中 M 项缺"会直接点出来（doctor.cpp 的 configured/missing）。
        // 静默的花屏和明说的缺文件，不是一码事。
        //
        // ⚠️ **没动过的组不要重写。** 2026-09-14 用户报「智谱的模型名又
        // 保存不了了」——真相是：他在模型那个窗口里把 llm.model 改成
        // glm-5.3 存好了，接着点了这一页的保存，而 llm 这一组的选中项
        // 仍然是 `zhipu-free`，那一项的 settings 里**写死着
        // `llm.model = glm-4.7-flash`**，于是把他刚存的模型名冲回默认。
        // 运行时内存里还是新值，所以当场看不出来，**下次读配置文件才跳
        // 回去**——最难查的那种。
        //
        // 判据是"这一组的选中项变了没有"，不是"这一页点没点保存"：
        // 他压根没碰这一组，我们就不该动它写下的任何一项。
        // （这条判据本身还要 current_option 认得出这一家才算数——
        //  那正是它按地址认、不按模型名认的原因，见上面那段。）
        //
        // **一组都没变时反而全写**：那正是按钮显示「重写配置」的那一下，
        // 用途就是配置手改坏了拿它修回来。少了这一支，那个功能会变成
        // 一个什么都不做的按钮。
        if (!opt->settings.empty() && (changed || !any_changed)) {
            // **整份 selections 传进去，不是只传这一组的 id。** 替换档
            // （编码器、VAE）挑的是哪一份就在里面，只传 id 的话写回配置的
            // 永远是默认那份——下的是小编码器、配置里写的是大编码器。
            json patch = setup::config_patch(selections);
            // **已经在这家了就别动模型名。** 上面那条「一组都没变时全写」
            // 是留给「重写配置」的，但它会连 `llm.model` 一起重写成这一项
            // 写死的那个默认值——而模型名恰恰是用户在那个窗口里自己
            // 挑的，不是这一页管的东西。判据是地址没变：地址一样就说明他
            // 还在这家，没换服务商，那模型名归他。
            if (!changed && patch.contains("llm") && patch["llm"].is_object() &&
                patch["llm"].contains("base_url") &&
                patch["llm"]["base_url"].is_string() &&
                same_service_key(patch["llm"]["base_url"].get<std::string>()) ==
                    same_service_key(settings.llm.base_url)) {
                patch["llm"].erase("model");
            }
            for (const auto& [section, values] : patch.items()) {
                if (!values.is_object()) {
                    immediate[section] = values;  // 不在任何小节里的顶层项
                    continue;
                }
                for (const auto& [k, v] : values.items()) immediate[section][k] = v;
            }
        }
        // 判据是"有没有文件"，不是"是不是 kNoneOption"：走云端 API
        // 那一项（zhipu-free）也一个文件都不下，但它带着三个必须写的旋钮。
        if (opt->files.empty()) continue;
        // **和写配置走同一份解析。** 只改一处的话会出现"下的是小编码器、
        // 配置里写的是大编码器"。
        for (const auto& f : setup::effective_files(g.key, *opt, selections)) {
            items.push_back({g.key, opt->id, f, setup::download_rel(g.key, f),
                             setup::resolve_url(source, f.repo, f.path)});
        }
    }

    try {
        persist(immediate, want_persist);
    } catch (const std::exception& e) {
        throw ApiError(500, SAYF("配置写不进去：%1", e.what()));
    }

    // ---- 「这部电影要哪一档」写进项目 ----
    //
    // 上面那一趟写的是**文件名**（`[models].image = "…gguf"`），那是这台
    // 机器的属性。而"要哪一档"是电影的属性、机器无关，写进项目里的
    // changji.toml，跟着项目目录走——派到别的机器上时那台照这个 id 去自己
    // 的模型目录里找（用户 2026-09-15：模型配置跟项目走，项目优先）。
    //
    // **不给 project 就只写全局**，和以前一模一样：设置页那一节、以及给
    // 对等机装模型那条路都不属于任何一部电影。
    std::string wrote_pick;
    if (const auto pj = body.find("project");
        pj != body.end() && pj->is_string() && !pj->get<std::string>().empty()) {
        json pick = json::object();
        for (const auto& [group, id] : selections) pick[group] = id;
        if (!pick.empty()) {
            const fs::path toml =
                paths::expand_user(pj->get<std::string>()) / "changji.toml";
            try {
                // 节名直接写 `models.pick`——写回那一层是按整串认节头的，
                // 于是文件里出现的就是 `[models.pick]`。
                wrote_pick = paths::to_utf8(
                    config::save_user_config(json{{"models.pick", pick}}, toml));
            } catch (const std::exception& e) {
                // **只是这一半没写成，别把整趟算失败**：文件已经下了（或者
                // 正要下），全局那份路径也写进去了。说清楚哪一半没成，
                // 比整个回 500 让人重来一遍强。
                throw ApiError(
                    500,
                    SAYF("这一档记不进项目里：%1。文件和本机配置都已经写好了，"
                         "下次打开这部电影会退回按文件名认",
                         e.what()));
            }
        }
    }

    if (!want_download || items.empty()) {
        // 两种情况回同一个形状，因为对调用方来说是同一件事：**没起下载**。
        //   * `download:false` —— 只保存。配置上面那次 persist 已经写进去
        //     了，要下什么由前端问过用户再说。
        //   * items 为空 —— 全选了"不下载"，或者选的都已经在盘上。
        // 两者都不算错。
        json out{{"started", false},
                 {"progress", setup::Downloader::instance().snapshot().to_json()}};
        if (!wrote_pick.empty()) out["pickWrittenTo"] = wrote_pick;
        return {200, std::move(out)};
    }

    // 每下完一个文件回来一次。**一组的文件全齐了才写那一组的配置**：
    // 只写一半的话，配置里 video 指着新模型、video_vae 还是上一档的，
    // 而这种组合 sd.cpp 不报错，只是出一段花屏。
    const auto on_item_done = [](const Item& done) {
        const auto snap = setup::Downloader::instance().snapshot();
        for (const auto& p : snap.items) {
            if (p.group != done.group || p.option != done.option) continue;
            if (p.state != setup::ItemState::Done && p.state != setup::ItemState::Present) {
                return;  // 这一组还没齐
            }
        }
        // **替换档要从下完的这几个文件反推。** 这条回调只拿得到 group 和
        // option，而编码器、VAE 挑了哪一份只有文件名知道；不反推的话，
        // 下的是小编码器、写进配置的是大编码器，加载时报"权重读不对"。
        std::map<std::string, std::string> sel{{done.group, done.option}};
        for (const auto& g : setup::catalog()) {
            if (g.key != done.group) continue;
            const setup::Option* o = g.find(done.option);
            if (o == nullptr) break;
            for (const auto& alt : o->alts) {
                for (const auto& p : snap.items) {
                    if (p.group != done.group || p.option != done.option) continue;
                    for (const auto& c : alt.choices) {
                        if (c.name == p.name) {
                            sel[setup::alt_key(g.key, alt.role)] = c.name;
                        }
                    }
                }
            }
            break;
        }
        try {
            persist(setup::config_patch(sel));
        } catch (const std::exception&) {
            // 写不进去不该把下载也停掉——文件是有用的，
            // 配置用户还能自己填。真正的报错留给下一次 state 请求：
            // 那时候这一组会显示成"文件都在，但配置没指过去"。
        }
    };

    setup::Downloader::instance().start(std::move(items), models_dir,
                                       setup::to_string(source), on_item_done);
    return {200, {{"started", true},
                  {"progress", setup::Downloader::instance().snapshot().to_json()}}};
}

ApiResult get_setup_progress() {
    return {200, setup::Downloader::instance().snapshot().to_json()};
}

ApiResult post_setup_cancel() {
    setup::Downloader::instance().cancel();
    return {200, setup::Downloader::instance().snapshot().to_json()};
}

ApiResult get_setup_index(const config::Settings& settings) {
    const fs::path dir = settings.models.dir_path(settings.workspace_path());
    const config::ModelIndex idx = config::index_models(dir, /*fresh=*/true);

    // 清单里每一份，按文件名记（替换档也算：小一档的编码器也是认得的）。
    struct Known {
        const Group* group;
        const setup::FileSpec* file;
    };
    std::multimap<std::string, Known> by_name;
    for (const auto& g : catalog()) {
        for (const auto& o : g.options) {
            for (const auto& f : o.files) by_name.emplace(file_name_of(f.name), Known{&g, &f});
            for (const auto& alt : o.alts) {
                for (const auto& c : alt.choices) {
                    by_name.emplace(file_name_of(c.name), Known{&g, &c});
                }
            }
        }
    }

    json files = json::array();
    std::size_t known = 0;
    std::uint64_t bytes = 0;
    for (const auto& f : idx.files) {
        bytes += f.bytes;
        json one{{"path", f.rel}, {"bytes", f.bytes}, {"status", "unknown"}};
        const auto [lo, hi] = by_name.equal_range(file_name_of(f.rel));
        const Known* hit = nullptr;
        const Known* near = nullptr;
        for (auto it = lo; it != hi; ++it) {
            if (it->second.file->bytes == f.bytes) { hit = &it->second; break; }
            if (near == nullptr) near = &it->second;
        }
        if (hit != nullptr) {
            ++known;
            one["status"] = "known";
            one["group"] = hit->group->key;
            one["groupTitle"] = SAY(hit->group->title);
            one["note"] = SAY(hit->file->note);
        } else if (near != nullptr) {
            // 名字对得上、大小不对：多半是没下完，或者同名的另一个版本。
            // **不算认得**——拿它去出片，加载时报的是"权重读不对"。
            one["status"] = "size";
            one["group"] = near->group->key;
            one["groupTitle"] = SAY(near->group->title);
            one["expectBytes"] = near->file->bytes;
        }
        files.push_back(std::move(one));
    }

    std::error_code ec;
    return {200,
            {{"dir", paths::to_utf8(dir)},
             {"exists", fs::is_directory(dir, ec)},
             {"truncated", idx.truncated},
             {"count", idx.files.size()},
             {"known", known},
             {"bytes", bytes},
             {"files", files}}};
}

}  // namespace changji::http
