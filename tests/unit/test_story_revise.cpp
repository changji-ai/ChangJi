// 改原稿的某一段。
//
// 这一组用例守的是同一件事：**改动范围可预期**。用户敢把写了一半的稿子
// 交给 AI，全靠"它只动我圈出来的那几行"；一旦它顺手改了别处，哪怕改得更
// 好，这个功能也就没人敢按第二次——因为他没法知道还有哪儿被动过。
//
// 位置一律按 UTF-8 字符算。按字节算的话切线会落在汉字中间，截出来的是
// 非法 UTF-8，一路流到提示词和字幕，最后表现成「整轨字幕不显示」。

#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "models/story.hpp"
#include "stages/story_revise.hpp"
#include "util/text.hpp"

using namespace changji;
using namespace changji::models;
using namespace changji::stages;
using json = nlohmann::json;

namespace {

/// 三段中文，段间空行。字符数是刻意数过的：
/// 「第一段。」4 字 + 「\n\n」2 + 「第二段在中间。」7 + 「\n\n」2 + 「第三段。」4
Story a_story() {
    Story s;
    s.logline = "深夜便利店，前任推门进来";
    s.tone = "克制";
    StoryCharacter c;
    c.name = "林晚";
    c.identity = "便利店夜班";
    s.characters.push_back(c);

    Chapter ch;
    ch.chapter_id = "ch01";
    ch.title = "伞与雨";
    ch.text = "第一段。\n\n第二段在中间。\n\n第三段。";
    // 一个有说法的钩子在第三段之前，一个在章尾
    ch.hooks.push_back({4, ""});           // 无名，第一段后
    ch.hooks.push_back({15, "他推门进来"});  // 有说法，第二段后
    ch.hooks.push_back({19, "伞留在门口"});  // 有说法，章尾
    s.chapters.push_back(ch);
    return s;
}

Span mid_span() {
    // 「第二段在中间。」在 [6, 13)
    return Span{"ch01", 6, 13};
}

}  // namespace

TEST_CASE("取出来的就是选中那一段，按字符不按字节") {
    const Story s = a_story();
    CHECK(span_text(s, mid_span()) == "第二段在中间。");
    // 越界夹住，不抛——这是给提示词拼上下文用的，抛了整条路走不下去
    CHECK(span_text(s, Span{"ch01", -50, 4}) == "第一段。");
    CHECK(span_text(s, Span{"ch01", 0, 9999}) == s.chapters[0].text);
    CHECK(span_text(s, Span{"没这一章", 0, 4}).empty());
}

TEST_CASE("提示词里要有选中那段、前后文、和人物名") {
    const Story s = a_story();
    const std::string p =
        build_revise_prompt(s, mid_span(), "这儿太赶了，铺一下情绪", {},
                            StyleLine::REALISTIC);

    CHECK(p.find("第二段在中间。") != std::string::npos);
    // 前后文：改一段话要知道它前面刚发生了什么、后面马上要发生什么，
    // 否则最常见的毛病是把前面已经交代过的事又交代一遍
    CHECK(p.find("第一段。") != std::string::npos);
    CHECK(p.find("第三段。") != std::string::npos);
    // **人物名必须带。** 不带的话模型会把"他"改成一个自己顺手起的名字，
    // 而那个名字在全片其它地方一次都没出现过
    CHECK(p.find("林晚") != std::string::npos);
    CHECK(p.find("这儿太赶了，铺一下情绪") != std::string::npos);
    // 这一条是整个设计的地基
    CHECK(p.find("只改选中的那一段") != std::string::npos);
}

TEST_CASE("没圈字：走「写」那套规矩，不是「改」") {
    // 空区间 [k, k)：章还是空的（[0, 0)）从头写，不然在那个位置插一段。
    // 「只改选中的那一段」对着一段空的没意义，而「篇幅和原来差不多」会让
    // 它写零个字——所以换一套规矩，上下文照带。
    Story s = a_story();
    const std::string mid = build_revise_prompt(s, Span{"ch01", 6, 6}, "这儿补一段环境",
                                                {}, StyleLine::REALISTIC);
    CHECK(mid.find("他要你在光标那个位置写一段") != std::string::npos);
    CHECK(mid.find("只改选中的那一段") == std::string::npos);
    CHECK(mid.find("第一段。") != std::string::npos);
    CHECK(mid.find("第三段。") != std::string::npos);
    CHECK(mid.find("这儿补一段环境") != std::string::npos);

    s.chapters[0].text.clear();
    s.chapters[0].hooks.clear();
    const std::string blank = build_revise_prompt(s, Span{"ch01", 0, 0}, "写个开头", {},
                                                  StyleLine::REALISTIC);
    CHECK(blank.find("这一章还是空的") != std::string::npos);
    // 人物名照带：不带的话它会自己起名
    CHECK(blank.find("林晚") != std::string::npos);
}

TEST_CASE("长度上限：从头写也有个顶") {
    // 空区间没有"选中的六倍"可算，原来直接放行——模型跑飞了会吐两万字。
    std::string got;
    for (int i = 0; i < 8001; ++i) got += "字";
    CHECK_THROWS(parse_plain_revision(got, 0, 0));
    CHECK_NOTHROW(parse_plain_revision(got.substr(0, 3 * 3000), 0, 0));
}

TEST_CASE("对话形式：之前的来回要带上") {
    const Story s = a_story();
    const std::vector<ReviseTurn> history = {
        {"user", "铺一下情绪"},
        {"assistant", "把那段拉长了一点"},
        {"user", "再短一点"},
    };
    const std::string p = build_revise_prompt(s, mid_span(), "再短一点", history,
                                              StyleLine::REALISTIC);
    // 用户说"再短一点"的时候，"一点"是相对上一版说的。丢了这段历史，
    // 模型只能从原文重新出发，于是改了三轮还在原地。
    CHECK(p.find("铺一下情绪") != std::string::npos);
    CHECK(p.find("把那段拉长了一点") != std::string::npos);
    CHECK(p.find("作者：") != std::string::npos);
    CHECK(p.find("你：") != std::string::npos);
}

TEST_CASE("解析：要 text，空的不行") {
    CHECK(parse_revision(json{{"text", "改好的一段"}}.dump(), 7).text ==
          "改好的一段");
    CHECK(parse_revision(json{{"text", "改好的"}, {"note", "铺了情绪"}}.dump(), 7)
              .note == "铺了情绪");

    CHECK_THROWS(parse_revision("模型今天想聊点别的", 7));
    CHECK_THROWS(parse_revision(json{{"note", "只有说明"}}.dump(), 7));
    CHECK_THROWS(parse_revision(json{{"text", "   "}}.dump(), 7));
}

TEST_CASE("解析：拦住把整章抄回来") {
    // 那样落盘之后整章内容会翻倍，而界面上只显示"改好了"——多出来的那一份
    // 要等写剧本时才发现，那时候已经隔了好几步。
    std::string huge;
    for (int i = 0; i < 400; ++i) huge += "很长的一段话。";
    CHECK_THROWS(parse_revision(json{{"text", huge}}.dump(), 7));

    // **变短是合法的。**「把这段压缩成一句」就该变短，拦下限会把一个正当
    // 的要求变成报错。
    CHECK(parse_revision(json{{"text", "一句。"}}.dump(), 700).text == "一句。");
    // 传 0 表示不检查长度
    CHECK(parse_revision(json{{"text", huge}}.dump(), 0).text == huge);
}

TEST_CASE("写回去：只换选中那一段，前后一个字不动") {
    const Story s = a_story();
    const Story next = apply_revision(s, mid_span(), "换过的第二段。");

    CHECK(next.chapters[0].text == "第一段。\n\n换过的第二段。\n\n第三段。");
    // 别的字段不受影响
    CHECK(next.chapters[0].title == s.chapters[0].title);
    CHECK(next.logline == s.logline);
    // 原来那份没被改（值语义，不是就地改）
    CHECK(s.chapters[0].text == "第一段。\n\n第二段在中间。\n\n第三段。");
}

TEST_CASE("写回去：有说法的钩子跟着挪，别被无名的顶掉") {
    const Story s = a_story();
    // 换成更长的一段：后面的钩子要往后挪
    const Story next = apply_revision(s, mid_span(), "换过的第二段长一些。");
    const int delta = 10 - 7;  // 新的 10 字，原来 7 字

    auto named_at = [](const Story& st, const std::string& what) -> int {
        for (const auto& h : st.chapters[0].hooks) {
            if (h.text == what) return h.at_char;
        }
        return -1;
    };

    // **有说法的钩子是一章停在哪的全部依据**（实跑里它把"停在真悬念上"
    // 的比例从 25% 抬到 56%）。图省事整章重算 paragraph_hooks 的话，这些
    // 会被一批无名的段落边界悄悄顶掉——每改一段就掉一批，界面上毫无反应。
    CHECK(named_at(next, "他推门进来") == 15 + delta);
    CHECK(named_at(next, "伞留在门口") == 19 + delta);
    // 位置是单调不减的：按位置对位的那几处都在有序集合里找最近的一条，乱序会找错
    for (std::size_t i = 1; i < next.chapters[0].hooks.size(); ++i) {
        CHECK(next.chapters[0].hooks[i - 1].at_char <=
              next.chapters[0].hooks[i].at_char);
    }
}

TEST_CASE("写回去：落在被换掉那段里面的钩子丢掉") {
    Story s = a_story();
    // 在选中区间**内部**加一个有说法的钩子
    s.chapters[0].hooks.push_back({9, "这句话马上要没了"});

    const Story next = apply_revision(s, mid_span(), "换过的。");
    for (const auto& h : next.chapters[0].hooks) {
        // 它指着的那句话已经不在了，留着的话界面上会指着一句不存在的悬念
        CHECK(h.text != "这句话马上要没了");
    }
}

TEST_CASE("写回去：越界夹住，不越过正文两头") {
    const Story s = a_story();
    const int len = s.chapters[0].text_len();

    const Story a = apply_revision(s, Span{"ch01", -10, 4}, "开头。");
    CHECK(a.chapters[0].text.rfind("开头。", 0) == 0);

    const Story b = apply_revision(s, Span{"ch01", len, len + 99}, "结尾。");
    CHECK(b.chapters[0].text.size() > s.chapters[0].text.size());
    CHECK(b.chapters[0].text.find("第三段。结尾。") != std::string::npos);

    CHECK_THROWS(apply_revision(s, Span{"没这一章", 0, 1}, "x"));
}

TEST_CASE("写回去：整章换掉也行，故事照样校验得过") {
    const Story s = a_story();
    const Story next =
        apply_revision(s, Span{"ch01", 0, s.chapters[0].text_len()},
                       "整章重写了一遍。\n\n第二段。");
    CHECK(next.chapters[0].text == "整章重写了一遍。\n\n第二段。");
    CHECK(next.validate().empty());
    // 钩子全在被换掉的范围里，所以有说法的一个都不剩，只有新算的段落边界
    for (const auto& h : next.chapters[0].hooks) CHECK(h.text.empty());
}

// ---------------------------------------------------------------------------
// 流式那条路：不要 JSON，直接吐正文
// ---------------------------------------------------------------------------

TEST_CASE("plain 提示词明说不要 JSON") {
    const std::string p = build_revise_prompt(a_story(), mid_span(), "铺一下", {},
                                              StyleLine::REALISTIC, true);
    // 逐字插进编辑器的话，用户先看到的会是 {"text":" 这几个字符
    CHECK(p.find("不要 JSON") != std::string::npos);
    CHECK(p.find("放进 text") == std::string::npos);

    // 非 plain 那条照旧要 JSON
    const std::string q = build_revise_prompt(a_story(), mid_span(), "铺一下", {},
                                              StyleLine::REALISTIC, false);
    CHECK(q.find("放进 text") != std::string::npos);
}

TEST_CASE("大白话解析：整段就是正文") {
    CHECK(parse_plain_revision("改完的一段。", 7).text == "改完的一段。");
    CHECK(parse_plain_revision("  改完的一段。  ", 7).text == "改完的一段。");
    CHECK_THROWS(parse_plain_revision("   ", 7));
}

TEST_CASE("大白话解析：剥掉模型自作主张加的包装") {
    // 这些字会**原样落进正文**，而正文是后面写剧本的输入——一句「修改后：」
    // 能一路活到分镜表里去。
    CHECK(parse_plain_revision("修改后：改完的一段。", 7).text == "改完的一段。");
    CHECK(parse_plain_revision("改写后:改完的一段。", 7).text == "改完的一段。");
    CHECK(parse_plain_revision("```\n改完的一段。\n```", 7).text == "改完的一段。");
    CHECK(parse_plain_revision("```text\n改完的一段。\n```", 7).text == "改完的一段。");

    // **正文里真出现这几个字不能动。** 人物说了「修改后果自负」的话，
    // 那是内容——只剥"开头就是它、后面紧跟冒号"的情况。
    CHECK(parse_plain_revision("修改后果自负，他说。", 7).text ==
          "修改后果自负，他说。");
}

TEST_CASE("大白话解析：照样拦整章吐回来") {
    std::string huge;
    for (int i = 0; i < 400; ++i) huge += "很长的一段话。";
    CHECK_THROWS(parse_plain_revision(huge, 7));
    // 变短合法
    CHECK(parse_plain_revision("一句。", 700).text == "一句。");
}

TEST_CASE("长度上限：短选区上的「拉长」不该被误伤") {
    // **2026-09-11 实跑撞到的。** 选中 40 字说"把这段拉长一点，多写点环境"，
    // 模型写了 643 字，整章 1889 字——它显然没抄整章，却撞上
    // `max(600, 40×6)` 那个 600 的地板被打回，报错还一口咬定"八成是把整章
    // 抄回来了"。指向完全错的方向，用户只会以为模型抽风。
    std::string got;
    for (int i = 0; i < 643; ++i) got += "字";
    // 整章 1889 字，六成 = 1133，643 在里面
    CHECK(parse_plain_revision(got, 40, 1889).text.size() > 0);

    // **真抄整章照样拦。** 1889 > 1133。
    std::string whole;
    for (int i = 0; i < 1889; ++i) whole += "字";
    CHECK_THROWS(parse_plain_revision(whole, 40, 1889));
}

TEST_CASE("长度上限：不知道整章多长时，退回只看倍数") {
    // whole 给 0 就是老规矩。粘贴导入那条路上拿不到整章长度。
    std::string got;
    for (int i = 0; i < 643; ++i) got += "字";
    CHECK_THROWS(parse_plain_revision(got, 40, 0));
    CHECK_THROWS(parse_plain_revision(got, 40));
}

TEST_CASE("长度上限：选区大的时候倍数那条说了算") {
    // 选中 1000 字的一大段，扩写成两倍是正当的；这时候"整章六成"反而更紧，
    // 所以取松的那条。
    std::string got;
    for (int i = 0; i < 2000; ++i) got += "字";
    CHECK(parse_plain_revision(got, 1000, 1889).text.size() > 0);
}

TEST_CASE("长度上限：报错不猜原因，只说事实和下一步") {
    std::string whole;
    for (int i = 0; i < 5000; ++i) whole += "字";
    try {
        parse_plain_revision(whole, 40, 1889);
        FAIL("该抛没抛");
    } catch (const std::exception& e) {
        const std::string why = e.what();
        // 猜错的原因比不说更糟：它把人往错的方向支
        CHECK(why.find("抄回来") == std::string::npos);
        CHECK(why.find("上限") != std::string::npos);
        CHECK(why.find("选大一点") != std::string::npos);
    }
}
