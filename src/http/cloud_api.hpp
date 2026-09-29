#pragma once

// 场记云那几条接口（docs/账号与云服务方案.md 第十节）。发请求的函数是注入的——这一层进得了
// 单元测试（测试目标不链网络库），真发请求的那两个在 server.cpp 里接上。
//
//   GET  /api/cloud              登没登、账号、开了哪几样云服务、额度（问云上 /api/cloud/me，缓存 60 秒）
//   GET  /api/cloud/gate         登录页问的：这个浏览器进没进门、这台绑没绑账号（免检）
//   POST /api/cloud/login/start  把这台引擎登到场记账号上 → 授权地址
//   POST /api/cloud/enter/start  没进门的浏览器用场记账号进这台引擎 → 授权地址（免检）
//   GET  /auth/cloud/callback    场记云授权回来（免检，是一次页面跳转）
//   POST /api/cloud/logout       云上吊销这把钥匙 + 删本地那份

#include <string>

#include <nlohmann/json.hpp>

#include "cloud/cloud.hpp"
#include "http/readonly.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// 真发请求的那两个（server.cpp 里接 http::default_http_get / llm::default_http_post）。
struct CloudIo {
    llm::HttpGet get;
    llm::HttpPost post;
};

/// `GET /api/cloud`。`refresh` 为真就不用缓存（登录、退出之后）。
ApiResult get_cloud(const CloudIo& io, const std::string& cloud_url, bool refresh = false);

/// `GET /api/cloud/gate`。`authed` 是这个请求真进门没有（`is_authed`）；`token_hint` 只在
/// 回环监听时给（界面预填本机口令），别的时候传空。
nlohmann::json cloud_gate(bool authed, bool loopback_bind, const std::string& token_hint,
                          const std::string& cloud_url);

/// `POST /api/cloud/login/start`（bind）和 `/enter/start`（enter）。`origin` 是这个请求自己的
/// 来源（`http://<Host>`），回调地址按它算：`<origin>/auth/cloud/callback`。
ApiResult post_cloud_start(const nlohmann::json& body, cloud::Purpose purpose,
                           const std::string& origin, const std::string& cloud_url);

/// 回调处理完往哪儿跳、要不要种进门的 cookie。
struct CloudRedirect {
    std::string location;
    std::string set_cookie;  ///< 空 = 不种
    bool changed = false;    ///< 登录状态变了（要推 `{type:"cloud", event:"changed"}`）
};

/// `GET /auth/cloud/callback`。`ui_token` 是这台引擎的网页口令（用场记账号进门成功时，种的就是
/// 它换来的 cookie——和拿口令进门同一个 cookie）；回环监听时传空，那种时候本来就不用进门。
CloudRedirect cloud_callback(const CloudIo& io, const std::string& code, const std::string& state,
                             const std::string& error, const std::string& ui_token);

/// `POST /api/cloud/logout`。
ApiResult post_cloud_logout(const CloudIo& io);

/// 缓存作废（登录、退出、换了云地址之后）。
void forget_cloud_cache();

}  // namespace changji::http
