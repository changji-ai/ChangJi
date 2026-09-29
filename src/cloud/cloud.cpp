#include "cloud/cloud.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#include "util/atomic_file.hpp"
#include "util/paths.hpp"
#include "util/sha256.hpp"
#include "util/text.hpp"

namespace changji::cloud {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

json to_json(const Account& a) {
    return {{"id", a.id},         {"name", a.name},     {"avatar", a.avatar},
            {"provider", a.provider}, {"status", a.status}, {"role", a.role}};
}

Account account_from_json(const json& j) {
    Account a;
    if (!j.is_object()) return a;
    const auto s = [&j](const char* k) {
        const auto it = j.find(k);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    a.id = s("id");
    a.name = s("name");
    a.avatar = s("avatar");
    a.provider = s("provider");
    a.status = s("status");
    a.role = s("role");
    return a;
}

// ---- 凭据文件 ----

fs::path creds_path() { return paths::user_config_dir("changji") / "cloud.json"; }

std::optional<Creds> load_creds() {
    std::ifstream in(creds_path(), std::ios::binary);
    if (!in) return std::nullopt;
    const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    Creds c;
    c.url = bare_url(j.value("url", std::string()));
    c.device_key = j.value("device_key", std::string());
    c.device_id = j.value("device_id", std::string());
    c.account = account_from_json(j.value("account", json::object()));
    if (const auto it = j.find("bound_at"); it != j.end() && it->is_number_integer()) {
        c.bound_at = it->get<std::int64_t>();
    }
    if (c.url.empty() || c.device_key.empty()) return std::nullopt;
    return c;
}

void save_creds(const Creds& c) {
    const json j = {{"url", c.url},
                    {"device_key", c.device_key},
                    {"device_id", c.device_id},
                    {"account", to_json(c.account)},
                    {"bound_at", c.bound_at}};
    std::error_code ec;
    fs::create_directories(creds_path().parent_path(), ec);
    util::write_file_atomic(creds_path(), j.dump(1, ' ', false, json::error_handler_t::replace),
                            /*private_only=*/true);
}

void clear_creds() {
    std::error_code ec;
    fs::remove(creds_path(), ec);
}

// ---- 地址 ----

std::string bare_url(const std::string& url) {
    std::string s = text::strip_ws(url);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string llm_base(const std::string& cloud_url) { return bare_url(cloud_url) + "/v1"; }
std::string render_url(const std::string& cloud_url) { return bare_url(cloud_url) + "/render"; }

std::optional<std::string> key_for(const std::string& base_url) {
    const auto c = load_creds();
    if (!c) return std::nullopt;
    if (bare_url(base_url) != llm_base(c->url)) return std::nullopt;
    return c->device_key;
}

bool is_cloud_node(const std::string& url) {
    const auto c = load_creds();
    return c && bare_url(url) == render_url(c->url);
}

std::optional<VirtualNode> virtual_node(const std::string& cloud_url) {
    if (bare_url(cloud_url).empty()) return std::nullopt;
    const auto c = load_creds();
    if (!c || c->url != bare_url(cloud_url)) return std::nullopt;
    return VirtualNode{render_url(c->url), c->device_key};
}

// ---- 登录那一趟 ----

std::string random_token(std::size_t bytes) {
    std::random_device rd;
    std::string raw(bytes, '\0');
    for (auto& ch : raw) ch = static_cast<char>(rd() & 0xff);
    return util::base64url(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

std::string pkce_challenge(const std::string& verifier) {
    const auto d = util::sha256_bytes(verifier);
    return util::base64url(d.data(), d.size());
}

namespace {

/// 主机名（去掉端口、方括号、小写）。
std::string host_only(std::string h) {
    h = text::strip_ws(h);
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!h.empty() && h.front() == '[') {
        const auto close = h.find(']');
        return close == std::string::npos ? h.substr(1) : h.substr(1, close - 1);
    }
    if (const auto colon = h.find(':'); colon != std::string::npos) h.erase(colon);
    return h;
}

/// 点分四段的 IPv4；不是就回 false。
bool ipv4(const std::string& h, std::array<int, 4>& out) {
    int n = 0;
    std::size_t i = 0;
    while (n < 4) {
        std::size_t j = i;
        while (j < h.size() && std::isdigit(static_cast<unsigned char>(h[j])) != 0) ++j;
        if (j == i || j - i > 3) return false;
        out[n] = std::stoi(h.substr(i, j - i));
        if (out[n] > 255) return false;
        ++n;
        if (n == 4) return j == h.size();
        if (j >= h.size() || h[j] != '.') return false;
        i = j + 1;
    }
    return false;
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

}  // namespace

bool redirect_host_ok(const std::string& host) {
    const std::string h = host_only(host);
    if (h == "127.0.0.1" || h == "localhost" || h == "::1") return true;
    std::array<int, 4> a{};
    if (!ipv4(h, a)) return false;
    if (a[0] == 127 || a[0] == 10) return true;
    if (a[0] == 172 && a[1] >= 16 && a[1] <= 31) return true;
    return a[0] == 192 && a[1] == 168;
}

std::string PendingLogins::issue(Pending p, std::int64_t now) {
    std::lock_guard lg(mu_);
    for (auto it = map_.begin(); it != map_.end();) {
        if (now - it->second.at > 10 * 60 * 1000) it = map_.erase(it);
        else ++it;
    }
    p.at = now;
    std::string state = random_token(18);
    map_[state] = std::move(p);
    return state;
}

std::optional<Pending> PendingLogins::take(const std::string& state, std::int64_t now) {
    std::lock_guard lg(mu_);
    const auto it = map_.find(state);
    if (state.empty() || it == map_.end()) return std::nullopt;
    Pending p = std::move(it->second);
    map_.erase(it);
    if (now - p.at > 10 * 60 * 1000) return std::nullopt;
    return p;
}

PendingLogins& pending() {
    static PendingLogins p;
    return p;
}

std::string authorize_url(const std::string& cloud_url, const std::string& redirect_uri,
                          const std::string& state, const std::string& challenge,
                          const std::string& device_name, const std::string& platform,
                          const std::string& provider) {
    std::string u = bare_url(cloud_url) + "/oauth/authorize?response_type=code&client_id=changji-engine";
    u += "&redirect_uri=" + pct(redirect_uri);
    u += "&state=" + pct(state);
    u += "&code_challenge=" + pct(challenge) + "&code_challenge_method=S256";
    u += "&device_name=" + pct(device_name);
    u += "&platform=" + pct(platform);
    // 只剩 Google 一家（2026-09-27 去掉了微信登录，server/src/oauth.js）
    if (provider == "google") u += "&provider=" + provider;
    return u;
}

std::optional<Creds> creds_from_token_reply(const json& j, const std::string& cloud_url,
                                            std::int64_t now) {
    if (!j.is_object()) return std::nullopt;
    Creds c;
    c.url = bare_url(cloud_url);
    c.device_key = j.value("device_key", std::string());
    c.device_id = j.value("device_id", std::string());
    c.account = account_from_json(j.value("account", json::object()));
    c.bound_at = now;
    if (c.device_key.empty() || c.account.id.empty()) return std::nullopt;
    return c;
}

std::string device_name() {
    std::string n = paths::env("COMPUTERNAME");
    if (n.empty()) n = paths::env("HOSTNAME");
    if (n.empty()) {
        std::ifstream in("/etc/hostname");
        std::getline(in, n);
    }
    n = text::strip_ws(n);
    return n.empty() ? std::string("场记") : n;
}

std::string platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

}  // namespace changji::cloud
