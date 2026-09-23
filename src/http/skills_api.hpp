#pragma once

// 技能那一页的接口：桌面端设置里「技能」那一类（2026-09-24）。
//
//   GET  /api/skills?path=<项目目录>                      三处的技能，外加各自在盘上哪儿
//   POST /api/skills/install {path, url, scope, replace}  从网上装一个
//   POST /api/skills/forget  {path, scope, folder}        删掉一个
//   POST /api/skills/move    {path, scope, folder, to}    挪到另一处（Claude 那一处的是拷）
//
// 回的都是**新的一整份**（同 GET），界面照着重画。认一个技能按「哪一处 + 文件夹名」，
// 不按名字：同一处的两个文件夹可以写着同一个 name。
//
// **走接口不在桌面端直接读盘**，理由同 memory_api.hpp：引擎可以在别的机器上。
//
// 实现在外层产品仓库的 `agent/skills_api.cpp`（技能是场记的东西）；单独检出
// 这个仓库时编的是 `skills_api_absent.cpp`，都回 404。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"

namespace changji::http {

ApiResult get_skills(const std::string& project);
/// `get` 是下载用的（`llm::default_http_get()`）；由路由那头给，这一层不链网络库。
ApiResult post_skills_install(const nlohmann::json& body, const llm::HttpGet& get);
ApiResult post_skills_forget(const nlohmann::json& body);
ApiResult post_skills_move(const nlohmann::json& body);

}  // namespace changji::http
