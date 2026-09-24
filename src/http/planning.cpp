#include "http/planning.hpp"
#include "config/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "http/job_stream.hpp"
#include "http/reset.hpp"
#include "models/project.hpp"
#include "http/prompt_peek.hpp"
#include "pipeline/activity.hpp"
#include "pipeline/storyboard_run.hpp"
#include "stages/bible.hpp"
#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

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

std::string opt_str(const json& body, const char* key, const std::string& def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

double opt_num(const json& body, const char* key, double def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_number()) return def;
    return it->get<double>();
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

/// 把生成阶段的异常翻成 400。
///
/// Python 那边 /api/bible 只 catch BibleError、/api/plan 分别 catch
/// BibleError 和 StoryboardError，而 _extract_json 抛的是 StoryboardError——
/// 于是 /api/bible 上"模型吐了一坨不是 JSON 的东西"这个最常见的失败
/// 穿到最外面变成 500。这里三种都接住，统一回 400。
/// 有意的偏离，详见方案里那一节。
template <typename F>
auto stage_guard(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const stages::BibleError& e) {
        throw ApiError(400, e.what());
    } catch (const stages::StoryboardError& e) {
        throw ApiError(400, e.what());
    } catch (const llm::LlmError& e) {
        throw ApiError(400, e.what());
    }
}

AssetLibrary generate_bible(const std::string& script, StyleLine style_line,
                            const std::string& aspect_ratio,
                            llm::Client& client, pipeline::CancelToken& tok) {
    llm::Request req;
    req.prompt = stages::build_bible_prompt(script, style_line);
    req.schema = stages::bible_schema();
    req.schema_name = "bible";
    req.on_thinking = thinking_sink();
    return stage_guard([&] {
        return stages::parse_bible(client.complete(req, tok), style_line,
                                   aspect_ratio);
    });
}

/// 从故事出圣经。名单已经在故事里了，这一步只定妆。
///
/// 和上面那个的区别见 stages/bible.hpp：老的那条是"读一章剧本找出角色"，
/// 于是全片共用的资产库其实是从第一章推出来的。
AssetLibrary generate_bible_from_story(const Story& story, StyleLine style_line,
                                       const std::string& aspect_ratio,
                                       llm::Client& client,
                                       pipeline::CancelToken& tok) {
    llm::Request req;
    req.prompt = stages::build_bible_prompt_from_story(story, style_line);
    req.schema = stages::bible_schema_for_story(story);
    req.schema_name = "bible";
    req.on_thinking = thinking_sink();
    return stage_guard([&] {
        return stages::parse_bible_for_story(client.complete(req, tok), style_line,
                                             aspect_ratio, story);
    });
}

Story load_story_or_400(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 把新出的设定合进资产库，并拼出回包。
///
/// 从故事出和从剧本出两条路走到这里是一样的，所以提出来：合并规则、
/// 重跑镜头的判断、回包形状都只该有一份。source 只是告诉前端这次的名单
/// 是从哪来的——「为什么这次多出来三个人」全靠它解释。
/// 库里叫这个名字的是哪一条。**判重靠名字，不靠 id。**
///
/// 场景 id 是模型自己起的 key 拼出来的，而它每次给同一个地方起的 key 都
/// 不一样：一个实见的项目里「临川大学旧礼堂后台」占了三条 id。
/// 按 id 判重等于不判——每定一次妆，同一个地方就再进来一条，25 个场景里
/// 10 个是重的，而且同一个地方会各出一张不一样的空景图。
template <typename T>
const std::string* find_by_name(const OrderedMap<T>& items,
                                const std::string& name) {
    const std::string key{text::strip_ws(name)};
    if (key.empty()) return nullptr;
    for (const auto& kv : items) {
        if (std::string(text::strip_ws(kv.second.name)) == key) return &kv.first;
    }
    return nullptr;
}

/// 分镜表引用着哪些 id。合并同名时**优先留这些**——丢掉一个被引用的 id
/// 等于把那些镜头指空，而那不报错，只是渲染时拿不到空间和光线。
std::set<std::string> ids_in_use(const Project& project) {
    std::set<std::string> out;
    for (const auto& ep : project.episodes) {
        for (const auto& sh : ep.shots) {
            if (sh.location_id.has_value() && !sh.location_id->empty()) {
                out.insert(*sh.location_id);
            }
            // scene_id 也可能直接写着场景 id（模型十次有八次只填它）
            if (!sh.scene_id.empty()) out.insert(sh.scene_id);
            for (const auto& c : sh.characters) out.insert(c.char_id);
        }
    }
    return out;
}

/// 把镜头上的引用改到留下来的那个 id 上。返回改了几镜。
int remap_shots(Project& project, const IdRemap& remap) {
    if (remap.empty()) return 0;
    const auto lookup = [&remap](const std::string& id) -> const std::string* {
        const auto it = remap.find(id);
        return it == remap.end() ? nullptr : &it->second;
    };
    int touched = 0;
    for (auto& ep : project.episodes) {
        for (auto& sh : ep.shots) {
            bool hit = false;
            if (sh.location_id.has_value()) {
                if (const std::string* to = lookup(*sh.location_id)) {
                    sh.location_id = *to;
                    hit = true;
                }
            }
            if (const std::string* to = lookup(sh.scene_id)) {
                sh.scene_id = *to;
                hit = true;
            }
            for (auto& c : sh.characters) {
                if (const std::string* to = lookup(c.char_id)) {
                    c.char_id = *to;
                    hit = true;
                }
            }
            if (hit) ++touched;
        }
    }
    return touched;
}

ApiResult merge_bible(const ProjectStore& store, const AssetLibrary& fresh,
                      bool overwrite, const char* source) {
    // 读项目、读库、合并、存两份、清镜头状态——一把锁（ProjectStore::lock）。
    // 这里面没有大模型，锁得住。
    const auto store_guard = store.lock();
    Project project = load_or_400(store);
    // **库要在这儿读，不能让调用方在出圣经之前就读好。**
    //
    // 出一次圣经要跑一分钟往上。这一分钟里界面完全可能在改角色——抽屉里
    // 改完外观点保存，直接落 assets.json。拿一分钟前那份当底 merge 回去，
    // 那笔改动就没了，而且不报错。项目那一份（上面一行）本来就是现读的，
    // 库这一份跟上它。
    AssetLibrary assets = load_assets_or_400(store);
    const std::set<std::string> in_use = ids_in_use(project);

    // ---- 一、先把库里已经重了的收一收 ----
    //
    // 2026-09-14 之前这儿按 id 判重，于是同一个地方每定一次妆就多一条。
    // 存量得有人收，而定妆正是"整理这个库"的那个动作。
    IdRemap remap = dedupe_locations(assets, in_use);
    for (const auto& kv : dedupe_characters(assets, in_use)) remap.insert(kv);

    // ---- 二、合并，不是替换 ----
    //
    // 同名的默认保留旧的：手改过的设定、传过的参考图都挂在旧的那一份上。
    // 勾了覆盖才让新的顶掉，**但 id 留旧的**——分镜表里存的是 id，换掉
    // 等于把已有的镜头指空。
    std::vector<std::string> added_c, added_l;
    int kept = 0;
    for (const auto& kv : fresh.characters) {
        const std::string* mine = find_by_name(assets.characters, kv.second.name);
        const std::string id = mine != nullptr ? *mine : kv.first;
        if (mine != nullptr && !overwrite) {
            ++kept;
            continue;
        }
        if (mine == nullptr) added_c.push_back(id);
        Character c = kv.second;
        c.char_id = id;
        assets.characters[id] = std::move(c);
    }
    for (const auto& kv : fresh.locations) {
        const std::string* mine = find_by_name(assets.locations, kv.second.name);
        const std::string id = mine != nullptr ? *mine : kv.first;
        if (mine != nullptr && !overwrite) {
            ++kept;
            continue;
        }
        if (mine == nullptr) added_l.push_back(id);
        Location l = kv.second;
        l.location_id = id;
        assets.locations[id] = std::move(l);
    }
    store.save_assets(assets);

    // 收掉的那些 id 可能正被镜头引用着，跟着改过去。
    const int remapped = remap_shots(project, remap);
    if (remapped > 0) store.save_project(project);

    // 外观变了等于全片提示词都变了，已渲染的镜头得退回重跑。
    // 只新增没覆盖的话，老镜头用的还是原来那份设定，不用动。
    const int reset = overwrite ? reset_all_shots(store) : 0;

    const auto is_new_c = [&added_c](const std::string& id) {
        return std::find(added_c.begin(), added_c.end(), id) != added_c.end();
    };
    const auto is_new_l = [&added_l](const std::string& id) {
        return std::find(added_l.begin(), added_l.end(), id) != added_l.end();
    };

    json chars = json::array();
    for (const auto& kv : assets.characters) {
        const Character& c = kv.second;
        chars.push_back({{"char_id", c.char_id},
                         {"name", c.name},
                         {"identity", c.appearance.identity},
                         {"face", c.appearance.face},
                         {"attire", c.appearance.attire},
                         {"is_new", is_new_c(c.char_id)}});
    }
    json locs = json::array();
    for (const auto& kv : assets.locations) {
        const Location& l = kv.second;
        locs.push_back({{"location_id", l.location_id},
                        {"name", l.name},
                        {"space", l.space},
                        {"lighting", l.lighting},
                        {"is_new", is_new_l(l.location_id)}});
    }

    return {200, {
        {"added_characters", added_c},
        {"added_locations", added_l},
        {"kept", kept},
        // 收掉了几条重的、跟着改了几镜。界面上要说出来——
        // "场景从 25 变成 15" 不解释的话看着像丢了东西。
        {"merged", static_cast<int>(remap.size())},
        {"remapped_shots", remapped},
        {"characters", chars},
        {"locations", locs},
        {"reset_shots", reset},
        {"source", source},
    }};
}


/// 对应 Python 的 round(x, 1)：**银行家舍入**。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

}  // namespace

ApiResult merge_assets(const ProjectStore& store, const AssetLibrary& fresh,
                       bool overwrite, const char* source) {
    return merge_bible(store, fresh, overwrite, source);
}

ApiResult post_assets_dedupe(const json& body) {
    // 把库里同名的场景/角色收成一条，镜头上的引用跟着改。**不叫模型。**
    //
    // 定妆那条路已经顺手做这件事了（见 merge_bible），但定妆要跑一趟大模型
    // ——思考模型十几分钟一次——而存量项目里的重名是 2026-09-14 之前按 id
    // 判重攒下来的（一个实见项目 25 个场景 9 个名字），收它们不该要等一趟
    // 大模型。这个口子就是那一半：只收，不出新的。
    forbid_extra(body, {"project"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    AssetLibrary assets = load_assets_or_400(store);

    const int before_l = static_cast<int>(assets.locations.size());
    const int before_c = static_cast<int>(assets.characters.size());
    const std::set<std::string> in_use = ids_in_use(project);
    IdRemap remap = dedupe_locations(assets, in_use);
    for (const auto& kv : dedupe_characters(assets, in_use)) remap.insert(kv);
    if (remap.empty()) {
        return {200, {{"merged", 0}, {"remapped_shots", 0},
                      {"characters", before_c}, {"locations", before_l}}};
    }
    store.save_assets(assets);
    const int remapped = remap_shots(project, remap);
    if (remapped > 0) store.save_project(project);

    json dropped = json::object();
    for (const auto& kv : remap) dropped[kv.first] = kv.second;
    return {200, {
        {"merged", static_cast<int>(remap.size())},
        {"remapped_shots", remapped},
        {"characters", static_cast<int>(assets.characters.size())},
        {"locations", static_cast<int>(assets.locations.size())},
        // 谁并进了谁。界面上不显示，但排障时"那个 id 去哪了"就靠它
        {"dropped", dropped},
    }};
}

ApiResult post_bible(const json& body, llm::Client& client,
                     pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "episode_id", "script", "overwrite", "source"});
    const std::string episode_id = opt_str(body, "episode_id", "");
    const bool overwrite = opt_bool(body, "overwrite", false);

    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    // **先验一遍资产库读不读得动，再去跑大模型。**
    //
    // 真正要用的那一份在 merge_bible 里现读（那是为了不吞掉出圣经这一分钟
    // 里用户在抽屉里改的东西，理由写在那儿）。但"文件本身是坏的"这件事要
    // 早说：不验的话，一个坏掉的 assets.json 会让人白等一分钟大模型，
    // 回来才看到 400。库不存在不算坏——load_assets 回一个空库，新项目第一次
    // 定妆走的正是这条。
    load_assets_or_400(store);

    // **顶栏那本账要记上。** 这是个同步接口，没有任务表那一套，
    // 2026-09-13 之前它在界面上整个不可见——而它占着 LLM 槽，
    // 另一头的活会挂在「显存不够加载 LLM」上却查不到挡路的是谁。
    // 见 pipeline/activity.hpp 开头那段。放在这儿盖住下面两条分支。
    pipeline::Activity act{"bible", paths::to_utf8(store.root()), episode_id,
                           SAY("正在定角色和场景")};
    const pipeline::CancelLink stop_here{tok, act};

    // 名单从哪来。默认看项目里有没有故事——有就从故事出，那份名单是全片
    // 完整的；没有就退回老路径从一章剧本里找，老项目还得能用。
    const std::string source = opt_str(body, "source", "auto");
    if (source != "auto" && source != "story" && source != "script") {
        throw unprocessable_top("source",
                                "Input should be 'auto', 'story' or 'script'",
                                body.at("source"), "enum");
    }
    // 定妆要知道这部电影是横是竖：比例由画幅派生，而它会写进资产库
    // （见 StyleProfile::aspect_ratio）。以前这儿吃 parse_bible 的默认值
    // "9:16"，横屏项目每定一次妆就被改回竖屏。
    const std::string ratio =
        config::load_settings(store.root()).video.aspect_ratio();

    const Story story = load_story_or_400(store);
    if (source == "story" && story.chapters.empty()) {
        throw ApiError(400, SAY("这个项目还没有故事，先去写一份大纲"));
    }
    if (source != "script" && !story.chapters.empty()) {
        const AssetLibrary from_story =
            generate_bible_from_story(story, project.style_line, ratio, client,
                                      tok);
        return merge_bible(store, from_story, overwrite, "story");
    }

    std::string script = text::strip_ws(opt_str(body, "script", ""));
    if (script.empty() && !episode_id.empty()) {
        const Episode* ep = project.episode_by_id(episode_id);
        script = ep ? text::strip_ws(ep->script) : std::string();
    }
    if (script.empty()) {
        // 没指定就用第一章有内容的剧本。角色设定是全片共用的，
        // 拿哪一章出都行，但总得有一章写好了。
        for (const auto& ep : project.episodes) {
            const std::string s = text::strip_ws(ep.script);
            if (!s.empty()) {
                script = s;
                break;
            }
        }
    }
    if (script.empty()) throw ApiError(400, SAY("还没有剧本，先去写一章"));

    const AssetLibrary fresh =
        generate_bible(script, project.style_line, ratio, client, tok);

    return merge_bible(store, fresh, overwrite, "script");
}

ApiResult post_plan(const json& body, llm::Client& client,
                    pipeline::CancelToken& tok) {
    // 注意这里**没有** forbid_extra。Python 的 PlanRequest 没写
    // model_config = {"extra": "forbid"}，pydantic 默认是忽略多余字段。
    // 加上校验就会拒掉 Python 能接受的请求。
    const bool peek = body.is_object() && body.value("peek", false);
    const std::string pasted =
        body.is_object() ? body.value("paste", std::string()) : std::string();
    const std::string script = text::strip_ws(need_str(body, "script"));
    if (script.empty()) throw ApiError(400, SAY("剧本是空的"));

    const std::string episode_id = opt_str(body, "episode_id", "ep01");
    const double duration_s = opt_num(body, "duration_s", 60.0);
    const bool regenerate = opt_bool(body, "regenerate_bible", false);
    int placed_lines = 0;

    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    AssetLibrary assets = load_assets_or_400(store);

    // 同 post_bible：同步接口也要在顶栏露面。一个 Activity 盖住整段——
    // 出分镜这一次点击底下可能要跑两趟模型（先补圣经再拆镜头），
    // 对用户那是一件事，中途只换那句话。
    pipeline::Activity act{"plan", paths::to_utf8(store.root()), episode_id,
                           SAY("正在拆镜头")};
    const pipeline::CancelLink stop_here{tok, act};

    // 角色设定。已有就不重做，避免覆盖用户改过的设定。
    if (regenerate || assets.characters.empty()) {
        act.set_message(SAY("正在定角色和场景"));
        // 有故事就从故事出——名单是全片完整的，不是从这一章里找出来的。
        const Story story = load_story_or_400(store);
        // 比例由画幅派生，理由同 post_bible 里那一段。
        const std::string ratio =
            config::load_settings(store.root()).video.aspect_ratio();
        assets = story.chapters.empty()
                     ? generate_bible(script, project.style_line, ratio, client,
                                      tok)
                     : generate_bible_from_story(story, project.style_line,
                                                 ratio, client, tok);
        store.save_assets(assets);
        act.set_message(SAY("正在拆镜头"));
    }

    // 单镜的时长档位是这部电影的属性（[video].max_shot_s），按项目那份设置
    // 算一遍再拆镜头——只挂在这条线程上（见 stages::ScopedVideoLimits）。
    const stages::ScopedVideoLimits limits_here{
        config::video_limits_for(config::load_settings(store.root()))};
    // 切场、拆镜、补台词、查覆盖、重编号、拉回时长，都在 run_storyboard 里
    // ——三处调用共用那一份，按场拆镜（2026-09-15）就是在那儿分的岔。
    pipeline::StoryboardRunOptions sb;
    sb.script = script;
    sb.assets = assets;
    sb.episode_id = episode_id;
    sb.duration_s = duration_s;
    sb.on_thinking = thinking_sink();
    sb.on_progress = [&act](const std::string& m) { act.set_message(m); };
    sb.peek = peek;
    if (!pasted.empty()) {
        sb.pasted = split_by_scene(pasted);
        // **数量对不上就当场说，别硬跑。** 少一段的话后面几场整体错位
        // 一场，而错位出来的分镜表看着是合法的，没有任何报错——人要等到
        // 出片才发现第二章的画面配着第三章的台词。
        const std::size_t want = stages::split_scenes(script, assets).size();
        if (want > 1 && sb.pasted.size() != want) {
            throw ApiError(
                400,
                SAYF("这一章拆成 %1 场，粘回来的只有 %2 段。每一场之间要留着"
                     "复制出去时那一行「===== 第 N/M 场 …… =====」",
                     std::to_string(want), std::to_string(sb.pasted.size())));
        }
    }

    if (peek) {
        // 按场跑的话这儿是三份拼起来的，各带场次头。见 StoryboardRunOptions::peek。
        const pipeline::StoryboardRunResult r =
            pipeline::run_storyboard(sb, client, tok);
        llm::Request shown;
        shown.schema_name = "storyboard";
        shown.prompt = r.peeked;
        return peek_prompt(shown);
    }

    std::vector<Shot> shots = stage_guard([&] {
        pipeline::StoryboardRunResult r = pipeline::run_storyboard(sb, client, tok);
        placed_lines = r.placed_lines;
        return std::move(r.shots);
    });

    // **写回去之前重新读一份。**
    //
    // 上面拆一趟镜头要一到几分钟（`regenerate` 那条还要先跑一趟定妆），
    // 而 `project` 是那几分钟**之前**读的。把它整份写回去，这期间界面上改
    // 的东西全被悄悄吞掉：别的章的镜头抽屉存的那一笔、改过的章名、手动加
    // 的一章。批量那条（post_plan_all）和写全片那条都是这么修的。
    const auto store_guard = store.lock();   // 读→改→存一把锁（ProjectStore::lock）
    Project latest = store.load_project();
    Episode* ep = latest.episode_by_id(episode_id);
    if (ep == nullptr) {
        Episode fresh;
        fresh.episode_id = episode_id;
        fresh.target_duration_s = duration_s;
        latest.episodes.push_back(std::move(fresh));
        ep = &latest.episodes.back();
    }
    ep->script = need_str(body, "script");   // 存原文，不是 strip 过的
    ep->target_duration_s = duration_s;
    ep->shots = shots;
    // **记下这张表是照哪一版剧本拆的**，理由见 Episode::shots_from。
    // 算在刚存下的这一份上——`ep->script` 就是这一趟拆镜头照着的那份原文，
    // 拿 ep 里的旧值算就会记成上一版，那正好是这一栏要防的事。
    ep->shots_from = chapter_text_fingerprint(ep->script);
    store.save_project(latest);

    // **按成片长度报，不按分镜表那串名义值加。** 模型只能按格子出帧，
    // 名义 4 秒出来是 4.458 秒（见 stages::real_total_s）。这个数不只是
    // 显示：前端拿它判「只排到 X，分镜太少」（`< target * 0.8`），
    // 名义值偏小会把排够了的一章误判成不够。
    const double total = stages::real_total_s(shots);
    int lipsync = 0;
    for (const Shot& s : shots) {
        if (s.needs_lipsync) ++lipsync;
    }

    json chars = json::array();
    for (const auto& kv : assets.characters) {
        chars.push_back({{"char_id", kv.second.char_id},
                         {"name", kv.second.name},
                         {"face", kv.second.appearance.face}});
    }
    json locs = json::array();
    for (const auto& kv : assets.locations) {
        locs.push_back({{"location_id", kv.second.location_id},
                        {"name", kv.second.name}});
    }

    // **有几镜没落到场景上。**
    //
    // 分镜 schema 里 location_id 是 `anyOf[enum, null]`，模型可以不填，
    // 而不填的后果是出首帧时**整段场景描述丢掉**——同一个咖啡馆的几镜
    // 各画各的。2026-09-13 实测 walk_c ep01：18 镜里 13 镜是空的，
    // 而那 13 镜的 scene_id 写着 loc_caf_day / loc_ca_day / lo_cafe_da
    // ——模型在抄自己上一条输出并且越抄越缺字母，link_location 要精确
    // 匹配，于是全部落空。
    //
    // **这里只报数，不强制模型必须选一个。** 对拍语料里有 82 条
    // `"location_id": null`，那是合法状态（手建的镜头、还没登记场景的
    // 项目）；而实测也见过选错的（sh001 是雨夜街头，却挂上了医院急诊科，
    // 出来的图是雨衣骑手站在病房里）。强制选等于把"没有"换成"可能是错
    // 的"，那不一定更好。人看见这个数就能自己判——补、还是重出一次。
    int no_location = 0;
    for (const Shot& s : shots) {
        if (!s.location_id.has_value() || s.location_id->empty()) ++no_location;
    }

    // 补完之后照理一句都不该漏。还漏的话说明落位那一步也没兜住，
    // 照旧报出来——不拦，但要让人看见。
    json warnings = json::array();
    for (const std::string& line :
         stages::missing_dialogue_lines(script, shots)) {
        warnings.push_back(line);
    }

    return {200, {
        {"episode_id", episode_id},
        {"shots", shots.size()},
        {"duration_s", round1(total)},
        {"missing_lines", warnings},
        {"placed_lines", placed_lines},
        {"shots_without_location", no_location},
        {"lipsync", lipsync},
        {"characters", chars},
        {"locations", locs},
        // **整份分镜按场装成一个数组**（用户 2026-09-18）。复制出去、改完
        // 粘回来（`paste`）是同一个形状，场次靠里面的 `scene` 对齐。
        {"scenes", stages::scenes_array(shots)},
    }};
}

}  // namespace changji::http
