#include "stages/story_import.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

/// 中文数字和阿拉伯数字，用来认「第三章」「第12节」。
bool is_number_char(const std::string& ch) {
    if (ch.size() == 1) {
        const char c = ch[0];
        return c >= '0' && c <= '9';
    }
    static const char* kCn[] = {"零", "一", "二", "三", "四", "五", "六",
                                "七", "八", "九", "十", "百", "千", "两"};
    for (const char* n : kCn) {
        if (ch == n) return true;
    }
    return false;
}

/// 这一行是不是章节标题。
///
/// 认四种写法。**宁可漏认不要错认**：错认会把正文中间的一句话当成章名，
/// 那一章从此从半句话开始；漏认最多是退回按字数切，切点照样落在段落边界上。
bool is_heading(const std::string& line, std::string& title_out) {
    const std::string s = text::strip_ws(line);
    if (s.empty()) return false;
    // 标题行都很短。正文里出现「第三章」字样的长句子不该被认成标题。
    if (text::utf8_len(s) > 40) return false;

    // ## 标题
    if (s[0] == '#') {
        std::size_t i = 0;
        while (i < s.size() && s[i] == '#') ++i;
        if (i > 6) return false;
        const std::string rest = text::strip_ws(s.substr(i));
        if (rest.empty()) return false;
        title_out = rest;
        return true;
    }

    // Chapter 3 / CHAPTER 12
    if (s.size() > 8) {
        std::string head = s.substr(0, 7);
        std::transform(head.begin(), head.end(), head.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (head == "chapter") {
            title_out = s;
            return true;
        }
    }

    const std::vector<std::string> chars = text::utf8_chars(s);

    // 楔子、序章、尾声这类独立成行的
    static const char* kStandalone[] = {"楔子", "序章", "序幕", "序",
                                        "尾声", "终章", "后记", "番外"};
    for (const char* w : kStandalone) {
        if (s == w || (s.rfind(w, 0) == 0 && text::utf8_len(s) <= 12)) {
            title_out = s;
            return true;
        }
    }

    // 第N章 / 第N节 / 第N回
    if (!chars.empty() && chars[0] == "第") {
        std::size_t i = 1;
        while (i < chars.size() && is_number_char(chars[i])) ++i;
        if (i > 1 && i < chars.size()) {
            const std::string& unit = chars[i];
            if (unit == "章" || unit == "节" || unit == "回" || unit == "话" ||
                unit == "幕") {
                title_out = s;
                return true;
            }
        }
    }
    return false;
}

/// 候选切点太多就等距抽稀。相邻两个段落边界差不了几个字，抽掉无所谓。
std::vector<int> thin_out(std::vector<int> v, std::size_t cap) {
    if (v.size() <= cap) return v;
    std::vector<int> out;
    const double step = static_cast<double>(v.size()) / static_cast<double>(cap);
    for (std::size_t k = 0; k < cap; ++k) {
        out.push_back(v[static_cast<std::size_t>(k * step)]);
    }
    return out;
}

std::string chapter_id(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ch%02zu", i + 1);
    return buf;
}

/// 取头一句当章名。没有标题行时用。
std::string first_sentence(const std::string& body, std::size_t max_chars = 16) {
    const std::vector<std::string> chars = text::utf8_chars(text::strip_ws(body));
    std::string out;
    for (std::size_t i = 0; i < chars.size() && i < max_chars; ++i) {
        const std::string& c = chars[i];
        if (c == "\n") break;
        if (c == "。" || c == "！" || c == "？" || c == "，") break;
        out += c;
    }
    return text::strip_ws(out);
}

}  // namespace

/// 段落边界：空行之后那个位置（按字符计），以及每个自然段的末尾。
///
/// 没有空行的文本（很多网文一行一段）就用换行当段落边界，不然一整章
/// 只有章界一个候选。
std::vector<int> paragraph_breaks(const std::string& body) {
    const std::vector<std::string> chars = text::utf8_chars(body);
    std::vector<int> breaks;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        if (chars[i] != "\n") continue;
        // 连着的换行算一处，位置取最后一个换行之后
        std::size_t j = i;
        while (j + 1 < chars.size() && chars[j + 1] == "\n") ++j;
        const int at = static_cast<int>(j + 1);
        if (at > 0 && at < static_cast<int>(chars.size())) breaks.push_back(at);
        i = j;
    }
    return breaks;
}

std::vector<Hook> paragraph_hooks(const std::string& text) {
    const std::string body = changji::text::strip_ws(text);
    std::vector<Hook> out;
    for (int at : thin_out(paragraph_breaks(body), kImportMaxHooks)) {
        Hook h;
        h.at_char = at;
        // **text 留空是刻意的。** 段落边界不是真正的钩子——真钩子要读懂剧情
        // 才找得出来。留空就等于说「这里可以切，但说不出为什么」，界面上
        // 显示成「章尾」而不是编一句假的钩子出来。
        out.push_back(std::move(h));
    }
    return out;
}

namespace {

Chapter make_chapter(std::size_t index, const std::string& title,
                     const std::string& body) {
    Chapter c;
    c.chapter_id = chapter_id(index);
    c.text = text::strip_ws(body);
    c.title = text::strip_ws(title);
    if (c.title.empty()) c.title = first_sentence(c.text);
    if (c.title.empty()) c.title = "第 " + std::to_string(index + 1) + " 章";

    c.hooks = paragraph_hooks(c.text);
    return c;
}

}  // namespace

std::vector<Chapter> split_pasted(const std::string& text, int target_chars) {
    std::vector<Chapter> out;
    const std::string body = text::strip_ws(text);
    if (body.empty()) return out;

    // 统一换行。粘进来的东西十有八九来自 Windows 或者网页，
    // \r 留着会混进章名和正文，最后出现在字幕上。
    std::string norm;
    norm.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\r') {
            if (i + 1 < body.size() && body[i + 1] == '\n') continue;
            norm += '\n';
            continue;
        }
        norm += body[i];
    }

    // ---- 先找标题行 ----
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : norm) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        lines.push_back(cur);
    }

    std::vector<std::size_t> heads;
    std::vector<std::string> titles;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t;
        if (is_heading(lines[i], t)) {
            heads.push_back(i);
            titles.push_back(t);
        }
    }

    // 两个以上标题才算「作者自己分过章」。只有一个的话多半是书名。
    if (heads.size() >= 2) {
        for (std::size_t k = 0; k < heads.size(); ++k) {
            const std::size_t from = heads[k] + 1;
            const std::size_t to = (k + 1 < heads.size()) ? heads[k + 1] : lines.size();
            std::string chunk;
            for (std::size_t i = from; i < to; ++i) {
                if (i > from) chunk += "\n";
                chunk += lines[i];
            }
            if (text::strip_ws(chunk).empty()) continue;
            out.push_back(make_chapter(out.size(), titles[k], chunk));
        }
        if (!out.empty()) return out;
    }

    // ---- 一个标题都认不出来：按字数在段落边界上切 ----
    const std::vector<std::string> chars = text::utf8_chars(norm);
    const int total = static_cast<int>(chars.size());
    const int target = target_chars > 0 ? target_chars : kImportTargetChars;

    std::vector<int> breaks = paragraph_breaks(norm);
    breaks.push_back(total);

    int pos = 0;
    while (pos < total) {
        int next = total;
        // 剩下的不到一章半就整块收尾，别留一个几百字的尾巴
        if (total - pos > target * 3 / 2) {
            const int ideal = pos + target;
            int best = -1;
            long best_d = 0;
            for (int b : breaks) {
                if (b <= pos) continue;
                const long d = std::labs(static_cast<long>(b) - ideal);
                if (best < 0 || d < best_d) {
                    best = b;
                    best_d = d;
                } else {
                    break;  // breaks 有序，距离先减后增
                }
            }
            if (best > pos) next = best;
        }
        std::string chunk;
        for (int i = pos; i < next; ++i) chunk += chars[i];
        if (!text::strip_ws(chunk).empty()) {
            out.push_back(make_chapter(out.size(), "", chunk));
        }
        pos = next;
    }
    return out;
}

}  // namespace changji::stages
