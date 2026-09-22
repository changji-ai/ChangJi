#pragma once

// 一条对话落在哪儿：`<项目目录>/chat.jsonl`。
//
// **这条对话是这部片子的一部分**，所以和 `project.json` / `story.json` 摆在
// 一起——拷走项目目录就把它一起拷走了，换台机器打开还是这一条。
//
// **jsonl 不是 json。** 一行一条、只追加：
//
//   · 写一条不用把整条对话读进来再整份写回去（几百条之后那是每说一句话就
//     重写一个几百 KB 的文件）；
//   · 进程被杀在半路，坏的只有最后那一行，前面的照样读得出来。整份 JSON
//     被截断的话，那是**整条对话都打不开**。
//
// 思考不落这儿：一次写作的思考几千字，而这份文件是要整条读回来的。
// 思考照旧走 `llm_log/` 和 `/api/task/thinking`。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::agent {

/// 对话里的一条。
///
/// **记的是「发生了什么」，不是「发给模型的那一串」。** 两者差别在系统提示
/// 词和每轮重算的状态摘要上——那些每轮都不一样，存下来只会让历史越读越糊涂。
/// 重放这条对话时，系统提示和摘要现算，历史只提供人说过什么、做过什么。
struct Turn {
    /// user / assistant / tool / system
    ///
    /// `system` 这一档是**引擎自己插的话**（「出片做完了：5 镜，1 镜降级」），
    /// 不是系统提示词。界面上它和场记说的话长得不一样。
    std::string role;
    std::string text;

    /// role == "tool" 时：这条回的是哪一次调用。
    std::string tool_name;
    std::string tool_id;

    /// role == "assistant" 且它要调工具时，原样存着（[{id,name,arguments}]）。
    nlohmann::json tool_calls;

    /// role == "tool" 时：这一次动的是哪一章（`ep01`）。没动某一章就是空的。
    ///
    /// 存它是为了**画布跟着对话走**：代理刚拆完第 3 章的分镜，而人眼前那面
    /// 墙还停在第 1 章。从 `episode_id` 那个参数里抠（`util/tool_slot.hpp`）。
    std::string episode;

    /// 毫秒。**存绝对时间不存相对时间**：界面上要显示「刚才」还是「昨天」，
    /// 那是画的时候算的事。
    std::int64_t at = 0;
};

nlohmann::json to_json(const Turn& t);
Turn turn_from_json(const nlohmann::json& j);

/// 一条对话落在哪个文件上。
///
/// **一部片子可以有好几条对话**（2026-09-21）：一条盯着第 3 章的分镜，另一条
/// 在改设定——两件事各自的来龙去脉搅在一条里，模型每轮都要读一遍不相干的。
///
///     chat_id 空      → `<项目目录>/chat.jsonl`      ← 一直以来那一条
///     chat_id = "c17" → `<项目目录>/chats/c17.jsonl`
///
/// ⚠️ **老项目一个字不用改**：它们的那一条就是"空 id"那一条，路径没变。
/// 新开的落在 `chats/` 底下。
///
/// ⚠️ **id 先过 `util::chat_id_ok`**（只认 `[A-Za-z0-9_-]`）。它会变成文件名，
/// 一个带 `..` 的就能写到项目目录外面去。这儿不查——查在接口那一层，
/// 查不过就当场拒；这儿只当它已经查过了。
std::filesystem::path transcript_path(const std::filesystem::path& project_root,
                                      const std::string& chat_id = {});

/// 这部片子有哪几条对话（不含空 id 那一条）。按 id 排。
std::vector<std::string> list_chat_ids(const std::filesystem::path& project_root);

/// 删掉一条对话。删掉了回 true，本来就不在回 false（**不是错**：两个人同时
/// 点了删，第二下不该报错）。
///
/// ⚠️ **这是真删文件，删了就没了。** 上一层要先问过人（见桌面端那一行上的
/// 「删掉？」）。这儿不问——一个既删又问的函数，用例没法只验删那一半。
bool remove_transcript(const std::filesystem::path& project_root,
                       const std::string& chat_id);

/// 读回整条。
///
/// 文件不在就回空（**不是错**：任何项目第一次说话之前都没有这个文件）。
/// **坏行跳过**：一行坏掉不该让整条对话打不开，那正是 jsonl 的用处。
std::vector<Turn> load_transcript(const std::filesystem::path& project_root,
                                  const std::string& chat_id = {});

/// 追加一条。目录不在会建。
void append_turn(const std::filesystem::path& project_root, const Turn& t,
                 const std::string& chat_id = {});

/// 把整条对话原样写回去。**只在搬家时用**（空手起头那条对话落在别处，
/// 项目建出来之后搬进项目目录）。
void write_transcript(const std::filesystem::path& project_root,
                      const std::vector<Turn>& turns,
                      const std::string& chat_id = {});

/// 现在的毫秒数。
std::int64_t now_ms();

}  // namespace changji::agent
