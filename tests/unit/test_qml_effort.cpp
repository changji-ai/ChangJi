// **「想多久」那几档：三处说的必须是同一套。**
//
// 这件事横跨三个地方，而三处各写一遍就会各自过期：
//
//   `llm/thinking.hpp`   `kThinkTiers`——**这是那一份**。哪个键存在、
//                        哪个模型认哪几档、字段叫什么，全在这儿。
//   `config/settings.cpp` `kEfforts`——存盘那道闸，只挡拼写错。
//   `ModelBar.qml`        `effortWords`——哪个键写哪个词，给人看的。
//
// 用户 2026-09-21 问的正是这件事：「每个厂商这个参数值和参数名都不一样如何
// 准确获取呢」。答案是**获取不到**（没有哪一家在 `/models` 里说自己支持
// 什么，探一次要花一次真调用而且 400 会把那次生成打掉），所以只能有一张表。
// 既然只能有一张，这条用例就负责让另外两处永远跟着它。
//
// 钉两条：
//
// 一、**界面上每个键都得有词，每个词都得有键。** 少一个：那一档在单子上
//     根本不出现，而引擎那头明明认；多一个：点下去存不进，报一句 422，
//     **而那句话在这条路上没人看**（`push()` 存完只是重新读一遍，读回来
//     还是旧值，界面上表现成"点了没反应"）。
//
// 二、**存盘那道闸得收得下每一个键。** 2026-09-21 差点掉进去：`off` 那一档
//     刚变成可选，而 `kEfforts` 里没有它——点「不要想」会被顶回来，同样是
//     "点了没反应"。
//
// **不钉"一共几档"也不钉哪个词对哪个值**——那是产品定的，改一次这条用例
// 就得跟着改一次（2026-09-21 一天里改过两回）。钉的是三处对不对得上。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    REQUIRE_MESSAGE(in.good(), "读不到 " << p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// 把 `"xxx"` 那几段字面量抠出来，从 `from` 到 `to` 之间。
std::set<std::string> quoted_between(const std::string& hay,
                                     const std::string& from,
                                     const std::string& to) {
    const auto a = hay.find(from);
    REQUIRE_MESSAGE(a != std::string::npos, "源码里找不着 " << from);
    const auto b = hay.find(to, a);
    REQUIRE_MESSAGE(b != std::string::npos, "源码里找不着 " << from << " 后面的 " << to);
    std::set<std::string> out;
    for (std::size_t i = a; i < b;) {
        const auto q1 = hay.find('"', i);
        if (q1 == std::string::npos || q1 >= b) break;
        const auto q2 = hay.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > b) break;
        out.insert(hay.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return out;
}

}  // namespace

TEST_CASE("「想多久」那几档：那张表、存盘那道闸、界面上那几个词，说的是同一套") {
    // 那一份：`inline constexpr const char* kThinkTiers[] = {…};`
    const std::string th = slurp(fs::path{CHANGJI_SRC_DIR} / "llm" / "thinking.hpp");
    const std::set<std::string> tiers = quoted_between(th, "kThinkTiers[] = {", "}");
    REQUIRE_MESSAGE(tiers.size() >= 4, "那张表只抠出 " << tiers.size()
                                       << " 档——这条用例八成是自己不认路了");

    // 存盘那道闸：`static const std::set<std::string> kEfforts = {…};`
    const std::string cfg = slurp(fs::path{CHANGJI_SRC_DIR} / "config" / "settings.cpp");
    const std::set<std::string> gate = quoted_between(cfg, "kEfforts = {", "}");

    // 界面：`readonly property var effortWords: [ { k: "max", l: … }, … ]`
    const std::string qml = slurp(fs::path{CHANGJI_DESKTOP_QML_DIR} / "ModelBar.qml");
    const auto a = qml.find("property var effortWords:");
    REQUIRE_MESSAGE(a != std::string::npos, "ModelBar.qml 里找不着 effortWords");
    const auto end = qml.find("\n    ]", a);
    REQUIRE(end != std::string::npos);
    std::set<std::string> ui;
    for (std::size_t i = qml.find("k:", a); i != std::string::npos && i < end;
         i = qml.find("k:", i + 1)) {
        const auto q1 = qml.find('"', i);
        if (q1 == std::string::npos || q1 >= end) break;
        const auto q2 = qml.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > end) break;
        ui.insert(qml.substr(q1 + 1, q2 - q1 - 1));
    }

    // 一、界面和那张表一一对上。
    for (const auto& k : tiers) {
        CAPTURE(k);
        CHECK_MESSAGE(ui.count(k) == 1,
                      "那张表里有这一档，而界面上没给它词——单子上根本不出现");
    }
    for (const auto& k : ui) {
        CAPTURE(k);
        CHECK_MESSAGE(tiers.count(k) == 1,
                      "界面上摆了这一档，而那张表里没有——永远灰着，点了也没用");
    }

    // 二、存盘那道闸收得下每一档。
    for (const auto& k : tiers) {
        CAPTURE(k);
        CHECK_MESSAGE(gate.count(k) == 1,
                      "存盘那道闸不收这一档——点下去被顶回来，表现成点了没反应");
    }
    CHECK_MESSAGE(gate.count("") == 1, "存盘那道闸不收空串——「爱想不想」那一档存不下");
}
