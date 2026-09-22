// 「挑一个文件夹」那条接口。给设置页那个模型目录用的。
//
// 敲路径这件事本来就不该靠记：模型动辄几十 GB，人挑的是"哪块盘还装得下"，
// 而那个路径多半在另一个窗口里。敲错一个字的后果不是报错，是下载落到一个
// 你没在看的地方。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <algorithm>

#include "config/settings.hpp"
#include "util/paths.hpp"
#include "http/readonly.hpp"

using namespace changji;
using namespace changji::http;
namespace fs = std::filesystem;

namespace {

fs::path fresh(const std::string& tag) {
    const auto root = fs::temp_directory_path() / "changji_fs_dirs" / tag;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

std::vector<std::string> names(const nlohmann::json& arr) {
    std::vector<std::string> out;
    for (const auto& e : arr) out.push_back(e.at("name").get<std::string>());
    return out;
}

}  // namespace

TEST_CASE("只列目录，不列文件") {
    // **这一条是这个接口的全部意义**：它唯一的用处是选一个放模型的地方，
    // 把几百个 .gguf 一起列出来只会把目录淹掉。
    const auto root = fresh("onlydirs");
    std::error_code ec;
    fs::create_directories(root / "models", ec);
    fs::create_directories(root / "另一块盘", ec);
    std::ofstream(root / "readme.txt") << "x";
    std::ofstream(root / "wan2.2.gguf") << "x";

    config::Settings s;
    const auto r = get_dirs(paths::to_utf8(root), s);
    REQUIRE(r.status == 200);
    const auto got = names(r.body.at("entries"));
    CHECK(got.size() == 2);
    CHECK(std::find(got.begin(), got.end(), "models") != got.end());
    CHECK(std::find(got.begin(), got.end(), "另一块盘") != got.end());
}

TEST_CASE("点开头的不列") {
    // `.git` `.venv` 这些在模型目录旁边到处都是，而没有人会把模型放进去。
    const auto root = fresh("dotted");
    std::error_code ec;
    fs::create_directories(root / ".git", ec);
    fs::create_directories(root / ".cache", ec);
    fs::create_directories(root / "weights", ec);

    config::Settings s;
    const auto r = get_dirs(paths::to_utf8(root), s);
    CHECK(names(r.body.at("entries")) == std::vector<std::string>{"weights"});
}

TEST_CASE("到根了就没有上一级") {
    // **别回一个指向自己的 parent**：界面上那个「上一级」会变成点了没反应。
    config::Settings s;
    const auto r = get_dirs("/", s);
    REQUIRE(r.status == 200);
    CHECK(r.body.at("parent").is_null());
}

TEST_CASE("有上一级的时候要给出来") {
    const auto root = fresh("hasparent");
    std::error_code ec;
    fs::create_directories(root / "inner", ec);

    config::Settings s;
    const auto r = get_dirs(paths::to_utf8(root / "inner"), s);
    REQUIRE(r.body.at("parent").is_string());
    CHECK(r.body.at("parent").get<std::string>() == paths::to_utf8(root));
}

TEST_CASE("路径为空：给几个起点，不是报错") {
    // **空手起步是这条接口存在的理由**——人要是知道路径，他就直接敲进那个
    // 框了。这时候回一个 400 等于把浏览器关在门外。
    config::Settings s;
    const auto r = get_dirs("", s);
    REQUIRE(r.status == 200);
    CHECK(r.body.at("path").get<std::string>().empty());
    CHECK(r.body.at("parent").is_null());
    CHECK(r.body.at("entries").empty());
    // 起点至少有用户目录那一条
    CHECK_FALSE(r.body.at("roots").empty());
    bool has_why = false;
    for (const auto& e : r.body.at("roots")) {
        if (e.contains("why")) has_why = true;
    }
    CHECK(has_why);
}

TEST_CASE("「不在」和「不是目录」要分开说") {
    // 前者多半是敲错了，后者是选了一个文件——两种要做的事不一样。
    const auto root = fresh("bad");
    std::ofstream(root / "a.json") << "{}";
    config::Settings s;

    CHECK_THROWS_AS(get_dirs(paths::to_utf8(root / "nope"), s), ApiError);
    CHECK_THROWS_AS(get_dirs(paths::to_utf8(root / "a.json"), s), ApiError);

    try {
        get_dirs(paths::to_utf8(root / "nope"), s);
    } catch (const ApiError& e) {
        CHECK(std::string(e.what()).find("不在") != std::string::npos);
    }
    try {
        get_dirs(paths::to_utf8(root / "a.json"), s);
    } catch (const ApiError& e) {
        CHECK(std::string(e.what()).find("不是一个目录") != std::string::npos);
    }
}
