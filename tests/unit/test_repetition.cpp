// 复读检测。
//
// **用例里那段真话是实跑抓到的。** 2026-09-11，一章写了 1124 字、稳稳过了
// 600 字的下限，而「你早就走了，我只是还在等。」一字不差出现了八次。
// 只量长度不看内容的话，那段东西会一路存进 story.json，再写成剧本、排成
// 分镜、配成音、渲成片——一整条流水线为一段复读机跑了一个多小时。
//
// 所以这组用例真正要守的是两头：
//   · 复读的必须抓住（否则等于没做）
//   · **正常的文不能冤枉**（否则用户点十次有三次莫名其妙失败，比不做更糟）

#include <doctest/doctest.h>

#include <string>

#include "stages/repetition.hpp"

using changji::stages::check_repetition;
using changji::stages::split_sentences;

TEST_CASE("切句：句号问号感叹号换行都算，引号不算") {
    const auto s = split_sentences("他来了。你是谁？走开！\n下一段。");
    REQUIRE(s.size() == 4);
    CHECK(s[0] == "他来了。");
    CHECK(s[1] == "你是谁？");
    CHECK(s[2] == "走开！");
    CHECK(s[3] == "下一段。");

    // **引号不特殊对待。** 切出来是「…还在等。」加一个光秃秃的 `」`，
    // 而那样正好：前半段正是要拿去比对的那一句，剩下的引号只有一个字符，
    // 够不着长度门槛、不进统计。为这点事做引号配对是给不存在的问题写代码。
    const auto q = split_sentences("他说：「你早就走了，我只是还在等。」");
    REQUIRE(q.size() == 2);
    CHECK(q[0] == "他说：「你早就走了，我只是还在等。");
    CHECK(q[1] == "」");
}

TEST_CASE("实跑抓到的那一段：同一句八次") {
    std::string bad = "深夜便利店的灯在雨夜里泛着昏黄的光，林然正把最后一盒泡面装进货架。\n\n";
    for (int i = 0; i < 8; ++i) {
        bad += "'你早就走了，我只是还在等。'林然喃喃自语，声音在空荡荡的店里回荡。\n\n";
        bad += "沈悠看着他，眼里满是愧疚，她知道自己已经无法再回到过去了。\n\n";
    }
    const auto r = check_repetition(bad);
    CHECK_FALSE(r.ok);
    CHECK(r.worst_count >= 3);
    // 报错要说清是哪一句——"这一章废了"而不说废在哪儿，用户只会觉得
    // 这个工具在随机报错
    CHECK(r.detail.find("你早就走了") != std::string::npos);
}

TEST_CASE("带不带引号算同一句") {
    // **不这么做会漏掉一半。** 退化循环里同一句常常一次带引号一次不带，
    // 按原样比的话它们算两句，一句也到不了三次。
    const std::string mixed =
        "'你早就走了，我只是还在等。'\n"
        "你早就走了，我只是还在等。\n"
        "「你早就走了，我只是还在等。」\n";
    const auto r = check_repetition(mixed);
    CHECK_FALSE(r.ok);
    CHECK(r.worst_count == 3);
}

TEST_CASE("短句重复不算数") {
    // 「他说。」「为什么？」在对白里反复出现一点也不奇怪。把它们算进去的话，
    // 对白密的那几章会被一律判废。
    std::string dialog;
    for (int i = 0; i < 10; ++i) {
        dialog += "他说。\n为什么？\n我知道。\n";
    }
    CHECK(check_repetition(dialog).ok);
}

TEST_CASE("正常的文不冤枉") {
    const std::string good =
        "深夜便利店的灯在雨夜里泛着昏黄的光，林然正把最后一盒泡面装进货架。\n\n"
        "门铃叮地一声响，他抬头，看见熟悉的身影站在门口，雨水顺着她的发梢滴落。\n\n"
        "那把伞他一眼就认出来了，伞骨上还留着那个歪歪扭扭的刻痕。\n\n"
        "他愣了两秒，声音低沉：「沈悠？」\n\n"
        "她没有说话，只是把伞放在柜台上，转身走进雨里。\n\n"
        "林然快步走到门口，雨水打在脸上，模糊了视线，他想追，最终只是站在原地。\n\n"
        "陈默坐在角落的长椅上看着这一切，轻轻叹了口气，他知道那把伞意味着什么。\n";
    const auto r = check_repetition(good);
    CHECK(r.ok);
    CHECK(r.unique_ratio > 0.9);
}

TEST_CASE("没有哪一句到三次，但整段在原地打转") {
    // 另一种形状：每句各来两遍，一句也没到三次，可整段读起来就是在绕圈。
    std::string loop;
    for (int i = 0; i < 2; ++i) {
        loop += "林然看着她，心里像是被什么狠狠揪了一下，说不出话来。\n";
        loop += "沈悠低下头，眼里满是愧疚，她知道自己无法再回到过去。\n";
        loop += "雨声渐渐变大，便利店外的路灯在雨中模糊成一片。\n";
        loop += "陈默站起身走到林然身边，轻声说了一句该放下了。\n";
    }
    const auto r = check_repetition(loop);
    CHECK_FALSE(r.ok);
    CHECK(r.unique_ratio < 0.7);
}

TEST_CASE("空的和很短的不判废") {
    // 这一层只管复读。**空正文和写太短由字数那条守卫管**——两条守卫各报
    // 各的话，用户看到的是"正文在复读：去掉重复只剩 0%"，而实际是模型
    // 一个字没写。
    CHECK(check_repetition("").ok);
    CHECK(check_repetition("就一句话。").ok);
}
