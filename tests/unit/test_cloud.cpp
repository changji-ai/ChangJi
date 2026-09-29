// 场记云（docs/账号与云服务方案.md）：登录那一趟、设备钥匙、虚的那一家 / 那一台。
//
// 钉的是会**静悄悄**出事的那几样：
//   · PKCE 算错——授权回来换不到钥匙，只报一句「invalid_grant」，查半天；
//   · 回调地址认得太宽——公网上的授权码被别的网站截走；
//   · 设备钥匙发给了别的服务（「大模型密钥只给存它的那一家」那条规矩）；
//   · 虚的那台机器被写进 config.toml——退出登录之后留下一台带着作废钥匙、永远 401 的机器；
//   · 用场记账号进门时账号不是主人也放进来了。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cloud/cloud.hpp"
#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "config/writeback.hpp"
#include "http/cloud_api.hpp"
#include "http/llm_info.hpp"
#include "util/sha256.hpp"

#include "scoped_env.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

cloud::Creds sample_creds(const std::string& url = "https://changji.xyz") {
    cloud::Creds c;
    c.url = url;
    c.device_key = "cjk_test_key";
    c.device_id = "d1";
    c.account = {"u1", "林晚", "https://x/a.jpg", "wechat", "active", "admin"};
    c.bound_at = 1;
    return c;
}

/// 记下发了什么、回预先给的答复。
struct FakeNet {
    std::vector<std::pair<std::string, std::string>> posts;  // url, body
    std::vector<std::string> gets;
    std::function<llm::HttpResponse(const std::string&, const std::string&)> on_post;
    std::function<llm::HttpResponse(const std::string&)> on_get;
    http::CloudIo io() {
        return {[this](const std::string& url, const std::map<std::string, std::string>&, double) {
                    gets.push_back(url);
                    return on_get ? on_get(url) : llm::HttpResponse{};
                },
                [this](const std::string& url, const std::string& body, const std::map<std::string, std::string>&,
                       double) {
                    posts.push_back({url, body});
                    return on_post ? on_post(url, body) : llm::HttpResponse{};
                }};
    }
};

llm::HttpResponse ok(const json& j) {
    llm::HttpResponse r;
    r.status = 200;
    r.body = j.dump();
    return r;
}

std::string query_of(const std::string& url, const std::string& key) {
    const auto q = url.find('?');
    std::stringstream ss(url.substr(q + 1));
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        if (kv.rfind(key + "=", 0) == 0) return kv.substr(key.size() + 1);
    }
    return {};
}

}  // namespace

TEST_CASE("场记云：PKCE 照 RFC 7636 的标准答案算") {
    // RFC 7636 附录 B
    CHECK(cloud::pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
          "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    CHECK(util::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 随机串：base64url、不带 =、够长
    const std::string t = cloud::random_token(32);
    CHECK(t.size() == 43);
    CHECK(t.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
          std::string::npos);
    CHECK(cloud::random_token(32) != t);
}

TEST_CASE("场记云：回调只认本机和局域网地址") {
    for (const char* h : {"127.0.0.1:8080", "localhost:8080", "[::1]:8080", "192.168.20.91:8080", "10.0.0.5",
                          "172.16.3.4:9", "172.31.255.1"}) {
        CAPTURE(h);
        CHECK(cloud::redirect_host_ok(h));
    }
    for (const char* h : {"changji.xyz", "8.8.8.8:8080", "172.32.0.1", "192.169.1.1", "evil.localhost.example",
                          "192.168.1", "192.168.1.1.5", ""}) {
        CAPTURE(h);
        CHECK_FALSE(cloud::redirect_host_ok(h));
    }
}

TEST_CASE("场记云：授权地址带齐 PKCE、state、回调，参数编过码") {
    const std::string u = cloud::authorize_url("https://changji.xyz/", "http://127.0.0.1:8080/auth/cloud/callback",
                                               "st&1", "ch", "张三 的电脑", "windows", "google");
    CHECK(u.rfind("https://changji.xyz/oauth/authorize?response_type=code&client_id=changji-engine", 0) == 0);
    CHECK(query_of(u, "redirect_uri") == "http%3A%2F%2F127.0.0.1%3A8080%2Fauth%2Fcloud%2Fcallback");
    CHECK(query_of(u, "state") == "st%261");
    CHECK(query_of(u, "code_challenge_method") == "S256");
    CHECK(query_of(u, "provider") == "google");
    CHECK(query_of(u, "device_name").find("%E5%BC%A0") == 0);
    // 认不出的 provider 不带
    CHECK(cloud::authorize_url("https://c", "r", "s", "c", "d", "p", "evil").find("provider") == std::string::npos);
}

TEST_CASE("场记云：发出去的那一趟一次性、十分钟") {
    cloud::PendingLogins pl;
    cloud::Pending p;
    p.verifier = "v";
    const std::string s = pl.issue(p, 1000);
    CHECK(pl.take(s, 2000).has_value());
    CHECK_FALSE(pl.take(s, 2000).has_value());
    const std::string s2 = pl.issue(p, 0);
    CHECK_FALSE(pl.take(s2, 10 * 60 * 1000 + 1).has_value());
    CHECK_FALSE(pl.take("", 0).has_value());
}

TEST_CASE("场记云：设备钥匙只给那朵云自己的 /v1；虚的那台机器不写进 config.toml") {
    test::ScopedUserConfigDir dir("cloud_keys");
    CHECK_FALSE(cloud::load_creds().has_value());
    cloud::save_creds(sample_creds());
    REQUIRE(cloud::load_creds().has_value());
    CHECK(cloud::load_creds()->account.name == "林晚");

    CHECK(cloud::key_for("https://changji.xyz/v1") == std::optional<std::string>("cjk_test_key"));
    CHECK(cloud::key_for("https://changji.xyz/v1/") == std::optional<std::string>("cjk_test_key"));
    CHECK_FALSE(cloud::key_for("https://changji.xyz").has_value());
    CHECK_FALSE(cloud::key_for("https://open.bigmodel.cn/api/paas/v4").has_value());
    CHECK_FALSE(cloud::key_for("https://changji.xyz.evil.example/v1").has_value());
    // 按地址取密钥的那一处（大模型客户端、设置页都走它）
    CHECK(config::read_api_key_for("https://changji.xyz/v1") == "cjk_test_key");
    CHECK(config::has_own_api_key("https://changji.xyz/v1"));
    CHECK(config::read_api_key_for("https://api.deepseek.com/v1") != "cjk_test_key");

    // 读配置：机器表里多一台「场记云」，带着设备钥匙
    config::Settings s = config::load_settings();
    bool found = false;
    for (const auto& n : s.peer.nodes) {
        if (n.url == "https://changji.xyz/render") {
            found = true;
            CHECK(n.token == "cjk_test_key");
        }
    }
    CHECK(found);
    // 写机器表：那一台跳过，别的照写
    json arr = json::array({{{"url", "https://changji.xyz/render"}, {"token", "cjk_test_key"}},
                            {{"url", "http://gpu-box:8080"}, {"token", "t"}}});
    const fs::path cfg = dir.dir() / "peers.toml";
    config::save_peer_nodes(arr, cfg);
    std::ifstream in(cfg);
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str().find("gpu-box") != std::string::npos);
    CHECK(ss.str().find("/render") == std::string::npos);
    CHECK(ss.str().find("cjk_") == std::string::npos);

    // 换了云地址：那把钥匙不认、那台机器也不加
    CHECK_FALSE(cloud::virtual_node("https://other.example").has_value());
    cloud::clear_creds();
    CHECK_FALSE(cloud::key_for("https://changji.xyz/v1").has_value());
    config::Settings s2 = config::load_settings();
    for (const auto& n : s2.peer.nodes) CHECK(n.url != "https://changji.xyz/render");
}

TEST_CASE("场记云：GET /api/cloud——没登、登着、钥匙被吊销、连不上") {
    test::ScopedUserConfigDir dir("cloud_status");
    http::forget_cloud_cache();
    FakeNet net;
    auto io = net.io();
    auto r = http::get_cloud(io, "https://changji.xyz");
    CHECK(r.body.at("enabled") == true);
    CHECK(r.body.at("signed_in") == false);
    CHECK(net.gets.empty());   // 没登就不去问
    CHECK(http::get_cloud(io, "").body.at("enabled") == false);

    cloud::save_creds(sample_creds());
    net.on_get = [](const std::string&) {
        return ok({{"account", {{"id", "u1"}, {"name", "林晚改名"}, {"status", "active"}, {"role", "admin"}}},
                   {"services", {{"llm", {{"ready", true}}}}},
                   {"quota", {{"tier", "member"}, {"llm_tokens", {{"used", 5}, {"limit", 100}}}}}});
    };
    r = http::get_cloud(io, "https://changji.xyz/");
    CHECK(r.body.at("signed_in") == true);
    CHECK(r.body.at("offline") == false);
    CHECK(r.body.at("quota").at("tier") == "member");
    CHECK(r.body.at("account").at("name") == "林晚改名");
    CHECK(cloud::load_creds()->account.name == "林晚改名");   // 名字跟着云上变
    REQUIRE(net.gets.size() == 1);
    CHECK(net.gets[0] == "https://changji.xyz/api/cloud/me");
    // 一分钟内再问用缓存
    http::get_cloud(io, "https://changji.xyz");
    CHECK(net.gets.size() == 1);

    // 连不上：不当成退出登录
    net.on_get = [](const std::string&) {
        llm::HttpResponse x;
        x.transport_error = "timeout";
        return x;
    };
    r = http::get_cloud(io, "https://changji.xyz", /*refresh=*/true);
    CHECK(r.body.at("signed_in") == true);
    CHECK(r.body.at("offline") == true);
    CHECK(r.body.at("quota").at("tier") == "member");   // 上一回问到的留着
    CHECK(cloud::load_creds().has_value());

    // 钥匙在账号页被吊销了：本地那份作废
    net.on_get = [](const std::string&) {
        llm::HttpResponse x;
        x.status = 401;
        return x;
    };
    r = http::get_cloud(io, "https://changji.xyz", true);
    CHECK(r.body.at("signed_in") == false);
    CHECK(r.body.at("revoked") == true);
    CHECK_FALSE(cloud::load_creds().has_value());
}

TEST_CASE("场记云：登录那一趟——公网地址不给登，绑、重登、换人、用账号进门") {
    test::ScopedUserConfigDir dir("cloud_login");
    http::forget_cloud_cache();
    FakeNet net;
    auto io = net.io();
    const std::string cloud_url = "https://changji.xyz";

    // 公网地址上开的界面：回调地址场记云不认，当场说清楚
    CHECK_THROWS_AS(http::post_cloud_start({}, cloud::Purpose::bind, "http://changji.example.com", cloud_url),
                    http::ApiError);
    // 还没绑：不能用账号进门
    CHECK_THROWS_AS(http::post_cloud_start({}, cloud::Purpose::enter, "http://192.168.1.2:8080", cloud_url),
                    http::ApiError);
    // 云关着
    CHECK_THROWS_AS(http::post_cloud_start({}, cloud::Purpose::bind, "http://127.0.0.1:8080", ""), http::ApiError);

    // 绑：授权地址 → 回调 → 换钥匙
    auto start = http::post_cloud_start({{"provider", "google"}}, cloud::Purpose::bind, "http://127.0.0.1:8080",
                                        cloud_url);
    CHECK(query_of(start.body.at("url"), "provider") == "google");
    // 微信 2026-09-27 去掉了：给了也不往场记云带
    CHECK(query_of(http::post_cloud_start({{"provider", "wechat"}}, cloud::Purpose::bind, "http://127.0.0.1:8080", cloud_url)
                       .body.at("url"),
                   "provider")
              .empty());
    const std::string url = start.body.at("url");
    const std::string state = query_of(url, "state");
    CHECK(query_of(url, "redirect_uri") == "http%3A%2F%2F127.0.0.1%3A8080%2Fauth%2Fcloud%2Fcallback");
    std::string verifier_sent;
    net.on_post = [&](const std::string& u, const std::string& body) {
        if (u.find("/oauth/token") != std::string::npos) {
            const json j = json::parse(body);
            verifier_sent = j.at("code_verifier");
            CHECK(j.at("code") == "c1");
            CHECK(j.at("redirect_uri") == "http://127.0.0.1:8080/auth/cloud/callback");
            return ok({{"device_key", "cjk_new"}, {"device_id", "d9"},
                       {"account", {{"id", "u1"}, {"name", "林晚"}, {"provider", "wechat"}}}});
        }
        return ok({{"ok", true}});
    };
    auto cb = http::cloud_callback(io, "c1", state, "", "");
    CHECK(cb.location == "/?cloud=ok");
    CHECK(cb.changed);
    // 发出去的校验码对得上授权地址里那个挑战码
    CHECK(cloud::pkce_challenge(verifier_sent) == query_of(url, "code_challenge"));
    REQUIRE(cloud::load_creds().has_value());
    CHECK(cloud::load_creds()->device_key == "cjk_new");
    // 同一个 state 用不了第二次
    CHECK(http::cloud_callback(io, "c1", state, "", "").location.find("login_error") != std::string::npos);

    // 登着「林晚」时另一个人来绑：不许悄悄换，他那把当场吊销
    net.posts.clear();
    net.on_post = [&](const std::string& u, const std::string&) {
        if (u.find("/oauth/token") != std::string::npos) {
            return ok({{"device_key", "cjk_other"}, {"account", {{"id", "u2"}, {"name", "陈默"}}}});
        }
        return ok({{"ok", true}});
    };
    const std::string s2 = query_of(
        http::post_cloud_start({}, cloud::Purpose::bind, "http://127.0.0.1:8080", cloud_url).body.at("url"), "state");
    cb = http::cloud_callback(io, "c2", s2, "", "");
    CHECK(cb.location.find("login_error") != std::string::npos);
    CHECK_FALSE(cb.changed);
    CHECK(cloud::load_creds()->device_key == "cjk_new");
    CHECK(net.posts.back().first == "https://changji.xyz/api/cloud/logout");

    // 用账号进门：不是主人不给进
    const std::string s3 = query_of(
        http::post_cloud_start({}, cloud::Purpose::enter, "http://192.168.1.2:8080", cloud_url).body.at("url"),
        "state");
    cb = http::cloud_callback(io, "c3", s3, "", "ui-tok");
    CHECK(cb.set_cookie.empty());
    CHECK(cb.location.find("login_error") != std::string::npos);

    // 主人：种进门的 cookie（值是网页口令），那把临时钥匙吊销，本地那份不动
    net.posts.clear();
    net.on_post = [&](const std::string& u, const std::string&) {
        if (u.find("/oauth/token") != std::string::npos) {
            return ok({{"device_key", "cjk_temp"}, {"account", {{"id", "u1"}, {"name", "林晚"}}}});
        }
        return ok({{"ok", true}});
    };
    const std::string s4 = query_of(
        http::post_cloud_start({}, cloud::Purpose::enter, "http://192.168.1.2:8080", cloud_url).body.at("url"),
        "state");
    cb = http::cloud_callback(io, "c4", s4, "", "ui-tok");
    CHECK(cb.location == "/");
    CHECK(cb.set_cookie.find("changji_ui=ui-tok") == 0);
    CHECK(cb.set_cookie.find("HttpOnly") != std::string::npos);
    CHECK(cloud::load_creds()->device_key == "cjk_new");
    REQUIRE_FALSE(net.posts.empty());
    CHECK(net.posts.back().first == "https://changji.xyz/api/cloud/logout");

    // 在场记云那边点了拒绝
    const std::string s5 = query_of(
        http::post_cloud_start({}, cloud::Purpose::bind, "http://127.0.0.1:8080", cloud_url).body.at("url"), "state");
    cb = http::cloud_callback(io, "", s5, "access_denied", "");
    CHECK(cb.location.find("login_error") != std::string::npos);

    // 退出：云上吊销、本地删掉
    net.posts.clear();
    auto out = http::post_cloud_logout(io);
    CHECK(out.body.at("ok") == true);
    CHECK_FALSE(cloud::load_creds().has_value());
    REQUIRE(net.posts.size() == 1);
    CHECK(net.posts[0].first == "https://changji.xyz/api/cloud/logout");
}

TEST_CASE("场记云：登录页问的那一条——进没进门、绑没绑、口令只在回环监听时给") {
    test::ScopedUserConfigDir dir("cloud_gate");
    auto g = http::cloud_gate(false, false, "tok", "https://changji.xyz");
    CHECK(g.at("authed") == false);
    CHECK(g.at("token_hint") == "");
    CHECK(g.at("cloud").at("bound") == false);
    CHECK(g.at("cloud").at("owner").is_null());
    cloud::save_creds(sample_creds());
    g = http::cloud_gate(true, true, "tok", "https://changji.xyz");
    CHECK(g.at("token_hint") == "tok");
    CHECK(g.at("cloud").at("bound") == true);
    CHECK(g.at("cloud").at("owner").at("name") == "林晚");
    // 登的是别的云：不算绑
    CHECK(http::cloud_gate(false, false, "", "https://other.example").at("cloud").at("bound") == false);
}

TEST_CASE("场记云：登着就在服务单子最前面多一家「场记云」，退出就没了") {
    test::ScopedUserConfigDir dir("cloud_providers");
    config::Settings s = config::runtime().snapshot();
    const config::Settings old = s;
    s.cloud.url = "https://changji.xyz";
    config::runtime().replace(s);
    auto first_is_cloud = [] {
        const auto r = http::get_llm_providers();
        const auto& ps = r.body.at("providers");
        return !ps.empty() && ps[0].value("id", std::string()) == "cloud";
    };
    CHECK_FALSE(first_is_cloud());
    cloud::save_creds(sample_creds());
    CHECK(first_is_cloud());
    const auto r = http::get_llm_providers();
    CHECK(r.body.at("providers")[0].at("base_url") == "https://changji.xyz/v1");
    CHECK(r.body.at("providers")[0].at("key_set") == true);
    cloud::clear_creds();
    CHECK_FALSE(first_is_cloud());
    config::runtime().replace(old);
}

TEST_CASE("本地模型闲卸那一项：默认五分钟，配置读得进来，离谱的数起服务时就说") {
    config::Settings s;
    CHECK(s.llm.keep_alive_minutes == doctest::Approx(5.0));
    CHECK(s.llm.validate().empty());
    s.llm.keep_alive_minutes = -1;
    CHECK(s.llm.validate().empty());   // 负数 = 一直留着
    s.llm.keep_alive_minutes = 100000;
    CHECK_FALSE(s.llm.validate().empty());

    test::ScopedUserConfigDir dir("keep_alive");
    const auto f = dir.dir() / "c.toml";
    {
        std::ofstream out(f);
        out << "[llm]\nkeep_alive_minutes = 0\n";
    }
    CHECK(config::load_settings_file(f).llm.keep_alive_minutes == doctest::Approx(0.0));
}
