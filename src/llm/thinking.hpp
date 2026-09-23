#pragma once

// **「想多久」这件事，每一家的参数名和取值都不一样，而且问不出来。**
//
// 2026-09-21 用户问的：「每个厂商这个参数值和参数名都不一样如何准确获取呢」。
// 答案是：**获取不到**。
//
//   · `/models` 那条接口只回一串 id，没有任何一家在里面说"我支持想多久"；
//   · 没有哪一份 OpenAI 兼容约定规定过这个字段；
//   · 探一次（发过去看回不回 400）要花一次真调用，而且 400 会把那一次生成
//     整个打掉——这正是我们最怕的那种失败（认错的代价是别家收到不认识的
//     字段直接 400，所有生成一起挂）。
//
// 所以只能有**一张表**。这个文件就是那张表，而且**只有这一份**：
//
//   · 拼请求的那两处（`build_payload` 和带工具那条）从这儿拿字段；
//   · 界面上那颗「想多久」从这儿拿"哪几档能选"（走 /api/llm/thinking）。
//
// ⚠️ **不许在别处照着模型名再判一遍**（CLAUDE.md 第八条）。2026-09-21
// 之前 `client.cpp` 里就有两份一模一样的 `glm-5` 判断，而界面那头第三份
// 是写死的"都不能选"——三份各自过期的样子是"界面上能选、发出去没用"。
//
// ⚠️ **认不出来就一个字段都不发**，和这张表不存在时完全一样。宁可少想，
// 不可发错。

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::llm {

/// 我们这套档位的键。**界面上那几个词另说**，那是界面的事。
///
/// `""`（不发这个字段）不在这儿：那一档谁都支持，因为它什么都不做。
inline constexpr const char* kThinkTiers[] = {"ultra", "max", "high", "low", "off"};

/// 这一家的这个模型，「想多久」能挑哪几档。
struct ThinkSupport {
    /// 能挑的那几档（键取自 `kThinkTiers`）。**空的就是一档都不认。**
    std::vector<std::string> tiers;
    /// 一档都不认时那句话。给界面照抄。
    std::string why;
};

/// 查表。`base_url` 现在没用上（认的是模型名，不是地址——同一个网关后面
/// 可以挂别家的模型），留着是因为哪天有按地址认的一家时不用改调用方。
ThinkSupport thinking_support(const std::string& base_url, const std::string& model);

/// 把挑中的那一档翻成这一家的字段，写进 `payload`。
///
/// `tier` 是 `kThinkTiers` 里那几个键之一，或者空串（什么都不做）。
/// **表里不认的组合一个字段都不写。**
///
/// 收的是 `ordered_json`：请求体那头用的就是它（字段顺序照着写下去的顺序，
/// 提示词日志里读起来才和真发出去的一样）。
void apply_thinking(nlohmann::ordered_json& payload, const std::string& base_url,
                    const std::string& model, const std::string& tier);

/// **「关掉思考」被服务拒了：记下来，回 true 让调用方再发一趟。**
///
/// 同一个模型能不能关，**跟着地址变，不跟着模型名走**——所以这件事上不了
/// 上面那张表。2026-09-23 拿同一把密钥实测：glm-5.3 / 5.3-flash 在
/// `…/api/coding/paas/v4` 上收 `thinking: disabled`，换到 `…/api/paas/v4`
/// 就回 400「该模型始终思考，不支持关闭思考；请使用 low、high 或 max。」
/// ——而用户配的 `reasoning_effort = "off"` 让读故事那一步整个挂掉。
///
/// 判据：发出去的 `sent` 里是 `thinking: disabled`，回的是 400，回包里说
/// 的是思考这件事。认出来就把「这个地址上的这个模型」记进进程里那份备忘，
/// 之后 `apply_thinking` 碰到它的 `off` 一律换成 `low`（这个模型没有
/// `low` 那一档就什么都不发，由它按自己的默认想）。
///
/// **再发一趟不会重复计费**：400 是请求没被接下，一个字都还没生成。
/// 备忘是进程内的——换一个地址、重启一次都会重新认一回，代价是一次 400。
bool learn_thinking_always_on(const nlohmann::ordered_json& sent, int status,
                              const std::string& body, const std::string& base_url,
                              const std::string& model);

}  // namespace changji::llm
