#include "models/story.hpp"

#include <algorithm>
#include <set>

#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::models {

namespace {

/// 章节 id 的形状：ch 开头，后面全是数字。
///
/// 比 episode_id 严一档（那边允许任意小写下划线串）。章节 id 是程序生成的，
/// 不经用户手，所以可以把形状钉死；钉死之后按 id 排序就等于按顺序排序。
bool is_chapter_id(const std::string& s) {
    if (s.size() < 3) return false;
    if (s.compare(0, 2, "ch") != 0) return false;
    return std::all_of(s.begin() + 2, s.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

const char* to_string(StoryScale v) {
    switch (v) {
        case StoryScale::SHORT: return "short";
        case StoryScale::MEDIUM: return "medium";
        case StoryScale::LONG: return "long";
    }
    return "medium";
}

int suggested_chapters(StoryScale scale) {
    // 只是给大纲提示词的一个量级，不是硬约束——模型按故事本身的需要多写少写
    // 一两章是对的。写死成「必须 N 章」会让它为了凑数注水或者硬收。
    switch (scale) {
        case StoryScale::SHORT: return 4;
        case StoryScale::MEDIUM: return 8;
        case StoryScale::LONG: return 16;
    }
    return 8;
}

const char* to_string(StorySource v) {
    switch (v) {
        case StorySource::AI: return "ai";
        case StorySource::PASTED: return "pasted";
        case StorySource::KEYWORDS: return "keywords";
    }
    return "ai";
}

int Chapter::text_len() const {
    return static_cast<int>(text::utf8_len(text));
}

bool Story::empty() const {
    return text::strip_ws(premise).empty() && chapters.empty();
}

const Chapter* Story::chapter_by_id(const std::string& chapter_id) const {
    for (const auto& c : chapters) {
        if (c.chapter_id == chapter_id) return &c;
    }
    return nullptr;
}

Chapter* Story::chapter_by_id(const std::string& chapter_id) {
    for (auto& c : chapters) {
        if (c.chapter_id == chapter_id) return &c;
    }
    return nullptr;
}

std::string chapter_text_fingerprint(const std::string& text) {
    const std::string body = text::strip_ws(text);
    if (body.empty()) return "";
    return text::sha1_hex(body);
}

std::string story_text_fingerprint(const Story& story) {
    // 章 id 也进去：两章的正文对调过，逐章的指纹一个不变，而那确实是改过。
    std::string all;
    for (const Chapter& c : story.chapters) {
        const std::string fp = chapter_text_fingerprint(c.text);
        if (fp.empty()) continue;
        all += c.chapter_id;
        all += ':';
        all += fp;
        all += '\n';
    }
    if (all.empty()) return "";
    return text::sha1_hex(all);
}

int Story::written_chapters() const {
    int n = 0;
    for (const auto& c : chapters) {
        if (!text::strip_ws(c.text).empty()) ++n;
    }
    return n;
}

std::vector<std::string> Story::validate() const {
    std::vector<std::string> errs;

    if (text::utf8_len(premise) > 2000) {
        errs.push_back(SAY("premise 最多 2000 字"));
    }
    if (episode_duration_s <= 0.0) {
        errs.push_back(SAY("episode_duration_s 必须大于 0"));
    }

    std::set<std::string> chapter_ids;
    for (const auto& c : chapters) {
        if (!is_chapter_id(c.chapter_id)) {
            errs.push_back(
                SAYF("章节 id 必须是 ch 加数字，当前是 %1", c.chapter_id));
            continue;
        }
        if (!chapter_ids.insert(c.chapter_id).second) {
            errs.push_back(SAYF("章节 id 重复：%1", c.chapter_id));
        }
        // 钩子越界是会真出事的一类错：位置是拿来在正文里对位的
        //（story_analyze 按它找回同一条钩子），指到正文外面去之后就再也
        // 对不上任何地方，而且一声不响。
        const int len = c.text_len();
        for (const auto& h : c.hooks) {
            if (h.at_char < 0 || h.at_char > len) {
                errs.push_back(SAYF("%1：钩子位置 %2 超出正文范围 0..%3",
                                    c.chapter_id, std::to_string(h.at_char),
                                    std::to_string(len)));
            }
        }
    }

    std::set<std::string> names;
    for (const auto& ch : characters) {
        if (text::strip_ws(ch.name).empty()) {
            errs.push_back(SAY("人物没有名字"));
        } else if (!names.insert(ch.name).second) {
            errs.push_back(SAYF("人物重名：%1", ch.name));
        }
    }
    // 关系的两端必须是登记过的人物。指向不存在的人时，界面上那条关系画不出来，
    // 而提示词里会凭空多出一个角色——这正是要防的漂移。
    for (const auto& r : relations) {
        if (!r.a.empty() && names.count(r.a) == 0) {
            errs.push_back(SAYF("关系指向了不存在的人物：%1", r.a));
        }
        if (!r.b.empty() && names.count(r.b) == 0) {
            errs.push_back(SAYF("关系指向了不存在的人物：%1", r.b));
        }
    }

    std::set<std::string> ep_ids;
    for (const auto& p : plan) {
        if (p.episode_id.empty()) {
            errs.push_back(SAY("章节计划里有一条没有 id"));
        } else if (!ep_ids.insert(p.episode_id).second) {
            errs.push_back(SAYF("章节计划里 id 重复：%1", p.episode_id));
        }
        if (p.target_duration_s <= 0.0) {
            errs.push_back(
                SAYF("%1：target_duration_s 必须大于 0", p.episode_id));
        }
        if (!p.from_chapter.empty() && chapter_ids.count(p.from_chapter) == 0) {
            errs.push_back(SAYF("%1：起始章 %2 不存在", p.episode_id,
                                p.from_chapter));
        }
        if (!p.to_chapter.empty() && chapter_ids.count(p.to_chapter) == 0) {
            errs.push_back(
                SAYF("%1：末章 %2 不存在", p.episode_id, p.to_chapter));
        }
    }

    return errs;
}

Story::ChapterRemoval Story::remove_chapter(const std::string& chapter_id) {
    ChapterRemoval out;
    const auto it = std::find_if(chapters.begin(), chapters.end(),
                                 [&](const Chapter& c) { return c.chapter_id == chapter_id; });
    if (it == chapters.end()) return out;
    const std::size_t at = static_cast<std::size_t>(it - chapters.begin());

    // 上一章 / 下一章：跨着这一章的计划条目要收缩到它们身上
    const Chapter* prev = at > 0 ? &chapters[at - 1] : nullptr;
    const Chapter* next = at + 1 < chapters.size() ? &chapters[at + 1] : nullptr;
    // 末尾字符按码点数——计划里的 to_char 就是码点偏移（见 EpisodePlan）
    const auto cp_len = [](const std::string& t) {
        int n = 0;
        for (unsigned char ch : t) if ((ch & 0xC0) != 0x80) ++n;
        return n;
    };

    std::vector<EpisodePlan> kept;
    kept.reserve(plan.size());
    for (EpisodePlan e : plan) {
        const bool from_here = e.from_chapter == chapter_id;
        const bool to_here = e.to_chapter == chapter_id;
        if (from_here && to_here) {
            ++out.plan_dropped;
            continue;
        }
        if (from_here) {
            // 起点挪到下一章开头。没有下一章说明它是最后一章，而 to 在更后面
            // 是不可能的——那就整条都不成立，丢掉。
            if (next == nullptr) { ++out.plan_dropped; continue; }
            e.from_chapter = next->chapter_id;
            e.from_char = 0;
            ++out.plan_moved;
        } else if (to_here) {
            if (prev == nullptr) { ++out.plan_dropped; continue; }
            e.to_chapter = prev->chapter_id;
            e.to_char = cp_len(prev->text);
            ++out.plan_moved;
        }
        kept.push_back(std::move(e));
    }
    plan = std::move(kept);
    chapters.erase(it);
    out.removed = true;
    return out;
}

}  // namespace changji::models
