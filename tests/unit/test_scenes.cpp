// 按场拆镜（2026-09-15）。
//
// 剧本多一种拍子 kind=scene，渲染成场次头「【第1场 · 夜 · 内 · 天台】」；
// 分镜按场次头切成几场，两场以上一场一次拆，每场的地点由引擎盖上去。
// 没有场次头的老剧本走整章那条路，一个字不变——那一条要钉死，它是三份
// 逐字节语料还能过的前提。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "llm/client.hpp"
#include "models/character.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/storyboard_run.hpp"
#include "stages/script.hpp"
#include "stages/storyboard.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

models::AssetLibrary make_assets() {
    models::AssetLibrary a;
    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性";
    lin.appearance.face = "黑色长直发";
    lin.appearance.attire = "白色衬衫";
    a.characters["c_lin_wan"] = lin;
    models::Character chen;
    chen.char_id = "c_chen_mo";
    chen.name = "陈默";
    chen.appearance.identity = "三十出头男性";
    chen.appearance.face = "短寸黑发";
    chen.appearance.attire = "黑色风衣";
    a.characters["c_chen_mo"] = chen;

    models::Location roof;
    roof.location_id = "loc_rooftop";
    roof.name = "夜晚天台";
    roof.space = "水泥地面";
    roof.lighting = "夜，冷光";
    a.locations["loc_rooftop"] = roof;
    models::Location cafe;
    cafe.location_id = "loc_cafe";
    cafe.name = "咖啡馆门口";
    cafe.space = "玻璃门";
    cafe.lighting = "日，柔光";
    a.locations["loc_cafe"] = cafe;
    a.style.global_style = "冷调";
    return a;
}

const char* kTwoSceneScript =
    "【开场钩子 0–5 秒】\n"
    "【第1场 · 夜 · 内 · 天台】\n"
    "林晚站在天台边缘。\n"
    "林晚：你来了。\n"
    "【冲突推进 5–33 秒】\n"
    "【第2场 · 日 · 外 · 咖啡馆门口】\n"
    "陈默推门出来。\n"
    "陈默：我不该来。\n";

/// 按顺序回固定答复、记下每次的提示词。
class ScriptedClient : public llm::Client {
public:
    explicit ScriptedClient(std::vector<std::string> replies)
        : replies_(std::move(replies)) {}
    std::string complete(const llm::Request& req, pipeline::CancelToken&) override {
        prompts.push_back(req.prompt);
        schemas.push_back(json(req.schema));
        if (next_ >= replies_.size()) throw llm::LlmError("没有更多的假答复了");
        return replies_[next_++];
    }
    std::vector<std::string> prompts;
    std::vector<json> schemas;

private:
    std::vector<std::string> replies_;
    std::size_t next_ = 0;
};

std::string one_shot_reply(const std::string& id, const std::string& who,
                           const std::string& picture, const std::string& line) {
    json shot = {
        {"shot_id", id}, {"scene_id", "x"}, {"order", 0},
        {"visual_desc", picture}, {"first_frame_prompt", picture},
        {"motion_prompt", "她抬头看向远处的灯，慢慢转身"},
        {"shot_size", "MS"}, {"camera_angle", "low"}, {"camera_move", "push_in"},
        {"lens", "portrait"}, {"lighting", "夜，路灯从左上斜射，硬光"},
        {"duration_s", 5.0}, {"continuous_with_prev", true},
        {"characters", json::array({{{"char_id", who}}})},
        {"dialogue", json::array({{{"char_id", who}, {"text", line}}})},
    };
    return json{{"shots", json::array({shot})}}.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// 剧本层：场次头
// ---------------------------------------------------------------------------

TEST_CASE("场次头：渲染、识别，和段头分得开") {
    CHECK(stages::scene_header(1, "夜 · 内 · 天台") == "【第1场 · 夜 · 内 · 天台】");
    CHECK(stages::scene_header(2, "") == "【第2场】");

    int idx = 0;
    std::string body;
    CHECK(stages::parse_scene_header("【第1场 · 夜 · 内 · 天台】", &idx, &body));
    CHECK(idx == 1);
    CHECK(body == "夜 · 内 · 天台");
    CHECK(stages::parse_scene_header("【第12场：日 / 外 / 街口】", &idx, &body));
    CHECK(idx == 12);
    CHECK(body == "日 / 外 / 街口");
    CHECK(stages::parse_scene_header("【第2场】", &idx, &body));
    CHECK(idx == 2);
    CHECK(body.empty());

    CHECK_FALSE(stages::is_scene_header("【开场钩子 0–5 秒】"));
    CHECK_FALSE(stages::is_scene_header("【字幕】三年后"));
    CHECK_FALSE(stages::is_scene_header("【第一场】"));   // 只认阿拉伯数字，渲染的就是它
    CHECK_FALSE(stages::is_scene_header("林晚：走。"));
    CHECK_FALSE(stages::is_act_header("【第1场 · 夜 · 内 · 天台】"));
}

TEST_CASE("kind=scene 的拍子渲染成场次头，序号是数出来的") {
    stages::ScriptDraft d;
    d.beats = {
        stages::Beat{"scene", "", "夜 · 内 · 天台"},
        stages::Beat{"action", "", "林晚站在天台边缘。"},
        stages::Beat{"dialogue", "林晚", "你来了。"},
        stages::Beat{"scene", "", "日 · 外 · 咖啡馆门口"},
        stages::Beat{"dialogue", "陈默", "我不该来。"},
    };
    CHECK(d.render() ==
          "【第1场 · 夜 · 内 · 天台】\n林晚站在天台边缘。\n林晚：你来了。\n"
          "【第2场 · 日 · 外 · 咖啡馆门口】\n陈默：我不该来。");
    // 场次头不是台词，字数和说话人都不算它
    CHECK(d.dialogue_chars() == 9);
    CHECK(d.speakers() == std::vector<std::string>{"林晚", "陈默"});
    // 段头去掉之后场次头还在：它是内容，不是形状
    CHECK(has(stages::strip_act_headers(d.render()), "【第2场"));
}

TEST_CASE("模型回的 scene 拍子解析进来，四段和平的都认") {
    const std::string raw =
        R"({"title":"雨","logline":"x","beats":[
            {"kind":"scene","speaker":"","text":"（夜 · 内 · 天台）"},
            {"kind":"dialogue","speaker":"林晚","text":"你来了。"}]})";
    const stages::ScriptDraft d = stages::parse_script(raw, 60.0);
    REQUIRE(d.beats.size() == 2);
    CHECK(d.beats[0].kind == "scene");
    CHECK(d.beats[0].text == "夜 · 内 · 天台");   // 括号削掉
    CHECK(d.render().rfind("【第1场 · 夜 · 内 · 天台】\n", 0) == 0);

    const std::string four = R"({"title":"雨","logline":"她等到了",
      "opening":{"beats":[{"kind":"scene","speaker":"","text":"夜 · 内 · 天台"},
                          {"kind":"dialogue","speaker":"林晚","text":"你来了。"}]},
      "escalation":{"beats":[{"kind":"dialogue","speaker":"陈默","text":"我不该来。"}]},
      "payoff":{"beats":[{"kind":"scene","speaker":"","text":"日 · 外 · 街口"},
                         {"kind":"action","speaker":"","text":"她把伞递过去。"}]},
      "cliff":{"beats":[{"kind":"dialogue","speaker":"陈默","text":"伞不是我的。"}]}})";
    const stages::ScriptDraft d4 = stages::parse_script(four, 60.0);
    const std::string text = d4.render();
    CHECK(has(text, "【开场钩子 0–5 秒】\n【第1场 · 夜 · 内 · 天台】\n林晚：你来了。"));
    CHECK(has(text, "【第2场 · 日 · 外 · 街口】\n她把伞递过去。"));
}

TEST_CASE("数拍子时场次头不算") {
    CHECK(stages::count_beats(kTwoSceneScript) == 4);
}

TEST_CASE("剧本的 schema 里 kind 多了 scene") {
    const json s = json(stages::script_schema());
    const json& kind = s.at("properties").at("beats").at("items").at("properties").at("kind");
    CHECK(kind.at("enum") == json::array({"action", "dialogue", "scene"}));
}

// ---------------------------------------------------------------------------
// 分镜层：切场
// ---------------------------------------------------------------------------

TEST_CASE("场次头那一截拆成时段、内外、地点") {
    stages::SceneBlock b;
    stages::parse_scene_body("夜 · 内 · 天台", b);
    CHECK(b.time == "夜");
    CHECK(b.inout == "内");
    CHECK(b.place == "天台");
    stages::parse_scene_body("日/外/咖啡馆门口", b);
    CHECK(b.time == "日");
    CHECK(b.inout == "外");
    CHECK(b.place == "咖啡馆门口");
    stages::parse_scene_body("黄昏，室内，林晚家客厅", b);
    CHECK(b.time == "黄昏");
    CHECK(b.inout == "室内");
    CHECK(b.place == "林晚家客厅");
    // 只写了地点
    stages::parse_scene_body("天台", b);
    CHECK(b.time.empty());
    CHECK(b.place == "天台");
    stages::parse_scene_body("", b);
    CHECK(b.place.empty());
}

TEST_CASE("地点名接到资产库：全等优先，其次互相包含，最后按顺序的子序列") {
    const auto a = make_assets();
    CHECK(stages::resolve_scene_location("夜晚天台", a) == std::optional<std::string>("loc_rooftop"));
    CHECK(stages::resolve_scene_location("天台", a) == std::optional<std::string>("loc_rooftop"));
    CHECK(stages::resolve_scene_location("咖啡馆门口的台阶", a) == std::optional<std::string>("loc_cafe"));
    CHECK_FALSE(stages::resolve_scene_location("医院走廊", a).has_value());
    CHECK_FALSE(stages::resolve_scene_location("", a).has_value());

    // **剧本里的地名常常比资产库那个多几个字。**
    // 2026-09-16 实测 ep06：资产库「城南酒吧」，剧本「城南深巷小酒吧」，
    // 中间插了三个字，上面两条一条都不中——那一场六镜 location_id 全空，
    // 场景层不拼、空景图不喂，六镜各画各的酒吧。
    models::AssetLibrary b = make_assets();
    models::Location bar;
    bar.location_id = "loc_chengnan_bar";
    bar.name = "城南酒吧";
    bar.space = "木质吧台";
    b.locations["loc_chengnan_bar"] = bar;
    models::Location office;
    office.location_id = "loc_song_office";
    office.name = "宋律师办公室";
    b.locations["loc_song_office"] = office;

    CHECK(stages::resolve_scene_location("城南深巷小酒吧", b) ==
          std::optional<std::string>("loc_chengnan_bar"));

    // **要按顺序，不能只看重合几个字。**「曾老板办公室」和「宋律师办公室」
    // 重合五个字，但「曾」根本不在里面，顺序一对就分得开。
    CHECK_FALSE(stages::resolve_scene_location("曾老板办公室", b) ==
                std::optional<std::string>("loc_song_office"));
    // 同理，别把「城南仓库」认成「城南酒吧」
    CHECK_FALSE(stages::resolve_scene_location("城南仓库", b).has_value());

    // 名字太短的不参与这一手，免得一两个字什么都能匹配上
    models::AssetLibrary c;
    models::Location door;
    door.location_id = "loc_door";
    door.name = "门口";
    c.locations["loc_door"] = door;
    CHECK_FALSE(stages::resolve_scene_location("公安局门前的台阶", c).has_value());
}

TEST_CASE("按场次头切场：段头归到它后面那一场，序号按出现次序数") {
    const auto scenes = stages::split_scenes(kTwoSceneScript, make_assets());
    REQUIRE(scenes.size() == 2);
    CHECK(scenes[0].index == 1);
    CHECK(scenes[0].place == "天台");
    CHECK(scenes[0].location_id == std::optional<std::string>("loc_rooftop"));
    // 第一个场次头之前的段头归到第一场；场次头本身不在 text 里
    CHECK(scenes[0].text == "【开场钩子 0–5 秒】\n林晚站在天台边缘。\n林晚：你来了。\n【冲突推进 5–33 秒】");
    CHECK(scenes[1].index == 2);
    CHECK(scenes[1].time == "日");
    CHECK(scenes[1].location_id == std::optional<std::string>("loc_cafe"));
    CHECK(scenes[1].text == "陈默推门出来。\n陈默：我不该来。");

    SUBCASE("模型编的序号不作数") {
        const auto s = stages::split_scenes("【第7场 · 夜 · 内 · 天台】\n林晚：走。\n【第3场】\n陈默：好。",
                                            make_assets());
        REQUIRE(s.size() == 2);
        CHECK(s[0].index == 1);
        CHECK(s[1].index == 2);
        CHECK(s[1].body.empty());
        CHECK_FALSE(s[1].location_id.has_value());
    }
    SUBCASE("没有场次头就是一场，index 0，text 是整份") {
        const std::string old = "林晚站在天台边缘。\n林晚：你来了。";
        const auto s = stages::split_scenes(old, make_assets());
        REQUIRE(s.size() == 1);
        CHECK(s[0].index == 0);
        CHECK(s[0].text == old);
    }
}

TEST_CASE("时长按各场的拍数分，每场至少最短那一档") {
    auto scenes = stages::split_scenes(kTwoSceneScript, make_assets());
    stages::assign_scene_seconds(scenes, 60.0);
    // 两场各两拍：对半
    CHECK(scenes[0].seconds == doctest::Approx(30.0));
    CHECK(scenes[1].seconds == doctest::Approx(30.0));
    auto tiny = stages::split_scenes("【第1场 · 夜 · 内 · 天台】\n林晚：走。\n【第2场】\n"
                                     "陈默推门。\n陈默：好。\n陈默：走。\n陈默：快。\n"
                                     "陈默：别回头。\n陈默：走。\n陈默：走。\n陈默：走。\n陈默：走。",
                                     make_assets());
    stages::assign_scene_seconds(tiny, 10.0);
    CHECK(tiny[0].seconds >= stages::duration_slots().front());
}

TEST_CASE("一场的提示词：钉死地点和时段，带共用的硬性要求，上一场的收尾按需带") {
    auto scenes = stages::split_scenes(kTwoSceneScript, make_assets());
    stages::assign_scene_seconds(scenes, 60.0);
    const auto quota = stages::DurationQuota::for_duration(scenes[1].seconds);
    const std::string p = stages::build_scene_storyboard_prompt(
        scenes[1], 2, make_assets(), quota, "ep01", "林晚回头看向楼梯口");
    CAPTURE(p);
    CHECK(has(p, "这一章共 2 场，这是第 2 场。"));
    CHECK(has(p, "这一场：日 · 外 · 咖啡馆门口"));
    CHECK(has(p, "location_id 一律填 loc_cafe。"));
    CHECK(has(p, "上一场收在：\n  林晚回头看向楼梯口"));
    CHECK(has(p, "不要标 continuous_with_prev"));
    CHECK(has(p, "  c_lin_wan：林晚"));
    CHECK(has(p, "  loc_cafe：咖啡馆门口"));
    CHECK(has(p, "1. shot_id 用 ep01_sh001 这样的格式"));
    // 共用的那份规则要在。
    //
    // **锚点挑的是"只可能出自那份规则"的句子，不是随手两行。** 2026-09-16
    // 这儿原来钉的是「2. order 从 0 开始递增。」和「lighting 必须填」——
    // 那两条当天砍了（order 被 renumber_shots 整个重排，模型写什么都覆盖；
    // lighting 那条和字段 description 一字不差）。钉具体措辞太脆，改钉
    // 那几条**只能写在规则里、schema 管不着**的：进画出画、开门、景别节奏。
    CHECK(has(p, "不写进画、出画"));
    CHECK(has(p, "不写开门、开窗、拉抽屉"));
    CHECK(has(p, "整场不能只用一种"));
    CHECK(has(p, "这一场的剧本：\n\n陈默推门出来。\n陈默：我不该来。"));
    CHECK(has(p, "只输出 JSON"));

    SUBCASE("第一场没有上一场；地点接不上时说清楚") {
        stages::SceneBlock first = scenes[0];
        first.location_id.reset();
        const std::string q = stages::build_scene_storyboard_prompt(
            first, 2, make_assets(), quota, "ep01", "");
        CHECK_FALSE(has(q, "上一场收在"));
        CHECK(has(q, "地点不在场景清单里"));
    }
}

TEST_CASE("一场的 schema 把 location_id 钉成这一场的") {
    const auto a = make_assets();
    const json s = json(stages::llm_scene_shot_schema(a, {}, std::string("loc_cafe")));
    const json& item = s.at("properties").at("shots").at("items");
    CHECK(item.at("properties").at("location_id").at("enum") == json::array({"loc_cafe"}));
    bool req = false;
    for (const auto& v : item.at("required")) {
        if (v == "location_id") req = true;
    }
    CHECK(req);
    // 接不上就和整章那份一样
    CHECK(json(stages::llm_scene_shot_schema(a, {}, std::nullopt)) ==
          json(stages::llm_shot_schema(a, {})));
}

TEST_CASE("盖场景的印：scene_id、location_id 统一，第一镜不接上一场的帧") {
    auto scenes = stages::split_scenes(kTwoSceneScript, make_assets());
    std::vector<models::Shot> shots(2);
    shots[0].continuous_with_prev = true;
    shots[1].continuous_with_prev = true;
    shots[1].location_id = "loc_rooftop";
    stages::stamp_scene(shots, scenes[1]);
    CHECK(shots[0].scene_id == "s2");
    CHECK(shots[1].scene_id == "s2");
    CHECK(shots[0].location_id == std::optional<std::string>("loc_cafe"));
    CHECK(shots[1].location_id == std::optional<std::string>("loc_cafe"));
    CHECK_FALSE(shots[0].continuous_with_prev);
    CHECK(shots[1].continuous_with_prev);
}

// ---------------------------------------------------------------------------
// 编排：一场走老路，两场以上一场一次
// ---------------------------------------------------------------------------

TEST_CASE("没有场次头：整章一次拆，提示词和以前逐字节一样") {
    const auto a = make_assets();
    const std::string script = "林晚站在天台边缘。\n林晚：你来了。";
    ScriptedClient client({one_shot_reply("ep01_sh001", "c_lin_wan", "天台边缘", "你来了。")});
    pipeline::StoryboardRunOptions o;
    o.script = script;
    o.assets = a;
    o.episode_id = "ep01";
    o.duration_s = 10.0;
    int progress = 0;
    o.on_progress = [&progress](const std::string&) { ++progress; };
    pipeline::CancelToken tok;
    const auto r = pipeline::run_storyboard(o, client, tok);
    REQUIRE(client.prompts.size() == 1);
    // **配额按剧本估的秒数，不按 duration_s。** 这儿原来钉的是
    // `for_duration(10.0)`——那时是先把一章定死多长，分镜按它拆、拆完再
    // 压回去。2026-09-16 用户定了只留章模式，那条路删了：一章多长由它自己
    // 的内容定。duration_s 现在只在剧本估不出秒数时兜底。
    CHECK(client.prompts[0] ==
          stages::build_storyboard_prompt(
              script, a,
              stages::DurationQuota::for_duration(
                  stages::estimate_script_seconds(script)),
              "ep01"));
    CHECK(r.scenes == 1);
    CHECK(progress == 0);
    REQUIRE(r.shots.size() == 1);
    CHECK(r.shots[0].shot_id == "ep01_sh001");
}

TEST_CASE("两场：一场一次拆，各自钉地点，合起来重编号，第二场带上一场的收尾") {
    const auto a = make_assets();
    ScriptedClient client({
        one_shot_reply("ep01_sh001", "c_lin_wan", "天台边缘的林晚", "你来了。"),
        one_shot_reply("ep01_sh001", "c_chen_mo", "咖啡馆门口的陈默", "我不该来。"),
    });
    pipeline::StoryboardRunOptions o;
    o.script = kTwoSceneScript;
    o.assets = a;
    o.episode_id = "ep01";
    o.duration_s = 60.0;
    std::vector<std::string> progress;
    o.on_progress = [&progress](const std::string& m) { progress.push_back(m); };
    pipeline::CancelToken tok;
    const auto r = pipeline::run_storyboard(o, client, tok);

    REQUIRE(client.prompts.size() == 2);
    CHECK(has(client.prompts[0], "这是第 1 场"));
    CHECK(has(client.prompts[0], "location_id 一律填 loc_rooftop"));
    CHECK_FALSE(has(client.prompts[0], "上一场收在"));
    CHECK(has(client.prompts[1], "这是第 2 场"));
    CHECK(has(client.prompts[1], "上一场收在：\n  天台边缘的林晚"));
    // 每场的 schema 都钉了自己的地点
    CHECK(client.schemas[0].at("properties").at("shots").at("items").at("properties")
              .at("location_id").at("enum") == json::array({"loc_rooftop"}));
    CHECK(client.schemas[1].at("properties").at("shots").at("items").at("properties")
              .at("location_id").at("enum") == json::array({"loc_cafe"}));

    CHECK(r.scenes == 2);
    REQUIRE(progress.size() == 2);
    CHECK(has(progress[0], "第 1/2 场"));
    CHECK(has(progress[1], "咖啡馆门口"));

    REQUIRE(r.shots.size() == 2);
    CHECK(r.shots[0].shot_id == "ep01_sh001");
    CHECK(r.shots[1].shot_id == "ep01_sh002");
    CHECK(r.shots[0].order == 0);
    CHECK(r.shots[1].order == 1);
    CHECK(r.shots[0].scene_id == "s1");
    CHECK(r.shots[1].scene_id == "s2");
    CHECK(r.shots[0].location_id == std::optional<std::string>("loc_rooftop"));
    CHECK(r.shots[1].location_id == std::optional<std::string>("loc_cafe"));
    // 模型标了紧接上一镜，跨场那一镜被抹掉
    CHECK_FALSE(r.shots[0].continuous_with_prev);
    CHECK_FALSE(r.shots[1].continuous_with_prev);
    // 焦段和光原样进来
    CHECK(r.shots[0].lens == models::Lens::PORTRAIT);
    CHECK(has(r.shots[0].lighting, "路灯"));
    // 台词都在
    REQUIRE(r.shots[0].dialogue.size() == 1);
    CHECK(r.shots[0].dialogue[0].text == "你来了。");
    REQUIRE(r.shots[1].dialogue.size() == 1);
    CHECK(r.shots[1].dialogue[0].text == "我不该来。");
}

TEST_CASE("一场的 schema 把 scene_id 也钉死：模型不必为会被盖掉的值猜") {
    // ⚠️ **拆完 `stamp_scene` 会把 scene_id 整个盖成 sN**——模型填什么都
    // 不作数，可它在 required 里，非填不可。2026-09-17 从思考流里读到的原话：
    // 「scene_id 可能填什么? 需要 pattern。可能填 ep01_sc01? …用户没指定。
    // 得选择。」整整一段推理花在一个会被覆盖的字段上。
    const auto a = make_assets();
    const json s = json(stages::llm_scene_shot_schema(
        a, {}, std::string("loc_cafe"), /*scene_index=*/3));
    const auto& item = s.at("properties").at("shots").at("items");
    const auto& sid = item.at("properties").at("scene_id");
    REQUIRE(sid.contains("enum"));
    CHECK(sid.at("enum").size() == 1);
    CHECK(sid.at("enum")[0] == "s3");
    // 两栏都得在 required 里，不然模型会整个略过（CLAUDE.md：管得住模型的
    // 不是措辞，是 required）。
    bool has_scene = false, has_loc = false;
    for (const auto& v : item.at("required")) {
        if (v == "scene_id") has_scene = true;
        if (v == "location_id") has_loc = true;
    }
    CHECK(has_scene);
    CHECK(has_loc);

    SUBCASE("不知道第几场就不钉") {
        const json t = json(stages::llm_scene_shot_schema(
            a, {}, std::string("loc_cafe"), /*scene_index=*/0));
        const auto& p = t.at("properties").at("shots").at("items")
                            .at("properties").at("scene_id");
        CHECK_FALSE(p.contains("enum"));
    }
}
