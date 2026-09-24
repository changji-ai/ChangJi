#pragma once

// `request_guard.hpp` 那条规矩挂到 Crow 上：**一个全局中间件**，每一个请求在进路由
// 之前过一遍。
//
// **为什么是全局的，不是每条路由各调一次**：2026-09-24 先是每条路由自己调
// `ours_only(req)`，只挂了十二条；一审就是九十多条会改东西的路由没挂——改设置、加
// 机器、派活、删片子，还有挂在同一个端口上的工作进程那几条（`/task` 能让引擎把
// 产物写到任意路径）。一条一条挂，新加的路由忘了挂就又是一个洞，而那种洞不报错。
// 全局的话新路由**默认就在门里**（CLAUDE.md 第八条：收成一处）。
//
// 中间件管不着的三样（2026-09-24 审查照着 Crow 1.2.0 的源码对过）：
//   · OPTIONS、404、405 和单页应用那个 catchall，在进中间件之前 Crow 自己就回了——
//     那几个本来就不执行什么。**catchall 里别加逻辑**：它连 Host 都还没读到。
//   · **WebSocket 升级**：中间件会跑，但它的「不认」被 Crow 丢掉、照样升级。所以每一条
//     `CROW_WEBSOCKET_ROUTE` 都要挂 `.onaccept(...)` 调 `accept_upgrade`（test_request_guard
//     里有一条读 server.cpp 源码的守卫，少挂一条当场红）。

#include <crow.h>

#include <nlohmann/json.hpp>

#include "http/request_guard.hpp"

namespace changji::http {

/// 一个 Crow 请求里这道门要看的那几样。
inline RequestFacts facts_of(const crow::request& req, const std::string& method) {
    return RequestFacts{method,
                        req.url,
                        req.get_header_value("Content-Type"),
                        req.get_header_value("Origin"),
                        req.get_header_value("Host"),
                        req.get_header_value("Sec-Fetch-Site"),
                        req.get_header_value("Sec-Fetch-Mode")};
}

struct SameSiteGuard {
    struct context {};

    /// 起服务之前按监听地址填（`guard_policy_for(host)`）。默认按回环算——猜错的方向
    /// 挑代价小的那个：多拦一个远端的名字，比放过一个重绑定的页面强。
    GuardPolicy policy;

    void before_handle(crow::request& req, crow::response& res, context&) {
        const std::string method = crow::method_name(req.method);
        const GuardVerdict v = judge_request(facts_of(req, method), policy);
        if (v.ok()) return;
        res.code = refusal_status(v);
        res.set_header("Content-Type", "application/json");
        res.body = nlohmann::json{{"detail", refusal_message(v)}}.dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace);
        res.end();
    }

    void after_handle(crow::request&, crow::response&, context&) {}
};

/// 引擎和工作进程都用这一种 app：门在里面。
using EngineApp = crow::App<SameSiteGuard>;

/// WebSocket 握手那一道（`CROW_WEBSOCKET_ROUTE(...).onaccept(...)`）。
inline bool accept_upgrade(const crow::request& req, const GuardPolicy& policy) {
    return judge_upgrade(facts_of(req, "GET"), policy).ok();
}

}  // namespace changji::http
