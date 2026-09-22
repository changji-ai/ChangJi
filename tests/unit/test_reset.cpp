// reset_all_shots：改了影响提示词的东西之后，把已渲染的镜头退回待跑。
//
// **这个函数原来一个测试都没有**，而它有九个调用点（upload.cpp 四处、
// editing_assets.cpp 三处、editing_batch.cpp、planning.cpp），
// 并且**是破坏性的**——它把用户已经渲染出来的结果作废掉。
//
// 上层那些接口的测试只比对响应里的 reset_shots 数字，比不到
// "哪些镜头被动了、哪些没被动"。数字对而规则错的改法是存在的：
// 比如把 LOCKED 也重置，计数照样对得上，用户锁定的镜头却没了。
//
// 语料里的项目只有两个镜头（audio_done 和 planned），九个状态里
// 覆盖不到七个。所以这里在语料的副本上把镜头铺开成九个状态各一个。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "http/reset.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using namespace changji;
using namespace changji::models;
namespace fs = std::filesystem;

namespace {

// 九个状态，顺序和 shot.hpp 里的 enum 一致。加了新状态而这里没加，
// 下面那条计数断言会失败——这是故意的，新状态要显式想一遍该不该重置。
const std::vector<ShotStatus> kAllStatuses = {
    ShotStatus::PLANNED,        ShotStatus::AUDIO_DONE,
    ShotStatus::FRAME_DONE,     ShotStatus::DRAFT_DONE,
    ShotStatus::DRAFT_REJECTED, ShotStatus::FINAL_DONE,
    ShotStatus::FINAL_REJECTED, ShotStatus::FALLBACK,
    ShotStatus::LOCKED};

fs::path pristine() {
    return paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           paths::from_utf8("项目_雨夜天台");
}

/// 复制一份语料项目，把 ep01 铺成九个镜头（九个状态各一个），
/// attempts 和 gate_notes 都填上非空值。ep02 保持一个镜头都没有，不动。
ProjectStore staged(const std::string& tag) {
    const fs::path dst = fs::temp_directory_path() /
                         paths::from_utf8("changji_重置_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制语料项目失败：" << ec.message());

    ProjectStore store(dst);
    Project project = store.load_project();
    REQUIRE(project.episodes.size() >= 2);
    REQUIRE_FALSE(project.episodes[0].shots.empty());

    const Shot proto = project.episodes[0].shots[0];
    project.episodes[0].shots.clear();
    for (std::size_t i = 0; i < kAllStatuses.size(); ++i) {
        Shot s = proto;
        s.shot_id = "ep01_s01_sh" + std::string(1, static_cast<char>('1' + i));
        s.order = static_cast<int>(i);
        s.status = kAllStatuses[i];
        s.attempts = 3;
        s.gate_notes = {"上一轮的意见"};
        project.episodes[0].shots.push_back(s);
    }
    store.save_project(project);
    return store;
}

const Shot& shot_with(const Project& p, ShotStatus original) {
    for (std::size_t i = 0; i < kAllStatuses.size(); ++i) {
        if (kAllStatuses[i] == original) return p.episodes[0].shots[i];
    }
    FAIL("找不到镜头");
    return p.episodes[0].shots[0];
}

}  // namespace

TEST_CASE("九个状态里只有七个被退回，LOCKED 和 PLANNED 不动") {
    ProjectStore store = staged("九态");
    const int n = http::reset_all_shots(store);

    // 九个状态减去 PLANNED（本来就是待跑）和 LOCKED（人工锁定）。
    CHECK(n == 7);

    // **从盘上重新读**，不是看内存里那份——reset_all_shots 是加载、
    // 改副本、存盘。少了最后一步的话，返回值照样对，而调用方
    // 下一次读项目时看到的还是旧状态。
    const Project after = ProjectStore(store.root()).load_project();
    REQUIRE(after.episodes[0].shots.size() == kAllStatuses.size());

    for (std::size_t i = 0; i < kAllStatuses.size(); ++i) {
        const Shot& s = after.episodes[0].shots[i];
        CAPTURE(i);
        if (kAllStatuses[i] == ShotStatus::LOCKED) {
            CHECK(s.status == ShotStatus::LOCKED);
        } else {
            CHECK(s.status == ShotStatus::PLANNED);
        }
    }
}

TEST_CASE("被退回的镜头，重试次数归零、上一轮的闸门意见清空") {
    ProjectStore store = staged("清field");
    http::reset_all_shots(store);
    const Project after = ProjectStore(store.root()).load_project();

    const Shot& redone = shot_with(after, ShotStatus::FINAL_DONE);
    // attempts 不归零的话，重试次数会从旧值接着数，
    // 第一次重跑就可能直接判超限、降级。
    CHECK(redone.attempts == 0);
    // gate_notes 是上一轮画面的意见。新画面还没出，留着会让
    // 界面上显示一条对不上的评语。
    CHECK(redone.gate_notes.empty());
}

TEST_CASE("锁定的镜头连 attempts 和 gate_notes 都不许动") {
    // 这条在头文件注释里没写，但是实际行为：`continue` 在清字段之前，
    // 所以 LOCKED 是**整个跳过**，不是"状态保留但字段清掉"。
    //
    // 单拎出来钉是因为它容易在重构时改掉——把清字段的三行提到
    // if 前面看着更整洁，行为却变了：用户锁定一镜正是想保住它的
    // 全部现状，包括"这镜重试过几次""闸门当初说了什么"。
    ProjectStore store = staged("锁定");
    http::reset_all_shots(store);
    const Project after = ProjectStore(store.root()).load_project();

    const Shot& locked = shot_with(after, ShotStatus::LOCKED);
    CHECK(locked.status == ShotStatus::LOCKED);
    CHECK(locked.attempts == 3);
    REQUIRE(locked.gate_notes.size() == 1);
    CHECK(locked.gate_notes[0] == "上一轮的意见");
}

TEST_CASE("本来就待跑的镜头不计数，免得给用户报一个虚高的数字") {
    // 返回值直接进响应体的 reset_shots，界面拿它提示"重置了 N 个镜头"。
    // 把 PLANNED 算进去的话，一个刚建好、一镜没跑的项目改一次设定
    // 会提示"重置了 40 个镜头"，而实际上什么都没重置。
    ProjectStore store = staged("待跑");
    Project p = store.load_project();
    for (auto& s : p.episodes[0].shots) s.status = ShotStatus::PLANNED;
    store.save_project(p);

    CHECK(http::reset_all_shots(store) == 0);
}

TEST_CASE("再点一次重置，返回 0") {
    // 幂等。界面上改一次设定弹一次提示，用户手抖点两下第二次应该
    // 说"没有需要重置的"，而不是又报一遍同样的数字。
    ProjectStore store = staged("幂等");
    CHECK(http::reset_all_shots(store) == 7);
    CHECK(http::reset_all_shots(store) == 0);
}

TEST_CASE("空章不出事") {
    // 语料里 ep02 一个镜头都没有。新建的项目、或者刚加还没出分镜的一章
    // 都是这样，而九个调用点里没有一个先查过那一章里有没有镜头。
    ProjectStore store = staged("空集");
    const Project before = store.load_project();
    REQUIRE(before.episodes[1].shots.empty());

    CHECK(http::reset_all_shots(store) == 7);   // 只数 ep01 的
    const Project after = ProjectStore(store.root()).load_project();
    CHECK(after.episodes[1].shots.empty());
}
