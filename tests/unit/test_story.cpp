// 故事层与章节计划。
//
// 这里钉住的是「一章一条」这条规矩的具体行为：章节计划里一章一条、
// 条目的 id 从章号推（ch07 → ep07）、每条覆盖的是整章，一条值多长
// 由这一章的正文字数算。
//
// 正文一律用汉字构造（zh(n) 给 n 个字），不用 ASCII：切分全程按 UTF-8
// **字符**算，用 ASCII 的话字符数和字节数相等，这个测试就测不到那个区别了，
// 而那正是最容易写错、错了又最难查的地方（切线落在汉字中间，截出来的是
// 非法 UTF-8，最后表现成离得很远的故障）。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/story_plan.hpp"
#include "util/paths.hpp"

using namespace changji::models;
using changji::stages::plan_episodes;
using changji::stages::prose_budget_chars;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// n 个汉字。每个 1 字符 3 字节。
std::string zh(int n) {
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i) s += "字";
    return s;
}

Chapter mk(const std::string& id, const std::string& title, int len) {
    Chapter c;
    c.chapter_id = id;
    c.title = title;
    c.text = zh(len);
    return c;
}

Chapter outline_only(const std::string& id, const std::string& title) {
    Chapter c;
    c.chapter_id = id;
    c.title = title;
    c.summary = "大纲阶段只有梗概，还没展开正文";
    return c;
}

Hook hook(int at, const std::string& text) {
    Hook h;
    h.at_char = at;
    h.text = text;
    return h;
}

}  // namespace

TEST_CASE("空故事是空的，老项目靠它走老路径") {
    Story s;
    CHECK(s.empty());
    CHECK(s.written_chapters() == 0);

    s.chapters.push_back(outline_only("ch01", "雨夜重逢"));
    CHECK_FALSE(s.empty());
    // 只有大纲不算写过正文
    CHECK(s.written_chapters() == 0);

    s.chapters[0].text = zh(100);
    CHECK(s.written_chapters() == 1);
}

TEST_CASE("Story 往返序列化，中文原样进出") {
    Story s;
    s.premise = "深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。";
    s.scale = StoryScale::LONG;
    s.source = StorySource::PASTED;
    s.logline = "一把伞牵出五年前的事";
    s.genre = "都市情感";
    s.tone = "克制";
    s.episode_duration_s = 90.0;

    StoryCharacter c;
    c.name = "林晚";
    c.identity = "便利店夜班店员";
    c.want = "把那把伞还回去，然后彻底了断";
    c.arc = "从躲着走到敢直视";
    s.characters.push_back(c);

    Relation r;
    r.a = "林晚";
    r.b = "林晚";
    r.kind = "自我";
    r.tension = "不肯承认还在意";
    s.relations.push_back(r);

    StoryLocation loc;
    loc.name = "便利店";
    loc.what = "临街的 24 小时便利店";
    loc.when = "深夜，冷白顶光";
    s.locations.push_back(loc);

    Chapter ch = mk("ch01", "雨夜重逢", 12);
    ch.summary = "他推门进来";
    ch.hooks.push_back(hook(6, "她认出那把伞"));
    Scene sc;
    sc.from_char = 0;
    sc.to_char = 12;
    sc.where = "深夜，便利店，冷白顶光";
    sc.pov = "林晚";
    sc.goal = "把伞要回来";
    sc.obstacle = "他不认这把伞";
    sc.worse = "她发现伞不是他带来的，是别人放在门口的";
    sc.turn = "伞柄上刻着别人的名字";
    ch.scenes.push_back(sc);
    ch.characters.push_back("林晚");
    ch.locations.push_back("便利店");
    s.chapters.push_back(ch);

    const Story back = json(s).get<Story>();

    CHECK(back.premise == s.premise);
    CHECK(back.scale == StoryScale::LONG);
    CHECK(back.source == StorySource::PASTED);
    CHECK(back.logline == s.logline);
    CHECK(back.episode_duration_s == doctest::Approx(90.0));
    REQUIRE(back.characters.size() == 1);
    CHECK(back.characters[0].name == "林晚");
    CHECK(back.characters[0].want == c.want);
    REQUIRE(back.relations.size() == 1);
    CHECK(back.relations[0].tension == r.tension);
    REQUIRE(back.locations.size() == 1);
    CHECK(back.locations[0].when == loc.when);
    REQUIRE(back.chapters.size() == 1);
    CHECK(back.chapters[0].title == "雨夜重逢");
    CHECK(back.chapters[0].text_len() == 12);
    REQUIRE(back.chapters[0].hooks.size() == 1);
    CHECK(back.chapters[0].hooks[0].at_char == 6);
    CHECK(back.chapters[0].hooks[0].text == "她认出那把伞");
    // 场次也要原样进出：写剧本那一步要拿它的 pov 和 where
    REQUIRE(back.chapters[0].scenes.size() == 1);
    CHECK(back.chapters[0].scenes[0].to_char == 12);
    CHECK(back.chapters[0].scenes[0].pov == "林晚");
    CHECK(back.chapters[0].scenes[0].turn == "伞柄上刻着别人的名字");

    // 老版本写的文件里没有新字段，读的时候要能退到默认值而不是抛。
    json partial = json{{"premise", "只有梗概"}};
    const Story old = partial.get<Story>();
    CHECK(old.premise == "只有梗概");
    CHECK(old.scale == StoryScale::MEDIUM);
    CHECK(old.chapters.empty());
}

TEST_CASE("validate 拦住会让切分出事的那几种坏数据") {
    SUBCASE("章节 id 形状不对") {
        Story s;
        s.chapters.push_back(mk("第一章", "雨夜重逢", 10));
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("ch 加数字") != std::string::npos);
    }

    SUBCASE("章节 id 重复") {
        Story s;
        s.chapters.push_back(mk("ch01", "甲", 10));
        s.chapters.push_back(mk("ch01", "乙", 10));
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("重复") != std::string::npos);
    }

    SUBCASE("钩子越界——正文里根本没有那个位置") {
        Story s;
        Chapter c = mk("ch01", "雨夜重逢", 10);
        c.hooks.push_back(hook(50, "越界了"));
        s.chapters.push_back(c);
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("超出正文范围") != std::string::npos);
    }

    SUBCASE("关系指向不存在的人物") {
        Story s;
        StoryCharacter c;
        c.name = "林晚";
        s.characters.push_back(c);
        Relation r;
        r.a = "林晚";
        r.b = "陈默";
        s.relations.push_back(r);
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("陈默") != std::string::npos);
    }

    SUBCASE("章节计划指向不存在的章") {
        Story s;
        s.chapters.push_back(mk("ch01", "甲", 10));
        EpisodePlan p;
        p.episode_id = "ep01";
        p.from_chapter = "ch01";
        p.to_chapter = "ch09";
        s.plan.push_back(p);
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("ch09") != std::string::npos);
    }

    SUBCASE("好数据不报错") {
        Story s;
        s.premise = "深夜便利店";
        Chapter c = mk("ch01", "雨夜重逢", 10);
        c.hooks.push_back(hook(10, "章尾也是合法位置"));
        s.chapters.push_back(c);
        CHECK(s.validate().empty());
    }
}

TEST_CASE("一章的正文容量按时长算") {
    CHECK(prose_budget_chars(60.0) == 900);
    CHECK(prose_budget_chars(30.0) == 450);
    CHECK(prose_budget_chars(0.0) == 0);
    CHECK(prose_budget_chars(-1.0) == 0);
}

TEST_CASE("没展开正文时，章节计划一章一条") {
    Story s;
    s.chapters.push_back(outline_only("ch01", "雨夜重逢"));
    s.chapters.push_back(outline_only("ch02", "五年前那把伞"));
    s.chapters.push_back(outline_only("ch03", "便利店打烊"));

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].episode_id == "ep01");
    CHECK(plan[0].title == "雨夜重逢");
    CHECK(plan[0].from_chapter == "ch01");
    CHECK(plan[2].episode_id == "ep03");
    CHECK(plan[2].to_chapter == "ch03");
    // 一章一条，不加上/下后缀
    CHECK(plan[1].title == "五年前那把伞");
}

TEST_CASE("一章一条：正文再长、钩子再多也不切") {
    // 用户 2026-09-16 定的：一章就是一条，多长由这一章自己的内容定。
    // 这儿原来按容量（60 秒 = 900 字）找钩子把一章切成上/中/下，
    // 章节表那头当天就改成一章一条了，章节计划这头没跟上——同一个 ep01，
    // 章节表说整章、章节计划说前三分之一，剧本页的「原文」读的是章节计划，
    // 一章 1800 字只显示前一半（2026-09-18 用户撞到）。
    Story s;
    Chapter c = mk("ch01", "雨夜重逢", 3000);
    for (int at = 300; at < 3000; at += 300) {
        c.hooks.push_back(hook(at, "钩子" + std::to_string(at)));
    }
    s.chapters.push_back(c);
    s.chapters.push_back(mk("ch02", "五年前那把伞", 300));

    for (double per : {30.0, 60.0, 120.0}) {
        CAPTURE(per);
        const auto plan = plan_episodes(s, per);
        REQUIRE(plan.size() == 2);
        CHECK(plan[0].episode_id == "ep01");
        CHECK(plan[0].from_chapter == "ch01");
        CHECK(plan[0].to_chapter == "ch01");
        CHECK(plan[0].from_char == 0);
        CHECK(plan[0].to_char == 3000);
        CHECK(plan[0].title == "雨夜重逢");   // 不带上/中/下
        CHECK(plan[0].hook == "钩子2700");     // 最后一条有说法的钩子
        CHECK(plan[1].episode_id == "ep02");
        CHECK(plan[1].from_chapter == "ch02");
        CHECK(plan[1].from_char == 0);
        CHECK(plan[1].to_char == 300);
    }
}

TEST_CASE("章节计划的 id 和章节表同一条规矩：ch07 → ep07") {
    // 以前章节计划的 id 是按切片顺序发的（ep01、ep02……），章节表按章号推
    // （ch07 → ep07），两套编号一错位，按 episode_id 查到的是隔壁章的半截。
    Story s;
    s.chapters.push_back(mk("ch01", "甲", 100));
    s.chapters.push_back(mk("ch07", "乙", 100));
    s.chapters.push_back(mk("intro", "丙", 100));   // 认不出编号：按位置，第 3 章

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].episode_id == "ep01");
    CHECK(plan[1].episode_id == "ep07");
    CHECK(plan[2].episode_id == "ep03");
    CHECK(changji::stages::episode_id_for_chapter("ch7", 0) == "ep07");
    CHECK(changji::stages::episode_id_for_chapter("ch12", 0) == "ep12");
}

TEST_CASE("有正文和没正文的章交替时顺序不乱") {
    Story s;
    s.chapters.push_back(outline_only("ch01", "甲"));
    s.chapters.push_back(mk("ch02", "乙", 1000));
    s.chapters.push_back(outline_only("ch03", "丙"));

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].from_chapter == "ch01");
    CHECK(plan[1].from_chapter == "ch02");
    CHECK(plan[2].from_chapter == "ch03");
    CHECK(plan[0].episode_id == "ep01");
    CHECK(plan[2].episode_id == "ep03");
}

TEST_CASE("一章值多长：有正文按字数估，没正文退回故事上存的那个数，再没有退回 kDefaultChapterS") {
    Story s;
    s.episode_duration_s = 30.0;
    s.chapters.push_back(mk("ch01", "雨夜重逢", 1500));
    s.chapters.push_back(outline_only("ch02", "五年前那把伞"));

    const auto plan = plan_episodes(s, 0.0);
    REQUIRE(plan.size() == 2);
    // 1500 字 ÷ 每秒 15 字 = 100 秒——和 sync_episodes_to_chapters 给章节
    // 记的是同一个数，剧本页和章节表才不会各说各的
    CHECK(plan[0].target_duration_s ==
          doctest::Approx(1500.0 / changji::stages::kProseCharsPerSecond));
    // 入参给 0：退到故事自己存的那个数
    CHECK(plan[1].target_duration_s == doctest::Approx(30.0));

    SUBCASE("两个来源都没有：退到 kDefaultChapterS") {
        // 2026-09-18 之前这最后一级回落是 `[assembly].episode_s`（一个用户
        // 配置项）。那一项随成片切段一起拔掉，回落收成了代码里的常量——
        // **收完没有任何一条用例钉它**，这条补上。
        s.episode_duration_s = 0.0;
        const auto fallen = plan_episodes(s, 0.0);
        REQUIRE(fallen.size() == 2);
        // 有正文的那一章轮不到回落，还是按字数估
        CHECK(fallen[0].target_duration_s ==
              doctest::Approx(1500.0 / changji::stages::kProseCharsPerSecond));
        CHECK(fallen[1].target_duration_s ==
              doctest::Approx(changji::stages::kDefaultChapterS));
    }
}

TEST_CASE("算出来的章节计划自己能过校验") {
    Story s;
    Chapter c = mk("ch01", "雨夜重逢", 3000);
    c.hooks.push_back(hook(900, "钩子一"));
    c.hooks.push_back(hook(1800, "钩子二"));
    s.chapters.push_back(c);
    s.plan = plan_episodes(s, 60.0);
    CHECK(s.validate().empty());
}

TEST_CASE("story.json 读写：不存在时是空的，存完读回来一样") {
    const fs::path root =
        fs::temp_directory_path() / changji::paths::from_utf8("changji_故事_读写");
    std::error_code ec;
    fs::remove_all(root, ec);

    ProjectStore store = ProjectStore::create(root, "gushi", "故事测试项目");

    // 老项目没有这个文件，要读出一个空的而不是抛
    CHECK(store.load_story().empty());

    Story s;
    s.premise = "深夜便利店，前任推门进来。";
    s.scale = StoryScale::SHORT;
    Chapter c = mk("ch01", "雨夜重逢", 20);
    c.hooks.push_back(hook(10, "她认出那把伞"));
    s.chapters.push_back(c);
    s.plan = plan_episodes(s, 60.0);
    store.save_story(s);

    const Story back = store.load_story();
    CHECK_FALSE(back.empty());
    CHECK(back.premise == s.premise);
    CHECK(back.scale == StoryScale::SHORT);
    REQUIRE(back.chapters.size() == 1);
    CHECK(back.chapters[0].hooks[0].text == "她认出那把伞");
    CHECK(back.plan.size() == s.plan.size());

    // 故事单独一个文件，不跟 project.json 搅在一起——正文十几万字，
    // 而 project.json 每改一次分镜状态就要原子重写一遍。
    CHECK(fs::is_regular_file(store.paths().story_file()));
    const Project p = store.load_project();
    CHECK(p.episodes.empty());

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// 删一章
// ---------------------------------------------------------------------------

namespace {

EpisodePlan span(const std::string& id, const std::string& from, int from_char,
                 const std::string& to, int to_char) {
    EpisodePlan e;
    e.episode_id = id;
    e.from_chapter = from;
    e.from_char = from_char;
    e.to_chapter = to;
    e.to_char = to_char;
    return e;
}

}  // namespace

TEST_CASE("删中间一章：跨着它的计划条目收缩，只在它里面的条目丢掉，id 不重排") {
    Story s;
    s.chapters = {mk("ch01", "一", 100), mk("ch02", "二", 200), mk("ch03", "三", 300)};
    s.plan = {
        span("ep01", "ch01", 0, "ch02", 50),    // 终点在 ch02 → 终点挪到 ch01 末尾
        span("ep02", "ch02", 50, "ch02", 200),  // 整条在 ch02 → 丢
        span("ep03", "ch02", 120, "ch03", 300), // 起点在 ch02 → 起点挪到 ch03 开头
        span("ep04", "ch03", 0, "ch03", 100),   // 不沾边 → 原样
    };

    const auto r = s.remove_chapter("ch02");
    CHECK(r.removed);
    CHECK(r.plan_dropped == 1);
    CHECK(r.plan_moved == 2);

    REQUIRE(s.chapters.size() == 2);
    CHECK(s.chapters[0].chapter_id == "ch01");
    CHECK(s.chapters[1].chapter_id == "ch03"); // 不重排成 ch02

    REQUIRE(s.plan.size() == 3);
    CHECK(s.plan[0].to_chapter == "ch01");
    CHECK(s.plan[0].to_char == 100); // ch01 是 100 个字，按码点数
    CHECK(s.plan[1].episode_id == "ep03");
    CHECK(s.plan[1].from_chapter == "ch03");
    CHECK(s.plan[1].from_char == 0);
    CHECK(s.plan[2].episode_id == "ep04");
    CHECK(s.plan[2].from_chapter == "ch03");
}

TEST_CASE("删头一章、末一章：没有可收缩的邻居时那条丢掉") {
    Story s;
    s.chapters = {mk("ch01", "一", 10), mk("ch02", "二", 20)};
    s.plan = {span("ep01", "ch01", 0, "ch02", 5)};
    const auto r = s.remove_chapter("ch01");
    CHECK(r.removed);
    CHECK(r.plan_moved == 1);
    REQUIRE(s.plan.size() == 1);
    CHECK(s.plan[0].from_chapter == "ch02");

    Story t;
    t.chapters = {mk("ch01", "一", 10)};
    t.plan = {span("ep01", "ch01", 0, "ch01", 10)};
    const auto q = t.remove_chapter("ch01");
    CHECK(q.removed);
    CHECK(q.plan_dropped == 1);
    CHECK(t.chapters.empty());
    CHECK(t.plan.empty());
}

TEST_CASE("没有这个 id 一个字不动") {
    Story s;
    s.chapters = {mk("ch01", "一", 10)};
    s.plan = {span("ep01", "ch01", 0, "ch01", 10)};
    const auto r = s.remove_chapter("ch99");
    CHECK_FALSE(r.removed);
    CHECK(s.chapters.size() == 1);
    CHECK(s.plan.size() == 1);
}
