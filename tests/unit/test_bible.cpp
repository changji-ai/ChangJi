// 角色圣经阶段的对拍测试。
//
// 语料由 tests/export_bible_golden.py 调**真实的 Python 函数**生成。
// 在这儿手写一份"期望输出"只能证明我理解得和自己一致，
// 证明不了和 Python 一致——而后者才是对拍要回答的问题。
//
// 提示词那几条是这里最重要的：拼接要求**逐字节**一致。差一个字不会报错，
// 只会让模型的输出悄悄变一点，而那种漂移要跑几十个镜头才看得出来。

#include <doctest/doctest.h>

#include <algorithm>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "stages/bible.hpp"
#include "stages/json_extract.hpp"
#include "util/text.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/stage_bible.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

models::StyleLine line_from(const std::string& s) {
    return s == "anime" ? models::StyleLine::ANIME : models::StyleLine::REALISTIC;
}

}  // namespace

// ---------------------------------------------------------------------------
// 「和 Python 逐字节一致」那几条，2026-09-17 退役
// ---------------------------------------------------------------------------
//
// 语料是当年冻下来的 Python 答案，用来证明移植没走样。**Python 引擎
// 2026-09-10 就删了**，那个用途从那天起就没了；断言还在，实际效果变成
// 「提示词和 schema 永远改不动」。
//
// 这一天里它挡住了三处量出来的改进：`camera_move` 的分类式禁令（模型对每
// 一镜重新论证一遍，一场戏想十五万字）、从没被填过的 `visual_desc`
//（两个项目 76% 和 100% 是空的）、以及 schema 的排版。用户拍板退役。
//
// **换掉的不是"有守卫"，是"守卫钉的是什么"**：钉结构（字段在不在、必填掉
// 没掉）而不是钉字节。前者是真出事——模型按错的名字产出、或者整个略过一栏，
// 全程不报错；后者只是让人改不动一句话。
//
TEST_CASE("slug 和 Python 一致") {
    for (const auto& c : golden().at("slugs")) {
        const std::string in = c.at("input").get<std::string>();
        CAPTURE(in);
        CHECK(text::slug(in) == c.at("output").get<std::string>());
    }
}

TEST_CASE("SHA-1 自检") {
    // slug 的退路依赖它。摘要算错的话，中文角色名会得到一个稳定但错误的 id——
    // 稳定意味着测试不会因为随机性失败，错误意味着永远和 Python 对不上。
    // 用几个标准测试向量钉死。
    CHECK(text::sha1_hex("") ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(text::sha1_hex("abc") ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(text::sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // 长度卡在分组边界上，补位写错的话只有这几个会不对。
    // 55 是"补位刚好塞得下"，56 是"刚好塞不下、要多加一整个分组"，
    // 64 是整分组，119 跨两个分组。全部用 hashlib 核对过。
    CHECK(text::sha1_hex(std::string(55, 'a')) ==
          "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(text::sha1_hex(std::string(56, 'a')) ==
          "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK(text::sha1_hex(std::string(64, 'a')) ==
          "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    CHECK(text::sha1_hex(std::string(119, 'a')) ==
          "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56");
}

TEST_CASE("字段清洗和 Python 一致") {
    for (const auto& c : golden().at("cleans")) {
        const std::string in = c.at("input").get<std::string>();
        CAPTURE(in);
        CHECK(text::clean_field(in) == c.at("output").get<std::string>());
    }
}

TEST_CASE("从模型输出里抠 JSON") {
    for (const auto& c : golden().at("extracts")) {
        const std::string raw = c.at("raw").get<std::string>();
        CAPTURE(raw);
        if (c.at("ok").get<bool>()) {
            json got;
            REQUIRE_NOTHROW(got = stages::extract_json(raw));
            CHECK(got == c.at("value"));
        } else {
            // 只比"是否失败"。错误文字不算契约。
            CHECK_THROWS(stages::extract_json(raw));
        }
    }
}

TEST_CASE("写到一半被掐断的 JSON，补齐了也算抠得出来") {
    // 2026-09-16 实测：写一章跑了十分钟，日志里那份输出开头一切正常
    // （scenes 数组、第一场的 where/pov/who/goal 全齐），就因为结尾没闭合，
    //     大模型输出不符合 chapter Schema：大模型输出里找不到合法 JSON
    // 整份作废、一个字不剩。
    //
    // 上面三条路全是"整份必须完好"：直接解、围栏里那段、第一个括号平衡的
    // 片段。被掐断时一条都不成立，而前面写好的东西其实都在。
    SUBCASE("结尾没闭合") {
        const std::string raw =
            R"({"scenes":[{"where":"办公室","pov":"曾老板","who":["曾老板"]},)"
            R"({"where":"走廊","pov":")";
        json got;
        REQUIRE_NOTHROW(got = stages::extract_json(raw));
        REQUIRE(got.is_object());
        // 写完的那一场要完整留下
        REQUIRE(got.at("scenes").size() >= 1);
        CHECK(got.at("scenes")[0].at("where") == "办公室");
        CHECK(got.at("scenes")[0].at("who").size() == 1);
    }

    SUBCASE("补出来是个空壳的，还是要抛") {
        // `{不平衡的括号` 补完是 `{}`——能解，但里面什么都没有。当成功
        // 往下游送比在这儿抛更糟：下一步拿着空数据再报一个更难懂的错。
        CHECK_THROWS(stages::extract_json("{不平衡的括号"));
        CHECK_THROWS(stages::extract_json("这一段里一个括号都没有"));
        CHECK_THROWS(stages::extract_json(""));
    }

    SUBCASE("前面带着中文前缀的半截，这一道管不了") {
        // close_partial_json 从头扫，「好的，这是结果：」这种前缀会让它
        // 一开始就不是 JSON。**写下来是因为我一开始以为它能管**：
        // 那种带前缀又完好的由 find_balanced 接住，带前缀又半截的谁都接不住。
        // 真要管得加一道"先跳到第一个 { 再补齐"，但那会和 find_balanced
        // 的括号计数打架，现在没有实跑证据说它值得。
        CHECK_THROWS(stages::extract_json(R"(好的：{"a":1,"b":)"));
    }
}

TEST_CASE("抠不出 JSON 时的错误消息按字符截，不按字节") {
    // 2026-09-11 实跑：模型吐了一大段中文没收口，错误消息里 raw.substr(0, 400)
    // 截在半个汉字上，进了任务快照之后 /api/script/series 序列化 JSON 直接 500，
    // 批量写正文的进度就再也看不见了。
    std::string raw;
    for (int i = 0; i < 300; ++i) raw += "这是一段没有大括号的中文，";
    try {
        stages::extract_json(raw);
        FAIL("应该抛");
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        CHECK(msg.find("找不到合法 JSON") != std::string::npos);
        // 能装进 JSON 再 dump 出来，就说明没截在字节中间
        CHECK_NOTHROW(json(msg).dump());
        CHECK(text::utf8_len(msg) < 450);
    }
}

TEST_CASE("默认负向提示词") {
    for (const auto& c : golden().at("negatives")) {
        const std::string style = c.at("style_line").get<std::string>();
        CAPTURE(style);
        CHECK(stages::default_negative(line_from(style)) ==
              c.at("output").get<std::string>());
    }
}

TEST_CASE("解析出的资产库和 Python 一致") {
    for (const auto& c : golden().at("parses")) {
        const std::string style = c.at("style_line").get<std::string>();
        CAPTURE(style);
        const models::AssetLibrary lib = stages::parse_bible(
            c.at("raw").get<std::string>(), line_from(style),
            c.at("aspect_ratio").get<std::string>());

        json got;
        to_json(got, lib);
        if (got != c.at("assets")) {
            MESSAGE("期望 " << c.at("assets").dump(1));
            MESSAGE("实得 " << got.dump(1));
        }
        CHECK(got == c.at("assets"));
    }
}

TEST_CASE("该报错的都报错") {
    for (const auto& c : golden().at("parse_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(
                stages::parse_bible(raw, models::StyleLine::REALISTIC, "9:16"),
                stages::BibleError);
        } else {
            CHECK_NOTHROW(
                stages::parse_bible(raw, models::StyleLine::REALISTIC, "9:16"));
        }
    }
}

TEST_CASE("抠不出 JSON 时抛的是 BibleError，这是有意和 Python 不一样") {
    // Python 那边 bible._parse 里 _extract_json 抛的是 StoryboardError，
    // 而 /api/bible 只 catch BibleError——于是"模型吐了一坨不是 JSON 的东西"
    // 这个最常见的失败会穿到最外面变成 500，用户看到的是
    // "Internal Server Error" 而不是那句能照着做的提示。
    //
    // 这里统一成 BibleError，也就是 400。语料里记着 Python 的
    // python_http_status = 500，就是为了让这条差异有据可查而不是被忘掉。
    for (const auto& c : golden().at("parse_failures")) {
        if (!c.at("raises").get<bool>()) continue;
        CAPTURE(c.at("name").get<std::string>());
        CHECK_THROWS_AS(
            stages::parse_bible(c.at("raw").get<std::string>(),
                                models::StyleLine::REALISTIC, "9:16"),
            stages::BibleError);
    }

    // 确认语料里真有那条 500，别哪天 Python 改好了这里还留着注释
    bool has_500 = false;
    for (const auto& c : golden().at("parse_failures")) {
        if (c.value("python_http_status", 0) == 500) has_500 = true;
    }
    CHECK_MESSAGE(has_500,
                  "Python 侧似乎已经修好了，这条差异说明该从方案里删掉");
}

TEST_CASE("定妆 schema 该有的栏都在，而且都在 required 里") {
    // ⚠️ **这条原来叫「schema 结构和 Python 一致」**，拿当年冻下来的 Python
    // 答案整份逐字节比。Python 引擎 2026-09-10 就删了，那个用途早没了——
    // 剩下的效果只有一个：**这份 schema 的每一个字都改不动**。2026-09-17
    // 一天里它挡住了三处量出来的改进（见 CLAUDE.md 砍提示词那一节），用户
    // 拍板退役。
    //
    // 换成钉**结构**：字段少一个、必填掉一个会红，而改一句描述不会。前者
    // 是真出事（模型按错的名字产出，解析阶段拿到一堆空字符串，全程不报错），
    // 后者正是我们一直想做的事。
    const auto s = stages::bible_schema();
    for (const char* k : {"characters", "locations"}) {
        CAPTURE(k);
        REQUIRE(s.at("properties").contains(k));
        CHECK(s.at("properties").at(k).at("type") == "array");
    }
    const auto& req = s.at("required");
    CHECK(std::find(req.begin(), req.end(), nlohmann::json("characters")) != req.end());
    CHECK(std::find(req.begin(), req.end(), nlohmann::json("locations")) != req.end());

    // 角色那一栏里，程序**必须**拿得到的几项——外观是由这几栏机械拼出来
    // 的（`build_character_ref_prompt`），少一栏就是一整类描述凭空消失，
    // 而且不报错。
    const auto& ch = s.at("properties").at("characters").at("items");
    for (const char* k : {"key", "name", "identity", "face", "attire"}) {
        CAPTURE(k);
        CHECK_MESSAGE(ch.at("properties").contains(k), "定妆少了一栏");
        bool required = false;
        for (const auto& v : ch.at("required")) {
            if (v == k) required = true;
        }
        CHECK_MESSAGE(required, "这一栏掉出 required 了，模型会整个略过");
    }
}

TEST_CASE("UTF-8 截断不会切出半个汉字") {
    const std::string s = "林晚站在天台边缘";
    for (std::size_t n = 0; n <= 10; ++n) {
        const std::string got = text::truncate_utf8(s, n);
        CAPTURE(n);
        // 长度必须是 3 的倍数（全是三字节汉字），切一半就不是了
        CHECK(got.size() % 3 == 0);
        CHECK(got.size() <= s.size());
        // 必须是原串的前缀
        CHECK(s.compare(0, got.size(), got) == 0);
    }
    CHECK(text::truncate_utf8(s, 100) == s);
    CHECK(text::truncate_utf8("", 10).empty());
    // 中英混排
    CHECK(text::truncate_utf8("ab林c", 3) == "ab林");
}
