// 那十七个小图标：**一个实例只许造用得着的那一个**，而且每一个都得接上线。
//
// 2026-09-21 量出来的一笔账。原来 `SlotIcon.qml` 里十七个形状是十七个 `Item`
// 全摆在那儿，靠 `visible: root.slotKey === "…"` 挑一个露出来——写着省事，
// 代价是**每一个实例都把十七个全造一遍**：47 个 `Rectangle` 加 16 个 `Item`，
// 真正看得见的只有三四个。
//
// 图标条那五颗的时候这笔账小得看不见。当天每条回话底下添了三到五颗之后就
// 不是了：一屏几十条，几千个 `QQuickItem` 挂在那儿。改成 `Component` ＋
// 一个 `Loader` 挑一个造，交错量了两轮：
//
//     旧 215.9 / 216.2 MB     新 193.6 / 194.0 MB     省下 22 MB（一成）
//
// 所以钉两条：
//
//   一、**不许再出现 `visible: root.slotKey ===`**。那是老写法的记号：
//       一出现就说明有人又把一个形状直接摆在了那儿。
//   二、**`Component` 和 `case` 要一一对上**。少一个 `case` 的后果是
//       那个键什么都不画——而它不报错、不警告，界面上就是一块空白，
//       而空白最像"这一格本来就没东西"。
//
// ⚠️ 这条只看**源码里怎么写的**。它拦不住"画歪了"，拦的是那一族改完
// 看着好好的、要么费内存要么一声不响少半截的写法。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// 从 `前缀` 后面把那个键名抠出来（认 `[A-Za-z0-9_]`）。抠不着回空串。
std::string key_after(const std::string& line, const std::string& prefix) {
    const auto at = line.find(prefix);
    if (at == std::string::npos) return {};
    std::size_t i = at + prefix.size();
    std::string out;
    while (i < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) {
        out += line[i++];
    }
    return out;
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return lines;
}

}  // namespace

TEST_CASE("小图标：一个实例只造用得着的那一个，而且每个都接上了线") {
    const fs::path file = fs::path{CHANGJI_DESKTOP_QML_DIR} / "SlotIcon.qml";
    REQUIRE_MESSAGE(fs::exists(file), "读不到 " << file.string());
    const auto lines = read_lines(file);
    REQUIRE(lines.size() > 100);

    std::set<std::string> models;   // Component { id: g_xxx }
    std::set<std::string> wired;    // case "xxx": return g_xxx
    std::vector<int> old_style;     // 老写法留下的行号

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.find("visible: root.slotKey ===") != std::string::npos) {
            old_style.push_back(static_cast<int>(i + 1));
        }
        const auto id = key_after(l, "id: g_");
        if (!id.empty()) models.insert(id);
        if (l.find("case \"") != std::string::npos) {
            const auto ret = key_after(l, "return g_");
            if (!ret.empty()) wired.insert(ret);
        }
    }

    // 一个都没找着就是这条用例自己不认路了，不是"全都合规"。
    REQUIRE_MESSAGE(models.size() >= 10,
                    "只找着 " << models.size() << " 个图标模子——这条用例八成是自己不认路了");

    for (const int ln : old_style) {
        CHECK_MESSAGE(false,
                      "SlotIcon.qml:" << ln
                      << " 还写着 `visible: root.slotKey ===`——那是老写法："
                      "十七个形状全造出来只露一个。加新图标要写成 "
                      "`Component { id: g_那个键 … }`，再在 `glyph()` 里加一行 `case`");
    }
    CHECK(old_style.empty());

    // 两边一一对上。
    std::vector<std::string> only_model, only_case;
    std::set_difference(models.begin(), models.end(), wired.begin(), wired.end(),
                        std::back_inserter(only_model));
    std::set_difference(wired.begin(), wired.end(), models.begin(), models.end(),
                        std::back_inserter(only_case));
    for (const auto& k : only_model) {
        CHECK_MESSAGE(false, "图标 `" << k << "` 有模子，`glyph()` 里却没有那一行 `case`"
                                        "——它什么都不会画，而且一声不响");
    }
    for (const auto& k : only_case) {
        CHECK_MESSAGE(false, "`glyph()` 里认得 `" << k << "`，却找不到它的模子");
    }
    CHECK(only_model.empty());
    CHECK(only_case.empty());
}
