// 账本上那一行动的是哪一格：**一个 kind 都不许漏**。
//
// 漏了的表现是面板上那句「场记正在动这一格」不出现——而它不出现的时候界面
// 什么都不说，人正盯着一份马上要被顶掉的稿子。加一族新活忘了分类，就是这么
// 坏的。
//
// 所以这条守卫**直接读 `cpp/src` 里每一个起任务的地方**，把 kind 抠出来
// 逐个问：要么有格，要么明说没有。做法同 `test_enums.cpp` / `test_tool_slot.cpp`。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "util/task_slot.hpp"

namespace fs = std::filesystem;
using changji::util::slot_of_task_kind;
using changji::util::task_kind_has_no_slot;

namespace {

/// `cpp/src` 里所有 `Activity{"…"` / `Activity(` / `Task>("…"` 的第一个字符串。
std::set<std::string> task_kinds() {
    std::set<std::string> out;
    const fs::path root{CHANGJI_SRC_DIR};
    // `Activity` 或 `Task` 后面四十个字符以内的第一个小写字符串。
    // 只认**真在起一件活**的地方：`Activity act{"…"` / `Activity act("…"` /
    // `make_shared<pipeline::Task>("…"`。松一点的话会把 `TaskState` 那几个
    // 状态词（done / failed / queued…）也抠进来——它们不是 kind。
    const std::regex re(
        R"RX((?:Task>|Activity)\s*(?:[A-Za-z_][A-Za-z0-9_]*\s*)?[({][^;]{0,40}?"([a-z_]+)")RX");
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension();
        if (ext != ".cpp" && ext != ".hpp") continue;
        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string src = ss.str();
        for (std::sregex_iterator it(src.begin(), src.end(), re), last;
             it != last; ++it) {
            out.insert((*it)[1].str());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("账本 → 哪一格：起任务用过的每个 kind 都得有个说法") {
    const auto kinds = task_kinds();
    // 一个都没抠出来就是这条用例自己不认路了（改了写法、挪了目录），
    // 不是"全都分好了"。
    REQUIRE_MESSAGE(kinds.size() > 8, "只抠出 " << kinds.size() << " 个 kind");

    for (const auto& k : kinds) {
        CAPTURE(k);
        const bool has = !slot_of_task_kind(k).empty();
        const bool none = task_kind_has_no_slot(k);
        CHECK_MESSAGE(has != none,
                      k << " 既没有格也没明说没有（或者两处都填了）——"
                           "它动的是界面上哪一格？");
    }
}

TEST_CASE("账本 → 哪一格：格名只能是图标条上那五个") {
    const std::set<std::string> ok{"story", "assets", "script", "shots", "film"};
    for (const auto& k : task_kinds()) {
        const std::string slot = slot_of_task_kind(k);
        if (slot.empty()) continue;
        CAPTURE(k);
        CAPTURE(slot);
        CHECK(ok.count(slot) == 1);
    }
}

TEST_CASE("账本 → 哪一格：image 不猜，由起任务那头自己说") {
    // 参考图在设定那一格、出首帧在镜头那一格，**同一个 kind**。
    // 猜一个的话总有一半是错的，而按错了的那一句比没有更糟。
    CHECK(slot_of_task_kind("image").empty());
    CHECK(task_kind_has_no_slot("image"));
    // 几个真的
    CHECK(slot_of_task_kind("write_one") == "story");
    CHECK(slot_of_task_kind("plan") == "shots");
    CHECK(slot_of_task_kind("trailer") == "film");
    CHECK(slot_of_task_kind("understand") == "assets");
    CHECK(slot_of_task_kind("完全没有这个 kind").empty());
}
