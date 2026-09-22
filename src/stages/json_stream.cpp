#include "stages/json_stream.hpp"

#include <utility>

namespace changji::stages {

namespace {

/// 一个码点编成 UTF-8。
///
/// 不借 nlohmann：那边的转换藏在解析器里，拿不到单个码点这一层。
void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

JsonFieldStreamer::JsonFieldStreamer(std::string field, bool repeating)
    : field_(std::move(field)), repeating_(repeating) {}

/// 这一串末尾有没有一个没收完的 UTF-8 字符；有的话返回它起始那一位。
///
/// UTF-8 的首字节自带长度（110xxxxx 两字节、1110xxxx 三字节、11110xxx 四
/// 字节），从末尾往回找最近的一个首字节，看后面够不够它要的长度就行。
/// 往回最多看三位——再长的序列不存在。
std::size_t incomplete_tail(const std::string& s) {
    const std::size_t n = s.size();
    for (std::size_t back = 1; back <= 3 && back <= n; ++back) {
        const auto c = static_cast<unsigned char>(s[n - back]);
        if ((c & 0xC0) == 0x80) continue;  // 续字节，接着往回找
        std::size_t need = 0;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return n;  // ASCII 或者非法字节：到这儿为止都是完整的
        return back < need ? n - back : n;
    }
    return n;
}

std::string JsonFieldStreamer::feed(const std::string& raw_piece) {
    // 上一次扣下来的半个字符补在前面。
    const std::string piece = tail_.empty() ? raw_piece : tail_ + raw_piece;
    tail_.clear();
    std::string fresh;
    for (const char c : piece) {
        switch (state_) {
            case State::Done:
                return fresh;

            case State::SeekKey:
                if (c == '"') {
                    key_.clear();
                    key_escape_ = false;
                    state_ = State::InKey;
                }
                break;

            case State::InKey:
                if (key_escape_) {
                    key_.push_back(c);
                    key_escape_ = false;
                } else if (c == '\\') {
                    key_escape_ = true;
                } else if (c == '"') {
                    state_ = State::AfterKey;
                } else {
                    key_.push_back(c);
                }
                break;

            case State::AfterKey:
                if (c == ':') {
                    // 是要找的那个键才往下走；不是的话回去继续找。
                    // **回 SeekKey 而不是跳过整个值**：值里的引号会被当成
                    // 新键的开头，于是"下一个键"可能落在值内部——但那一层
                    // 的内容不会正好等于 field_，所以不会误触发。真正会出
                    // 问题的是值里恰好有 `"text":` 这么一串，而那要正文里
                    // 出现一段 JSON 才可能。
                    state_ = (key_ == field_) ? State::SeekValue : State::SeekKey;
                } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    state_ = State::SeekKey;
                }
                break;

            case State::SeekValue:
                if (c == '"') {
                    state_ = State::InValue;
                } else if (c == '[') {
                    // **一串字符串。** 章节正文的 schema 就是这个形状
                    // （paragraphs 一段一项）。2026-09-11 栽过一次：改
                    // schema 那次这里还只认字符串，于是流式一个字都抠不
                    // 出来——界面上就是"AI 写作没有热更新"，而后端不报
                    // 任何错，正文最后照样落库，所以查起来毫无线索。
                    array_ = true;
                    state_ = State::SeekItem;
                } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    // 既不是字符串也不是数组（数字、对象）。这一层不管。
                    state_ = State::SeekKey;
                }
                break;

            case State::SeekItem:
                if (c == '"') {
                    // 两项之间补分隔符。**补在下一项开头、不补在上一项结尾**：
                    // 补在结尾的话最后一项后面会多出一个，而流式是边看边写的，
                    // 那个多出来的换行会一直挂在光标前面。
                    if (wrote_any_) {
                        fresh += kArraySeparator;
                        out_ += kArraySeparator;
                    }
                    state_ = State::InValue;
                } else if (c == ']') {
                    // 这个键还会再出现（scenes[].paragraphs 一场一个数组）
                    // 就回去接着找下一个，别在第一场收完就收工。
                    state_ = repeating_ ? State::SeekKey : State::Done;
                }
                // 逗号和空白跳过
                break;

            case State::InValue:
                if (c == '\\') {
                    state_ = State::Escape;
                } else if (c == '"') {
                    wrote_any_ = true;
                    // 数组里一项收完还有下一项；单个字符串收完就完了——
                    // 除非这个键还会再出现，那就回去接着找。
                    state_ = array_ ? State::SeekItem
                                    : (repeating_ ? State::SeekKey : State::Done);
                } else {
                    fresh.push_back(c);
                    out_.push_back(c);
                }
                break;

            case State::Escape: {
                // **转义可能被切在两个 token 中间。** 这是流式独有的：攒齐
                // 了再解析的代码永远碰不到，而它错了的表现是正文里凭空多出
                // 几个反斜杠。状态机天然管住了——`\` 和后面那个字符分两次
                // 到也没关系。
                state_ = State::InValue;
                switch (c) {
                    case 'n': fresh.push_back('\n'); out_.push_back('\n'); break;
                    case 't': fresh.push_back('\t'); out_.push_back('\t'); break;
                    case 'r': fresh.push_back('\r'); out_.push_back('\r'); break;
                    case 'b': fresh.push_back('\b'); out_.push_back('\b'); break;
                    case 'f': fresh.push_back('\f'); out_.push_back('\f'); break;
                    case 'u':
                        hex_.clear();
                        state_ = State::Unicode;
                        break;
                    // `"` `\` `/` 以及认不出来的，原样吐出去
                    default: fresh.push_back(c); out_.push_back(c); break;
                }
                break;
            }

            case State::Unicode: {
                if (hex_value(c) < 0) {
                    // 不是十六进制：这一串坏了。丢掉，回去接着读值——
                    // 中断整段比吞掉后面几千字好。
                    state_ = State::InValue;
                    break;
                }
                hex_.push_back(c);
                if (hex_.size() < 4) break;
                state_ = State::InValue;

                std::uint32_t cp = 0;
                for (const char h : hex_) {
                    cp = (cp << 4) | static_cast<std::uint32_t>(hex_value(h));
                }
                hex_.clear();

                // 代理对。高位先来，低位配上才是一个码点。
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    high_ = static_cast<std::uint16_t>(cp);
                    break;
                }
                if (cp >= 0xDC00 && cp <= 0xDFFF && high_ != 0) {
                    const std::uint32_t full =
                        0x10000 + ((static_cast<std::uint32_t>(high_) - 0xD800) << 10) +
                        (cp - 0xDC00);
                    high_ = 0;
                    std::string one;
                    append_utf8(one, full);
                    fresh += one;
                    out_ += one;
                    break;
                }
                high_ = 0;
                std::string one;
                append_utf8(one, cp);
                fresh += one;
                out_ += one;
                break;
            }
        }
    }
    // 末尾那半个字符扣下来，等下一段。**out_ 里也不能留**——收尾时调用方
    // 会拿 text() 和权威那份对，留着半个字符两边就对不上。
    const std::size_t cut = incomplete_tail(fresh);
    if (cut < fresh.size()) {
        tail_ = fresh.substr(cut);
        out_.erase(out_.size() - tail_.size());
        fresh.erase(cut);
    }
    return fresh;
}

}  // namespace changji::stages
