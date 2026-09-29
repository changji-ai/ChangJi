// 本机引擎只认自己人（`http/request_guard.hpp` + `http/crow_guard.hpp`，2026-09-24）。
//
// 钉的是会静悄悄漏的那几处：
//   · Content-Type 按 MIME 本体比、**只认名单上的**（`text/plain; x=application/json`、
//     `text/ping` 都不认）；
//   · DNS 重绑定：回环监听时 Host 只认回环；对外监听时浏览器发来的只认 IP 和名单；
//   · `Sec-Fetch-Site`：跨站的 `<img>`、跳转到接口上都不认（Origin 挡不住它们）；
//   · 回环的 Origin 要和 Host 同端口（本机别的端口上的网页不算自己人）；
//   · 桌面端、curl、别的引擎不带 Origin / Sec-Fetch，照样放行；
//   · 中间件真挂上了：拿一个真的 crow::request 走一遍 before_handle；
//   · 每一条 WebSocket 路由都挂了 onaccept（中间件的「不认」对升级不起作用）。

#include <doctest/doctest.h>

#include <crow.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
#include <iterator>
#include <string>

#include "http/crow_guard.hpp"
#include "http/listen_addr.hpp"
#include "http/readonly.hpp"
#include "http/request_guard.hpp"
#include "http/ui_token.hpp"
#include "util/paths.hpp"

using namespace changji;
using http::GuardPolicy;
using http::Refusal;
using http::RequestFacts;

namespace {

GuardPolicy loopback() {
    GuardPolicy p;
    p.loopback_bind = true;
    return p;
}

GuardPolicy public_bind() {
    GuardPolicy p;
    p.loopback_bind = false;
    p.tokens = {"s3cret"};
    return p;
}

constexpr const char* kBearer = "Bearer s3cret";

struct Req {
    std::string method = "POST";
    std::string path = "/api/settings";
    std::string ct = "application/json";
    std::string origin;
    std::string host = "127.0.0.1:8080";
    std::string site;
    std::string mode;
    std::string auth;
    std::string cookie;
    std::string qtoken;
    std::string ticket;
    RequestFacts facts() const { return {method, path, ct, origin, host, site, mode, auth, cookie, qtoken, ticket}; }
};

Refusal judge(const Req& r, const GuardPolicy& p = loopback()) { return http::judge_request(r.facts(), p).refusal; }

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("门：Content-Type 按 MIME 本体比，不拿子串找") {
    CHECK(http::mime_essence("application/json") == "application/json");
    CHECK(http::mime_essence("Application/JSON; charset=UTF-8") == "application/json");
    CHECK(http::mime_essence("  application/json ;charset=utf-8") == "application/json");
    CHECK(http::mime_essence("text/plain; x=application/json") == "text/plain");
    CHECK(http::mime_essence("") == "");
    for (const char* simple : {"", "text/plain", "TEXT/PLAIN;charset=utf-8", "text/plain; x=application/json",
                               "application/x-www-form-urlencoded", "multipart/form-data; boundary=application/json"}) {
        CAPTURE(simple);
        CHECK(http::is_simple_content_type(simple));
    }
    CHECK_FALSE(http::is_simple_content_type("application/json"));
    CHECK_FALSE(http::is_simple_content_type("application/octet-stream"));
}

TEST_CASE("门：主机名、回环、IP 字面量、接口路径") {
    CHECK(http::host_of("http://localhost:5173") == "localhost");
    CHECK(http::host_of("HTTP://Evil.Example:80/x?y") == "evil.example");
    CHECK(http::host_of("127.0.0.1:8080") == "127.0.0.1");
    CHECK(http::host_of("[::1]:8080") == "[::1]");
    CHECK(http::host_of("localhost.:8080") == "localhost");
    for (const char* h : {"localhost", "LOCALHOST", "127.0.0.1", "127.9.8.7", "[::1]", "::1", "app.localhost"}) {
        CAPTURE(h);
        CHECK(http::is_loopback_host(h));
    }
    for (const char* h : {"evil.example", "128.0.0.1", "127.0.0.1.evil.example", "localhost.evil.example",
                          "0.0.0.0", "192.168.1.5", "", "localhost2"}) {
        CAPTURE(h);
        CHECK_FALSE(http::is_loopback_host(h));
    }
    CHECK(http::is_ip_literal("43.110.148.239"));
    CHECK(http::is_ip_literal("[2001:db8::1]"));
    CHECK_FALSE(http::is_ip_literal("256.1.1.1"));
    CHECK_FALSE(http::is_ip_literal("films.example.com"));
    for (const char* p : {"/api/settings", "/bff/setup/download", "/ws", "/task", "/task/7/cancel", "/blob/x",
                          "/setup/download", "/status", "/health"}) {
        CAPTURE(p);
        CHECK(http::is_api_path(p));
    }
    for (const char* p : {"/", "/assets/index.js", "/project/abc", "/tasks", "/apix"}) {
        CAPTURE(p);
        CHECK_FALSE(http::is_api_path(p));
    }
}

TEST_CASE("门：自己人照样进——桌面端、curl、别的引擎、网页端同源、Vite、SSH 隧道") {
    // 不带 Origin、不带 Sec-Fetch（Qt、curl、httplib）：JSON 放行。Qt 的 XHR 会自己补 charset。
    CHECK(judge(Req{}) == Refusal::none);
    CHECK(judge(Req{.ct = "application/json;charset=UTF-8"}) == Refusal::none);
    CHECK(judge(Req{.host = "localhost:8080"}) == Refusal::none);
    // 引擎自己带的网页端（同源，浏览器带 Sec-Fetch-Site: same-origin）。
    CHECK(judge(Req{.origin = "http://127.0.0.1:8080", .site = "same-origin", .mode = "cors"}) == Refusal::none);
    // Vite：同源的请求它把 Origin 换成引擎自己的（vite.config.js），Host 换成引擎的（changeOrigin）。
    CHECK(judge(Req{.origin = "http://127.0.0.1:8080", .site = "same-origin"}) == Refusal::none);
    // SSH 隧道：浏览器开 localhost:9000，两边一样。
    CHECK(judge(Req{.origin = "http://localhost:9000", .host = "localhost:9000", .site = "same-origin"}) ==
          Refusal::none);
    CHECK(judge(Req{.origin = "http://[::1]:8080", .host = "[::1]:8080"}) == Refusal::none);
    // 别的引擎往工作进程那几条上送的：blob 是 octet-stream，取消是空的 JSON，探活 POST /health。
    CHECK(judge(Req{.path = "/blob/abc", .ct = "application/octet-stream", .host = "127.0.0.1:9101"}) ==
          Refusal::none);
    CHECK(judge(Req{.path = "/task/7/cancel", .host = "127.0.0.1:9101"}) == Refusal::none);
    CHECK(judge(Req{.path = "/health", .host = "127.0.0.1:9101"}) == Refusal::none);
    // 网页端传参考图、声音、对话附件：multipart，只在那四条上认。
    for (const char* up : {"/api/character/reference", "/api/location/reference", "/api/character/voice",
                           "/api/chat/upload"}) {
        CAPTURE(up);
        CHECK(judge(Req{.path = up, .ct = "multipart/form-data; boundary=x", .origin = "http://127.0.0.1:8080",
                        .site = "same-origin"}) == Refusal::none);
    }
    // 读的：同源的 GET、书签打开（none）、别人给的链接跳转到界面上。
    CHECK(judge(Req{.method = "GET", .path = "/api/project", .ct = "", .site = "same-origin"}) == Refusal::none);
    CHECK(judge(Req{.method = "GET", .path = "/", .ct = "", .site = "none", .mode = "navigate"}) == Refusal::none);
    CHECK(judge(Req{.method = "GET", .path = "/", .ct = "", .site = "cross-site", .mode = "navigate"}) ==
          Refusal::none);
    CHECK(judge(Req{.method = "GET", .path = "/assets/index.js", .ct = "", .site = "cross-site",
                    .mode = "navigate"}) == Refusal::none);
    // DELETE 不判 Content-Type（桌面端删对话不带）。
    CHECK(judge(Req{.method = "DELETE", .path = "/api/chats", .ct = ""}) == Refusal::none);
    // HTTP/1.0 不带 Host：浏览器不会这样。
    CHECK(judge(Req{.host = ""}) == Refusal::none);
}

TEST_CASE("门：Content-Type 只认名单上的——简单请求、浏览器自己发的那几种、放错地方的") {
    for (const char* ct : {"text/plain", "text/plain;charset=UTF-8", "", "application/x-www-form-urlencoded",
                           "text/plain; x=application/json", "multipart/form-data; boundary=application/json",
                           "text/ping", "application/csp-report", "application/reports+json"}) {
        CAPTURE(ct);
        CHECK(judge(Req{.ct = ct}) == Refusal::content_type);
    }
    // 放错地方的：multipart 不在传文件那四条上、octet-stream 不在 /blob 上。
    CHECK(judge(Req{.ct = "multipart/form-data; boundary=x", .origin = "http://127.0.0.1:8080"}) ==
          Refusal::content_type);
    CHECK(judge(Req{.ct = "application/octet-stream"}) == Refusal::content_type);
    // 身体被无视的那几条也一样：/api/stop 收 text/ping 原来是放行的（2026-09-24 审查）。
    CHECK(judge(Req{.path = "/api/stop", .ct = "text/ping"}) == Refusal::content_type);
}

TEST_CASE("门：别的网站、本机别的端口发来的 Origin 一律不认") {
    for (const char* o : {"http://evil.example", "https://evil.example:8080", "null", "file://",
                          "http://127.0.0.1:8080/x", "http://user@127.0.0.1:8080", "http://127.0.0.1.evil.example"}) {
        CAPTURE(o);
        CHECK(judge(Req{.origin = o}) == Refusal::origin);
    }
    // 本机别的端口上跑着的网页（另一个开发服务器、笔记本）：回环，但端口不一样。
    CHECK(judge(Req{.origin = "http://localhost:5173"}) == Refusal::origin);
    CHECK(judge(Req{.origin = "http://127.0.0.1:3000"}) == Refusal::origin);
    // 传文件那四条也判 Origin：它们没法靠 Content-Type 挡。
    CHECK(judge(Req{.path = "/api/character/reference", .ct = "multipart/form-data; boundary=x",
                    .origin = "http://evil.example"}) == Refusal::origin);
    CHECK(judge(Req{.method = "DELETE", .path = "/api/chats", .ct = "", .origin = "http://evil.example"}) ==
          Refusal::origin);
}

TEST_CASE("门：Sec-Fetch-Site——跨站的 <img>、跳转到接口上，Origin 挡不住的那半") {
    // `<img src="http://127.0.0.1:8080/api/nodes/setup?url=...">`：GET、不带 Origin。
    CHECK(judge(Req{.method = "GET", .path = "/api/nodes/setup", .ct = "", .site = "cross-site", .mode = "no-cors"}) ==
          Refusal::origin);
    // `location = ".../api/..."`：跳转，但跳到的是接口。
    CHECK(judge(Req{.method = "GET", .path = "/api/nodes/setup", .ct = "", .site = "cross-site", .mode = "navigate"}) ==
          Refusal::origin);
    // 同站（本机别的端口、别的子域）也不认。
    CHECK(judge(Req{.method = "GET", .path = "/api/project", .ct = "", .site = "same-site", .mode = "no-cors"}) ==
          Refusal::origin);
    // no-cors 的 POST 就算 Content-Type 蒙对了也先在这儿挡下。
    CHECK(judge(Req{.site = "cross-site", .mode = "no-cors"}) == Refusal::origin);
}

TEST_CASE("门：DNS 重绑定——回环监听连 GET 都不认；对外监听时浏览器发来的只认 IP 和名单") {
    CHECK(judge(Req{.origin = "http://evil.example:8080", .host = "evil.example:8080"}) == Refusal::host);
    CHECK(judge(Req{.method = "GET", .path = "/api/chat/history", .ct = "", .host = "evil.example:8080",
                    .site = "same-origin"}) == Refusal::host);
    // 对外监听（Docker 的 0.0.0.0、局域网）：重绑定到这台之后，浏览器发来的 GET 带着
    // Sec-Fetch-Site: same-origin、Host 是那个域名——不认。
    CHECK(judge(Req{.method = "GET", .path = "/api/projects", .ct = "", .host = "evil.example:8080",
                    .site = "same-origin", .mode = "cors"},
                public_bind()) == Refusal::host);
    // 别的引擎按域名连过来（`[[peer.nodes]]` 写的是 gpu.example.com）：不带 Sec-Fetch，照样进。
    CHECK(judge(Req{.method = "GET", .path = "/status", .ct = "", .host = "gpu.example.com:8080"}, public_bind()) ==
          Refusal::none);
    // 对外监听时 Origin 是域名、和 Host 同名也不认（域名可以重绑定）。
    CHECK(judge(Req{.origin = "http://evil.example:8080", .host = "evil.example:8080"}, public_bind()) ==
          Refusal::origin);
}

TEST_CASE("门：远端那台（对外监听）——同一个 IP 同一个端口放行，域名要写进允许名单") {
    const std::string host = "43.110.148.239:8080";
    CHECK(judge(Req{.origin = "http://43.110.148.239:8080", .host = host, .site = "same-origin",
                    .cookie = "changji_ui=s3cret"},
                public_bind()) == Refusal::none);
    CHECK(judge(Req{.origin = "http://43.110.148.239:3000", .host = host}, public_bind()) == Refusal::origin);
    CHECK(judge(Req{.origin = "http://43.110.148.239", .host = "43.110.148.239", .auth = kBearer}, public_bind()) ==
          Refusal::none);
    // SSH 隧道开对外监听的那台（AutoDL）：localhost 两边同端口。
    CHECK(judge(Req{.origin = "http://localhost:8080", .host = "localhost:8080", .site = "same-origin",
                    .cookie = "changji_ui=s3cret"},
                public_bind()) == Refusal::none);
    // 反向代理后面的域名：不在名单里不认，写进名单就认。
    CHECK(judge(Req{.origin = "https://films.example.com", .host = "films.example.com", .site = "same-origin"},
                public_bind()) == Refusal::host);
    GuardPolicy named = public_bind();
    named.allowed_hosts = {"films.example.com"};
    CHECK(judge(Req{.origin = "https://films.example.com", .host = "films.example.com", .site = "same-origin",
                    .cookie = "changji_ui=s3cret"},
                named) == Refusal::none);
    GuardPolicy lo = loopback();
    lo.allowed_hosts = {"studio.local"};
    CHECK(judge(Req{.method = "GET", .path = "/", .ct = "", .host = "studio.local:8080"}, lo) == Refusal::none);
}

TEST_CASE("门：WebSocket 握手判 Host、Sec-Fetch-Site、Origin") {
    const auto up = [](std::string origin, std::string host, std::string site = "") {
        const RequestFacts f{"GET", "/api/ws", "", origin, host, site, "websocket"};
        return http::judge_upgrade(f, loopback()).refusal;
    };
    CHECK(up("", "127.0.0.1:8080") == Refusal::none);                                       // 桌面端 QWebSocket
    CHECK(up("http://127.0.0.1:8080", "127.0.0.1:8080", "same-origin") == Refusal::none);   // 网页端 / Vite 改过的
    CHECK(up("http://evil.example", "127.0.0.1:8080", "cross-site") == Refusal::origin);
    CHECK(up("http://evil.example", "127.0.0.1:8080") == Refusal::origin);
    CHECK(up("null", "127.0.0.1:8080") == Refusal::origin);
    CHECK(up("http://evil.example:8080", "evil.example:8080") == Refusal::host);
}

TEST_CASE("门：按监听地址定规矩，允许名单从环境变量读（光着写的 IPv6 也认）") {
    const std::string old = paths::env("CHANGJI_ALLOWED_HOSTS");
    paths::set_env("CHANGJI_ALLOWED_HOSTS", "https://Films.Example.com:443, studio.local;fe80::1,,");
    const auto lo = http::guard_policy_for("127.0.0.1");
    CHECK(lo.loopback_bind);
    REQUIRE(lo.allowed_hosts.size() == 3);
    CHECK(lo.allowed_hosts[0] == "films.example.com");
    CHECK(lo.allowed_hosts[1] == "studio.local");
    CHECK(lo.allowed_hosts[2] == "[fe80::1]");
    CHECK_FALSE(http::guard_policy_for("0.0.0.0").loopback_bind);
    paths::set_env("CHANGJI_ALLOWED_HOSTS", old);
}

TEST_CASE("门：不认时的状态码和那一句——别用 Crow 不认识的 421") {
    CHECK(http::refusal_status({Refusal::host, "evil.example"}) == 403);
    CHECK(http::refusal_status({Refusal::origin, "http://evil.example"}) == 403);
    CHECK(http::refusal_status({Refusal::content_type, "text/plain"}) == 415);
    const std::string h = http::refusal_message({Refusal::host, "evil.example"});
    CHECK(h.find("evil.example") != std::string::npos);
    CHECK(h.find("CHANGJI_ALLOWED_HOSTS") != std::string::npos);
    const std::string o = http::refusal_message({Refusal::origin, "http://evil.example"});
    CHECK(o.find("http://evil.example") != std::string::npos);
    CHECK(o.find("CHANGJI_ALLOWED_HOSTS") != std::string::npos);
    CHECK_FALSE(http::refusal_message({Refusal::content_type, "x"}).empty());
}

TEST_CASE("门：中间件本身——一个真的 crow::request 走 before_handle") {
    http::SameSiteGuard guard;
    guard.policy = loopback();
    http::SameSiteGuard::context ctx;

    {   // 别的网站 no-cors 发来的 text/plain：当场回 403，回包里是那句人话。
        crow::request req;
        req.method = crow::HTTPMethod::Post;
        req.url = "/api/settings";
        req.add_header("Host", "127.0.0.1:8080");
        req.add_header("Origin", "http://evil.example");
        req.add_header("Content-Type", "text/plain");
        crow::response res;
        guard.before_handle(req, res, ctx);
        CHECK(res.is_completed());
        CHECK(res.code == 403);
        CHECK(res.body.find("detail") != std::string::npos);
    }
    {   // 桌面端发的 JSON：放行（没结束、码没动）。
        crow::request req;
        req.method = crow::HTTPMethod::Post;
        req.url = "/api/settings";
        req.add_header("Host", "127.0.0.1:8080");
        req.add_header("Content-Type", "application/json");
        crow::response res;
        guard.before_handle(req, res, ctx);
        CHECK_FALSE(res.is_completed());
        CHECK(res.code == 200);
    }
    {   // 重绑定进来的 GET：Host 不是回环。
        crow::request req;
        req.method = crow::HTTPMethod::Get;
        req.url = "/api/projects";
        req.add_header("Host", "evil.example:8080");
        crow::response res;
        guard.before_handle(req, res, ctx);
        CHECK(res.code == 403);
    }
}

TEST_CASE("门：引擎和工作进程都用带门的 app，每一条 WebSocket 路由都挂了 onaccept") {
    const std::filesystem::path src{CHANGJI_SRC_DIR};
    for (const char* f : {"http/server.cpp", "infer/worker_server.cpp", "infer/worker_server.hpp"}) {
        CAPTURE(f);
        const std::string s = slurp(src / f);
        REQUIRE_FALSE(s.empty());
        // 光秃秃的 SimpleApp 没有门：引擎、工作进程哪一个用了它，那个端口就又开着了。
        CHECK(s.find("crow::SimpleApp") == std::string::npos);
    }
    const std::string server = slurp(src / "http/server.cpp");
    CHECK(server.find("EngineApp app;") != std::string::npos);
    CHECK(server.find("get_middleware<SameSiteGuard>().policy = guard_policy_for(") != std::string::npos);
    // 中间件的「不认」对 WebSocket 升级不起作用（Crow 丢掉、照样升级）：每一条 WS 路由
    // 都得自己挂 onaccept，挂到下一个分号之前。
    int routes = 0;
    for (auto at = server.find("CROW_WEBSOCKET_ROUTE("); at != std::string::npos;
         at = server.find("CROW_WEBSOCKET_ROUTE(", at + 1)) {
        ++routes;
        const auto end = server.find(';', at);
        CAPTURE(server.substr(at, 60));
        CHECK(server.substr(at, end - at).find(".onaccept(") != std::string::npos);
    }
    CHECK(routes >= 2);
}

TEST_CASE("门：对外监听要口令——一条 curl 接扩展就是在这台机器上起命令（2026-09-26 审出来、真跑过）") {
    const GuardPolicy pub = public_bind();
    // 审查时那一条：不带口令 POST /api/mcp/add，引擎当场起了 /bin/sh。
    CHECK(judge(Req{.path = "/api/mcp/add", .host = "192.0.2.2:8080"}, pub) == Refusal::auth);
    CHECK(judge(Req{.path = "/api/connections", .host = "192.0.2.2:8080"}, pub) == Refusal::auth);
    // 读的也要：项目、对话里什么都有。
    CHECK(judge(Req{.method = "GET", .path = "/api/projects", .ct = "", .host = "192.0.2.2:8080"}, pub) ==
          Refusal::auth);
    CHECK(judge(Req{.method = "GET", .path = "/api/media", .ct = "", .host = "192.0.2.2:8080"}, pub) ==
          Refusal::auth);
    // **界面那几页不要口令了**（2026-09-27）：一包静态文件，没有数据；没进门的浏览器拿到界面，
    // 由界面自己摆登录页（场记账号 / 本机口令）。数据全在上面那些接口后面。
    CHECK(judge(Req{.method = "GET", .path = "/", .ct = "", .host = "192.0.2.2:8080", .site = "none",
                    .mode = "navigate"},
                pub) == Refusal::none);
    CHECK(judge(Req{.method = "GET", .path = "/assets/index-abc.js", .ct = "", .host = "192.0.2.2:8080"}, pub) ==
          Refusal::none);
    // 口令不对、写法不对的都不认。
    for (const char* a : {"Bearer wrong", "Bearer s3cre", "Bearer s3cret2", "s3cret", "Basic s3cret", "Bearer ",
                          "Bearer"}) {
        CAPTURE(a);
        CHECK(judge(Req{.host = "192.0.2.2:8080", .auth = a}, pub) == Refusal::auth);
    }
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "changji_ui=nope"}, pub) == Refusal::auth);
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "other=s3cret"}, pub) == Refusal::auth);
    // 三种带法都认：头（桌面端、别的程序）、cookie（网页）、查询串（播放器）。
    CHECK(judge(Req{.host = "192.0.2.2:8080", .auth = kBearer}, pub) == Refusal::none);
    CHECK(judge(Req{.host = "192.0.2.2:8080", .auth = "bearer   s3cret "}, pub) == Refusal::none);
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "a=1; changji_ui=s3cret; b=2"}, pub) == Refusal::none);
    CHECK(judge(Req{.method = "GET", .path = "/api/media", .ct = "", .host = "192.0.2.2:8080", .qtoken = "s3cret"},
                pub) == Refusal::none);
    // [peer].token 也认（server.cpp 把两个都放进去）。
    GuardPolicy two = pub;
    two.tokens.push_back("peer-tok");
    CHECK(judge(Req{.host = "192.0.2.2:8080", .auth = "Bearer peer-tok"}, two) == Refusal::none);
    // 一条口令都没有：谁都进不去。空串不算口令。
    GuardPolicy none = public_bind();
    none.tokens = {""};
    CHECK(judge(Req{.host = "192.0.2.2:8080", .auth = "Bearer "}, none) == Refusal::auth);
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "changji_ui="}, none) == Refusal::auth);
    // 前四条不认的照旧回它们那一句（口令摆在最后）。
    CHECK(judge(Req{.ct = "text/plain", .host = "192.0.2.2:8080"}, pub) == Refusal::content_type);
    // 回环监听一概不判。
    CHECK(judge(Req{.path = "/api/mcp/add"}) == Refusal::none);
}

TEST_CASE("门：对外监听时自己验票的那几条不归口令管") {
    const GuardPolicy pub = public_bind();
    for (const char* p : {"/task", "/task/7", "/task/7/cancel", "/blob/abc", "/blob/abc/probe", "/setup/state",
                          "/setup/download", "/status", "/health", "/slots/wait", "/api/lan/ticket"}) {
        CAPTURE(p);
        CHECK(http::auth_exempt("GET", p));
        CHECK(judge(Req{.method = "GET", .path = p, .ct = "", .host = "192.0.2.2:8080"}, pub) == Refusal::none);
    }
    for (const char* p : {"/api/system", "/api/lan", "/api/task/thinking", "/api/mcp/add", "/api/cloud",
                          "/api/media", "/bff/settings/llm"}) {
        CAPTURE(p);
        CHECK_FALSE(http::auth_exempt("GET", p));
    }
    // 界面那几页：读的放行，写的不放（它们本来就没有写的路由，放了也是白放一个口子）
    for (const char* p : {"/", "/taskx", "/assets/index.js", "/favicon.ico"}) {
        CAPTURE(p);
        CHECK(http::auth_exempt("GET", p));
        CHECK_FALSE(http::auth_exempt("POST", p));
    }
    // 登录页没进门时要问的那几条（场记云）：只放那几条、只放那一种方法
    CHECK(http::auth_exempt("GET", "/api/cloud/gate"));
    CHECK_FALSE(http::auth_exempt("POST", "/api/cloud/gate"));
    CHECK(http::auth_exempt("POST", "/api/cloud/enter/start"));
    CHECK_FALSE(http::auth_exempt("POST", "/api/cloud/login/start"));   // 把引擎登到账号上：要进门
    CHECK_FALSE(http::auth_exempt("POST", "/api/cloud/logout"));
    CHECK(http::auth_exempt("GET", "/auth/cloud/callback"));
    // 局域网上那几台读负载：带着这台发出去的票。只认那一条、只认读。
    GuardPolicy lan = pub;
    lan.lan_ticket_ok = [](const std::string& t) { return t == "tick"; };
    CHECK(judge(Req{.method = "GET", .path = "/api/system", .ct = "", .host = "192.0.2.2:8080", .ticket = "tick"},
                lan) == Refusal::none);
    CHECK(judge(Req{.method = "GET", .path = "/api/system", .ct = "", .host = "192.0.2.2:8080", .ticket = "nope"},
                lan) == Refusal::auth);
    CHECK(judge(Req{.method = "GET", .path = "/api/projects", .ct = "", .host = "192.0.2.2:8080", .ticket = "tick"},
                lan) == Refusal::auth);
    CHECK(judge(Req{.path = "/api/system", .host = "192.0.2.2:8080", .ticket = "tick"}, lan) == Refusal::auth);
    CHECK(judge(Req{.method = "GET", .path = "/api/system", .ct = "", .host = "192.0.2.2:8080", .ticket = "tick"},
                pub) == Refusal::auth);   // 没接上判票的函数：不认
}

TEST_CASE("门：对外监听时 WebSocket 也要口令（推送里有对话、正文、项目路径）") {
    const auto up = [](std::string auth, std::string cookie) {
        const RequestFacts f{"GET", "/api/ws", "", "", "192.0.2.2:8080", "", "websocket", auth, cookie};
        return http::judge_upgrade(f, public_bind()).refusal;
    };
    CHECK(up("", "") == Refusal::auth);
    CHECK(up(kBearer, "") == Refusal::none);
    CHECK(up("", "changji_ui=s3cret") == Refusal::none);
    CHECK(http::refusal_status({Refusal::auth, ""}) == 401);
    CHECK(http::refusal_message({Refusal::auth, ""}).find("token") != std::string::npos);
}

TEST_CASE("门：cookie 头里取一个值") {
    CHECK(http::cookie_value("changji_ui=abc", "changji_ui") == "abc");
    CHECK(http::cookie_value("a=1; changji_ui=abc ; b=2", "changji_ui") == "abc");
    CHECK(http::cookie_value("a=1;changji_ui=\"abc\"", "changji_ui") == "abc");
    CHECK(http::cookie_value("xchangji_ui=abc", "changji_ui").empty());
    CHECK(http::cookie_value("", "changji_ui").empty());
    CHECK(http::cookie_value("changji_ui", "changji_ui").empty());
}

TEST_CASE("门：网页登录——带对的口令打开种 cookie、跳回不带口令的地址；没带回登录页") {
    http::SameSiteGuard guard;
    guard.policy = public_bind();
    http::SameSiteGuard::context ctx;
    const auto get = [&](const std::string& url, const std::string& query) {
        crow::request req;
        req.method = crow::HTTPMethod::Get;
        req.url = url;
        req.url_params = crow::query_string("?" + query);
        req.add_header("Host", "192.0.2.2:8080");
        crow::response res;
        guard.before_handle(req, res, ctx);
        return res;
    };
    {
        auto res = get("/project", "token=s3cret");
        CHECK(res.is_completed());
        CHECK(res.code == 303);
        CHECK(res.get_header_value("Location") == "/project");
        const std::string c = res.get_header_value("Set-Cookie");
        CHECK(c.find("changji_ui=s3cret") == 0);
        CHECK(c.find("HttpOnly") != std::string::npos);
        CHECK(c.find("SameSite=Strict") != std::string::npos);
    }
    {   // 只跳站内：`//evil.example` 在浏览器眼里是别的网站。
        auto res = get("//evil.example", "token=s3cret");
        CHECK(res.code == 303);
        CHECK(res.get_header_value("Location") == "/");
    }
    {   // 口令不对：不种 cookie、不跳；界面照常给（界面自己问 /api/cloud/gate 还没进门，摆登录页、说口令不对）。
        auto res = get("/", "token=wrong");
        CHECK_FALSE(res.is_completed());
        CHECK(res.get_header_value("Set-Cookie").empty());
    }
    {   // 没带：界面那几页照常给（2026-09-27 起由界面摆登录页），接口回 401 JSON。
        auto page = get("/", "");
        CHECK_FALSE(page.is_completed());
        auto api = get("/api/projects", "");
        CHECK(api.code == 401);
        CHECK(api.body.find("detail") != std::string::npos);
        // 接口上带 ?token= 不种 cookie、不跳，直接放行（播放器那种）。
        auto media = get("/api/media", "token=s3cret");
        CHECK_FALSE(media.is_completed());
    }
    {   // 桌面端带头的 POST：放行。
        crow::request req;
        req.method = crow::HTTPMethod::Post;
        req.url = "/api/mcp/add";
        req.add_header("Host", "192.0.2.2:8080");
        req.add_header("Content-Type", "application/json");
        req.add_header("Authorization", kBearer);
        crow::response res;
        guard.before_handle(req, res, ctx);
        CHECK_FALSE(res.is_completed());
    }
}

TEST_CASE("门：没口令那句话每一种语言里都留着 ?token=（网页靠它认出这道门、整页重开去填口令）") {
    // webapp/client/src/api/index.js 的 isLoginWall 认的就是这几个字。
    const std::string zh = "这台引擎对外监听，要口令。浏览器里在地址后面加 ?token=口令 打开一次；"
                           "别的程序带 Authorization: Bearer 口令。口令在引擎启动时的日志里";
    int langs = 0;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path{CHANGJI_SRC_DIR} / ".." / "i18n")) {
        const std::string name = e.path().filename().string();
        if (name.rfind("engine_", 0) != 0 || e.path().extension() != ".json") continue;
        const auto j = nlohmann::json::parse(slurp(e.path()));
        CAPTURE(name);
        REQUIRE(j.contains(zh));
        CHECK(j[zh].get<std::string>().find("?token=") != std::string::npos);
        ++langs;
    }
    CHECK(langs >= 10);
    const std::string web = slurp(std::filesystem::path{CHANGJI_SRC_DIR} / ".." / ".." / "webapp/client/src/api/index.js");
    CHECK(web.find("includes('?token=')") != std::string::npos);
}

TEST_CASE("门：这台的口令只有一个——配置里的排第一，空着才轮到环境变量、旧文件、现生") {
    const auto dir = std::filesystem::temp_directory_path() / "changji_ui_token_test";
    std::filesystem::remove_all(dir);
    const std::string old = paths::env("CHANGJI_UI_TOKEN");
    paths::set_env("CHANGJI_UI_TOKEN", "");
    // 配置里空着、也没有旧文件：现生一个，先落在 ui_token 里（搬进配置是调用方的事）。
    const auto a = http::resolve_machine_token("", dir);
    CHECK(a.from == "new");
    CHECK(a.value.size() == 32);
    CHECK(a.value.find_first_not_of("0123456789abcdef") == std::string::npos);
#ifndef _WIN32
    const auto perms = std::filesystem::status(dir / "ui_token").permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
#endif
    // 搬不进配置的话下次起来还是它，不换（书签、种好的 cookie、别的机器表上那一行都接着能用）。
    const auto b = http::resolve_machine_token("", dir);
    CHECK(b.from == "file");
    CHECK(b.value == a.value);
    // 被手改成太短的：不认，重生一个。
    { std::ofstream(dir / "ui_token") << "123\n"; }
    const auto c = http::resolve_machine_token("", dir);
    CHECK(c.from == "new");
    CHECK(c.value != "123");
    // 环境变量排在旧文件前面……
    paths::set_env("CHANGJI_UI_TOKEN", " from-env ");
    const auto d = http::resolve_machine_token("", dir);
    CHECK(d.from == "env");
    CHECK(d.value == "from-env");
    // ……而配置里那个排在所有前面：设置页改的是它，改了就得算数。
    const auto e = http::resolve_machine_token(" from-config ", dir);
    CHECK(e.from == "config");
    CHECK(e.value == "from-config");
    paths::set_env("CHANGJI_UI_TOKEN", old);
    std::filesystem::remove_all(dir);
}

TEST_CASE("门：设置页填的口令——至少 16 个字，只用放进 cookie、查询串、TOML 都不用转义的字") {
    CHECK(http::token_problem("0123456789abcdef").empty());
    CHECK(http::token_problem("My-home.token_2026~x").empty());
    CHECK(http::token_problem(http::fresh_token()).empty());
    CHECK_FALSE(http::token_problem("").empty());
    CHECK_FALSE(http::token_problem("short-token").empty());
    for (const char* bad : {"0123456789abcdef ", "0123456789abcdef;", "0123456789abcdef\"", "0123456789abc=ef",
                            "0123456789abcde&f", "0123456789abcde%f", "口令口令口令口令口令口令"}) {
        CAPTURE(bad);
        CHECK_FALSE(http::token_problem(bad).empty());
    }
    CHECK(http::fresh_token() != http::fresh_token());
}

TEST_CASE("门：口令改了当场生效——门每条请求现问，旧的立刻不认") {
    std::string now = "old-token-0000000";
    GuardPolicy pol;
    pol.loopback_bind = false;
    pol.live_token = [&now] { return now; };
    const auto with = [&](const std::string& t) {
        return judge(Req{.host = "192.0.2.2:8080", .auth = "Bearer " + t}, pol);
    };
    CHECK(with("old-token-0000000") == Refusal::none);
    now = "new-token-1111111";
    CHECK(with("old-token-0000000") == Refusal::auth);
    CHECK(with("new-token-1111111") == Refusal::none);
    // cookie、查询串同一个判据。
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "changji_ui=new-token-1111111"}, pol) == Refusal::none);
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "changji_ui=old-token-0000000"}, pol) == Refusal::auth);
    // 现问回空串（还没定下来）：谁都不认，不是谁都认。
    now.clear();
    CHECK(with("") == Refusal::auth);
    CHECK(judge(Req{.host = "192.0.2.2:8080", .cookie = "changji_ui="}, pol) == Refusal::auth);
}

TEST_CASE("门：引擎对外监听时真把口令接上了，而且各处都现问") {
    const std::string server = slurp(std::filesystem::path{CHANGJI_SRC_DIR} / "http/server.cpp");
    CHECK(server.find("settle_machine_token(settings, opts.host)") != std::string::npos);
    CHECK(server.find("pol.live_token = [] { return config::runtime().peer_token(); }") != std::string::npos);
    // 起服务时抄一份捕进门里、捕进用账号进门种的 cookie 里——改完口令旧的照样进得来。
    CHECK(server.find("pol.tokens = {") == std::string::npos);
    CHECK(server.find("cloud_policy.tokens.front()") == std::string::npos);
    // 派活那道门同理：认的是现在的，不是起服务那一刻 settings 里那份。
    const std::string worker = slurp(std::filesystem::path{CHANGJI_SRC_DIR} / "infer/worker_server.cpp");
    CHECK(worker.find("config::runtime().peer_token()") != std::string::npos);
    CHECK(worker.find("token_ok(req.get_header_value(\"Authorization\"), settings.peer.token)") == std::string::npos);
}

TEST_CASE("监听：局域网地址只认那三段") {
    CHECK(http::is_lan_ipv4(192, 168));
    CHECK(http::is_lan_ipv4(10, 0));
    CHECK(http::is_lan_ipv4(10, 255));
    CHECK(http::is_lan_ipv4(172, 16));
    CHECK(http::is_lan_ipv4(172, 31));
    CHECK_FALSE(http::is_lan_ipv4(172, 15));
    CHECK_FALSE(http::is_lan_ipv4(172, 32));
    CHECK_FALSE(http::is_lan_ipv4(198, 18));    // 代理软件的虚拟网卡
    CHECK_FALSE(http::is_lan_ipv4(169, 254));   // 没拿到地址时系统自己给的
    CHECK_FALSE(http::is_lan_ipv4(127, 0));
    CHECK_FALSE(http::is_lan_ipv4(8, 8));
    for (const auto& ip : http::lan_ipv4s()) {
        CAPTURE(ip);
        CHECK(ip.find('.') != std::string::npos);
    }
    CHECK(http::lan_ipv4s().size() <= 3);
}

TEST_CASE("监听：回环上有人在听的端口不算空着（macOS 上 0.0.0.0 和别人的 127.0.0.1 能同时绑上）") {
    asio::io_context io;
    asio::ip::tcp::acceptor acc(io);
    const asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    acc.open(ep.protocol());
    acc.bind(ep);
    const int port = static_cast<int>(acc.local_endpoint().port());
    // 别人在 127.0.0.1 上听着：不管 0.0.0.0 那头绑不绑得上，都算占着。
    acc.listen();
    CHECK_FALSE(http::port_usable("0.0.0.0", port));
    CHECK_FALSE(http::port_usable("127.0.0.1", port));
    acc.close();
    CHECK_FALSE(http::port_usable("0.0.0.0", 0));
    CHECK_FALSE(http::port_usable("0.0.0.0", 70000));
}

TEST_CASE("监听：起来之后终端上那一块——对外监听时每个地址都带口令，点开就能进") {
    const std::vector<std::string> lan = {"192.168.20.239", "172.17.0.1"};
    const std::string pub = http::ready_banner("0.0.0.0", 8080, "tok-1234567890abcdef", lan);
    CHECK(pub.find("http://127.0.0.1:8080/?token=tok-1234567890abcdef") != std::string::npos);
    CHECK(pub.find("http://192.168.20.239:8080/?token=tok-1234567890abcdef") != std::string::npos);
    CHECK(pub.find("http://172.17.0.1:8080/?token=tok-1234567890abcdef") != std::string::npos);
    // 口令单独再说一遍（要抄到别的机器上「加一台」），而且不许是占位符。
    CHECK(pub.find("tok-1234567890abcdef（") != std::string::npos);
    CHECK(pub.find("<") == std::string::npos);
    // 云主机那种问不出局域网地址的：本机那条之外说一句怎么换。
    const std::string cloud = http::ready_banner("0.0.0.0", 8080, "tok-1234567890abcdef", {});
    CHECK(cloud.find("http://127.0.0.1:8080/?token=") != std::string::npos);
    CHECK(cloud.find("127.0.0.1 换成") != std::string::npos);
    // 听某一个具体地址：只摆那一个。
    const std::string one = http::ready_banner("192.168.1.5", 9000, "tok-1234567890abcdef", lan);
    CHECK(one.find("http://192.168.1.5:9000/?token=") != std::string::npos);
    CHECK(one.find("127.0.0.1") == std::string::npos);
    CHECK(one.find("192.168.20.239") == std::string::npos);
    // 只听本机：不要口令（地址上也不挂），说清怎么让别的电脑也打得开。
    const std::string loop = http::ready_banner("127.0.0.1", 8080, "tok-1234567890abcdef", lan);
    CHECK(loop.find("http://127.0.0.1:8080/\n") != std::string::npos);
    CHECK(loop.find("token") == std::string::npos);
    CHECK(loop.find("--host 0.0.0.0") != std::string::npos);
    CHECK(loop.find("192.168.20.239") == std::string::npos);
}

TEST_CASE("监听：那一块等服务真起来了才打，不靠 wait_for_server_start") {
    // bind 失败时 Crow 根本不通知，wait_for_server_start 永远等下去；先打「起来了」再报端口
    // 被占也不行。挂在 tick 上：只有服务真转起来才走。
    const std::string server = slurp(std::filesystem::path{CHANGJI_SRC_DIR} / "http/server.cpp");
    const auto tick = server.find("app.tick(");
    const auto banner = server.find("ready_banner(opts.host, opts.port");
    REQUIRE(tick != std::string::npos);
    REQUIRE(banner != std::string::npos);
    CHECK(tick < banner);
    CHECK(server.find("wait_for_server_start") == server.rfind("wait_for_server_start"));   // 只在注释里提一次
    // 旧的那一条夹在日志里的 WARNING 不再打。
    CHECK(server.find("网页端口令：%1") == std::string::npos);
    // Windows 上 Crow 的 SO_REUSEADDR 让第二个引擎也绑得上同一个端口、照样说「起来了」：
    // run() 头一件事就是问端口有没有人在听，**在起任何线程之前**。
    const auto run_at = server.find("void run(const config::Settings& settings_in, const Options& opts) {");
    const auto check_at = server.find("if (!port_usable(opts.host, opts.port))");
    const auto pump_at = server.find("PumpAtExit");
    REQUIRE(run_at != std::string::npos);
    REQUIRE(check_at != std::string::npos);
    CHECK(run_at < check_at);
    CHECK(check_at < server.find("settle_machine_token(settings, opts.host)"));
    CHECK(check_at < pump_at);
}

TEST_CASE("监听：没人用的端口算空着") {
    // 要一个系统刚给的、从没人连过的端口（没有 TIME_WAIT 之类的尾巴）。
    int port = 0;
    {
        asio::io_context io;
        asio::ip::tcp::acceptor acc(io);
        const asio::ip::tcp::endpoint ep(asio::ip::make_address("0.0.0.0"), 0);
        acc.open(ep.protocol());
        acc.bind(ep);
        port = static_cast<int>(acc.local_endpoint().port());
    }
    CHECK(http::port_usable("0.0.0.0", port));
}

TEST_CASE("请求体取栏：类型不对回 400 说清哪一栏，没给用默认") {
    using changji::http::ApiError;
    const nlohmann::json body = {{"s", "x"}, {"n", 3}, {"b", true}, {"z", nullptr}, {"f", 1.5}};
    CHECK(changji::http::field_str(body, "s") == "x");
    CHECK(changji::http::field_str(body, "missing", "d") == "d");
    CHECK(changji::http::field_str(body, "z", "d") == "d");
    CHECK(changji::http::field_bool(body, "b", false));
    CHECK(changji::http::field_int(body, "n", 0) == 3);
    CHECK(changji::http::field_num(body, "f", 0.0) == doctest::Approx(1.5));
    CHECK(changji::http::field_str(nlohmann::json::array(), "s", "d") == "d");
    for (const auto& f : std::vector<std::function<void()>>{
             [&] { (void)changji::http::field_str(body, "n"); },
             [&] { (void)changji::http::field_bool(body, "s", false); },
             [&] { (void)changji::http::field_int(body, "f", 0); },
             [&] { (void)changji::http::field_num(body, "s", 0.0); }}) {
        try {
            f();
            FAIL("应该 400");
        } catch (const ApiError& e) {
            CHECK(e.status() == 400);
        }
    }
}
