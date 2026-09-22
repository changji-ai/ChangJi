#include "stages/story_revise.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "stages/story_import.hpp"
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 按 **UTF-8 字符**切。按字节切会把汉字劈成三段，截出来的是非法 UTF-8，
/// 它会一路流到提示词和字幕，最后表现成「整轨字幕不显示」这种离得很远的
/// 故障。和 script_story.cpp 里那个是同一件事。
std::string slice_chars(const std::string& s, int from, int to) {
    const std::vector<std::string> chars = text::utf8_chars(s);
    const int n = static_cast<int>(chars.size());
    const int a = std::clamp(from, 0, n);
    const int b = std::clamp(to, a, n);
    std::string out;
    for (int i = a; i < b; ++i) out += chars[i];
    return out;
}

const Chapter* find(const Story& story, const std::string& chapter_id) {
    return story.chapter_by_id(chapter_id);
}


/// 拦"改着改着把整章吐回来"。两条路共用。
///
/// 那样落盘之后整章内容会翻倍，而界面上只显示"改好了"——多出来的那一份
/// 要等写剧本时才发现，那时候已经隔了好几步。**只拦上限不拦下限**：
/// "把这段压缩成一句"是正当要求。
///
/// 上限取**两条里松的那条**：
///
///   - 选中字数的六倍。改一小段扩写成六倍，已经远超"改"的范畴。
///   - **整章的六成。** 这条才是"抄整章"的真判据——原来只有上面那条，
///     而倍数在短选区上必然误伤：2026-09-11 实跑，选中 40 字说"把这段拉长
///     一点，多写点环境"，模型写了 643 字（整章 1889 字，它显然没抄整章），
///     却撞上 `max(600, 40×6)` 那个 600 的地板被打回，报错还一口咬定
///     "八成是把整章抄回来了"——指向完全错的方向，用户只会以为模型抽风。
///
/// `whole_chars` 是这一章现在一共多少字，给 0 表示不知道（那就只剩倍数
/// 那条，和以前一样）。
void guard_length(const std::string& text_in, int span_chars, int whole_chars) {
    if (span_chars <= 0) {
        // 空区间（从头写 / 在光标处插一段）没有"选中的六倍"可算。原来直接
        // 放行——模型跑飞了会吐两万字进一章。顶按一章的量给。
        const int got = static_cast<int>(text::utf8_len(text_in));
        if (got > 8000) {
            throw std::runtime_error("写了 " + std::to_string(got) +
                                     " 个字，一章装不下这么多（上限 8000）。"
                                     "让它分几次写，或者说清篇幅");
        }
        return;
    }
    const int by_span = span_chars * 6;
    const int by_whole = whole_chars > 0 ? whole_chars * 3 / 5 : 0;
    const int cap = std::max(600, std::max(by_span, by_whole));
    const int got = static_cast<int>(text::utf8_len(text_in));
    if (got > cap) {
        // **不再猜原因。** 只说事实和下一步该怎么做——猜错的原因比不说更
        // 糟，它把人往错的方向支。
        throw std::runtime_error(
            "改完有 " + std::to_string(got) + " 个字，选中的只有 " +
            std::to_string(span_chars) + " 个，超出这一段能改到的上限（" +
            std::to_string(cap) + " 字）。要大改的话把范围选大一点，"
            "或者用「改整章」");
    }
}

/// 剥掉模型自作主张加的包装。
///
/// 十次里有一两次它会包一层 ``` 代码块、或者在前面写一句「修改后：」。
/// 这些字会**原样落进正文**——而正文是后面写剧本的输入，一句「修改后：」
/// 能一路活到分镜表里去。
std::string unwrap(std::string body) {
    body = text::strip_ws(body);

    // 三个反引号开头的代码块，第一行可能带语言名。
    if (body.rfind("```", 0) == 0) {
        const std::size_t nl = body.find('\n');
        const std::size_t close = body.rfind("```");
        if (nl != std::string::npos && close != std::string::npos && close > nl) {
            body = text::strip_ws(body.substr(nl + 1, close - nl - 1));
        }
    }

    // 开头那句「修改后：」。只剥**第一行就是它**的情况——正文里真出现这
    // 三个字的话（人物说了这句），那是内容，不能动。
    static const char* kLeads[] = {"修改后", "改写后", "改完", "新的一段",
                                   "以下是改完的一段", "以下是修改后的正文"};
    for (const char* lead : kLeads) {
        const std::string want = lead;
        if (body.rfind(want, 0) != 0) continue;
        std::string rest = body.substr(want.size());
        const std::string colon = rest.rfind("：", 0) == 0   ? "："
                                  : rest.rfind(":", 0) == 0 ? ":"
                                                            : "";
        if (colon.empty()) continue;
        body = text::strip_ws(rest.substr(colon.size()));
        break;
    }
    return body;
}

}  // namespace

std::string span_text(const Story& story, const Span& span) {
    const Chapter* c = find(story, span.chapter_id);
    if (c == nullptr) return {};
    return slice_chars(c->text, span.from_char, span.to_char);
}

const ordered& revise_schema() {
    static const ordered schema = [] {
        ordered props = ordered::object();
        props["text"] = {
            {"type", "string"},
            {"description",
             "改完的这一段正文，直接拿去替换选中的那一段。"
             "只写这一段，不要把整章抄回来，也不要带章节标题"}};
        props["note"] = {
            {"type", "string"},
            {"description", "一句话说你改了什么。不进正文"}};
        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = ordered::array({"text"});
        return s;
    }();
    return schema;
}

std::string build_revise_prompt(const Story& story, const Span& span,
                                const std::string& instruction,
                                const std::vector<ReviseTurn>& history,
                                StyleLine style_line, bool plain) {
    const Chapter* c = find(story, span.chapter_id);
    if (c == nullptr) throw std::runtime_error("没有这一章：" + span.chapter_id);

    std::string out;
    out += prompt::story_revise::kHead;
    out += style_line == StyleLine::ANIME ? prompt::story_revise::kHintAnime
                                          : prompt::story_revise::kHintRealistic;
    // **没圈字就是"写"，不是"改"。** 空区间 [k, k)：章还是空的（[0, 0)）
    // 从头写，不然就在那个位置插一段。规矩换一套——「只改选中的那一段」
    // 对着一段空的没意义，而「篇幅和原来差不多」会让它写零个字。
    const bool insert = span.from_char >= span.to_char;
    out += insert ? prompt::story_revise::kRulesInsert : prompt::story_revise::kRules;

    // ---- 压缩的全局记忆。只要名字和身份 ----
    //
    // 改一段话用不着整份人物表，但**名字必须带**：不带的话模型会把"他"
    // 改成一个自己顺手起的名字，而那个名字在全片其它地方一次都没出现过。
    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    if (!story.characters.empty()) {
        out += "【人物】";
        for (std::size_t i = 0; i < story.characters.size(); ++i) {
            if (i > 0) out += "；";
            out += story.characters[i].name;
            if (!story.characters[i].identity.empty()) {
                out += "（" + story.characters[i].identity + "）";
            }
        }
        out += "\n";
    }

    // ---- 上下文 ----
    const std::string before =
        slice_chars(c->text, span.from_char - kReviseContextChars,
                    span.from_char);
    const std::string after =
        slice_chars(c->text, span.to_char, span.to_char + kReviseContextChars);
    out += "\n【这一章】" + c->title + "\n";
    if (insert && c->text_len() == 0) {
        out += "\n【这一章还是空的】从头写。\n";
    } else if (insert) {
        if (!before.empty()) out += "\n【光标前面】\n……" + before + "\n";
        out += "\n【光标在这儿：新写的那段插在这里】\n";
        if (!after.empty()) out += "\n【光标后面】\n" + after + "……\n";
    } else {
        if (!before.empty()) out += "\n【选中那段前面】\n……" + before + "\n";
        out += "\n【选中要改的那一段】\n" + span_text(story, span) + "\n";
        if (!after.empty()) out += "\n【选中那段后面】\n" + after + "……\n";
    }

    // ---- 来回 ----
    //
    // 用户说"再短一点"的时候，"一点"是相对上一版说的。丢了这段历史，
    // 模型只能从原文重新出发，于是改了三轮还在原地。
    if (!history.empty()) {
        out += prompt::story_revise::kHistoryHead;
        for (const auto& t : history) {
            const std::string who = t.role == "assistant" ? "你" : "作者";
            out += who + "：" + text::collapse_ws(t.text) + "\n";
        }
    }

    out += prompt::story_revise::kTaskHead;
    out += text::strip_ws(instruction);
    out += plain ? prompt::story_revise::kPlainTail : prompt::story_revise::kTail;
    return out;
}

Revision parse_revision(const std::string& raw, int span_chars,
                        int whole_chars) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text::strip_ws(raw));
    } catch (const std::exception&) {
        throw std::runtime_error("模型没回 JSON，改稿这一步走不下去");
    }
    if (!j.is_object() || !j.contains("text") || !j.at("text").is_string()) {
        throw std::runtime_error("模型回的东西里没有 text 那一段");
    }

    Revision r;
    r.text = text::strip_ws(j.at("text").get<std::string>());
    if (j.contains("note") && j.at("note").is_string()) {
        r.note = text::clean_field(j.at("note").get<std::string>());
    }
    if (r.text.empty()) throw std::runtime_error("改完是空的，没法替换");

    // **拦住"改着改着把整章吐回来"。** 那样落盘之后整章内容会翻倍，而界面
    // 上只显示"改好了"——多出来的那一份要等写剧本时才发现，那时候已经隔了
    // 好几步。变短是合法的（"把这段压缩成一句"就该变短），所以只拦上限。
    guard_length(r.text, span_chars, whole_chars);
    return r;
}


Revision parse_plain_revision(const std::string& raw, int span_chars,
                              int whole_chars) {
    Revision r;
    r.text = unwrap(raw);
    if (r.text.empty()) throw std::runtime_error("改完是空的，没法替换");
    guard_length(r.text, span_chars, whole_chars);
    return r;
}

Story apply_revision(const Story& story, const Span& span,
                     const std::string& text_in) {
    Story next = story;
    Chapter* c = next.chapter_by_id(span.chapter_id);
    if (c == nullptr) throw std::runtime_error("没有这一章：" + span.chapter_id);

    const int len = c->text_len();
    const int a = std::clamp(span.from_char, 0, len);
    const int b = std::clamp(span.to_char, a, len);

    const int delta = static_cast<int>(text::utf8_len(text_in)) - (b - a);
    c->text = slice_chars(c->text, 0, a) + text_in +
              slice_chars(c->text, b, len);

    // **候选位置必须跟着挪。** 它们是字符偏移，改一段字之后后面每一个都
    // 错位了——不管的话「这儿悬着什么」就挂到了另一句话上，而这件事不报
    // 任何错：下次读一遍正文（story_analyze）按 at_char 对位时照着错的位置
    // 对，章尾那条说法也跟着指歪。
    //
    // **有说法的那些要保住。** 它们是读懂剧情标出来的真钩子，一章停在哪
    // 全靠它们（实跑里有名钩子把"停在真悬念上"的比例从 25% 抬到 56%）。
    // 图省事整章重算 paragraph_hooks 的话，这些会被一批无名的段落边界
    // 悄悄顶掉——每改一段就掉一批，而界面上一点反应都没有。
    std::vector<Hook> kept;
    for (const Hook& h : c->hooks) {
        if (h.text.empty()) continue;  // 无名的下面按新正文重算就是了
        if (h.at_char <= a) {
            // 这一条落在改动之前：它说的是"这儿之前那段字悬着什么"，
            // 而那段字一个都没动。照留。
            kept.push_back(h);
        } else if (h.at_char > b) {
            Hook moved = h;
            moved.at_char = h.at_char + delta;
            kept.push_back(moved);
        }
        // (a, b] 这一段里的丢掉。**右端是闭的**：正好落在 b 上的那一条，
        // 说的是"刚被换掉的那段字悬着什么"——字换了，那句说明就成了假的，
        // 而假的比没有更糟：写剧本那一步会照着它把这一章停在一个已经不
        // 存在的悬念上。
    }

    // **场的边界也要跟着挪**，道理和钩子一样：它们是字符偏移，写剧本的
    // 场次头（episode_scenes）是照着它们取的。不管的话改完一段字，这一章
    // 的场就全错位了——不报任何错，只在剧本里表现成场次头对不上正文。
    //
    // 边界落在被改掉那一段里的，一律收到这次改动的末尾：场与场之间不能
    // 留缝，也不能重叠。
    const int new_end = b + delta;
    int prev_end = 0;
    std::vector<Scene> scenes;
    for (Scene s : c->scenes) {
        if (s.to_char > b) {
            s.to_char += delta;
        } else if (s.to_char > a) {
            s.to_char = new_end;
        }
        s.from_char = prev_end;
        if (s.to_char <= s.from_char) continue;  // 被改动整个吃掉的场不留
        prev_end = s.to_char;
        scenes.push_back(std::move(s));
    }
    // 末场一律顶到章尾：改完之后正文长度变了，最后一场不该停在正文中间。
    if (!scenes.empty()) scenes.back().to_char = c->text_len();
    c->scenes = std::move(scenes);

    std::set<int> taken;
    for (const Hook& h : kept) taken.insert(h.at_char);
    for (const Hook& h : paragraph_hooks(c->text)) {
        if (taken.count(h.at_char) == 0) kept.push_back(h);
    }
    std::sort(kept.begin(), kept.end(),
              [](const Hook& x, const Hook& y) { return x.at_char < y.at_char; });
    c->hooks = std::move(kept);
    return next;
}

}  // namespace changji::stages
