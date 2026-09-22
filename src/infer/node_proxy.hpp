#pragma once

// 把一个请求转给某台节点。
//
// **为什么要转，而不是让界面直接连那台。** 浏览器连不上：那台的地址
// 可能只有引擎这边通（隧道、内网），而且口令在配置里、不该发到前端去。
// 所以界面只跟本机的引擎说话，引擎替它跑一趟。
//
// `local` 这个地址**不走这儿**——调用方自己直接调本地那份实现。转发一圈
// 回到自己身上是白白多一次 HTTP，而且要求本机得对自己可达（它未必）。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"

namespace changji::infer {

struct ProxyResult {
    /// HTTP 状态码。**0 表示压根没连上**——那和"对面回了个错"是两回事，
    /// 要分得开：前者去查网络和地址，后者按对面那句话办。
    int status = 0;
    nlohmann::json body;
};

/// 转一个 GET 过去。`path` 形如 `/setup/state`。
ProxyResult node_get(const config::Settings& s, const std::string& node_url,
                     const std::string& path, int timeout_s = 10);

/// 转一个 POST 过去。
ProxyResult node_post(const config::Settings& s, const std::string& node_url,
                      const std::string& path, const nlohmann::json& body,
                      int timeout_s = 10);

}  // namespace changji::infer
