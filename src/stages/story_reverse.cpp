#include "stages/story_reverse.hpp"

#include "stages/script.hpp"

#include <cstdio>

#include "stages/story_import.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

std::string chapter_id(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ch%02zu", i + 1);
    return buf;
}

/// 几章算什么体量。
///
/// 只影响以后让模型续写时提示词里那个"大概写几章"的量级，所以按
/// suggested_chapters 那三个数（4 / 8 / 16）的中点分。反推出来的故事本身
/// 已经有多少章是多少章，这个字段不会去动它。
StoryScale scale_for(std::size_t chapters) {
    if (chapters <= 6) return StoryScale::SHORT;
    if (chapters <= 12) return StoryScale::MEDIUM;
    return StoryScale::LONG;
}

}  // namespace

Story story_from_episodes(const Project& project) {
    Story story;
    story.premise = project.premise;
    // **不是 AI 写的，所以不能标 AI。** 这个字段决定界面上那句"这个故事
    // 哪来的"，标错了用户会以为大纲是模型编的，而它其实是他自己那几章。
    story.source = StorySource::PASTED;

    double duration_sum = 0.0;
    int duration_count = 0;

    for (const Episode& ep : project.episodes) {
        const std::string script = text::strip_ws(ep.script);
        // 没剧本的跳过：反推出来是一章空正文，既提不出结构，章节计划那
        // 一条也是个空区间，只会在章节列表里占一行说不清的东西。
        if (script.empty()) continue;

        Chapter ch;
        ch.chapter_id = chapter_id(story.chapters.size());
        ch.title = ep.title.empty() ? ep.episode_id : ep.title;
        ch.summary = ep.synopsis;
        // 段头（「【开场钩子 0–5 秒】」）是剧本的东西，进了"小说正文"是噪音
        ch.text = stages::strip_act_headers(script);
        // 段落边界登记成候选位置，说法留空。**反推这一步不碰大模型**，
        // 章里没有场次表，正文里程序认得出来的位置就只有段落边界这一份。
        // 读一遍正文（story_analyze）把「这儿悬着什么」挂上来时，位置对
        // 得上就填进已经在这儿的这一条，对不上才新加一条。
        //
        // 2026-09-18 查过一遍谁在读它：按时长切段那条链拔掉之后，没有任
        // 何地方拿它当刀口了，空说法的候选界面上也不显示——今天它就是给
        // story_analyze 对位用的。想删之前先去看 story_analyze.cpp 里那个
        // put，它是按 at_char 对位的。
        ch.hooks = paragraph_hooks(ch.text);

        EpisodePlan p;
        // **指回真正的 episode_id**，不是章 id。老项目里它可能不连续
        // （删过几章），而章 id 是重排过的；两边就靠这一个字段对上。
        p.episode_id = ep.episode_id;
        p.title = ch.title;
        // 钩子留空：这一条的边界就是章界，而章界是这一章本来就有的边界，
        // 不是读懂剧情找出来的悬念。**宁可空着**——填一句假的，界面上会
        // 显示成"这一章停在这儿"，而那句话没有任何依据。
        p.target_duration_s = ep.target_duration_s;
        p.from_chapter = ch.chapter_id;
        p.from_char = 0;
        p.to_chapter = ch.chapter_id;
        p.to_char = ch.text_len();

        if (ep.target_duration_s > 0.0) {
            duration_sum += ep.target_duration_s;
            ++duration_count;
        }

        story.chapters.push_back(std::move(ch));
        story.plan.push_back(std::move(p));
    }

    story.scale = scale_for(story.chapters.size());
    // 回落时长取现有各章的平均。**不用默认的 60 秒**：老项目那几章是按
    // 自己那个时长排的分镜，拿 60 秒写进故事里，下次重算章节计划会得到一
    // 份和现有章节对不上的表，而用户并没有改过时长。
    if (duration_count > 0) {
        story.episode_duration_s = duration_sum / duration_count;
    }
    return story;
}

}  // namespace changji::stages
