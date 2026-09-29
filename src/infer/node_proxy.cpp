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

/// 不在机器表里的：不发请求、不带口令，回一句人话。见 node_proxy.hpp 的 proxy_target。
/// 那句话指路照界面上的名字：机器表 2026-09-28 起摆在「设置 ▸ 互联」里。
ProxyResult not_listed(const std::string& node_url) {
    return {404, json{{"detail", SAYF("%1 不在机器列表中：请先在「设置 ▸ 互联」中添加这台机器", node_url)}}};
}

httplib::Client make_client(const std::string& token, const std::string& node_url,
                            int timeout_s, std::string& prefix) {
    const auto [origin, p] = split_url(node_url);
    prefix = p;
    httplib::Client cli(origin);
    cli.set_connection_timeout(timeout_s, 0);
    cli.set_read_timeout(timeout_s, 0);
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
                      SAYF("无法连接 %1：%2", node_url,
                           httplib::to_string(res.error()))}}};
    }
    auto body = json::parse(res->body, nullptr, false);
    if (body.is_discarded()) {
        body = json{{"detail", SAYF("对方机器返回的内容不是 JSON（%1）",
                                    std::to_string(res->status))}};
    }
    return {res->status, std::move(body)};
}

}  // namespace

ProxyResult node_get(const config::Settings& s, const std::string& node_url,
                     const std::string& path, int timeout_s) {
    const auto target = proxy_target(s, node_url);
    if (!target.listed) return not_listed(node_url);
    std::string prefix;
    auto cli = make_client(target.token, node_url, timeout_s, prefix);
    return finish(cli.Get(prefix + path), node_url);
}

ProxyResult node_post(const config::Settings& s, const std::string& node_url,
                      const std::string& path, const json& body,
                      int timeout_s) {
    const auto target = proxy_target(s, node_url);
    if (!target.listed) return not_listed(node_url);
    std::string prefix;
    auto cli = make_client(target.token, node_url, timeout_s, prefix);
    return finish(cli.Post(prefix + path, body.dump(), "application/json"),
                  node_url);
}

}  // namespace changji::infer
