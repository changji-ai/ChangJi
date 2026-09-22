#include "http/scripting.hpp"

#include "config/settings.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "http/job_stream.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/activity.hpp"
#include "stages/script.hpp"
#include "stages/script_story.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// 校验请求体里没有多余字段。
///
/// 对应 pydantic 的 model_config = {"extra": "forbid"}。**422 不是 400**——
/// FastAPI 走的是校验错误那条路，body 是结构化数组不是一句话。
/// 对拍语料抓到过我在别处写成 400。
void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体要是一个对象"));
    for (const auto& kv : body.items()) {
        // **`stream` 一律放行。** 它是传输层的信封字段，不是业务字段：
        // 路由那一层（script_route / batch_route）拿它决定这件活挪不挪到
        // 后台、结果往哪条 WebSocket 送，处理函数多半根本不看它。
        //
        // 原来是各家自己往白名单里加，2026-09-14 栽了：给「照故事定妆」
        // 接上思考流之后前端开始发 stream，而 post_bible 的白名单里没有，
        // 一按就是 422 `Extra inputs are not permitted`。十几个处理函数
        // 挨个加，漏一个的表现就是那一步整个不能用。
        //
        // `async` 不在这儿放行是因为它在更上面就被 take_async 摘掉了
        // （见 server.cpp），到这儿本来就没有。
        if (kv.key() == "stream") continue;
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(kv.key(), "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

/// 必填的字符串字段。缺了是 **422 不是 400**。
///
/// 少一个必填字段走的是 pydantic 的校验那条路，和多一个字段
/// （extra="forbid"）是同一类错误，前端也按同一种方式处理。
/// 写成 400 的话前端拿到的 detail 是字符串不是数组，
/// 校验错误的高亮逻辑整个不生效——对拍抓到过。
std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        // **input 是整个请求体，不是 null。** FastAPI 报缺字段时把父对象
        // 放进 input，前端拿它回显"你提交的是这些"。写 null 的话那一栏是空的。
        // 实时对拍抓出来的（写接口那一轮）。
        throw unprocessable_top(key, "Field required", body, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

std::string opt_str(const json& body, const char* key,
                    const std::string& def = "") {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 取一个带范围的数字。越界抛 422，和 pydantic 的 Field(gt=..., le=...) 一致。
double num_in_range(const json& body, const char* key, double def, double gt,
                    double le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", *it,
                                "float_type");
    }
    const double v = it->get<double>();
    // 边界值用 bound_text 印，不用 std::to_string——后者给六位小数，
    // 1800.0 会变成 "1800.000000"，而 pydantic 的消息里是 "1800"。
    // 而且要带 ctx，前端靠它填出"最大 1800"这种中文提示。
    if (v <= gt) {
        throw out_of_range(key, "Input should be greater than " + bound_text(gt),
                           *it, "greater_than", "gt", gt);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + bound_text(le), *it,
            "less_than_equal", "le", le);
    }
    return v;
}

int int_in_range(const json& body, const char* key, int def, int ge, int le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number_integer()) {
        throw unprocessable_top(key, "Input should be a valid integer", *it,
                                "int_type");
    }
    const int v = it->get<int>();
    if (v < ge) {
        throw out_of_range(
            key, "Input should be greater than or equal to " + std::to_string(ge),
            *it, "greater_than_equal", "ge", ge);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + std::to_string(le),
            *it, "less_than_equal", "le", le);
    }
    return v;
}

ProjectStore open_project(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        // Python 那边 catch 的是 FileNotFoundError 和 ValueError，都转 400
        throw ApiError(400, e.what());
    }
}

AssetLibrary load_assets_or_400(const ProjectStore& store) {
    try {
        return store.load_assets();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 把大模型的异常翻成 400。
///
/// 三个接口都是 `except ScriptError: raise HTTPException(400, str(exc))`。
/// LlmError 也走这条——它的消息本来就是给用户看的那句话。
template <typename F>
auto llm_guard(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const stages::ScriptError& e) {
        throw ApiError(400, e.what());
    } catch (const llm::LlmError& e) {
        throw ApiError(400, e.what());
    }
}

json draft_common(const stages::ScriptDraft& draft, double duration_s) {
    return {
        {"title", draft.title},
        {"logline", draft.logline},
        {"script", draft.render()},
        {"speakers", draft.speakers()},
        {"dialogue_chars", draft.dialogue_chars()},
        {"budget_chars", stages::budget_chars(duration_s)},
        {"beats", draft.beats.size()},
    };
}

std::vector<std::string> character_names(const AssetLibrary& assets) {
    std::vector<std::string> names;
    for (const auto& kv : assets.characters) names.push_back(kv.second.name);
    return names;
}

}  // namespace

std::string previous_scripts(const models::Project& project,
                             const std::string& before, std::size_t keep) {
    std::vector<std::string> earlier;
    for (const auto& ep : project.episodes) {
        // **到这一章为止。** 把后面几章也塞进去的话，模型会把还没发生的事
        // 当成已经发生的写。
        if (!before.empty() && ep.episode_id == before) break;
        // 预告片是从正片里剪出来的，再拿它当写正片的上下文，模型会开始抄
        // 自己的预告，越写越像宣传语。
        if (ep.episode_id == kTrailerEpisodeId) continue;
        const std::string s = text::strip_ws(ep.script);
        if (s.empty()) continue;
        // 这一段是**贴进提示词**给模型看的，不是界面文案。
        earlier.push_back(SAY_NEVER("【") + ep.episode_id +
                          SAY_NEVER("】\n") + s);
    }
    const std::size_t skip = earlier.size() > keep ? earlier.size() - keep : 0;
    std::string out;
    for (std::size_t i = skip; i < earlier.size(); ++i) {
        if (i > skip) out += "\n\n";
        out += earlier[i];
    }
    return out;
}

ApiResult post_script_premise(const json& body, llm::Client& client,
                              pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "keywords", "count"});
    const std::string keywords = opt_str(body, "keywords");
    if (text::utf8_len(keywords) > 200) {
        throw unprocessable_top("keywords",
                                "String should have at most 200 characters",
                                body.at("keywords"), "string_too_long");
    }
    const int count = int_in_range(body, "count", 3, 3, 5);

    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);

    // 项目上已有的梗概算一个「已经想过的方向」，避免连点两次拿回同一批。
    // 已经写了几章的话，那些也算。
    std::vector<std::string> existing;
    if (!text::strip_ws(project.premise).empty()) existing.push_back(project.premise);
    for (const auto& ep : project.episodes) {
        if (!text::strip_ws(ep.synopsis).empty()) existing.push_back(ep.synopsis);
    }

    const std::string prompt = stages::build_premise_prompt(
        keywords, project.style_line, count, existing);

    llm::Request req;
    req.prompt = prompt;
    req.schema = stages::premise_schema();
    req.schema_name = "premises";
    req.on_thinking = thinking_sink();

    // **顶栏那本账要记上。** 这几个接口是同步的，没有任务表那一套，
    // 2026-09-13 之前它们在界面上整个不可见：用户点了「重新改编」，
    // 顶栏一片安静，而这一刻 LLM 槽是被它占着的——另一头的批量写作会
    // 挂在「显存不够加载 LLM」上，挡路的那件事却查不到。
    // 见 pipeline/activity.hpp 开头那段。
    pipeline::Activity act{"premise", paths::to_utf8(store.root()), "",
                           SAY("正在想梗概")};
    const pipeline::CancelLink stop_here{tok, act};
    const auto ideas = llm_guard([&] {
        return stages::parse_premises(client.complete(req, tok));
    });

    json out = json::array();
    for (const auto& i : ideas) {
        out.push_back({{"title", i.title}, {"premise", i.premise},
                       {"hook", i.hook}});
    }
    return {200, {{"ideas", out}, {"style_line", to_string(project.style_line)}}};
}

ApiResult post_script_write(const json& body, llm::Client& client,
                            pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "episode_id", "premise", "duration_s",
                        "continue_from_previous", "reuse_characters",
                        "variation"});
    const std::string premise = need_str(body, "premise");
    const std::string episode_id = opt_str(body, "episode_id");
    const double duration_s =
        num_in_range(body, "duration_s", 60.0, 0.0, 1800.0);
    const bool continue_prev = opt_bool(body, "continue_from_previous", true);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    const AssetLibrary assets = load_assets_or_400(store);

    // 这一章在章节计划里有对应的一条吗？有就走故事那条：这一章要发生什么
    // 已经定好了，模型只负责把那一段变成拍子。**失忆是在那条路上治好的**——
    // 带的上下文是压缩的全局记忆（大纲、人物、关系、前情提要每章一句），
    // 不是下面那个「最近三章原文截 4000 字符」。
    Story story;
    const EpisodePlan* plan = nullptr;
    EpisodePlan chapter_plan_storage;
    // 没有秒数、没有字数：这一章写多长由内容定。
    //
    // 2026-09-16 之前这儿有个 `chapter_mode` 开关，关着的时候走另一套：
    // 剧本按「这一章总时长约 N 秒」写，不够凑、超了压。那条路当天整个
    // 删了（用户定的），开关跟着没了——下面几处原来都挂在它上面，
    // 现在是无条件的。
    if (!episode_id.empty()) {
        try {
            story = store.load_story();
        } catch (const std::exception&) {
            // 读不了就当没有，退回老路径。老项目本来就没有这个文件。
        }
        // 一条章节记录就是整整一章，计划按章配（整章、钩子取最后一场的
        // turn）。**不按 episode_id 去盘上那份章节计划里查**——那张表的 id 是
        // 按切片发的，一章切两段就有两条，和「ch07 → ep07」对不上，查到的是
        // 隔壁章的半截（2026-09-16 实撞）。
        {
            const Episode* ep = project.episode_by_id(episode_id);
            // **没正文的章不写剧本。**
            //
            // 用户 2026-09-19：「我一个字没写的章节哪来的剧本和分镜」。
            // 挂着章、而那一章的正文是空的时候，下面那条路照样写得出来
            // ——写剧本这一步在没正文时退回用三五句章节梗概
            // （stages/script_story.cpp 里【这一章】那一段），于是三五句
            // 大纲被扩写成三千多字的戏，人再打开那一章，看见的是一份
            // 自己从没写过的剧本。
            //
            // 挡在这儿，不是只挡在批量那三条路上：那三条挑章时也问同一句
            // （http/batch.cpp 的 `script_missing`），但这一条是**这次调用
            // 自己收得到的事实**——挡在源头，才不用指望每个新调用点都记得
            // 先问一句。
            //
            // 只管挂着章的那些。老项目、手动加的章、预告片
            // （`chapter_refs` 空）本来就没有正文可照，照梗概写是它们唯一
            // 的走法，照旧放行。
            if (ep != nullptr && !ep->chapter_refs.empty()) {
                const std::string& cid = ep->chapter_refs.front();
                if (!models::chapter_written(story, *ep)) {
                    const Chapter* c = story.chapter_by_id(cid);
                    const std::string who =
                        c == nullptr || c->title.empty() ? cid : c->title;
                    throw ApiError(400,
                                   SAYF("「%1」还没有正文，写不了剧本——先去"
                                        "故事页把这一章写出来",
                                        who));
                }
                chapter_plan_storage =
                    stages::chapter_plan(story, cid, ep->target_duration_s);
                plan = &chapter_plan_storage;
            }
        }
        if (plan == nullptr) {
            for (const auto& p : story.plan) {
                if (p.episode_id == episode_id) {
                    plan = &p;
                    break;
                }
            }
        }
    }

    const std::string previous = (continue_prev && plan == nullptr)
                                     ? previous_scripts(project, episode_id)
                                     : std::string{};

    const std::vector<std::string> names =
        reuse_chars ? character_names(assets) : std::vector<std::string>{};

    std::vector<stages::ScenePlan> chapter_scenes;

    std::string prompt;
    const char* source = "premise";
    // 这一章四段的形状（占几秒、每段是什么戏）。
    //
    // **两条路都摇。** 原来这儿写死 0，注释是「照梗概续写那条老路子保持
    // 原样」——那条路于是永远是 8%/10%/57% 加同一出戏（钩子 → 推进 →
    // 回报 → 留扣）。保持原样保住的是"每章一个模子"，而那正是用户说的
    // 「剧本时间线也都差不多」。
    //
    // 摇一个就够：提示词、schema、解析三处共用这一个变量，形状和秒数
    // 自然对得上（对不上的表现是段头的秒数和模型看到的不一样，不报错）。
    //
    // **body 里给了就用给的**，和出图那边的 seed 一个规矩（见 ref_gen.hpp）：
    // 界面上的"再摇一次"就是不送这个字段，"还要刚才那个节奏"就是把上次的
    // 数送回来。对拍语料送的是 0，那一档和以前一字不差。
    const std::uint32_t variation =
        body.is_object() && body.contains("variation")
            // 下界写 -1 不是 0：num_in_range 的下界是**开区间**
            // （`v <= gt` 就报 422），而 0 是合法值——它正是"不浮动"那一档。
            ? static_cast<std::uint32_t>(
                  num_in_range(body, "variation", 0.0, -1.0, 4294967295.0))
            : stages::random_shape();
    // **提到 if 外面**：下面按场分次调用时还要用它重拼提示词。
    std::string prev_tail;
    if (plan != nullptr) {
        // 上一章的结尾拿来接语气。**按盘上那份章节计划的顺序取上一条**，不是
        // 按 project.episodes 的顺序——后者可能被手动加过章、插过预告片。
        // 挂着章的那条按章走：找 chapter_refs 指着上一章的那条章节记录。
        if (plan == &chapter_plan_storage) {
            const Chapter* prev_ch = nullptr;
            for (std::size_t i = 1; i < story.chapters.size(); ++i) {
                if (story.chapters[i].chapter_id == plan->from_chapter) {
                    prev_ch = &story.chapters[i - 1];
                    break;
                }
            }
            if (prev_ch != nullptr) {
                for (const Episode& e : project.episodes) {
                    if (std::find(e.chapter_refs.begin(), e.chapter_refs.end(),
                                  prev_ch->chapter_id) != e.chapter_refs.end()) {
                        prev_tail = stages::script_tail(e.script);
                        break;
                    }
                }
            }
        } else {
            for (std::size_t i = 0; i < story.plan.size(); ++i) {
                if (story.plan[i].episode_id != episode_id) continue;
                if (i == 0) break;
                const Episode* prev_ep =
                    project.episode_by_id(story.plan[i - 1].episode_id);
                if (prev_ep != nullptr) {
                    prev_tail = stages::script_tail(prev_ep->script);
                }
                break;
            }
        }
        // 形状每写一次摇一个新的（ComfyUI 的 randomize 那个意思）——**不是
        // 按章号哈希**，那样同一章永远是同一个形状，人不喜欢这一章的节奏也
        // 换不掉。不满意就再点一次「重新改编」，满意了点采用，形状跟着定下来。
        // 走故事这条没有形状：剧本照正文的场走，见 build_chapter_script_prompt。
        chapter_scenes = stages::chapter_scene_plan(story, *plan);
        prompt = stages::build_chapter_script_prompt(
            story, *plan, project.style_line, names, prev_tail, chapter_scenes);
        source = "story";
    } else {
        prompt = stages::build_script_prompt(
            premise, duration_s, project.style_line, previous, names, variation);
    }

    // 走故事那条时时长以章节计划为准：那份表是按每章时长算出来的，
    // 请求里带的那个可能是页面上的旧值，用它算预算会和实际排的镜头对不上。
    const double used_duration =
        plan != nullptr ? plan->target_duration_s : duration_s;

    llm::Request req;
    req.prompt = prompt;
    // 四段的 schema，每段的拍数地板按这一章的时长算。**地板是这一步唯一
    // 管用的东西**：提示词里"要凑够"模型不听，minItems 4 它就写 4 拍——
    // 实测 60 秒的章写出 13 秒的剧本，就是从这儿来的。
    //
    // 名字同理：提示词里说了「一字不改」，实跑还是写出了林浩 / Lin Hao /
    // LinHao / Su Wan 四种。收成枚举，和分镜那边收 char_id 是一个道理。
    const bool chapter_script = plan != nullptr;
    req.schema = chapter_script
                     ? stages::script_schema_for_chapter(chapter_scenes, names)
                     : stages::script_schema(used_duration, names, variation);
    req.schema_name = "script";
    req.on_thinking = thinking_sink();

    // 走故事那条叫「改编」，照梗概写叫「写」——按钮上的字就是这么分的
    // （EpScript.vue 里那个 writeLabel），这儿跟着它说，不然顶栏说的和
    // 用户刚点的那个按钮对不上。
    // **名字要说清是哪一章。** 任务页面上一排下来，「正在改编成剧本」五个
    // 字每一行都一样，说不出是哪一章（用户 2026-09-17：「任务名要显示清楚
    // 干什么的」）。
    pipeline::Activity act{
        "script", paths::to_utf8(store.root()), episode_id,
        (plan != nullptr ? SAY("改编成剧本") : SAY("写剧本")) +
            (episode_id.empty() ? std::string{} : " · " + episode_id)};
    const pipeline::CancelLink stop_here{tok, act};

    // **一场一次调用，不是整章一次。**
    //
    // 2026-09-20 量出来的：这一步整章一次要模型吐 10~17K 字的 JSON，而
    // **超过一个长度谁都撑不住**——`finish_reason` 是 stop（正常写完），
    // 但 `scenes` 开成 `{` 关成 `]`，括号配不上。实测的分界线：
    //
    //     Qwen3-8B    ~6K 字       5345 / 6056 合法；9071 起全坏
    //     glm-5.3     ~13K 字      10799 / 13045 合法；13703 起全坏（低思考档）
    //
    // 不是模型大小的问题——**换更大的模型只是把断点往后推**。拆开之后每场
    // 一两千字，两边都在安全区里。
    //
    // 拆法不用动提示词：`ScenePlan` 自带 `key`（s1/s2…），传单场进去，
    // 提示词里【这一章的场】那一段和 schema 就都只剩那一场，而【这一章】
    // 的正文照旧整章给——那是上下文，要留。
    //
    // 分镜那一步 2026-09-15 就是这么拆的（storyboard_run.cpp 里
    // `scenes.size() > 1` 走逐场），这儿照它。
    stages::ScriptDraft draft;
    if (chapter_script && chapter_scenes.size() > 1) {
        for (std::size_t i = 0; i < chapter_scenes.size(); ++i) {
            const std::vector<stages::ScenePlan> one{chapter_scenes[i]};
            llm::Request one_req = req;
            one_req.prompt = stages::build_chapter_script_prompt(
                story, *plan, project.style_line, names, prev_tail, one);
            one_req.schema = stages::script_schema_for_chapter(one, names);
            // 顶栏说清在写第几场，不然一章三场看着像卡住了。
            act.set_note(SAYF("第 %1/%2 场", std::to_string(i + 1),
                              std::to_string(chapter_scenes.size())));
            const stages::ScriptDraft part = llm_guard([&] {
                return stages::parse_chapter_script(client.complete(one_req, tok),
                                                    one);
            });
            // 标题和梗概取第一场那次的：每场都会写一份，后面的没有更多信息。
            if (draft.title.empty()) draft.title = part.title;
            if (draft.logline.empty()) draft.logline = part.logline;
            draft.beats.insert(draft.beats.end(), part.beats.begin(),
                               part.beats.end());
            draft.acts.insert(draft.acts.end(), part.acts.begin(),
                              part.acts.end());
        }
    } else {
        draft = llm_guard([&] {
            const std::string raw = client.complete(req, tok);
            return chapter_script
                       ? stages::parse_chapter_script(raw, chapter_scenes)
                       : stages::parse_script(raw, used_duration, variation);
        });
    }

    // 梗概存到项目上。下次写新一章时直接回填，不用凭记忆重打。
    //
    // **写回之前重读一遍。** `project` 是这个请求一开头读的那一份，而上面
    // 那次生成跑了一两分钟——这几分钟里界面完全可能在改别的东西（镜头抽屉
    // 存一笔、改个章名、加一章）。这儿真正要改的只有 premise 一个字段，
    // 却要把整份旧 project 写回去，那些改动就被悄悄吞掉了。
    //
    // 同一个形状在批量那两条长任务上也有，理由写在 batch.cpp 里那两段。
    const std::string trimmed = text::strip_ws(premise);
    if (project.premise != trimmed) {
        Project latest = store.load_project();
        if (latest.premise != trimmed) {
            latest.premise = text::truncate_utf8(trimmed, 2000);
            store.save_project(latest);
        }
        // 本地这份也跟上：下面还要拿它拼回包
        project.premise = latest.premise;
    }

    const int budget = stages::budget_chars(used_duration);
    const auto chars = static_cast<double>(draft.dialogue_chars());
    json out = draft_common(draft, used_duration);
    // 写长了后面配音会把镜头撑爆，写短了成片不够时长，都得说出来
    // 章模式没有字数预算——长度由内容定，不判长短。
    // ⚠️ **`fit` 是接口的取值，不是给人看的话。** `cpp/tests/golden/
    // endpoints_scripting.json` 逐字钉着 `"fit": "偏短"`，翻了就是改了接口。
    // 要在界面上显示，该由读它的那一头按取值挑话说。
    out["fit"] = chapter_script          ? SAY_NEVER("合适")
                 : chars > budget * 1.35 ? SAY_NEVER("偏长")
                 : chars < budget * 0.6  ? SAY_NEVER("偏短")
                                         : SAY_NEVER("合适");
    out["continued_from"] = !previous.empty();
    out["reused_characters"] = names;
    // 这一章是照着故事写的还是照着一句梗概续的。前端靠它说清
    // 「这一章为什么是这些内容」，也让人一眼看出有没有走上新路子。
    out["source"] = source;
    if (plan != nullptr) {
        out["chapters"] = stages::episode_chapters(story, *plan);
        out["hook"] = plan->hook;
    }
    return {200, out};
}

ApiResult post_script_trailer(const json& body, llm::Client& client,
                              pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "duration_s", "episode_ids", "reuse_characters"});
    const double duration_s =
        num_in_range(body, "duration_s", 20.0, 0.0, 120.0);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    std::set<std::string> wanted;
    const auto ids = body.find("episode_ids");
    if (ids != body.end() && ids->is_array()) {
        for (const auto& v : *ids) {
            if (v.is_string()) wanted.insert(v.get<std::string>());
        }
    }

    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const AssetLibrary assets = load_assets_or_400(store);

    std::vector<const Episode*> picked;
    for (const auto& ep : project.episodes) {
        if (text::strip_ws(ep.script).empty()) continue;
        // 预告片自己不该当自己的素材
        if (ep.episode_id == kTrailerEpisodeId) continue;
        if (!wanted.empty() && wanted.count(ep.episode_id) == 0) continue;
        picked.push_back(&ep);
    }
    if (picked.empty()) {
        throw ApiError(400,
                       SAY("没有可用来剪预告的章节。先写几章正片，"
                           "再回来剪预告"));
    }



    std::string source;
    for (std::size_t i = 0; i < picked.size(); ++i) {
        if (i) source += "\n\n";
        // 同上：写预告的提示词里那一段，给模型看的。
        source += SAY_NEVER("【") + picked[i]->episode_id + " " +
                  picked[i]->title + SAY_NEVER("】\n") +
                  text::strip_ws(picked[i]->script);
    }

    const std::vector<std::string> names =
        reuse_chars ? character_names(assets) : std::vector<std::string>{};

    const std::string prompt = stages::build_trailer_prompt(
        project.premise, duration_s, project.style_line, source, names);

    llm::Request req;
    req.prompt = prompt;
    req.schema = stages::script_schema();  // 平的那份：预告片是蒙太奇，不分四段
    req.schema_name = "trailer";
    req.on_thinking = thinking_sink();

    pipeline::Activity act{"trailer", paths::to_utf8(store.root()), "",
                           SAY("正在剪预告")};
    const pipeline::CancelLink stop_here{tok, act};
    const stages::ScriptDraft draft = llm_guard([&] {
        return stages::parse_script(client.complete(req, tok));
    });

    const int budget = stages::budget_chars(duration_s);
    const auto chars = static_cast<double>(draft.dialogue_chars());
    json out = draft_common(draft, duration_s);
    // 预告片写长了比正片更要命：刷到第三秒还没看到钩子，人就划走了。
    // 所以上界比正片严得多——正片是 budget * 1.35，这里就是 budget。
    // 同上：`fit` 是接口的取值，不翻。
    out["fit"] = chars > budget          ? SAY_NEVER("偏长")
                 : chars < budget * 0.35 ? SAY_NEVER("偏短")
                                         : SAY_NEVER("合适");
    json from = json::array();
    for (const Episode* ep : picked) from.push_back(ep->episode_id);
    out["from_episodes"] = from;
    out["episode_id"] = kTrailerEpisodeId;
    return {200, out};
}

}  // namespace changji::http
