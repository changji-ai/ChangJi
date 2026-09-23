#pragma once

// 一轮对话：拼上下文 → 调模型 → 跑工具 → 再来，直到它开口说话。
//
// 形状和 `stages/story_from_web.cpp` 那条一样（那是这个仓库里第一条带工具的
// 对话，十轮上网看热点写一章）。差别只在工具表：那条只有三个上网的工具，
// 这条是整部片子。
//
// ---
//
// **状态每轮重算，不从历史里读。**
//
// 模型很容易把「我刚才让它写了第 3 章」记成「第 3 章已经写好了」——而那两件
// 事之间隔着四十分钟和一道闸门。所以每一轮都现问一遍盘上是什么样
// （`current_state`），历史只负责"人说过什么、做过什么"。
//
// **循环跑在引擎里，不在界面里。** 这个仓库为这件事付过两次学费：出图队列
// 活在标签页的闭包里（CLAUDE.md 第九条），一键成片的链条活在浏览器里
//（`http/oneclick.hpp` 开头）。两次的结论都是收进引擎。

#include <functional>
#include <string>
#include <vector>

#include "agent/tools.hpp"
#include "agent/transcript.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"

namespace changji::agent {

struct LoopHooks {
    /// 又落下一条（人说的、场记说的、工具回的）。外面拿它落库 + 推给界面。
    std::function<void(const Turn&)> on_turn;
    /// 场记那句话在流式长。给的是**增量**。
    std::function<void(const std::string& piece)> on_delta;
    /// 模型"先想再说"的那一段。
    std::function<void(const std::string& piece)> on_thinking;
    /// 正在调哪个工具。界面上那一行「在看这部片子现在什么样…」。
    /// 要调一个工具了。`what` 是说给人听的那句（`tool_label`）；
    /// `name` / `args` 原样带上——**界面要知道这会儿在动哪一格的哪一章**，
    /// 好在那块面板上说一声「正在动，先别改」（方案第四节「别打扰」最后
    /// 一条）。只给 `what` 的话那头只能去认中文，而认中文就是改一个字
    /// 就散架（CLAUDE.md 第八条）。
    std::function<void(const std::string& what, const std::string& name,
                       const std::string& args)>
        on_tool;
    /// **这一下让不让做。** 跑一个工具之前问一声：回空串就是放行；回一句话就
    /// 是不让——那句话当成这个工具的回话交给模型（它据此改主意或者去问人），
    /// 工具本身不跑。
    ///
    /// 不挂就是全放行（用例、老路径）。权限那一档的判法在 `http/chat_api.cpp`。
    std::function<std::string(const std::string& name, const std::string& args)>
        on_permit;
};

/// 要调这个工具时，界面上（和任务账本上）那一行说什么。**给人看的，翻。**
/// 每个工具都得有一句，见实现上那段。
std::string tool_label(const std::string& name, const std::string& args);

/// 「每步问我」那一问里说的：要做什么、对哪一件（「改 ep01_sh004：运镜、台词」）。
/// `tool_label` 是进度的说法（「在改一镜」），摆进「场记要做：」读着别扭，也不说
/// 是哪一件。**给人看的，翻。**
std::string tool_ask(const std::string& name, const std::string& args);

/// 「这部片子现在什么样」，几百字。**每轮重算**。
std::string current_state(const ToolContext& ctx);

/// 开场那几条消息：系统提示 + 当前状态 + 人在说哪一章 + 历史。
///
/// **纯函数**（状态那段由调用方给），所以用例能钉住"发出去的到底长什么样"。
///
/// `here` 是人这会儿把话说在哪一章上（`ep01`），空就是没指，那一段整段不发
/// ——发一句「他没指哪一章」是白占提示词，模型本来就该按上下文办。
std::vector<llm::Message> build_messages(const std::string& state,
                                         const std::vector<Turn>& history,
                                         const std::string& here = {});

/// 跑一轮。模型可能连着调几次工具，最后说一句话；回的就是那句话。
///
/// `history` 是到此为止的整条对话（**包括人刚说的那一句**）。每调一次工具，
/// 这个函数会把 assistant 那条和 tool 那条通过 `hooks.on_turn` 交出去——
/// 落库是外面的事，这儿不碰文件。
///
/// 转了 `max_rounds` 轮还在调工具就停下来，回一句照实说的话。**不抛**：
/// 一条对话卡住了该在界面上看得见，而不是变成一个红框。
std::string run_turn(llm::Client& client, ToolContext& ctx,
                     const std::vector<Turn>& history, pipeline::CancelToken& tok,
                     const LoopHooks& hooks, int max_rounds = 8);

}  // namespace changji::agent
