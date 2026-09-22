// 真实的 HTTP 实现。**只有这个文件 include httplib。**
//
// 分出来是为了让 client.cpp 能进单元测试目标而不用链 httplib——
// 请求怎么拼、错误怎么翻成人话、返回怎么抽内容，那三件事全是纯逻辑。

#include "util/httplib.hpp"
#include "util/say.hpp"

#include <cstdint>
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
        return out;
    };
}

HttpGet default_http_get() {
    return [](const std::string& url, const std::map<std::string, std::string>& headers,
              double timeout_s) -> HttpResponse {
        HttpResponse out;
        const auto [origin, path] = split_base(url);
        httplib::Client cli(origin);
        const int secs = static_cast<int>(timeout_s);
        cli.set_connection_timeout(secs, 0);
        cli.set_read_timeout(secs, 0);
        cli.set_write_timeout(secs, 0);
        cli.set_follow_location(true);
        cli.set_decompress(true);
        httplib::Headers h;
        for (const auto& kv : headers) h.emplace(kv.first, kv.second);
        const auto res = cli.Get(path, h);
        if (!res) {
            out.transport_error = httplib::to_string(res.error());
            return out;
        }
        out.status = res->status;
        out.body = res->body;
        return out;
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
        rq.content_receiver = [&](const char* data, std::size_t len,
                                  std::uint64_t, std::uint64_t) {
            if (status >= 400) {
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
    return [](const std::string& url,
              const std::map<std::string, std::string>& headers,
              double timeout_s) -> llm::HttpResponse {
        // 和 POST 那份共用同一套拆地址和超时设置。抽个公共函数不划算——
        // 两边加起来二十行，抽出来反而要多一层间接。
        const std::size_t scheme_end = url.find("://");
        const std::size_t host_start =
            scheme_end == std::string::npos ? 0 : scheme_end + 3;
        const std::size_t path_start = url.find('/', host_start);
        const std::string origin =
            path_start == std::string::npos ? url : url.substr(0, path_start);
        const std::string path =
            path_start == std::string::npos ? "/" : url.substr(path_start);

        // **没编 TLS 的话 httplib 的构造函数直接抛**，异常一路冒到
        // server.cpp 的 guard 那里，变成一句
        //   500 服务端出错：'https' scheme is not supported.
        // 而这一条路上的默认地址正好是 https（智谱）——表现就是设置页
        // 的模型列表整个 500，llm_info 里那句"列不出来不该让设置页打不开"
        // 也就白写了，连同那份 known 小抄一起到不了前端。
        //
        // 翻成一次普通的"连不上"，上层那三条失败分支照常走。
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
        if (origin.rfind("https://", 0) == 0) {
            llm::HttpResponse out;
            out.status = 0;
            out.transport_error =
                SAY("这个程序编译时没带 TLS（CHANGJI_SSL=OFF），"
                    "发不了 https 请求");
            return out;
        }
#endif

        httplib::Client cli(origin);
        const int secs = static_cast<int>(timeout_s);
        cli.set_connection_timeout(secs, 0);
        cli.set_read_timeout(secs, 0);
        cli.set_write_timeout(secs, 0);
        cli.set_follow_location(true);

        httplib::Headers h;
        for (const auto& kv : headers) h.emplace(kv.first, kv.second);

        const auto res = cli.Get(path, h);
        llm::HttpResponse out;
        if (!res) {
            out.status = 0;
            out.transport_error = httplib::to_string(res.error());
            return out;
        }
        out.status = res->status;
        out.body = res->body;
        return out;
    };
}

}  // namespace changji::http
