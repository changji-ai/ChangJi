#include "http/flow.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "util/say.hpp"

namespace changji::http {

using nlohmann::json;

namespace {

/// 取字符串字段，没有或类型不对就回空串。
///
/// **不能用 `value<std::string>`**：字段是 null 的时候它会抛
/// type_error，而 `location_id` 这类字段没填就是 null，是正常状态。
std::string str_of(const json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

const json& arr_of(const json& o, const char* key) {
    static const json kEmpty = json::array();
    if (!o.is_object()) return kEmpty;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return kEmpty;
    return *it;
}

}  // namespace

json flow_steps() {
    // 和 flow.js 的 STEPS 逐字对齐。**前端按 key 认人**，
    // 改一个字侧边栏就少一格。
    return json::array({
        json{{"key", "project"}, {"phase", "series"}, {"title", SAY("项目")},
             {"hint", SAY("选一个项目，或者新建一个")}},
        // **故事在剧本前面。** 原来第二步就是「剧本大纲」，而那一页是
        // 拿一句梗概逐章续写——章数人填、上下文只带前三章。故事这一步
        // 先把完整故事和章节定下来，剧本才有得可写。
        json{{"key", "story"}, {"phase", "series"}, {"title", SAY("故事")},
             {"hint", SAY("一章一章写；让 AI 写或改的都是眼前这一章")}},
        // **角色和场景 2026-09-11 合成一步「设定」。** 它们本来就是同一件
        // 事：人和地方在同一个 assets.json 里，都从故事提。而场景那一格
        // 原来还挂在「分集」阶段——那是个错位，场景库是全片共用的。
        json{{"key", "assets"}, {"phase", "series"}, {"title", SAY("设定")},
             {"hint", SAY("理解故事：人物、场景、长相、剧本一次出来；再把参考图画齐")}},
        // **镜头、成片、上传 2026-09-11 合成一步「这一章」。**
        //
        // 2026-09-10 已经把分镜和制作合过一次（同一张分镜表，一页排一页跑，
        // 而人在同一镜上来回）。这次是同一条理由再往外一层：人做的事是
        // **对照着看**——对着这句台词看这一镜对不对，看完整章顺手发出去。
        // 分成几页，来回换页才知道这一镜出自哪句话。
        json{{"key", "episode"}, {"phase", "episode"}, {"title", SAY("这一章")},
             {"hint", SAY("剧本、镜头、出片，都在这一章上；整部电影是最后把各章接起来")}},
        // 成片：有一章出了片就轮到它，把出了片的章接成一部电影。
        json{{"key", "film"}, {"phase", "series"}, {"title", SAY("成片")},
             {"hint", SAY("出了片的章按顺序接成一部完整的电影")}},
    });
}

json flow_assess(const json& project, const json& shots, const json& outputs,
                 const std::string& episode_id, const json& story,
                 const json& assets, const json& film) {
    json done = json::object();
    json counters = json::object();

    // ---- 项目 ----
    done["project"] = !str_of(project, "project_id").empty();

    // ---- 故事：**至少一章有内容**，不是"有章节" ----
    //
    // 判据不看梗概：梗概是写故事的输入，光有梗概什么都还没发生。
    //
    // 也不能只看"有没有章节"。故事页那颗「直接开写」建的正是**一章空的**
    // （`{title:"第一章", summary:"", text:""}`，那一条的用意是"开始写之前
    // 不需要配置任何东西"）——按章节数判的话，它一按下去这一步就打上勾、
    // 上面那个「下一步」的点跳到「设定」去，而人正要开始写第一章。
    //
    // 内容 = summary 或 text 非空。**两个都要认**：采用大纲那条路先有
    // summary、正文还没写；从别处粘正文进来那条先有 text、没有 summary。
    // 只认其中一个，另一条路就会在真写完之后还显示成没做。
    const auto& story_chapters = arr_of(story, "chapters");
    done["story"] = std::any_of(
        story_chapters.begin(), story_chapters.end(), [](const auto& c) {
            return !str_of(c, "summary").empty() || !str_of(c, "text").empty();
        });
    counters["chapters"] = story_chapters.size();
    counters["plannedEpisodes"] = arr_of(story, "plan").size();

    // ---- 「剧本大纲」那一格 2026-09-11 没有了 ----
    //
    // 全片那半在故事页，单章那半在「这一章」的剧本视图。`done` 里因此
    // 不能有这个键——test_flow 的「多出来的 key 是死代码」那条盯着。
    //
    // 写过几章这个数留在 counters 里。**但界面上没有一处读它**：项目栏
    // 那句「10 章里落成 3 章」是 project-stage.js 自己从项目的 episodes
    // 里数出来的。这儿原来写着"故事页要显示「已落成几章」"，那是把
    // 「有这么个数」当成了「有人在用这个数」。
    const auto& episodes = arr_of(project, "episodes");
    int written = 0;
    for (const auto& e : episodes) {
        const bool has_shots = e.is_object() && e.contains("shots") &&
                               e["shots"].is_number() &&
                               e["shots"].get<int>() > 0;
        if (has_shots || !str_of(e, "synopsis").empty()) ++written;
    }
    counters["writtenEpisodes"] = written;

    // ---- 设定 ----
    const auto& characters = arr_of(project, "characters");

    // 场景那一半的判据。
    //
    // 三个条件都要：注册过场景、这一章用到的场景都注册过、
    // **而且没有"靠 scene_id 蒙对"的镜头**。
    // 最后一条是关键：`scene_id` 恰好等于某个 location_id 的时候画面能出，
    // 但那是巧合不是关联——这一步不算做完，用户得去显式关联一次。
    std::set<std::string> known;
    for (const auto& l : arr_of(project, "locations")) {
        const auto id = str_of(l, "location_id");
        if (!id.empty()) known.insert(id);
    }
    std::set<std::string> used;
    int unlinked = 0;
    std::vector<std::string> missing;
    for (const auto& s : shots) {
        const auto loc = str_of(s, "location_id");
        if (!loc.empty()) {
            used.insert(loc);
        } else {
            const auto scene = str_of(s, "scene_id");
            if (!scene.empty() && known.count(scene)) {
                used.insert(scene);
                ++unlinked;
            }
        }
    }
    for (const auto& id : used) {
        if (!known.count(id)) missing.push_back(id);
    }
    const bool scenes_ok = !known.empty() && missing.empty() && unlinked == 0;

    // ---- 理解过没有 ----
    // 三样都在才算：结构在故事里（人物表）、长相在库里（assets 的人物）、
    // 每一章的剧本都写了。少一样，「出图」和「这一章」都还没到时候。
    // 用户 2026-09-17 定的顺序：理解故事 → 出图 → 这一章。
    const auto& story_chars = arr_of(story, "characters");
    const auto& lib_chars = arr_of(assets, "characters");
    int linked = 0;
    bool scripts_ok = true;
    for (const auto& e : episodes) {
        if (arr_of(e, "chapter_refs").empty()) continue;
        ++linked;
        if (!(e.is_object() && e.value("script_chars", 0) > 0)) scripts_ok = false;
    }
    const bool understood =
        !story_chars.empty() && !lib_chars.empty() && linked > 0 && scripts_ok;

    // ---- 参考图画齐了没有 ----
    // 每个人三张脸、每个地方一张空景。**数缺的，不数有的**——「有没有一张
    // 缺」才是正面判据（CLAUDE.md：别拿数量当判据）。
    int refs_missing = 0;
    for (const auto& c : lib_chars) {
        for (const char* slot : {"ref_front", "ref_three_quarter", "ref_back"}) {
            if (str_of(c, slot).empty()) ++refs_missing;
        }
    }
    for (const auto& l : arr_of(assets, "locations")) {
        if (str_of(l, "ref_empty").empty()) ++refs_missing;
    }
    const bool refs_ok = !lib_chars.empty() && refs_missing == 0;

    // 「设定」打勾 = 理解过 + 图画齐。原来只看"库里有人、场景对得上"。
    done["assets"] = understood && refs_ok;
    counters["charactersOk"] = !characters.empty();
    counters["scenesOk"] = scenes_ok;
    counters["understood"] = understood;
    counters["refsMissing"] = refs_missing;
    counters["refsOk"] = refs_ok;

    // ---- 镜头：每一镜都出到成片状态才算完 ----
    //
    // **判据取的是原来「制作」那条，不是「分镜」那条。** 合成一步之后
    // 这一格代表的是"这一章的镜头做完了"，光有分镜表不算做完——
    // 那时候一帧画面都还没有。宽松的判据会让侧边栏早早打勾，
    // 而用户回头发现什么都没出。
    static const std::set<std::string> kFinal = {"final_done", "locked",
                                                 "fallback"};
    int produced = 0;
    int lipsync = 0;
    double planned = 0.0;
    for (const auto& s : shots) {
        if (kFinal.count(str_of(s, "status"))) ++produced;
        if (s.is_object() && s.value("needs_lipsync", false)) ++lipsync;
        // **优先 real_duration_s。** `duration_s` 是名义值，模型按格子出帧，
        // 名义 2 秒实际出 2.333 秒——这个数报给界面当「这一章排了多长」，
        // 名义值会偏小。老响应没有这个字段时才退回去。
        if (s.is_object() && s.contains("real_duration_s") &&
            s["real_duration_s"].is_number()) {
            planned += s["real_duration_s"].get<double>();
        } else if (s.is_object() && s.contains("duration_s") &&
                   s["duration_s"].is_number()) {
            planned += s["duration_s"].get<double>();
        }
    }
    const bool shots_done =
        !shots.empty() && produced == static_cast<int>(shots.size());

    // ---- 成片：产物里有这一章的 ----
    //
    // ⚠️ **不能用裸 `find`。** 章号到 99 以内是 `ep%02d`，**第 100 章起
    // 变成 `ep100`、`ep101`……**（story_plan.cpp 的 `ep_id`、
    // http/episodes.cpp 的 `ep_fmt` 都刻意留了这一支）。于是站在 `ep10`
    // 上时 `"ep107.mp4".find("ep10")` 命中——ep100 到 ep109 十个成片文件全算
    // 成 ep10 的，这一格当场打勾，而 ep10 可能一帧都没出。整本书改编下来
    // 动辄七八十上百章，这不是假想的数。
    //
    // 判据要求**两边都挨着非字母数字**（或者到头）：`ep01.mp4` 真、
    // `ep01_2k.mp4` 真（手动超分那份还算这一章）、`导演版_ep01.mp4` 真、
    // `ep107.mp4` 对 `ep10` 假。前端 api/labels.js 的 `isFilmOf` 是同一
    // 条规则的 JS 版，两边要一起改。
    const auto film_of = [](const std::string& name, const std::string& id) {
        const auto alnum = [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z');
        };
        for (std::size_t at = name.find(id); at != std::string::npos;
             at = name.find(id, at + 1)) {
            const bool left = at > 0 && alnum(static_cast<unsigned char>(name[at - 1]));
            const std::size_t end = at + id.size();
            const bool right =
                end < name.size() && alnum(static_cast<unsigned char>(name[end]));
            if (!left && !right) return true;
        }
        return false;
    };
    bool has_film = false;
    for (const auto& o : outputs) {
        const auto name = str_of(o, "name");
        if (episode_id.empty() || film_of(name, episode_id)) {
            has_film = true;
            break;
        }
    }
    // ---- 这一章 ----
    //
    // **判据取原来「成片」那条**：装配出这一章的成片才算这一章做完了。
    //
    // 不取「镜头全出完」：那时候还没装配，拿不到成片。也不把投递算进来——
    // 投递记录存在 Node 那个 BFF 自己的配置里，引擎这边没有那份数据，
    // 而且发不发是可选的收尾动作，把它算进来会让这一格永远不打勾。
    done["episode"] = has_film;
    // 镜头有没有全出完另外报一个布尔，判定本身用的是上面那条。
    // **界面不读它**（「这一章」那一页的「还差 N 首帧 / N 视频」是自己
    // 拿 /api/shots 数的），但 test_flow 有四条用例盯着它，是这个文件里
    // 被测得最细的一条判据——「光有分镜表不算出完」说的就是它。
    counters["shotsDone"] = shots_done;

    // ---- 出了几章的片：有一章就能合成（成片那一格出不出现看 filmedChapters），
    //      allFilmed 只是给页面说"还有几章没出片"用 ----
    int filmed = 0;
    int linked_all = 0;
    for (const auto& e : episodes) {
        if (arr_of(e, "chapter_refs").empty()) continue;
        ++linked_all;
        const auto id = str_of(e, "episode_id");
        bool mine = false;
        for (const auto& o : outputs) {
            if (film_of(str_of(o, "name"), id)) {
                mine = true;
                break;
            }
        }
        if (mine) ++filmed;
    }
    counters["filmedChapters"] = filmed;
    counters["allFilmed"] = linked_all > 0 && filmed == linked_all;
    // 那一部电影在不在
    done["film"] = !arr_of(film, "files").empty();

    counters["shots"] = static_cast<int>(shots.size());
    counters["produced"] = produced;
    counters["characters"] = static_cast<int>(characters.size());
    counters["locations"] = static_cast<int>(known.size());
    counters["episodeLocations"] = static_cast<int>(used.size());
    counters["missingLocations"] = missing;
    counters["unlinkedShots"] = unlinked;
    counters["lipsync"] = lipsync;
    counters["plannedDurationS"] = planned;
    counters["outputs"] = static_cast<int>(outputs.size());

    return json{{"done", done}, {"counters", counters}};
}

}  // namespace changji::http
