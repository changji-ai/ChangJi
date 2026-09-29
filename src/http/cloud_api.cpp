#include "http/cloud_api.hpp"

#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>

#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::http {

using json = nlohmann::json;

namespace {

constexpr double kTimeoutS = 15.0;

/// `/api/cloud/me` 的缓存：界面几处都问（头像、设置、登录页），一分钟问一次云就够。
struct MeCache {
    std::mutex mu;
    std::string key;  ///< 哪一把钥匙问的（换了账号就不认）
    json me;
    std::chrono::steady_clock::time_point at{};
    bool has = false;
};
MeCache& me_cache() {
    static MeCache c;
    return c;
}

json account_or_null(const cloud::Account& a) {
    if (a.id.empty()) return nullptr;
    return cloud::to_json(a);
}

/// 从一段回包里抽那句人话（`detail` / `error_description` / `error`）。
std::string reason_of(const llm::HttpResponse& r) {
    if (r.transport_error) return *r.transport_error;
    const json j = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_object()) {
        for (const char* k : {"detail", "error_description", "error"}) {
            const auto it = j.find(k);
            if (it != j.end() && it->is_string()) return it->get<std::string>();
        }
    }
    return "HTTP " + std::to_string(r.status);
}

std::string pct(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string fail_to(const std::string& msg) { return "/?login_error=" + pct(msg); }

std::string host_of_origin(const std::string& origin) {
    std::string h = origin;
    if (const auto at = h.find("://"); at != std::string::npos) h.erase(0, at + 3);
    if (const auto slash = h.find('/'); slash != std::string::npos) h.erase(slash);
    return h;
}

/// 云上吊销一把钥匙（退出登录、用账号进门之后那把临时的）。吊销不成不拦着——本地那份照删。
void revoke(const CloudIo& io, const std::string& cloud_url, const std::string& key) {
    if (!io.post || key.empty()) return;
    io.post(cloud::bare_url(cloud_url) + "/api/cloud/logout", "{}",
            {{"Authorization", "Bearer " + key}, {"Content-Type", "application/json"}}, kTimeoutS);
}

}  // namespace

void forget_cloud_cache() {
    std::lock_guard lg(me_cache().mu);
    me_cache().has = false;
}

ApiResult get_cloud(const CloudIo& io, const std::string& cloud_url_in, bool refresh) {
    const std::string cloud_url = cloud::bare_url(cloud_url_in);
    json out = {{"enabled", !cloud_url.empty()},
                {"url", cloud_url},
                {"signed_in", false},
                {"offline", false},
                {"account", nullptr},
                {"services", nullptr},
                {"quota", nullptr},
                {"bound_owner", nullptr}};
    auto c = cloud::load_creds();
    // 登着的是另一朵云（改过 [cloud].url）：这一朵上算没登录，那把钥匙也不往这儿发
    if (!c || c->url != cloud_url || cloud_url.empty()) return {200, out};

    out["signed_in"] = true;
    out["account"] = account_or_null(c->account);
    out["bound_owner"] = account_or_null(c->account);

    json me;
    bool fresh = false;
    {
        std::lock_guard lg(me_cache().mu);
        auto& mc = me_cache();
        if (!refresh && mc.has && mc.key == c->device_key &&
            std::chrono::steady_clock::now() - mc.at < std::chrono::seconds(60)) {
            me = mc.me;
            fresh = true;
        }
    }
    if (!fresh && io.get) {
        const auto r = io.get(cloud_url + "/api/cloud/me", {{"Authorization", "Bearer " + c->device_key}},
                              kTimeoutS);
        if (r.status == 401) {
            // **钥匙在账号页被吊销了 / 账号停用了**：本地那份作废，照实说没登录
            cloud::clear_creds();
            forget_cloud_cache();
            out["signed_in"] = false;
            out["account"] = nullptr;
            out["bound_owner"] = nullptr;
            out["revoked"] = true;
            return {200, out};
        }
        if (r.status == 200) {
            me = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
            if (me.is_object()) {
                std::lock_guard lg(me_cache().mu);
                auto& mc = me_cache();
                mc.key = c->device_key;
                mc.me = me;
                mc.at = std::chrono::steady_clock::now();
                mc.has = true;
            }
        } else {
            // **连不上不当成退出登录**：出差没网、云在重启，人还是登着的
            out["offline"] = true;
            std::lock_guard lg(me_cache().mu);
            if (me_cache().has && me_cache().key == c->device_key) me = me_cache().me;
        }
    }
    if (me.is_object()) {
        if (const auto it = me.find("account"); it != me.end() && it->is_object()) {
            const cloud::Account a = cloud::account_from_json(*it);
            out["account"] = cloud::to_json(a);
            // 名字、头像、开通没开通跟着云上变：记回本地那份（登录页上那一行主人要对）
            if (a.name != c->account.name || a.avatar != c->account.avatar || a.status != c->account.status ||
                a.role != c->account.role) {
                c->account = a;
                try {
                    cloud::save_creds(*c);
                } catch (...) {
                }
                out["bound_owner"] = cloud::to_json(a);
            }
        }
        if (const auto it = me.find("services"); it != me.end()) out["services"] = *it;
        if (const auto it = me.find("quota"); it != me.end()) out["quota"] = *it;
    }
    return {200, out};
}

json cloud_gate(bool authed, bool loopback_bind, const std::string& token_hint,
                const std::string& cloud_url_in) {
    const std::string cloud_url = cloud::bare_url(cloud_url_in);
    const auto c = cloud::load_creds();
    const bool bound = c && !cloud_url.empty() && c->url == cloud_url;
    json owner = nullptr;
    if (bound) owner = {{"name", c->account.name}, {"avatar", c->account.avatar}};
    return {{"authed", authed},
            {"loopback_bind", loopback_bind},
            {"token_hint", loopback_bind ? token_hint : std::string()},
            {"cloud", {{"enabled", !cloud_url.empty()}, {"url", cloud_url}, {"bound", bound}, {"owner", owner}}}};
}

ApiResult post_cloud_start(const json& body, cloud::Purpose purpose, const std::string& origin,
                           const std::string& cloud_url_in) {
    const std::string cloud_url = cloud::bare_url(cloud_url_in);
    // 地址不在设置页上（「账号与云服务」那一页自己也这么说：改本机配置的 [cloud] url），
    // 所以这句指配置文件，不说"设置里"——照那句去设置里找的人找不到那一格。
    if (cloud_url.empty()) throw ApiError(409, SAY("云服务未启用（本机配置中的 [cloud] url 为空）"));
    const auto c = cloud::load_creds();
    const bool bound = c && c->url == cloud_url;
    if (purpose == cloud::Purpose::enter && !bound) {
        throw ApiError(409, SAY("此引擎尚未绑定场记账号，请使用本机口令登录"));
    }
    if (!cloud::redirect_host_ok(host_of_origin(origin))) {
        // 回调地址是公网域名：场记云不认（那种地址上的授权码能被别的网站截走）
        throw ApiError(409, SAY("通过公网地址访问时无法使用场记账号登录，请使用本机口令登录"));
    }
    std::string provider = body.is_object() ? body.value("provider", std::string()) : std::string();
    if (provider != "google") provider.clear();  // 只剩 Google 一家（2026-09-27 去掉了微信）

    cloud::Pending p;
    p.verifier = cloud::random_token(32);
    p.redirect_uri = cloud::bare_url(origin) + "/auth/cloud/callback";
    p.provider = provider;
    p.cloud_url = cloud_url;
    p.purpose = purpose;
    const std::string challenge = cloud::pkce_challenge(p.verifier);
    const std::string redirect = p.redirect_uri;
    const std::string state = cloud::pending().issue(std::move(p), cloud::now_ms());
    return {200,
            {{"url", cloud::authorize_url(cloud_url, redirect, state, challenge, cloud::device_name(),
                                          cloud::platform_name(), provider)}}};
}

CloudRedirect cloud_callback(const CloudIo& io, const std::string& code, const std::string& state,
                             const std::string& error, const std::string& ui_token) {
    CloudRedirect out;
    const auto p = cloud::pending().take(state, cloud::now_ms());
    if (!p) {
        out.location = fail_to(SAY("登录已过期，请重新登录"));
        return out;
    }
    if (!error.empty()) {
        out.location = fail_to(error == "access_denied" ? SAY("已在场记云中拒绝授权") : error);
        return out;
    }
    if (code.empty() || !io.post) {
        out.location = fail_to(SAY("场记云未返回授权码"));
        return out;
    }
    const json req = {{"grant_type", "authorization_code"},
                      {"code", code},
                      {"code_verifier", p->verifier},
                      {"redirect_uri", p->redirect_uri}};
    const auto r = io.post(p->cloud_url + "/oauth/token", req.dump(), {{"Content-Type", "application/json"}},
                           kTimeoutS);
    if (r.status != 200) {
        out.location = fail_to(r.status == 0 ? SAYF("无法连接场记云：%1", reason_of(r))
                                             : SAYF("场记云未返回设备密钥：%1", reason_of(r)));
        return out;
    }
    const auto got = cloud::creds_from_token_reply(json::parse(r.body, nullptr, false), p->cloud_url,
                                                   cloud::now_ms());
    if (!got) {
        out.location = fail_to(SAYF("场记云未返回设备密钥：%1", std::string("device_key")));
        return out;
    }
    const auto have = cloud::load_creds();
    const bool bound = have && have->url == p->cloud_url;

    if (p->purpose == cloud::Purpose::enter) {
        // 用场记账号进门：**账号得是这台引擎的主人**。那把刚换来的钥匙用不着，当场吊销，
        // 不在账号页「我的设备」上留一行。
        revoke(io, p->cloud_url, got->device_key);
        if (!bound || have->account.id != got->account.id) {
            out.location = fail_to(SAY("此场记账号不是该引擎的所有者"));
            return out;
        }
        if (!ui_token.empty()) {
            out.set_cookie = "changji_ui=" + ui_token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000";
        }
        out.location = "/";
        return out;
    }

    // 绑：第一次登录就是主人；登着别人时不许悄悄换（先退出）
    if (bound && have->account.id != got->account.id) {
        revoke(io, p->cloud_url, got->device_key);
        out.location = fail_to(SAYF("此引擎已登录「%1」，请先退出登录再切换账号", have->account.name));
        return out;
    }
    if (bound) revoke(io, have->url, have->device_key);  // 同一个人重登：旧的那把收掉
    try {
        cloud::save_creds(*got);
    } catch (const std::exception& e) {
        out.location = fail_to(e.what());
        return out;
    }
    forget_cloud_cache();
    out.changed = true;
    out.location = "/?cloud=ok";
    return out;
}

ApiResult post_cloud_logout(const CloudIo& io) {
    if (const auto c = cloud::load_creds()) revoke(io, c->url, c->device_key);
    cloud::clear_creds();
    forget_cloud_cache();
    return {200, {{"ok", true}}};
}

}  // namespace changji::http
