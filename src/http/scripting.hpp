#pragma once

// 剧本相关的三个接口：想选题、写一章、剪预告。
//
// 三个都是**同步**的：调一次大模型，几十秒内返回，不进 job 表。
// 写全片（/api/script/series）不一样，那个要跑几分钟，走 job 表和
// WebSocket 广播。
//
// 大模型客户端是传进来的，不是这里造的。测试要用 ReplayClient——
// 真调模型的话每次输出都不一样，接口的形状就没法对拍了。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// 预告片挂的章节 id。它在好几处要被特殊对待，所以是个常量。
inline constexpr const char* kTrailerEpisodeId = "trailer";

/// 写这一章剧本时带上的前情：**这一章之前**最近三章的剧本原文。
///
/// ⚠️ **原来这段在两处各写了一遍，而且行为不一样**（CLAUDE.md 的「同一件事
/// 别在两处各写一遍」）：`batch.cpp` 那份取"最近三章写好的"（不管在这一章
/// 前面还是后面），`scripting.cpp` 那份取"这一章之前的最近三章"。写全片时
/// 两者恰好相等，所以谁也没发现——而单写一章时前一种会把**后面几章**当成
/// 已经发生的事
/// 喂给模型，模型就照着写。收成一处，按后一种。
///
/// `before` 留空表示"写在末尾"，此时全部已写的都算在前面。
/// 预告片永远不算：它是从正片里剪出来的，拿它当上下文模型会开始抄自己的
/// 预告，越写越像宣传语。
///
/// **只要最近三章**：全片塞进去小模型撑不住，离得远的剧情对下一章的连贯
/// 性也没什么帮助。2026-09-17 实测这一段是剧本那条提示词里最大的一块
/// （三章 7200 字符，而同一条里的 schema 才 1701）——真要再砍，砍的是
/// 这个数，而**那是个会动到成片质量的决定**，得先拿两版实际剧本比过。
std::string previous_scripts(const models::Project& project,
                             const std::string& before = "",
                             std::size_t keep = 3);

/// POST /api/script/premise —— 想几个选题给人挑。不落库。
ApiResult post_script_premise(const nlohmann::json& body, llm::Client& client,
                              pipeline::CancelToken& tok);

/// POST /api/script/write —— 写一章，回给界面让人过目。
///
/// 只有梗概会落库（下次写新一章时回填，不用凭记忆重打），剧本本身不落。
/// 剧本是整条流水线的源头，源头没审过就往下跑，后面几十分钟的渲染全是白跑。
ApiResult post_script_write(const nlohmann::json& body, llm::Client& client,
                            pipeline::CancelToken& tok);

/// POST /api/script/trailer —— 剪一条预告片。不落库。
ApiResult post_script_trailer(const nlohmann::json& body, llm::Client& client,
                              pipeline::CancelToken& tok);

}  // namespace changji::http
