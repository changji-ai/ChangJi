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

/// 三条传文件的路由（webapp 的 FormData）：只有它们认 multipart。
bool is_upload_route(std::string_view path) {
    return path == "/api/character/reference" || path == "/api/location/reference" ||
           path == "/api/character/voice";
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

}  // namespace

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
    if (m == "get" || m == "head" || m == "options") return {};

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
    return {};
}

int refusal_status(const GuardVerdict& v) {
    switch (v.refusal) {
        case Refusal::host:
        case Refusal::origin: return 403;
        case Refusal::content_type: return 415;
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
        case Refusal::none: break;
    }
    return {};
}

}  // namespace changji::http
