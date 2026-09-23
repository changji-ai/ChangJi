// 工具 → 哪一格：**一个都不许漏**。
//
// 漏了的表现是消息末尾那颗「看看…」按钮不出现——而它不出现的时候界面上
// 什么都不说，人只会觉得"这次它没做出东西"。加一族新工具忘了分类，就是这么
// 坏的。
//
// 所以这条守卫**直接读 `agent/tools.cpp` 里每一个 `fn("…")`**，逐个问：
// 要么有格，要么明说没有。两样都不是就红。做法同 `test_enums.cpp`。

#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "util/tool_slot.hpp"

using changji::util::slot_of_tool;
using changji::util::tool_shows_nothing;

namespace {

/// `agent/tools.cpp` 里所有 `fn("名字"` 的名字。
std::set<std::string> tool_names() {
    // 读**编进去的那一份**：外层仓库有 agent/ 时是它，不是 src/agent 里的旧版。
    const std::string path = std::string(CHANGJI_AGENT_SRC_DIR) + "/tools.cpp";
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "读不到 " << path);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    std::set<std::string> out;
    const std::string head = "fn(\"";
    for (auto at = src.find(head); at != std::string::npos;
         at = src.find(head, at + head.size())) {
        const auto from = at + head.size();
        const auto close = src.find('"', from);
        if (close == std::string::npos) break;
        out.insert(src.substr(from, close - from));
    }
    return out;
}

}  // namespace

TEST_CASE("工具 → 哪一格：tools.cpp 里每一个都得有个说法") {
    const auto names = tool_names();
    // 一个都没抠出来就是这条用例自己不认路了（改了写法、挪了文件），
    // 不是"全都分好了"。
    REQUIRE_MESSAGE(names.size() > 15, "只抠出 " << names.size() << " 个工具名");

    for (const auto& n : names) {
        CAPTURE(n);
        const bool has_slot = !slot_of_tool(n).empty();
        const bool nothing = tool_shows_nothing(n);
        // 要么有格，要么明说没有。**两个都真不行**——那说明有人两处都填了，
        // 而按钮出不出得看哪一处先判，那种歧义留着迟早出事。
        CHECK_MESSAGE(has_slot != nothing,
                      n << " 既没有格也没明说没有（或者两处都填了）");
    }
}

TEST_CASE("工具 → 哪一格：格名只能是图标条上那五个") {
    const std::set<std::string> ok{"story", "assets", "script", "shots", "film"};
    for (const auto& n : tool_names()) {
        const std::string slot = slot_of_tool(n);
        if (slot.empty()) continue;
        CAPTURE(n);
        CAPTURE(slot);
        // 打错一个字（"shot" / "films"）的表现是按钮点了开出一格空面板。
        CHECK(ok.count(slot) == 1);
    }
}

TEST_CASE("工具 → 哪一格：认不出来的回空串，不瞎猜") {
    CHECK(slot_of_tool("完全没有这个工具").empty());
    CHECK(slot_of_tool("").empty());
    CHECK(!tool_shows_nothing("完全没有这个工具"));
}

TEST_CASE("工具 → 哪一格：几个真的") {
    CHECK(slot_of_tool("storyboard_plan_all") == "shots");
    CHECK(slot_of_tool("story_write_chapters") == "story");
    CHECK(slot_of_tool("refs_make") == "assets");
    CHECK(slot_of_tool("film_join") == "film");
    CHECK(slot_of_tool("script_write_all") == "script");
    // 建项目做不出"可看的东西"，按钮不该出现。
    CHECK(slot_of_tool("create_project").empty());
}

TEST_CASE("这一次动的是哪一章：正常那几档") {
    using changji::util::episode_of_args;
    CHECK(episode_of_args(R"({"episode_id":"ep03"})") == "ep03");
    CHECK(episode_of_args(R"({"path":"/x","episode_id":"ep01","n":3})") == "ep01");
    CHECK(episode_of_args(R"({"path":"/x"})").empty());
    CHECK(episode_of_args("").empty());
}

TEST_CASE("这一次动的是哪一章：args 是模型填的，坏成什么样都不许抛") {
    using changji::util::episode_of_args;
    // 这一条在工具跑完之后的收尾路上——抛出去就把一次成功的调用变成报错。
    CHECK(episode_of_args("不是 JSON").empty());
    CHECK(episode_of_args("[1,2,3]").empty());
    CHECK(episode_of_args("null").empty());
    CHECK(episode_of_args(R"({"episode_id":3})").empty());
    CHECK(episode_of_args(R"({"episode_id":null})").empty());
    CHECK(episode_of_args(R"({"episode_id":["ep01"]})").empty());
    CHECK(episode_of_args(R"({"episode_id":"ep0)").empty());
}

TEST_CASE("有格的工具：得说清是读还是改") {
    // 差别落在稿纸上：**改的那几个要把那一格锁住**（人在那儿打的字会被落回来
    // 的那一稿顶掉，而他看不出发生过什么），读的不锁。
    //
    // 新加一个"写"的工具忘了列进来，表现是**人打的字静悄悄没了**——
    // 所以这一条逐个问：有格的每一个，读还是改，得有个说法。
    for (const auto& n : tool_names()) {
        if (changji::util::slot_of_tool(n).empty()) continue;
        CAPTURE(n);
        const bool w = changji::util::tool_rewrites(n);
        // 名字里带 read 的一律是读；别的要么在改的名单里，要么得是 read。
        const bool looks_read = n.find("_read") != std::string::npos;
        CHECK_MESSAGE(w != looks_read,
                      n << " 既不在「会改」那张表里，名字也不是 *_read"
                           "——它到底改不改那一格？");
    }
}

TEST_CASE("会不会改：认不出的当不改") {
    CHECK_FALSE(changji::util::tool_rewrites("完全没有这个工具"));
    CHECK(changji::util::tool_rewrites("storyboard_plan_all"));
    CHECK_FALSE(changji::util::tool_rewrites("shots_read"));
}
