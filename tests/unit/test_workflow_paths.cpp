// **CI 那几份 yml 里引到的路径，得真有那个文件。**
//
// 改名、挪地方、打错一个字——这一类错**只有推上去才知道**，而知道的方式是
// 一趟构建跑到一半停在「No such file or directory」上。桌面端那条线更贵：
// 它要装 Qt、编 sd.cpp 和 llama.cpp，三个平台几十分钟，才走到那一行。
//
// 这条用例花的是几毫秒。
//
// 认两种写法：`changji/cpp/...`（Unix）和 `changji\cpp\...`（yml 里
// Windows 那几步写的就是反斜杠）。带 `${{ }}` 的跳过——那是模板，
// 展开之后是什么这一层不知道。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

/// 仓库根：`.github/`、`cpp/`、`desktop/`、`webapp/` 都在这一层。
/// `CHANGJI_SRC_DIR` 是 `<根>/cpp/src`，往上数**两层**。
fs::path repo_root() {
    return fs::path{CHANGJI_SRC_DIR}.parent_path().parent_path();
}

/// 把 yml 里那一串解成真路径。
///
/// ⚠️ **yml 里写的是 `changji/cpp/...` 而不是 `cpp/...`**：CI 上
/// `actions/checkout` 带 `path: changji`，仓库落在工作区的 `changji/` 底下，
/// 所以那一串是**工作区相对**的，头上那截 `changji/` 指的就是仓库自己。
/// 剥掉它，剩下的对着仓库根找。
///
/// ⚠️ **别靠"检出目录叫不叫 changji"来定位。** 原来那版往上数三层、拿检出
/// 目录的爹当根，于是 `.github/workflows` 被找到了**仓库外面**——第一个
/// `REQUIRE` 当场停，底下十七条路一条都没验着。红的样子像"CI 路径写错了"，
/// 其实是这条用例自己站错了地方；而它一停，它要挡的那一类错就全放过去了。
fs::path resolve_ci(const std::string& p) {
    static const std::string kCheckout = "changji/";
    return repo_root() /
           (p.rfind(kCheckout, 0) == 0 ? p.substr(kCheckout.size()) : p);
}

/// 这一串看着像不像"仓库里的一条路"。
///
/// 只认 `changji/` 和 `.github/` 开头的：别的（`../desktop/...` 这种注释里
/// 的相对写法、`/usr/bin/...` 这种系统路径）不归这条用例管，认了只会误报。
bool looks_like_path(const std::string& t) {
    if (t.find("${{") != std::string::npos) return false;   // 模板，展开了才知道
    if (t.find('*') != std::string::npos) return false;     // 通配
    if (t.find('{') != std::string::npos) return false;     // `package_{a,b}.sh` 那种简写
    return t.rfind("changji/", 0) == 0 || t.rfind(".github/", 0) == 0;
}

/// 把 yml 切成一个个"可能是路径"的串。
///
/// 反斜杠换成斜杠（Windows 那几步那么写），前后的引号、逗号、括号剥掉。
std::set<std::string> paths_in(const fs::path& f) {
    std::ifstream in(f);
    REQUIRE_MESSAGE(in.good(), "读不到 " << f.string());
    std::set<std::string> out;
    for (std::string line; std::getline(in, line);) {
        std::string tok;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            const char c = i < line.size() ? line[i] : ' ';
            const bool sep = c == ' ' || c == '\t' || c == '"' || c == '\'' ||
                             c == ',' || c == '(' || c == ')' || c == '`' ||
                             c == '\r';
            if (!sep) { tok += (c == '\\' ? '/' : c); continue; }
            if (!tok.empty()) {
                // 结尾那几个标点剥掉：`路径。`、`路径）`、`路径:`
                while (!tok.empty() &&
                       (tok.back() == ':' || tok.back() == ';' || tok.back() == '.')) {
                    tok.pop_back();
                }
                // `./.github/...` → `.github/...`
                if (tok.rfind("./", 0) == 0) tok = tok.substr(2);
                if (looks_like_path(tok)) out.insert(tok);
            }
            tok.clear();
        }
    }
    return out;
}

}  // namespace

TEST_CASE("CI 那几份 yml 里引到的路径，文件真的在") {
    const fs::path dir = repo_root() / ".github" / "workflows";
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    int checked = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".yml") continue;
        for (const auto& p : paths_in(entry.path())) {
            ++checked;
            const fs::path full = resolve_ci(p);
            CAPTURE(entry.path().filename().string());
            CAPTURE(p);
            CHECK_MESSAGE(fs::exists(full), "这条路上没有东西：" << full.string());
        }
    }

    // 一条都没抠出来就是这条用例自己不认路了，不是"全都对"。
    REQUIRE_MESSAGE(checked >= 10,
                    "只抠出 " << checked << " 条路——这条用例八成是自己不认路了");
}
