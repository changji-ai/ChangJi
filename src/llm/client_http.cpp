// 真实的 HTTP 实现。**只有这个文件 include httplib。**
//
// 分出来是为了让 client.cpp 能进单元测试目标而不用链 httplib——
// 请求怎么拼、错误怎么翻成人话、返回怎么抽内容，那三件事全是纯逻辑。

#include "util/httplib.hpp"
#include "util/net_inward.hpp"
#include "util/say.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "http/llm_info.hpp"
#include "llm/client.hpp"

namespace changji::llm {

namespace {

/// 把 base_url 拆成 "scheme://host:port" 和路径前缀两半。
///
/// httplib 的 Client 要单独的 host 和 path，而配置里给的是
/// "http://127.0.0.1:11434/v1" 这样一整条。
std::pair<std::string, std::string> split_base(const std::string& url) {
    const std::size_t scheme_end = url.find("://");
    const std::size_t host_start =
        scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const std::size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos) return {url, "/"};
    return {url.substr(0, path_start), url.substr(path_start)};
}

/// 响应头键一律小写（见 `HttpResponse::headers`）。同名的几条只留最后一条。
std::map<std::string, std::string> lower_headers(const httplib::Headers& h) {
    std::map<std::string, std::string> out;
    for (const auto& kv : h) {
        std::string k = kv.first;
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out[k] = kv.second;
    }
    return out;
}

}  // namespace

HttpPost default_http_post() {
    return [](const std::string& url, const std::string& body,
              const std::map<std::string, std::string>& headers,
              double timeout_s) -> HttpResponse {
        const auto [origin, path] = split_base(url);

        httplib::Client cli(origin);
        // 三个超时都要设。只设 read 的话，连不上的机器会卡在 connect 上
        // 直到系统默认超时——Windows 上那是 20 秒往上，用户以为程序死了。
        const int secs = static_cast<int>(timeout_s);
        cli.set_connection_timeout(secs, 0);
        cli.set_read_timeout(secs, 0);
        cli.set_write_timeout(secs, 0);
        cli.set_follow_location(true);

        httplib::Headers h;
        for (const auto& kv : headers) h.emplace(kv.first, kv.second);

        const auto res = cli.Post(path, h, body, "application/json");
        if (!res) {
            HttpResponse out;
            out.status = 0;
            out.transport_error = httplib::to_string(res.error());
            return out;
        }
        HttpResponse out;
        out.status = res->status;
        out.body = res->body;
        out.headers = lower_headers(res->headers);
        return out;
    };
}

namespace {

/// `scheme://host[:port]` 里的主机名：小写、不带方括号、不带末尾那个点。
std::string host_of_origin(const std::string& origin) {
    std::string h = origin;
    if (const auto at = h.find("://"); at != std::string::npos) h.erase(0, at + 3);
    if (const auto at = h.rfind('@'); at != std::string::npos) h.erase(0, at + 1);
    if (!h.empty() && h.front() == '[') {
        const auto close = h.find(']');
        h = close == std::string::npos ? h.substr(1) : h.substr(1, close - 1);
    } else if (const auto colon = h.find(':'); colon != std::string::npos) {
        h.erase(colon);
    }
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!h.empty() && h.back() == '.') h.pop_back();
    return h;
}

/// 自己跟跳转最多几跳（和上网工具那头一样）。
constexpr int kMaxHops = 5;

/// 这一回是跳转、还说了跳去哪儿，就回下一跳的完整地址；否则回空（原样交回去）。
std::optional<std::string> next_hop(const std::string& at, const HttpResponse& r) {
    if (r.status < 300 || r.status >= 400 || r.status == 304) return std::nullopt;
    const auto loc = r.headers.find("location");
    if (loc == r.headers.end() || loc->second.empty()) return std::nullopt;
    std::string next = redirect_target(at, loc->second);
    if (next.rfind("http://", 0) != 0 && next.rfind("https://", 0) != 0) return std::nullopt;
    if (const auto hash = next.find('#'); hash != std::string::npos) next.erase(hash);
    return next;
}

/// **没编 TLS 的话 httplib 的构造函数直接抛**，异常一路冒到 server.cpp 的 guard 那里，
/// 变成一句「500 服务端出错：'https' scheme is not supported.」——而这条路上的默认
/// 地址正好是 https（智谱、GitHub）。翻成一次普通的"连不上"，上层失败分支照常走。
/// 跳转那一跳也要判：http 的地址跳到 https 上是常事。
std::optional<std::string> tls_refusal([[maybe_unused]] const std::string& origin) {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (origin.rfind("https://", 0) == 0) {
        return SAY("当前程序编译时未包含 TLS（CHANGJI_SSL=OFF），无法发送 https 请求");
    }
#endif
    return std::nullopt;
}

}  // namespace

HttpGet default_http_get(bool follow_redirects, std::size_t max_body, bool public_only) {
    if (public_only) follow_redirects = false;
    return [follow_redirects, max_body, public_only](const std::string& first_url,
                                                     const std::map<std::string, std::string>& headers,
                                                     double timeout_s) -> HttpResponse {
        const int secs = static_cast<int>(timeout_s);
        // **整趟也有个头。** 下面那几个超时是「每读一次」的：对面一秒挤一个字节，读永远
        // 不超时，一次读网页能占着那一轮（从网上写一章时还占着写作那个槽）几个钟头。
        // 整趟最多是单次超时的六倍、不少于两分钟（装技能要拉几十 MB）。跳转算在同一趟里。
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(std::max(120, secs * 6));
        std::string url = first_url;
        // **跳转自己跟，不交给 httplib**：它跟跳转时把 Location 的路径解一遍码再编回去，
        // 带签名的地址就对不上了（见 request_target）。
        for (int hop = 0;; ++hop) {
            HttpResponse out;
            const auto [origin, path] = split_base(url);
            if (const auto why = tls_refusal(origin)) {
                out.status = 0;
                out.transport_error = *why;
                return out;
            }
            httplib::Client cli(origin);
            if (public_only) {
                // 见 util/net_inward.hpp：字面地址直接判；域名先解析、每个地址都判、钉住去连。
                const std::string host = host_of_origin(origin);
                const util::PublicHost v = util::vet_public_host(host);
                if (!v.refuse.empty()) {
                    out.status = 0;
                    out.transport_error = v.refuse;
                    return out;
                }
                if (!v.pin.empty()) cli.set_hostname_addr_map({{host, v.pin}});
            }
            cli.set_connection_timeout(secs, 0);
            cli.set_read_timeout(secs, 0);
            cli.set_write_timeout(secs, 0);
            cli.set_follow_location(false);
            cli.set_url_encode(false);
            cli.set_decompress(true);
            httplib::Headers h;
            for (const auto& kv : headers) h.emplace(kv.first, kv.second);
            // **正文边收边数，过了上限当场断开**（2026-09-24）。原来整段收完才交出去，
            // 而这条路上的网址是模型、网页、人贴进来的——一个回无底正文的地址能把整个
            // 引擎的内存吃光，装技能那条路在下载完之后才比大小，已经晚了。
            bool too_big = false;
            bool too_slow = false;
            const auto res = cli.Get(
                request_target(path), h,
                [&](const httplib::Response& r) {
                    out.status = r.status;
                    out.headers = lower_headers(r.headers);
                    return true;
                },
                [&](const char* data, std::size_t len) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        too_slow = true;
                        return false;
                    }
                    if (out.body.size() + len > max_body) {
                        too_big = true;
                        return false;
                    }
                    out.body.append(data, len);
                    return true;
                });
            if (too_slow) {
                out.status = 0;
                out.body.clear();
                out.transport_error = SAY("服务器响应过慢，接收中途已断开");
                return out;
            }
            if (too_big) {
                out.status = 0;
                out.body.clear();
                out.transport_error = SAYF("响应内容过大（超过 %1 MB），接收中途已断开",
                                           std::to_string(max_body / (1024 * 1024)));
                return out;
            }
            if (!res) {
                out.status = 0;
                out.transport_error = httplib::to_string(res.error());
                return out;
            }
            if (out.body.empty() && !res->body.empty()) out.body = res->body;
            if (follow_redirects && hop < kMaxHops) {
                if (const auto next = next_hop(url, out)) {
                    url = *next;
                    continue;
                }
            }
            return out;
        }
    };
}

HttpPostStream default_http_post_stream() {
    return [](const std::string& url, const std::string& body,
              const std::map<std::string, std::string>& headers,
              double timeout_s, const OnChunk& on_chunk) -> HttpResponse {
        const auto [origin, path] = split_base(url);

        httplib::Client cli(origin);
        const int secs = static_cast<int>(timeout_s);
        cli.set_connection_timeout(secs, 0);
        // **读超时是"两段之间最多等多久"，不是整条流的总时长。** 流式
        // 那条一开就是几分钟，按整条算的话得设成天文数字；按段算，
        // 模型每吐一个字就重置一次，卡住才会真超时。
        cli.set_read_timeout(secs, 0);
        cli.set_write_timeout(secs, 0);
        cli.set_follow_location(true);

        // ⚠️ **只能走 Client::send()。** 0.15.3 的 Post 没有带
        // ContentReceiver 的重载（Get 有，Post 没有），拿普通 Post 的话
        // httplib 会把整条流攒完再返回——那就又回到"整段到"了，而且
        // **看不出来**：功能照常，只是流式一点不流。
        httplib::Request rq;
        rq.method = "POST";
        rq.path = path;
        for (const auto& kv : headers) rq.headers.emplace(kv.first, kv.second);
        rq.body = body;

        HttpResponse out;
        // 状态码要在**收正文之前**知道：>= 400 时那份 body 是错误信息，
        // 不能往 on_chunk 里送（那边是按正文解的），得攒下来交给上层翻译。
        int status = 0;
        rq.response_handler = [&status](const httplib::Response& res) {
            status = res.status;
            return true;
        };
        // **服务端没理会 stream、回了一份普通 JSON 的，攒进 out.body。**
        //
        // 原来正文一律交给 on_chunk，指望末尾那句 `res->body` 兜底——可带了
        // content_receiver 的话 httplib 根本不往 res->body 里放（0.15.3 那段读
        // 正文的代码只往 receiver 里送），于是上层那两处「按整段解一次」永远
        // 拿到空串：网关不认 stream 时对话一个字都不回，也不报错（2026-09-25
        // 拿一个不认 stream 的假服务实测撞到）。看第一个非空白字节：`{` / `[`
        // 开头的是整份 JSON，SSE 是 `data:` / `:` / `event:` 开头的。
        int sniffed = 0;   // 0 还没见到字；1 是流；2 是整份 JSON
        rq.content_receiver = [&](const char* data, std::size_t len,
                                  std::uint64_t, std::uint64_t) {
            if (status >= 400) {
                out.body.append(data, len);
                return true;
            }
            if (sniffed == 0) {
                for (std::size_t i = 0; i < len; ++i) {
                    const char c = data[i];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                    sniffed = (c == '{' || c == '[') ? 2 : 1;
                    break;
                }
            }
            if (sniffed == 2) {
                out.body.append(data, len);
                return true;
            }
            return on_chunk(data, len);
        };

        const auto res = cli.send(rq);
        if (!res) {
            out.status = 0;
            // 取消时我们自己从 on_chunk 里返回了 false，httplib 报的是
            // Canceled。那不是"连不上"，上层按取消处理，所以照样带回去。
            out.transport_error = httplib::to_string(res.error());
            return out;
        }
        out.status = res->status;
        // 正常那条的正文已经从 on_chunk 走了；但服务端要是没理会 stream、
        // 回了一份普通 JSON，那份内容也在 out.body 里——上层会试着按整段解。
        if (out.body.empty() && !res->body.empty()) out.body = res->body;
        return out;
    };
}

}  // namespace changji::llm

namespace changji::http {

HttpGet default_http_get() {
    return [](const std::string& first_url,
              const std::map<std::string, std::string>& headers,
              double timeout_s) -> llm::HttpResponse {
        // 和 llm 那份共用拆地址、跟跳转、编请求目标那几样（见 llm::request_target：
        // 跳转不交给 httplib，GitHub 发布文件那条带签名的地址才拿得到）。
        std::string url = first_url;
        for (int hop = 0;; ++hop) {
            llm::HttpResponse out;
            const auto [origin, path] = llm::split_base(url);
            if (const auto why = llm::tls_refusal(origin)) {
                out.status = 0;
                out.transport_error = *why;
                return out;
            }
            httplib::Client cli(origin);
            const int secs = static_cast<int>(timeout_s);
            cli.set_connection_timeout(secs, 0);
            cli.set_read_timeout(secs, 0);
            cli.set_write_timeout(secs, 0);
            cli.set_follow_location(false);
            cli.set_url_encode(false);

            httplib::Headers h;
            for (const auto& kv : headers) h.emplace(kv.first, kv.second);

            const auto res = cli.Get(llm::request_target(path), h);
            if (!res) {
                out.status = 0;
                out.transport_error = httplib::to_string(res.error());
                return out;
            }
            out.status = res->status;
            out.body = res->body;
            out.headers = llm::lower_headers(res->headers);
            if (hop < llm::kMaxHops) {
                if (const auto next = llm::next_hop(url, out)) {
                    url = *next;
                    continue;
                }
            }
            return out;
        }
    };
}

}  // namespace changji::http
