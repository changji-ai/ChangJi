#include "http/llm_info.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "http/llm_providers.inc.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {


}  // namespace

ApiResult get_llm_providers() {
    static const json base = json::parse(stages::prompt::kLlmProvidersJson);

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
    json providers = json::array();
    for (const auto& p : base) {
        json one = p;
        const std::string url = p.value("base_url", std::string());
        one["key_set"] = config::has_own_api_key(url);
        for (const char* field : {"name", "note"}) {
            if (const auto it = one.find(field);
                it != one.end() && it->is_string()) {
                *it = SAY(it->get<std::string>());
            }
        }
        providers.push_back(std::move(one));
    }
    return {200, {{"providers", providers}}};
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
                      {"error", SAYF("连不上 %1：%2", url, *r.transport_error)},
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
                      {"error", SAYF("%1 返回的不是 JSON", url)},
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
