// 补齐没写完的 JSON。
//
// 这一层的错法很安静：补出来的东西解不出来，界面上就是"大纲不流式"，
// 后端一个字都不报——和当年 JsonFieldStreamer 抠错字段名那次一模一样。
// 所以边界情况一条条摆在这儿。

#include <doctest/doctest.h>

#include <string>

#include "stages/json_partial.hpp"

using changji::stages::close_partial_json;
using changji::stages::PartialJson;

namespace {

/// 喂一段，拿一份快照。
nlohmann::json snap(const std::string& s) {
    PartialJson p;
    p.feed(s);
    return p.snapshot();
}

}  // namespace

TEST_CASE("补齐：还开着的括号按相反顺序补上") {
    const auto j = snap(R"({"premise":"一个女人回到老家","chapters":[{"title":"回家")");
    REQUIRE(j.is_object());
    CHECK(j["premise"] == "一个女人回到老家");
    REQUIRE(j["chapters"].is_array());
    CHECK(j["chapters"][0]["title"] == "回家");
}

TEST_CASE("补齐：值写了一半要**留着**，那正是正在长出来的那句话") {
    // 掐掉的话每次只看得见写完的句子，动画就没了。
    const auto j = snap(R"({"logline":"她在葬礼上遇见了)");
    REQUIRE(j.is_object());
    CHECK(j["logline"] == "她在葬礼上遇见了");
}

TEST_CASE("补齐：键写了一半要掐掉，连它前面那个逗号一起") {
    // `{"a":1,"ti` 补成 `{"a":1,"ti"}` 还是解不了——键补齐了还缺值。
    const auto j = snap(R"({"genre":"都市","ti)");
    REQUIRE(j.is_object());
    CHECK(j["genre"] == "都市");
    CHECK_FALSE(j.contains("ti"));
}

TEST_CASE("补齐：结尾是冒号、逗号、半个数字") {
    // 冒号后面还没开始写值：给个 null，键留着（界面上那一项先空着）
    auto j = snap(R"({"tone":"克制","episodes":)");
    REQUIRE(j.is_object());
    CHECK(j["tone"] == "克制");
    CHECK(j["episodes"].is_null());

    // 孤零零的逗号
    j = snap(R"({"a":1,)");
    REQUIRE(j.is_object());
    CHECK(j["a"] == 1);

    // 数字写了一半：`12.` 解不了，掐掉那半个数，**键留着给 null**。
    // ⚠️ 键留着意味着 `j.contains("b")` 是真而 `j["b"]` 是 null——
    // 调用方**不能**拿 `value("b", "")` 去取，nlohmann 在 null 上转字符串
    // 抛的是 type_error。取值一律先问 is_string()，见 story_api 那个 str_of。
    j = snap(R"({"a":1,"b":12.)");
    REQUIRE(j.is_object());
    CHECK(j["a"] == 1);
    CHECK(j["b"].is_null());

    // 裸词写了一半：`tru`。同上
    j = snap(R"({"a":1,"ok":tru)");
    REQUIRE(j.is_object());
    CHECK(j["a"] == 1);
    CHECK(j["ok"].is_null());
}

TEST_CASE("补齐：字符串里的括号和引号不算结构") {
    const auto j = snap(R"({"summary":"他说「{不行}」，然后 \"走了\"")");
    REQUIRE(j.is_object());
    CHECK(j["summary"] == "他说「{不行}」，然后 \"走了\"");
}

TEST_CASE("补齐：结尾是落单的反斜杠") {
    // `"abc\` 后面补个引号是 `"abc\"`——反斜杠把那个引号转义掉了，
    // 字符串还是没关上。所以要先把它掐掉。
    const auto j = snap(R"({"text":"第一行\)");
    REQUIRE(j.is_object());
    CHECK(j["text"] == "第一行");

    // 两个反斜杠是一个真正的反斜杠，不能掐
    const auto k = snap(R"({"text":"第一行\\)");
    REQUIRE(k.is_object());
    CHECK(k["text"] == "第一行\\");
}

TEST_CASE("补齐：markdown 围栏") {
    // 有的模型不管你怎么说都要加一圈 ```json
    const auto j = snap("```json\n{\"genre\":\"悬疑\"");
    REQUIRE(j.is_object());
    CHECK(j["genre"] == "悬疑");
}

TEST_CASE("补齐：解不出来就回 null，别猜") {
    // **必须是真的 null**：parse(..., false) 失败时回的是 discarded，
    // 而 discarded 的 is_null() 是假，调用方那句 `if (is_null()) return;`
    // 会漏掉它，然后在 discarded 上取字段，抛 type_error。
    CHECK(snap("").is_null());
    CHECK(snap("   ").is_null());
    CHECK(snap("不是 JSON").is_null());
    CHECK_FALSE(snap("不是 JSON").is_discarded());
}

TEST_CASE("补齐：一段一段喂进去，中间每一帧都不该炸") {
    const std::string full =
        R"({"premise":"一个女人回到老家","logline":"她在葬礼上遇见了前任",)"
        R"("genre":"都市情感","chapters":[{"title":"回家","summary":"她下了车。",)"
        R"("characters":["林岚","周野"]},{"title":"葬礼","summary":"雨很大。"}]})";
    PartialJson p;
    int parsed = 0;
    for (std::size_t i = 0; i < full.size(); ++i) {
        p.feed(full.substr(i, 1));
        const auto j = p.snapshot();   // 不许抛
        if (j.is_object()) ++parsed;
    }
    // 不要求每一帧都解得出来，但绝大多数帧应该是能的——
    // 解不出来的只有"键写了一半"那种，而那种也不该多。
    CHECK(parsed > static_cast<int>(full.size()) / 2);

    // 最后一帧就是原文本身
    const auto done = p.snapshot();
    REQUIRE(done.is_object());
    CHECK(done["chapters"].size() == 2);
    CHECK(done["chapters"][1]["summary"] == "雨很大。");
    CHECK(done["chapters"][0]["characters"][1] == "周野");
}
