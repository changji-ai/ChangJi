// `assets.json` 形状不对的时候，**不能读成"这个项目还没有角色"**。
//
// `characters` / `locations` 是 id → 内容 的对象。反序列化用的是
// `NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT`——类型对不上时它不报错，
// 回默认值，也就是一个空库。于是一份手改歪了的文件读出来和新项目一模一样：
// 界面显示「还没有角色」，而文件里其实有十几个角色、几十条服装、一堆参考图
// 路径。人看不出差别，接着点「照故事定妆」，merge_bible 就把它盖掉了。
//
// 手改这个文件是**写在文档里的用法**（character.cpp：「唯一的办法是手改
// assets.json」），所以这一处必须说话。
//
// 真引擎上撞到的：往 assets.json 里写了一份 `characters` 是**数组**的库，
// /api/assets 回的是 `{"characters":[],"locations":[]}`，整页显示「还没有
// 角色」，一个字都没说文件有问题。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "http/readonly.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using json = nlohmann::json;
using namespace changji;
using namespace changji::http;
using namespace changji::models;
namespace fs = std::filesystem;

namespace {

/// 建一个空项目，回它的 store。
ProjectStore fresh(const std::string& tag) {
    const auto root = fs::temp_directory_path() / "changji_assets_shape" / tag;
    std::error_code ec;
    fs::remove_all(root, ec);
    return ProjectStore::create(root, "t_" + tag, tag, StyleLine::REALISTIC);
}

void write_assets(const ProjectStore& store, const std::string& body) {
    std::ofstream(store.paths().assets_file()) << body;
}

}  // namespace

TEST_CASE("对象形状：照常读出来") {
    auto store = fresh("ok");
    write_assets(store, R"({
      "characters": {"c_lin": {"char_id": "c_lin", "name": "林晚"}},
      "locations":  {"loc_t": {"location_id": "loc_t", "name": "天台"}}
    })");

    const AssetLibrary lib = store.load_assets();
    REQUIRE(lib.characters.size() == 1);
    CHECK(lib.characters.at("c_lin").name == "林晚");
    CHECK(lib.locations.size() == 1);
}

TEST_CASE("写成数组要抛，不能悄悄变成空库") {
    // ⚠️ 这一条是要害。抛出去之后，上层 `load_assets_or_400` 那条本来就是
    // 为"文件本身是坏的"准备的——人看到的是一句话，不是一个空页面。
    auto store = fresh("array");
    write_assets(store, R"({
      "characters": [{"char_id": "c_lin", "name": "林晚"}],
      "locations":  {}
    })");

    CHECK_THROWS_AS(store.load_assets(), std::runtime_error);

    SUBCASE("那句话得说清是哪个键、现在是什么、以及别在这个状态下重新定妆") {
        try {
            store.load_assets();
            FAIL("没抛");
        } catch (const std::runtime_error& e) {
            const std::string msg = e.what();
            CHECK(msg.find("characters") != std::string::npos);
            CHECK(msg.find("array") != std::string::npos);
            CHECK(msg.find("盖掉") != std::string::npos);
        }
    }
}

TEST_CASE("locations 写歪了同样要抛") {
    auto store = fresh("loc");
    write_assets(store, R"({"characters": {}, "locations": "天台"})");
    CHECK_THROWS_AS(store.load_assets(), std::runtime_error);
}

TEST_CASE("这两个键缺了、或者是 null，都还算「这个项目还没有角色」") {
    // **别把"没有"也算成坏**：新项目、以及只写了画风的库，走的正是这条。
    auto store = fresh("absent");

    SUBCASE("整个键都没有") {
        write_assets(store, R"({"style": {"style_line": "realistic"}})");
        CHECK(store.load_assets().characters.empty());
    }
    SUBCASE("是 null") {
        write_assets(store, R"({"characters": null, "locations": null})");
        CHECK(store.load_assets().characters.empty());
    }
    SUBCASE("文件根本不在") {
        std::error_code ec;
        fs::remove(store.paths().assets_file(), ec);
        CHECK(store.load_assets().characters.empty());
    }
}

// ---- 「有图 / 缺图」判的是文件，不是路径那一栏 ----
//
// 这一条拦的是一个**一声不响**的坏法。`assets_read`（代理那个工具）原来读
// 的是 `reference` —— 仓库里除了那三行再没有第二处提到这个字段，也就是说
// 它永远是空的，于是那句摘要对每一个角色都写「(缺图)」，图在不在都一样。
// 模型照着它去重出已经有的图，人看不出哪儿不对。2026-09-21 做设定那一格时
// 撞见。
//
// 换成引擎算好的 `has_ref` 之后，这一条钉住它**真的在看盘上那个文件**：
// 路径填着而文件不在 → 缺图；文件真摆进去 → 有图。
TEST_CASE("设定：有没有参考图，判的是文件在不在，不是路径填没填") {
    auto store = fresh("hasref");
    write_assets(store, R"({
      "characters": {"c_lin": {"char_id": "c_lin", "name": "林晚",
                               "ref_front": "refs/c_lin_front.png"}},
      "locations":  {"loc_t": {"location_id": "loc_t", "name": "天台",
                               "ref_empty": "refs/loc_t_empty.png"}}
    })");

    const auto one = [](const json& r, const char* group) {
        REQUIRE(r.contains(group));
        REQUIRE(r.at(group).size() == 1);
        return r.at(group).at(0).value("has_ref", true);
    };

    // 路径填着，文件还没出。**这时候必须是"缺图"**——原来那一版这儿也回
    // false，但它是因为读了个不存在的字段，换成真图之后照样回 false。
    {
        const auto r = http::get_assets(paths::to_utf8(store.root()));
        REQUIRE(r.status == 200);
        CHECK(one(r.body, "characters") == false);
        CHECK(one(r.body, "locations") == false);
    }

    // 图真摆进去。
    fs::create_directories(store.root() / "refs");
    std::ofstream(store.root() / "refs" / "c_lin_front.png") << "not really a png";
    std::ofstream(store.root() / "refs" / "loc_t_empty.png") << "not really a png";
    {
        const auto r = http::get_assets(paths::to_utf8(store.root()));
        REQUIRE(r.status == 200);
        CHECK(one(r.body, "characters") == true);
        CHECK(one(r.body, "locations") == true);
    }

    // 路径那一栏空着的，一律缺图——别去 stat 一个空路径。
    write_assets(store, R"({
      "characters": {"c_lin": {"char_id": "c_lin", "name": "林晚"}},
      "locations":  {}
    })");
    {
        const auto r = http::get_assets(paths::to_utf8(store.root()));
        REQUIRE(r.status == 200);
        CHECK(one(r.body, "characters") == false);
    }
}

// ---- 随便指一个目录，`/api/assets` 也得说"这不是项目" ----
//
// 三个接口原来对"什么算项目"说的不是同一句话：`/api/project` 和 `/api/shots`
// 紧接着会 `load_project()`，路径不对就 400；而 `/api/assets` 只
// `load_assets()`——文件不在就回一个空库，**200 加「没有角色、没有场景」**。
//
// 后果在界面上：桌面端设定那一格看到 200 + 空库，就照实写「还没有人物和场景。
// 说一句「把人物和场景提出来」就行」，而那个目录根本不是项目。**空的和坏的
// 长得一样**，还顺嘴教人往里加东西。
TEST_CASE("设定：随便一个目录也得回「这不是项目」，不是一个空库") {
    const auto dir = fs::temp_directory_path() / "changji_notaproject_assets";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const std::string p = paths::to_utf8(dir);

    // ⚠️ **接 `std::exception`，不是 `ApiError`。** `load_project()` 抛的是
    // `std::runtime_error`，HTTP 那一层的 `guard` 才把它转成 400 + detail。
    // 只接 ApiError 的话用例自己会被那个异常打断（第一版就是这么红的）。
    const auto why = [](auto&& fn) {
        try {
            fn();
            return std::string("（没抛）");
        } catch (const std::exception& e) {
            // `ApiError` 也是 `std::runtime_error`，它的 what() 就是那句
            // detail（见 readonly.hpp 里的构造函数）。所以一支就够，
            // 不用分开接——第一版分开接，还在 ApiError 上调了 `.value()`，
            // 而 detail 这时候是个字符串不是对象，当场又抛一次。
            return std::string(e.what());
        }
    };
    const std::string from_assets = why([&] { get_assets(p); });
    const std::string from_project = why([&] { get_project(p); });

    CAPTURE(from_assets);
    CHECK(!from_assets.empty());
    CHECK(from_assets.find("不是一个项目目录") != std::string::npos);
    // **400，不是 500。** 让原始异常漏出去的话 `guard` 会按"没预料到的"
    // 处理：500 加一句「服务端出错：」——而路径填错不是服务端的错
    //（实测过：改之前 `/api/assets` 回 500、`/api/project` 回 400）。
    try {
        get_assets(p);
        FAIL("该抛");
    } catch (const ApiError& e) {
        CHECK(e.status() == 400);
    }
    // **一处写，三处用**：别各写一句意思差不多的。
    CHECK(from_assets == from_project);

    fs::remove_all(dir, ec);
}
