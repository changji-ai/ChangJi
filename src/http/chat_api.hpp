#pragma once

// 对话那几条接口。**全是新路由**——网页那 126 个接口一个都没动。
//
//     POST /api/chat          发一句话。立刻 202，不在这条请求里等
//     GET  /api/chat/history  这条对话
//     POST /api/chat/stop     停这一轮
//     （推送走 WebSocket 的 "chat" 频道）
//
// ---
//
// **为什么 POST 立刻回，不等。**
//
// 一轮里模型可能连着调几次工具，每次都要等它想完；再往后接上派活那几个工具，
// 一句话能牵出四十分钟的活。同步等的后果这个仓库量过：Crow 的一条线程管着
// 一批连接，占住它，落在它上面的请求全都干等——2026-09-11 实测写一章的五十几
// 秒里 `/api/run`、`/api/system` 卡二十多秒，于是"一写字界面就卡住"
//（见 `http/server.hpp` 里 concurrency 那段）。
//
// **一个项目同时只跑一轮。** 第二句话进来回 409，不排队：两轮并着跑会往同
// 一个 chat.jsonl 里交叉写，而且各自看到的状态摘要都是半截的。

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "http/run.hpp"   // RunDeps
#include "llm/client.hpp"

namespace changji::http {

/// POST /api/chat —— body: `{project?, text}`
///
/// **`project` 可以是空的**：第一句话之前还没有项目，代理自己会去建。那之前
/// 这条对话落在用户数据目录，建出来之后整条搬进项目目录。
///
/// **`chat` 也可以是空的**：那就是一直以来那一条。一部片子可以开好几条
///（一条盯着分镜、一条在改设定），各落各的文件，见 `agent/transcript.hpp`。
///
/// 回 `{started, project}`。正在跑就回 409。
///
/// `run_deps` 是出片那条要的后端工厂。**由调用方给，这个文件不自己去够**：
/// 造它的 `http/run_deps.cpp` 链着 httplib 和体检，进不了测试目标——
/// 在这儿直接调的话，整个测试二进制就链不起来了（2026-09-21 实撞）。
/// 装配写在 `server.cpp` 那一处，本来也是它该管的事。
ApiResult post_chat(const nlohmann::json& body, std::shared_ptr<llm::Client> client,
                    std::function<RunDeps()> run_deps);

/// GET /api/chat/history?project=…&chat=… —— 回 `{turns: [...]}`
///
/// `project` 空着就回"还没有项目"那条对话；`chat` 空着就是一直以来那一条
///（`<项目目录>/chat.jsonl`）。`chat` 不合规回 400（它会变成文件名）。
ApiResult get_chat_history(const std::string& project, const std::string& chat = {});

/// GET /api/chats?path=… —— 这部片子有哪几条对话。
///
/// 回 `{chats:[{id, title, at, turns}]}`，一直以来那一条（空 id）排最前。
/// **名字取第一句人说的话**——那句话就是这条对话的来由，让人另取一个名字
/// 是给他加一件事。
ApiResult list_chats(const std::string& project);

/// DELETE /api/chats?path=…&chat=… —— 删掉一条对话。
///
/// **真删文件。** 界面那头先问过人了（那一行上的「删掉？」），这儿不再问。
/// 回 `{ok, gone}`：`gone` 是"本来在不在"——本来就不在也回 200，两个人同时
/// 点了删，第二下不该报错。
///
/// ⚠️ **正在跑的那一部不许删**（回 409）：那一轮还在往这个文件里写，删了
/// 之后它接着写，等于又建了一个半截的。
ApiResult delete_chat(const std::string& project, const std::string& chat);

/// POST /api/chat/stop —— body: `{project?}`。没在跑就什么都不做，照样 200。
ApiResult post_chat_stop(const nlohmann::json& body);

/// POST /api/chat/fork —— 从某一条回话上**分出一条新对话**。
///
/// body: `{project, chat?, to, upto}`，回 `{chat, turns}`。
///
/// 从这儿往前的全抄进新的一条，往后的一句都不带——人接着说的那句话就落在
/// 那个岔口上。**老的那条一个字不动**：分叉是"再试一条路"，不是"把说过的
/// 话删掉"，而删掉是不可逆的。
///
/// **`upto` 是毫秒时间戳，不是第几条。**
///
/// 界面上看得见的行和盘上的条数**对不上**：工具回的那几条里有一族根本不摆
/// （`worth_showing`），而摆出来的那几行还会合并。让界面去数第几条，等于让
/// 它把那个筛子再实现一遍——而两处一旦不一致，分出来的对话会从别的地方切
/// 开，人还看不出是哪儿错了。时间戳是这两头都有、且都一样的那个数。
///
/// 一毫秒里有好几条（一轮里工具和回话挨着落）就**全留下**：它们本来就是
/// 同一口气说的，切在中间的话新对话开头是半截的。
///
/// ⚠️ `upto` 必须大于 0。没有时间戳的老对话分不出来——照实回 400，
/// 别猜一个位置切下去。
ApiResult post_chat_fork(const nlohmann::json& body);

}  // namespace changji::http
