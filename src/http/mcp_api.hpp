#pragma once

// 扩展（MCP）那一页的接口：桌面端设置里「扩展」那一类（2026-09-24）。
//
//   GET  /api/mcp?path=<项目目录>                       两处的扩展和各自眼下的样子
//   POST /api/mcp/add    {path, scope, text, name, replace}   粘进来的那一段（README 上的
//                                                        `{"mcpServers": …}`）加进去
//   POST /api/mcp/remove {path, scope, name}             拿掉
//   POST /api/mcp/move   {path, name, to}                挪到另一处
//   POST /api/mcp/enable {path, scope, name, on}         启用 / 停用
//   POST /api/mcp/trust  {path, name}                    这部片子里带来的那一个，点头让它起
//
// 回的都是**新的一整份**（同 GET）。GET 会在后台把该起的起起来（不等），界面
// 看见还有「在起」的就过一会儿再问一次。
//
// 实现在外层产品仓库的 `agent/mcp_api.cpp`；单独检出这个仓库时编的是
// `mcp_api_absent.cpp`，都回 404。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// `post` 是网上那种扩展用的（`llm::default_http_post()`）；由路由那头给，
/// 这一层不链网络库。
ApiResult get_mcp(const std::string& project, const llm::HttpPost& post);
ApiResult post_mcp_add(const nlohmann::json& body, const llm::HttpPost& post);
ApiResult post_mcp_remove(const nlohmann::json& body, const llm::HttpPost& post);
ApiResult post_mcp_move(const nlohmann::json& body, const llm::HttpPost& post);
ApiResult post_mcp_enable(const nlohmann::json& body, const llm::HttpPost& post);
ApiResult post_mcp_trust(const nlohmann::json& body, const llm::HttpPost& post);

/// 把扩展的进程都关掉。`http::run()` 收尾时调（服务停了就关），main() 和桌面端不用管。
void mcp_shutdown();

}  // namespace changji::http
