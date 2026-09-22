#include "http/reset.hpp"

namespace changji::http {

using namespace changji::models;

namespace {

/// 一章里的那两条规则。**只此一份**——头文件上写着三份拷贝迟早分叉，
/// 现在两个入口（整个项目 / 指定几章）共用它，规则只能改在一个地方。
int reset_episode(Episode& ep) {
    int n = 0;
    for (auto& shot : ep.shots) {
        if (shot.status == ShotStatus::PLANNED ||
            shot.status == ShotStatus::LOCKED) {
            continue;
        }
        shot.status = ShotStatus::PLANNED;
        shot.attempts = 0;
        shot.gate_notes.clear();
        ++n;
    }
    return n;
}

}  // namespace

int reset_all_shots(const ProjectStore& store) {
    Project project = store.load_project();
    int n = 0;
    for (auto& ep : project.episodes) n += reset_episode(ep);
    store.save_project(project);
    return n;
}

int reset_shots_in(const ProjectStore& store,
                   const std::set<std::string>& episode_ids) {
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
