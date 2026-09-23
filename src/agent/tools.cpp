#include "agent/tools.hpp"

#include "models/shot.hpp"
#include "util/chapter_word.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "http/batch.hpp"
#include "http/editing.hpp"
#include "http/episodes.hpp"
#include "http/film.hpp"
#include "http/projects.hpp"
#include "http/readonly.hpp"
#include "http/upload.hpp"
#include "http/ref_gen.hpp"
#include "http/run.hpp"
#include "http/story_api.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/task_board.hpp"
#include "util/cancel_words.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace changji::agent {

namespace {

/// 一个工具的定义，照 OpenAI 那套 function 格式包一层。
ordered fn(const std::string& name, const std::string& desc, ordered props,
           std::vector<std::string> required = {}) {
    ordered params;
    params["type"] = "object";
    params["properties"] = std::move(props);
    if (!required.empty()) params["required"] = required;

    ordered f;
    f["name"] = name;
    f["description"] = desc;
    f["parameters"] = std::move(params);

    ordered t;
    t["type"] = "function";
    t["function"] = std::move(f);
    return t;
}

ordered str_arg(const std::string& desc) {
    ordered p;
    p["type"] = "string";
    p["description"] = desc;
    return p;
}

/// 工具参数解析。**坏参数不抛**，回一个空对象，各工具自己按缺省处理。
json parse_args(const std::string& raw) {
    try {
        json j = json::parse(raw.empty() ? "{}" : raw);
        return j.is_object() ? j : json::object();
    } catch (const std::exception&) {
        return json::object();
    }
}

std::string arg_str(const json& a, const char* key) {
    if (!a.contains(key)) return {};
    const auto& v = a.at(key);
    return v.is_string() ? v.get<std::string>() : std::string();
}

/// 这一章到哪一步了，一句话。
///
/// **判据全写成正面条件**（「有没有一镜能用」），不是「数组非空」——
/// CLAUDE.md 第四条那三处坑全是后者。回包里 `shots` 这个数本来就已经是
/// 「能用的镜头数」了（readonly.cpp 里那段注释），这儿照用。
std::string episode_line(const json& e) {
    const std::string id = e.value("episode_id", "");
    const std::string title = e.value("title", "");
    const int shots = e.value("shots", 0);
    const int chars = e.value("script_chars", 0);

    int filmed = 0;
    int total = 0;
    if (e.contains("status") && e.at("status").is_object()) {
        for (const auto& [k, v] : e.at("status").items()) {
            const int n = v.is_number_integer() ? v.get<int>() : 0;
            total += n;
            // **判据问 models，别在这儿列名字。** 这儿原来自己列了
            // `final` / `final_ok` / `done`——三个都不存在，于是这个数
            // 永远是 0（见 `models::status_has_film` 上那段）。
            models::ShotStatus st{};
            from_json(json(k), st);
            // `from_json` 认不出时回默认值（PLANNED），正好不算有片；
            // 但**不能只靠它**——真拼错了要看得见，所以再比一次名字。
            if (to_string(st) == k && models::status_has_film(st)) filmed += n;
        }
    }

    std::string s = id;
    if (!title.empty()) s += " 《" + title + "》";
    s += "：";
    if (chars <= 0) {
        s += "还没有剧本";
    } else {
        s += "剧本 " + std::to_string(chars) + " 字";
    }
    if (shots > 0) {
        s += "，" + std::to_string(shots) + " 镜";
        if (total > 0) s += "，出片 " + std::to_string(filmed) + "/" + std::to_string(total);
    } else {
        s += "，还没拆分镜";
    }
    return s;
}

}  // namespace

std::string describe_project(const json& p, const json& story) {
    if (p.is_null() || !p.is_object() || p.empty()) return "还没有项目。";

    std::string out;
    const std::string title = p.value("title", "");
    if (!title.empty()) out += "片名：" + title + "\n";
    const std::string premise = p.value("premise", "");
    if (!premise.empty()) out += "讲的是：" + premise + "\n";

    // 故事那一层：有几章、写了几章正文。
    //
    // ⚠️ `/api/story` 的回包**包了一层**：顶层的 `chapters` 是数目不是数组
    // （见 http/rail.cpp 里 any_chapter_written 上那段）。这两个数是引擎自己
    // 数的，别在这儿再数一遍。
    const int n_ch = story.is_object() ? story.value("chapters", 0) : 0;
    const int n_written = story.is_object() ? story.value("written", 0) : 0;
    if (n_ch > 0) {
        out += "故事：" + std::to_string(n_ch) + " 章，其中 " +
               std::to_string(n_written) + " 章写了正文\n";
    } else {
        out += "故事：还没有\n";
    }

    // 设定。
    const std::size_t chars = p.contains("characters") && p.at("characters").is_array()
                                  ? p.at("characters").size() : 0;
    const std::size_t locs = p.contains("locations") && p.at("locations").is_array()
                                 ? p.at("locations").size() : 0;
    out += "设定：" + std::to_string(chars) + " 个角色、" + std::to_string(locs) + " 个场景\n";

    // 逐章。**只列前十条**：二十章全列进去，这段摘要就比人说的话还长，
    // 而它每一轮都重发一次。
    if (p.contains("episodes") && p.at("episodes").is_array()) {
        const auto& eps = p.at("episodes");
        out += "各章：\n";
        std::size_t shown = 0;
        for (const auto& e : eps) {
            if (shown >= 10) {
                out += "  …（还有 " + std::to_string(eps.size() - shown) + " 章）\n";
                break;
            }
            out += "  " + episode_line(e) + "\n";
            shown++;
        }
        if (eps.empty()) out += "  （一章都还没有）\n";
    }
    return out;
}

ordered tool_specs() {
    ordered specs = ordered::array();

    specs.push_back(fn("project_state",
                       "这部片子现在什么样：几章、每章到哪一步、有几个角色几个场景。"
                       "**每次要动手之前先看一眼**，别照着上一轮的记忆做判断。",
                       ordered::object()));

    specs.push_back(fn("list_projects", "这台机器上有哪些片子。", ordered::object()));

    {
        ordered props;
        props["name"] = str_arg("片名。人没说就按他要拍的内容起一个短的。");
        props["premise"] = str_arg("这部片子讲什么，一两句话。");
        specs.push_back(fn("create_project",
                           "新建一部片子。**手上还没有项目时先调它**——后面所有的活都要有项目才做得了。",
                           std::move(props), {"name"}));
    }

    {
        ordered props;
        props["chapter_id"] = str_arg("只要某一章的正文就填它（形如 ch01）。不填只回章节表。");
        specs.push_back(fn("story_read",
                           "读故事：大纲、章节表，或者某一章的正文。"
                           "**正文很长，不要整本读**——要看哪一章就填 chapter_id。",
                           std::move(props)));
    }

    {
        ordered props;
        props["episode_id"] = str_arg("哪一章（形如 ep01）。");
        specs.push_back(fn("script_read", "读某一章的剧本（拍子、对白）。",
                           std::move(props), {"episode_id"}));
    }

    {
        ordered props;
        props["episode_id"] = str_arg("哪一章（形如 ep01）。");
        specs.push_back(fn("shots_read",
                           "读某一章的分镜表：每一镜的台词、景别、运镜、时长，和它出到哪一步了。",
                           std::move(props), {"episode_id"}));
    }

    {
        ordered props;
        props["episode_id"] = str_arg("哪一章（形如 ep01）。");
        props["shot_id"] = str_arg("哪一镜（形如 ep01_sh008）。");
        props["duration_s"] = ordered{{"type", "number"},
                                      {"description", "这一镜多长（秒）。嫌赶就加，嫌拖就减。"}};
        props["shot_size"] = str_arg("景别：ECU/CU/MCU/MS/MLS/LS/ELS。");
        props["camera_move"] = str_arg(
            "运镜：static/pan_left/pan_right/tilt_up/tilt_down/push_in/pull_out/handheld/orbit。");
        props["dialogue"] = str_arg(
            "这一镜的台词。这一镜原来只有一句就换掉那一句；原来没有台词或者有好几句，"
            "整镜的台词就换成这一句。");
        props["speaker"] = str_arg(
            "这句台词谁说：填设定里的角色名；旁白填「旁白」。"
            "这一镜原来没台词、或者要换个人说时才要填，不填就沿用原来说话的人。"
            "说话的人得在这一镜的画面里。");
        props["first_frame_prompt"] = str_arg("首帧提示词，整段换掉。");
        specs.push_back(fn("shot_edit",
                           "改一镜：时长、景别、运镜、台词、首帧提示词，改哪样填哪样。"
                           "**改完要重出这一镜才看得见**——接着调 render_run 并且只点这一镜。",
                           std::move(props), {"episode_id", "shot_id"}));
    }

    {
        ordered props;
        props["kind"] = str_arg("character（角色）还是 location（场景）。");
        props["id"] = str_arg("角色或场景的 id（assets_read 里那个）。");
        props["path"] = str_arg("图片在这台机器上的路径。人把图拖进窗口时会带上它。");
        props["slot"] = str_arg(
            "角色专用：front（正面）/ three_quarter（四分之三）/ back（背面）。不填按正面。");
        specs.push_back(fn("assets_set_reference",
                           "把一张图设成某个角色或场景的参考图。"
                           "**人拖了一张图进来、又说了这是谁**的时候用它。"
                           "参考图是跨镜头一致的最硬手段——传了图之后那几镜要重出才看得见。",
                           std::move(props), {"kind", "id", "path"}));
    }

    specs.push_back(fn("assets_read", "读设定：有哪些角色和场景，参考图出了没有。",
                       ordered::object()));

    specs.push_back(fn("outputs_read", "出了哪些片：文件、时长。", ordered::object()));

    specs.push_back(fn("tasks_read",
                       "引擎现在在忙什么：在跑的、排队的、刚做完的和各自用了多久。",
                       ordered::object()));

    // ---- 派活。**回的是「开始…」，不是做完了。** ----
    //
    // 每一句都是「开始 + 干的是什么」，**不带「做完了我再说」那半句**。
    // 那是一个承诺，该由场记自己说；写在工具的回话里，模型会连着那半句一起
    // 复述，于是对话里同一件事连说两遍。而"做完了会来说"这件事本来就由机制
    // 兑现（`watch_and_react` 干完了往对话里追一条），不靠这句话。
    //
    // 这几件活里最短的也要几分钟，长的一个钟头起。工具里等着的话，那个槽被
    // 占死，期间任何别的写作都 409（http/oneclick.hpp 开头那段写的就是这个）。
    // 派出去之后去答人的话，做完了引擎会往这条对话里追一条。

    {
        ordered props;
        props["chapters"] = ordered{{"type", "integer"},
                                    {"minimum", 1},
                                    {"maximum", 40},
                                    {"description", "要几章（1 到 40）。不填按体量推。"}};
        specs.push_back(fn("story_outline",
                           "写整个故事的大纲和章节表。**手上还没有故事时先走它**——"
                           "后面每一步都是照着它展开的。几分钟。",
                           std::move(props)));
    }

    specs.push_back(fn("story_write_chapters",
                       "把还没正文的章一口气全展开成小说正文。一章几分钟，二十章就是一两个钟头。",
                       ordered::object()));

    specs.push_back(fn("assets_understand",
                       "理解故事：把人物、关系、场景提出来，定长相，再逐章写剧本。"
                       "**有了正文就该走它**——后面拆分镜要的东西都在这一步出来。",
                       ordered::object()));

    {
        ordered props;
        props["overwrite"] = ordered{{"type", "boolean"},
                                     {"description", "连已经有图的也重画。默认只画缺的。"}};
        specs.push_back(fn("refs_make",
                           "把缺的参考图画齐：角色三视图、场景空景图。"
                           "**出首帧之前必须有**（出图模型是图像编辑模型时，没有参考图直接跑不动）。",
                           std::move(props)));
    }

    specs.push_back(fn("script_write_all",
                       "把所有挂着章、又还没剧本的章一次改编完。",
                       ordered::object()));

    specs.push_back(fn("storyboard_plan_all",
                       "把还没分镜的章一次全拆成分镜表。**空章拆不了**，要先有剧本。",
                       ordered::object()));

    {
        ordered props;
        props["episode_id"] = str_arg("哪一章（形如 ep01）。");
        props["only_frames"] = ordered{{"type", "boolean"},
                                       {"description", "只出首帧，不出片。先看看画面对不对时用它。"}};
        props["shot_ids"] = ordered{
            {"type", "array"},
            {"items", ordered{{"type", "string"}}},
            {"description",
             "只重出这几镜（形如 [\"ep01_sh008\"]）。**人只是对某一镜不满意时一定要填它**"
             "——不填就是整章重来，一章一个钟头，而他要的只是那一镜。"}};
        props["preview_s"] = ordered{{"type", "integer"},
                                     {"description", "只做前多少秒。想先看一小段时填它。"}};
        specs.push_back(fn("render_run",
                           "出片：配音 → 首帧 → 成片。一章十几分钟到一个钟头。",
                           std::move(props), {"episode_id"}));
    }

    specs.push_back(fn("film_join",
                       "把出了片的章按章序接成**一部完整的电影**，一个文件。没片的章跳过。",
                       ordered::object()));

    // ---- 控制 ----

    specs.push_back(fn("task_cancel",
                       "停下正在跑的活。人说「停」「别跑了」的时候用它。",
                       ordered::object()));

    {
        ordered props;
        props["question"] = str_arg("要问的那句话。短，具体。");
        specs.push_back(fn("ask_user",
                           "停下来问人。**要看画面才能定的事一律走它**——你看不见画面。"
                           "会盖掉人写过的东西、或者要花很久的动作，也先问。",
                           std::move(props), {"question"}));
    }

    return specs;
}

namespace {

/// 拿 ApiResult 的 body；非 2xx 就把那句话翻给模型听。
/// 一件活没成时那句话。`what` 是干的是什么（「读设定」这种）。
///
/// ⚠️ `detail` 那一半**已经是当前界面语言**了：它来自引擎内部那次调用抛的
/// `ApiError`，而那些话是包了 `SAY()` 的、按进程的全局语言查表。也就是说
/// **模型在出错这条路上本来就会收到德语**——那是另一条口子，不是这儿开的，
/// 记在方案里了。这儿管的只是外面这层壳别再和里面那半说两种话。
std::string body_or_error(const http::ApiResult& r, const std::string& what,
                          i18n::Audience to = i18n::Audience::model()) {
    if (r.status >= 200 && r.status < 300) return {};
    std::string detail;
    if (r.body.is_object() && r.body.contains("detail")) {
        const auto& d = r.body.at("detail");
        detail = d.is_string() ? d.get<std::string>() : d.dump();
    }
    return SAYF_TO(to, "%1没成：%2", SAY_TO(to, what),
                   detail.empty() ? std::to_string(r.status) : detail);
}

std::string need_project(const ToolContext& ctx) {
    if (ctx.project.empty()) {
        return "手上还没有项目。先用 create_project 建一部，或者用 list_projects 看看有哪些。";
    }
    return {};
}

/// 派活之前拍一张盘面（见 `ToolContext::baseline`）。**一轮里只拍第一次**：
/// 连派两件时，比的该是"这一轮之前"，不是"第二件之前"。
void mark_baseline(ToolContext& ctx) {
    if (ctx.baseline || ctx.project.empty()) return;
    ctx.baseline = std::make_shared<Baseline>(take_baseline(paths::from_utf8(ctx.project)));
}

/// 「谁说这句」：模型填的名字 → 设定里的 char_id。
///
/// 回 `true` 时 `out` 是 char_id，**空串是旁白**；认不出回 `false`，`why` 是
/// 回给模型的那句话。**认不出不猜**：认错一个人，那句台词就用另一个人的
/// 嗓子念出来，而画面上那个人的嘴在动——比报一句错难查得多。
bool speaker_id(const std::string& project, const std::string& who, std::string& out,
                std::string& why) {
    const std::string w = text::strip_ws(who);
    for (const char* narrator : {"旁白", "narrator", "Narrator", "narration", "voiceover", "VO"}) {
        if (w == narrator) {
            out.clear();
            return true;
        }
    }
    const auto r = http::get_assets(project);
    if (r.status >= 200 && r.status < 300 && r.body.contains("characters") &&
        r.body.at("characters").is_array()) {
        for (const auto& c : r.body.at("characters")) {
            const std::string id = c.value("char_id", std::string());
            if (w == id || w == c.value("name", std::string())) {
                out = id;
                return true;
            }
        }
    }
    why = "认不出这个人：" + w + "。说话的人要填设定里的角色名，旁白就填「旁白」。";
    return false;
}

/// 往这一次调用上挂附件。null（图不在盘上）和空数组都不挂。
void attach(ToolContext& ctx, const json& m) {
    if (m.is_array()) {
        for (const auto& one : m) ctx.media.push_back(one);
    } else if (m.is_object()) {
        ctx.media.push_back(m);
    }
}

}  // namespace

std::string dispatch_label(const std::string& tool) {
    if (tool == "story_outline") return "写大纲";
    if (tool == "story_write_chapters") return "写正文";
    if (tool == "assets_understand") return "读故事、提人物和场景";
    if (tool == "script_write_all") return "写剧本";
    if (tool == "storyboard_plan_all") return "拆镜头";
    if (tool == "refs_make") return "画参考图";
    if (tool == "film_join") return "接成一部";
    // **认不出就回它自己的名字。** 编一个好听的中文才是把人带偏
    //（和枚举那张表同一条规矩）。
    return tool;
}

std::string run_tool(ToolContext& ctx, const std::string& name,
                     const std::string& arguments_json, i18n::Audience to) {
    const json a = parse_args(arguments_json);

    try {
        if (name == "project_state") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const auto pr = http::get_project(ctx.project);
            if (const auto e = body_or_error(pr, "读项目"); !e.empty()) return e;
            json story;
            const auto sr = http::get_story(ctx.project);
            if (sr.status >= 200 && sr.status < 300) story = sr.body;
            return describe_project(pr.body, story);
        }

        if (name == "list_projects") {
            const auto r = http::get_projects(ctx.settings);
            if (const auto e = body_or_error(r, "读项目库"); !e.empty()) return e;
            const auto& items = r.body.is_object() && r.body.contains("projects")
                                    ? r.body.at("projects") : r.body;
            if (!items.is_array() || items.empty()) return "一部片子都还没有。";
            std::string out = "这台机器上的片子：\n";
            for (const auto& it : items) {
                out += "  " + it.value("name", it.value("title", std::string("?")));
                // 一句话比路径有用得多：人问"有哪些片子"时要的是认出哪一部，
                // 而路径每一条都长得一样，前六十个字符还全一致。
                const std::string line = it.value("logline", std::string());
                if (!line.empty()) out += "　" + line;
                out += "\n";
            }
            return out;
        }

        if (name == "create_project") {
            const std::string pname = arg_str(a, "name");
            if (pname.empty()) return "要给这部片子起个名字（name）。";
            // **接口那头叫 `path` 不叫 `name`**（只填名字就落在项目库根目录
            // 下，见 http/projects.cpp 的 resolve_project_path）。发错字段
            // 的表现是一段 422 的结构化校验错，而模型看到它只会原样复述给
            // 人听——2026-09-21 拿假模型跑通那一遍就撞在这儿。
            json body;
            body["path"] = pname;
            body["title"] = pname;
            const auto r = http::post_new_project(body, ctx.settings);
            if (const auto e = body_or_error(r, "建项目"); !e.empty()) return e;
            const std::string root = r.body.value("root", r.body.value("path", std::string()));
            if (root.empty()) return "建好了，但没拿到目录——这不该发生，先别继续。";
            ctx.project = root;
            if (ctx.on_project_created) ctx.on_project_created(root);

            const std::string premise = arg_str(a, "premise");
            if (!premise.empty()) {
                json pb;
                pb["project"] = root;
                pb["premise"] = premise;
                http::post_project_premise(pb);
            }
            // **不报绝对路径。** 模型用不上它（后面的工具都走 ctx.project），
            // 而这一行会原样出现在对话里——一条七八十字的路径占两行，
            // 把"建好了"这句话挤没了。
            return "建好了：" + pname;
        }

        if (name == "story_read") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const auto r = http::get_story(ctx.project);
            if (const auto e = body_or_error(r, SAY_NOOP("读故事"), to); !e.empty()) return e;
            // 回包包了一层，章节表在 `story` 底下。
            const json& st = r.body.contains("story") ? r.body.at("story") : r.body;
            const std::string want = arg_str(a, "chapter_id");

            if (!st.contains("chapters") || !st.at("chapters").is_array() ||
                st.at("chapters").empty()) {
                return SAY_TO(to, "还没有故事。");
            }
            const auto& chs = st.at("chapters");

            if (!want.empty()) {
                for (const auto& c : chs) {
                    if (c.value("chapter_id", std::string()) != want) continue;
                    const std::string text = c.value("text", std::string());
                    if (text.empty()) {
                        return SAYF_TO(to, "%1还没有正文。",
                                       util::chapter_word(want, to));
                    }
                    return SAYF_TO(to, "%1 《%2》\n%3",
                                   util::chapter_word(want, to),
                                   c.value("title", std::string()), text);
                }
                return SAYF_TO(to, "没有%1。", util::chapter_word(want, to));
            }

            std::string out;
            const std::string logline = st.value("logline", "");
            if (!logline.empty()) out += SAYF_TO(to, "一句话：%1\n", logline);
            out += SAYF_TO(to, "共 %1 章：\n", std::to_string(chs.size()));
            for (const auto& c : chs) {
                const std::size_t n = text::utf8_len(c.value("text", std::string()));
                out += SAYF_TO(
                    to, "  %1 《%2》%3　%4\n", c.value("chapter_id", std::string()),
                    c.value("title", std::string()),
                    n > 0 ? SAYF_TO(to, "（正文 %1 字）", std::to_string(n))
                          : SAY_TO(to, "（还没正文）"),
                    c.value("summary", std::string()));
            }
            return out;
        }

        if (name == "script_read") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const std::string ep = arg_str(a, "episode_id");
            if (ep.empty()) return SAY_TO(to, "要说是哪一章（episode_id）。");
            const auto r = http::get_script(ctx.project, ep);
            if (const auto e = body_or_error(r, SAY_NOOP("读剧本"), to); !e.empty()) return e;
            const std::string text = r.body.value("script", "");
            if (text.empty()) {
                return SAYF_TO(to, "%1还没有剧本。", util::chapter_word(ep, to));
            }
            // 头一行的形状和别的几个「读一格」对齐：**哪一章 · 有多少**。
            // 「…的剧本：」把「剧本」又说了一遍——而这段话会原样摆到那一格
            // 的面板上，而面板标题写的就是「剧本」（`/api/peek`）。
            // 字数也不是凑的：一份两百字的剧本和一份两千字的，要做的事不一样。
            return SAYF_TO(to, "%1 · %2 字\n%3", util::chapter_word(ep, to),
                           std::to_string(text::utf8_len(text)), text);
        }

        if (name == "shots_read") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const std::string ep = arg_str(a, "episode_id");
            if (ep.empty()) return SAY_TO(to, "要说是哪一章（episode_id）。");
            const auto r = http::get_shots(ctx.project, ep);
            if (const auto e = body_or_error(r, SAY_NOOP("读分镜"), to); !e.empty()) return e;
            const auto& shots = r.body.contains("shots") ? r.body.at("shots") : r.body;
            if (!shots.is_array() || shots.empty()) {
                return SAYF_TO(to, "%1还没有分镜。", util::chapter_word(ep, to));
            }

            // **降级的那几镜先说，说清为什么。**
            //
            // `fallback` 是"重试超限，留了最后那一版"——片子在、闸门没过。
            // 它和"出好了"在状态里只差一个词，混在十七行里根本看不见；而人
            // 要决定重不重出，靠的就是这几行。
            std::string bad;
            int degraded = 0;
            for (const auto& s : shots) {
                const std::string st = s.value("status", std::string());
                if (st != "fallback" && st != "final_rejected" && st != "draft_rejected") continue;
                degraded++;
                std::string why;
                const auto notes = s.find("gate_notes");
                if (notes != s.end() && notes->is_array() && !notes->empty()) {
                    for (std::size_t i = 0; i < notes->size() && i < 2; ++i) {
                        if (i) why += SAY_TO(to, "；");
                        const auto& n = notes->at(i);
                        why += n.is_string() ? n.get<std::string>() : n.dump();
                    }
                }
                const std::string sid =
                    util::short_shot_id(s.value("shot_id", std::string()));
                bad += why.empty() ? SAYF_TO(to, "  %1（%2）\n", sid, st)
                                   : SAYF_TO(to, "  %1（%2：%3）\n", sid, st, why);
            }

            // **整句一个键**：「共 N 镜」和后面那半原来是 `+` 拼的，拼出来
            // 的是三个半截，翻的人既看不见中间填什么，也改不了语序。
            std::string out =
                degraded > 0
                    ? SAYF_TO(to, "%1共 %2 镜，其中 %3 镜没过闸门：\n",
                              util::chapter_word(ep, to),
                              std::to_string(shots.size()),
                              std::to_string(degraded)) + bad
                    : SAYF_TO(to, "%1共 %2 镜：\n", util::chapter_word(ep, to),
                              std::to_string(shots.size()));
            for (const auto& s : shots) {
                out += "  " + s.value("shot_id", std::string()) + " " +
                       s.value("shot_size", std::string()) + "/" +
                       s.value("camera_move", std::string()) + " " +
                       std::to_string(s.value("duration_s", 0.0)) + "s [" +
                       s.value("status", std::string()) + "]";
                // ⚠️ **`dialogue` 是个数组**（一镜可能好几句），不是字符串。
                // 原来这儿写的是 `s.value("dialogue", std::string())` ——
                // nlohmann 的 `value` 在类型对不上时**抛**
                // （`type_error.302: type must be string, but is array`），
                // 于是这个工具在**任何真项目上都跑不动**：模型问"第 3 章的
                // 分镜什么样"，收回去的是一句 C++ 异常文本。
                // 2026-09-21 拿假模型让代理真调一次才撞出来——读代码读不出来，
                // 而单测也没有（这一条补在 test_agent.cpp 里）。
                std::string d;
                if (const auto it = s.find("dialogue");
                    it != s.end() && it->is_array()) {
                    for (const auto& line : *it) {
                        if (!line.is_object()) continue;
                        const std::string t = line.value("text", std::string());
                        if (t.empty()) continue;
                        if (!d.empty()) d += " ";
                        d += t;
                    }
                }
                if (!d.empty()) out += SAYF_TO(to, " 「%1」", d);
                out += "\n";
            }
            // 读的时候一起摆那几张首帧：人跟着看的是画面，不是那张表。
            attach(ctx, frames_media(paths::from_utf8(ctx.project), ep));
            return out;
        }

        if (name == "assets_read") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const auto r = http::get_assets(ctx.project);
            if (const auto e = body_or_error(r, SAY_NOOP("读设定"), to); !e.empty()) return e;
            std::string out;
            const auto list = [&](const char* key, const char* label) {
                if (!r.body.contains(key) || !r.body.at(key).is_array()) return;
                const auto& arr = r.body.at(key);
                out += SAYF_TO(to, "%1（%2）：", SAY_TO(to, label),
                               std::to_string(arr.size()));
                for (const auto& it : arr) {
                    out += it.value("name", std::string("?"));
                    // **说清有没有图**：这一步人要做的活就是"看图对不对"。
                    //
                    // 这儿原来读的是 `reference`——**一个从来不存在的字段**
                    // （仓库里除了这三行再没有第二处提到它）。于是不管图在
                    // 不在，这句话一律写「(缺图)」，而且一声不响。模型照着
                    // 它去重出已经有的图，人看不出哪儿不对。2026-09-21 做设定
                    // 那一格时撞见。
                    //
                    // 现在读引擎算好的 `has_ref`（`get_assets` 里那一段，
                    // 判的是**文件在不在盘上**）。
                    out += it.value("has_ref", false) ? SAY_TO(to, "(有图) ")
                                                      : SAY_TO(to, "(缺图) ");
                }
                out += "\n";
            };
            // 两个名字在这儿只是**登记**，真翻在上面那行 `SAY_TO(to, label)`
            // ——这儿翻的话，`label` 是 `const char*`，接不上。
            list("characters", SAY_NOOP("角色"));
            list("locations", SAY_NOOP("场景"));
            attach(ctx, references_media(paths::from_utf8(ctx.project)));
            return out.empty() ? SAY_TO(to, "还没有设定。") : out;
        }

        if (name == "assets_set_reference") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const std::string kind = arg_str(a, "kind");
            const std::string id = arg_str(a, "id");
            const std::string path = arg_str(a, "path");
            if (id.empty() || path.empty()) return "要说是谁（id）、图在哪儿（path）。";

            // **图是在引擎这台机器上读的。** 引擎在别的机器上时，桌面端拖进
            // 来的那个路径在对面根本不存在——那时候照实说，别装作传上去了。
            std::ifstream in(paths::from_utf8(path), std::ios::binary);
            if (!in) return "这个文件读不到：" + path + "（引擎在别的机器上时，"
                            "得把图放到那台机器上）";
            const std::string data{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
            if (data.empty()) return "这个文件是空的：" + path;

            // 内容类型按扩展名猜。猜不出的交给接口去拒——那头认得的格式
            // 只有它自己说了算，在这儿再判一遍就是第二份实现。
            //
            // ⚠️ **只在最后一段里找那个点。** 原来直接
            // `path.substr(path.find_last_of('.') + 1)`：路径里一个点都没有
            // 时 `find_last_of` 回 npos，加一等于 0，于是"扩展名"是**整条
            // 路径**。猜不出就按 jpeg 算，所以 `~/.ssh/id_rsa` 这种也能一路
            // 走到上传那头（那头 2026-09-21 起看字节了，会当场说不是图）。
            const std::string base = path.substr(path.find_last_of("/\\") + 1);
            const auto dot = base.find_last_of('.');
            std::string ext = dot == std::string::npos ? std::string() : base.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string ctype = ext == "png"    ? "image/png"
                                      : ext == "webp" ? "image/webp"
                                                      : "image/jpeg";

            const auto r = kind == "location"
                               ? http::post_location_reference(ctx.project, id, ctype, data)
                               : http::post_character_reference(
                                     ctx.project, id,
                                     arg_str(a, "slot").empty() ? "front" : arg_str(a, "slot"),
                                     ctype, data);
            if (const auto e = body_or_error(r, "存参考图"); !e.empty()) return e;
            // 存进去的那一张，当场摆出来：人拖进来的图和"它认成了谁"对不对，
            // 看一眼就知道。
            attach(ctx, reference_media(paths::from_utf8(ctx.project), id,
                                        kind == "location" ? std::string("empty")
                                        : arg_str(a, "slot").empty() ? std::string("front")
                                                                     : arg_str(a, "slot")));
            // ⚠️ **工具的回话里不写 Markdown。** 它会原样摆到对话里当一行
            // 小字（那一行是纯文本，不是 Markdown），`**` 就是两个星号。
            // 底下那条守卫钉着这件事。
            return "存好了：" + id +
                   " 的参考图。那几镜要重出才看得见——传了图等于换了长相。";
        }

        if (name == "outputs_read") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const auto r = http::get_outputs(ctx.project);
            if (const auto e = body_or_error(r, SAY_NOOP("读成片目录"), to); !e.empty()) return e;
            const auto& files = r.body.contains("files") ? r.body.at("files") : r.body;
            if (!files.is_array() || files.empty()) return SAY_TO(to, "还没有出片。");
            attach(ctx, outputs_media(paths::from_utf8(ctx.project)));
            std::string out =
                SAYF_TO(to, "出了 %1 个文件：\n", std::to_string(files.size()));
            for (const auto& f : files) {
                out += "  " + f.value("name", f.value("rel", std::string("?")));
                if (f.contains("duration_s")) {
                    out += SAYF_TO(to, "（%1s）",
                                   std::to_string(f.value("duration_s", 0.0)));
                }
                out += "\n";
            }
            return out;
        }

        if (name == "tasks_read") {
            const json b = pipeline::task_board(ctx.project);
            std::string out;
            const auto section = [&](const char* key, const char* label) {
                if (!b.contains(key) || !b.at(key).is_array()) return;
                const auto& arr = b.at(key);
                if (arr.empty()) return;
                out += std::string(label) + "：\n";
                std::size_t shown = 0;
                for (const auto& t : arr) {
                    if (shown >= 8) {
                        out += "  …（还有 " + std::to_string(arr.size() - shown) + " 件）\n";
                        break;
                    }
                    out += "  " + t.value("title", std::string("?"));
                    // 账本给的是 `seconds`。**"领到了"不等于"正在画"**
                    // （CLAUDE.md 第九条），working 是假的那几行别报进度。
                    if (t.value("working", true)) {
                        out += "（" + std::to_string(static_cast<int>(t.value("seconds", 0.0))) +
                               " 秒）";
                    } else {
                        out += "（等位置）";
                    }
                    out += "\n";
                    shown++;
                }
            };
            section("running", "在跑");
            section("queued", "排队");
            section("done", "刚做完");
            return out.empty() ? "引擎现在闲着。" : out;
        }

        // ---- 派活 ----
        if (name == "story_outline" || name == "story_write_chapters" ||
            name == "assets_understand" || name == "script_write_all" ||
            name == "storyboard_plan_all") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            if (!ctx.client) return "这台机器上现在派不了活（没有大模型客户端）。";
            mark_baseline(ctx);

            json body;
            body["project"] = ctx.project;

            if (name == "story_outline") {
                // **异步那条要 stream 和 async 一起给**（story_api.cpp 里那个
                // 判据是两个都要）。只给一个的话它就地同步跑几分钟，而这一轮
                // 对话会一直挂在那儿。
                body["async"] = true;
                body["stream"] = "agent";
                // 几章。**模型填的什么都可能是**（`3.0`、`"3"`、`0`、`100`）：
                // 是个整数就收，出了范围照实说——原样递过去的话接口回的是一段
                // 422 的结构化校验错，模型只会原样复述给人听。
                if (a.contains("chapters") && !a.at("chapters").is_null()) {
                    const json& c = a.at("chapters");
                    double n = -1;
                    if (c.is_number()) n = c.get<double>();
                    else if (c.is_string()) {
                        try { n = std::stod(c.get<std::string>()); } catch (...) {}
                    }
                    if (n != static_cast<int>(n) || n < 1 || n > 40) {
                        return "章数要是 1 到 40 之间的整数。不确定就别填，按体量推。";
                    }
                    body["chapters"] = static_cast<int>(n);
                }
                pipeline::CancelToken tok;
                const auto r = http::post_story_outline(body, *ctx.client, tok);
                if (const auto e = body_or_error(r, "写大纲"); !e.empty()) return e;
                ctx.dispatched.push_back(name);
                return "开始" + dispatch_label("story_outline") + "。";
            }

            const auto r = name == "story_write_chapters"
                               ? http::post_story_chapters(body, ctx.client)
                           : name == "assets_understand"
                               ? http::post_story_understand(body, ctx.client)
                           : name == "script_write_all"
                               ? http::post_script_all(body, ctx.client)
                               : http::post_plan_all(body, ctx.client);
            if (const auto e = body_or_error(r, "派活"); !e.empty()) return e;
            // **回包里 started=false 不是错**：没有一章需要做，照实说
            // （batch.cpp 里那一支就是这么设计的）。
            if (!r.body.value("started", true)) {
                return "没有需要做的——该做的都做过了。";
            }
            ctx.dispatched.push_back(name);
            return "开始" + dispatch_label(name) + "。";
        }

        if (name == "refs_make") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            mark_baseline(ctx);
            json body;
            body["project"] = ctx.project;
            if (a.value("overwrite", false)) body["overwrite"] = true;
            const auto r = http::post_references_generate_all(body);
            if (const auto e = body_or_error(r, "出参考图"); !e.empty()) return e;
            ctx.dispatched.push_back(name);
            return "开始" + dispatch_label("refs_make") + "。";
        }

        if (name == "shot_edit") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            const std::string ep = arg_str(a, "episode_id");
            const std::string sid = arg_str(a, "shot_id");
            if (ep.empty() || sid.empty()) return "要说是哪一章、哪一镜。";

            json patch = json::object();
            for (const char* k : {"shot_size", "camera_move", "first_frame_prompt"}) {
                if (const std::string v = arg_str(a, k); !v.empty()) patch[k] = v;
            }
            if (a.contains("duration_s") && a.at("duration_s").is_number()) {
                patch["duration_s"] = a.at("duration_s");
            }
            const fs::path root = paths::from_utf8(ctx.project);
            const json before = shot_json(root, ep, sid);

            // 台词那一栏**不是普通字段**（editing.cpp 里单拎出来的那一段）。
            //
            // 原来这儿一律发 `dialogue_texts: [这一句]`——那是"逐条改字"，条数
            // 必须和盘上对上，于是**原来没台词的镜头加不上台词、有两句的也改
            // 不了**，回来一句「台词条数对不上」（2026-09-23 实测）。现在：
            //
            //   · 原来正好一句、没说换人 → 还是逐条改字（留着那一句的情绪、音色）；
            //   · 别的情形 → `dialogue_lines`，整镜换成这一句，说话人按 `speaker`，
            //     没填就沿用原来第一句的；原来一句都没有又没填，照实问。
            const std::string line = arg_str(a, "dialogue");
            const std::string who = arg_str(a, "speaker");
            if (!line.empty()) {
                const json old = before.is_object() && before.contains("dialogue") &&
                                         before.at("dialogue").is_array()
                                     ? before.at("dialogue")
                                     : json::array();
                if (old.size() == 1 && who.empty()) {
                    patch["dialogue_texts"] = json::array({line});
                } else {
                    json speaker = nullptr;   // null = 旁白
                    if (!who.empty()) {
                        std::string id, why;
                        if (!speaker_id(ctx.project, who, id, why)) return why;
                        if (!id.empty()) speaker = id;
                    } else if (!old.empty()) {
                        const json& first = old.at(0);
                        if (first.is_object() && first.contains("char_id") &&
                            first.at("char_id").is_string()) {
                            speaker = first.at("char_id");
                        }
                    } else {
                        return "这一镜原来没有台词，得说清这句是谁说的"
                               "（填角色名；旁白就填「旁白」）。";
                    }
                    patch["dialogue_lines"] =
                        json::array({{{"char_id", speaker}, {"text", line}}});
                }
            }
            if (patch.empty()) return "没说要改什么。";

            json body;
            body["project"] = ctx.project;
            body["episode_id"] = ep;
            body["shot_id"] = sid;
            body["patch"] = patch;
            const auto r = http::post_shot(body);
            if (const auto e = body_or_error(r, "改这一镜"); !e.empty()) return e;
            // 改了哪几栏、从什么改成什么，摆在这条上。
            attach(ctx, shot_change_media(root, ep, sid, before, shot_json(root, ep, sid)));
            // **说清"还要重出才看得见"**：改了画面相关的字段，引擎会把这一镜
            // 的状态退回未开工（editing.hpp 上那句），但盘上那一版还是旧的。
            // **不写工具名和参数名。** 这句话人也在看，`render_run 填上
            // shot_ids` 对他没有意义；而模型那头本来就知道——提示词里那条
            // 「只重做那一处」和 `render_run` 的 `shot_ids` 描述里都写着，
            // 在这儿再说一遍还犯了"同一件事说两处"。
            // 镜号说给人听的那一份：`ep03_sh007` → 「第 3 章 sh007」。
            // 前缀那一段换成「第几章」——光一个 `sh007` 在对话里说不清是哪
            // 一章（镜头墙上不用说，那一格的抬头已经写着）。
            return "改好了：" + util::chapter_word(ep) + " " +
                   util::short_shot_id(sid) + "。要看见得重出这一镜。";
        }

        if (name == "render_run") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            if (!ctx.run_deps) return "这台机器上现在出不了片（后端没装配好）。";
            const std::string ep = arg_str(a, "episode_id");
            if (ep.empty()) return "要说是哪一章（episode_id）。";

            json body;
            body["project"] = ctx.project;
            body["episode_id"] = ep;
            if (a.value("only_frames", false)) body["skip_final"] = true;
            // 只重出这几镜。**不填就是整章**，而整章是一个钟头。
            int only = 0;
            if (a.contains("shot_ids") && a.at("shot_ids").is_array()) {
                json ids = json::array();
                for (const auto& v : a.at("shot_ids")) {
                    if (v.is_string() && !v.get<std::string>().empty()) ids.push_back(v);
                }
                if (!ids.empty()) {
                    only = static_cast<int>(ids.size());
                    body["shot_ids"] = ids;
                }
            }
            if (a.contains("preview_s") && a.at("preview_s").is_number_integer()) {
                body["preview_s"] = a.at("preview_s");
            }
            mark_baseline(ctx);
            const auto r = http::post_run(body, ctx.run_deps());
            if (const auto e = body_or_error(r, "出片"); !e.empty()) return e;
            // **报清楚动了几镜**：人问的是"你要重做多少"，而"开始出片了"
            // 这句话在一镜和十七镜上长得一模一样。
            ctx.dispatched.push_back(name);
            return std::string("开始出片：") + ep + "，" +
                   (only > 0 ? std::to_string(only) + " 镜" : "整章") + "。";
        }

        if (name == "film_join") {
            if (const auto s = need_project(ctx); !s.empty()) return s;
            mark_baseline(ctx);
            json body;
            body["project"] = ctx.project;
            const auto r = http::post_film_join(body);
            if (const auto e = body_or_error(r, "合成"); !e.empty()) return e;
            ctx.dispatched.push_back(name);
            return "开始" + dispatch_label("film_join") + "。";
        }

        // ---- 控制 ----
        if (name == "task_cancel") {
            // **两个槽都要停。** 出片（Run）和写作（Write）是两条独立的活，
            // 人说「停」时说的是"现在跑着的那件"，而他多半不知道那是哪一槽。
            const bool run = pipeline::jobs().cancel(pipeline::JobKind::Run);
            const bool write = pipeline::jobs().cancel(pipeline::JobKind::Write);
            bool refs = false;
            if (!ctx.project.empty()) {
                json body;
                body["project"] = ctx.project;
                try {
                    const auto r = http::post_references_generate_all_stop(body);
                    refs = r.status >= 200 && r.status < 300;
                } catch (const std::exception&) {}
            }
            if (!run && !write && !refs) return "现在没有在跑的活。";
            // 「人按的停」那个词收在 util/cancel_words.hpp 一处，界面靠它认
            // ——换个说法就把"取消"显示成红报错（CLAUDE.md 第八条）。
            return std::string(util::kStopToken1) + "了。";
        }

        if (name == "ask_user") {
            const std::string q = arg_str(a, "question");
            if (q.empty()) return "要问什么（question）？";
            return std::string(kAskUserMark) + q;
        }

        return "没有这个工具：" + name;
    } catch (const http::ApiError& e) {
        // ⚠️ **壳和瓤要说同一种话。** `e.what()` 那一半是包过 `SAY()` 的，
        // 也就是当前界面语言；外面这层壳原来是写死的中文，于是德语下摆出来
        // 的是「跑不动：Diese Asset-Bibliothek wurde nicht geladen…」。
        // 2026-09-22 在设定那一格上实地撞见——这已经是同一种坏法的第三次了
        //（ref_gen.cpp、local_client.cpp 各一次）。
        return SAYF_TO(to, "跑不动：%1", e.what());
    } catch (const std::exception& e) {
        // **不抛。** 抛出去整轮对话就断了，而模型完全可以换个工具再试。
        return SAYF_TO(to, "跑不动：%1", e.what());
    }
}

}  // namespace changji::agent
