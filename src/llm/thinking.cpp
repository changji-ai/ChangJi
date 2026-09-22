#include "llm/thinking.hpp"

#include "util/say.hpp"

#include <algorithm>

namespace changji::llm {

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

/// `glm-4.7-flash` → 4、7。认不出来回 {-1,-1}。
///
/// **按版本号比，不按前缀穷举**：智谱那条线是"4.5 及以上支持 thinking、
/// 5.2 及以上支持 reasoning_effort"，写成一串 `rfind("glm-4.5")` 的话
/// 下一个小版本出来就漏（同 CLAUDE.md 第六条：引原话别引条号）。
std::pair<int, int> glm_version(const std::string& m) {
    if (m.rfind("glm-", 0) != 0) return {-1, -1};
    std::size_t i = 4;
    int major = 0;
    bool any = false;
    for (; i < m.size() && m[i] >= '0' && m[i] <= '9'; ++i) {
        major = major * 10 + (m[i] - '0');
        any = true;
    }
    if (!any) return {-1, -1};
    if (i >= m.size() || m[i] != '.') return {major, 0};
    ++i;
    int minor = 0;
    for (; i < m.size() && m[i] >= '0' && m[i] <= '9'; ++i) {
        minor = minor * 10 + (m[i] - '0');
    }
    return {major, minor};
}

bool at_least(std::pair<int, int> v, int major, int minor) {
    if (v.first < 0) return false;
    return v.first > major || (v.first == major && v.second >= minor);
}

}  // namespace

ThinkSupport thinking_support(const std::string& /*base_url*/,
                              const std::string& model) {
    const std::string m = lower(model);
    const auto v = glm_version(m);

    // ---- 智谱 GLM ----
    //
    // 文档写着：**GLM-4.5 及以上支持 `thinking`**（开/关），
    // **GLM-5.2 及以上支持 `reasoning_effort`**，而 5.3 / 5.3-Flash
    // 只收 low / high / max。
    //
    // 所以 4.5～5.1 只有"关"这一档能挑（开是默认），5.2 以上多三档。
    if (at_least(v, 5, 2)) return {{"max", "high", "low", "off"}, ""};
    if (at_least(v, 4, 5)) {
        return {{"off"}, SAY("这个模型只能开或者关，挑不了想多久")};
    }

    // ---- 别家 ----
    //
    // **一个字段都不发。** 各家的名字和取值都不一样（有的是
    // `enable_thinking` 加 `thinking_budget`，有的是另起一个 reasoner 模型，
    // 有的压根没有），而认错的代价是 400 把整次生成打掉。
    // 哪天摸准了某一家，在这儿加一行，界面和请求两头一起跟着变。
    return {{}, SAY("这个模型不认「想多久」，发过去也没用")};
}

void apply_thinking(nlohmann::ordered_json& payload, const std::string& base_url,
                    const std::string& model, const std::string& tier) {
    if (tier.empty()) return;
    const auto sup = thinking_support(base_url, model);
    if (std::find(sup.tiers.begin(), sup.tiers.end(), tier) == sup.tiers.end()) {
        return;   // 这一家不认这一档：一个字段都不写
    }
    if (tier == "off") {
        payload["thinking"] = nlohmann::ordered_json{{"type", "disabled"}};
        return;
    }
    payload["thinking"] = nlohmann::ordered_json{{"type", "enabled"}};
    payload["reasoning_effort"] = tier;
}

}  // namespace changji::llm
