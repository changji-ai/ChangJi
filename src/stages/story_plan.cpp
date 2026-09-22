#include "stages/story_plan.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "stages/script_story.hpp"

namespace changji::stages {

using models::Chapter;
using models::EpisodePlan;
using models::Story;

int prose_budget_chars(double duration_s) {
    if (duration_s <= 0.0) return 0;
    const long v = std::lround(duration_s * kProseCharsPerSecond);
    return v < 1 ? 1 : static_cast<int>(v);
}

std::string episode_id_for_chapter(const std::string& chapter_id,
                                   std::size_t index) {
    std::string digits;
    for (const char ch : chapter_id) {
        if (ch >= '0' && ch <= '9') digits += ch;
    }
    if (digits.empty()) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02d", static_cast<int>(index + 1));
        digits = buf;
    }
    // 补到两位：ch7 和 ch07 要落在同一个 ep07 上
    while (digits.size() < 2) digits.insert(digits.begin(), '0');
    return "ep" + digits;
}

std::vector<EpisodePlan> plan_episodes(const Story& story, double fallback_duration_s) {
    double per = fallback_duration_s;
    if (per <= 0.0) per = story.episode_duration_s;
    if (per <= 0.0) per = kDefaultChapterS;

    std::vector<EpisodePlan> out;
    out.reserve(story.chapters.size());
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        const Chapter& ch = story.chapters[i];
        // 这一章值多长：有正文按字数估，没正文按回落值。**和
        // sync_episodes_to_chapters 给章节表记的是同一个数**——两边各算各的，
        // 剧本页按章节计划排、章节表按自己的数拆镜头，迟早对不上。
        const int len = ch.text_len();
        const double dur = len > 0 ? static_cast<double>(len) / kProseCharsPerSecond
                                   : per;
        // 整章、章名、钩子取最后一场的 turn：和写剧本那头（scripting.cpp）
        // 用的是同一个 chapter_plan，剧本页和写剧本看到的是同一份计划。
        EpisodePlan p = chapter_plan(story, ch.chapter_id, dur);
        p.episode_id = episode_id_for_chapter(ch.chapter_id, i);
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace changji::stages
