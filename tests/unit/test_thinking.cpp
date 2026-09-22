// 「想多久」那张表（`llm/thinking.hpp`）。
//
// 它答两个问题：**这个模型能挑哪几档**（界面照它变灰）、**挑中的那一档
// 该发什么字段**（拼请求的两处照它填）。两件事必须同一份答案——不然就是
// "界面上能选、发出去没用"。
//
// ⚠️ 这里最要紧的一条是**认不出就一个字段都不发**。发错的代价不是"想少了"，
// 是别家收到不认识的字段直接 400，**那一次生成整个打掉**。

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "llm/thinking.hpp"

using namespace changji;

namespace {

bool has(const llm::ThinkSupport& s, const std::string& k) {
    return std::find(s.tiers.begin(), s.tiers.end(), k) != s.tiers.end();
}

}  // namespace

TEST_CASE("想多久：智谱 5.2 以上四档全认") {
    for (const std::string m : {"glm-5.3", "glm-5.3-flash", "glm-5.2-air", "GLM-5.2"}) {
        // ⚠️ **接成 `std::string` 再 CAPTURE。** `const char*` 印出来是
        // 一串指针（`m := 0x1037d25c9`），红了也不知道是哪个模型。
        CAPTURE(m);
        const auto s = llm::thinking_support("", m);
        CHECK(has(s, "max"));
        CHECK(has(s, "high"));
        CHECK(has(s, "low"));
        CHECK(has(s, "off"));
        // **`ultra` 谁都没有。** 摆在单子上是话（"还能更狠吗"——不能），
        // 不是一档真能挑的东西。
        CHECK_FALSE(has(s, "ultra"));
    }
}

TEST_CASE("想多久：智谱 4.5 到 5.1 只能开或者关") {
    // 文档写着 GLM-4.5 及以上支持 `thinking`（开/关），5.2 及以上才有
    // `reasoning_effort`。**默认那个模型 `glm-4.7-flash` 正落在这一档**
    // ——它一直摆在界面上，而"想多久"对它一个字都发不出去。
    for (const std::string m : {"glm-4.5", "glm-4.6", "glm-4.7-flash"}) {
        CAPTURE(m);
        const auto s = llm::thinking_support("", m);
        CHECK(has(s, "off"));
        CHECK_FALSE(has(s, "max"));
        CHECK_FALSE(has(s, "high"));
        CHECK_FALSE(has(s, "low"));
        CHECK_FALSE(s.why.empty());
    }
}

TEST_CASE("想多久：不认识的一律一档都不给") {
    // 4.5 以下、别家的、乱填的，都走这条。**回一句人话**，界面照抄。
    for (const std::string m : {"glm-4.2", "glm-4", "gpt-5", "qwen-max",
                                "deepseek-reasoner", "", "glm-", "随便写的"}) {
        CAPTURE(m);
        const auto s = llm::thinking_support("", m);
        CHECK(s.tiers.empty());
        CHECK_FALSE(s.why.empty());
    }
}

TEST_CASE("想多久：认不出就一个字段都不发") {
    // ⚠️ 这一条是这张表存在的理由。别家收到不认识的字段直接 400，
    // **那一次生成整个打掉**——比"想得久"严重得多。
    for (const std::string m : {"gpt-5", "qwen-max", "glm-4.2"}) {
        for (const std::string tier : {"max", "high", "low", "off", "ultra"}) {
            CAPTURE(m);
            CAPTURE(tier);
            nlohmann::ordered_json p = nlohmann::ordered_json::object();
            llm::apply_thinking(p, "", m, tier);
            CHECK(p.empty());
        }
    }
    // 认识的模型 + 它不认的那一档，同样一个字都不发。
    nlohmann::ordered_json p = nlohmann::ordered_json::object();
    llm::apply_thinking(p, "", "glm-4.7-flash", "max");
    CHECK(p.empty());
    // 空档（「爱想不想」）：谁都一样，什么都不发。
    llm::apply_thinking(p, "", "glm-5.3", "");
    CHECK(p.empty());
}

TEST_CASE("想多久：认识的那几档填什么字段") {
    {
        nlohmann::ordered_json p = nlohmann::ordered_json::object();
        llm::apply_thinking(p, "", "glm-5.3", "max");
        CHECK(p["thinking"]["type"] == "enabled");
        CHECK(p["reasoning_effort"] == "max");
    }
    {
        // **「不要想」发的是 `disabled`，而且不带 `reasoning_effort`**
        //（既然不想了，再说想多久是自相矛盾的一份请求）。
        nlohmann::ordered_json p = nlohmann::ordered_json::object();
        llm::apply_thinking(p, "", "glm-5.3", "off");
        CHECK(p["thinking"]["type"] == "disabled");
        CHECK(p.find("reasoning_effort") == p.end());
    }
    {
        nlohmann::ordered_json p = nlohmann::ordered_json::object();
        llm::apply_thinking(p, "", "glm-4.7-flash", "off");
        CHECK(p["thinking"]["type"] == "disabled");
    }
}
