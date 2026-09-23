#pragma once

// 给模型的工具表 + 引擎这头真跑的实现。
//
// **按「人要什么」切，不按接口路径切。** 引擎有 126 个路由，原样摆给模型是
// 最省事也最坏的做法——它得自己搞清楚 `/api/story/adopt` 和
// `/api/story/episodes` 的先后。一个工具内部可以连着调三个处理函数。
//
// 这一版只有「看」那几个，外加建项目（空手起头那条路要它）。派活的（写故事、
// 出分镜、出片）下一期接。
//
// ---
//
// **回给模型的是人话，不是 JSON 原样。** 一章分镜 dump 出来上万字符，而模型
// 要的只是「17 镜，12 镜出了片，2 镜降级」。工具的返回值是提示词的一部分，
// 和 schema 一样占预算（CLAUDE.md 第三条：量比例再动手）。
//
// **名字只用下划线，不用点。** OpenAI 那套 function name 的合法字符是
// `[a-zA-Z0-9_-]`，各家网关对点的处理不一致——有的 400，有的静悄悄改名，
// 而改了名之后我们按名字分派就再也认不出来。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/outcome.hpp"
#include "config/settings.hpp"
#include "http/run.hpp"
#include "llm/client.hpp"
#include "util/say.hpp"

namespace changji::agent {

/// 工具跑起来要知道的那点东西。
struct ToolContext {
    /// 这条对话认的项目目录。**可能是空的**——第一句话之前还没有项目。
    std::string project;
    config::Settings settings;

    /// `create_project` 建出来之后把新目录回填给外面。
    ///
    /// **一条对话认一个项目**，建完之后这条对话后面所有的工具都落在它上面，
    /// 所以这件事不能只改 ctx 里那一份：外头还要拿它决定对话往哪儿落库。
    std::function<void(const std::string& root)> on_project_created;

    /// 派活那几个要的大模型客户端。**空 = 这台机器上派不了活**（用例里就是
    /// 空的）——那时候那几个工具照实说一句，不假装派出去了。
    std::shared_ptr<llm::Client> client;

    /// 这一轮里**真派出去了**哪几件活（工具名）。
    ///
    /// 派活的工具成功那一下往这儿记一笔。调用方靠它决定要不要守着——
    /// 守到活干完，再往对话里追一条「做完了」。**没有这一笔的话，代理说的
    /// 那句"做完了我再说"就永远不会兑现**，而它每次都会那么说。
    std::vector<std::string> dispatched;

    /// 人这会儿把话说在哪一章上（`ep01`）。**空 = 他没指**。
    ///
    /// 界面上输入框顶上那一行写着它（上下文条），所以人是**看着**这一章说
    /// 的话——「改一下」「这一章再紧一点」指的就是它。不把这件事告诉模型的
    /// 话，同一句话在两头意思不一样：人以为在说第 3 章，模型按最后动过的那
    /// 一章办，而**两边都不会觉得自己错了**。
    ///
    /// 只管"没指明时按哪一章算"，**不是锁**：他说「第 1 章那段删掉」照样
    /// 是第 1 章。
    std::string here;

    /// 出片那条要的后端。
    ///
    /// **做成回调不是直接调 `default_run_deps()`**：造它的那个文件
    /// （`http/run_deps.cpp`）链着 httplib 和体检，进不了测试目标。
    std::function<http::RunDeps()> run_deps;

    /// 这一次调用**带回来的东西**（图、片、改动前后的字），形状见
    /// `agent/outcome.hpp` 的 `media_item`。
    ///
    /// 工具往这儿放，`loop.cpp` 跑完一个工具就整份收走、挂到那条 tool 的
    /// `Turn::media` 上——**不进工具的回话**：回话是给模型读的，一张图的
    /// 路径对它没用，而对人来说那张图本身才是"刚才做了什么"。
    nlohmann::json media = nlohmann::json::array();

    /// 这一轮派出去第一件活**之前**那一刻的盘面。没派过活就是空的。
    ///
    /// 派活的几个工具在真派之前拍一张（只拍第一次：一轮里连派两件，比的
    /// 该是"这一轮之前"）。活干完之后 `http/chat_api.cpp` 拿它比出这一回
    /// 做出了什么，挂到「跑完了」那一条上。
    std::shared_ptr<Baseline> baseline;

    /// 刚跑完的那个工具**没做成**（接口回了错、跑出异常）。`loop.cpp` 读完
    /// 就清掉，挂到那条 tool 的 `Turn::level` 上。
    ///
    /// 只管"真砸了"那一档。「要说是哪一章」「没说要改什么」这种是在跟模型
    /// 要参数，它补上就过了，不算砸——全标红的话一轮对话里一半是红的，
    /// 红就不再有意思。
    bool failed = false;
};

/// 给模型的工具表（OpenAI 那套 function 格式）。
nlohmann::ordered_json tool_specs();

/// 派出去的那件活，说给人听的名字（「写正文」「拆镜头」…）。
///
/// **四个派活的工具四句话，不是一句通用的。** 共用一句「派出去了」的时候人
/// 看不出派的是什么，模型于是拿正文替它补一句，对话里同一件事连说两遍。
/// 认不出的工具回它自己的名字——**原样至少是实话**。
std::string dispatch_label(const std::string& tool);

/// 跑一个工具，回一段文字。
///
/// **名字不认识、参数不对、跑出异常，都回一句话，不抛。** 抛出去的话整轮
/// 对话就断了，而模型完全有能力换个工具再试一次——这一条和
/// `stages::run_web_tool` 是同一个规矩。
///
/// ⚠️ `to` 是**这一遍说给谁听**（见 `util/say.hpp` 的 `i18n::Audience`）。
/// 默认给模型，也就是中文原话。`/api/peek`（「就地看一眼」那五格）拿这几个
/// `*_read` 的回话**原样摆到界面上**，那一头传 `Audience::human()`。
///
/// **只有那几个 `*_read` 认这个参数**，别的工具回的话只有模型读得到。
std::string run_tool(ToolContext& ctx, const std::string& name,
                     const std::string& arguments_json,
                     i18n::Audience to = i18n::Audience::model());

// ---- 下面这几个是可测的纯拼装，用例直接拿它们比 ----

/// 模型要人拿主意时，工具回的那句话前面挂这个记号。
///
/// **为什么要一个记号**：`ask_user` 和别的工具一样是"调用 → 回一段文字"，
/// 而调用方要能分出"这一轮该停下来等人"和"接着往下做"。不挂记号的话，
/// 模型收到工具结果会接着自己往下猜——而它本来就是因为猜不准才问的。
inline constexpr const char* kAskUserMark = "\x01ASK\x01";

/// 接口报错的那一段说成人话：422 那种 `[{loc, msg, …}]` 说成「哪一栏：出了什么事」，
/// 字符串照原样。**给人看也给模型看**——原来原样 dump 出去，两边读到的都是一段 JSON。
std::string readable_detail(const nlohmann::json& body);

/// 「这部片子现在什么样」：几章、每章到哪一步、有没有在跑的活。
///
/// 这一段**每轮都会重算一份塞进提示词**，所以它要短、要全、要说人话。
std::string describe_project(const nlohmann::json& project_json,
                             const nlohmann::json& story_json);

}  // namespace changji::agent
