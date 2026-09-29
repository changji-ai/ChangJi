#pragma once

// **这一条对话用谁家的哪个模型。**
//
// 用户 2026-09-26：「大模型选择应该会话隔离」「会话里的模型名和模型地址持久化
// 保存」。原来输入框底下那一条（厂商 / 模型 / 想多久）改的是**整台机器**的
// `[llm]`：这条对话换个想得久的写正文，另一条对话正在拆分镜，跟着就换了——
// 而它根本没碰那一条。
//
// 做法三件：
//
//   1. **挑法落在对话旁边**（`<片子>/chats/<编号>.model.json`，一直以来那一条是
//      `<片子>/chat.model.json`）：跟着对话走，片子拷走一起拷走；对话搬家、分叉、
//      删掉，这一份跟着搬、跟着抄、跟着删（`agent/chat_api.cpp`）。**不存密钥**
//      ——密钥照旧按地址各存一把（`config::read_api_key_for`）。
//   2. **挂在线程上**（`PickScope`，同 `util::WriterScope`）：对话那一轮开工时读盘
//      立一个；它派出去的活（任务表、`Offload`）在派活那一刻抄走、到自己线程上再
//      立一个。底下叫模型的地方一律问 `effective_llm()`——一路把"哪个模型"当参数
//      传下去要改几十个函数。
//   3. **没挑就跟全局**：新开的对话、页面上的按钮、没走对话的活，一个字都不变。
//
// 只管两条后端：`remote`（打 API）和 `local`（进程内那份 gguf）。命令行那条
// 是整台机器的事（装了哪个命令行、订阅是谁的），不按对话分。

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"

namespace changji::llm {

struct ChatPick {
    /// `remote` / `local`。**空 = 没挑后端**（只挑了想多久，或者什么都没挑）。
    std::string backend;
    /// 打 API 那一家的地址（`remote`）。
    std::string base_url;
    /// `remote`：发出去的模型名。`local`：界面上叫它什么（文件名去掉 `.gguf`）。
    std::string model;
    /// `local`：权重文件，**相对模型目录**（`llm/Qwen3-8B-Q4_K_M.gguf`）。
    /// 存相对的：模型目录换了、挪了，按名字还找得到（`config::find_model`）。
    std::string file;
    /// 想多久。**没值 = 跟全局**；空串 = 不发这个字段（「爱想不想」）。
    std::optional<std::string> effort;

    bool empty() const { return backend.empty() && !effort.has_value(); }
};

nlohmann::json to_json(const ChatPick& p);
/// 认不出的字段、不认识的后端一律当没挑——**手改坏了的一份不该让对话开不了口**。
ChatPick pick_from_json(const nlohmann::json& j);

/// 把挑法叠到一份配置上，回**这一条对话实际会用的**那份 `[llm]`。
///
/// - `remote`：地址、模型换成挑的；密钥**换成那个地址自己名下那把**（带着上一家
///   的钥匙去敲下一家，回来是 401，而 401 长得像"这个模型不让你用"）；
///   按任务分流的那张表（`task_models`）清掉——人在这条对话里挑了一个模型，意思
///   是这条对话里写什么都用它。模型名空着（刚换了家还没挑）就留全局那个名字。
/// - `local`：后端换成进程内，`local_weights` 指到那份文件（找不到就留空，
///   退回 `[models].llm`，借槽时照实报错）。
/// - `effort` 有值：`reasoning_effort` 换成它、`task_efforts` 清掉，理由同上。
config::LLMConfig with_pick(const config::Settings& s, const ChatPick& p);

/// 整台机器那份默认写成一份挑法（一条对话说第一句话时把它记成自己的，见
/// `agent/chat_api.cpp`）。走命令行的回空的——那条不按对话分。
ChatPick pick_of(const config::Settings& s);

/// 此刻这条线程上的挑法。没立过 `PickScope` 就是没有。
std::optional<ChatPick> current_pick();

/// 在这个作用域里，这条线程上的挑法是 `p`。可以嵌套，出了作用域恢复外面那个。
/// 传 `nullopt` = 明确地"没挑"（跟全局）。
class PickScope {
public:
    explicit PickScope(std::optional<ChatPick> p);
    ~PickScope();
    PickScope(const PickScope&) = delete;
    PickScope& operator=(const PickScope&) = delete;
};

/// 此刻这条线程上该用的 `[llm]`：全局那份叠上这条线程上的挑法。
/// **生产里叫模型的那一个客户端就问它**（`llm::make_client`）。
config::LLMConfig effective_llm();
config::LLMConfig effective_llm(const config::Settings& s);

// ---- 落盘 ----

/// 一条对话落在哪个目录：片子本身；还没有片子的那几条落在
/// `<用户数据目录>/chat`。**对话记录和这一份共用这一个判据**
/// （`agent/chat_api.cpp` 的 `dir_for` 就调它）。
std::filesystem::path chat_root(const std::string& project);

/// 那一份挑法的文件。
std::filesystem::path chat_pick_path(const std::filesystem::path& root,
                                     const std::string& chat);

/// 读。没有、读不动、是一条链接都回 `nullopt`。
std::optional<ChatPick> load_chat_pick(const std::filesystem::path& root,
                                       const std::string& chat);

/// 写。`p.empty()` 就删掉那份文件。先写旁边一份再换过去；那个位置（或 `chats/`
/// 本身）是链接的话不写——**片子是可以拷来的**，同 `agent/transcript.cpp`。
/// 写不成抛 `std::runtime_error`。
void save_chat_pick(const std::filesystem::path& root, const std::string& chat,
                    const ChatPick& p);

/// 对话搬家（建出片子）、分叉、删掉的时候跟着走。都不抛。
void move_chat_pick(const std::filesystem::path& from, const std::filesystem::path& to,
                    const std::string& chat);
void copy_chat_pick(const std::filesystem::path& root, const std::string& from_chat,
                    const std::string& to_chat);
void remove_chat_pick(const std::filesystem::path& root, const std::string& chat);

}  // namespace changji::llm
