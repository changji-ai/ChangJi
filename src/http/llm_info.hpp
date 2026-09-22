#pragma once

// 大模型的接入信息：常见平台的地址清单、可用模型列表。
//
// ---
//
// 方案的破契约白名单里原本有这两条，理由是"进程内推理之后语义重定义"。
// **决策 4 之后这个理由不成立了。**
//
// 那条决策定的是"Pi 保留、模型文件走配置"，而它的一个直接后果是：
// 用远端大模型不再是过渡状态，是长期形态之一——树莓派没有跑 14B 的内存，
// 它只能打到局域网的 Windows 机器或者云端。所以：
//
//   /api/llm/providers  原样保留。那份"省得查文档"的地址清单照样有用。
//   /api/llm/models     **扩展**不是替换。远端有哪些模型照旧列，
//                       另外加一个字段列本地的 gguf。
//
// 加字段是向后兼容的：前端现在只读 .models 和 .error，多出来的它不看。
// 换成"改成列本地 gguf"就真的破契约了——设置页那个下拉框会突然从
// "远端服务上的模型"变成"本机文件"，而用户配的是远端服务。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// GET /api/llm/providers —— 常见大模型平台的接入地址。
///
/// 这些平台都提供 OpenAI 兼容接口，所以不用为每一家写适配器——
/// 填对地址和密钥就能用。列出来只是免得用户去翻各家文档找那一行 base_url，
/// 选完仍然可以手改，这里不锁死任何东西。
ApiResult get_llm_providers();

/// 发一次 GET。和 llm::HttpPost 一样是注入的，理由也一样。
using HttpGet = std::function<llm::HttpResponse(
    const std::string& url, const std::map<std::string, std::string>& headers,
    double timeout_s)>;

/// GET /api/llm/models —— 那台大模型服务上都有哪些模型，外加本机的 gguf。
///
/// 模型名以前只能手打。打错了要跑到写剧本那一步才报错，而报出来的是一个
/// 404——用户看不出是地址错了还是名字错了。列表拉过来给人选，
/// 这类错就没机会发生。
///
/// 问不到就返回空列表并说明原因，界面退回手打，不至于因为列不出来就没法填。
ApiResult get_llm_models(const config::Settings& settings, const HttpGet& fetch);

/// 同上，但**问指定的那一家**，不是配置里存着的那一家。
///
/// ⚠️ **换平台那一下必须用它。** 模型窗里挑了另一家之后，界面上的地址已经
/// 换了，而配置里还是旧的——照旧问 `get_llm_models` 的话，回来的是**上一家
/// 的模型列表**，而且一声不响（用户 2026-09-17：「大语言模型选择平台后无法
/// 立即刷新模型列表」）。
///
/// `base_url` 留空就用配置里那个；`api_key` 留空也用配置里那把——刚换一家、
/// 密钥还没填的时候，回来多半是 401，而那条路本来就返回"空列表加一句原因"。
///
/// **密钥只走请求体，不进查询串**：查询串会落进访问日志和浏览器历史。
ApiResult post_llm_models(const nlohmann::json& body,
                          const config::Settings& settings,
                          const HttpGet& fetch);

/// 用 cpp-httplib 发 GET。定义在 client_http.cpp 里。
HttpGet default_http_get();

}  // namespace changji::http
