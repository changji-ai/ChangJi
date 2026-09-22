// 参考图的提示词。
//
// 钉的是两条不能松的规矩：外观块必须逐字用 render_prompt 那一份
// （参考图和成片里必须是同一个人），以及空景图里不许有人。

#include <doctest/doctest.h>

#include <string>

#include "models/character.hpp"
#include "stages/ref_images.hpp"

using namespace changji;
using namespace changji::models;
using changji::stages::build_character_ref_prompt;
using changji::stages::build_location_ref_prompt;
using changji::stages::ref_negative;
using changji::stages::ref_poses;

namespace {

Character a_character() {
    Character c;
    c.char_id = "c_lin_wan";
    c.name = "林晚";
    c.appearance.identity = "二十七八岁女性，克制";
    c.appearance.body = "偏瘦，肩背挺";
    c.appearance.face = "齐肩黑直发，圆眼，单眼皮";
    c.appearance.attire = "便利店藏青制服外套";
    return c;
}

Location a_location() {
    Location l;
    l.location_id = "loc_store";
    l.name = "便利店";
    l.space = "临街玻璃门，两排货架";
    l.lighting = "夜间冷白顶光";
    l.palette = "冷青加一点暖黄";
    return l;
}

StyleProfile a_style() {
    StyleProfile s;
    s.style_line = StyleLine::REALISTIC;
    s.global_style = "夜戏，低饱和，轻微颗粒";
    return s;
}

}  // namespace

TEST_CASE("三个朝向，键要和 Character 上那三个字段对得上") {
    const auto& poses = ref_poses();
    REQUIRE(poses.size() == 3);
    CHECK(poses[0].key == "front");
    CHECK(poses[1].key == "three_quarter");
    CHECK(poses[2].key == "back");
    // Character::ref_for_pose 认的就是这几个键，对不上的话生成出来的图
    // 挂不到任何一个槽上
    Character c = a_character();
    c.ref_front = "refs/a.png";
    CHECK(c.ref_for_pose("front").has_value());
}

TEST_CASE("角色参考图：外观块逐字用 render_prompt 那一份") {
    const Character c = a_character();
    const StyleProfile s = a_style();
    const std::string body = c.render_prompt(s.style_line);

    for (const auto& p : ref_poses()) {
        const std::string prompt = build_character_ref_prompt(c, s, p.key);
        CAPTURE(p.key);
        // **这是这组用例存在的理由。** 参考图和后面每一镜必须是同一个人，
        // 而那靠的就是这段字逐字节相同——这里自己再写一套形容词的话，
        // 参考图上那个人和成片里那个人就是两个人。
        CHECK(prompt.find(body) != std::string::npos);
        // 朝向进去了
        CHECK(prompt.find(p.phrase) != std::string::npos);
        // 背景写死纯色：背景里有内容的话，后面每一镜都会把它当成这个人
        // 身上带的
        CHECK(prompt.find("纯灰色背景") != std::string::npos);
        // 全片调子也带上
        CHECK(prompt.find("夜戏，低饱和，轻微颗粒") != std::string::npos);
    }
}

TEST_CASE("认不出的朝向按正面出，别抛也别出一张没朝向的") {
    const Character c = a_character();
    const StyleProfile s = a_style();
    const std::string got = build_character_ref_prompt(c, s, "侧躺");
    CHECK(got == build_character_ref_prompt(c, s, "front"));
}

TEST_CASE("空景图里不许有人") {
    const std::string p = build_location_ref_prompt(a_location(), a_style());
    // 空景图是场景一致性的锚点，里面站个人的话，那个人会被当成这个地方的
    // 一部分，一路复制到每一镜里去——而那是个不属于任何角色、也没法改的人
    CHECK(p.find("画面里没有任何人物") != std::string::npos);
    // **但屋里的东西要在。** 「没有人」和「这地方是空的」是两回事，而出图
    // 模型很容易听成后者：2026-09-12 实见便利店货架一件货都没有，像一家
    // 清仓完的店——而它是这部电影一半戏发生的地方。
    CHECK(p.find("陈设") != std::string::npos);
    // ⚠️ **不许再写「空镜」。** 那是电影行话（这一镜没有人），而模型看见的
    // 是「空」。它正是上面那张空货架的来源。
    CHECK(p.find("空镜") == std::string::npos);
    // 外观块照样逐字用
    CHECK(p.find(a_location().render_prompt(StyleLine::REALISTIC)) !=
          std::string::npos);
}

TEST_CASE("拼出来的串里不能有空段留下的重复标点") {
    // 外观块里几段是空的很常见（手改过、或者模型漏了）。不跳空段的话会拼出
    // 「，，」，而这个串会原样进出图提示词，分词器把它当内容。
    Character bare;
    bare.char_id = "c_x";
    bare.name = "无名";
    StyleProfile plain;
    plain.style_line = StyleLine::REALISTIC;

    const std::string p = build_character_ref_prompt(bare, plain, "front");
    CHECK(p.find("，，") == std::string::npos);
    CHECK(p.find("。，") == std::string::npos);

    const std::string q = build_location_ref_prompt(Location{}, plain);
    CHECK(q.find("，，") == std::string::npos);
}

TEST_CASE("负向：比出片那份多压多人和文字") {
    const std::string n = ref_negative(a_style());
    // 三视图里多出一个人，后面分镜就分不清哪个是主角
    CHECK(n.find("多个人物") != std::string::npos);
    // 参考图上的字会被当成服装花纹复制下去
    CHECK(n.find("文字") != std::string::npos);
    CHECK(n.find("水印") != std::string::npos);

    // 项目自己配了负向就用它的，不要把默认那份也堆上去
    StyleProfile custom = a_style();
    custom.negative_prompt = "只压这一条";
    const std::string c = ref_negative(custom);
    CHECK(c.rfind("只压这一条", 0) == 0);
    CHECK(c.find("多个人物") != std::string::npos);
}
