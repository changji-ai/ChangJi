// 图标字体：网页和桌面端共用一份（2026-09-27）。
//
// 用户：「找一个图标字体文件，桌面和 webapp 共用，使用同样的图标，包括其他的图标」。
// 名字只在 `brand/icons/icons.json` 里写一遍，`brand/icons/gen.py` 生成两张表
// （网页 `webapp/client/src/icons.gen.js`、桌面 `desktop/qml/IconFont.qml`）和两份字体。
//
// 钉的是会**静悄悄**坏的那几样：
//   · 有人手改了其中一张表、或者改了 icons.json 没重新生成——两边同一个名字画出两个形状；
//   · 界面上写了一个表里没有的名字——网页画成 info、桌面端什么都不画，都不报错；
//   · 字体文件没进版本库 / 没嵌进去——整排空白。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "desktop_sources.hpp"

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path{CHANGJI_SRC_DIR}.parent_path().parent_path(); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 一张表：名字 → `\uXXXX` 那四位。
std::map<std::string, std::string> table(const std::string& text, const std::regex& row) {
    std::map<std::string, std::string> out;
    for (std::sregex_iterator it(text.begin(), text.end(), row), end; it != end; ++it) {
        out[(*it)[1].str()] = (*it)[2].str();
    }
    return out;
}

std::set<std::string> json_names() {
    const std::string text = slurp(repo() / "brand" / "icons" / "icons.json");
    std::set<std::string> out;
    const std::regex row(R"re("([A-Za-z]\w*)"\s*:\s*")re");
    for (std::sregex_iterator it(text.begin(), text.end(), row), end; it != end; ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

}  // namespace

TEST_CASE("图标：网页和桌面端那两张表是同一份 icons.json 生成的") {
    if (!changji_test::desktop_sources()) return;
    const auto web = table(slurp(repo() / "webapp" / "client" / "src" / "icons.gen.js"),
                           // 不用 `^` + multiline：MSVC 的 <regex> 没有 multiline。
                           std::regex(R"re(\n  (\w+): '\\u([0-9a-f]{4})',)re"));
    const auto desk = table(slurp(changji_test::desktop_dir() / "qml" / "IconFont.qml"),
                            std::regex(R"re("(\w+)": "\\u([0-9a-f]{4})")re"));
    const auto names = json_names();
    REQUIRE(names.size() > 20);
    CHECK(web.size() == names.size());
    CHECK(desk.size() == names.size());
    for (const auto& n : names) {
        CAPTURE(n);
        REQUIRE_MESSAGE(web.count(n), "icons.json 改了没跑 python brand/icons/gen.py？");
        REQUIRE(desk.count(n));
        CHECK(web.at(n) == desk.at(n));
    }
    // 字体本身在：桌面端嵌 ttf，网页打包 woff2。
    CHECK(fs::file_size(repo() / "brand" / "icons" / "changji-icons.ttf") > 1000);
    CHECK(fs::file_size(repo() / "webapp" / "client" / "src" / "assets" / "changji-icons.woff2") > 1000);
    CHECK(fs::exists(repo() / "brand" / "icons" / "LICENSE-tabler.txt"));
    // 桌面端真把它嵌进去了。
    CHECK(slurp(changji_test::desktop_dir() / "CMakeLists.txt").find("changji-icons.ttf") !=
          std::string::npos);
}

TEST_CASE("图标：界面上写死的名字表里都有") {
    if (!changji_test::desktop_sources()) return;
    const auto names = json_names();
    // 网页：`<AppIcon name="x"` 和 `<AppIcon ... name="x"`（写死的那些；拼出来的查不了）。
    const std::regex web_use(R"re(<AppIcon\b[^>]*?\sname="(\w+)")re");
    std::set<std::string> alias = {"chevron_down"};   // AppIcon 里那张老名字表
    for (const auto& e : fs::recursive_directory_iterator(repo() / "webapp" / "client" / "src")) {
        if (!e.is_regular_file() || e.path().extension() != ".vue") continue;
        const std::string text = slurp(e.path());
        for (std::sregex_iterator it(text.begin(), text.end(), web_use), end; it != end; ++it) {
            const std::string n = (*it)[1].str();
            CAPTURE(e.path().filename().string());
            CAPTURE(n);
            CHECK((names.count(n) || alias.count(n)));
        }
    }
    // 桌面端：`slotKey: "x"`、`icon: "x"`（sense 那一颗是自己画的，不走字体）。
    const std::regex desk_use(R"re((?:slotKey|icon):\s*"(\w+)")re");
    for (const auto& e : fs::directory_iterator(changji_test::desktop_dir() / "qml")) {
        if (!e.is_regular_file() || e.path().extension() != ".qml") continue;
        const std::string text = slurp(e.path());
        for (std::sregex_iterator it(text.begin(), text.end(), desk_use), end; it != end; ++it) {
            const std::string n = (*it)[1].str();
            if (n == "sense") continue;
            CAPTURE(e.path().filename().string());
            CAPTURE(n);
            CHECK(names.count(n));
        }
    }
}
