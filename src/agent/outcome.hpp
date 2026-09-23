#pragma once

// 派出去的活**做出了什么**：图、片、改过的那几段字，外加几件成、几件没成。
//
// 用户 2026-09-23：「所有 llm 的信息内容操作生成的图片/视频编辑的文本都要在
// 对话里体现显示出来」。在这之前，一件活派出去到做完，对话里只有两句话：
//
//     · 开始画参考图。
//     ——— 刚才派出去的活跑完了。———
//
// 出了几张、长什么样、哪几件没成，一样都不在对话里。更坏的是**没成也是这
// 一句**：2026-09-23 翻「旧手机故事」那条对话，写大纲派了二十多次，每一次
// 做完都是「跑完了」，而故事一章都没有——人看不出，模型也看不出。
//
// ---
//
// **判据挂在产出物上，不挂在"派了什么"上**（CLAUDE.md 第十½条同一个道理）。
// 一件活能牵出好几件（出片那一件底下是配音、首帧、每一镜），而它们各自落在
// 哪儿、叫什么，只有盘上说得准。所以做法是：
//
//   1. 派第一件活之前拍一张盘面（`take_baseline`）：每张图、每条片的 mtime，
//      每段字（大纲、正文、剧本、分镜表、设定）的指纹；
//   2. 做完了再拍一张，**比出变了的那几样**（`changed_media`）。
//
// 比 mtime 不比"路径在不在"：首帧和参考图都是**原地覆盖**的，重出一张，
// 路径一个字没变。
//
// 字那一族比指纹不存全文：一部二十章的片子，正文加剧本几十万字，盘面里只
// 要知道"变没变"。变了的那几段**做完那一下再现读**，读的是和 `*_read` 那几个
// 工具同一份（`run_tool` 给人看的那一遍），一处拼，两处用（CLAUDE.md 第八条）。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace changji::agent {

/// 盘面上的一样东西。
struct Piece {
    /// 文件那一族：相对项目根的路径。字那一族是空的。
    std::string rel;
    /// 文件那一族：mtime（秒，带小数）。
    double mtime = 0.0;
    /// 字那一族：内容的指纹。
    std::size_t sig = 0;
};

/// 一张盘面。
struct Baseline {
    /// 拍的那一刻（毫秒）。
    std::int64_t at = 0;
    /// 那一刻账本上最大的那个 id。**比它大的才是这一回派出去的**——账本上
    /// 还躺着上一轮、别的项目做完的两百件，按项目筛不掉同一部片子上一回的。
    std::uint64_t task_floor = 0;
    /// 键见 .cpp 里 `take_baseline` 上那张表。
    std::map<std::string, Piece> pieces;
};

/// 拍一张。**不抛**：读不动的那几样（项目还没建全、story.json 还不在）就不在
/// 盘面里，做完那一下它们出现了，正好算"新出的"。
Baseline take_baseline(const std::filesystem::path& root);

/// 一件附件。`kind` 是 image / video / text。
///
/// `rel` 是相对项目根的路径（文字那一族是空的）；`title` 是给人看的那一行
///（「董平 · 正面」「第 1 章 sh003」）；`text` 是文字那一族的正文；
/// `poster` 是视频那一族的封面（那一镜的首帧），没有就空。
nlohmann::json media_item(const std::string& kind, const std::string& rel,
                          const std::string& title, const std::string& text = {},
                          const std::string& poster = {});

// ---- 同步那几个工具当场挂上的 ----
//
// 派出去的活要等做完才比得出产出；而改一镜、设一张参考图、读一眼设定是
// **当场**就有结果的，挂在那条工具回话上。

/// 一张参考图。`slot` 是 front / three_quarter / back，场景是 empty。
/// 图不在盘上回 null。
nlohmann::json reference_media(const std::filesystem::path& root,
                               const std::string& id, const std::string& slot);

/// 设定里所有**在盘上的**参考图（角色正面、场景空景），读设定时一起摆。
nlohmann::json references_media(const std::filesystem::path& root);

/// 一章里出了首帧的那几镜，读分镜时一起摆。
nlohmann::json frames_media(const std::filesystem::path& root,
                            const std::string& episode_id);

/// 成片目录那几条，读成片时一起摆。
nlohmann::json outputs_media(const std::filesystem::path& root);

/// 一镜此刻的样子（json）。找不到回 null。改之前、改之后各取一份交给
/// `shot_change_media` 比。
nlohmann::json shot_json(const std::filesystem::path& root,
                         const std::string& episode_id, const std::string& shot_id);

/// 改一镜：**改了哪几栏、从什么改成什么**，外加那一镜的首帧（看得出是哪一镜）。
nlohmann::json shot_change_media(const std::filesystem::path& root,
                                 const std::string& episode_id,
                                 const std::string& shot_id,
                                 const nlohmann::json& before,
                                 const nlohmann::json& after);

/// `before` 以来**变了的和新出的**，按「字 → 图 → 片」排好、截过上限。
///
/// 上限是给界面留的：一章出片是十几镜，每镜一张首帧一条片子，整部电影
/// 就是几百件——全摆进一条消息里，这条对话就只剩这一条了。
nlohmann::json changed_media(const std::filesystem::path& root,
                             const Baseline& before);

/// 这一回派出去的活，几件成、几件没成——**说给人听，也说给模型听**。
///
/// 回的是接在「刚才派出去的活跑完了。」后面那几行；账本上找不到这一回的活
/// 就回空。没成的那几件**一件一行，带着原话**：模型要靠它决定下一步是重试
/// 还是去问人，而原来它只收到一句「跑完了」，于是同一件派了二十多次。
std::string work_report(const std::string& project, std::uint64_t task_floor);

/// 账本上此刻最大的 id（见 `Baseline::task_floor`）。
std::uint64_t task_floor_now();

}  // namespace changji::agent
