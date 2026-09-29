#include "http/llm_info.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cloud/cloud.hpp"
#include "config/model_index.hpp"
#include "config/runtime.hpp"
#include "config/writeback.hpp"
#include "http/llm_providers.inc.hpp"
#include "infer/llama_chat.hpp"
#include "infer/scheduler.hpp"
#include "llm/chat_pick.hpp"
#include "llm/local_client.hpp"
#include "util/chat_id.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

/// 地址比较前先抹掉尾巴上的 `/` 和空白：`…/v1` 和 `…/v1/` 是同一家。
std::string bare_url(std::string u) {
    u = text::strip_ws(u);
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

bool http_url(const std::string& u) {
    return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

/// 人加过的那几条。读不动、坏了都当一条没有——**一份手改坏了的文件不该让厂商
/// 单子整个打不开**。
json read_custom() {
    std::ifstream in(custom_providers_path(), std::ios::binary);
    if (!in) return json::array();
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_array()) return json::array();
    json out = json::array();
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        const auto str = [&](const char* k) {
            const auto it = e.find(k);
            return it != e.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        const std::string url = bare_url(str("base_url"));
        if (!http_url(url)) continue;
        out.push_back({{"id", str("id")}, {"name", str("name")}, {"base_url", url}});
    }
    return out;
}

void write_custom(const json& list) {
    const fs::path file = custom_providers_path();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw ApiError(500, SAYF("无法写入：%1", paths::to_utf8(tmp)));
        out << list.dump(1) << '\n';
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            throw ApiError(500, SAYF("无法写入：%1", paths::to_utf8(tmp)));
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw ApiError(500, SAYF("无法写入：%1", paths::to_utf8(file)));
    }
}

const json& builtin_providers() {
    static const json base = json::parse(stages::prompt::kLlmProvidersJson);
    return base;
}

/// 本机、局域网的地址（不要密钥的那一类）。**判据只有一份**：`LLMConfig::needs_api_key`。
bool local_address(const std::string& url) {
    config::LLMConfig c;
    c.base_url = url;
    return !c.needs_api_key();
}

/// 一个没用过的编号：`my-<毫秒>`，撞上了往后挪。
std::string fresh_id(const json& list) {
    auto n = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
    for (;; ++n) {
        const std::string id = "my-" + std::to_string(n);
        bool used = false;
        for (const auto& e : list) used = used || e.value("id", std::string()) == id;
        if (!used) return id;
    }
}

}  // namespace

fs::path custom_providers_path() {
    return paths::user_config_dir("changji") / "llm_providers.json";
}

// ---- 每一家的模型单子记在盘上 ----

fs::path models_cache_path(const std::string& base_url) {
    // 文件名和那一家的密钥同一个短名（`config::api_key_path_for`）：同一个地址认成
    // 同一家，只在一处算。
    return paths::user_config_dir("changji") / "llm_models" /
           (config::api_key_path_for(bare_url(base_url)).filename().native() +
            paths::from_utf8(".json").native());
}

namespace {

struct ModelsCache {
    std::vector<std::string> models;
    std::set<std::string> seen;
    bool exists = false;
};

ModelsCache read_models_cache(const std::string& base_url) {
    ModelsCache c;
    std::ifstream in(models_cache_path(base_url), std::ios::binary);
    if (!in) return c;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return c;
    c.exists = true;
    for (const char* k : {"models", "seen"}) {
        const auto it = j.find(k);
        if (it == j.end() || !it->is_array()) continue;
        for (const auto& v : *it) {
            if (!v.is_string()) continue;
            if (std::string(k) == "models") c.models.push_back(v.get<std::string>());
            else c.seen.insert(v.get<std::string>());
        }
    }
    return c;
}

void write_models_cache(const std::string& base_url, const ModelsCache& c) {
    const fs::path file = models_cache_path(base_url);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << json{{"base_url", bare_url(base_url)},
                    {"models", c.models},
                    {"seen", std::vector<std::string>(c.seen.begin(), c.seen.end())}}
                   .dump(1)
            << '\n';
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
}

json fresh_of(const ModelsCache& c) {
    json out = json::array();
    for (const auto& m : c.models) {
        if (!c.seen.count(m)) out.push_back(m);
    }
    return out;
}

}  // namespace

json remember_models(const std::string& base_url, json result) {
    if (base_url.empty() || !result.is_object()) return result;
    ModelsCache c = read_models_cache(base_url);
    std::vector<std::string> got;
    if (const auto it = result.find("models"); it != result.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_string()) got.push_back(v.get<std::string>());
        }
    }
    if (!got.empty()) {
        // 头一回问到这一家：全当看过——刚加上的一家满屏都是点，等于没有点。
        if (!c.exists) c.seen.insert(got.begin(), got.end());
        c.models = got;
        write_models_cache(base_url, c);
    } else if (!c.models.empty()) {
        // 这一回没问到（断网、那一家挂了）：拿上一次记下的顶上，原因照旧摆着。
        result["models"] = c.models;
        result["cached"] = true;
    }
    result["new"] = fresh_of(c);
    return result;
}

ApiResult post_llm_models_seen(const json& body) {
    const std::string url = bare_url(field_str(body, "base_url"));
    if (url.empty()) throw ApiError(400, SAY("缺少 base_url"));
    ModelsCache c = read_models_cache(url);
    if (!c.exists) return {200, {{"ok", true}}};
    c.seen.insert(c.models.begin(), c.models.end());
    write_models_cache(url, c);
    return {200, {{"ok", true}}};
}

// ---- 新对话用哪个：最后一次挑的 ----

void remember_default(const llm::ChatPick& p) {
    json llm_patch = json::object();
    json models_patch = json::object();
    config::Settings s = config::runtime().snapshot();
    if (p.backend == "remote" && !p.base_url.empty() && !p.model.empty()) {
        const bool moved = bare_url(s.llm.base_url) != bare_url(p.base_url);
        s.llm.backend = "remote";
        s.llm.base_url = p.base_url;
        s.llm.model = p.model;
        // 换了家，钥匙换成那一家自己名下那把（同 post_connections）。
        if (moved) s.llm.api_key = config::read_api_key_for(p.base_url);
        llm_patch = {{"backend", "remote"}, {"base_url", p.base_url}, {"model", p.model}};
    } else if (p.backend == "local" && !p.file.empty()) {
        s.llm.backend = "local";
        s.models.llm = p.file;
        llm_patch = {{"backend", "local"}};
        models_patch = {{"llm", p.file}};
    } else if (!p.effort.has_value() || p.effort->empty()) {
        return;   // 半截的（换了家还没挑模型）：不动默认
    }
    // 空串不写进全局：那不是一档（见 `llm/chat_pick.cpp` 的 pick_from_json）。
    if (p.effort.has_value() && !p.effort->empty()) {
        s.llm.reasoning_effort = *p.effort;
        llm_patch["reasoning_effort"] = *p.effort;
    }
    json patch = json::object();
    if (!llm_patch.empty()) patch["llm"] = llm_patch;
    if (!models_patch.empty()) patch["models"] = models_patch;
    try {
        config::save_user_config(patch);
    } catch (const std::exception& e) {
        throw ApiError(500, SAYF("无法保存默认模型：%1", e.what()));
    }
    config::runtime().replace(s);
    if (p.backend == "local") llm::ensure_llm_slot();
}

ApiResult post_llm_default(const json& body) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体须为 JSON 对象"));
    llm::ChatPick p;
    p.backend = field_str(body, "backend", "remote");
    if (p.backend == "remote") {
        p.base_url = bare_url(field_str(body, "base_url"));
        p.model = text::strip_ws(field_str(body, "model"));
        if (!http_url(p.base_url)) {
            throw ApiError(400, SAY("地址须以 http:// 或 https:// 开头，通常以 /v1 结尾"));
        }
        if (p.model.empty()) throw ApiError(400, SAY("请选择模型"));
    } else if (p.backend == "local") {
        p.file = field_str(body, "file");
        const config::Settings s = config::runtime().snapshot();
        const auto hit = config::find_model(s.models.dir_path(s.workspace_path()), p.file);
        if (p.file.empty() || !hit) {
            throw ApiError(400, SAYF("模型目录中没有此编剧模型：%1", p.file));
        }
    } else {
        throw ApiError(400, SAYF("无法识别的后端：%1（仅支持 remote / local）", p.backend));
    }
    remember_default(p);
    return {200, {{"ok", true}}};
}

ApiResult get_llm_providers() {
    const json& base = builtin_providers();

    // **每一家旁边多报一句"这把钥匙填过没有"。**
    //
    // 密钥本来就是**按地址各存一把**的（`api_keys/<地址短名>`，见
    // settings.hpp 上那段），只是一直没人问过"哪几家填过"。
    // 界面上那条「厂商」靠它决定给谁看：没填密钥的平台摆出来只有一个用处
    // ——让人挑中之后挨一个 401，而 401 长得像"这个模型不让你用"。
    //
    // ⚠️ **只报有没有，不报是什么。** 这条接口是 GET，会落进访问日志。
    //
    // 每次都重新读盘（一家一个小文件，十几个 stat）。不缓存是故意的：
    // 人在设置里刚填完一把，回来这条就得跟着变。
    // **翻译在这儿，不在那张表里。**
    //
    // `kLlmProvidersJson` 是一整块 JSON 数据，不是一条条字面量——就地包
    // `SAY()` 没地方包。和模型清单（`setup/catalog.cpp` + `setup_api.cpp`）
    // 同一条规矩：表里留中文原话，出 json 的这一处查表。
    //
    // **只翻 `name` 和 `note`。** `id` 和 `base_url` 是键：前者前端拿来认
    // 这一家（选中哪个就写哪个），后者要一字不差地发出去。
    //
    // 另外两栏（用户 2026-09-26）：`added` 进不进输入框底下那张厂商单子——填过
    // 密钥的、人按「加」加过的；`custom` 是不是人自己加的（清单上没有的那几家）。
    const json custom = read_custom();
    const auto in_custom = [&](const std::string& url) {
        for (const auto& e : custom) {
            if (bare_url(e.value("base_url", std::string())) == bare_url(url)) return true;
        }
        return false;
    };
    json providers = json::array();
    // **登着场记云就排第一家**（docs/账号与云服务方案.md 第四节）：不用配密钥，钥匙是设备钥匙
    // （cloud.json）。虚的一行：退出登录就没了，不进 llm_providers.json。名字是牌子，不翻。
    {
        const auto snap = config::runtime().snapshot();
        const std::string cloud_url = cloud::bare_url(snap.cloud.url);
        if (const auto c = cloud::load_creds(); c && !cloud_url.empty() && c->url == cloud_url) {
            providers.push_back({{"id", "cloud"},
                                 {"name", "场记云"},
                                 {"base_url", cloud::llm_base(cloud_url)},
                                 {"local", false},
                                 {"note", ""},
                                 {"key_set", true},
                                 {"added", true},
                                 {"custom", false},
                                 {"cloud", true}});
        }
    }
    for (const auto& p : base) {
        json one = p;
        const std::string url = p.value("base_url", std::string());
        const bool key_set = config::has_own_api_key(url);
        one["key_set"] = key_set;
        one["added"] = key_set || in_custom(url);
        one["custom"] = false;
        for (const char* field : {"name", "note"}) {
            if (const auto it = one.find(field);
                it != one.end() && it->is_string()) {
                *it = SAY(it->get<std::string>());
            }
        }
        providers.push_back(std::move(one));
    }
    // 人自己加的、清单上没有的那几家，排在后面。
    for (const auto& e : custom) {
        const std::string url = e.value("base_url", std::string());
        bool builtin = false;
        for (const auto& p : base) {
            builtin = builtin || bare_url(p.value("base_url", std::string())) == url;
        }
        if (builtin) continue;
        providers.push_back({{"id", e.value("id", std::string())},
                             {"name", e.value("name", std::string())},
                             {"base_url", url},
                             {"local", local_address(url)},
                             {"note", ""},
                             {"key_set", config::has_own_api_key(url)},
                             {"added", true},
                             {"custom", true}});
    }
    return {200, {{"providers", providers}}};
}

ApiResult post_llm_provider_add(const json& body) {
    const std::string name = text::strip_ws(field_str(body, "name"));
    const std::string url = bare_url(field_str(body, "base_url"));
    const std::string key = text::strip_ws(field_str(body, "api_key"));
    if (!http_url(url)) {
        throw ApiError(400, SAY("地址须以 http:// 或 https:// 开头，通常以 /v1 结尾"));
    }
    if (url.find_first_of(" \t\r\n") != std::string::npos) {
        throw ApiError(400, SAY("地址须以 http:// 或 https:// 开头，通常以 /v1 结尾"));
    }
    // 清单上那一家：名字照清单上的，不另起一条。
    std::string id;
    std::string shown = name;
    for (const auto& p : builtin_providers()) {
        if (bare_url(p.value("base_url", std::string())) == url) {
            id = p.value("id", std::string());
            shown = SAY(p.value("name", std::string()));
        }
    }
    if (shown.empty()) {
        throw ApiError(400, SAY("请填写名称（将显示在输入框下方的模型列表中）"));
    }
    if (shown.size() > 120) throw ApiError(400, SAY("名称过长"));

    json list = read_custom();
    bool found = false;
    for (auto& e : list) {
        if (bare_url(e.value("base_url", std::string())) != url) continue;
        e["name"] = shown;
        if (!id.empty()) e["id"] = id;
        id = e.value("id", std::string());
        found = true;
    }
    if (!found) {
        if (id.empty()) id = fresh_id(list);
        list.push_back({{"id", id}, {"name", shown}, {"base_url", url}});
    }
    // 先存密钥再记名单：密钥存不进去的话这一家挑了也是 401，别先摆上单子。
    if (!key.empty()) {
        try {
            config::write_api_key_for(url, key);
        } catch (const std::exception& e) {
            throw ApiError(500, SAYF("无法保存密钥：%1", e.what()));
        }
    }
    write_custom(list);
    return {200, {{"ok", true}, {"id", id}, {"name", shown}, {"base_url", url}}};
}

ApiResult post_llm_provider_update(const json& body) {
    const std::string id = field_str(body, "id");
    if (id.empty()) throw ApiError(400, SAY("缺少 id"));
    const bool has_name = body.is_object() && body.contains("name") && !body.at("name").is_null();
    const bool has_url =
        body.is_object() && body.contains("base_url") && !body.at("base_url").is_null();
    const std::string name = text::strip_ws(field_str(body, "name"));
    const std::string want_url = bare_url(field_str(body, "base_url"));
    const std::string key = text::strip_ws(field_str(body, "api_key"));

    // 清单上那一家：**只换密钥**。它的地址就是它是谁（换了地址就不是这一家了），
    // 名字跟着界面语言走——改了的话换个语言又变回去。
    std::string builtin_url;
    std::string builtin_name;
    for (const auto& p : builtin_providers()) {
        if (p.value("id", std::string()) == id) {
            builtin_url = bare_url(p.value("base_url", std::string()));
            builtin_name = SAY(p.value("name", std::string()));
        }
    }
    json list = read_custom();
    json* mine = nullptr;
    for (auto& e : list) {
        if (e.value("id", std::string()) == id ||
            (!builtin_url.empty() && bare_url(e.value("base_url", std::string())) == builtin_url)) {
            mine = &e;
        }
    }
    if (builtin_url.empty() && mine == nullptr) {
        throw ApiError(404, SAYF("列表中没有此服务：%1", id));
    }

    const std::string old_url =
        !builtin_url.empty() ? builtin_url : bare_url(mine->value("base_url", std::string()));
    std::string new_url = old_url;
    if (has_url && want_url != old_url) {
        if (!builtin_url.empty()) {
            // 说法照设置页上那颗按钮：2026-09-28 起叫「添加服务」（原来叫「加一家」）。
            throw ApiError(400, SAY("预设服务的地址不能修改；如需使用其他地址，请另行添加服务"));
        }
        if (!http_url(want_url) || want_url.find_first_of(" \t\r\n") != std::string::npos) {
            throw ApiError(400, SAY("地址须以 http:// 或 https:// 开头，通常以 /v1 结尾"));
        }
        // 撞上另一家的地址：两条指着同一个地方，密钥也只有一把，删哪条都连累另一条。
        for (const auto& p : builtin_providers()) {
            if (bare_url(p.value("base_url", std::string())) == want_url) {
                throw ApiError(409, SAYF("已有服务使用此地址：%1", SAY(p.value("name", std::string()))));
            }
        }
        for (const auto& e : list) {
            if (&e != mine && bare_url(e.value("base_url", std::string())) == want_url) {
                throw ApiError(409, SAYF("已有服务使用此地址：%1", e.value("name", std::string())));
            }
        }
        new_url = want_url;
    }
    std::string shown = !builtin_url.empty() ? builtin_name
                                             : (has_name && !name.empty()
                                                    ? name
                                                    : mine->value("name", std::string()));
    if (shown.empty()) throw ApiError(400, SAY("请填写名称（将显示在输入框下方的模型列表中）"));
    if (shown.size() > 120) throw ApiError(400, SAY("名称过长"));

    // ---- 密钥：给了新的就换；换了地址没给新的，旧的那把跟着搬过去 ----
    try {
        if (!key.empty()) {
            config::write_api_key_for(new_url, key);
        } else if (new_url != old_url && config::has_own_api_key(old_url)) {
            config::write_api_key_for(new_url, config::read_api_key_for(old_url));
        }
        if (new_url != old_url) config::write_api_key_for(old_url, "");
    } catch (const std::exception& e) {
        throw ApiError(500, SAYF("无法保存密钥：%1", e.what()));
    }

    if (mine == nullptr) {
        // 清单上那一家、原来只填过密钥没按过「加」：记成加过了。
        list.push_back({{"id", id}, {"name", shown}, {"base_url", new_url}});
    } else {
        (*mine)["name"] = shown;
        (*mine)["base_url"] = new_url;
    }
    write_custom(list);

    if (new_url != old_url) {
        // 记下的模型单子跟着搬（看过哪几个也还算数）。
        std::error_code ec;
        const fs::path from = models_cache_path(old_url);
        if (fs::exists(from, ec)) {
            fs::create_directories(models_cache_path(new_url).parent_path(), ec);
            fs::rename(from, models_cache_path(new_url), ec);
        }
        // **默认指着这一家的话跟着换**：不换就是新对话照旧去敲那个旧地址。
        const config::Settings s = config::runtime().snapshot();
        if (s.llm.backend == "remote" && bare_url(s.llm.base_url) == old_url) {
            llm::ChatPick d;
            d.backend = "remote";
            d.base_url = new_url;
            d.model = s.llm.model;
            remember_default(d);
        }
    }
    return {200, {{"ok", true},
                  {"id", id},
                  {"name", shown},
                  {"base_url", new_url},
                  {"moved", new_url != old_url}}};
}

ApiResult post_llm_provider_remove(const json& body) {
    const std::string id = field_str(body, "id");
    if (id.empty()) throw ApiError(400, SAY("缺少 id"));
    std::string url;
    for (const auto& p : builtin_providers()) {
        if (p.value("id", std::string()) == id) url = bare_url(p.value("base_url", std::string()));
    }
    const json list = read_custom();
    json kept = json::array();
    for (const auto& e : list) {
        const std::string u = bare_url(e.value("base_url", std::string()));
        if (e.value("id", std::string()) == id || (!url.empty() && u == url)) {
            url = u;
            continue;
        }
        kept.push_back(e);
    }
    if (url.empty()) throw ApiError(404, SAYF("列表中没有此服务：%1", id));
    write_custom(kept);
    try {
        config::write_api_key_for(url, "");
    } catch (const std::exception&) {
        // 名单上已经拿掉了；密钥文件删不掉的话它还算「填过密钥」，下次又摆回来。
        // 那颗按钮 2026-09-28 起叫「删除」（原来叫「拿掉」），回的话跟着它说。
        throw ApiError(500, SAY("已从列表中删除，但它的密钥文件无法删除（没有权限？）"));
    }
    return {200, {{"ok", true}}};
}

ApiResult get_llm_local(const config::Settings& settings) {
    const fs::path dir = settings.models.dir_path(settings.workspace_path());
    json models = json::array();
    const auto idx = config::index_models(dir);
    for (const auto& f : idx.files) {
        if (!llm::is_llm_weights(f.rel, f.bytes)) continue;
        models.push_back({{"name", paths::to_utf8(paths::from_utf8(f.rel).stem())},
                          {"file", f.rel},
                          {"bytes", f.bytes}});
    }
    // **显存那一行**：装没装、装的是哪一份、占了多少、闲着的话还有几秒就卸
    // （界面上「本地模型占着显存 · 5 分钟后释放 · 现在释放」）
    const auto st = infer::scheduler().slot_state(infer::Slot::LLM);
    json loaded = nullptr;
    if (st.loaded) {
        const std::string full = llm::loaded_llm_file();
        std::string rel = full;
        std::error_code ec;
        const auto r = fs::relative(paths::from_utf8(full), dir, ec);
        if (!ec && !r.empty() && paths::to_utf8(r).rfind("..", 0) != 0) {
            rel = paths::to_utf8(r);
            for (char& c : rel) if (c == '\\') c = '/';   // 和 models 那一列同一种写法
        }
        const std::size_t measured = infer::scheduler().measured_vram(infer::Slot::LLM);
        loaded = {{"file", rel},
                  {"name", paths::to_utf8(paths::from_utf8(full).stem())},
                  {"in_use", st.leases > 0},
                  {"vram_gb", measured > 0 ? json(static_cast<double>(measured) / (1024.0 * 1024 * 1024)) : json(nullptr)},
                  {"release_in_s", st.release_in_s ? json(*st.release_in_s) : json(nullptr)}};
    }
    return {200, {{"available", infer::llama_chat_available()},
                  {"dir", paths::to_utf8(dir)},
                  {"models", models},
                  {"truncated", idx.truncated},
                  {"loaded", loaded},
                  {"keep_alive_minutes", settings.llm.keep_alive_minutes}}};
}

ApiResult post_llm_local_release() {
    const auto st = infer::scheduler().slot_state(infer::Slot::LLM);
    if (!st.loaded) return {200, {{"released", false}, {"loaded", false}}};
    // 正在用（写着、对话着）不强卸：抽走正在用的模型是崩，不是一句报错。用完就按闲卸那条走。
    if (st.leases > 0) {
        throw ApiError(409, SAY("本地模型正在使用，使用结束后才能释放"));
    }
    const bool ok = infer::scheduler().evict(infer::Slot::LLM);
    return {200, {{"released", ok}, {"loaded", !ok}}};
}

namespace {

json effective_json(const config::Settings& s, const std::optional<llm::ChatPick>& pick) {
    const llm::ChatPick p = pick.value_or(llm::ChatPick{});
    json e;
    if (p.backend == "remote") {
        e = {{"backend", "remote"}, {"base_url", p.base_url}, {"model", p.model}, {"file", ""}};
    } else if (p.backend == "local") {
        e = {{"backend", "local"}, {"base_url", ""}, {"model", p.model}, {"file", p.file}};
    } else {
        std::string model = s.llm.model;
        std::string file;
        if (s.llm.backend == "local") {
            model = paths::to_utf8(paths::from_utf8(s.models.llm).stem());
            file = s.models.llm;
        } else if (s.llm.backend == "command") {
            model = s.llm.command;
        }
        e = {{"backend", s.llm.backend},
             {"base_url", s.llm.base_url},
             {"model", model},
             {"file", file}};
    }
    e["effort"] = p.effort.has_value() && !p.effort->empty() ? *p.effort
                                                              : s.llm.reasoning_effort;
    return e;
}

fs::path chat_dir_checked(const std::string& project, const std::string& chat) {
    if (!chat.empty() && !util::chat_id_ok(chat)) {
        throw ApiError(400, SAYF("对话编号只能包含字母、数字、`_` 和 `-`：%1", chat));
    }
    const fs::path root = llm::chat_root(project);
    std::error_code ec;
    if (!project.empty() && !fs::is_directory(root, ec)) {
        throw ApiError(404, SAYF("项目目录不存在：%1", project));
    }
    return root;
}

}  // namespace

ApiResult get_chat_model(const std::string& project, const std::string& chat,
                         const config::Settings& settings) {
    const fs::path root = chat_dir_checked(project, chat);
    const auto pick = llm::load_chat_pick(root, chat);
    return {200, {{"pick", pick ? llm::to_json(*pick) : json(nullptr)},
                  {"effective", effective_json(settings, pick)}}};
}

ApiResult post_chat_model(const json& body, const config::Settings& settings) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体须为 JSON 对象"));
    const std::string project = field_str(body, "project");
    const std::string chat = field_str(body, "chat");
    const fs::path root = chat_dir_checked(project, chat);

    llm::ChatPick p = llm::load_chat_pick(root, chat).value_or(llm::ChatPick{});
    if (field_bool(body, "reset", false)) p = llm::ChatPick{};

    const auto has = [&](const char* k) { return body.contains(k) && !body.at(k).is_null(); };

    if (has("backend")) {
        const std::string backend = field_str(body, "backend");
        if (backend == "remote") {
            const std::string url =
                bare_url(has("base_url") ? field_str(body, "base_url") : p.base_url);
            if (!http_url(url)) {
                throw ApiError(400, SAY("地址须以 http:// 或 https:// 开头，通常以 /v1 结尾"));
            }
            const bool moved = p.backend != "remote" || bare_url(p.base_url) != url;
            p.backend = "remote";
            p.base_url = url;
            p.file.clear();
            // 换了家又没带名字：清空，别带着上一家的名字去敲这一家。
            if (moved) p.model.clear();
        } else if (backend == "local") {
            if (!infer::llama_chat_available()) {
                throw ApiError(400, SAY("当前程序未包含本地大模型（构建时 CHANGJI_LLAMA=OFF），"
                                        "无法运行本地模型"));
            }
            const std::string file = field_str(body, "file");
            if (file.empty()) throw ApiError(400, SAY("本地运行需要选择一个模型文件"));
            const fs::path dir = settings.models.dir_path(settings.workspace_path());
            const auto hit = config::find_model(dir, file);
            std::error_code ec;
            const std::uint64_t bytes = hit ? fs::file_size(*hit, ec) : 0;
            if (!hit || !llm::is_llm_weights(file, ec ? 0 : bytes)) {
                throw ApiError(400, SAYF("模型目录中没有此编剧模型：%1", file));
            }
            p.backend = "local";
            p.base_url.clear();
            p.file = file;
            p.model = paths::to_utf8(paths::from_utf8(file).stem());
            llm::ensure_llm_slot();
        } else if (backend.empty() || backend == "global") {
            // 跟全局：后端那几栏清掉，想多久那一栏留着。
            p.backend.clear();
            p.base_url.clear();
            p.model.clear();
            p.file.clear();
        } else {
            throw ApiError(400, SAYF("无法识别的后端：%1（仅支持 remote / local）", backend));
        }
    }
    if (has("model") && p.backend != "local") {
        const std::string model = text::strip_ws(field_str(body, "model"));
        if (p.backend.empty()) {
            // 只挑了模型名、没挑过家：接在全局那一家上。
            if (settings.llm.backend != "remote") {
                throw ApiError(400, SAY("全局设置未使用 API 服务，请先选择服务再选择模型"));
            }
            p.backend = "remote";
            p.base_url = bare_url(settings.llm.base_url);
        }
        p.model = model;
    }
    if (has("effort")) {
        // 空串 = 这条对话不再自己挑，跟全局。**不是**「不发那个字段、随服务默认」
        // ——那一档 2026-09-27 去掉了（`llm/chat_pick.cpp` 的 pick_from_json 上说了
        // 为什么）。
        const std::string e = text::strip_ws(field_str(body, "effort"));
        if (e.empty()) {
            p.effort.reset();
        } else {
            p.effort = e;
        }
    }

    try {
        llm::save_chat_pick(root, chat, p);
    } catch (const std::exception& e) {
        throw ApiError(500, SAYF("无法保存此对话选择的模型：%1", e.what()));
    }
    // **最后一次挑的就是新对话的默认**（用户 2026-09-26：「在会话中选择模型后也要持久
    // 保存，下次新建会话使用最后一次使用的模型」）。挑了一半的（换了家还没挑模型）不算。
    // 这一次改了哪几栏就只动哪几栏：只挑了想多久，别把家和模型也写一遍。
    {
        llm::ChatPick d;
        if (has("backend") || has("model") || has("file")) {
            d.backend = p.backend;
            d.base_url = p.base_url;
            d.model = p.model;
            d.file = p.file;
        }
        if (has("effort")) d.effort = p.effort;
        const bool complete = (d.backend == "remote" && !d.model.empty()) ||
                              (d.backend == "local" && !d.file.empty());
        if (!complete) {
            d.backend.clear();
            d.base_url.clear();
            d.model.clear();
            d.file.clear();
        }
        if (!d.empty()) remember_default(d);
    }
    return get_chat_model(project, chat, config::runtime().snapshot());
}

ApiResult post_llm_models(const nlohmann::json& body,
                          const config::Settings& settings,
                          const HttpGet& fetch) {
    // 覆盖一份再走原来那条路：下面所有的判断（known_models 按地址给小抄、
    // 四条失败路径的说法）都按"这一家"来，不用抄第二遍。
    config::Settings s = settings;
    if (body.is_object()) {
        if (const auto it = body.find("base_url");
            it != body.end() && it->is_string()) {
            const std::string u = text::strip_ws(it->get<std::string>());
            if (!u.empty() && u != s.llm.base_url) {
                s.llm.base_url = u;
                // ⚠️ **换了地址就得换钥匙。** 密钥是**按地址各存一把**的
                //（`api_keys/<地址短名>`），而 `settings` 里那把是"上一家"
                // 的——拿它去问新的一家，回来必然是 401，而 401 长得像
                // "这个模型不让你用"。
                //
                // 2026-09-21 实撞：两家的密钥都存好了，界面上从 DeepSeek 切
                // 回智谱，模型列表空着说 401；同一把钥匙直接 curl 智谱的
                // `/models` 是 200。
                //
                // 没存过这一家就留空——空 Bearer 照旧 401，而那时候
                // "401" 正是实情（这家的钥匙真没填）。
                s.llm.api_key = config::read_api_key_for(u);
            }
        }
        if (const auto it = body.find("api_key");
            it != body.end() && it->is_string()) {
            const std::string k = text::strip_ws(it->get<std::string>());
            // 身上带着一把的优先（设置页里刚敲进框里、还没存下的那一把）。
            if (!k.empty()) s.llm.api_key = k;
        }
    }
    return get_llm_models(s, fetch);
}

ApiResult get_llm_models(const config::Settings& settings, const HttpGet& fetch) {
    const std::string url = settings.llm.base_url + "/models";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + settings.llm.api_key}};

    // **本机 gguf 那一段 2026-09-14 去掉了。** 进程内后端删了之后，
    // 列出来的文件一个都选不了——摆着只会让人以为还能在本机跑。
    // 字段留着是给前端的：少一个键会让老页面在取值时炸，而这一层
    // 没法知道对面是不是新版。
    json local = {{"dir", ""}, {"files", json::array()}, {"current", ""}};

    // 我们认识的这家有什么，见 known_models。**四条返回路径都带上它**，
    // 尤其是失败那三条：刚装好还没填密钥时 `/models` 必然 401，而那正是
    // 用户最需要「这家都有什么、该挑哪个」的时候。
    //
    // 它和 `models` **是两个字段，不合并**。`models` 是这台服务此刻真答
    // 应的东西，前端那句「这台服务上没有 X」靠它判；混进我们的小抄之后
    // 那句话就会在模型真的不存在时也不吭声。合并交给前端去做。
    json known = json::array();
    for (const auto& [id, note] : llm::known_models(settings.llm.base_url)) {
        known.push_back({{"id", id}, {"note", note}});
    }

    // 超时写死 10 秒，不用 llm.timeout_s。那个是给生成用的，默认 300 秒——
    // 拿它来问一个列表，服务不在的时候设置页会转五分钟圈。
    const llm::HttpResponse r = fetch(url, headers, 10.0);

    // 失败的三种情况都返回空列表加一句原因，界面退回手打。
    // **不抛异常**：列不出来不该让整个设置页打不开。
    if (r.transport_error.has_value()) {
        return {200, {{"models", json::array()},
                      {"error", SAYF("无法连接 %1：%2", url, *r.transport_error)},
                      {"local", local},
                      {"known", known}}};
    }
    if (r.status >= 400) {
        return {200, {{"models", json::array()},
                      {"error", SAYF("%1 返回 %2", url, std::to_string(r.status))},
                      {"local", local},
                      {"known", known}}};
    }
    const json body = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
        return {200, {{"models", json::array()},
                      {"error", SAYF("%1 返回的内容不是 JSON", url)},
                      {"local", local},
                      {"known", known}}};
    }

    // OpenAI 兼容接口回 {"data": [{"id": ...}]}，也有直接回数组的
    const json items = (body.is_object() && body.contains("data"))
                           ? body["data"]
                           : body;
    std::set<std::string> names;   // set 顺带做了去重和排序
    if (items.is_array()) {
        for (const auto& item : items) {
            std::string name;
            if (item.is_object()) {
                const auto it = item.find("id");
                if (it != item.end() && !it->is_null()) {
                    name = it->is_string() ? it->get<std::string>() : it->dump();
                }
            } else if (item.is_string()) {
                name = item.get<std::string>();
            } else if (!item.is_null()) {
                name = item.dump();
            }
            if (!name.empty()) names.insert(name);
        }
    }

    // 成功时**没有 error 键**，失败时没有 current 键。两种形状不一样，
    // 照抄 Python。前端两个都用 ?? 兜着，但形状是契约。
    return {200, {
        {"models", std::vector<std::string>(names.begin(), names.end())},
        {"current", settings.llm.model},
        {"local", local},
        {"known", known},
    }};
}

}  // namespace changji::http
