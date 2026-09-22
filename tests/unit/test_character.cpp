// Character / Location / AssetLibrary 的对拍测试。
//
// 与 test_shot.cpp 的区别：这里有一类断言是**逐字节**的。
// 方案第六节把提示词拼接单独列出来——接口契约只要求结构兼容，
// 但提示词的输出差一个标点画面重心就变，所以那部分必须字符串全等。

#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"

using namespace changji::models;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path
                    << "（语料在版本库里，Python 引擎删掉之后不再重新生成）");
    json j;
    in >> j;
    return j;
}

/// 语料里的 null 对应 C++ 的 nullopt。
void check_opt(const json& expected, const std::optional<std::string>& got,
               const std::string& where) {
    CAPTURE(where);
    if (expected.is_null()) {
        CHECK_FALSE(got.has_value());
    } else {
        REQUIRE(got.has_value());
        CHECK(*got == expected.get<std::string>());
    }
}

}  // namespace

TEST_CASE("外观块渲染与 Python 逐字节相同") {
    const json cases = load_golden("appearance_render");
    REQUIRE(cases.is_array());
    REQUIRE(cases.size() >= 5);

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const AppearanceBlock block = c.at("block").get<AppearanceBlock>();

        // 这两条是逐字节比较，不是结构比较
        CHECK(block.render(StyleLine::REALISTIC) ==
              c.at("realistic").get<std::string>());
        CHECK(block.render(StyleLine::ANIME) ==
              c.at("anime").get<std::string>());
    }
}

TEST_CASE("尾部标点确实被剥掉了，而且没劈坏汉字") {
    // 这一条盯的是"按字符剥还是按字节剥"。中文标点是 3 字节，
    // 按字节剥会把前一个汉字的尾字节吃掉，产出乱码。
    const json cases = load_golden("appearance_render");
    for (const auto& c : cases) {
        if (c.at("name").get<std::string>() != "各段都带尾部标点") continue;
        const AppearanceBlock block = c.at("block").get<AppearanceBlock>();
        const std::string got = block.render(StyleLine::REALISTIC);

        CHECK(got == c.at("realistic").get<std::string>());
        // 不该出现连续的分隔符——那正是不剥标点时的症状
        CHECK(got.find("。，") == std::string::npos);
        CHECK(got.find("，，") == std::string::npos);
        // 结果必须是合法 UTF-8：不能有孤立的续字节开头
        CHECK((static_cast<unsigned char>(got.front()) & 0xC0) != 0x80);
    }
}

TEST_CASE("角色提示词渲染，含换装与 LoRA 触发词") {
    const json g = load_golden("character_render");
    const Character c = g.at("character").get<Character>();
    const json& r = g.at("render");

    CHECK(c.char_id == "c_lin_yuan");
    CHECK(c.name == "林渊");
    CHECK(c.wardrobe.size() == 2);
    CHECK(c.lora_strength == doctest::Approx(0.85));
    std::vector<std::string> errs;
    c.validate(errs);
    CHECK(errs.empty());

    // 逐字节
    CHECK(c.render_prompt(StyleLine::REALISTIC) ==
          r.at("realistic_default").get<std::string>());
    CHECK(c.render_prompt(StyleLine::REALISTIC, "suit_soaked") ==
          r.at("realistic_soaked").get<std::string>());
    CHECK(c.render_prompt(StyleLine::ANIME) ==
          r.at("anime_default").get<std::string>());
    CHECK(c.render_prompt(StyleLine::ANIME, "casual") ==
          r.at("anime_casual").get<std::string>());
    // 认不出来的服装状态要退回默认，不是报错也不是空串
    CHECK(c.render_prompt(StyleLine::REALISTIC, "not_exist") ==
          r.at("unknown_wardrobe").get<std::string>());

    SUBCASE("换装只替换 attire 那一段") {
        const std::string base = c.render_prompt(StyleLine::REALISTIC);
        const std::string soaked = c.render_prompt(StyleLine::REALISTIC, "suit_soaked");
        CHECK(base != soaked);
        // 身份段在最前面，换装不该动它
        CHECK(base.substr(0, 12) == soaked.substr(0, 12));
    }

    SUBCASE("wardrobe_desc 的退级") {
        const json& w = g.at("wardrobe_desc");
        CHECK(c.wardrobe_desc("default") == w.at("default").get<std::string>());
        CHECK(c.wardrobe_desc("suit_soaked") == w.at("suit_soaked").get<std::string>());
        CHECK(c.wardrobe_desc("not_exist") == w.at("not_exist").get<std::string>());
        CHECK(c.wardrobe_desc("") == w.at("empty").get<std::string>());
    }

    SUBCASE("参考图按朝向退级") {
        const json& rp = g.at("ref_for_pose");
        for (auto it = rp.begin(); it != rp.end(); ++it) {
            check_opt(it.value(), c.ref_for_pose(it.key()), it.key());
        }
    }
}

TEST_CASE("场景渲染与资产库") {
    const json g = load_golden("asset_library");
    const AssetLibrary lib = g.at("library").get<AssetLibrary>();

    CHECK(lib.character_ids() ==
          g.at("character_ids").get<std::vector<std::string>>());
    CHECK(lib.location_ids() ==
          g.at("location_ids").get<std::vector<std::string>>());
    CHECK(lib.validate().empty());

    const json& lr = g.at("location_render");
    const Location& rooftop = lib.locations.at("loc_rooftop");
    const Location& alley = lib.locations.at("loc_alley");

    CHECK(rooftop.render_prompt(StyleLine::REALISTIC) ==
          lr.at("rooftop_realistic").get<std::string>());
    CHECK(rooftop.render_prompt(StyleLine::ANIME) ==
          lr.at("rooftop_anime").get<std::string>());
    // palette 为空，不该留下一个多余的分隔符
    CHECK(alley.render_prompt(StyleLine::REALISTIC) ==
          lr.at("alley_realistic").get<std::string>());

    SUBCASE("引用校验") {
        const json& v = g.at("validate_references");
        CHECK(lib.validate_references({"c_lin_yuan"}, {"loc_rooftop"}) ==
              v.at("all_known").get<std::vector<std::string>>());
        CHECK(lib.validate_references({"c_lin_yuan", "c_chen_mo"},
                                      {"loc_alley", "loc_office"}) ==
              v.at("missing_both").get<std::vector<std::string>>());
    }

    SUBCASE("往返深度相等") {
        const json round_tripped = lib;
        CHECK(round_tripped == g.at("library"));
    }
}

TEST_CASE("从身份描述猜性别") {
    for (const auto& c : load_golden("guess_gender")) {
        const std::string identity = c.at("identity").get<std::string>();
        CAPTURE(identity);
        CHECK(guess_gender(identity) == c.at("expected").get<std::string>());
    }
}

TEST_CASE("音色挑选的排序规则") {
    const json g = load_golden("pick_voice");
    const auto pool = g.at("pool").get<std::vector<std::string>>();

    for (const auto& c : g.at("cases")) {
        const std::string gender = c.at("gender").get<std::string>();
        const int index = c.at("index").get<int>();
        CAPTURE(gender);
        CAPTURE(index);

        const auto use_pool = c.contains("pool")
                                  ? c.at("pool").get<std::vector<std::string>>()
                                  : pool;
        check_opt(c.at("expected"), pick_voice(use_pool, gender, index),
                  gender + "/" + std::to_string(index));
    }
}

// ---------------------------------------------------------------------------
// 同名合并
// ---------------------------------------------------------------------------
//
// 2026-09-14 实见：一个项目 25 个场景里 10 个是重的——「临川大学旧礼堂后台」
// 占了三条 id。场景 id 是模型起的 key 拼出来的，同一个地方每次的 key 都不一样，
// 按 id 判重等于不判。

namespace {

Location loc(const std::string& id, const std::string& name,
             const std::string& space = "", const char* ref = nullptr) {
    Location l;
    l.location_id = id;
    l.name = name;
    l.space = space;
    if (ref) l.ref_empty = ref;
    return l;
}

}  // namespace

TEST_CASE("同名场景收成一条，返回丢掉的 id 指向留下的") {
    AssetLibrary lib;
    lib.locations["loc_a"] = loc("loc_a", "旧礼堂后台", "堆着道具箱");
    lib.locations["loc_b"] = loc("loc_b", "旧礼堂后台");
    lib.locations["loc_c"] = loc("loc_c", "舞台中央");

    const IdRemap remap = dedupe_locations(lib);

    REQUIRE(lib.locations.size() == 2);
    CHECK(lib.locations.contains("loc_a"));  // 描述写得全的留下
    CHECK(lib.locations.contains("loc_c"));
    CHECK_FALSE(lib.locations.contains("loc_b"));
    REQUIRE(remap.size() == 1);
    CHECK(remap.at("loc_b") == "loc_a");
}

TEST_CASE("分镜引用着的那条优先留，哪怕它描述更空") {
    AssetLibrary lib;
    lib.locations["loc_full"] = loc("loc_full", "天台", "水泥地面", "refs/x.png");
    lib.locations["loc_used"] = loc("loc_used", "天台");

    const IdRemap remap = dedupe_locations(lib, {"loc_used"});

    REQUIRE(lib.locations.size() == 1);
    CHECK(lib.locations.contains("loc_used"));
    CHECK(remap.at("loc_full") == "loc_used");
    // 留下的那条把空着的字段从丢掉的补上：描述和空景图都不该跟着丢
    CHECK(lib.locations.at("loc_used").space == "水泥地面");
    REQUIRE(lib.locations.at("loc_used").ref_empty.has_value());
    CHECK(*lib.locations.at("loc_used").ref_empty == "refs/x.png");
}

TEST_CASE("留下的那条不被丢掉的盖掉：只补空的") {
    AssetLibrary lib;
    lib.locations["loc_1"] = loc("loc_1", "小巷", "砖墙窄巷");
    lib.locations["loc_2"] = loc("loc_2", "小巷", "另一种写法");

    dedupe_locations(lib, {"loc_1"});
    CHECK(lib.locations.at("loc_1").space == "砖墙窄巷");
}

TEST_CASE("都一样时留 id 字典序最小的，结果可复现") {
    AssetLibrary a;
    a.locations["loc_zeta"] = loc("loc_zeta", "门口");
    a.locations["loc_beta"] = loc("loc_beta", "门口");
    a.locations["loc_alpha"] = loc("loc_alpha", "门口");
    AssetLibrary b = a;

    const IdRemap ra = dedupe_locations(a);
    const IdRemap rb = dedupe_locations(b);
    CHECK(a.locations.contains("loc_alpha"));
    CHECK(ra == rb);
    CHECK(ra.size() == 2);
}

TEST_CASE("名字只认前后空白，不做模糊匹配") {
    AssetLibrary lib;
    lib.locations["loc_1"] = loc("loc_1", " 旧礼堂后台 ");
    lib.locations["loc_2"] = loc("loc_2", "旧礼堂后台");
    lib.locations["loc_3"] = loc("loc_3", "旧礼堂后台走廊");  // 不是同一个地方

    const IdRemap remap = dedupe_locations(lib);
    CHECK(lib.locations.size() == 2);
    CHECK(remap.size() == 1);
    CHECK(lib.locations.contains("loc_3"));
}

TEST_CASE("名字空着的一律不碰") {
    AssetLibrary lib;
    lib.locations["loc_1"] = loc("loc_1", "");
    lib.locations["loc_2"] = loc("loc_2", "");
    CHECK(dedupe_locations(lib).empty());
    CHECK(lib.locations.size() == 2);
}

TEST_CASE("角色同名也收，参考图和音色跟着补") {
    AssetLibrary lib;
    Character a;
    a.char_id = "c_chen_yu";
    a.name = "陈屿";
    a.ref_front = "refs/front.png";
    Character b;
    b.char_id = "c_chenyu";
    b.name = "陈屿";
    b.voice_id = "v1";
    b.appearance.face = "方脸";
    lib.characters["c_chen_yu"] = a;
    lib.characters["c_chenyu"] = b;

    const IdRemap remap = dedupe_characters(lib);
    REQUIRE(lib.characters.size() == 1);
    // a 有一张参考图，b 只有描述——参考图更值钱
    REQUIRE(lib.characters.contains("c_chen_yu"));
    CHECK(remap.at("c_chenyu") == "c_chen_yu");
    const Character& kept = lib.characters.at("c_chen_yu");
    CHECK(kept.appearance.face == "方脸");
    REQUIRE(kept.voice_id.has_value());
    CHECK(*kept.voice_id == "v1");
}
