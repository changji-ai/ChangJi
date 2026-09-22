// 枚举值 → 中文那张表：**一个值都不许漏**。
//
// 漏了的表现是界面上冒出一个 `pan_left`——中文界面上一个英文枚举名，而且
// 全程不报错。加一个枚举值忘了给中文，就是这么坏的。
//
// 所以这条守卫**直接读 `models/shot.hpp` 里那几个
// `NLOHMANN_JSON_SERIALIZE_ENUM` 块**，把里面的字符串全抠出来，逐个去表里
// 找。加一个枚举值、不给中文，这条当场红。
//
// 做法是从 `webapp/client/src/api/enum-labels.test.js` 抄来的——那条用例读
// 的也是 C++ 源码（CLAUDE.md 第八条：收不动就让它会响）。区别只是这一条不
// 跨语言，读的是同一个仓库里隔壁那个头文件。

#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "http/enums.hpp"
#include "models/shot.hpp"

using json = nlohmann::json;
using namespace changji;

namespace {

/// 从 `models/shot.hpp` 里那个 SERIALIZE_ENUM 块里，把引号里的字符串全抠出来。
///
/// **只认第一个块**：同一个枚举名不会出现两次；找不到就 FAIL，不要静悄悄
/// 回一个空集合——空集合会让下面每一条断言都"通过"。
std::set<std::string> enum_values(const std::string& enum_name) {
    const std::string path = std::string(CHANGJI_SRC_DIR) + "/models/shot.hpp";
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "读不到 " << path);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    const std::string head = "NLOHMANN_JSON_SERIALIZE_ENUM(" + enum_name + ",";
    const auto at = src.find(head);
    REQUIRE_MESSAGE(at != std::string::npos, "shot.hpp 里找不到 " << head);
    const auto end = src.find("})", at);
    REQUIRE(end != std::string::npos);

    std::set<std::string> out;
    const std::string block = src.substr(at + head.size(), end - at - head.size());
    for (std::size_t i = 0; i < block.size(); ++i) {
        if (block[i] != '"') continue;
        const auto close = block.find('"', i + 1);
        if (close == std::string::npos) break;
        out.insert(block.substr(i + 1, close - i - 1));
        i = close;
    }
    return out;
}

/// 表里这一族有哪些 value。
std::set<std::string> table_values(const json& enums, const std::string& key) {
    std::set<std::string> out;
    REQUIRE(enums.contains(key));
    for (const auto& row : enums.at(key)) out.insert(row.value("value", std::string()));
    return out;
}

}  // namespace

TEST_CASE("枚举中文表：shot.hpp 里有的值，一个都不能少") {
    const json e = http::enums_json();

    const std::pair<const char*, const char*> pairs[] = {
        {"ShotSize", "shot_size"},
        {"CameraAngle", "camera_angle"},
        {"CameraMove", "camera_move"},
        {"ShotStatus", "shot_status"},
    };

    for (const auto& [cpp_name, key] : pairs) {
        CAPTURE(cpp_name);
        const auto want = enum_values(cpp_name);
        const auto have = table_values(e, key);
        REQUIRE(!want.empty());
        for (const auto& v : want) {
            CAPTURE(v);
            CHECK(have.count(v) == 1);
        }
        // 反过来也查一遍：表里多出一个 shot.hpp 里没有的值，说明手打错了字
        // ——而打错的那一条在界面上就是"这一栏永远显示英文原文"。
        for (const auto& v : have) {
            CAPTURE(v);
            CHECK(want.count(v) == 1);
        }
    }
}

TEST_CASE("枚举中文表：每一条都得有中文，而且不能是英文原文") {
    const json e = http::enums_json();
    for (const auto& [key, rows] : e.items()) {
        for (const auto& row : rows) {
            const std::string v = row.value("value", std::string());
            const std::string zh = row.value("label", std::string());
            CAPTURE(key);
            CAPTURE(v);
            CHECK(!zh.empty());
            // 中文没填时最容易犯的错是把 value 原样抄一遍。
            CHECK(zh != v);
        }
    }
}

TEST_CASE("枚举中文表：状态那一族用引擎已有的那份，不另写一遍") {
    // `models::status_zh` 早就有了，进的是命令行摘要和报错消息。这一条钉住
    // 「两处说的是同一句话」——不然同一个状态在界面上和报错里叫两个名字。
    const json e = http::enums_json();
    for (const auto& row : e.at("shot_status")) {
        const std::string v = row.value("value", std::string());
        models::ShotStatus s{};
        from_json(json(v), s);
        CAPTURE(v);
        CHECK(row.value("label", std::string()) == std::string(models::status_zh(s)));
    }
}
