#pragma once

// 记忆那一页的接口：桌面端设置里「记忆」那一类（2026-09-23）。
//
//   GET  /api/memory?path=<项目目录>          两处的记忆，外加各自在盘上哪儿
//   POST /api/memory/forget {path, scope, name}   忘掉一条
//   POST /api/memory/move   {path, name, to}      挪到另一处
//
// 回的都是**新的一整份**（同 GET），界面照着重画，不自己在本地推。
//
// **走接口不在桌面端直接读盘**：引擎可以在别的机器上，那时「这台电脑」那一处
// 和项目目录都在引擎那台——桌面端自己读盘读的是另一台的。
//
// 实现在外层产品仓库的 `agent/memory_api.cpp`（场记的代码在外层）；单独检出
// 这个仓库时编的是 `memory_api_absent.cpp`，三条都回 404，照实说这一版没有。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

ApiResult get_memory(const std::string& project);
ApiResult post_memory_forget(const nlohmann::json& body);
ApiResult post_memory_move(const nlohmann::json& body);

}  // namespace changji::http
