#include "config/writeback.hpp"

#include "cloud/cloud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <atomic>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "config/settings.hpp"
#include "util/atomic_file.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

#include <toml++/toml.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::config {

namespace {

/// 行尾注释剥掉（引号里的 `#` 不算）。`[llm]  # 大模型` 原来认不出是节头：
/// 往 [llm] 里写的键被当成"整节都没有"，文件末尾又追加一个 `[llm]`——
/// 同一张表定义两次，下次读配置整份报错。
std::string strip_comment(const std::string& line) {
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (quote == '"' && c == '\\') { ++i; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '#') return line.substr(0, i);
    }
    return line;
}

/// 一行是不是节头 `[xxx]`，是的话返回节名。
///
/// 只认最朴素的写法。数组表 `[[x]]` 和带引号的节名 `["a.b"]` 都不认——
/// 这个配置文件里不会有那些，认了反而多一堆没测过的分支。
bool section_header(const std::string& line, std::string& name) {
    const std::string t = text::strip_ws(strip_comment(line));
    if (t.size() < 3 || t.front() != '[' || t.back() != ']') return false;
    if (t[1] == '[') return false;  // 数组表，不管
    name = text::strip_ws(t.substr(1, t.size() - 2));
    return !name.empty() && name.find('"') == std::string::npos;
}

/// 一行是不是 `key = ...`（**不含注释掉的**），是的话返回键名。
bool key_line(const std::string& line, std::string& key) {
    const std::string t = text::strip_ws(line);
    if (t.empty() || t[0] == '#') return false;
    const std::size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = text::strip_ws(t.substr(0, eq));
    if (key.empty()) return false;
    // **带引号的键也要认出来。** 裸键放不下的字符（比如 `video/video_llm`
    // 里的斜杠）写出去时会加引号（见 toml_key）；这儿不认的话，下一次写
    // 同一个键会当成"文件里没有"再追加一行，同一个键越攒越多。
    if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
        std::string inner;
        for (std::size_t i = 1; i + 1 < key.size(); ++i) {
            if (key[i] == '\\' && i + 2 < key.size()) ++i;
            inner += key[i];
        }
        if (inner.empty()) return false;
        key = inner;
        return true;
    }
    // 裸键只允许这些字符。写成别的多半是我没认出来的语法，宁可不动它。
    return std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

/// 从键值行 `start` 起，这个值一直延到哪一行（含）。
///
/// **多行的值**（`off = [` 换行 `"llm",` 换行 `]`，或者 `"""` 起头的长字符串）
/// 原来只认第一行：改这个键时只换掉第一行，后面几行留在原地，整份文件读不了；
/// 扫节的时候还会把长字符串里一行 `[xxx]` 当成节头。这儿按 TOML 的规矩数括号
/// 和引号（注释、字符串里的括号不算），数到平了为止。
std::size_t value_end(const std::vector<std::string>& lines, std::size_t start) {
    int depth = 0;
    enum { None, Basic, Literal, MultiBasic, MultiLiteral } str = None;
    bool seen_eq = false;
    for (std::size_t li = start; li < lines.size(); ++li) {
        const std::string& l = lines[li];
        std::size_t i = 0;
        if (!seen_eq) {
            const auto eq = l.find('=');
            if (eq == std::string::npos) return start;
            seen_eq = true;
            i = eq + 1;
        }
        for (; i < l.size(); ++i) {
            const char c = l[i];
            if (str == MultiBasic) {
                if (c == '\\') { ++i; continue; }
                if (l.compare(i, 3, "\"\"\"") == 0) { str = None; i += 2; }
                continue;
            }
            if (str == MultiLiteral) {
                if (l.compare(i, 3, "'''") == 0) { str = None; i += 2; }
                continue;
            }
            if (str == Basic) {
                if (c == '\\') { ++i; continue; }
                if (c == '"') str = None;
                continue;
            }
            if (str == Literal) {
                if (c == '\'') str = None;
                continue;
            }
            if (l.compare(i, 3, "\"\"\"") == 0) { str = MultiBasic; i += 2; continue; }
            if (l.compare(i, 3, "'''") == 0) { str = MultiLiteral; i += 2; continue; }
            if (c == '"') { str = Basic; continue; }
            if (c == '\'') { str = Literal; continue; }
            if (c == '#') break;
            if (c == '[' || c == '{') ++depth;
            else if ((c == ']' || c == '}') && depth > 0) --depth;
        }
        // 单行字符串不会跨行：没收口就是写坏了的一行，别把后面的吞进来。
        if (str == Basic || str == Literal) str = None;
        if (depth == 0 && str == None) return li;
    }
    return lines.size() - 1;   // 到文件尾都没收口：整段都算它的
}

std::vector<std::string> split_lines(const std::string& s, bool& had_final_nl) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == '\n') {
            // 行尾的 \r 单独留着，写回去时原样带上——
            // 文件本来是 CRLF 的话，改一行不该把它变成混合换行
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    had_final_nl = !s.empty() && s.back() == '\n';
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string join_lines(const std::vector<std::string>& lines, bool final_nl) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || final_nl) out += "\n";
    }
    return out;
}

/// 这一行用的是不是 CRLF。改行时要跟着。
bool is_crlf(const std::string& line) {
    return !line.empty() && line.back() == '\r';
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

/// TOML 基本字符串（带双引号）。**控制字符要转义**：原来只转反斜杠和双引号，
/// 以为"这个配置里不会出现控制字符"——而字符串是从接口进来的（命令模板、
/// 口令、路径都是人贴的），贴进来一个换行，写出去的那一行在引号中间断开，
/// 下次读配置整份报错，所有设置一起没了。换行、制表这些用短写法，其余
/// （含 DEL）写成 \uXXXX。
static std::string toml_basic_string(const std::string& in) {
    std::string out = "\"";
    for (const char c : in) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (u < 0x20 || u == 0x7f) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", u);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

/// TOML 的裸键只允许 A-Z a-z 0-9 _ -，别的都要加引号。
///
/// **不加就把整份配置写坏了。** 2026-09-17 实测：出片那一组的替换档按
/// `video/video_llm` 这种键记进 `[models.pick]`，写出来是裸的
/// `video/video_llm = "…"`，下一次读配置直接 "Error while parsing
/// key-value pair"——**整个项目打不开了**，而写的时候一声不吭。
std::string toml_key(const std::string& key) {
    const auto bare = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    };
    bool ok = !key.empty();
    for (const char c : key) {
        if (!bare(c)) { ok = false; break; }
    }
    if (ok) return key;
    return toml_basic_string(key);
}

std::string to_toml_literal(const json& v) {
    if (v.is_string()) {
        return toml_basic_string(v.get<std::string>());
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        const double d = v.get<double>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.10g", d);
        std::string s(buf);
        // **必须带小数点。** 写成 `1` 的话下次读回来是整数，
        // 而对应的字段类型是 float，toml++ 取值时会拒绝，
        // 表现是整份配置加载失败——一次写回把配置写坏了。
        if (s.find('.') == std::string::npos &&
            s.find('e') == std::string::npos &&
            s.find("inf") == std::string::npos &&
            s.find("nan") == std::string::npos) {
            s += ".0";
        }
        return s;
    }
    if (v.is_null()) {
        // TOML 没有 null。Python 那边写回时把 None 换成空串，照抄。
        return "\"\"";
    }
    if (v.is_array()) {
        // `off = ["llm", "video"]` 就是这一种。
        std::string out = "[";
        bool first = true;
        for (const auto& e : v) {
            if (!first) out += ", ";
            first = false;
            out += to_toml_literal(e);
        }
        return out + "]";
    }
    // 对象这里用不到（内联表 `{a = 1}` 全仓一处都不写）。真传进来了就
    // dump 成 JSON——那不是合法 TOML，下次加载会报错，但比静默写一个空值强。
    return v.dump();
}

namespace {

/// 一行是不是**没被注释掉的**数组表头 `[[xxx]]`，是的话返回里面那个名字。
bool array_header(const std::string& line, std::string& name) {
    const std::string t = text::strip_ws(line);
    if (t.size() < 5 || t.compare(0, 2, "[[") != 0) return false;
    const auto close = t.find("]]");
    if (close == std::string::npos) return false;
    name = text::strip_ws(t.substr(2, close - 2));
    return !name.empty();
}

/// 一行是不是任意一种节头（`[x]` 或 `[[x]]`）。删块时用它找块的结尾。
bool any_header(const std::string& line) {
    const std::string t = text::strip_ws(line);
    return t.size() >= 3 && t.front() == '[' && t.find(']') != std::string::npos;
}

/// 两个写回（`save_user_config` / `save_peer_nodes`）共用一把。
///
/// 两个都是「读整份 → 改几行 → 写整份」同一个 config.toml。设置页存一下、
/// 机器表加一台同时发生的话，各读各写，后写的那份把前一份的改动冲掉，一声
/// 不响（2026-09-25 审出来：`peer_edit_mu` 只管机器表那几条自己之间）。
/// 种子文件那个固定名字（`.changji_config_seed.toml`）两件并着也会互相踩。
std::mutex& write_mu() {
    static std::mutex mu;
    return mu;
}

/// 整份换，**不原地截断重写**：写到旁边一个独一份的临时文件、写全了再换名。
/// 原地 trunc 的话写到一半进程没了、盘满了，config.toml 就剩半截——下次起来
/// 所有设置全没；同时来读的那一下也会读到半截。
///
/// 机器那份 config.toml **一律 0600**：机器表每一行的口令就写在里头
///（`[[peer.nodes]].token`），原来临时文件按默认 umask 建（0644），换名之后
/// 同机别的账号都读得到。片子里那份（changji.toml）只有电影的设置，沿用它原来
/// 的权限。是个链接（dotfiles）时写到它指着的那份上，不把链接换掉。
void write_text_atomic(const fs::path& target, const std::string& text) {
    // **写之前先自己读一遍。** 这儿是逐行编辑（为了留住注释），碰上没见过的
    // 写法就可能编出一份读不了的——换名之后下次起来所有设置全没。读不了就
    // 不写，原来那份原样留着，抛出去让调用的那头照实说。
    try {
        (void)toml::parse(text);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(SAYF("无法写入 %1", paths::to_utf8(target)) + " (" +
                                 std::string(e.description()) + ")");
    }
    std::error_code ec;
    const bool machine =
        fs::weakly_canonical(target, ec) == fs::weakly_canonical(user_config_path(), ec);
    try {
        util::write_file_atomic(target, text, /*private_only=*/machine);
    } catch (const std::exception&) {
        throw std::runtime_error(SAYF("无法写入 %1", paths::to_utf8(target)));
    }
}

}  // namespace

fs::path save_peer_nodes(const json& nodes,
                         const std::optional<fs::path>& path) {
    const std::lock_guard<std::mutex> lock(write_mu());
    const fs::path target = path ? *path : user_config_path();
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);

    std::string original = read_file(target);
    if (original.empty()) {
        // 同 save_user_config：文件不在就从内置模板起步，注释也一起有了。
        const fs::path tmp = target.parent_path() /
                             paths::from_utf8(".changji_peers_seed.toml");
        write_default_config(tmp);
        original = read_file(tmp);
        fs::remove(tmp, ec);
    }

    bool final_nl = true;
    const std::vector<std::string> lines = split_lines(original, final_nl);
    // 文件本来是 CRLF 的话，新写的几行也要跟着——同一份文件里混着两种
    // 换行，git 和编辑器都会闹。判据取第一行（空文件时按 LF）。
    const bool crlf = !lines.empty() && is_crlf(lines.front());
    const std::string eol = crlf ? "\r" : "";

    // **先把已有的块整段删掉。** 一个块从 `[[peer.nodes]]` 那一行起，
    // 到下一个节头（任意一种）之前为止。注释掉的示例块以 `#` 开头，
    // array_header 认不出它，所以原样留着。
    std::vector<std::string> kept;
    kept.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size();) {
        std::string name;
        if (array_header(lines[i], name) && name == "peer.nodes") {
            ++i;
            while (i < lines.size() && !any_header(lines[i])) ++i;
            continue;
        }
        kept.push_back(lines[i]);
        ++i;
    }
    // 删完之后末尾多半留着一串空行（原来那几个块之间的），收掉。
    while (!kept.empty() && text::strip_ws(kept.back()).empty()) kept.pop_back();

    const auto emit = [&](const std::string& key, const json& v) {
        kept.push_back(toml_key(key) + " = " + to_toml_literal(v) + eol);
    };
    if (nodes.is_array()) {
        for (const auto& n : nodes) {
            if (!n.is_object()) continue;
            const std::string url = n.value("url", std::string());
            if (url.empty()) continue;   // 没地址的一行写出去也没用
            // 场记云那一台是虚的（登着才有），写进去就留下一台带着作废钥匙的机器
            if (cloud::is_cloud_node(url)) continue;
            kept.push_back(eol);
            kept.push_back("[[peer.nodes]]" + eol);
            emit("url", url);
            // **口令空着就不写这一行。** 写 `token = ""` 和不写是一个意思
            // （留空就用 `[peer].token`），而文件里多一行空值，下次人来读
            // 会以为这台特意设了个空口令。
            if (const auto t = n.value("token", std::string()); !t.empty()) {
                emit("token", t);
            }
            if (const auto it = n.find("off");
                it != n.end() && it->is_array() && !it->empty()) {
                emit("off", *it);
            }
        }
    }

    const std::string text = join_lines(kept, true);
    write_text_atomic(target, text);
    return target;
}

fs::path save_user_config(const json& patch,
                          const std::optional<fs::path>& path) {
    const std::lock_guard<std::mutex> lock(write_mu());
    const fs::path target = path ? *path : user_config_path();
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);

    std::string original = read_file(target);
    if (original.empty()) {
        // 文件不存在就从内置模板起步，这样注释也一起有了
        const fs::path tmp = target.parent_path() /
                             paths::from_utf8(".changji_config_seed.toml");
        write_default_config(tmp);
        original = read_file(tmp);
        fs::remove(tmp, ec);
    }

    bool final_nl = true;
    std::vector<std::string> lines = split_lines(original, final_nl);

    // 把每个节的范围先扫出来：节头行号，以及这一节最后一个键值行的行号。
    // 用后者而不是节的末尾，是为了让新键插在已有键的后面而不是
    // 尾随注释的后面——那些注释多半是在解释下一节。
    struct SectionInfo {
        std::size_t header = 0;
        std::size_t last_key = 0;
        bool has_any_key = false;
        std::map<std::string, std::size_t> keys;
    };
    std::map<std::string, SectionInfo> sections;
    std::string current;
    SectionInfo* cur = nullptr;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string name;
        if (section_header(lines[i], name)) {
            current = name;
            sections[name].header = i;
            cur = &sections[name];
            continue;
        }
        // **数组表 `[[peer.nodes]]` 也是节的结尾。** section_header 不认它
        // （这儿不改数组表，理由见它上面），但不认不等于"还在上一节里"：
        // 2026-09-19 实撞——`[models]` 后面紧跟着 `[[peer.nodes]]`，往
        // `[models]` 里加一个新键 `llm`，扫到的"这一节最后一个键"是
        // peer.nodes 的 `token`，新键于是插进了对等机那一块。内存里对、
        // 盘上错，重启就丢；项目页按盘上那份认，人看到的是"没保存上"。
        if (array_header(lines[i], name)) {
            current.clear();
            cur = nullptr;
            continue;
        }
        std::string key;
        if (key_line(lines[i], key)) {
            // 多行的值（数组、长字符串）整段算这一个键的，里头的行不再扫
            const std::size_t end = value_end(lines, i);
            if (cur != nullptr) {
                cur->keys[key] = i;
                cur->last_key = end;
                cur->has_any_key = true;
            }
            i = end;
        }
    }

    // 顶层的键（不在任何节里）单独记。vram_gb_override 就是这种。
    std::map<std::string, std::size_t> top_keys;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (any_header(lines[i])) break;  // 到第一个节头（含数组表）就停
        std::string key;
        if (key_line(lines[i], key)) {
            top_keys[key] = i;
            i = value_end(lines, i);
        }
    }

    // 收集要改的行和要插的行。**先收集再统一施工**——边遍历边插会让
    // 前面记下来的行号全部失效。
    std::map<std::size_t, std::string> replacements;              // 行号 -> 新内容
    std::set<std::size_t> dropped;   // 换掉的多行值后面那几行
    std::map<std::size_t, std::vector<std::string>> insertions;   // 插在这一行之后
    std::vector<std::string> appended;                            // 追加到文件末尾

    const auto make_line = [](const std::string& key, const json& value,
                              bool crlf) {
        return toml_key(key) + " = " + to_toml_literal(value) + (crlf ? "\r" : "");
    };
    // 换掉一个键：多行的值连后面那几行一起换，不然残行留在文件里整份读不了。
    const auto replace_at = [&](std::size_t at, std::string line) {
        replacements[at] = std::move(line);
        const std::size_t end = value_end(lines, at);
        for (std::size_t k = at + 1; k <= end; ++k) dropped.insert(k);
    };

    for (const auto& item : patch.items()) {
        const std::string& section = item.key();
        const json& values = item.value();

        if (!values.is_object()) {
            // 顶层标量
            const auto it = top_keys.find(section);
            if (it != top_keys.end()) {
                replace_at(it->second,
                           make_line(section, values, is_crlf(lines[it->second])));
            } else {
                // 顶层键要插在**第一个节头之前**，不然它会被算进那一节里
                std::size_t first_header = lines.size();
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (any_header(lines[i])) {   // 数组表也是节头，理由见上面扫节那段
                        first_header = i;
                        break;
                    }
                }
                if (first_header == 0) {
                    // 文件头一行就是节头，只能插在最前面
                    insertions[static_cast<std::size_t>(-1)].push_back(
                        make_line(section, values, false));
                } else {
                    insertions[first_header - 1].push_back(
                        make_line(section, values, false));
                }
            }
            continue;
        }

        const auto sit = sections.find(section);
        if (sit == sections.end()) {
            // 整节都没有，追加到文件末尾
            appended.push_back("");
            appended.push_back("[" + section + "]");
            for (const auto& kv : values.items()) {
                appended.push_back(make_line(kv.key(), kv.value(), false));
            }
            continue;
        }

        SectionInfo& info = sit->second;
        for (const auto& kv : values.items()) {
            const auto kit = info.keys.find(kv.key());
            if (kit != info.keys.end()) {
                replace_at(kit->second,
                           make_line(kv.key(), kv.value(), is_crlf(lines[kit->second])));
            } else {
                const std::size_t after =
                    info.has_any_key ? info.last_key : info.header;
                insertions[after].push_back(
                    make_line(kv.key(), kv.value(), is_crlf(lines[after])));
            }
        }
    }

    std::vector<std::string> out;
    out.reserve(lines.size() + 16);
    // 插在最前面的（罕见：文件头一行就是节头）
    const auto pre = insertions.find(static_cast<std::size_t>(-1));
    if (pre != insertions.end()) {
        for (const auto& l : pre->second) out.push_back(l);
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto rep = replacements.find(i);
        if (!dropped.count(i)) {
            out.push_back(rep != replacements.end() ? rep->second : lines[i]);
        }
        const auto ins = insertions.find(i);
        if (ins != insertions.end()) {
            for (const auto& l : ins->second) out.push_back(l);
        }
    }
    for (const auto& l : appended) out.push_back(l);

    const std::string text = join_lines(out, final_nl);
    write_text_atomic(target, text);
    return target;
}

}  // namespace changji::config
