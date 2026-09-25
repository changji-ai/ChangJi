#include "http/reset.hpp"

namespace changji::http {

using namespace changji::models;

namespace {

/// 一章里的那两条规则。**只此一份**——头文件上写着三份拷贝迟早分叉，
/// 现在两个入口（整个项目 / 指定几章）共用它，规则只能改在一个地方。
bool reset_one(Shot& shot) {
    if (shot.status == ShotStatus::PLANNED || shot.status == ShotStatus::LOCKED) {
        return false;
    }
    shot.status = ShotStatus::PLANNED;
    shot.attempts = 0;
    shot.gate_notes.clear();
    return true;
}

int reset_episode(Episode& ep) {
    int n = 0;
    for (auto& shot : ep.shots) n += reset_one(shot) ? 1 : 0;
    return n;
}

/// 挑着退：`hit` 说这一镜算不算。规则还是 reset_one 那一份。
template <typename Hit>
int reset_where(const ProjectStore& store, Hit hit) {
    const auto store_guard = store.lock();   // 读→改→存一把锁
    Project project = store.load_project();
    int n = 0;
    for (auto& ep : project.episodes) {
        for (auto& shot : ep.shots) {
            if (hit(shot) && reset_one(shot)) ++n;
        }
    }
    if (n > 0) store.save_project(project);
    return n;
}

}  // namespace

int reset_shots_with_character(const ProjectStore& store, const std::string& char_id) {
    return reset_where(store, [&](const Shot& s) {
        for (const auto& c : s.characters) {
            if (c.char_id == char_id) return true;
        }
        return false;
    });
}

int reset_shots_at_location(const ProjectStore& store, const std::string& location_id) {
    return reset_where(store, [&](const Shot& s) {
        return s.location_id.has_value() && *s.location_id == location_id;
    });
}

int reset_all_shots(const ProjectStore& store) {
    const auto store_guard = store.lock();   // 读→改→存一把锁（ProjectStore::lock）
    Project project = store.load_project();
    int n = 0;
    for (auto& ep : project.episodes) n += reset_episode(ep);
    store.save_project(project);
    return n;
}

int reset_shots_in(const ProjectStore& store,
                   const std::set<std::string>& episode_ids) {
    const auto store_guard = store.lock();   // 读→改→存一把锁
    Project project = store.load_project();
    int n = 0;
    for (auto& ep : project.episodes) {
        if (episode_ids.count(ep.episode_id) == 0) continue;
        n += reset_episode(ep);
    }
    store.save_project(project);
    return n;
}

}  // namespace changji::http
