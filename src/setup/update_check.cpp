#include "setup/update_check.hpp"

#include <chrono>
#include <mutex>
#include <ratio>

#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::setup {

namespace {

/// 发布在哪个仓库。**空就用默认那个**（配置里写了空串也不该拼出一个坏地址）。
std::string repo_of(const config::UpdateConfig& cfg) {
    return cfg.repo.empty() ? config::UpdateConfig{}.repo : cfg.repo;
}

/// 是不是打 tag 出来的正式版：`v` 后面只有数和点（`v2.3`、`v2.3.1`）。
bool is_release_version(const std::string& v) {
    if (v.size() < 2 || v[0] != 'v') return false;
    bool digit = false;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const char c = v[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '.') return false;
    }
    return digit;
}

}  // namespace

std::string update_channel(const config::UpdateConfig& cfg, const std::string& current) {
    if (!cfg.channel.empty()) return cfg.channel == "beta" ? "beta" : "release";
    return is_release_version(text::strip_ws(current)) ? "release" : "beta";
}

std::string version_json_url(const config::UpdateConfig& cfg, const std::string& current) {
    return "https://github.com/" + repo_of(cfg) + "/releases/download/" +
           update_channel(cfg, current) + "/version.json";
}

bool is_different_version(const std::string& current,
                          const std::string& latest) {
    const std::string a = text::strip_ws(current);
    const std::string b = text::strip_ws(latest);
    // 任一边不知道就当没有新版：**报一个假的"有新版"比不报更糟**，
    // 人点过去发现下不到。
    if (a.empty() || b.empty()) return false;
    return a != b;
}

UpdateInfo check_update(const config::UpdateConfig& cfg,
                        const std::string& current, const Fetch& fetch) {
    UpdateInfo out;
    out.current = text::strip_ws(current);
    out.url = "https://github.com/" + repo_of(cfg) + "/releases/tag/" +
              update_channel(cfg, current);
    if (!fetch) {
        out.error = SAY("没法发请求");
        return out;
    }
    const std::string where = version_json_url(cfg, current);
    const std::string body = fetch(where);
    if (body.empty()) {
        out.error = SAYF("取不到版本信息（%1）", where);
        return out;
    }
    const auto js = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (js.is_discarded() || !js.is_object()) {
        // **不把原文贴出来。** 取到的多半是一张 404 页面，几十 KB 的 HTML
        // 摆进界面没人读得下去。
        out.error = SAY("那头回的不是版本信息");
        return out;
    }
    out.latest = text::strip_ws(js.value("version", std::string()));
    out.built_at = js.value("built_at", std::string());
    if (out.latest.empty()) {
        out.error = SAY("版本信息里没有 version 这一栏");
        return out;
    }
    out.newer = is_different_version(out.current, out.latest);
    return out;
}

UpdateInfo cached_update(UpdateCache& c, const config::UpdateConfig& cfg,
                         const std::string& current, const Fetch& fetch,
                         bool force) {
    {
        std::lock_guard lg(c.mu);
        if (!force) {
            // 关着就一次都不问。**手上没有答案时也别编一个**——回一份只填了
            // 「手上这个是哪一版」的，界面照着说"没查"。
            if (!cfg.auto_check) {
                UpdateInfo out;
                out.current = current;
                if (c.has) out = c.last;
                out.error = c.has ? out.error : SAY("自动检查关着");
                return out;
            }
            if (c.has) {
                const auto age = std::chrono::steady_clock::now() - c.at;
                const double hours =
                    std::chrono::duration<double, std::ratio<3600>>(age).count();
                // every_hours <= 0：起服务后问过一次就不再自己问。
                if (cfg.every_hours <= 0 || hours < cfg.every_hours) return c.last;
            }
        }
    }
    UpdateInfo fresh = check_update(cfg, current, fetch);
    std::lock_guard lg(c.mu);
    // **问砸了不要盖掉上一次问到的那份。** 网断一下就把"有新版"抹成
    // "取不到"，而那条消息本来是对的。
    if (fresh.error.empty() || !c.has) {
        c.last = fresh;
        c.has = true;
        c.at = std::chrono::steady_clock::now();
    }
    return c.has ? c.last : fresh;
}

}  // namespace changji::setup
