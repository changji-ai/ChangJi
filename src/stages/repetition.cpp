#include "stages/repetition.hpp"

#include <algorithm>
#include <map>

#include "util/text.hpp"

namespace changji::stages {

namespace {

/// 句末标点。中英文都收——粘进来的稿子可能是英文标点。
bool ends_sentence(const std::string& ch) {
    static const char* kEnders[] = {"。", "！", "？", "…", "；",
                                    ".", "!",  "?",  ";"};
    for (const char* e : kEnders) {
        if (ch == e) return true;
    }
    return false;
}

/// 比对用的形态：去掉首尾空白，再去掉引号之类的包裹。
///
/// **不这么做会漏掉一半。** 退化循环里同一句话常常一次带引号一次不带
/// （`'你早就走了'` 和 `你早就走了`），按原样比的话它们算两句，
/// 一句也到不了三次。
std::string normalize_impl(const std::string& s) {
    std::string out = text::strip_ws(s);
    static const char* kWrap[] = {"'", "'", "\"", "\"", "「", "」",
                                  "‘",  "’",  "“",  "”",  "『", "』"};
    bool changed = true;
    while (changed && !out.empty()) {
        changed = false;
        for (const char* w : kWrap) {
            const std::string ws = w;
            if (out.size() >= ws.size() && out.compare(0, ws.size(), ws) == 0) {
                out = out.substr(ws.size());
                changed = true;
            }
            if (out.size() >= ws.size() &&
                out.compare(out.size() - ws.size(), ws.size(), ws) == 0) {
                out = out.substr(0, out.size() - ws.size());
                changed = true;
            }
        }
        out = text::strip_ws(out);
    }
    return out;
}

}  // namespace

std::string repeat_key(const std::string& s) { return normalize_impl(s); }

std::vector<std::string> split_sentences(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const std::string& ch : text::utf8_chars(text)) {
        if (ch == "\n") {
            if (!text::strip_ws(cur).empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += ch;
        if (ends_sentence(ch)) {
            // **标点后面跟着的引号要带上**是做不到的（这里一个字一个字走，
            // 看不到后面）。留给 normalize 去掉包裹，效果一样。
            if (!text::strip_ws(cur).empty()) out.push_back(cur);
            cur.clear();
        }
    }
    if (!text::strip_ws(cur).empty()) out.push_back(cur);
    return out;
}

RepetitionReport check_repetition(const std::string& text) {
    RepetitionReport r;
    const std::vector<std::string> sentences = split_sentences(text);
    if (sentences.empty()) return r;

    std::map<std::string, int> seen;
    std::size_t total_chars = 0;
    std::size_t unique_chars = 0;
    for (const std::string& s : sentences) {
        const std::string key = repeat_key(s);
        const std::size_t n = text::utf8_len(key);
        total_chars += n;
        // 太短的句子不进统计，也不算进比例：「他说。」重复十次是正常的，
        // 算进去只会把比例拉低、冤枉一段好文。
        if (n < kRepeatMinSentenceChars) {
            unique_chars += n;
            continue;
        }
        const int count = ++seen[key];
        if (count == 1) unique_chars += n;
        if (count > r.worst_count) {
            r.worst_count = count;
            r.worst = key;
        }
    }

    r.unique_ratio =
        total_chars == 0 ? 1.0
                         : static_cast<double>(unique_chars) /
                               static_cast<double>(total_chars);

    // **整块重来一遍。** 前两条都抓不住它：每句只出现两次，够不着三次那条；
    // 十几句重复在一两千字里也拉不低去重比。判据是"连着几句都在别处出现
    // 过"——零星撞车到不了五句连着，而真复制一段情节起步就是十几句。
    int run = 0;
    for (const std::string& sentence : sentences) {
        const std::string key = repeat_key(sentence);
        if (text::utf8_len(key) < kRepeatMinSentenceChars) continue;
        if (seen[key] > 1) {
            ++run;
            if (run > r.worst_run) r.worst_run = run;
        } else {
            run = 0;
        }
    }

    if (r.worst_count >= kRepeatMaxSame) {
        r.ok = false;
        r.detail = "这一句出现了 " + std::to_string(r.worst_count) + " 次：「" +
                   text::truncate_utf8(r.worst, 30) + "」。模型卡在复读里了";
        return r;
    }
    if (r.worst_run >= kRepeatMaxRun) {
        r.ok = false;
        r.detail = "有 " + std::to_string(r.worst_run) +
                   " 句连着和别处一模一样，整块重来了一遍";
        return r;
    }
    if (r.unique_ratio < kRepeatMinUniqueRatio) {
        r.ok = false;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", r.unique_ratio * 100.0);
        r.detail = std::string("去掉重复的句子之后只剩 ") + buf +
                   "%，整段在原地打转";
        return r;
    }
    return r;
}

}  // namespace changji::stages
