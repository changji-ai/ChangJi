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
// ⚠️ **思考现在落这儿了**（2026-09-22 改的）。这儿原来写着「思考不落这儿：
// 一次写作的思考几千字，而这份文件是要整条读回来的」——用户要"思考的内容
// 要持久保留"，而那条路（`/api/task/thinking`）**只在这件活还活着的时候
// 有答案**：一轮跑完，账本上那一行退休，想过的那几千字就再也问不出来了。
//
// 为什么跟着这一行走、不另起一个文件：**分叉、删对话、拷项目目录**这三件
// 事眼下都是"一行一条、整份搬走"的语义（见 `fork` / `list_chat_ids`）。
// 思考单独放一份的话，这三处各要多认一次它，而漏掉的那一处就是"分叉出来
// 的对话里思考不见了"或者"删了对话盘上还留着一堆孤儿"。
//
// 代价用上限挡住：`kThinkingKeep` 只留最后那一截（见下）。这份文件是整条
// 读回来的，而一次代理对话的思考一两千字，几十轮也就几百 KB。
//
// 发给模型的那一串里**不带它**（`loop.cpp` 只读 role/text/tool_*）：
// 它是给人看的记录，不是上下文。

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::agent {

/// 一条里最多留多少**字节**的思考。
///
/// 超了从**头上**截，留最近那一截——人回头翻的是"它最后是怎么想的"，
/// 而开头那几句多半是复述任务。和 `pipeline::Task::append_thinking` 的
/// 上限是同一个形状，只是这儿小得多：那份活在内存里，这份要整条读回来。
inline constexpr std::size_t kThinkingKeep = 24 * 1024;

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

    /// role == "assistant" 时：**说这句话之前它想了什么**。没想就是空的。
    ///
    /// 长度上限见 `kThinkingKeep`（超了从头上截）。
    std::string thinking;

    /// role == "assistant" 且它要调工具时，原样存着（[{id,name,arguments}]）。
    nlohmann::json tool_calls;

    /// role == "tool" 时：这一次动的是哪一章（`ep01`）。没动某一章就是空的。
    ///
    /// 存它是为了**画布跟着对话走**：代理刚拆完第 3 章的分镜，而人眼前那面
    /// 墙还停在第 1 章。从 `episode_id` 那个参数里抠（`util/tool_slot.hpp`）。
    std::string episode;

    /// 这一条**带着的东西**：图、片、改过的那几段字。给人看的，**不发给模型**
    ///（`loop.cpp` 只读 role/text/tool_*，同 `thinking`）。
    ///
    /// 用户 2026-09-23：「所有 llm 的信息内容操作生成的图片/视频编辑的文本
    /// 都要在对话里体现显示出来」。在这之前对话里只有一行「开始画参考图。」
    /// 和一句「刚才派出去的活跑完了。」——出了几张、长什么样、哪几件没成，
    /// 一样都不在对话里，要自己去翻右边那几格。
    ///
    /// 一件一个对象，形状见 `agent/outcome.hpp` 的 `media_item`。
    /// **存相对项目根的路径，不存地址**：地址是界面那头按"引擎在不在本机"
    /// 现算的（`desktop/media_url.hpp`），存死了拷到别的机器上就打不开。
    nlohmann::json media;

    /// 这一条**出没出岔子**。空 = 正常；`error` = 没做成（工具报错、模型那头
    /// 出错）；`warn` = 做了但没做完（转满轮数停下、派出去的活里有几件没成）。
    ///
    /// 界面靠它把出错的那几条画得和正常的不一样（2026-09-23 之前一样——
    /// 「跑不动：…」和「改好了：…」同一个颜色，扫一眼分不出哪一步砸了）。
    /// **判据是这一栏，不是那句话**：认「跑不动」三个字的话改一个字就散，
    /// 十一种语言也认不过来（CLAUDE.md 第八条）。
    std::string level;

    /// 引擎插的「跑完了」那一条：几件成、几件没成，**拆开的那一份**（形状见
    /// `agent/outcome.hpp` 的 `WorkReport`）。正文 `text` 里是同一件事的平文，
    /// 给模型读；这一份给界面画——没成的那几行要画成出错的样子，而平文里分不
    /// 出哪一行是哪一节。
    nlohmann::json report;

    /// **附在这一句后面发给模型、界面上不摆的那一段。**
    ///
    /// 人带着附件说一句话时（`agent/attachments.hpp`）：界面上那一条是他说的话
    /// 加几张缩略图，模型收到的是话 + 「【附件】董平.png（图片，1.2 MB）路径：…」
    /// ——字稿、Word 稿还贴着正文。那几千字摆进人的气泡里就是一整屏，而模型
    /// 不贴就什么都读不到。`loop.cpp` 拼消息时接在 `text` 后面。
    std::string for_model;

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
