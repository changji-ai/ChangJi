#include "stages/json_partial.hpp"

#include <cctype>

namespace changji::stages {

namespace {

/// 扫一遍原文，记下三件事：结构栈、有没有停在字符串里、这个字符串是键还是值。
struct Scan {
    std::vector<char> stack;   ///< 开着的 '{' / '['，栈顶在末尾
    bool in_string = false;
    bool string_is_key = false;
    /// 停在字符串里时，那个开引号的下标。
    std::size_t string_at = 0;
    /// 最后一个**有意义**的结构字符的下标（不含空白，不含字符串内部）。
    std::size_t last_struct = std::string::npos;
    char last_struct_ch = '\0';
};

Scan scan(const std::string& s) {
    Scan st;
    bool escaped = false;
    // 对象里下一个字符串是键还是值：碰到 '{' 或对象里的 ',' 之后是键，
    // 碰到 ':' 之后是值。数组里一律算值。
    bool expect_key = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (st.in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                st.in_string = false;
            }
            continue;
        }
        switch (c) {
            case '"':
                st.in_string = true;
                st.string_is_key = expect_key;
                st.string_at = i;
                break;
            case '{':
                st.stack.push_back('{');
                expect_key = true;
                st.last_struct = i;
                st.last_struct_ch = c;
                break;
            case '[':
                st.stack.push_back('[');
                expect_key = false;
                st.last_struct = i;
                st.last_struct_ch = c;
                break;
            case '}':
            case ']':
                if (!st.stack.empty()) st.stack.pop_back();
                expect_key = !st.stack.empty() && st.stack.back() == '{';
                st.last_struct = i;
                st.last_struct_ch = c;
                break;
            case ':':
                expect_key = false;
                st.last_struct = i;
                st.last_struct_ch = c;
                break;
            case ',':
                expect_key = !st.stack.empty() && st.stack.back() == '{';
                st.last_struct = i;
                st.last_struct_ch = c;
                break;
            default:
                break;
        }
    }
    return st;
}

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

std::string close_partial_json(const std::string& raw) {
    // 前面那截空白和 markdown 围栏（有的模型照样会加 ```json）掐掉。
    std::size_t begin = 0;
    while (begin < raw.size() && is_ws(raw[begin])) ++begin;
    if (raw.compare(begin, 7, "```json") == 0) begin += 7;
    else if (raw.compare(begin, 3, "```") == 0) begin += 3;
    std::string s = raw.substr(begin);
    if (const auto fence = s.rfind("```"); fence != std::string::npos) {
        s = s.substr(0, fence);
    }
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    if (s.empty()) return {};

    Scan st = scan(s);

    if (st.in_string) {
        if (st.string_is_key) {
            // 半个键：`{"cha` —— 补成什么都解不出来（键补齐了还缺值，
            // 值补上了又是瞎猜）。整段掐掉，连它前面那个逗号一起。
            s = s.substr(0, st.string_at);
            while (!s.empty() && (is_ws(s.back()) || s.back() == ',')) s.pop_back();
        } else {
            // 半个值：**留着**，这正是"正在长出来的那句话"。
            // 结尾是落单的反斜杠时先掐掉它，不然 `"abc\` 补个引号还是坏的。
            if (!s.empty() && s.back() == '\\') {
                std::size_t back = 0;
                for (std::size_t i = s.size(); i-- > 0 && s[i] == '\\';) ++back;
                if (back % 2 == 1) s.pop_back();
            }
            s += '"';
        }
        st = scan(s);
    }

    // 结尾是 `:` 或者 `,`：一个还没开始写的值 / 一个还没来的项。
    while (!s.empty()) {
        while (!s.empty() && is_ws(s.back())) s.pop_back();
        if (s.empty()) break;
        if (s.back() == ':') {
            s += "null";     // 键已经在了，给它一个空值，比掐掉键省事
            break;
        }
        if (s.back() == ',') {
            s.pop_back();    // 孤零零的逗号
            continue;
        }
        break;
    }

    // 结尾停在一个没写完的**裸词**上（数字、true/false/null 的前几个字母）。
    // `12.` 和 `tru` 都解不了，掐到上一个结构字符。
    {
        std::size_t i = s.size();
        while (i > 0 && !is_ws(s[i - 1]) && s[i - 1] != ',' && s[i - 1] != ':' &&
               s[i - 1] != '{' && s[i - 1] != '[' && s[i - 1] != '"' &&
               s[i - 1] != '}' && s[i - 1] != ']') {
            --i;
        }
        if (i < s.size()) {
            const std::string tail = s.substr(i);
            const bool ok_word = tail == "true" || tail == "false" || tail == "null";
            bool ok_number = !tail.empty();
            for (char c : tail) {
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' &&
                    c != '+' && c != 'e' && c != 'E' && c != '.') {
                    ok_number = false;
                    break;
                }
            }
            if (ok_number && (tail.back() == '.' || tail.back() == '-' ||
                              tail.back() == 'e' || tail.back() == 'E' ||
                              tail.back() == '+')) {
                ok_number = false;   // 数字写了一半
            }
            if (!ok_word && !ok_number) {
                s = s.substr(0, i);
                while (!s.empty() && (is_ws(s.back()) || s.back() == ',')) s.pop_back();
                if (!s.empty() && s.back() == ':') s += "null";
                st = scan(s);
            }
        }
    }

    // 按相反顺序把还开着的括号补上。
    for (std::size_t i = st.stack.size(); i-- > 0;) {
        s += st.stack[i] == '{' ? '}' : ']';
    }
    return s;
}

nlohmann::json PartialJson::snapshot() const {
    const std::string closed = close_partial_json(raw_);
    if (closed.empty()) return nullptr;
    // **不抛异常**：解不出来就是这一帧不推，下一段 token 到了多半就好了。
    // parse 的第三个参数是 false = 不抛，失败时回的是 discarded，
    // 而 discarded 的 is_null() 是**假**——不换成真正的 null 的话，
    // 调用方那句 `if (snap.is_null()) return;` 就漏掉了这种情况，
    // 然后在一个 discarded 上取字段，抛的是 type_error。
    auto v = nlohmann::json::parse(closed, nullptr, false);
    return v.is_discarded() ? nlohmann::json(nullptr) : v;
}

}  // namespace changji::stages
