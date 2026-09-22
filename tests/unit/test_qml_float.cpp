// **挂在自己身子外面的那一小块，必须是 `Floater`。**
//
// 那一族是"鼠标停上去冒出来的牌子 / 表"：它不在自己那一格里，而是往上或者
// 往下探出去，落在**别人的内容**上。而这套界面是平的——牌子是 `surface`
// 加一道细边，正文和输入框也是 `surface` 加一道细边，两件叠上就糊成一件。
//
// 2026-09-21 截图里同时撞见两处：
//
//   · 一条回话底下那颗「改一改重说」的牌子，在正文中间啃掉一块白；
//   · 输入框底下那盏感知灯的表，和输入框糊成一件，看着像输入框裂了一道。
//
// 治它的是 `Floater.qml`（一层影子 + 重一档的边），规矩也写在那儿。
//
// 判据只认**一句结构上的话**：`anchors.top: parent.bottom` 或者
// `anchors.bottom: parent.top`——「我挂在我爹的外面」。这一句在的，就得是
// `Floater`。挂在自己身子里的（停上去才出现的那颗小按钮）不算这一族。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "desktop_sources.hpp"

namespace fs = std::filesystem;

namespace {

/// 把注释切掉再看（同 `test_qml_round.cpp`，理由写在那儿：一条只认字面的
/// 用例被一句注释骗过去，正是它自己该防的那一类）。
std::string code_of(const std::string& line) {
    const auto cut = line.find("//");
    return cut == std::string::npos ? line : line.substr(0, cut);
}

bool has(const std::string& hay, const std::string& needle) {
    return code_of(hay).find(needle) != std::string::npos;
}

int depth_of(const std::string& line) {
    int d = 0;
    for (const char c : code_of(line)) {
        if (c == '{') ++d;
        if (c == '}') --d;
    }
    return d;
}

struct Bare {
    std::string file;
    int line = 0;
    std::string head;
};

}  // namespace

TEST_CASE("桌面端的 QML：挂在身子外面的牌子必须是 Floater") {
    if (!changji_test::desktop_sources()) return;
    const fs::path dir{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    std::vector<Bare> bare;
    int checked = 0;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".qml") continue;
        if (entry.path().filename() == "Floater.qml") continue;   // 它自己
        std::ifstream in(entry.path());
        REQUIRE(in.good());
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);

        for (std::size_t i = 0; i < lines.size(); ++i) {
            // 一块的头一行：`Xxx {`。只看带大写开头的那一族（元素），
            // 别把 `function f() {` 之类也算进来。
            const std::string code = code_of(lines[i]);
            const auto brace = code.rfind('{');
            if (brace == std::string::npos) continue;
            std::string head;
            for (const char c : code.substr(0, brace)) {
                if (!std::isspace(static_cast<unsigned char>(c))) head += c;
            }
            if (head.empty() || !std::isupper(static_cast<unsigned char>(head[0]))) continue;

            // 这一块从这儿到配对的那个括号；只看它自己身子上那几句（深度 1）。
            int depth = 0;
            bool hangs_out = false;
            for (std::size_t j = i; j < lines.size(); ++j) {
                const int before = depth;
                depth += depth_of(lines[j]);
                if (before == 1 && (has(lines[j], "anchors.top: parent.bottom") ||
                                    has(lines[j], "anchors.bottom: parent.top"))) {
                    hangs_out = true;
                }
                if (depth == 0 && j > i) break;
            }
            if (!hangs_out) continue;

            ++checked;
            if (head != "Floater") {
                bare.push_back({entry.path().filename().string(),
                                static_cast<int>(i + 1), head});
            }
        }
    }

    // 一处都没找着就是这条用例自己不认路了，不是"全都合规"。
    REQUIRE_MESSAGE(checked >= 5,
                    "只找着 " << checked << " 块挂在身子外面的——这条用例八成是自己不认路了");

    for (const auto& b : bare) {
        CAPTURE(b.file);
        CAPTURE(b.line);
        CAPTURE(b.head);
        FAIL_CHECK("这一块挂在自己身子外面，落在别人的内容上，得是 Floater");
    }
}

// **每条回话底下那一排，牌子挂在上面。**
//
// 这一条单拎出来，因为它坏起来**一点痕迹都没有**：那一排在一条回话的最底下，
// 而**最后那一条的底下就是对话栏的下沿**——牌子整块落在栏外面被剪光，界面上
// 什么都不显示，也不报任何错。而人最常停上去的正是最后那一条（刚说完的那句、
// 刚写完的那段）。2026-09-21 之前它一直是挂底下的，谁都以为"这一排本来就没
// 牌子"。
TEST_CASE("桌面端的 QML：回话底下那一排的牌子挂在上面") {
    if (!changji_test::desktop_sources()) return;
    const fs::path f = fs::path{CHANGJI_DESKTOP_QML_DIR} / "FootKey.qml";
    std::ifstream in(f);
    REQUIRE_MESSAGE(in.good(), "读不到 " << f.string());

    bool up = false, down = false;
    for (std::string l; std::getline(in, l);) {
        if (has(l, "anchors.bottom: parent.top")) up = true;
        if (has(l, "anchors.top: parent.bottom")) down = true;
    }
    CHECK_MESSAGE(up, "牌子没挂在上面——最后一条回话上它会被对话栏整块剪掉");
    CHECK_MESSAGE(!down, "牌子挂到底下去了——最后一条回话上它会被对话栏整块剪掉");
}
