// **界面上新加一句话，十一种语言里会静悄悄地少一句。**
//
// 那一句在别的语言下**原样显示中文**——不报错、不空白、不留记号。一屏
// 阿拉伯语里夹着两个汉字，只有那一国的人看得见，而他们看不懂也说不清是
// 哪儿漏了。`cpp/tools/i18n.sh` 头上写着「改完界面上的话记得跑一次
// update」，可那是一句叮嘱，没人拦着。这几条就是那道拦。
//
// 拦四件事，每一件都是真出过的形状：
//
// | 漏掉的那一步 | 不拦的话长什么样 |
// |---|---|
// | 加了 `qsTr()` 没跑 `i18n.sh update` | 那一句在十种语言里都显示中文 |
// | 跑了 update 没翻（`type="unfinished"`） | 同上，而且 `.ts` 里看着"有这一条" |
// | 翻完没跑 `release` | `.qm` 还是上一版，界面上一个字都没变 |
// | 加了一种语言只加了一处名单 | 抽不到它 / 塞不进二进制，而两样都不响 |
//
// **名单有三处**（`i18n.sh` 的 `LANGS`、`desktop/CMakeLists.txt` 的
// `qt_add_resources`、`i18n/` 目录里的文件），收不成一处——一处是 shell、
// 一处是 CMake、一处是磁盘。收不动就让它会响，见 CLAUDE.md「同一件事别在
// 两处各写一遍」。
//
// ⚠️ **设置里那个「挑语言」不是第四处。** 它走
// `QDirIterator(":/i18n")`（`desktop/tongue.hpp`）——二进制里真有哪几份
// `.qm` 就列哪几份，也就是上面第二处的结果。手写一份的话，加一种语言
// 漏掉它的表现是「装是装进去了，界面上却挑不到」，一个字都不报。
//
// ⚠️ **不在这儿写死句数。** 判据是"三处对得上、一句不缺、一句没空着"，
// 不是"多少句"——写死的数每加一句就过期一次。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/llm_providers.inc.hpp"
#include "http/prompt_peek.hpp"
#include "setup/catalog.hpp"
#include "util/say.hpp"
#include "desktop_sources.hpp"

namespace fs = std::filesystem;

namespace {

/// 仓库里 `changji/cpp` 那一层。`CHANGJI_SRC_DIR` 是 `<…>/cpp/src`。
fs::path cpp_dir() { return fs::path{CHANGJI_SRC_DIR}.parent_path(); }

/// 桌面端那一层。**2026-09-22 起它在仓库根上**（`changji/desktop`），不再是
/// `cpp/desktop`——壳和核心分开。这个文件里有十来处要读它底下的东西
/// （CMakeLists、i18n/、qml/），**只在这儿写一遍路径**：上次这几处各写各的，
/// 搬一次目录就得改十来行，漏一行的表现是那一条用例静悄悄读到空文件。
fs::path desk_dir() { return cpp_dir().parent_path() / "desktop"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/// 一个 C/QML 字符串字面量的转义还原。只认这一套源码里真出现的那两个。
std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out += (n == 'n') ? '\n' : (n == 't') ? '\t' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

/// 把注释剥掉。**认字符串里的 `//`**——`"http://某台:8080"` 那一句是真要
/// 翻的，从第一个 `/` 一刀切下去的话它会变成一个没收口的字面量，于是抠不
/// 到，于是"`.ts` 里有而源码里没有"，于是一条**永远红着的假警报**。
///
/// 非剥不可：`util/say.hpp` 的文档注释里写着一行 `SAYF("…")` 的例子——
/// 那是给人看的样例，不是一处调用，可它长得一模一样。
std::string no_comments(const std::string& t) {
    std::string out;
    out.reserve(t.size());
    bool in_str = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (in_str) {
            out += t[i];
            if (t[i] == '\\' && i + 1 < t.size()) { out += t[++i]; continue; }
            if (t[i] == '"' || t[i] == '\n') in_str = false;   // 换行也算收口，防跑飞
            continue;
        }
        if (t[i] == '"') { in_str = true; out += t[i]; continue; }
        if (t[i] == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            if (i < t.size()) out += '\n';
            continue;
        }
        if (t[i] == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) ++i;
            ++i;
            continue;
        }
        out += t[i];
    }
    return out;
}

/// 把一个文件里所有 `qsTr("…")` / `tr("…")` 的**原话**抠出来。
///
/// 只认紧跟着括号的那个字面量：`qsTr("…").arg(x)` 抠的是前半截，和
/// `lupdate` 抠的是同一段（`%1` 留在里头，它本来就是原话的一部分）。
void harvest(const std::string& text, std::set<std::string>& into,
             const std::vector<std::string>& calls, bool second_arg = false) {
    for (const std::string& call : calls) {
        std::size_t at = 0;
        while ((at = text.find(call, at)) != std::string::npos) {
            const std::size_t start = at;
            at += call.size();
            // `substr(` / `qsTr(` 里也有 `tr(`——前面挨着标识符的不算。
            if (start > 0 && ident_char(text[start - 1])) continue;
            std::size_t i = start + call.size();
            while (i < text.size() && (text[i] == ' ' || text[i] == '\n'
                                       || text[i] == '\t' || text[i] == '\r')) ++i;
            // `SAY_TO(to, "…")`：头一个参数是"说给谁听"，句子在第二个。
            // 跳过那个标识符和它后面那个逗号——跳不过去就不是这个形状，
            // 不认（宁可漏一句，也别把别的东西当成话收进来）。
            if (second_arg) {
                const std::size_t was = i;
                while (i < text.size() && (ident_char(text[i]) || text[i] == ':'
                                           || text[i] == '.')) ++i;
                if (i == was) continue;
                while (i < text.size() && (text[i] == ' ' || text[i] == '\n'
                                           || text[i] == '\t' || text[i] == '\r')) ++i;
                if (i >= text.size() || text[i] != ',') continue;
                ++i;
                while (i < text.size() && (text[i] == ' ' || text[i] == '\n'
                                           || text[i] == '\t' || text[i] == '\r')) ++i;
            }
            if (i >= text.size() || text[i] != '"') continue;   // 变量，不是字面量
            // ⚠️ **挨着的几个字面量是一句。** C++ 和 QML 都把 `"甲" "乙"`
            // 拼成一句，源码里那些长句子正是这么折行的——只读头一截的话，
            // 抠出来的是半句，而半句在哪一份表里都找不到。
            std::string lit;
            bool closed = false;
            while (i < text.size() && text[i] == '"') {
                ++i;
                closed = false;
                for (; i < text.size(); ++i) {
                    if (text[i] == '\\' && i + 1 < text.size()) { lit += text[i]; lit += text[i + 1]; ++i; continue; }
                    if (text[i] == '"') { closed = true; break; }
                    if (text[i] == '\n') break;                 // 没收口就跨了行，不认
                    lit += text[i];
                }
                if (!closed) break;
                ++i;
                while (i < text.size() && (text[i] == ' ' || text[i] == '\n'
                                           || text[i] == '\t' || text[i] == '\r')) ++i;
            }
            if (closed && !lit.empty()) into.insert(unescape(lit));
        }
    }
}


bool has_han(const std::string& s) {
    // UTF-8 里 CJK 统一表意文字是 E4..E9 打头那三字节那一段。够用了——
    // 这儿只要分开"人话"和"产品名"。
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0xE4 && c <= 0xE9) return true;
    }
    return false;
}

/// 模型清单上**桌面端真会画出来的**那几段话。
///
/// ⚠️ **只收这三样是量出来的，不是偷懒。** 设置 ▸ 模型文件那一页画的是
/// `g.title`、`g.purpose`（`SettingsSheet.qml` 里那两行）和选中那一档的
/// `o.label`。剩下的几样（`o.note` / `o.family_note` / 每个文件的 `note` /
/// 替换档的 `title`）**只有网页那一套在画**——而 `webapp/` 是定稿冻住的、
/// 自己那一层的字也全是中文，把引擎这头翻过去只会让那一页一半一半。
/// 那几样记在方案的「还没做的」里，不在这条用例的判据里。
std::set<std::string> catalog_words() {
    std::set<std::string> out;
    const auto keep = [&out](const std::string& s) {
        if (has_han(s)) out.insert(s);
    };
    for (const auto& g : changji::setup::catalog()) {
        keep(g.title);
        keep(g.purpose);
        // 名字是拼出来的，按 `label_parts` 那条规矩拆——**和出 json 那头
        // 同一个函数**，判据和做法才不会各走各的。
        for (const auto& o : g.options)
            for (const auto& one : changji::setup::label_parts(o.label)) keep(one);
    }
    return out;
}

/// 大模型平台那本地址簿（项目页点编剧模型名弹出来那个窗里的「服务」下拉）
/// 上给人看的话。
///
/// 和模型清单同一条规矩：`http/llm_providers.inc.hpp` 是**一整块 JSON
/// 数据**，翻译发生在出 json 的那一处（`http/llm_info.cpp`）。所以抠
/// `SAY("…")` 抠不到它们，要走数据本身。
///
/// **只收 `name` 和 `note`**：`id` 和 `base_url` 是键，一个字不许动。
/// 判据仍是"里头有没有汉字"——`Ollama`、`vLLM` 这些纯产品名不翻。
std::set<std::string> provider_words() {
    std::set<std::string> out;
    const auto all = nlohmann::json::parse(
        changji::stages::prompt::kLlmProvidersJson, nullptr, false);
    REQUIRE_FALSE(all.is_discarded());
    for (const auto& p : all) {
        for (const char* field : {"name", "note"}) {
            const auto it = p.find(field);
            if (it == p.end() || !it->is_string()) continue;
            const std::string v = it->get<std::string>();
            if (has_han(v)) out.insert(v);
        }
    }
    return out;
}

/// `.ts` 里一句一句地「中文原话 → 译文」。
///
/// 译文那一段**连标签一起取**：带数的那一族里头是几个 `<numerusform>`，
/// 几档的字都要一起查，而这儿只查"里面有没有某个词"，标签混在里面无所谓。
std::vector<std::pair<std::string, std::string>> ts_messages(const std::string& ts) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t at = 0;
    while ((at = ts.find("<source>", at)) != std::string::npos) {
        at += 8;
        const std::size_t se = ts.find("</source>", at);
        if (se == std::string::npos) break;
        const std::string src = ts.substr(at, se - at);
        const std::size_t ts_at = ts.find("<translation", se);
        if (ts_at == std::string::npos) break;
        const std::size_t open = ts.find('>', ts_at);
        const std::size_t close = ts.find("</translation>", ts_at);
        if (open == std::string::npos || close == std::string::npos) break;
        out.emplace_back(src, ts.substr(open + 1, close - open - 1));
        at = close;
    }
    return out;
}

/// `.ts` 里 `<source>` 那一族。XML 那三个实体还原回去。
std::set<std::string> sources_of(const std::string& ts) {
    std::set<std::string> out;
    std::size_t at = 0;
    while ((at = ts.find("<source>", at)) != std::string::npos) {
        at += 8;
        const std::size_t end = ts.find("</source>", at);
        if (end == std::string::npos) break;
        std::string one = ts.substr(at, end - at);
        for (const auto& [ent, ch] : std::vector<std::pair<std::string, std::string>>{
                 {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}}) {
            std::size_t k = 0;
            while ((k = one.find(ent, k)) != std::string::npos) {
                one.replace(k, ent.size(), ch);
                k += ch.size();
            }
        }
        out.insert(one);
        at = end;
    }
    return out;
}

/// `i18n.sh` 里那一行 `LANGS="en zh_TW …"`。
std::vector<std::string> langs_from_script() {
    const std::string sh = slurp(cpp_dir() / "tools" / "i18n.sh");
    const std::size_t at = sh.find("LANGS=\"");
    REQUIRE(at != std::string::npos);
    const std::size_t end = sh.find('"', at + 7);
    REQUIRE(end != std::string::npos);
    std::vector<std::string> out;
    std::string cur;
    for (const char c : sh.substr(at + 7, end - at - 7)) {
        if (c == ' ') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// 一句话在 `.qm` 里长什么样：UTF-16 大端。**不是 UTF-8**——按 UTF-8 找
/// 一辈子找不着，而"找不着"看着就像"这一版没编进去"。
std::string utf16be(const std::string& u8) {
    std::string out;
    for (std::size_t i = 0; i < u8.size();) {
        unsigned cp = 0;
        const unsigned char c = static_cast<unsigned char>(u8[i]);
        int more = 0;
        if (c < 0x80)        { cp = c;          more = 0; }
        else if (c < 0xE0)   { cp = c & 0x1Fu;  more = 1; }
        else if (c < 0xF0)   { cp = c & 0x0Fu;  more = 2; }
        else                 { cp = c & 0x07u;  more = 3; }
        ++i;
        for (int k = 0; k < more && i < u8.size(); ++k, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(u8[i]) & 0x3Fu);
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            const unsigned hi = 0xD800u + (cp >> 10), lo = 0xDC00u + (cp & 0x3FFu);
            out += static_cast<char>(hi >> 8); out += static_cast<char>(hi & 0xFF);
            out += static_cast<char>(lo >> 8); out += static_cast<char>(lo & 0xFF);
        } else {
            out += static_cast<char>(cp >> 8); out += static_cast<char>(cp & 0xFF);
        }
    }
    return out;
}

/// `.ts` 里最长的那一句译文。拿它去 `.qm` 里对——最长的那句最不容易和
/// 别的语言撞脸，也最可能在改动里变过。
std::string longest_translation(const std::string& ts) {
    std::string best;
    std::size_t at = 0;
    while ((at = ts.find("<translation>", at)) != std::string::npos) {
        at += 13;
        const std::size_t end = ts.find("</translation>", at);
        if (end == std::string::npos) break;
        const std::string one = ts.substr(at, end - at);
        // ⚠️ **带数的那一族里头是几个 `<numerusform>`，不是一句话。** 整段
        // 连标签一起量的话它必是"最长的"，而 `.qm` 里当然找不到那串标签，
        // 于是这条用例红得莫名其妙（2026-09-22 加复数那天当场红在这儿）。
        // 挑一句**平的**去对，照样够用。
        if (one.size() > best.size() && one.find('&') == std::string::npos
            && one.find('<') == std::string::npos) {
            best = one;
        }
        at = end;
    }
    return best;
}

}  // namespace

TEST_CASE("多语言 · 三处名单对得上") {
    if (!changji_test::desktop_sources()) return;
    const std::vector<std::string> langs = langs_from_script();
    CHECK(langs.size() > 1);

    const std::string cmake = slurp(desk_dir() / "CMakeLists.txt");
    for (const std::string& lang : langs) {
        CAPTURE(lang);
        // 塞进二进制那一处
        CHECK(cmake.find("i18n/changji_" + lang + ".qm") != std::string::npos);
        // 磁盘上那两个文件
        CHECK(fs::exists(desk_dir() / "i18n" / ("changji_" + lang + ".ts")));
        CHECK(fs::exists(desk_dir() / "i18n" / ("changji_" + lang + ".qm")));
    }

    // 反过来也要对：目录里多出来一份没人抽、没人编的 `.ts`，加它的人会
    // 以为加上了。
    for (const auto& e : fs::directory_iterator(desk_dir() / "i18n")) {
        if (e.path().extension() != ".ts") continue;
        std::string stem = e.path().stem().string();   // changji_xx
        const std::string lang = stem.substr(std::string{"changji_"}.size());
        CAPTURE(lang);
        CHECK(std::find(langs.begin(), langs.end(), lang) != langs.end());
    }
}

TEST_CASE("多语言 · 界面上每一句都在名单里，而且都翻了") {
    if (!changji_test::desktop_sources()) return;
    // 界面那一头：`qsTr()` / `tr()` 的原话。
    std::set<std::string> said;
    const fs::path desk = desk_dir();
    for (const auto& e : fs::recursive_directory_iterator(desk)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".qml" && ext != ".cpp" && ext != ".hpp") continue;
        harvest(no_comments(slurp(e.path())), said, {"qsTr(", "tr("});
    }
    CHECK(said.size() > 100);   // 抠空了的话下面那几条全会"通过"

    for (const std::string& lang : langs_from_script()) {
        CAPTURE(lang);
        const fs::path ts = desk / "i18n" / ("changji_" + lang + ".ts");
        const std::string text = slurp(ts);

        // 一、没翻的那几句带着 `type="unfinished"`，而它们在界面上显示中文。
        CHECK(text.find("type=\"unfinished\"") == std::string::npos);

        // 二、界面上说的每一句，这一份里都得有。缺的那一句就是"加了 qsTr
        //     没跑 update"——而它在这一国的界面上是一句中文。
        const std::set<std::string> have = sources_of(text);
        for (const std::string& one : said) {
            if (have.count(one)) continue;
            CAPTURE(one);
            FAIL_CHECK("这一句界面上说了，" << lang << " 那一份里没有"
                       "（跑一次 `sh cpp/tools/i18n.sh`）");
        }

        // 三、反过来：源码里已经没有了、`.ts` 里还留着。`lupdate -no-obsolete`
        //     本来会删掉，留着就说明这一份是手改的，跟源码脱了节。
        for (const std::string& one : have) {
            if (said.count(one)) continue;
            CAPTURE(one);
            FAIL_CHECK("这一句 " << lang << " 那一份里有，源码里已经没人说了");
        }
    }
}

TEST_CASE("多语言 · 编出来的 .qm 跟得上 .ts") {
    if (!changji_test::desktop_sources()) return;
    // 翻完忘了 `i18n.sh release` 的话，`.ts` 是新的而界面上一个字没变
    // ——**两个文件都在、都不小**，看不出哪儿不对。
    const fs::path i18n = desk_dir() / "i18n";
    for (const std::string& lang : langs_from_script()) {
        CAPTURE(lang);
        const std::string ts = slurp(i18n / ("changji_" + lang + ".ts"));
        const std::string qm = slurp(i18n / ("changji_" + lang + ".qm"));
        // 空壳 `.qm`（`lrelease` 一次都没跑过）量出来是几十个字节。
        CHECK(qm.size() > 1000);
        const std::string longest = longest_translation(ts);
        REQUIRE(longest.size() > 0);
        CHECK(qm.find(utf16be(longest)) != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 引擎自己说的话（`SAY()` / `cpp/i18n/engine_*.json`）
//
// 和界面那半截是两套机制、同一个毛病：漏一句就**原样显示中文**，不报错。
// 2026-09-21 实测到的样子是十一份表里十份是空的 `{}`——机制建好了，
// 表没人填，而那六句在十种语言下一直显示着中文，界面全翻了也看不出来。
//
// ⚠️ **只管包进 `SAY()` 的那些。** 引擎里带中文的字面量三千多条，其中
// `stages/` 和 `agent/tools.cpp` 那几百条是**说给模型听的**，一个字都不
// 许动（理由在 `util/say.hpp` 头上）。所以这儿的判据是"包了的都翻了"，
// 不是"带中文的都翻了"——后者会把人逼着去翻提示词。

namespace {

/// 一份引擎的表里的「键 → 一句译文」。
///
/// 不拉 nlohmann 进来：这一条要查的正是"这份文件长得对不对"，而一个能
/// 容错的解析器会把手改坏的地方悄悄读过去。
///
/// ⚠️ **带数的那一族值是个对象**（`{"one": …, "other": …}`，见
/// `util/say.hpp` 的 `i18n::Plural`）：**一档摊成一行，键都是外面那个键**。
/// 这样下面三条一条都不用改——"键在不在"照旧，而"值里不许有汉字"那一条
/// 顺带把六档全查了。
///
/// ⚠️ 原来这儿是「把所有带引号的串两个两个配对」。那种配法一撞上嵌套的
/// 对象就**整份错位**：`{"写完了 %n 章": {"one": "…"}}` 会被读成键
/// 「写完了 %n 章」值「one」，再往后每一行的键和值都差着一格，而且**一条
/// 用例都不会红**——它只会开始抱怨一堆根本不存在的句子。
std::vector<std::pair<std::string, std::string>> flat_json(const std::string& t) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string outer, key;          // outer：带数那一族外面那个键
    bool want_key = true;
    int depth = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '{') { ++depth; want_key = true; continue; }
        if (t[i] == '}') { --depth; if (depth <= 1) outer.clear(); want_key = true; continue; }
        if (t[i] == ',') { want_key = true; continue; }
        if (t[i] != '"') continue;
        std::string one;
        for (++i; i < t.size() && t[i] != '"'; ++i) {
            if (t[i] == '\\' && i + 1 < t.size()) { one += t[i]; one += t[i + 1]; ++i; continue; }
            one += t[i];
        }
        one = unescape(one);
        if (want_key) {
            key = one;
            want_key = false;
            // 这个键的值要是个对象，下一个 `{` 会把 depth 推到 2。先记着
            // 外面这个键，里头那几档都算它的。
            if (depth == 1) outer = key;
            continue;
        }
        out.emplace_back(depth >= 2 ? outer : key, one);
    }
    return out;
}

}  // namespace

TEST_CASE("多语言 · 引擎说的每一句，十一份表里都得有") {
    // 引擎那一头：`SAY()` 包起来的原话。
    std::set<std::string> said;
    // **扫编进去的那几份。** 外层仓库有 agent/ 时（`CHANGJI_AGENT_OUTER`），
    // src/agent/ 和 src/http/chat_api.cpp 是没编进去的旧版——扫它们的话，外层
    // 那份新加的句子没人查，旧版里已经没人说的句子反倒被当成"还在说"。
    std::vector<fs::path> files;
    const fs::path src_dir = cpp_dir() / "src";
    for (const auto& e : fs::recursive_directory_iterator(src_dir)) {
        if (!e.is_regular_file()) continue;
        if (CHANGJI_AGENT_OUTER && (e.path().parent_path() == src_dir / "agent" ||
                                    e.path() == src_dir / "http" / "chat_api.cpp")) {
            continue;
        }
        files.push_back(e.path());
    }
    if (CHANGJI_AGENT_OUTER) {
        for (const auto& e : fs::directory_iterator(fs::path{CHANGJI_AGENT_SRC_DIR})) {
            if (e.is_regular_file()) files.push_back(e.path());   // tests/ 不算
        }
    }
    for (const auto& path : files) {
        const std::string ext = path.extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        // 生成物里也有这些字（烤好的表、嵌进去的网页），但那儿没有 `SAY(`。
        //
        // `SAY_NOOP(` 也要抠：它展开之后一个字符都没变，句子躺在一张静态
        // 表里，真翻是在用它的那一行 `SAY(kSlotNames[i])`。漏了它的话，
        // 表里那几句会被判成"源码里已经没人说了"。
        const std::string src = no_comments(slurp(path));
        harvest(src, said, {"SAY(", "SAYF(", "SAYN(", "SAY_NOOP("});
        // `SAY_TO(to, "…")` / `SAYF_TO(to, "…")`：一句话两个读者，句子在
        // **第二个参数**上。模型那一遍拿原话、人那一遍查表，所以这些句子
        // 和别的一样得在十一份表里都有。
        harvest(src, said, {"SAY_TO(", "SAYF_TO(", "SAYN_TO("}, /*second_arg=*/true);
    }
    CHECK(said.size() > 1);      // 抠空了的话下面全会"通过"

    const fs::path dir = cpp_dir() / "i18n";
    const std::string was = changji::i18n::spoken();

    for (const std::string& lang : langs_from_script()) {
        CAPTURE(lang);
        const fs::path p = dir / ("engine_" + lang + ".json");
        REQUIRE_MESSAGE(fs::exists(p), "少一份引擎的表：" << p.string());
        const auto rows = flat_json(slurp(p));

        std::set<std::string> keys;
        for (const auto& [k, v] : rows) keys.insert(k);

        // 一、包了 `SAY()` 的每一句，这一份里都得有。
        for (const std::string& one : said) {
            if (keys.count(one)) continue;
            CAPTURE(one);
            FAIL_CHECK("这一句引擎会说，" << lang << " 那一份表里没有"
                       "（填 cpp/i18n/engine_" << lang
                       << ".json，再跑 python3 cpp/tools/gen_say.py）");
        }
        // 二、反过来：表里有而源码里没人说了。留着的话，下一个查"这句怎么
        //     没翻"的人会在表里找到它，然后怀疑机制坏了。
        //
        //     ⚠️ **要翻的话有两个来源。** 一半是 `SAY("…")` 的字面量，
        //     另一半是模型清单里的**数据**（那张表留着中文原话，翻发生在出
        //     json 那头，见下一条用例）。只按字面量判的话，清单那十九句会被
        //     当成"源码里没人说的"，而它们天天画在设置页上。
        //
        //     ⚠️ **单独检出引擎仓库时不判这一半**（`CHANGJI_AGENT_OUTER` 是 0）。
        //     这几份表是整个产品的，对话代理在外层仓库（2026-09-23 起），它说的
        //     那几句只有带着外层编的时候才扫得到——单独检出时它们"没人说"是
        //     对的，删了的话带着外层编就少了翻译。带着外层编（本机、CI 打包）
        //     照样一句一句判。
        for (const std::string& k : keys) {
            if (!CHANGJI_AGENT_OUTER) break;
            if (said.count(k) || catalog_words().count(k) ||
                provider_words().count(k)) {
                continue;
            }
            CAPTURE(k);
            FAIL_CHECK("这一句 " << lang << " 那份表里有，源码里已经没人 SAY 了");
        }
        // 三、**汉字不该出现在这几种语言的译文里。**
        //
        //     2026-09-22 我自己干过一次：俄语那一份里写着
        //     「mtmd подключён,媒体 маркер %1」——原话里那两个字漏了没换。
        //     前面两条查的是"键在不在"，这种漏换**键在、值也非空、看着全对**，
        //     只有真读那一句的人才看得出来。
        //
        //     ⚠️ **日文和韩文不在这一条里**：那两种语言里汉字是正当的。
        //     繁體更不用说。剩下八种一个汉字都不该有，除了下面那张小白名单
        //     ——`第01集.mp4` 是磁盘上真实存在的文件名，翻了反而指不着东西。
        //
        //     **标点也算。** 2026-09-22 起一台德语引擎点「出片」，看到的是
        //     「Stimme im Prozess：Es fehlt…」——两头的话都翻了，中间那个
        //     全角冒号是代码里直接拼上去的。`has_han` 抓不到它（标点不是
        //     汉字），而它一句德文里就那么立着。下面这几个中文标点在那八种
        //     语言里一个都不该有。破折号 `—` 和省略号 `…` 不在其列：
        //     那两个西文里本来就用。
        if (lang != "ja" && lang != "ko" && lang.rfind("zh", 0) != 0) {
            static const std::vector<std::string> kCjkPunct = {
                "：", "；", "，", "。", "、", "！", "？", "「", "」",
                "『", "』", "（", "）", "〈", "〉", "《", "》", "【", "】",
            };
            for (const auto& [k, v] : rows) {
                for (const std::string& punct : kCjkPunct) {
                    if (v.find(punct) == std::string::npos) continue;
                    CAPTURE(k);
                    CAPTURE(v);
                    CAPTURE(punct);
                    FAIL_CHECK("这一句 " << lang << " 的译文里留着中文标点"
                               "（多半是代码里直接拼上去的，改成 SAYF 的"
                               "一个 %1 空位）");
                }
            }
            for (const auto& [k, v] : rows) {
                if (v.find("第01集") != std::string::npos) continue;
                if (!has_han(v)) continue;
                CAPTURE(k);
                CAPTURE(v);
                FAIL_CHECK("这一句 " << lang << " 的译文里还留着汉字"
                           "——多半是有一截忘了换");
            }
        }

        // 三½、**空位要和原话对得上。**
        //
        //     漏一个 `%2` 的话，那一段数据在这种语言下**整个消失**，而句子
        //     照样通顺——看一眼看不出来。多写一个 `%4` 更糟：`fill()` 给不
        //     出来就把 `%4` 两个字符原样留在屏幕上。
        //
        //     ⚠️ **这一条挡不住"位置串了"**：`%1` 和 `%2` 互换过来，两边的
        //     集合还是一样的。俄语和阿拉伯语那句「%1 有 %2 镜一张参考图都拿
        //     不到」原来就是这么错的——**把章名当成了那个数**，屏幕上出来
        //     「в 第1章 кадров … : 5」。那种只有人读一遍才看得出来。
        //
        //     ⚠️ **`%n` 不在这一条里。** 带数的那一族里，阿拉伯语的「一」和
        //     「二」两档**正确的说法本来就不带那个数**（«لقطة واحدة» 自己就是
        //     "一个"），所以只查 `%1`…`%9`。
        {
            auto slots = [](const std::string& x) {
                std::set<std::string> out;
                for (std::size_t i = 0; i + 1 < x.size(); ++i) {
                    if (x[i] == '%' && x[i + 1] >= '1' && x[i + 1] <= '9') {
                        out.insert(x.substr(i, 2));
                    }
                }
                return out;
            };
            for (const auto& [k, v] : rows) {
                if (v.empty() || slots(k) == slots(v)) continue;
                CAPTURE(k);
                CAPTURE(v);
                FAIL_CHECK("这一句 " << lang << " 的译文里空位对不上原话"
                           "——少一个就是那段数据在这种语言下整个消失，"
                           "多一个就是屏幕上立着 %N 两个字符");
            }
        }

        // 四、**开头结尾那个换行要跟着。**
        //
        //     命令行那一族靠它断行：`"载好了，采样率 %1 Hz\n"` 少了尾巴上
        //     那个 `\n`，下一句就贴着它印在同一行上。而这种漏**在表里看着
        //     完全正常**——值非空、没有汉字、占位符也对。2026-09-22 阿拉伯
        //     语那一句就是这么漏的。
        for (const auto& [k, v] : rows) {
            if (v.empty()) continue;
            const auto tail = [](const std::string& x) {
                return !x.empty() && x.back() == '\n';
            };
            const auto head = [](const std::string& x) {
                return !x.empty() && x.front() == '\n';
            };
            if (tail(k) == tail(v) && head(k) == head(v)) continue;
            CAPTURE(k);
            CAPTURE(v);
            FAIL_CHECK("这一句 " << lang << " 的译文开头或结尾少了（或多了）"
                       "一个换行——印出来会和上下句黏在一起");
        }

        // 五、**真让它说一遍。** 前两条比的是两个文件，这一条走的是跑起来
        //     那条路：`say_tables.inc.hpp` 是 `gen_say.py` 烤出来的，填了表
        //     忘了烤的话，json 是新的而**进二进制的还是上一版**——两个文件
        //     都在、都不空，肉眼看不出来。
        //
        //     2026-09-21 实测撞到的还要更靠前一步：十一份表里**十份是空的
        //     `{}`**，那六句在十种语言下一直显示中文。空表和"漏了这一句"
        //     长得一模一样，而界面那 251 句全翻了，看上去只像是"这一段没翻"。
        //
        //     ⚠️ **带数的那一族（键里有 `%n`）走的是另一条路**：json 里一个
        //     键对着几档，`flat_json` 把它摊成了几行，`say()` 查的那张表里
        //     根本没有这个键。这儿改成"把 0…200 说一遍"——
        //     **给的每一档都得有某个数说得出来**，多给一档（英语给了 `two`）
        //     和少给一档一样是错的，而少给那一档另有守卫盯着
        //     （`test_say.cpp` 的「十一种语言都给齐了自己那几档」）。
        changji::i18n::speak(lang);
        std::map<std::string, std::set<std::string>> forms;
        for (const auto& [k, v] : rows) {
            if (v.empty()) continue;
            if (k.find("%n") != std::string::npos) { forms[k].insert(v); continue; }
            CAPTURE(k);
            CAPTURE(v);
            CHECK(changji::i18n::say(k) == v);
        }
        for (const auto& [k, want] : forms) {
            std::set<std::string> got;
            for (long long n = 0; n <= 200; ++n) got.insert(changji::i18n::say_n(k, n));
            CAPTURE(k);
            for (const std::string& one : got) {
                if (want.count(one)) continue;
                CAPTURE(one);
                FAIL_CHECK("说出来的这一句不在 engine_" << lang << ".json 里"
                           "——多半是表填了没烤（python3 cpp/tools/gen_say.py）");
            }
            for (const std::string& one : want) {
                if (got.count(one)) continue;
                CAPTURE(one);
                FAIL_CHECK("这一档填了，可 0…200 里没有一个数说得出它来"
                           "——" << lang << " 根本用不上这一档，写了也白写");
            }
        }
    }
    changji::i18n::speak(was);   // 进程级的开关，别留给下一条用例
}

// ---------------------------------------------------------------------------
// 模型清单（设置 ▸ 模型文件）上那些话
//
// 这一族和上面那族**判据不一样**：`setup/catalog.cpp` 里那张表留的是中文
// 原话，翻译发生在出 json 的那几处（`http/setup_api.cpp`，理由写在那儿）。
// 所以抠 `SAY("…")` 字面量抠不到它们——**要走的是数据本身**：把整张表遍历
// 一遍，凡是**带汉字**的那几段，十一份表里都得有。
//
// 判据是"里头有没有汉字"，因为这张表里两种东西混着：产品名
//（`Qwen-Image · Q8_0`、`Q4_K_M`）和人话（`不下载 · 之后再说`）。前者不翻，
// 而它们的区别正好就是有没有汉字。

TEST_CASE("多语言 · 模型清单上每一句给人看的话都翻了") {
    const std::set<std::string> words = catalog_words();
    // 遍历空了的话下面全会"通过"。收窄判据之后眼下十九句。
    CHECK(words.size() > 10);

    const fs::path dir = cpp_dir() / "i18n";
    for (const std::string& lang : langs_from_script()) {
        CAPTURE(lang);
        // ⚠️ **判据是"表里有没有这个键"，不是"译文和原话一不一样"。**
        // 繁體那一份里「配音模型」和「MiniMax-H3 完整」逐字就是原话——
        // 按"一不一样"判的话，翻好了的那两句会被报成没翻，而那种假警报
        // 逼着人去把一句对的话改错。
        const auto rows = flat_json(slurp(dir / ("engine_" + lang + ".json")));
        std::set<std::string> keys;
        for (const auto& [k, v] : rows) keys.insert(k);
        for (const std::string& one : words) {
            if (keys.count(one)) continue;
            CAPTURE(one);
            FAIL_CHECK("模型清单上这一句，" << lang << " 那一份里没有"
                       "（填 cpp/i18n/engine_" << lang
                       << ".json，再跑 python3 cpp/tools/gen_say.py）");
        }
    }
}

// ---------------------------------------------------------------------------
// **没包起来的中文**
//
// 前面几条查的都是"包起来的那些翻了没有"。可**从来没包过的那一句，谁都
// 看不见**：`.ts` 里没有它，十一份表里也没有它，两边都干干净净——而界面上
// 它在十一种语言下都显示中文。
//
// 2026-09-21 量出来：桌面端 179 处中文字面量没经过 `qsTr()`。其中一百多处
// 是 `objectName`（自检按名字点的那一族，**一个字都不许翻**——翻了自检当场
// 全瞎），三十来处是 `qWarning` / `qInfo`（给开发者看的，不是界面）。真漏
// 的是十四句：稿纸上的「%1 字 / 空的」、「在想」、引擎起不来那五句、
// 「没存上 / 已存 / 存着…」那一族。
//
// 判据：**画出来的中文必须包起来**。不该包的走下面那张白名单，一条一个
// 理由——白名单是让人**看得见自己在豁免什么**，而不是让规则悄悄变松。

namespace {

/// 这几处的中文**不该包**，每一条都写清为什么。
bool spared(const std::string& lit) {
    static const std::set<std::string> kOk = {
        // Script.qml：**拿来认字的**，不是拿来显示的。剧本正文里一场的
        // 开头写作「【第 3 场」，这两个串是在判断"这一行是不是场头"。
        // 翻了的话，中文剧本再也认不出场头来。
        "【第", "场",
        // Sidebar.qml：这两处拼的是 `objectName`（`对话·互联·原先那条`），
        // 扫描里那一档就按这个名字点。
        "原先那条",
        // main.cpp `item_name()`：只出现在自检失败时报的那句「这一下被谁
        // 接走了」里，界面上不画。
        "（没有）",
        // main.cpp：一条 `qWarning` 拆成了两行，第二行看不出自己是日志。
        "（要么是 MouseArea 没铺到这儿，要么接它的是个 Handler，",
        "这个口子认不出 Handler）",
        // main.cpp `setApplicationName`：**这是 QSettings 的域名**
        //（`com.changji.场记`），不是给人看的名字。给人看的那个是
        // `setApplicationDisplayName("changji")`。翻了等于把所有人的设置
        // 搬了家——而且是一声不响地搬。
        "场记",
        // installer/core/Install.cpp `writeShortcut` 的 `what`：只拼进
        // `util::log` 那几行（「桌面快捷方式已创建: …」），界面上不画。
        "桌面快捷方式", "开始菜单快捷方式",
    };
    return kOk.count(lit) > 0;
}

/// `qsTr(...)` / `tr(...)` 括号里那几段字面量在文件里的位置。
std::vector<std::pair<std::size_t, std::size_t>> wrapped_spans(
    const std::string& t, const std::vector<std::string>& calls) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const std::string& call : calls) {
        std::size_t at = 0;
        while ((at = t.find(call, at)) != std::string::npos) {
            const std::size_t start = at;
            at += call.size();
            if (start > 0 && ident_char(t[start - 1])) continue;
            std::size_t i = start + call.size();
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
            while (i < t.size() && t[i] == '"') {
                const std::size_t from = i;
                for (++i; i < t.size(); ++i) {
                    if (t[i] == '\\' && i + 1 < t.size()) { ++i; continue; }
                    if (t[i] == '"' || t[i] == '\n') break;
                }
                if (i >= t.size() || t[i] != '"') break;
                out.emplace_back(from, i);
                ++i;
                while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
            }
        }
    }
    return out;
}

bool is_log_line(const std::string& l) {
    for (const char* c : {"qWarning(", "qInfo(", "qDebug(", "qCritical(", "qFatal(",
                          // 安装器那一套（desktop/installer/core/Util.h）：写的是
                          // %TEMP%\changji-setup.log，给查问题的人看，不上界面。
                          "util::log("})
        if (l.find(c) != std::string::npos) return true;
    // 同一个文件里（core/Util.cpp）不带命名空间直接调的那种：只认打头的。
    const std::size_t head = l.find_first_not_of(" \t");
    return head != std::string::npos && l.compare(head, 4, "log(") == 0;
}

/// 不是界面、是构建机上跑的命令行小工具：打包工具、打包前跑的回归门禁。
/// 它们的中文印在构建日志里，给的是做发布的人。
bool is_build_tool(const fs::path& p) {
    const std::string g = p.generic_string();
    return g.find("/installer/mkpkg/") != std::string::npos
        || g.find("/installer/tools/") != std::string::npos;
}

}  // namespace

TEST_CASE("多语言 · 界面上不许有没包起来的中文") {
    if (!changji_test::desktop_sources()) return;
    const fs::path desk = desk_dir();
    int bad = 0;
    for (const auto& e : fs::recursive_directory_iterator(desk)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".qml" && ext != ".cpp" && ext != ".hpp") continue;
        if (is_build_tool(e.path())) continue;
        const std::string t = no_comments(slurp(e.path()));
        // `say8(` / `sayw(`：安装器里不走 Qt 的那几句（installer/core/Words.h），
        // 查的是 words.inc 那张表——表齐不齐由下一条用例管。
        const auto spans = wrapped_spans(t, {"qsTr(", "tr(", "say8(", "sayw("});
        const auto inside = [&spans](std::size_t p) {
            for (const auto& [a, b] : spans) if (p >= a && p <= b) return true;
            return false;
        };
        // 一条 `qWarning` 可能拆成好几行，第二行看不出自己是日志。
        bool in_log = false;
        std::size_t line_at = 0;
        while (line_at <= t.size()) {
            const std::size_t end = std::min(t.find('\n', line_at), t.size());
            const std::string line = t.substr(line_at, end - line_at);
            if (is_log_line(line)) in_log = true;
            for (std::size_t i = line_at; i < end; ++i) {
                if (t[i] != '"') continue;
                const std::size_t from = i;
                std::string lit;
                for (++i; i < end; ++i) {
                    if (t[i] == '\\' && i + 1 < end) { lit += t[i]; lit += t[i + 1]; ++i; continue; }
                    if (t[i] == '"') break;
                    lit += t[i];
                }
                if (i >= end) break;
                if (!has_han(lit) || inside(from)) continue;
                if (in_log || spared(unescape(lit))) continue;
                // `objectName` 和喂给它的 `name:` / `tag:`：自检按名字点，
                // 翻了当场全瞎。
                if (line.find("objectName") != std::string::npos) continue;
                const std::string ls = line.substr(line.find_first_not_of(" \t"));
                if (ls.rfind("name:", 0) == 0 || ls.rfind("tag:", 0) == 0) continue;
                if (line.find(" name:") != std::string::npos
                    || line.find("{ name:") != std::string::npos) continue;
                ++bad;
                CAPTURE(e.path().filename().string());
                CAPTURE(lit);
                FAIL_CHECK("这一句画在界面上，却没包进 qsTr()/tr()"
                           "——它在十一种语言下都会显示中文");
            }
            if (!line.empty() && line.back() == ';') in_log = false;
            if (end >= t.size()) break;
            line_at = end + 1;
        }
    }
    CHECK(bad == 0);
}

// ---------------------------------------------------------------------------
// 安装器里不走 Qt 的那几句（installer/core/Words.h + words.inc）
//
// stub 和 uninstall.exe 不链 Qt，启动图亮起来的时候 .qm 还没解出来，所以它们
// 自己带一张表。上一条用例只认「包没包起来」，这一条管**表齐不齐**：少一句
// 就是那一国人在报错框里看见一句中文，而且不报错。
// ---------------------------------------------------------------------------

namespace {

/// words.inc 里一行一句：`CJ_WORD("原话", "en", "zh_TW", …)`。
std::vector<std::vector<std::string>> words_rows(const std::string& inc) {
    std::vector<std::vector<std::string>> rows;
    std::size_t at = 0;
    while ((at = inc.find("CJ_WORD(", at)) != std::string::npos) {
        std::size_t i = at + 8;
        std::vector<std::string> lits;
        // 读到这一句自己的右括号为止。字面量整段吃掉，里面的「（%1）」不算。
        while (i < inc.size() && inc[i] != ')') {
            if (inc[i] == '"') {
                std::string lit;
                for (++i; i < inc.size() && inc[i] != '"'; ++i) {
                    if (inc[i] == '\\' && i + 1 < inc.size()) { lit += inc[i]; lit += inc[i + 1]; ++i; continue; }
                    lit += inc[i];
                }
                lits.push_back(unescape(lit));
            }
            ++i;
        }
        rows.push_back(std::move(lits));
        at = i;
    }
    return rows;
}

int count_of(const std::string& s, const std::string& what) {
    int n = 0;
    for (std::size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}

}  // namespace

TEST_CASE("多语言 · 安装器不走 Qt 的那张表：用到的每一句都在，十一格都填了") {
    if (!changji_test::desktop_sources()) return;
    const fs::path inst = desk_dir() / "installer";

    // 源码里用到的原话。
    std::set<std::string> used;
    for (const auto& e : fs::recursive_directory_iterator(inst)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
        harvest(no_comments(slurp(e.path())), used, {"say8(", "sayw("});
    }
    CHECK(used.size() > 20);   // 抠空了的话下面全会"通过"

    // 表里的语言顺序就是 i18n.sh 的 LANGS——Words.cpp 那个数组要和它一模一样，
    // 不然每一格都错位一国（德国人看见的是法语），而且不报错。
    const auto langs = langs_from_script();
    {
        const std::string cpp = slurp(inst / "core" / "Words.cpp");
        const std::size_t a = cpp.find("kLangs[] = {");
        REQUIRE(a != std::string::npos);
        std::vector<std::string> order;
        for (std::size_t i = a, end = cpp.find("};", a); i < end; ++i) {
            if (cpp[i] != '"') continue;
            const std::size_t close = cpp.find('"', i + 1);
            order.push_back(cpp.substr(i + 1, close - i - 1));
            i = close;
        }
        CHECK(order == langs);
    }

    std::map<std::string, std::vector<std::string>> table;
    for (const auto& row : words_rows(no_comments(slurp(inst / "core" / "words.inc")))) {
        REQUIRE_FALSE(row.empty());
        const std::string& zh = row.front();
        CAPTURE(zh);
        REQUIRE(row.size() == langs.size() + 1);
        CHECK_MESSAGE(table.emplace(zh, std::vector<std::string>(row.begin() + 1, row.end())).second,
                      "同一句在表里出现了两次，查的时候只认头一行");
        for (std::size_t k = 0; k < langs.size(); ++k) {
            const std::string& tr = row[k + 1];
            CAPTURE(langs[k]);
            CAPTURE(tr);
            CHECK_FALSE(tr.empty());
            // 占位一个都不能丢：丢了 %1 的那一国，报错框里就没有那个路径。
            for (const char* ph : {"%1", "%2", "%3"})
                CHECK(count_of(tr, ph) == count_of(zh, ph));
            // 除了繁体和日语（本来就写汉字）那两格，译文里不该有汉字——有就是
            // 把原话粘过去了。
            if (langs[k] != "zh_TW" && langs[k] != "ja") CHECK_FALSE(has_han(tr));
        }
    }
    CHECK(table.size() > 20);

    for (const std::string& one : used) {
        if (table.count(one)) continue;
        CAPTURE(one);
        FAIL_CHECK("源码里 say8 / sayw 了这一句，words.inc 里没有——别的语言下它显示中文");
    }
    for (const auto& [zh, _] : table) {
        if (used.count(zh)) continue;
        CAPTURE(zh);
        FAIL_CHECK("words.inc 里有这一句，源码里已经没人说了");
    }
}

// ---------------------------------------------------------------------------
// 大模型平台那本地址簿（项目页 ▸ 编剧模型名 ▸「服务」下拉）
//
// 判据同上一条：`http/llm_providers.inc.hpp` 是一整块 JSON 数据，翻译发生
// 在 `http/llm_info.cpp` 出 json 的那一处。抠字面量抠不到，走数据本身。

TEST_CASE("多语言 · 平台地址簿上每一句给人看的话都翻了") {
    const std::set<std::string> words = provider_words();
    // 遍历空了的话下面全会"通过"。眼下二十八句。
    CHECK(words.size() > 20);

    const fs::path dir = cpp_dir() / "i18n";
    for (const std::string& lang : langs_from_script()) {
        CAPTURE(lang);
        const auto rows = flat_json(slurp(dir / ("engine_" + lang + ".json")));
        std::set<std::string> keys;
        for (const auto& [k, v] : rows) keys.insert(k);
        for (const std::string& one : words) {
            if (keys.count(one)) continue;
            CAPTURE(one);
            FAIL_CHECK("地址簿上这一句，" << lang << " 那一份里没有"
                       "（填 cpp/i18n/engine_" << lang
                       << ".json，再跑 python3 cpp/tools/gen_say.py）");
        }
    }
}

// ---- 场次头是结构，不是话 ----
//
// `/api/plan?peek` 回的那份提示词，按场拆时是三段拼起来的，每段前面一行
//
//     ===== 第 1/3 场：夜 · 内 · 客厅 =====
//
// 人把三段各喂给别的模型，再一起粘回来；`split_by_scene` 靠这一行切段。
//
// **这一行现在是翻译的**（2026-09-22），所以那几个等号是它唯一还认得出的
// 东西。哪一种语言的译文把等号丢了、或者在等号前面加了字，那一种语言下
// 「复制出去、粘回来」这条路就整条断掉——而症状是一句"粘回来的只有 1 段"，
// 半个字都提不到真正的原因。
TEST_CASE("多语言 · 十一种语言的场次头都切得开") {
    struct ScopedLang {
        std::string old_ = changji::i18n::spoken();
        explicit ScopedLang(const std::string& l) { changji::i18n::speak(l); }
        ~ScopedLang() { changji::i18n::speak(old_); }
    };
    // 和 `pipeline/storyboard_run.cpp` 里那两句**逐字一样**。改了那边，
    // 这儿也要改——不然这条用例守的是一个没人说的句子。
    static const char* kHeads[] = {
        "\n\n===== 第 %1/%2 场 =====\n\n",
        "\n\n===== 第 %1/%2 场：%3 =====\n\n",
    };
    for (const std::string& lang : langs_from_script()) {
        ScopedLang here(lang);
        for (const char* zh : kHeads) {
            const std::string head =
                changji::i18n::fill(SAY(zh), {"1", "3", "夜 · 内 · 客厅"});
            CAPTURE(lang);
            CAPTURE(head);
            // 两段假的回答，每段前面一行场次头——**真的那份就是这个形状**，
            // 头一行之前什么都没有（头之前的东西 split_by_scene 是丢掉的）。
            const std::string pasted =
                head + "{\"shots\": []}" + head + "{\"shots\": []}";
            const auto parts = changji::http::split_by_scene(pasted);
            CHECK(parts.size() == 2);
        }
    }
}

// ---- 并排摆着的那几个名字，同一种语言里不许撞 ----
//
// ⚠️ **这一条是照着截图加的，不是想出来的。** 2026-09-22 抓德语那张图时
// 看见：镜头墙的抬头写着 `Einstellungen`，而右边设置那张纸也叫
// `Einstellungen`——德语里 Einstellung 既是"设置"也是电影的"镜头"。一个词
// 指两个地方，点进去才知道去了哪儿。查了一遍**四种语言都撞了**：中日韩
// 的「設定 / 설정」同时是"应用设置"和"人物场景设定"。
//
// 撞了不报错、不崩、用例也全绿——**只有把两个名字并排摆出来看才看得见**。
// 所以把"并排"这件事写成判据：图标条那五格（引擎给的）和设置那张纸的名字
// （界面给的），同一种语言里两两不许相同。
//
// 改的时候记住**动哪一边**：「设置」那个词是平台惯例（德语 Einstellungen、
// 日语 設定），动不得；该换的是另一边——镜头改成 `Shot`（德语电影行当本来
// 就这么说），设定改成「設定集 / 設定資料 / 설정집」（动画圈的说法，正是
// "人物场景那一册"）。
TEST_CASE("多语言 · 并排摆着的名字不许撞") {
    if (!changji_test::desktop_sources()) return;
    const fs::path desk = desk_dir();
    for (const std::string& lang : langs_from_script()) {
        CAPTURE(lang);
        std::map<std::string, std::string> engine;
        for (const auto& [k, v] : flat_json(
                 slurp(cpp_dir() / "i18n" / ("engine_" + lang + ".json")))) {
            engine.emplace(k, v);
        }
        // `.ts` 里「设置」那一句的译文。**按 `<source>` 找**，不按位置。
        std::string ui_settings;
        {
            const std::string ts =
                slurp(desk / "i18n" / ("changji_" + lang + ".ts"));
            const std::string mark = "<source>设置</source>";
            std::size_t at = ts.find(mark);
            if (at != std::string::npos) {
                at = ts.find("<translation>", at);
                if (at != std::string::npos) {
                    at += std::string("<translation>").size();
                    const std::size_t end = ts.find("</translation>", at);
                    if (end != std::string::npos) ui_settings = ts.substr(at, end - at);
                }
            }
        }

        // 引擎给的那几格：图标条 + 侧边栏那几步。
        std::map<std::string, std::string> shown;   // 译文 -> 是哪一个
        for (const char* zh : {"故事", "设定", "剧本", "镜头", "片子",
                               "项目", "这一章", "成片"}) {
            const auto it = engine.find(zh);
            if (it == engine.end() || it->second.empty()) continue;
            const std::string who = std::string("引擎:") + zh;
            const auto seen = shown.find(it->second);
            CHECK_MESSAGE(seen == shown.end(),
                          lang << " 里「" << it->second << "」同时是 "
                               << (seen == shown.end() ? std::string() : seen->second)
                               << " 和 " << who);
            shown[it->second] = who;
        }
        // 界面给的：设置那张纸自己的名字。
        if (!ui_settings.empty()) {
            const auto seen = shown.find(ui_settings);
            CHECK_MESSAGE(seen == shown.end(),
                          lang << " 里「" << ui_settings << "」同时是 "
                               << (seen == shown.end() ? std::string() : seen->second)
                               << " 和 界面:设置");
        }
    }
}

// ---------------------------------------------------------------------------
// 一个概念一个词

TEST_CASE("多语言 · 同一件东西不许有两个叫法") {
    if (!changji_test::desktop_sources()) return;
    // **两个词说一件事，看上去像两件事。** 2026-09-22 量出来的样子：日语的
    // 「镜头」在引擎表里 ショット 44 处、カット 33 处，韩语 숏 49 处、
    // 컷 31 处——而**界面那半截各只用一个**（ja 全是 ショット，ko 全是 컷）。
    // 于是同一块屏上，镜头墙那一行说 ショット，底下引擎报的那句说 カット。
    //
    // ⚠️ **判据是「跟界面那个词走」**，不是"哪个词出现得多"。用户在镜头墙
    // 上看到的就是界面那个词，引擎的话是摆在它旁边的。
    //
    // ⚠️ **例外要写出是哪个词组，不能只写"有例外"**：日语「ハードカット」
    // （硬切，转场的一种）本来就该是 カット，那是另一个概念。
    struct Rule {
        std::string lang;
        std::string banned;                    // 这个词不许再出现
        std::vector<std::string> ok;           // 除非它是这几个词组的一部分
        std::string instead;
        // 只在中文原话里带这几个字的那些句子上查。**空 = 整份查。**
        //
        // ⚠️ 俄语要这一档：`план` 有两个意思，「大纲」那个是正当的
        // （`замена плана`），「镜头」那个才是错的——俄语里镜头是 `кадр`，
        // 「раскадровка」（分镜）就是从它来的，而 `план` 说的是**景别**
        // （`крупный план` = 特写，表里那几个景别正是这么写的）。
        // 一刀切禁掉这个词会把十五句说大纲的话一起判错。
        std::string when;
    };
    const std::vector<Rule> rules = {
        // 「ショートカット」是快捷方式（安装器那一页的「创建桌面快捷方式」），
        // 和镜头无关，同 ハードカット 一样是另一个词。
        {"ja", "カット", {"ハードカット", "ショートカット"}, "ショット", ""},
        {"ko", "숏", {}, "컷", ""},
        {"ru", "план", {}, "кадр", "镜"},
    };

    auto without = [](std::string s, const std::vector<std::string>& ok) {
        for (const std::string& w : ok) {
            for (std::size_t at = 0; (at = s.find(w, at)) != std::string::npos;) {
                s.erase(at, w.size());
            }
        }
        return s;
    };

    for (const Rule& r : rules) {
        CAPTURE(r.lang);
        CAPTURE(r.banned);
        // 引擎那半截。
        const auto rows = flat_json(slurp(cpp_dir() / "i18n" / ("engine_" + r.lang + ".json")));
        for (const auto& [k, v] : rows) {
            if (!r.when.empty() && k.find(r.when) == std::string::npos) continue;
            if (without(v, r.ok).find(r.banned) == std::string::npos) continue;
            CAPTURE(k);
            CAPTURE(v);
            FAIL_CHECK("这一句用的是「" << r.banned << "」，而界面那半截说的是「"
                       << r.instead << "」——同一块屏上两个词说一件事");
        }
        // 界面那半截。**源文是中文**，所以整份扫一遍不会误伤。
        // 带 `when` 的那种只能按句查，界面这半截走 `<source>`。
        const std::string ts = slurp(desk_dir() / "i18n"
                                     / ("changji_" + r.lang + ".ts"));
        if (r.when.empty()) {
            CHECK_MESSAGE(without(ts, r.ok).find(r.banned) == std::string::npos,
                          "changji_" << r.lang << ".ts 里出现了「" << r.banned
                          << "」，这半截说的该是「" << r.instead << "」");
            continue;
        }
        for (const auto& [src, one] : ts_messages(ts)) {
            if (src.find(r.when) == std::string::npos) continue;
            if (without(one, r.ok).find(r.banned) == std::string::npos) continue;
            CAPTURE(src);
            CAPTURE(one);
            FAIL_CHECK("changji_" << r.lang << ".ts 里这一句用的是「" << r.banned
                       << "」，该是「" << r.instead << "」");
        }
    }
}
