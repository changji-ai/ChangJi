#include "llm/thinking.hpp"

#include "util/say.hpp"

#include <algorithm>
#include <mutex>
#include <set>

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

// 「关不掉思考」的备忘，见 learn_thinking_always_on。键是地址 + 模型：
// 同一个模型换一条路就能关（coding/paas/v4 能、paas/v4 不能）。
std::mutex& always_on_mu() {
    static std::mutex m;
    return m;
}
std::set<std::string>& always_on() {
    static std::set<std::string> s;
    return s;
}
std::string always_on_key(const std::string& base_url, const std::string& model) {
    std::string u = base_url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u + '\n' + lower(model);
}
bool always_thinks(const std::string& base_url, const std::string& model) {
    std::lock_guard<std::mutex> lk(always_on_mu());
    return always_on().count(always_on_key(base_url, model)) > 0;
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
                    const std::string& model, const std::string& asked) {
    if (asked.empty()) return;
    const auto sup = thinking_support(base_url, model);
    const auto has = [&](const std::string& t) {
        return std::find(sup.tiers.begin(), sup.tiers.end(), t) != sup.tiers.end();
    };
    if (!has(asked)) return;   // 这一家不认这一档：一个字段都不写

    // 这个地址上的这个模型关不掉（被拒过一回）：退到最少想的那一档。
    std::string tier = asked;
    if (tier == "off" && always_thinks(base_url, model)) {
        if (!has("low")) return;
        tier = "low";
    }
    if (tier == "off") {
        payload["thinking"] = nlohmann::ordered_json{{"type", "disabled"}};
        return;
    }
    payload["thinking"] = nlohmann::ordered_json{{"type", "enabled"}};
    payload["reasoning_effort"] = tier;
}

bool learn_thinking_always_on(const nlohmann::ordered_json& sent, int status,
                              const std::string& body, const std::string& base_url,
                              const std::string& model) {
    if (status != 400) return false;
    const auto t = sent.find("thinking");
    if (t == sent.end() || !t->is_object() || t->value("type", "") != "disabled") {
        return false;
    }
    // 这一趟里跟思考沾边的只有 `thinking: disabled` 那一个字段，所以 400
    // 里提到思考，说的就是它。**不认错误码**：智谱的 1210 是笼统的"参数
    // 有误"，别的参数错了也回它。
    //
    // ⚠️ 「思考」是拿去比服务商原话的，不是给人看的，翻了就对不上。
    const std::string b = lower(body);
    if (b.find(SAY_NEVER("思考")) == std::string::npos &&
        b.find("thinking") == std::string::npos) {
        return false;
    }
    std::lock_guard<std::mutex> lk(always_on_mu());
    always_on().insert(always_on_key(base_url, model));
    return true;
}

}  // namespace changji::llm
