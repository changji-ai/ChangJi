// 桌面端每一块显示文字的地方，都得把 `textFormat` 说清楚。
//
// **不说清楚的默认值是 `Text.AutoText`，而它会去猜。** 猜的后果是人打的字
// 被当成标记吃掉：
//
//     人打的： 把 <b>第三章</b> 的标题改一下，注意 a<b 那个条件
//     屏幕上： 把 第三章 的标题改一下，注意 a
//
// 前半截被当成加粗标记吃了，**后半截整个不见了**——`a<b` 开了一个没有收尾
// 的标签，它后面的字全进了那个标签里。人以为自己发出去的是一句话，屏幕上
// 是半句。2026-09-21 在自己的气泡上截到的。
//
// 所以规矩定成**一刀切**：`Text` / `TextEdit` / `TextArea` 一律写明。
// 现在摆的是死字符串、以后换成模型给的那一串——那一天没人会想起来补这一行，
// 而坏掉的样子还是"话说了一半"。
//
// 做法同 `test_enums.cpp` / `test_rail.cpp`：收不动就让它会响（CLAUDE.md
// 第八条）。这一条读的是 QML 源码。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// 一处没写明的：哪个文件、第几行、是什么。
struct Naked {
    std::string file;
    int line = 0;
    std::string kind;
};

/// 这一行是不是 `Text {` / `TextEdit {` / `TextArea {` 的开头。
/// 回的是缩进宽度和类型名；不是的话回 -1。
int opens_text(const std::string& line, std::string& kind) {
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    for (const char* k : {"TextEdit", "TextArea", "Text"}) {
        const std::string name(k);
        if (line.compare(i, name.size(), name) != 0) continue;
        std::size_t j = i + name.size();
        while (j < line.size() && line[j] == ' ') ++j;
        if (j >= line.size() || line[j] != '{') continue;
        // `{` 后面只许有空白：`Text { ... }` 写成一行的话下面那套按行走的
        // 判断就不成立，真出现了要当场看见，别装作没有。
        for (std::size_t t = j + 1; t < line.size(); ++t) {
            if (line[t] != ' ' && line[t] != '\r') return -1;
        }
        kind = name;
        return static_cast<int>(i);
    }
    return -1;
}

int brace_delta(const std::string& line) {
    int d = 0;
    for (const char c : line) {
        if (c == '{') ++d;
        if (c == '}') --d;
    }
    return d;
}

}  // namespace

TEST_CASE("桌面端的 QML：显示文字的地方一律写明 textFormat") {
    const fs::path dir{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    std::vector<Naked> naked;
    int seen = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".qml") continue;
        std::ifstream in(entry.path());
        REQUIRE(in.good());
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);

        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string kind;
            const int indent = opens_text(lines[i], kind);
            if (indent < 0) continue;
            ++seen;
            // 数花括号找这一块的闭合。
            int depth = 0;
            std::size_t end = i;
            for (std::size_t j = i; j < lines.size(); ++j) {
                depth += brace_delta(lines[j]);
                if (depth == 0 && j > i) { end = j; break; }
                end = j;
            }
            // **只认自己这一层的属性行**（缩进正好多四格）。嵌在里头的那些
            // 是别人的，算它们的话会让外面这一层白白过关。
            const std::string want = std::string(indent + 4, ' ') + "textFormat";
            bool said = false;
            for (std::size_t j = i + 1; j < end; ++j) {
                if (lines[j].compare(0, want.size(), want) == 0) { said = true; break; }
            }
            if (!said) {
                naked.push_back({entry.path().filename().string(),
                                 static_cast<int>(i + 1), kind});
            }
        }
    }

    // 一个都没找着就是这条用例自己坏了（改了写法、挪了目录），不是"全都合规"。
    REQUIRE_MESSAGE(seen > 20, "只找着 " << seen << " 处 Text——这条用例八成是自己不认路了");

    for (const auto& n : naked) {
        CAPTURE(n.file);
        CAPTURE(n.line);
        CAPTURE(n.kind);
        CHECK_MESSAGE(false, n.file << ":" << n.line << " 的 " << n.kind
                                    << " 没写 textFormat（默认的 AutoText 会把人打的 `<` 吃掉）");
    }
    CHECK(naked.empty());
}

// ---- 另一头：摆出来的字里别写 Markdown 记号 ----
//
// 2026-09-21 实撞：设置页那句提示写成
//
//     qsTr("……互相看得见——**看见不等于能用**，要给谁用得逐台点开。")
//
// 而它落在一块 `Text.PlainText` 上，于是屏幕上原样印着四个星号。
//
// 这一族的特点是**写的时候完全自然**——这个仓库里的注释、commit、文档全在
// 用 `**` 强调，手指头顺下来就带进了界面字符串。而界面上那几块显示文字的
// 地方**绝大多数是 PlainText**（上面那条用例钉的就是这个），它们不认。
//
// 所以钉一条：QML 里 `qsTr("…")` 的那串字里不许出现 `**`。
// 真要强调，换个说法或者用「」——这一屏上本来也没有加粗这一档。

TEST_CASE("桌面端的 QML：摆给人看的字里别写 Markdown 记号") {
    const fs::path dir{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    struct Bad { std::string file; int line; };
    std::vector<Bad> bad;
    int looked = 0;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".qml") continue;
        std::ifstream in(entry.path());
        REQUIRE(in.good());
        std::string line;
        int no = 0;
        while (std::getline(in, line)) {
            ++no;
            // 注释里随便写——那是给人读的，不摆在界面上。
            const auto cut = line.find("//");
            const std::string code =
                cut == std::string::npos ? line : line.substr(0, cut);
            const auto at = code.find("qsTr(\"");
            if (at == std::string::npos) continue;
            ++looked;
            const auto end = code.find('"', at + 6);
            if (end == std::string::npos) continue;
            const std::string text = code.substr(at + 6, end - at - 6);
            if (text.find("**") != std::string::npos) {
                bad.push_back({entry.path().filename().string(), no});
            }
        }
    }

    REQUIRE_MESSAGE(looked >= 60,
                    "只找着 " << looked << " 句 qsTr——这条用例八成是自己不认路了");

    for (const auto& b : bad) {
        CAPTURE(b.file);
        CAPTURE(b.line);
        CHECK_MESSAGE(false,
                      b.file << ":" << b.line
                      << " 这句摆给人看的字里写了 `**`——它落在 PlainText 上，"
                      "屏幕上原样印出四个星号");
    }
    CHECK(bad.empty());
}
