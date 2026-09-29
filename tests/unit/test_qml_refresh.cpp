// 桌面端两处「看着不对、但不报错」的刷新（用户 2026-09-27 报的）：
//
//   一、在「模型文件」里下完一份编剧模型，输入框底下的厂商单子上没有「本地」，得重开程序。
//       单子只在程序起来时问过一次本地有哪几份——开单子之前要再问一遍。
//   二、下模型时那几条进度条一直闪。「下到哪儿了」那一列的模型是 `down.items`，而 `down`
//       一秒整份换一次：每一秒每一行都拆了重建，进度条从 0 重新长一遍。模型要换成只在
//       「下的是哪几件」变了时才换的键（`downKeys`），每一行按键去 `down` 里现取。
//       （离屏台架实测：同样五次轮询，按 `down.items` 建了 10 行，按键只建 2 行。）
//
// 这两样都没法在单元测试里真跑 QML，钉的是写法：改回去的那一下这儿就红。

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "desktop_sources.hpp"

namespace {

std::string slurp_qml(const char* name) {
    std::ifstream in(changji_test::desktop_dir() / "qml" / name, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 从 `from` 起到下一个 `until` 为止那一段。
std::string block(const std::string& s, const std::string& from, const std::string& until) {
    const auto a = s.find(from);
    if (a == std::string::npos) return {};
    const auto b = s.find(until, a + from.size());
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

}  // namespace

TEST_CASE("桌面端：厂商单子开之前先问一遍本地有哪几份（下完模型不用重开程序）") {
    if (!changji_test::desktop_sources()) return;
    const std::string bar = slurp_qml("ModelBar.qml");
    const std::string vendor = block(bar, "objectName: \"挑厂商\"", "onChose:");
    REQUIRE_FALSE(vendor.empty());
    CHECK(vendor.find("beforeOpen") != std::string::npos);
    CHECK(vendor.find("pullLocal(") != std::string::npos);
}

TEST_CASE("桌面端：「下到哪儿了」那一列按键建行，不拿一秒一换的 down.items 当模型（进度条不闪）") {
    if (!changji_test::desktop_sources()) return;
    const std::string sheet = slurp_qml("SettingsSheet.qml");
    CHECK(sheet.find("property var downKeys") != std::string::npos);
    const std::string list = block(sheet, "// ---- 下到哪儿了 ----", "// 整趟下砸了那句话");
    REQUIRE_FALSE(list.empty());
    CHECK(list.find("model: root.downKeys") != std::string::npos);
    CHECK(list.find("root.down.items") == std::string::npos);
}

// 三、「思考内容一多就卡顿卡死」（用户 2026-09-27）。实测（假模型三万字思考、一帧三个字）：
//     原来那一版 7 秒吃满一个核、18 秒起窗口「未响应」再没回来；改完整趟界面线程随叫随答。
//     两层各钉一条：思考那一块只往后接、不整篇重灌；流着的那一条攒一拍再通知界面。
TEST_CASE("桌面端：思考那一块只往后接，不把 text 绑在 row.thinking 上（一片一整篇重排会卡死）") {
    if (!changji_test::desktop_sources()) return;
    const std::string main = slurp_qml("Main.qml");
    const std::string mind = block(main, "objectName: \"想了什么\"", "// ⚠️ **这一条是 `TextEdit` 不是 `Text`**");
    REQUIRE_FALSE(mind.empty());
    CHECK(mind.find("text: row.thinking") == std::string::npos);
    CHECK(mind.find("insert(length") != std::string::npos);
    CHECK(mind.find("onTextChanged") == std::string::npos);
}

TEST_CASE("桌面端：流着的那一条攒一拍再通知界面，不是来一片 dataChanged 一次") {
    if (!changji_test::desktop_sources()) return;
    std::ifstream in(changji_test::desktop_dir() / "chat_model.cpp", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    const std::string think = block(src, "void ChatModel::feedThinking(", "\n}");
    REQUIRE_FALSE(think.empty());
    CHECK(think.find("dataChanged") == std::string::npos);
    CHECK(think.find("markStream(") != std::string::npos);
    const std::string delta = block(src, "void ChatModel::feedDelta(", "\n}");
    REQUIRE_FALSE(delta.empty());
    CHECK(delta.find("dataChanged") == std::string::npos);
}
