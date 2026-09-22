// 批量类的编辑接口：/api/shots/batch /api/shots/reorder /api/shots/link_locations。
//
// 这三个的共同点是**一次动很多镜头**，所以每一个都有一条"哪些不该动"的规则：
// batch 的 reset 跳过锁定的，reorder 不重跑任何镜头，link_locations 只补空着的。
// 这些规则比接口本身重要——错了就是一次误操作废掉几小时的渲染。

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "http/editing.hpp"
#include "http/reset.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, SAYF("请求里缺少字符串字段 %1", key));
    }
    return body.at(key).get<std::string>();
}

std::vector<std::string> str_array(const json& body, const char* key) {
    std::vector<std::string> out;
    if (!body.contains(key) || body.at(key).is_null()) return out;
    if (!body.at(key).is_array()) {
        throw ApiError(400, SAYF("%1 要是数组", key));
    }
    for (const auto& v : body.at(key)) {
        if (!v.is_string()) throw ApiError(400, SAYF("%1 里要放字符串", key));
        out.push_back(v.get<std::string>());
    }
    return out;
}

ProjectStore open_project(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

/// 顿号分隔，对应 Python 的 "、".join(...)。
std::string join_cn(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += SAY("、");
        out += v[i];
    }
    return out;
}

}  // namespace

ApiResult post_shots_batch(const json& body) {
    // 批量改状态。shot_ids 为空表示整章。
    static const std::set<std::string> kActions = {
        "reset", "lock", "unlock", "clear_notes",
    };
    const std::string action = need_str(body, "action");
    if (kActions.count(action) == 0) {
        // 可选值按字典序列出，对应 Python 的 sorted(actions)
        std::vector<std::string> sorted_actions(kActions.begin(), kActions.end());
        throw ApiError(400, SAYF("不认识的操作 %1，可选：%2", action,
                                 join_cn(sorted_actions)));
    }

    ProjectStore store = open_project(body);
    const std::string episode_id = need_str(body, "episode_id");
    Project project = store.load_project();
    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));

    const std::vector<std::string> wanted = str_array(body, "shot_ids");
    const std::set<std::string> wanted_set(wanted.begin(), wanted.end());

    std::vector<Shot*> targets;
    if (wanted.empty()) {
        for (auto& s : ep->shots) targets.push_back(&s);
    } else {
        for (auto& s : ep->shots) {
            if (wanted_set.count(s.shot_id) != 0) targets.push_back(&s);
        }
        std::set<std::string> found;
        for (const Shot* s : targets) found.insert(s->shot_id);
        std::vector<std::string> missing;
        for (const auto& id : wanted_set) {
            if (found.count(id) == 0) missing.push_back(id);
        }
        if (!missing.empty()) {
            // set 已按字典序，对应 Python 的 sorted(missing)
            throw ApiError(404, SAYF("没有这些镜头：%1", join_cn(missing)));
        }
    }

    int changed = 0;
    for (Shot* shot : targets) {
        if (action == "reset") {
            // 锁定的镜头是人工确认过的，批量重置不该动它们，
            // 否则一次误操作就把已经审过的片全废了
            if (shot->status == ShotStatus::LOCKED) continue;
            shot->status = ShotStatus::PLANNED;
            shot->attempts = 0;
            shot->gate_notes.clear();
            ++changed;
        } else if (action == "lock") {
            if (shot->status != ShotStatus::LOCKED) {
                shot->status = ShotStatus::LOCKED;
                ++changed;
            }
        } else if (action == "unlock") {
            if (shot->status == ShotStatus::LOCKED) {
                shot->status = (shot->video_path.has_value() &&
                                !shot->video_path->empty())
                                   ? ShotStatus::FINAL_DONE
                                   : ShotStatus::PLANNED;
                ++changed;
            }
        } else if (action == "clear_notes") {
            if (!shot->gate_notes.empty()) {
                shot->gate_notes.clear();
                ++changed;
            }
        }
    }

    store.save_project(project);

    // 注意这个计数是**循环之后**算的，和 Python 一致。
    // reset 动作跳过了锁定的镜头，它们仍然是 LOCKED 所以数得到；
    // lock 动作虽然把所有镜头都变成 LOCKED，但条件里带了 action == reset，
    // 所以是 0。照抄这个顺序，别"顺手优化"成循环里累加。
    int skipped_locked = 0;
    if (action == "reset") {
        for (const Shot* s : targets) {
            if (s->status == ShotStatus::LOCKED) ++skipped_locked;
        }
    }

    return {200, {
        {"changed", changed},
        {"total", static_cast<int>(targets.size())},
        {"skipped_locked", skipped_locked},
    }};
}

ApiResult post_shots_reorder(const json& body) {
    // 按给定顺序重排镜头。
    //
    // 必须给出完整列表。只传"把 A 挪到第 3 位"这类增量指令的话，
    // 界面和引擎对当前顺序的理解一旦对不上，结果就是把成片剪乱，
    // 而且是那种要播一遍才发现的乱。
    //
    // 不重跑任何镜头：换顺序不改画面，已经渲染好的还能用。
    //
    // （原来这儿还有一句「转场是装配时按前后镜头算的，跟着新顺序自然就
    // 对了」。**装配根本不渲染转场**——拼接走的是 `-f concat -c copy`，
    // 全树一处 xfade / acrossfade 都没有，见 media/assemble.cpp 里那段。
    // transition_in / transition_dur_s 到今天为止只是记下来的意图。
    // 结论不变：换顺序确实不用重跑。）
    ProjectStore store = open_project(body);
    const std::string episode_id = need_str(body, "episode_id");
    Project project = store.load_project();
    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));

    const std::vector<std::string> wanted = str_array(body, "shot_ids");
    const std::set<std::string> wanted_set(wanted.begin(), wanted.end());
    if (wanted_set.size() != wanted.size()) {
        throw ApiError(400, SAY("顺序里有重复的镜头 id"));
    }

    std::set<std::string> current;
    for (const auto& s : ep->shots) current.insert(s.shot_id);

    if (wanted_set != current) {
        std::vector<std::string> missing, extra;
        for (const auto& id : current) {
            if (wanted_set.count(id) == 0) missing.push_back(id);
        }
        for (const auto& id : wanted_set) {
            if (current.count(id) == 0) extra.push_back(id);
        }
        std::string msg = SAY("顺序表和这一章的镜头对不上。");
        if (!missing.empty()) msg += SAYF("少了：%1。", join_cn(missing));
        if (!extra.empty()) msg += SAYF("多了：%1。", join_cn(extra));
        throw ApiError(400, msg);
    }

    std::map<std::string, int> rank;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        rank[wanted[i]] = static_cast<int>(i);
    }

    int moved = 0;
    for (const auto& s : ep->shots) {
        if (s.order != rank.at(s.shot_id)) ++moved;
    }
    for (auto& s : ep->shots) s.order = rank.at(s.shot_id);

    store.save_project(project);
    return {200, {{"moved", moved}, {"total", static_cast<int>(wanted.size())}}};
}

ApiResult post_shots_link_locations(const json& body) {
    // 把 location_id 空着的镜头接回场景。
    //
    // 老项目里的分镜多半只填了 scene_id。那样渲染时场景描述整段丢掉，
    // 跑出来的画面同一个房间每镜都不一样，而且不报任何错。
    ProjectStore store = open_project(body);
    const std::string episode_id =
        body.contains("episode_id") && body.at("episode_id").is_string()
            ? body.at("episode_id").get<std::string>()
            : std::string();

    Project project;
    AssetLibrary assets;
    try {
        project = store.load_project();
        assets = store.load_assets();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }

    json linked = json::object();
    int total = 0;
    for (auto& ep : project.episodes) {
        // 空章号表示整个项目都补
        if (!episode_id.empty() && ep.episode_id != episode_id) continue;
        int n = 0;
        for (auto& shot : ep.shots) {
            if (shot.location_id.has_value() && !shot.location_id->empty()) continue;
            if (assets.locations.count(shot.scene_id) != 0) {
                shot.location_id = shot.scene_id;
                ++n;
            }
        }
        if (n) {
            linked[ep.episode_id] = n;
            total += n;
        }
    }
    if (total) store.save_project(project);

    // 接上之后提示词才完整，已渲染的那些是按缺场景的提示词跑出来的。
    //
    // **只退真接过的那几章。** 这儿原来是 reset_all_shots——接一章的场景，
    // 整个项目已经渲染好的镜头全被退回待跑，而别的章的提示词一个字都没变
    // （见 reset.hpp 开头那条判据）。用户按的那颗按钮上写的是这一章，
    // 代价却是下一次「开始」把全片重跑几小时。`linked` 里装的正好是
    // "哪一章真接上了几镜"，没接上的章（n 为 0）也不该动。
    std::set<std::string> touched;
    for (const auto& kv : linked.items()) touched.insert(kv.key());
    const int reset = total ? reset_shots_in(store, touched) : 0;
    return {200, {{"linked", total}, {"episodes", linked}, {"reset_shots", reset}}};
}

}  // namespace changji::http
