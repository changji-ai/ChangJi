#pragma once

// 本机引擎只认自己人：**浏览器里别的网页发来的请求一律不认**（2026-09-24）。
//
// 引擎是一个本机 HTTP 服务，而浏览器里随便哪个网页都能往 127.0.0.1 发请求：
//
//   · CORS 的「简单请求」（GET / HEAD / POST，Content-Type 只能是 text/plain、
//     application/x-www-form-urlencoded、multipart/form-data 或者不带，不带自定义
//     头）**不预检**——网页看不见回包，可请求照样到了、照样执行了。改设置（能把
//     大模型后端改成「命令行」）、加机器（把口令发给它指定的地址）、派 GPU 活、
//     删片子，一个 `fetch(url, {mode: "no-cors", body})` 就够。`<img>`、跳转这种 GET
//     连 Origin 都不带。
//   · 别的（JSON、PUT/DELETE、自定义头）要先预检；这个服务从不回 CORS 的许可头，
//     预检过不去，浏览器自己就拦了。**别加 CORS 中间件**（Crow 的 CORSHandler 会给
//     每个回包挂 `Access-Control-Allow-Origin: *`，这一整道就白设了）。
//   · **DNS 重绑定**：evil.example 先回自己的页面、再把域名解析到 127.0.0.1（或者这台
//     的局域网 IP），那个页面对浏览器来说就和引擎同源了——能发 JSON、还能读回包。只比
//     「Origin 和 Host 是不是同一个名字」挡不住它（两边都是 evil.example）。
//
// 所以一道门、四条（`judge_request`，全局中间件和工作进程那道门都调它）：
//
//   一，Host。回环监听（桌面端、命令行默认）时只认回环的名字、地址——正经进来的只可能
//       是这些。对外监听（远端那台、Docker 的 0.0.0.0）时，**浏览器发来的**（带
//       `Sec-Fetch-*`）只认 IP、回环、名单里的名字：域名就是重绑定的样子；不带的
//       （别的引擎按域名连过来、curl）不判。要认自己的域名写进 `CHANGJI_ALLOWED_HOSTS`。
//   二，`Sec-Fetch-Site`（浏览器自己填、网页改不了）：跨站、同站（别的端口、别的子域）
//       发来的一律不认——除了**跳转到界面那几页**（`/`、`/assets/…`，不是接口）。这一条
//       挡的是 Origin 挡不住的那半：`<img>`、跳转、no-cors 的 GET。
//   三，Origin（会改东西的那几种方法）：带了的，得是**和 Host 同一个地址同一个端口**
//       （回环也要同端口——本机别的端口上跑着的网页不算自己人）、或者名单里的。`null`、
//       认不出的都不认。不带的放行：桌面端（Qt）、curl、别的引擎都不带。
//   四，Content-Type（POST / PUT / PATCH）：**只认名单上的**——JSON；`/blob/…`（别的
//       引擎送参考图）认 octet-stream；三条传文件的路由认 multipart。别的一律不认
//       （`text/ping`、`application/csp-report` 这种浏览器自己会发的也在里头）。
//
// ⚠️ 这一道**只防浏览器里的网页**，不防网络上的人：对外监听时 curl 不带 Origin、
// 自己挑 Content-Type，照样进得来。对外监听的 /api 要鉴权是另一件事。

#include <string>
#include <string_view>
#include <vector>

namespace changji::http {

/// Content-Type 的 MIME 本体：`;` 前面那一截，去掉首尾空白，小写。
/// `"Application/JSON; charset=utf-8"` → `"application/json"`。
///
/// **不能拿子串找**：`text/plain; x=application/json` 的本体是 text/plain（CORS 认它
/// 是简单请求、不预检），而它里面有 "application/json" 这几个字。
std::string mime_essence(std::string_view content_type);

/// 这个 Content-Type 浏览器发了**不预检**（含空的）：text/plain、
/// application/x-www-form-urlencoded、multipart/form-data。
bool is_simple_content_type(std::string_view content_type);

/// `Origin` / `Host` / 网址里的主机名：小写，去掉 `scheme://`、路径、端口、末尾那个点；
/// IPv6 留着方括号（`[::1]:8080` → `[::1]`）。认不出的原样小写回去（照「不认」处理）。
std::string host_of(std::string_view origin_or_host);

/// 回环：`localhost`、`*.localhost`、`127.0.0.0/8`、`[::1]`（`::1`）。
bool is_loopback_host(std::string_view host);

/// IP 字面量（IPv4 点分四段、或者方括号里的 IPv6）。域名不算。
bool is_ip_literal(std::string_view host);

/// 接口的路径（不是界面那几页）：`/api/…`、`/bff/…`、WebSocket、工作进程那几条
/// （`/task`、`/blob/…`、`/setup/…`、`/status`、`/health`）。跨站跳转到这些不认。
bool is_api_path(std::string_view path);

struct GuardPolicy {
    /// 监听的是回环（`!infer::is_public_bind(host)`）。
    bool loopback_bind = true;
    /// 额外认的主机名（小写、不带端口；IPv6 带方括号）：`CHANGJI_ALLOWED_HOSTS`，逗号分隔。
    /// 反向代理、自己的域名要写进来。
    std::vector<std::string> allowed_hosts;
};

/// 按监听地址和环境变量 `CHANGJI_ALLOWED_HOSTS` 拼一份。
GuardPolicy guard_policy_for(const std::string& bind_host);

/// 一个请求里这道门要看的那几样（都是原样的头，没有就空）。`path` 不带查询串。
struct RequestFacts {
    std::string_view method;
    std::string_view path;
    std::string_view content_type;
    std::string_view origin;
    std::string_view host;
    std::string_view fetch_site;   ///< `Sec-Fetch-Site`
    std::string_view fetch_mode;   ///< `Sec-Fetch-Mode`
};

enum class Refusal { none, host, origin, content_type };

struct GuardVerdict {
    Refusal refusal = Refusal::none;
    /// 不认的那个值（主机名、Origin、Sec-Fetch-Site、Content-Type），原样，给那句话用。
    std::string what;
    bool ok() const { return refusal == Refusal::none; }
};

/// 每一个请求都过这一道（全局中间件、工作进程那道门）。
GuardVerdict judge_request(const RequestFacts& r, const GuardPolicy& policy);

/// WebSocket 握手：Host、Sec-Fetch-Site、Origin（浏览器对 WebSocket 不做 CORS，任何网页
/// 都能连上来订阅对话、正文的推送——这一道是唯一挡它的）。
GuardVerdict judge_upgrade(const RequestFacts& r, const GuardPolicy& policy);

/// 不认时回什么状态码：Content-Type 415，别的 403。
///
/// ⚠️ **别用 421**（Misdirected Request，按理最贴切）：这一版 Crow 的状态码表里没有它，
/// 连接那一层把不认识的码一律改成 500（同 cmake/patch_crow_422.cmake 修的那件事），
/// 人看见的是「服务端出错」。2026-09-24 审查时抓包看见的。
int refusal_status(const GuardVerdict& v);

/// 不认时那一句（给人看的，SAY）。
std::string refusal_message(const GuardVerdict& v);

}  // namespace changji::http
