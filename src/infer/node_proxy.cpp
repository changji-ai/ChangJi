#include "infer/node_proxy.hpp"
#include "util/say.hpp"

#include "util/httplib.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto pos = url.find("://");
    const std::string rest =
        pos == std::string::npos ? url : url.substr(pos + 3);
    const auto slash = rest.find('/');
    if (slash == std::string::npos) return {url, ""};
    return {url.substr(0, pos == std::string::npos ? slash : pos + 3 + slash),
            rest.substr(slash)};
}

/// 派活给这台时用哪个口令。那台单独配了就用它的，否则用全局那个。
std::string token_for(const config::Settings& s, const std::string& node_url) {
    for (const auto& n : s.peer.nodes) {
        if (n.url == node_url) {
            return n.token.empty() ? s.peer.token : n.token;
        }
    }
    // **不在配置里也照发。** 这种情况是界面传了个没登记的地址，
    // 那头会回 401 或者连不上——两种都比这里静默拒绝好查。
    return s.peer.token;
}

httplib::Client make_client(const config::Settings& s,
                            const std::string& node_url, int timeout_s,
                            std::string& prefix) {
    const auto [origin, p] = split_url(node_url);
    prefix = p;
    httplib::Client cli(origin);
    cli.set_connection_timeout(timeout_s, 0);
    cli.set_read_timeout(timeout_s, 0);
    const std::string token = token_for(s, node_url);
    if (!token.empty()) cli.set_bearer_token_auth(token);
    return cli;
}

/// 把 httplib 的回应翻成 ProxyResult。
///
/// **body 不是 JSON 时也要给出点什么**：对面可能是个反向代理回的 502
/// HTML，那时候一句"对面回的不是 JSON"比一个空对象有用得多。
ProxyResult finish(const httplib::Result& res, const std::string& node_url) {
    if (!res) {
        return {0,
                json{{"detail",
                      SAYF("连不上 %1：%2", node_url,
                           httplib::to_string(res.error()))}}};
    }
    auto body = json::parse(res->body, nullptr, false);
    if (body.is_discarded()) {
        body = json{{"detail", SAYF("对面回的不是 JSON（%1）",
                                    std::to_string(res->status))}};
    }
    return {res->status, std::move(body)};
}

}  // namespace

ProxyResult node_get(const config::Settings& s, const std::string& node_url,
                     const std::string& path, int timeout_s) {
    std::string prefix;
    auto cli = make_client(s, node_url, timeout_s, prefix);
    return finish(cli.Get(prefix + path), node_url);
}

ProxyResult node_post(const config::Settings& s, const std::string& node_url,
                      const std::string& path, const json& body,
                      int timeout_s) {
    std::string prefix;
    auto cli = make_client(s, node_url, timeout_s, prefix);
    return finish(cli.Post(prefix + path, body.dump(), "application/json"),
                  node_url);
}

}  // namespace changji::infer
