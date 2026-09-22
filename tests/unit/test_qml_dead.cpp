// QML 里**声明了却没人读**的属性。
//
// 这一族的特点是：**它看着在起作用。** 代码里写着 `property real lineHeight:
// 1.75`，谁读到都会以为行距是 1.75——而那个类型根本没有这个属性，QML 只是
// 顺手给这个对象挂了一个新的、没人读的字段，不报错、不警告。一页中文正文
// 就那么挤成一堵墙，而源码上白纸黑字写着 1.75（`Manuscript.qml` 上的原话，
// 实测那会儿行距是 1.3）。
//
// 2026-09-21 又撞到一次，这次是自己造的：把图标条和顶上那一行从"跟着对话
// 那一栏走"改成"跟着主内容区走"之后，`chatLeft` / `chatRight` 就没人读了
// ——**而它们头上那段注释还写着「标题栏上那两样东西跟着它走」**，说的正好
// 和代码相反。死代码不碍事，说反话的注释会把下一个人带沟里。
//
// 所以钉一条：**QML 里 `property 名字: 值` 声明出来的，得有人读。**
//
// ⚠️ **要连 C++ 一起找。** `lightsDx` 在 QML 里一次都没被引用，读它的是
// `main.cpp` 里 `w->property("lightsDx")`——只扫 qml 目录的话，这条用例
// 会把一个活得好好的属性判成死的。
//
// 扫不到的那几种（`required property`、被字符串拼出来的名字）本来就不在
// 这条规则里：前者是 delegate 从 model 那头注进来的，没有"谁读它"这回事。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "desktop_sources.hpp"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// `名字` 在 `hay` 里作为一个完整的词出现了几次。
int words_in(const std::string& hay, const std::string& name) {
    int n = 0;
    std::size_t at = 0;
    while ((at = hay.find(name, at)) != std::string::npos) {
        const bool left = at == 0 || !(std::isalnum(static_cast<unsigned char>(hay[at - 1])) ||
                                       hay[at - 1] == '_');
        const std::size_t end = at + name.size();
        const bool right = end >= hay.size() ||
                           !(std::isalnum(static_cast<unsigned char>(hay[end])) ||
                             hay[end] == '_');
        if (left && right) ++n;
        at = end;
    }
    return n;
}

}  // namespace

TEST_CASE("桌面端的 QML：声明出来的属性得有人读") {
    if (!changji_test::desktop_sources()) return;
    const fs::path qml{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(qml), "读不到 " << qml.string());

    // 把整个 qml 目录和它旁边那一层 C++ 都读进来当"有没有人读"的底本。
    std::string haystack;
    std::vector<std::pair<std::string, std::string>> sources;   // 文件名 → 正文
    for (const auto& e : fs::directory_iterator(qml)) {
        if (e.path().extension() != ".qml") continue;
        const auto text = slurp(e.path());
        sources.emplace_back(e.path().filename().string(), text);
        haystack += text;
        haystack += "\n";
    }
    // ⚠️ C++ 那一层：`w->property("lightsDx")` 这种读法只在这儿看得见。
    const fs::path cpp = qml.parent_path();
    for (const auto& e : fs::directory_iterator(cpp)) {
        const auto ext = e.path().extension();
        if (ext != ".cpp" && ext != ".hpp") continue;
        haystack += slurp(e.path());
        haystack += "\n";
    }
    REQUIRE(sources.size() >= 10);

    const std::regex decl(R"(^[ \t]*(?:readonly[ \t]+)?property[ \t]+\w+[ \t]+(\w+)[ \t]*:)");

    struct Dead { std::string file; int line; std::string name; };
    std::vector<Dead> dead;
    int looked = 0;

    for (const auto& [name, text] : sources) {
        std::istringstream in(text);
        std::string line;
        int no = 0;
        while (std::getline(in, line)) {
            ++no;
            std::smatch m;
            if (!std::regex_search(line, m, decl)) continue;
            ++looked;
            // 声明那一行自己也算一次，所以 1 次 = 没人读。
            if (words_in(haystack, m[1].str()) <= 1) {
                dead.push_back({name, no, m[1].str()});
            }
        }
    }

    // 一处都没找着就是这条用例自己不认路了。
    REQUIRE_MESSAGE(looked >= 40,
                    "只找着 " << looked << " 个属性声明——这条用例八成是自己不认路了");

    for (const auto& d : dead) {
        CAPTURE(d.file);
        CAPTURE(d.line);
        CHECK_MESSAGE(false,
                      d.file << ":" << d.line << " `" << d.name
                      << "` 声明了却没人读——要么是改了别处忘了拆，"
                      "要么是那个类型根本没有这个属性（QML 会顺手挂一个新的，"
                      "不报错），而源码上它看着在起作用");
    }
    CHECK(dead.empty());
}
