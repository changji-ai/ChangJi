// 剧本 / 选题 / 预告片阶段的对拍测试。
//
// 语料由 tests/export_script_golden.py 调**真实的 Python 函数**生成。
//
// 这一阶段的难点在文本清洗那三个函数。它们要按 UTF-8 **字符**做事——
// 括号匹配、长度判断、截断都是。按字节做会把汉字劈成半个，
// 而那个半个字节会一路流进提示词和字幕。

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "http/scripting.hpp"
#include "stages/script.hpp"
#include "util/text.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/stage_script.json";
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

std::vector<std::string> strs(const json& j) {
    return j.get<std::vector<std::string>>();
}

/// 长文本比对失败时报第一处不同，直接比看不出差在哪。
void check_text(const std::string& got, const std::string& want) {
    if (got != want) {
        std::size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
        MESSAGE("第一处不同在字节 " << i << "，长度 期望 " << want.size()
                                  << " 实得 " << got.size());
        MESSAGE("期望…" << want.substr(i > 40 ? i - 40 : 0, 110));
        MESSAGE("实得…" << got.substr(i > 40 ? i - 40 : 0, 110));
    }
    CHECK(got == want);
}

}  // namespace

TEST_CASE("削掉整段外面套的括号") {
    for (const auto& c : golden().at("wrappers")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::strip_wrapper(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("只削套住整段的那一层") {
    // 光数左右个数不够：「（甲说）乙答（丙笑）」左右各两个，数目相等，
    // 但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
    CHECK(stages::strip_wrapper("（甲说）乙答（丙笑）") == "（甲说）乙答（丙笑）");
    CHECK(stages::strip_wrapper("（前）中间（后）") == "（前）中间（后）");
    // 真的套住整段的要削
    CHECK(stages::strip_wrapper("（他犹豫了一下）") == "他犹豫了一下");
    CHECK(stages::strip_wrapper("（（（三层）））") == "三层");
    // 削完是空的就不削
    CHECK(stages::strip_wrapper("（）") == "（）");
}

TEST_CASE("削掉开头的时间码") {
    for (const auto& c : golden().at("timecodes")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::strip_leading_timecode(in) ==
              c.at("out").get<std::string>());
    }
}

TEST_CASE("时间码要既有数字又有单位才削") {
    // 只看数字的话，「（第3次）他又问」开头也会被削掉
    CHECK(stages::strip_leading_timecode("（他犹豫了）他开口") == "（他犹豫了）他开口");
    CHECK(stages::strip_leading_timecode("（第3次）他又问") == "（第3次）他又问");
    CHECK(stages::strip_leading_timecode("[0-3秒] 画面特写：雨水") == "画面特写：雨水");
}

TEST_CASE("说话人归一") {
    for (const auto& c : golden().at("speakers")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::normalize_speaker(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("none 和旁白不能当成角色名") {
    // 原样当名字用的话，成片字幕上会出现「none：寂静」
    CHECK(stages::normalize_speaker("none").empty());
    CHECK(stages::normalize_speaker("None").empty());
    CHECK(stages::normalize_speaker("旁白").empty());
    CHECK(stages::normalize_speaker("（无）").empty());
    // 正常名字要留着
    CHECK(stages::normalize_speaker("林晚") == "林晚");
    CHECK(stages::normalize_speaker("（林晚）") == "林晚");
}

TEST_CASE("对白字数预算") {
    for (const auto& c : golden().at("budgets")) {
        const double in = c.at("in").get<double>();
        CAPTURE(in);
        CHECK(stages::budget_chars(in) == c.at("out").get<int>());
    }
    // 下限 20 字。时长再短也得能说一句话。
    CHECK(stages::budget_chars(0.0) == 20);
    CHECK(stages::budget_chars(-100.0) == 20);
}

TEST_CASE("三个提示词逐字节钉住：改了必须是有意识地改") {
    // **这条原来叫"和 Python 逐字节一致"**，语料是当年冻下来的 Python
    // 答案，用来证明移植没走样。Python 引擎 2026-09-10 删了之后，这个
    // 用途就没了——而断言还在，实际效果变成"提示词永远改不动"：
    // 想让剧本写得更细，第一步就撞在这儿。
    //
    // 现在它的用途是**快照**：提示词是这套东西的行为核心，改一个字
    // 出来的剧本就不一样，所以不能被顺手改掉。要改就连语料一起改，
    // 让这件事在 diff 里看得见。
    //
    // 2026-09-11 第一次这么改：给剧本加了"动作要拍得出来"那四条
    // （8~11），语料里 22 处跟着更新。premise 和 trailer 一个字没动。

    for (const auto& c : golden().at("prompts")) {
        const std::string kind = c.at("kind").get<std::string>();
        const auto line = line_from(c.at("style_line").get<std::string>());
        CAPTURE(kind);
        CAPTURE(c.at("style_line").get<std::string>());

        std::string got;
        if (kind == "script") {
            CAPTURE(c.at("duration_s").get<double>());
            got = stages::build_script_prompt(
                c.at("premise").get<std::string>(),
                c.at("duration_s").get<double>(), line,
                c.at("previous").get<std::string>(),
                strs(c.at("characters")));
        } else if (kind == "premise") {
            got = stages::build_premise_prompt(
                c.at("keywords").get<std::string>(), line,
                c.at("count").get<int>(), strs(c.at("existing")));
        } else {
            got = stages::build_trailer_prompt(
                c.at("premise").get<std::string>(),
                c.at("duration_s").get<double>(), line,
                c.at("episodes").get<std::string>(),
                strs(c.at("characters")));
        }
        check_text(got, c.at("prompt").get<std::string>());
    }
}

TEST_CASE("时长格式化是银行家舍入") {
    // Python 的 f"{x:.0f}" 和 C 的 %.0f 都是 round-half-to-even。
    // 用四舍五入的话 0.5 会变成 1，提示词里那个数字就和 Python 不一样了。
    const std::string p0 = stages::build_script_prompt(
        "梗概", 0.5, models::StyleLine::REALISTIC);
    CHECK(p0.find("总时长约 0 秒") != std::string::npos);
    const std::string p1 = stages::build_script_prompt(
        "梗概", 1.5, models::StyleLine::REALISTIC);
    CHECK(p1.find("总时长约 2 秒") != std::string::npos);
    const std::string p2 = stages::build_script_prompt(
        "梗概", 2.5, models::StyleLine::REALISTIC);
    CHECK(p2.find("总时长约 2 秒") != std::string::npos);
}

TEST_CASE("超长的前情和章节要按字符截断") {
    // 按字节截会把最后一个汉字劈成半个，那半个字节直接进提示词。
    const std::string long_prev(5000, 'x');  // 先用 ASCII 确认长度逻辑
    const std::string p = stages::build_script_prompt(
        "梗概", 60.0, models::StyleLine::REALISTIC, long_prev);
    CHECK(p.find(std::string(4000, 'x')) != std::string::npos);
    CHECK(p.find(std::string(4001, 'x')) == std::string::npos);

    SUBCASE("中文的截断点在字符边界上") {
        std::string cn;
        for (int i = 0; i < 5000; ++i) cn += "很";
        const std::string q = stages::build_script_prompt(
            "梗概", 60.0, models::StyleLine::REALISTIC, cn);
        // 截出来的那段必须是 4000 个完整的「很」
        std::string want;
        for (int i = 0; i < 4000; ++i) want += "很";
        CHECK(q.find(want) != std::string::npos);
        CHECK(q.find(want + "很") == std::string::npos);
    }
}

TEST_CASE("解析出的剧本和 Python 一致") {
    for (const auto& c : golden().at("parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const stages::ScriptDraft d =
            stages::parse_script(c.at("raw").get<std::string>());

        CHECK(d.title == c.at("title").get<std::string>());
        CHECK(d.logline == c.at("logline").get<std::string>());

        json beats = json::array();
        for (const stages::Beat& b : d.beats) {
            beats.push_back({{"kind", b.kind},
                             {"speaker", b.speaker},
                             {"text", b.text}});
        }
        if (beats != c.at("beats")) {
            MESSAGE("期望 " << c.at("beats").dump(1));
            MESSAGE("实得 " << beats.dump(1));
        }
        CHECK(beats == c.at("beats"));
        CHECK(d.speakers() == strs(c.at("speakers")));
        CHECK(d.dialogue_chars() == c.at("dialogue_chars").get<std::size_t>());
        check_text(d.render(), c.at("render").get<std::string>());
    }
}

TEST_CASE("解析时的三处兜底") {
    const json& c = golden().at("parses").at(0);
    const stages::ScriptDraft d =
        stages::parse_script(c.at("raw").get<std::string>());

    // 一，说了话却没说是谁说的，降级成动作而不是丢掉。
    //     丢掉的话这句台词就从成片里消失了，那比配错声音还糟。
    bool found_demoted = false;
    for (const auto& b : d.beats) {
        if (b.text == "远处传来汽笛声。") {
            CHECK(b.kind == "action");
            CHECK(b.speaker.empty());
            found_demoted = true;
        }
    }
    CHECK(found_demoted);

    // 二，开头的时间码和外层引号都被削掉了
    for (const auto& b : d.beats) {
        CHECK(b.text.rfind("[0-3秒]", 0) != 0);
        CHECK(b.text.rfind("“", 0) != 0);
    }

    // 三，空文本的那一拍整条丢掉
    for (const auto& b : d.beats) CHECK_FALSE(b.text.empty());
}

TEST_CASE("剧本该报错的都报错") {
    for (const auto& c : golden().at("parse_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_script(raw), stages::ScriptError);
        } else {
            CHECK_NOTHROW(stages::parse_script(raw));
        }
    }
}

TEST_CASE("解析出的选题和 Python 一致") {
    for (const auto& c : golden().at("premise_parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const auto ideas =
            stages::parse_premises(c.at("raw").get<std::string>());

        json got = json::array();
        for (const auto& i : ideas) {
            got.push_back({{"title", i.title},
                           {"premise", i.premise},
                           {"hook", i.hook}});
        }
        if (got != c.at("ideas")) {
            MESSAGE("期望 " << c.at("ideas").dump(1));
            MESSAGE("实得 " << got.dump(1));
        }
        CHECK(got == c.at("ideas"));
    }
}

TEST_CASE("选题该报错的都报错") {
    for (const auto& c : golden().at("premise_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_premises(raw), stages::ScriptError);
        } else {
            CHECK_NOTHROW(stages::parse_premises(raw));
        }
    }
}

TEST_CASE("渲染成后面几步认的写法") {
    for (const auto& c : golden().at("renders")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        stages::ScriptDraft d;
        d.title = "t";
        d.logline = "l";
        for (const auto& b : c.at("beats")) {
            d.beats.push_back(stages::Beat{b.at("kind").get<std::string>(),
                                           b.at("speaker").get<std::string>(),
                                           b.at("text").get<std::string>()});
        }
        check_text(d.render(), c.at("render").get<std::string>());
        CHECK(d.speakers() == strs(c.at("speakers")));
        CHECK(d.dialogue_chars() == c.at("dialogue_chars").get<std::size_t>());
    }
}

TEST_CASE("对白字数按字符不按字节") {
    // 中文一个字三字节。用 size() 的话预算判断会偏大三倍，
    // 每一章都会被判成"超出很多"。
    stages::ScriptDraft d;
    d.beats.push_back(stages::Beat{"dialogue", "林晚", "你说过会来的"});
    CHECK(d.dialogue_chars() == 6);
    CHECK(text::utf8_len("你说过会来的") == 6);
}

TEST_CASE("两个 schema 和 Python 一致") {
    CHECK(json(stages::script_schema()) == golden().at("script_schema"));
    CHECK(json(stages::premise_schema()) == golden().at("premise_schema"));
}

// ---- 四段 ----
//
// 2026-09-12 加的。60 秒的章写出 13 秒的剧本，根子是剧本这一层没有承载
// 时长的形状：一个平的 beats 数组，地板写死 4。一章是四拍按秒排的，
// 时长是剧本自己长出来的——所以 schema 长成四段，每段自己的拍数地板。

TEST_CASE("一章按秒切成四段，四段拼起来是整章") {
    const auto acts = stages::act_plan(60.0);
    REQUIRE(acts.size() == 4);
    CHECK(acts[0].key == "opening");
    CHECK(acts[3].key == "cliff");
    CHECK(acts[0].from_s == 0);
    CHECK(acts[3].to_s == 60);
    int sum = 0;
    for (std::size_t i = 0; i < acts.size(); ++i) {
        CAPTURE(acts[i].key);
        CHECK(acts[i].to_s > acts[i].from_s);
        if (i) CHECK(acts[i].from_s == acts[i - 1].to_s);
        CHECK(acts[i].min_beats >= 2);
        CHECK(acts[i].max_beats > acts[i].min_beats);
        sum += acts[i].to_s - acts[i].from_s;
    }
    CHECK(sum == 60);
    // 60 秒：开场 5、推进 28、回报 21、留扣 6
    CHECK(acts[0].to_s == 5);
    CHECK(acts[1].to_s == 33);
    CHECK(acts[2].to_s == 54);
    // 开场不超过 8 秒、留扣不超过 10 秒——三分钟的集也一样，钩子不能拖
    const auto longer = stages::act_plan(180.0);
    CHECK(longer[0].to_s == 8);
    CHECK(longer[3].to_s - longer[3].from_s == 10);
    // 时长小到没法分也不能崩：银行家舍入那几条用例会传 0.5
    const auto tiny = stages::act_plan(0.5);
    REQUIRE(tiny.size() == 4);
    for (const auto& a : tiny) CHECK(a.to_s > a.from_s);
}

TEST_CASE("四段的 schema：四个键按顺序，各自带拍数的地板") {
    const json s = json(stages::script_schema(60.0));
    const auto required = s.at("required").get<std::vector<std::string>>();
    CHECK(required == std::vector<std::string>{"title", "logline", "opening",
                                               "escalation", "payoff", "cliff"});
    const json& esc = s.at("properties").at("escalation").at("properties").at("beats");
    CHECK(esc.at("minItems").get<int>() == stages::act_plan(60.0)[1].min_beats);
    CHECK(esc.at("minItems").get<int>() >= 6);
    CHECK(esc.contains("maxItems"));
    // 每一拍的字数也有地板，空拍凑数在语法层就过不去
    CHECK(esc.at("items").at("properties").at("text").at("minLength").get<int>() >= 2);
    // 平的那份不动：预告片还在用
    CHECK_FALSE(json(stages::script_schema()).at("properties").contains("opening"));
}

TEST_CASE("四段的回包拼成一份平的拍子，渲染时带段头") {
    const std::string raw = R"({"title":"雨","logline":"她等到了",
      "opening":{"beats":[{"kind":"action","speaker":"","text":"天台，雨。"},
                          {"kind":"dialogue","speaker":"林晚","text":"你来了。"}]},
      "escalation":{"beats":[{"kind":"dialogue","speaker":"陈默","text":"我不该来。"}]},
      "payoff":{"beats":[{"kind":"action","speaker":"","text":"她把伞递过去。"}]},
      "cliff":{"beats":[{"kind":"dialogue","speaker":"陈默","text":"伞不是我的。"}]}})";
    const stages::ScriptDraft d = stages::parse_script(raw, 60.0);
    REQUIRE(d.acts.size() == 4);
    CHECK(d.beats.size() == 5);
    CHECK(d.acts[0].beats.size() == 2);
    CHECK(d.acts[0].from_s == 0);
    CHECK(d.acts[0].to_s == 5);
    CHECK(d.beats[1].speaker == "林晚");
    CHECK(d.beats[4].text == "伞不是我的。");
    const std::string text = d.render();
    CHECK(text.rfind("【开场钩子 0–5 秒】\n天台，雨。\n林晚：你来了。\n【冲突推进 5–33 秒】", 0) == 0);
    CHECK(stages::is_act_header("【开场钩子 0–5 秒】"));
    // 段头去掉之后就是原来那份
    CHECK(stages::strip_act_headers(text) ==
          "天台，雨。\n林晚：你来了。\n陈默：我不该来。\n她把伞递过去。\n陈默：伞不是我的。");
    // 不带时长解析：段头没有秒数，但段还在
    const stages::ScriptDraft d0 = stages::parse_script(raw);
    CHECK(d0.acts.size() == 4);
    CHECK(d0.render().rfind("【开场钩子】\n", 0) == 0);
}

TEST_CASE("平的回包照旧，没有段头") {
    const std::string raw =
        R"({"title":"雨","logline":"x","beats":[{"kind":"dialogue","speaker":"林晚","text":"你来了。"}]})";
    const stages::ScriptDraft d = stages::parse_script(raw, 60.0);
    CHECK(d.acts.empty());
    CHECK(d.render() == "林晚：你来了。");
}

TEST_CASE("段头识别不误伤正常的拍子") {
    CHECK(stages::is_act_header("【章尾留扣 54–60 秒】"));
    CHECK(stages::is_act_header("【情绪回报】"));
    std::string label;
    int from = -1, to = -1;
    CHECK(stages::parse_act_header("【冲突推进 5-33 秒】", &label, &from, &to));
    CHECK(label == "冲突推进");
    CHECK(from == 5);
    CHECK(to == 33);
    CHECK_FALSE(stages::is_act_header("【字幕】三年后"));
    CHECK_FALSE(stages::is_act_header("【倒计时 10 秒】"));
    CHECK_FALSE(stages::is_act_header("【三年后的一天】"));
    CHECK_FALSE(stages::is_act_header("林晚：走。"));
}

TEST_CASE("提示词里写明四段各占几秒、至少几拍") {
    const std::string p =
        stages::build_script_prompt("梗概", 60.0, models::StyleLine::REALISTIC);
    CHECK(p.find("开场钩子（0–5 秒）") != std::string::npos);
    CHECK(p.find("章尾留扣（54–60 秒）") != std::string::npos);
    const auto acts = stages::act_plan(60.0);
    CHECK(p.find("至少 " + std::to_string(acts[1].min_beats) + " 拍") !=
          std::string::npos);
}

TEST_CASE("动作行开头的机位标签削掉") {
    // 实跑里模型写的那一行。不削的话渲染出来和一句台词一模一样，
    // 下游会当成一个叫「镜头特写」的角色在说话。
    CHECK(stages::strip_camera_prefix("镜头特写：病历单上的日期是三年前。") ==
          "病历单上的日期是三年前。");
    CHECK(stages::strip_camera_prefix("特写:她的手") == "她的手");
    CHECK(stages::strip_camera_prefix("全景：雨中的街道") == "雨中的街道");

    // 正文里本来就有的冒号不动
    CHECK(stages::strip_camera_prefix("牌子上写着：营业中") == "牌子上写着：营业中");
    CHECK(stages::strip_camera_prefix("林浩把箱子放下。") == "林浩把箱子放下。");
    // 冒号前太长的不是标签
    CHECK(stages::strip_camera_prefix("他盯着那块画面很久才说：走吧") ==
          "他盯着那块画面很久才说：走吧");
    // 只有标签没内容时留着，削成空串这一拍会被丢掉
    CHECK(stages::strip_camera_prefix("特写：") == "特写：");

    SUBCASE("后期和转场的标签也要削") {
        // **实跑撞上的**（预告片那条路）：
        //     黑屏前最后一帧：林浩抬头望向镜头，雨水顺着脸颊滑落……
        // 冒号前七个字，script_dialogue_pairs 认「冒号前 ≤12 字 = 说话人」，
        // 于是整句动作描写变成一个叫「黑屏前最后一帧」的人在说话；名字认不出
        // 就落成旁白，**旁白音会把它念出来**。
        CHECK(stages::strip_camera_prefix(
                  "黑屏前最后一帧：林浩抬头望向镜头，雨水顺着脸颊滑落。") ==
              "林浩抬头望向镜头，雨水顺着脸颊滑落。");
        CHECK(stages::strip_camera_prefix("淡入：清晨的街道") == "清晨的街道");
        CHECK(stages::strip_camera_prefix("定格：他回头的那一瞬") ==
              "他回头的那一瞬");

        // **不收「字幕」**：「字幕：三年后」削成「三年后」之后，让旁白念
        // 一句"三年后"其实是正当的转场处理，不该在这一层替人决定。
        CHECK(stages::strip_camera_prefix("字幕：三年后") == "字幕：三年后");
    }

    SUBCASE("只削动作行，台词不动") {
        const std::string raw =
            R"({"title":"x","logline":"y","beats":[
                {"kind":"action","speaker":"","text":"镜头特写：那张纸"},
                {"kind":"dialogue","speaker":"林浩","text":"你听我说：别走"}]})";
        const stages::ScriptDraft d = stages::parse_script(raw);
        CHECK(d.beats[0].text == "那张纸");
        CHECK(d.beats[1].text == "你听我说：别走");
    }
}

TEST_CASE("整句圆括号的是舞台提示，不是台词") {
    // **2026-09-13 实跑撞上的**（walk_c ep04 批量出分镜那一轮）：
    //     ep04_sh005  （脚步声）
    //     ep04_sh010  （旁白/环境音）
    // 两条都落在台词字段、char_id 为空，会用旁白音念出来，字幕上也照写。
    //
    // 规则从结构上来：剧本格式里这叫 parenthetical（业内叫 wrylie），
    // 是给演员的提示——怎么说、做什么动作——**从来不念**。
    CHECK(stages::is_stage_direction("（脚步声）"));
    CHECK(stages::is_stage_direction("（旁白/环境音）"));
    CHECK(stages::is_stage_direction("(beat)"));
    CHECK(stages::is_stage_direction("  （低声）  "));   // 首尾空白不算数

    SUBCASE("没被整句包住的不算") {
        // 首尾各有一个括号，但不是一对——整句并没有被包住。
        CHECK_FALSE(stages::is_stage_direction("（甲）说完（乙）"));
        CHECK_FALSE(stages::is_stage_direction("（他犹豫）我不去"));
        CHECK_FALSE(stages::is_stage_direction("我不去（他犹豫）"));
        CHECK_FALSE(stages::is_stage_direction("你来了。"));
        CHECK_FALSE(stages::is_stage_direction(""));
    }

    SUBCASE("段头不能当提示删掉") {
        // 【开场钩子 0–5 秒】是段头，认了 【】 就会把它删了。
        CHECK_FALSE(stages::is_stage_direction("【开场钩子 0–5 秒】"));
        CHECK_FALSE(stages::is_stage_direction("[0-3秒]"));
    }

    SUBCASE("走完整条解析：当动作行，不当台词") {
        // **要在 strip_wrapper 之前判**：它会把「（脚步声）」削成「脚步声」，
        // 括号一没就和正常台词长得一模一样了。
        const std::string raw =
            R"({"title":"x","logline":"y","beats":[
                {"kind":"dialogue","speaker":"林浩","text":"（脚步声）"},
                {"kind":"dialogue","speaker":"林浩","text":"我不去。"}]})";
        const stages::ScriptDraft d = stages::parse_script(raw);
        REQUIRE(d.beats.size() == 2);
        CHECK(d.beats[0].kind == "action");     // 不是 dialogue
        CHECK(d.beats[0].text == "脚步声");
        CHECK(d.beats[1].kind == "dialogue");
        CHECK(d.beats[1].text == "我不去。");
        // 对白字数不该把提示算进去
        CHECK(d.dialogue_chars() == 4);
    }
}

TEST_CASE("开头漏出来的 markdown 列表符号削掉") {
    // **2026-09-13 实跑撞上的**（walk_c ep02）：剧本里那一行是
    //     林浩：-为什么要在一家普通餐厅下单？
    // 那个 `-` 是 markdown 列表符号漏进了字符串字段（「JSON bleed」）——
    // GBNF 约束的是 JSON 的结构，字段的内容照样带训练数据里的格式痕迹。
    // 它会一路走到字幕上，观众看得见。
    CHECK(stages::strip_list_marker("-为什么要在一家普通餐厅下单？") ==
          "为什么要在一家普通餐厅下单？");
    CHECK(stages::strip_list_marker("- 你好") == "你好");
    CHECK(stages::strip_list_marker("* 你好") == "你好");
    CHECK(stages::strip_list_marker("• 你好") == "你好");

    SUBCASE("破折号开头不动——那是正当写法") {
        // 中文里破折号开头表示话被打断或话外补白，削了是改文意。
        CHECK(stages::strip_list_marker("——我不去。") == "——我不去。");
        CHECK(stages::strip_list_marker("—等等！") == "—等等！");
    }

    SUBCASE("不在开头的不动") {
        CHECK(stages::strip_list_marker("三号-B 出口") == "三号-B 出口");
        CHECK(stages::strip_list_marker("你走吧-") == "你走吧-");
    }

    SUBCASE("后面紧跟数字的减号是符号，不是列表符号") {
        // **2026-09-13 扫一份新项目的设定时发现的**：场景名是
        // 「-1层停尸间 B区 3号冷柜」——负一层。台词里同理。
        // 这是上一版 strip_list_marker 的漏洞，削了正好把意思弄反。
        CHECK(stages::strip_list_marker("-3度，冻得我手都伸不直") ==
              "-3度，冻得我手都伸不直");
        CHECK(stages::strip_list_marker("-1层停尸间") == "-1层停尸间");
        CHECK(stages::strip_list_marker("+2 分") == "+2 分");
        // 减号后面跟空格的仍然是列表符号
        CHECK(stages::strip_list_marker("- 3号出口在左边") == "3号出口在左边");
        // 跟汉字的也是
        CHECK(stages::strip_list_marker("-为什么是我？") == "为什么是我？");
    }

    SUBCASE("削成空串就不削") {
        // 「-」自己就是这一拍的全部内容时，留着比丢掉强：
        // 削空之后 parse_beats_into 会把整拍丢掉。
        CHECK(stages::strip_list_marker("-") == "-");
        CHECK(stages::strip_list_marker("- ") == "-");
    }

    SUBCASE("走完整条解析：台词和动作行都削") {
        const std::string raw =
            R"({"title":"x","logline":"y","beats":[
                {"kind":"dialogue","speaker":"林浩","text":"-为什么是我？"},
                {"kind":"action","speaker":"","text":"- 林浩推门进来。"}]})";
        const stages::ScriptDraft d = stages::parse_script(raw);
        REQUIRE(d.beats.size() == 2);
        CHECK(d.beats[0].text == "为什么是我？");
        CHECK(d.beats[1].text == "林浩推门进来。");
    }
}

TEST_CASE("台词里裹着的旁白剥掉，不然会被念出来") {
    // **2026-09-13 实跑撞上的**（walk_c ep02）：模型照抄了原文那一句，
    // 「她说，语气平静但带着一丝疲惫」是旁白却落在台词字段里，
    // 配音会连着念出来。
    CHECK(stages::strip_speech_tags(
              R"(“你来了。”她说，语气平静但带着一丝疲惫。)") == "你来了。");

    // 中文小说的对话只有四种形式，四种里引号都贴着句子的一头
    CHECK(stages::strip_speech_tags(R"(他说：“我不去。”)") == "我不去。");
    CHECK(stages::strip_speech_tags(R"(“我不去。”他说。)") == "我不去。");
    // 提示语在中：两半要接起来，不能只留一半
    CHECK(stages::strip_speech_tags(R"(“我不去，”他说，“你也别去。”)") ==
          "我不去，你也别去。");

    SUBCASE("引号夹在中间的不动——那不是对话形式") {
        // 剥了会把整句话吃掉只剩两个字，而这句话本身就是要说出口的。
        const std::string mid = R"(他说过“再见”，然后走了)";
        CHECK(stages::strip_speech_tags(mid) == mid);
    }

    SUBCASE("正常台词一个字都不动") {
        // 剧本里的台词本来就不带引号，这条路要完全无副作用。
        CHECK(stages::strip_speech_tags("你听我说：别走") == "你听我说：别走");
        CHECK(stages::strip_speech_tags("我已经下定决心了。") ==
              "我已经下定决心了。");
        CHECK(stages::strip_speech_tags("") == "");
    }

    SUBCASE("整句就是一对引号的交给 strip_wrapper，这里不碰") {
        // 引号外没有字，不算裹了旁白。
        CHECK(stages::strip_speech_tags(R"(“你来了。”)") == R"(“你来了。”)");
    }

    SUBCASE("开头重复的人名削掉") {
        CHECK(stages::strip_speech_tags("苏婉：你来了", "苏婉") == "你来了");
        CHECK(stages::strip_speech_tags("苏婉:你来了", "苏婉") == "你来了");
        // 不是这个人的名字就不动
        CHECK(stages::strip_speech_tags("林浩：你来了", "苏婉") ==
              "林浩：你来了");
    }

    SUBCASE("剥空了就别剥") {
        // 宁可多念一句旁白，也不要这一镜彻底没声音。
        const std::string empty_quote = R"(“”她说。)";
        CHECK(stages::strip_speech_tags(empty_quote) == empty_quote);
    }

    SUBCASE("走完整条解析：台词干净，动作行不受影响") {
        const std::string raw =
            R"({"title":"x","logline":"y","beats":[
                {"kind":"dialogue","speaker":"苏婉","text":"\"你来了。\"她说，语气平静但带着一丝疲惫。"},
                {"kind":"action","speaker":"","text":"她正在为一位病人换药。"}]})";
        const stages::ScriptDraft d = stages::parse_script(raw);
        REQUIRE(d.beats.size() == 2);
        CHECK(d.beats[0].text == "你来了。");
        CHECK(d.beats[1].text == "她正在为一位病人换药。");
    }
}

TEST_CASE("给了角色名，speaker 就收成枚举") {
    // 实跑里三个角色写出了四种名字：林浩、Lin Hao、LinHao、Su Wan。
    // 下一步分镜按名字找 char_id，找不到那句就变成旁白——不配音色、
    // 不做口型，字幕上还挂着 Su Wan。
    const json s = json(stages::script_schema(60.0, {"林浩", "苏婉"}));
    const json& sp = s.at("properties").at("opening").at("properties")
                      .at("beats").at("items").at("properties").at("speaker");
    REQUIRE(sp.contains("enum"));
    const auto names = sp.at("enum").get<std::vector<std::string>>();
    // 空串留给动作行
    CHECK(names == std::vector<std::string>{"林浩", "苏婉", ""});

    SUBCASE("没有角色名时不收，老项目和第一章照常") {
        const json bare = json(stages::script_schema(60.0));
        CHECK_FALSE(bare.at("properties").at("opening").at("properties")
                        .at("beats").at("items").at("properties")
                        .at("speaker").contains("enum"));
    }

    SUBCASE("平的那份不动，预告片还在用") {
        CHECK_FALSE(json(stages::script_schema()).at("properties").at("beats")
                        .at("items").at("properties").at("speaker")
                        .contains("enum"));
    }
}

TEST_CASE("形状不写死：同一个时长能摇出不一样的节奏") {
    // 写死的话 60 秒永远是 5/28/21/6，连着看几章是一个模子。
    // 总拍数还是按时长来（防 13 秒那个毛病回来），但各段怎么分随这一章变。
    const auto base = stages::act_plan(60.0);        // 不浮动，老行为
    CHECK(base[0].to_s == 5);
    CHECK(base[3].from_s == 54);

    std::set<std::string> shapes;
    for (int i = 0; i < 40; ++i) {
        const auto a = stages::act_plan(60.0, stages::random_shape());
        REQUIRE(a.size() == 4);
        // 不管怎么摇，四段首尾相接、加起来正好一章
        CHECK(a[0].from_s == 0);
        CHECK(a[3].to_s == 60);
        int beats = 0;
        for (std::size_t k = 0; k < a.size(); ++k) {
            CHECK(a[k].to_s > a[k].from_s);
            if (k) CHECK(a[k].from_s == a[k - 1].to_s);
            CHECK(a[k].min_beats >= 2);
            beats += a[k].min_beats;
        }
        // 总量守得住：60 秒怎么摇都还是十几拍起
        CHECK(beats >= 12);
        shapes.insert(std::to_string(a[0].to_s) + "/" + std::to_string(a[1].to_s) +
                      "/" + std::to_string(a[2].to_s));
    }
    // 四十次至少摇出好几种形状，不是一个模子
    CHECK(shapes.size() >= 5);
}

TEST_CASE("戏的走法也换，不只是秒数") {
    // **只浮动秒数不够。** 上一条只问了"开场占几秒"，而十章下来仍然是同一出
    // 戏演快一点演慢一点：钩子 → 推进 → 回报 → 留扣，一章不落。用户的判词
    // 「提取出来剧本时间线也都差不多」说的正是这个。
    SUBCASE("variation = 0 还是老那一套，一个字没改") {
        const auto a = stages::act_plan(60.0);
        REQUIRE(a.size() == 4);
        CHECK(a[0].label == "开场钩子");
        CHECK(a[1].label == "冲突推进");
        CHECK(a[2].label == "情绪回报");
        CHECK(a[3].label == "章尾留扣");
        CHECK(a[0].brief == "一句冲突台词或一个反常画面，三秒内有事发生，不铺垫");
    }

    SUBCASE("摇得出好几出不一样的戏") {
        std::set<std::string> kinds;
        for (std::uint32_t v = 1; v <= 60; ++v) {
            const auto a = stages::act_plan(60.0, v);
            REQUIRE(a.size() == 4);
            std::string sig;
            for (const auto& x : a) {
                CHECK_FALSE(x.label.empty());
                CHECK_FALSE(x.brief.empty());   // 说法必填，提示词和 schema 都要用
                sig += x.label + "|";
            }
            kinds.insert(sig);
        }
        // 表里现在是五组。六十个种子摇不满五组的话，多半是种子没接上，
        // 或者取模那一下把低位摊平了——两种坏法的表现都是"只出一两组"。
        CHECK(kinds.size() >= 4);
    }

    SUBCASE("槽位名不跟着变——那是 schema 的键，换了老项目就读不出来") {
        for (std::uint32_t v : {0u, 1u, 7u, 12345u, 0xffffffffu}) {
            CAPTURE(v);
            const auto a = stages::act_plan(60.0, v);
            CHECK(a[0].key == "opening");
            CHECK(a[1].key == "escalation");
            CHECK(a[2].key == "payoff");
            CHECK(a[3].key == "cliff");
        }
    }

    SUBCASE("提示词和 schema 说的是同一出戏") {
        // 三处（提示词、schema 描述、解析）各查各的词表迟早会错开一处，
        // 而错开的表现是模型看到的段和我们解析的段对不上，**不报错**。
        // 所以说法统一从 ActSpec::brief 来。
        const std::uint32_t seed = 0xabcdef01;
        const auto acts = stages::act_plan(60.0, seed);
        const std::string brief = stages::render_act_brief(acts);
        const json sc = json(stages::script_schema(60.0, {}, seed));
        for (const auto& a : acts) {
            CAPTURE(a.key);
            CHECK(brief.find(a.brief) != std::string::npos);
            CHECK(brief.find(a.label) != std::string::npos);
            const std::string desc = sc.at("properties").at(a.key).at("properties")
                                       .at("beats").at("description").get<std::string>();
            CHECK(desc.find(a.brief) != std::string::npos);
            CHECK(desc.find(a.label) != std::string::npos);
        }
    }

    SUBCASE("段头识别认得所有形状的标签") {
        // 剧本正文里那行「【当头一击 0–6 秒】」也得认出来，否则老项目
        // 反推故事时那一行会当成正文混进小说里。
        for (std::uint32_t v = 1; v <= 60; ++v) {
            const auto a = stages::act_plan(60.0, v);
            for (const auto& x : a) {
                CAPTURE(x.label);
                CHECK(stages::is_act_header(
                    stages::act_header(x.label, x.from_s, x.to_s)));
            }
        }
    }
}

TEST_CASE("摇出来的形状要和 schema、解析用的是同一个") {
    // 种子对不上的话，段头上的秒数和模型看到的不是一回事
    const std::uint32_t seed = stages::random_shape();
    const auto acts = stages::act_plan(60.0, seed);
    const json s = json(stages::script_schema(60.0, {}, seed));
    const json& esc = s.at("properties").at("escalation").at("properties").at("beats");
    CHECK(esc.at("minItems").get<int>() == acts[1].min_beats);

    const std::string raw = R"({"title":"x","logline":"y",
      "opening":{"beats":[{"kind":"dialogue","speaker":"甲","text":"一"}]},
      "escalation":{"beats":[{"kind":"dialogue","speaker":"甲","text":"二"}]},
      "payoff":{"beats":[{"kind":"dialogue","speaker":"甲","text":"三"}]},
      "cliff":{"beats":[{"kind":"dialogue","speaker":"甲","text":"四"}]}})";
    const stages::ScriptDraft d = stages::parse_script(raw, 60.0, seed);
    REQUIRE(d.acts.size() == 4);
    CHECK(d.acts[1].from_s == acts[1].from_s);
    CHECK(d.acts[1].to_s == acts[1].to_s);
}

TEST_CASE("前情只取这一章之前的几章，预告片不算") {
    // ⚠️ **这段原来在两处各写了一遍，取法还不一样**（CLAUDE.md 第七条）：
    // `batch.cpp` 那份取"最近三章写好的"，不管在这一章前面还是后面；
    // `scripting.cpp` 那份取"这一章之前的最近三章"。整部写下来时两者恰好相等
    // （那一章是写完才建出来的），所以谁也没发现——而**单写一章时前一种会
    // 把后面几章当成已经发生的事喂给模型**，模型就照着写。
    models::Project p;
    for (const char* id : {"ep01", "ep02", "trailer", "ep03", "ep04"}) {
        models::Episode ep;
        ep.episode_id = id;
        ep.script = std::string(id) + " 的剧本正文";
        p.episodes.push_back(ep);
    }

    SUBCASE("到这一章为止") {
        const std::string out = http::previous_scripts(p, "ep03");
        CHECK(out.find("ep01") != std::string::npos);
        CHECK(out.find("ep02") != std::string::npos);
        // **后面那两集一个字都不能进来**，这是这条用例的要害。
        CHECK(out.find("ep03") == std::string::npos);
        CHECK(out.find("ep04") == std::string::npos);
        // 预告片是从正片里剪出来的，拿它当上下文模型会开始抄自己的预告。
        CHECK(out.find("trailer") == std::string::npos);
    }

    SUBCASE("终点留空 = 全部已写的") {
        const std::string out = http::previous_scripts(p);
        CHECK(out.find("ep04") != std::string::npos);
        CHECK(out.find("trailer") == std::string::npos);
    }

    SUBCASE("只要最近几章：整部塞进去撑不住") {
        const std::string out = http::previous_scripts(p, "", /*keep=*/2);
        CHECK(out.find("ep01") == std::string::npos);
        CHECK(out.find("ep03") != std::string::npos);
        CHECK(out.find("ep04") != std::string::npos);
    }
}
