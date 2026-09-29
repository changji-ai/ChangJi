#pragma once

// 会话分叉那两条接口（2026-09-27）。一条对话是一棵树：「改一改重说」发出去那一句在被改那一句
// 的上一行底下另起一支，旧的那一支原样留着；每个分叉点选哪一支记在那个分叉点自己身上（见外层
// agent/transcript.hpp 「分叉」那一段）。
//
//     GET  /api/chat/forks   眼前这条线上的分叉点
//     POST /api/chat/branch  某个分叉点换一支
//
// 实现在外层 agent/chat_branch_api.cpp；单独检出引擎仓库时编 chat_branch_api_absent.cpp，照实回 404。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

/// GET /api/chat/forks?project=&chat= —— 眼前这条线上的分叉点，从上到下：
///
///     {forks: [{parent, variants: [{seq, at, text, selected, below}]}]}
///
/// `parent`：分叉点（那几支共同的上一行，-1 = 开头）。`below`：走这一支、按它底下各分叉点记着的选法
/// 一路下去还会遇到几个分叉点。**只有眼前这条线上的**——别的支上的分叉点在那一支被选中之前不出现。
ApiResult get_chat_forks(const std::string& project, const std::string& chat);

/// POST /api/chat/branch —— body `{project, chat?, parent, child}`：`parent` 那个分叉点换到 `child` 那一支。
/// 回 `{ok, forks}`，推一条 `{event: "switched"}`（开着这一条的窗口都重读）。这一条正在跑回 409
/// （那一轮还在往眼前这条线上接）；`child` 不是 `parent` 底下的一支回 400。
ApiResult post_chat_branch(const nlohmann::json& body);

}  // namespace changji::http
