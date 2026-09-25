#include "http/episodes.hpp"
#include "http/job_stream.hpp"
#include "config/runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "models/story.hpp"
#include "pipeline/activity.hpp"
#include "stages/script.hpp"
#include "stages/script_story.hpp"
#include "stages/story_plan.hpp"
#include "pipeline/storyboard_run.hpp"
#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

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

ProjectStore open_project(const std::string& path) {
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

std::string ep_fmt(int n) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ep%02d", n);
    return std::string(buf);
}

/// 对应 re.fullmatch(r"[a-z0-9_]+", s)。
bool is_valid_episode_id(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

/// 把 shot_id / scene_id 里的旧章号换成新的，**只换第一处**。
/// 对应 Python 的 s.replace(old, new, 1)。
std::string replace_first(const std::string& s, const std::string& from,
                          const std::string& to) {
    if (from.empty()) return s;
    const std::size_t pos = s.find(from);
    if (pos == std::string::npos) return s;
    return s.substr(0, pos) + to + s.substr(pos + from.size());
}

}  // namespace

std::string next_episode_id(const Project& project) {
    std::set<std::string> existing;
    int n = 1;
    for (const auto& ep : project.episodes) {
        existing.insert(ep.episode_id);
        if (ep.episode_id.size() > 2 && ep.episode_id.compare(0, 2, "ep") == 0) {
            const bool all_digits =
                std::all_of(ep.episode_id.begin() + 2, ep.episode_id.end(),
                            [](unsigned char c) { return c >= '0' && c <= '9'; });
            if (all_digits) ++n;
        }
    }
    while (existing.count(ep_fmt(n))) ++n;
    return ep_fmt(n);
}

ApiResult get_script(const std::string& path, const std::string& episode_id) {
    ProjectStore store = open_project(path);
    const Project project = load_or_400(store);
    const Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));
    return {200, {
        {"script", ep->script},
        {"target_duration_s", ep->target_duration_s},
        {"title", ep->title},
    }};
}

ApiResult get_script_context(const std::string& path,
                             const std::string& episode_id) {
    ProjectStore store = open_project(path);
    const Project project = load_or_400(store);
    const Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));

    // 读不了就当没有：老项目本来就没有 story.json，这一章照样有答案，只是短一些。
    Story story;
    try {
        story = store.load_story();
    } catch (const std::exception&) {
    }
    // 一条章节记录就是整整一章，计划按章配（整章，钩子取最后一场的 turn）。
    // **不按 episode_id 去章节计划里查。** 写剧本那头（scripting.cpp）2026-09-16
    // 就改成这样了，这儿漏了：老项目盘上那份计划把一章切成上/中/下，ep01 查到
    // 的是前三分之一，剧本页的「原文」就只有半章（2026-09-18 用户撞到）。
    // 「上一章」取故事里的前一章，id 按同一条规矩推。
    EpisodePlan chapter_plan_storage;
    const EpisodePlan* plan = nullptr;
    std::string prev_episode_id;
    if (!ep->chapter_refs.empty()) {
        const std::string& cid = ep->chapter_refs.front();
        for (std::size_t i = 0; i < story.chapters.size(); ++i) {
            if (story.chapters[i].chapter_id != cid) continue;
            chapter_plan_storage =
                stages::chapter_plan(story, cid, ep->target_duration_s);
            chapter_plan_storage.episode_id = episode_id;
            plan = &chapter_plan_storage;
            if (i > 0) {
                prev_episode_id = stages::episode_id_for_chapter(
                    story.chapters[i - 1].chapter_id, i - 1);
            }
            break;
        }
    }
    if (plan == nullptr) {
        // 老项目：章节记录上没记章，只能按盘上那份计划的 id 查，上一章取表里的上一条。
        for (std::size_t i = 0; i < story.plan.size(); ++i) {
            if (story.plan[i].episode_id != episode_id) continue;
            plan = &story.plan[i];
            if (i > 0) prev_episode_id = story.plan[i - 1].episode_id;
            break;
        }
    }
    // 时长和写剧本那一步同源：有章节计划按章节计划，没有按这一章自己的。
    const double duration =
        plan != nullptr ? plan->target_duration_s : ep->target_duration_s;

    // 四段按秒怎么排。
    //
    // ⚠️ **这里只能按 variation = 0 算，也就是"步步紧逼"那一组**（开场钩子 /
    // 冲突推进 / 情绪回报 / 章尾留扣）。真正写剧本那一下 variation 是
    // `random_shape()` 现摇的（见 post_script_write），五组走法里挑一组，
    // 而那个数**一个字节都没存下来**——这条接口没法知道那一章用的是哪一组。
    //
    // 所以这几个 label 只在五分之一的情况下和剧本里的段头对得上：一章写的
    // 是「【当头一击 0–5 秒】」，这儿报的却是「开场钩子」。秒数也一样，
    // 三个比例都随 variation 浮动。
    //
    // 界面今天不画这一段（四段读数是 ScriptReader 从**剧本正文里的段头**
    // 解出来的，那才是这一章真正的那一组）。要画之前先想清楚上面这件事，
    // 或者先把 variation 存进 Episode 里。
    json acts = json::array();
    for (const auto& a : stages::act_plan(duration)) {
        acts.push_back({{"key", a.key},
                        {"label", a.label},
                        {"from_s", a.from_s},
                        {"to_s", a.to_s},
                        {"min_beats", a.min_beats},
                        {"max_beats", a.max_beats}});
    }

    json out = {
        {"episode_id", episode_id},
        {"title", ep->title},
        {"source", plan != nullptr ? "story" : "premise"},
        // **这一章挂着故事里的某一章，而那一章一个字都还没写。**
        //
        // 剧本页拿它做两件事：把「改编成剧本」关掉（按了也是 400，引擎那头
        // 挡着，见 http/scripting.cpp），以及给已经躺在那儿的剧本挂一块牌子
        // 说清它是照什么写的——用户 2026-09-19：「我一个字没写的章节哪来的
        // 剧本和分镜」。
        //
        // **由引擎回答，不让页面自己推。** 页面手里只有 `text` 空不空，而
        // "没正文"和"这一章不在章节计划上"（老项目、手动加的章、预告片，
        // 那几种照梗概写是对的）在它看来长得一样。同一句话在三处问，收在
        // models::chapter_written。
        {"chapter_blank", !ep->chapter_refs.empty() &&
                              !models::chapter_written(story, *ep)},
        {"target_duration_s", duration},
        {"budget_chars", stages::budget_chars(duration)},
        {"acts", acts},
        {"chapters", json::array()},
        {"scenes", json::array()},
        {"text", ""},
        {"hook", ""},
        {"previous_tail", ""},
    };
    if (plan != nullptr) {
        json chapters = json::array();
        for (const auto& id : stages::episode_chapters(story, *plan)) {
            const Chapter* c = story.chapter_by_id(id);
            chapters.push_back({{"chapter_id", id},
                                {"title", c != nullptr ? c->title : ""}});
        }
        out["chapters"] = chapters;
        out["scenes"] = json(stages::episode_scenes(story, *plan));
        out["text"] = stages::episode_text(story, *plan);
        out["hook"] = plan->hook;
        // 上一章的结尾——故事里的前一章，和写剧本那一步一样。
        if (!prev_episode_id.empty()) {
            const Episode* prev = project.episode_by_id(prev_episode_id);
            if (prev != nullptr) {
                out["previous_tail"] = stages::script_tail(prev->script);
            }
        }
    }
    return {200, out};
}

ApiResult post_script(const json& body, llm::Client& client,
                      pipeline::CancelToken& tok) {
    // 没有 extra="forbid"：Python 的 ScriptUpdateRequest 没写 model_config，
    // pydantic 默认忽略多余字段。加上校验会拒掉它能接受的请求。
    const std::string episode_id = need_str(body, "episode_id");
    const std::string script = need_str(body, "script");
    const bool regenerate = opt_bool(body, "regenerate", false);

    ProjectStore store = open_project(need_str(body, "project"));
    Project project = load_or_400(store);
    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));

    ep->script = script;
    // 注意是 `if req.duration_s:` 不是 `is not None`——**0 也算没给**。
    // 照抄：给 0 的话下面按原时长走，不会把这一章设成零秒。
    const auto dit = body.find("duration_s");
    if (dit != body.end() && dit->is_number() && dit->get<double>() != 0.0) {
        ep->target_duration_s = dit->get<double>();
    }
    // 梗概。**空字符串不算给了**——手工存剧本时前端不带这一项，
    // 带了也可能是空的，那种情况下不能把已有的梗概洗掉。
    //
    // 以前这一项根本不收，结果单章这条路上 synopsis 永远是空的
    // （只有「写全片」会写）。连着坏三处：剧本页的「已写 N 章」
    // 一直是 0、「AI 剪一条预告片」永远点不亮、下次写新一章时
    // build_premise_prompt/build_script_prompt 的 existing 也是空的——
    // 「接着前几章写」勾了等于没勾，上下文断在这儿。
    const std::string synopsis = text::strip_ws(opt_str(body, "synopsis", ""));
    if (!synopsis.empty()) {
        ep->synopsis = text::truncate_utf8(synopsis, 2000);
    }

    json out = {{"saved", true}, {"regenerated", false}};
    if (regenerate) {
        const AssetLibrary assets = store.load_assets();
        // 单镜的时长档位是这部电影的属性（[video].max_shot_s），按项目那份设置
        // 算一遍再拆镜头——只挂在这条线程上（见 stages::ScopedVideoLimits）。
        const stages::ScopedVideoLimits limits_here{
            config::video_limits_for(config::load_settings(store.root()))};

        // 同步接口也要在顶栏露面，理由见 pipeline/activity.hpp 开头那段：
        // 它占着 LLM 槽，不露面的话别人挂在「显存不够」上而挡路的是谁查不到。
        pipeline::Activity act{"plan", paths::to_utf8(store.root()), episode_id,
                               SAY("正在拆镜头")};
        const pipeline::CancelLink stop_here{tok, act};
        // 切场、拆镜、补台词、查覆盖、重编号、拉回时长都在 run_storyboard 里，
        // 和 post_plan 是同一份（按场拆镜在那儿分岔）。
        pipeline::StoryboardRunOptions sb;
        sb.script = script;
        sb.assets = assets;
        sb.episode_id = episode_id;
        sb.duration_s = ep->target_duration_s;
        sb.on_thinking = thinking_sink();
        sb.on_progress = [&act](const std::string& m) { act.set_message(m); };
        try {
            ep->shots = pipeline::run_storyboard(sb, client, tok).shots;
            // 照哪一版剧本拆的，见 Episode::shots_from。这一支拆的就是刚
            // 存进去那一版，所以这儿记完之后 shots_stale() 当场是 false。
            ep->shots_from = chapter_text_fingerprint(script);
        } catch (const stages::StoryboardError& e) {
            throw ApiError(400, e.what());
        } catch (const llm::LlmError& e) {
            throw ApiError(400, e.what());
        }
        out["regenerated"] = true;
        out["shots"] = ep->shots.size();
    }
    // **只把这一章换进此刻盘上那份，一把锁。** `project` 是进门时读的，
    // 拆镜头那一趟要几分钟——整份写回去，这几分钟里别的对话写好的别几章的
    // 剧本、分镜全被冲回旧样子（批量那几条早就是"重读、只换自己那一格"）。
    const auto store_guard = store.lock();
    Project latest = load_or_400(store);
    Episode* dst = latest.episode_by_id(episode_id);
    if (dst == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));
    dst->script = ep->script;
    dst->target_duration_s = ep->target_duration_s;
    dst->synopsis = ep->synopsis;
    if (regenerate) {
        dst->shots = ep->shots;
        dst->shots_from = ep->shots_from;
    }
    store.save_project(latest);
    return {200, out};
}

ApiResult post_episode(const json& body) {
    const std::string title = opt_str(body, "title", "");
    double target = 60.0;
    const auto tit = body.find("target_duration_s");
    if (tit != body.end() && tit->is_number()) target = tit->get<double>();

    ProjectStore store = open_project(need_str(body, "project"));
    const auto store_guard = store.lock();   // 读→改→存一把锁（CLAUDE.md 第十四条）
    Project project = load_or_400(store);

    std::string ep_id = text::strip_ws(opt_str(body, "episode_id", ""));
    if (ep_id.empty()) ep_id = next_episode_id(project);
    if (!is_valid_episode_id(ep_id)) {
        throw ApiError(400, SAY("章节 id 只能用小写字母、数字和下划线"));
    }
    if (project.episode_by_id(ep_id) != nullptr) {
        throw ApiError(409, SAYF("章节 %1 已存在", ep_id));
    }

    Episode ep;
    ep.episode_id = ep_id;
    ep.title = title;
    ep.target_duration_s = target;
    project.episodes.push_back(std::move(ep));
    store.save_project(project);
    return {200, {{"episode_id", ep_id}, {"title", title}}};
}

ApiResult post_episode_action(const json& body) {
    const std::string episode_id = need_str(body, "episode_id");
    const std::string action = need_str(body, "action");

    ProjectStore store = open_project(need_str(body, "project"));
    const auto store_guard = store.lock();   // 读→改→存一把锁（CLAUDE.md 第十四条）
    Project project = load_or_400(store);
    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));

    if (action == "delete") {
        // 至少留一章。删空了界面会退回"还没有项目"的状态，
        // 用户以为整个项目没了。
        if (project.episodes.size() <= 1) {
            throw ApiError(400, SAY("至少要留一章"));
        }
        project.episodes.erase(
            std::remove_if(project.episodes.begin(), project.episodes.end(),
                           [&](const Episode& e) {
                               return e.episode_id == episode_id;
                           }),
            project.episodes.end());
        store.save_project(project);
        return {200, {{"deleted", episode_id}}};
    }

    if (action == "rename") {
        ep->title = opt_str(body, "new_title", "");
        const std::string title = ep->title;
        store.save_project(project);
        // **挂着故事里一章的，那一章的标题一起改。** 章节记录上的名字是从
        // 故事那一章抄来的（sync_episodes_to_chapters 每次存故事都照抄一遍），
        // 只改这一头的话，下一次谁存一下故事，名字就静悄悄变回去了。
        // 空名字不往故事里写：故事那头标题不许空（/api/story/chapter/edit）。
        if (!ep->chapter_refs.empty() && !text::strip_ws(title).empty()) {
            models::Story story = store.load_story();
            for (auto& c : story.chapters) {
                if (c.chapter_id == ep->chapter_refs.front() && c.title != title) {
                    c.title = title;
                    store.save_story(story);
                    break;
                }
            }
        }
        return {200, {{"renamed", episode_id}, {"title", title}}};
    }

    if (action == "duplicate") {
        // 编号规则和 next_episode_id **不一样**：这里数的是全部章节数，
        // 那边只数 epNN。Python 侧就是这么写的，照抄。
        //
        // 差别在于项目里有 trailer 时：新建走 next_episode_id 得到 ep03，
        // 复制走这里得到 ep04（因为 trailer 也被数了）。两个入口给出不同的
        // 编号，看着像 bug，但改了会让两边的项目文件对不上。
        std::set<std::string> existing;
        for (const auto& e : project.episodes) existing.insert(e.episode_id);
        int n = static_cast<int>(project.episodes.size()) + 1;
        while (existing.count(ep_fmt(n))) ++n;
        const std::string new_id = ep_fmt(n);

        std::vector<Shot> shots;
        for (const Shot& s : ep->sorted_shots()) {
            Shot copy = s;
            copy.shot_id = replace_first(copy.shot_id, episode_id, new_id);
            copy.scene_id = replace_first(copy.scene_id, episode_id, new_id);
            // 产出物和状态不带过去，复制出来的是要重跑的。
            // 带过去的话它会显示成已完成但没有文件，点播放是黑的。
            copy.status = ShotStatus::PLANNED;
            copy.attempts = 0;
            copy.gate_notes.clear();
            copy.frame_path = std::nullopt;
            copy.video_path = std::nullopt;
            copy.duration_locked = false;
            for (auto& line : copy.dialogue) {
                line.audio_path = std::nullopt;
                line.actual_duration_s = std::nullopt;
            }
            shots.push_back(std::move(copy));
        }

        Episode fresh;
        fresh.episode_id = new_id;
        fresh.title =
            SAYF("%1 副本", ep->title.empty() ? episode_id : ep->title);
        fresh.script = ep->script;
        fresh.target_duration_s = ep->target_duration_s;
        fresh.shots = std::move(shots);
        // **这一栏要跟着走。** 副本的剧本和分镜都是原样抄的，那它照的就是
        // 同一版剧本。不带过去的话副本读出来是空串（"说不清"），于是原章
        // 挂着「照旧剧本拆的」、它的副本干干净净——同一张表两种说法。
        fresh.shots_from = ep->shots_from;
        const std::size_t count = fresh.shots.size();
        project.episodes.push_back(std::move(fresh));
        store.save_project(project);
        return {200, {{"episode_id", new_id}, {"shots", count}}};
    }

    throw ApiError(400, SAYF("不认识的操作 %1，可选：delete、duplicate、"
                             "rename",
                             action));
}

}  // namespace changji::http
