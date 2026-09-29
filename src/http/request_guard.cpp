#include "http/request_guard.hpp"

#include <algorithm>
#include <cctype>

#include "infer/peer_auth.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::http {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

/// 四条传文件的路由（webapp 的 FormData）：只有它们认 multipart。
bool is_upload_route(std::string_view path) {
    return path == "/api/character/reference" || path == "/api/location/reference" ||
           path == "/api/character/voice" || path == "/api/chat/upload";
}

/// 别的引擎送参考图、产物（`infer/worker_pool.cpp`）：只有它认 octet-stream。
bool is_blob_route(std::string_view path) { return starts_with(path, "/blob/"); }

bool allowed(std::string_view host, const GuardPolicy& policy) {
    return std::find(policy.allowed_hosts.begin(), policy.allowed_hosts.end(), host) !=
           policy.allowed_hosts.end();
}

/// `host:port` 里的端口；没写就按 scheme 的默认（http 80、https 443）。
std::string port_of(std::string_view authority, std::string_view scheme) {
    std::string a = lower(trim(authority));
    if (const auto at = a.find("://"); at != std::string::npos) a.erase(0, at + 3);
    if (const auto slash = a.find_first_of("/?#"); slash != std::string::npos) a.erase(slash);
    std::size_t colon = std::string::npos;
    if (!a.empty() && a.front() == '[') {
        const auto close = a.find(']');
        if (close != std::string::npos && close + 1 < a.size() && a[close + 1] == ':') colon = close + 1;
    } else {
        colon = a.rfind(':');
    }
    if (colon != std::string::npos && colon + 1 < a.size()) return a.substr(colon + 1);
    return scheme == "https" ? "443" : "80";
}

/// 这是浏览器发的（带 `Sec-Fetch-*`——那几个头网页改不了，httplib、curl、Qt 不带）。
bool from_browser(const RequestFacts& r) {
    return !trim(r.fetch_site).empty() || !trim(r.fetch_mode).empty();
}

/// Origin 认不认。**不带的由调用方放行**，这儿只判带了的。
bool origin_ok(std::string_view origin, std::string_view host, const GuardPolicy& policy) {
    const std::string o = lower(trim(origin));
    if (o == "null") return false;   // 沙盒 iframe、本地文件、跨站跳转之后：一律不认
    const auto sep = o.find("://");
    if (sep == std::string::npos) return false;
    const std::string scheme = o.substr(0, sep);
    if (scheme != "http" && scheme != "https") return false;
    // Origin 只有 scheme://host[:port]，带了路径、查询串、用户名的就是伪造或者认不出。
    const std::string rest = o.substr(sep + 3);
    if (rest.empty() || rest.find_first_of("/?#@") != std::string::npos) return false;
    const std::string oh = host_of(o);
    if (oh.empty()) return false;
    if (allowed(oh, policy)) return true;
    const std::string hh = host_of(host);
    const bool same_port = port_of(o, scheme) == port_of(host, scheme);
    // 回环：Host 也得是回环、**端口也得一样**——本机别的端口上跑着的网页（另一个开发
    // 服务器、笔记本）不算自己人。引擎自己的页面、SSH 隧道（localhost:9000 两边一样）、
    // Vite（它把同源的请求改成引擎自己的 Origin，见 webapp/client/vite.config.js）都过。
    if (is_loopback_host(oh)) return is_loopback_host(hh) && same_port;
    // 远端那台上直接开网页端（http://<ip>:8080）：同一个 IP、同一个端口。只认 IP——
    // 域名的话重绑定正好就是「两边同一个名字」。
    return is_ip_literal(oh) && oh == hh && same_port;
}

bool host_ok(const RequestFacts& r, const GuardPolicy& policy) {
    // 不带 Host 的只有 HTTP/1.0（1.1 不带 Host Crow 直接回 400）——浏览器不会这样。
    if (trim(r.host).empty()) return true;
    const std::string h = host_of(r.host);
    if (is_loopback_host(h) || allowed(h, policy)) return true;
    if (policy.loopback_bind) return false;
    // 对外监听：IP 都认；域名只有**浏览器发来的**才判——别的引擎可能按域名配的
    // （`[[peer.nodes]]` 里写着 gpu.example.com），它们不带 Sec-Fetch-*。
    return is_ip_literal(h) || !from_browser(r);
}

/// `Sec-Fetch-Site` 那一条：跨站、同站（别的端口、别的子域）发来的只认「跳转到界面」。
bool fetch_site_ok(const RequestFacts& r) {
    const std::string site = lower(trim(r.fetch_site));
    if (site.empty() || site == "same-origin" || site == "none") return true;
    // cross-site / same-site：只有跳转到界面那几页（别人给了个链接、书签）。跳到接口上
    // 不认——`location = ".../api/nodes/setup?url=..."` 一样能触发副作用。
    return lower(trim(r.fetch_mode)) == "navigate" && !is_api_path(r.path);
}

bool ct_equal(std::string_view a, std::string_view b) {
    // 长度本来就藏不住；等长时逐字节比完再回，不提前短路。
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

/// 第五条：对外监听时带没带着认的口令（或者是自己验票的那几条）。
bool authed(const RequestFacts& r, const GuardPolicy& policy) {
    if (policy.loopback_bind) return true;
    if (auth_exempt(r.method, r.path)) return true;
    const std::string m = lower(trim(r.method));
    // 局域网上那几台读负载（`lan/sense.cpp`）：带着这台发出去的票。
    if ((m == "get" || m == "head") && r.path == "/api/system" && !trim(r.lan_ticket).empty() &&
        policy.lan_ticket_ok && policy.lan_ticket_ok(std::string(trim(r.lan_ticket)))) {
        return true;
    }
    const std::string_view a = trim(r.authorization);
    if (a.size() > 7 && lower(a.substr(0, 7)) == "bearer " && token_matches(trim(a.substr(7)), policy)) {
        return true;
    }
    if (token_matches(cookie_value(r.cookie, kUiCookie), policy)) return true;
    return token_matches(r.query_token, policy);
}

}  // namespace

std::string cookie_value(std::string_view header, std::string_view name) {
    std::size_t i = 0;
    while (i <= header.size()) {
        const auto semi = header.find(';', i);
        const std::string_view part =
            trim(header.substr(i, semi == std::string_view::npos ? std::string_view::npos : semi - i));
        const auto eq = part.find('=');
        if (eq != std::string_view::npos && trim(part.substr(0, eq)) == name) {
            std::string_view v = trim(part.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return std::string(v);
        }
        if (semi == std::string_view::npos) break;
        i = semi + 1;
    }
    return {};
}

bool token_matches(std::string_view got, const GuardPolicy& policy) {
    got = trim(got);
    if (got.empty()) return false;
    bool hit = false;
    // 每一条都比完：哪一条对上了不从耗时上漏出来。
    for (const auto& t : policy.tokens) {
        if (t.empty()) continue;
        hit = ct_equal(got, t) || hit;
    }
    if (policy.live_token) {
        const std::string t = policy.live_token();
        if (!t.empty()) hit = ct_equal(got, t) || hit;
    }
    return hit;
}

bool auth_exempt(std::string_view method, std::string_view path) {
    if (path == "/task" || path == "/status" || path == "/health" || path == "/api/lan/ticket") return true;
    for (const std::string_view p : {"/task/", "/blob/", "/setup/", "/slots/"}) {
        if (starts_with(path, p)) return true;
    }
    const std::string m = lower(trim(method));
    // **界面那几页（一包静态文件）不要口令**（2026-09-27，docs/账号与云服务方案.md）：那里面没有
    // 任何数据，数据都在接口后面、接口照旧要口令。放行之后没进门的浏览器也拿得到界面，由界面
    // 自己摆登录页（场记账号 / 本机口令），不再是一张只有一个输入框的光秃秃的页。
    if ((m == "get" || m == "head") && !is_api_path(path)) return true;
    // 登录页没进门时要问的那几条：进没进门、这台绑没绑账号；用场记账号进门那一趟的起点和回调。
    if ((m == "get" || m == "head") && path == "/api/cloud/gate") return true;
    if (m == "post" && path == "/api/cloud/enter/start") return true;
    return path == "/auth/cloud/callback";
}

bool is_authed(const RequestFacts& r, const GuardPolicy& policy) {
    if (policy.loopback_bind) return true;
    const std::string_view a = trim(r.authorization);
    if (a.size() > 7 && lower(a.substr(0, 7)) == "bearer " && token_matches(trim(a.substr(7)), policy)) {
        return true;
    }
    if (token_matches(cookie_value(r.cookie, kUiCookie), policy)) return true;
    return token_matches(r.query_token, policy);
}

std::string mime_essence(std::string_view content_type) {
    std::string_view s = content_type;
    if (const auto semi = s.find(';'); semi != std::string_view::npos) s = s.substr(0, semi);
    return lower(trim(s));
}

bool is_simple_content_type(std::string_view content_type) {
    const std::string e = mime_essence(content_type);
    return e.empty() || e == "text/plain" || e == "application/x-www-form-urlencoded" ||
           e == "multipart/form-data";
}

std::string host_of(std::string_view origin_or_host) {
    std::string s = lower(trim(origin_or_host));
    if (const auto at = s.find("://"); at != std::string::npos) s.erase(0, at + 3);
    if (const auto slash = s.find_first_of("/?#"); slash != std::string::npos) s.erase(slash);
    if (!s.empty() && s.front() == '[') {
        const auto close = s.find(']');
        return close == std::string::npos ? s : s.substr(0, close + 1);
    }
    if (const auto colon = s.find(':'); colon != std::string::npos) s.erase(colon);
    // 末尾那个点（`localhost.`）是同一个名字。
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool is_ip_literal(std::string_view host) {
    const std::string h = lower(trim(host));
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') {
        const std::string in = h.substr(1, h.size() - 2);
        return !in.empty() && in.find(':') != std::string::npos &&
               std::all_of(in.begin(), in.end(), [](char c) {
                   return std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.';
               });
    }
    int parts = 0;
    std::size_t start = 0;
    for (;;) {
        const auto dot = h.find('.', start);
        const std::string part = h.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!all_digits(part) || part.size() > 3 || std::stoi(part) > 255) return false;
        ++parts;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts == 4;
}

bool is_loopback_host(std::string_view host) {
    const std::string h = lower(trim(host));
    if (h == "localhost" || h == "[::1]" || h == "::1") return true;
    // `*.localhost`：浏览器不去问 DNS、直接当回环（RFC 6761），谁也注册不了这个后缀。
    if (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0) return true;
    return h.rfind("127.", 0) == 0 && is_ip_literal(h);
}

bool is_api_path(std::string_view path) {
    for (const std::string_view p : {"/api/", "/bff/", "/blob/", "/setup/", "/task/"}) {
        if (starts_with(path, p)) return true;
    }
    return path == "/api" || path == "/bff" || path == "/ws" || path == "/task" || path == "/status" ||
           path == "/health";
}

GuardPolicy guard_policy_for(const std::string& bind_host) {
    GuardPolicy p;
    p.loopback_bind = !infer::is_public_bind(bind_host);
    const std::string raw = paths::env("CHANGJI_ALLOWED_HOSTS");
    std::string one;
    const auto flush = [&] {
        std::string e(trim(one));
        one.clear();
        // 光着写的 IPv6（`fe80::1`）：套上方括号再认，不然会被当成「主机:端口」切掉。
        if (std::count(e.begin(), e.end(), ':') >= 2 && e.find('[') == std::string::npos &&
            e.find("://") == std::string::npos) {
            e = "[" + e + "]";
        }
        const std::string h = host_of(e);
        if (!h.empty()) p.allowed_hosts.push_back(h);
    };
    for (const char c : raw) {
        if (c == ',' || c == ' ' || c == ';' || c == '\t') {
            flush();
        } else {
            one += c;
        }
    }
    flush();
    return p;
}

GuardVerdict judge_upgrade(const RequestFacts& r, const GuardPolicy& policy) {
    if (!host_ok(r, policy)) return {Refusal::host, std::string(trim(r.host))};
    if (!fetch_site_ok(RequestFacts{r.method, "/ws", r.content_type, r.origin, r.host, r.fetch_site, ""})) {
        return {Refusal::origin, std::string(trim(r.fetch_site))};
    }
    if (!trim(r.origin).empty() && !origin_ok(r.origin, r.host, policy)) {
        return {Refusal::origin, std::string(trim(r.origin))};
    }
    // 推送里有对话、正文、项目路径：对外监听时同样要口令（网页带着 cookie，桌面端带头）。
    if (!authed(r, policy)) return {Refusal::auth, {}};
    return {};
}

GuardVerdict judge_request(const RequestFacts& r, const GuardPolicy& policy) {
    // 一，Host（**哪个方法都判**：重绑定之后那个页面连 GET 的回包都读得到）。
    if (!host_ok(r, policy)) return {Refusal::host, std::string(trim(r.host))};

    // 二，Sec-Fetch-Site（哪个方法都判：`<img>`、跳转这种 GET 不带 Origin，只有它挡得住）。
    if (!fetch_site_ok(r)) return {Refusal::origin, std::string(trim(r.fetch_site))};

    const std::string m = lower(trim(r.method));
    // 读的那几种不判 Origin、Content-Type：浏览器发 `<img>`、跳转这种 GET 本来就不带
    // Origin（上面那条已经挡了跨站的）。有副作用的 GET 是那条路由自己的毛病。
    if (m == "get" || m == "head" || m == "options") {
        if (!authed(r, policy)) return {Refusal::auth, {}};
        return {};
    }

    // 三，Origin。
    if (!trim(r.origin).empty() && !origin_ok(r.origin, r.host, policy)) {
        return {Refusal::origin, std::string(trim(r.origin))};
    }

    // 四，Content-Type：会带请求体的那几种，**只认名单上的**。DELETE 本来就要预检，不判
    // （桌面端删对话不带 Content-Type）。
    if (m == "post" || m == "put" || m == "patch") {
        const std::string e = mime_essence(r.content_type);
        const bool ok = e == "application/json" ||
                        (e == "multipart/form-data" && is_upload_route(r.path)) ||
                        (e == "application/octet-stream" && is_blob_route(r.path));
        if (!ok) return {Refusal::content_type, std::string(trim(r.content_type))};
    }
    // 五，口令（只在对外监听时）。摆在最后：前四条不认的照旧回它们那一句。
    if (!authed(r, policy)) return {Refusal::auth, {}};
    return {};
}

int refusal_status(const GuardVerdict& v) {
    switch (v.refusal) {
        case Refusal::host:
        case Refusal::origin: return 403;
        case Refusal::content_type: return 415;
        case Refusal::auth: return 401;
        case Refusal::none: break;
    }
    return 200;
}

std::string refusal_message(const GuardVerdict& v) {
    switch (v.refusal) {
        case Refusal::host:
            return SAYF("这台引擎只认本机的主机名，不认「%1」。要用自己的域名，写进环境变量 CHANGJI_ALLOWED_HOSTS",
                        v.what);
        case Refusal::origin:
            return SAYF("这条接口不收别的网站发来的请求（%1）。要从自己的域名打开，写进环境变量 CHANGJI_ALLOWED_HOSTS",
                        v.what);
        case Refusal::content_type: return SAY("这条接口只收 JSON");
        case Refusal::auth:
            return SAY("这台引擎对外监听，要口令。浏览器里在地址后面加 ?token=口令 打开一次；"
                       "别的程序带 Authorization: Bearer 口令。口令在引擎启动时的日志里");
        case Refusal::none: break;
    }
    return {};
}

std::string login_page() {
    // 原生的一个表单：GET 回到根上、带着 token——引擎认了就种 cookie、跳回去。
    // 不带脚本：这一页出现在"还没进门"的时候，越少越好。
    std::string html;
    html += "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<meta name=\"color-scheme\" content=\"light dark\"><title>";
    html += SAY("场记 · 要口令");
    html += "</title><style>body{font:16px system-ui,sans-serif;max-width:28rem;margin:12vh auto;"
            "padding:0 16px;line-height:1.6}input{font:inherit;width:100%;box-sizing:border-box;"
            "padding:.5rem;margin:.5rem 0}button{font:inherit;padding:.5rem 1.2rem}</style></head><body><h1>";
    html += SAY("场记 · 要口令");
    html += "</h1><p>";
    html += SAY("这台引擎对外监听，进来要口令。口令在引擎启动时的日志里（「网页端口令」那一行），"
                "也可以是配置里的 [peer].token。");
    html += "</p><form method=\"get\" action=\"/\"><label for=\"t\">";
    html += SAY("口令");
    html += "</label><input id=\"t\" name=\"token\" type=\"password\" autocomplete=\"current-password\" "
            "required autofocus><button type=\"submit\">";
    html += SAY("进去");
    html += "</button></form></body></html>";
    return html;
}

}  // namespace changji::http
