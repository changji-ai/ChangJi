// 从老项目已有的剧本反推故事骨架。
//
// 钉的都是"错了不报错"那一类：章 id 必须重排（老项目的 episode_id 可能
// 不连续）、章节计划必须指回真正的 episode_id、边界必须一个字都不挪、
// 钩子宁可空着。

#include <doctest/doctest.h>

#include <string>

#include "models/project.hpp"
#include "stages/story_reverse.hpp"

using namespace changji;
using namespace changji::models;
using changji::stages::story_from_episodes;

namespace {

/// 一段像剧本的正文。段落边界要有几个，否则候选位置只剩章界。
std::string a_script(const std::string& mark) {
    std::string out;
    for (int i = 0; i < 6; ++i) {
        out += mark + "，第 " + std::to_string(i) + " 段。";
        out += "这里再写几句把这一段撑开，让它看起来像一段真的正文而不是一行字。\n\n";
    }
    return out;
}

Episode an_episode(const std::string& id, const std::string& title,
                   const std::string& script, double dur = 60.0) {
    Episode ep;
    ep.episode_id = id;
    ep.title = title;
    ep.script = script;
    ep.target_duration_s = dur;
    return ep;
}

}  // namespace

TEST_CASE("章 id 重排，章节计划指回真正的 episode_id") {
    Project p;
    p.project_id = "old";
    p.premise = "老项目的那句梗";
    // 删过几章，episode_id 是断的。**这正是不能照搬 episode_id 当章 id
    // 的原因**：章 id 要求 ^ch[0-9]+$ 且在故事里连续。
    p.episodes.push_back(an_episode("ep01", "第一集", a_script("一")));
    p.episodes.push_back(an_episode("ep03", "第三集", a_script("三")));
    p.episodes.push_back(an_episode("ep07", "第七集", a_script("七")));

    const Story s = story_from_episodes(p);

    REQUIRE(s.chapters.size() == 3);
    CHECK(s.chapters[0].chapter_id == "ch01");
    CHECK(s.chapters[1].chapter_id == "ch02");
    CHECK(s.chapters[2].chapter_id == "ch03");

    REQUIRE(s.plan.size() == 3);
    // 章 id 重排了，但章节计划这一条必须还认得老的 episode_id——两边就靠它对上。
    CHECK(s.plan[0].episode_id == "ep01");
    CHECK(s.plan[1].episode_id == "ep03");
    CHECK(s.plan[2].episode_id == "ep07");
    CHECK(s.plan[1].from_chapter == "ch02");
    CHECK(s.plan[1].to_chapter == "ch02");

    CHECK(s.premise == "老项目的那句梗");
    // 不是模型编的，标 AI 的话界面会说这大纲是 AI 写的
    CHECK(s.source == StorySource::PASTED);
    // 校验得过，否则存不进 story.json
    CHECK(s.validate().empty());
}

TEST_CASE("边界一个字都不挪：一条计划盖住的就是一整章") {
    Project p;
    p.project_id = "old";
    p.episodes.push_back(an_episode("ep01", "一", a_script("一")));

    const Story s = story_from_episodes(p);
    REQUIRE(s.plan.size() == 1);
    // 老项目那几章已经排了分镜、出了片。边界一挪，渲染好的镜头就对不上
    // 它该在的那一段——所以反推出来的每一条都是从头到尾的整章。
    CHECK(s.plan[0].from_char == 0);
    CHECK(s.plan[0].to_char == s.chapters[0].text_len());
}

TEST_CASE("没剧本的章跳过，不留一章空正文") {
    Project p;
    p.project_id = "old";
    p.episodes.push_back(an_episode("ep01", "写了的", a_script("一")));
    p.episodes.push_back(an_episode("ep02", "还没写", ""));
    p.episodes.push_back(an_episode("ep03", "只有空白", "   \n\n  "));
    p.episodes.push_back(an_episode("ep04", "也写了", a_script("四")));

    const Story s = story_from_episodes(p);
    // 空正文那一章既提不出结构也没有正文可写，只会在章节列表里占一行
    REQUIRE(s.chapters.size() == 2);
    CHECK(s.chapters[0].title == "写了的");
    CHECK(s.chapters[1].title == "也写了");
    REQUIRE(s.plan.size() == 2);
    CHECK(s.plan[1].episode_id == "ep04");
}

TEST_CASE("钩子宁可空着，不编一句") {
    Project p;
    p.project_id = "old";
    p.episodes.push_back(an_episode("ep01", "一", a_script("一")));

    const Story s = story_from_episodes(p);
    // 切点是章界，而章界是这一章本来就有的边界，不是读懂剧情找出来的悬念。
    // 填一句的话界面上会显示成"这一章停在这儿"，而那句话没有任何依据。
    CHECK(s.plan[0].hook.empty());
    // 段落边界照样登记成候选位置：读一遍正文（story_analyze）把「这儿悬着
    // 什么」挂上来时，就是按 at_char 往这几条上对位的
    CHECK(s.chapters[0].hooks.size() > 1);
    for (const auto& h : s.chapters[0].hooks) {
        CHECK(h.text.empty());  // 段落边界不是真钩子，不许假装是
    }
}

TEST_CASE("章时长取现有各章的平均，不用默认的 60") {
    Project p;
    p.project_id = "old";
    p.episodes.push_back(an_episode("ep01", "一", a_script("一"), 30.0));
    p.episodes.push_back(an_episode("ep02", "二", a_script("二"), 30.0));
    p.episodes.push_back(an_episode("ep03", "三", a_script("三"), 60.0));

    const Story s = story_from_episodes(p);
    // 老项目那几章是按自己那个时长排的分镜。这个数今天管的是**没正文的章
    // 回落多长、往下写新章时正文写多少字**（stages::prose_budget_chars）。
    // 写 60 秒进去的话，接着写出来的章会比老项目那几章长出一截，而用户
    // 并没有改过时长。
    CHECK(s.episode_duration_s == doctest::Approx(40.0));
}

TEST_CASE("一章都没有就是空故事，让调用方去报 400") {
    Project p;
    p.project_id = "old";
    const Story s = story_from_episodes(p);
    CHECK(s.chapters.empty());
    CHECK(s.plan.empty());
    // Story::empty() 是"这个项目还没走新流程"的判据，反推不出东西时
    // 必须仍然是空的——否则老项目会被当成已经有故事了
    CHECK(s.empty());
}

TEST_CASE("没标题的章拿 episode_id 当章名，不留一个空名字") {
    Project p;
    p.project_id = "old";
    p.episodes.push_back(an_episode("ep01", "", a_script("一")));

    const Story s = story_from_episodes(p);
    // 章节列表里一行空白，用户分不清那是哪一章
    CHECK(s.chapters[0].title == "ep01");
    CHECK(s.plan[0].title == "ep01");
}
