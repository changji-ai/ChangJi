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

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "http/crow_guard.hpp"
#include "http/request_guard.hpp"
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
    return p;
}

struct Req {
    std::string method = "POST";
    std::string path = "/api/settings";
    std::string ct = "application/json";
    std::string origin;
    std::string host = "127.0.0.1:8080";
    std::string site;
    std::string mode;
    RequestFacts facts() const { return {method, path, ct, origin, host, site, mode}; }
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
    // 网页端传参考图、声音：multipart，只在那三条上认。
    for (const char* up : {"/api/character/reference", "/api/location/reference", "/api/character/voice"}) {
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
    // 放错地方的：multipart 不在传文件那三条上、octet-stream 不在 /blob 上。
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
    // 传文件那三条也判 Origin：它们没法靠 Content-Type 挡。
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
    CHECK(judge(Req{.origin = "http://43.110.148.239:8080", .host = host, .site = "same-origin"}, public_bind()) ==
          Refusal::none);
    CHECK(judge(Req{.origin = "http://43.110.148.239:3000", .host = host}, public_bind()) == Refusal::origin);
    CHECK(judge(Req{.origin = "http://43.110.148.239", .host = "43.110.148.239"}, public_bind()) == Refusal::none);
    // SSH 隧道开对外监听的那台（AutoDL）：localhost 两边同端口。
    CHECK(judge(Req{.origin = "http://localhost:8080", .host = "localhost:8080", .site = "same-origin"},
                public_bind()) == Refusal::none);
    // 反向代理后面的域名：不在名单里不认，写进名单就认。
    CHECK(judge(Req{.origin = "https://films.example.com", .host = "films.example.com", .site = "same-origin"},
                public_bind()) == Refusal::host);
    GuardPolicy named = public_bind();
    named.allowed_hosts = {"films.example.com"};
    CHECK(judge(Req{.origin = "https://films.example.com", .host = "films.example.com", .site = "same-origin"},
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
