// 界面上那张表关掉的那些格子，存在 `<项目库>/nodes.json`。
//
// 要钉死的是**写砸了不能把上一份弄丢**。这一处的意义就在这儿：header 上
// 写着「写不进去抛 runtime_error——界面上点了开关要么生效、要么当场说没
// 生效，不能默默回到原样」。而"默默回到原样"最容易发生的地方不是写，是
// 替换：原来那一步是 `fs::remove(p)` 再 `fs::rename(tmp, p)`，rename 一旦
// 失败，**用户关掉的那些格子就全没了**——下次读回来是"一个都没关"，一台
// 特意不让它出片的机器又开始接活。
//
// `fs::rename` 在标准里就要求目标存在时替换掉它（项目存盘那条走的就是
// 这个，models/project.cpp 写着理由），先删那一下从来就不必要。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "infer/node_prefs.hpp"
#include "util/paths.hpp"

using namespace changji;
using namespace changji::infer;
namespace fs = std::filesystem;

namespace {

/// 一个干净的项目库目录。
fs::path fresh(const std::string& tag) {
    const auto root = fs::temp_directory_path() / "changji_node_prefs_test" / tag;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

}  // namespace

TEST_CASE("存下去读回来，是同一份") {
    const auto ws = fresh("roundtrip");
    NodePrefs prefs;
    prefs["local"] = {Capability::Video};
    prefs["http://gpu-box:9001"] = {Capability::Llm, Capability::Tts};

    save_node_prefs(ws, prefs);
    const auto back = load_node_prefs(ws);

    CHECK(back.size() == 2);
    CHECK(back.at("local").count(Capability::Video) == 1);
    CHECK(back.at("http://gpu-box:9001").size() == 2);
    CHECK(back.at("http://gpu-box:9001").count(Capability::Llm) == 1);
}

TEST_CASE("一个都没关的那台不占一行") {
    // 留一行空数组只是噪音，而这个文件是人会去看的。
    const auto ws = fresh("empty");
    NodePrefs prefs;
    prefs["local"] = {};
    prefs["http://a:1"] = {Capability::Frame};
    save_node_prefs(ws, prefs);

    const auto back = load_node_prefs(ws);
    CHECK(back.count("local") == 0);
    CHECK(back.count("http://a:1") == 1);
}

TEST_CASE("读不出来一律当成「没关任何东西」") {
    // **这个默认值要挑不改变行为的那边**（header 上写着理由）：读坏了就
    // 全关的话，表现是整条流水线突然没机器可派，而人完全不知道是一个
    // JSON 坏了。
    const auto ws = fresh("broken");

    SUBCASE("文件根本不在") {
        CHECK(load_node_prefs(ws).empty());
    }

    SUBCASE("半截 JSON") {
        std::ofstream(node_prefs_path(ws)) << R"({"off": {"local": ["vi)";
        CHECK(load_node_prefs(ws).empty());
    }

    SUBCASE("认不出的能力名") {
        std::ofstream(node_prefs_path(ws))
            << R"({"off": {"local": ["video", "煮咖啡"]}})";
        const auto back = load_node_prefs(ws);
        // 认得出的那个留着，认不出的那个丢掉——**不是整台作废**
        CHECK(back.count("local") == 1);
        CHECK(back.at("local").count(Capability::Video) == 1);
        CHECK(back.at("local").size() == 1);
    }
}

TEST_CASE("盖掉上一份：写第二遍不留上一遍的残渣") {
    const auto ws = fresh("overwrite");
    NodePrefs first;
    first["local"] = {Capability::Video, Capability::Frame};
    save_node_prefs(ws, first);

    NodePrefs second;
    second["local"] = {Capability::Tts};
    save_node_prefs(ws, second);

    const auto back = load_node_prefs(ws);
    REQUIRE(back.count("local") == 1);
    CHECK(back.at("local").size() == 1);
    CHECK(back.at("local").count(Capability::Tts) == 1);
    CHECK(back.at("local").count(Capability::Video) == 0);

    // ⚠️ **上一份在整个过程中不许消失。** 原来那一版先 `fs::remove(p)`
    // 再 rename，rename 挂了的话盘上一份都不剩。这儿断言的是替换完之后
    // 只有正主、没有那个 .part——半成品留着的话，人打开项目库看见一个
    // `nodes.json.part` 只会更糊涂。
    CHECK(fs::exists(node_prefs_path(ws)));
    CHECK_FALSE(fs::exists(ws / "nodes.json.part"));
}

TEST_CASE("写不进去要抛，不能默默回到原样") {
    // 界面上点了开关要么生效、要么当场说没生效（header 上那句）。
    // 拿一个存在的**文件**当项目库：临时文件建不出来。
    const auto ws = fresh("nodir") / paths::from_utf8("这是个文件不是目录");
    std::ofstream(ws) << "x";
    REQUIRE(fs::is_regular_file(ws));

    NodePrefs prefs;
    prefs["local"] = {Capability::Video};
    CHECK_THROWS_AS(save_node_prefs(ws, prefs), std::runtime_error);
}
