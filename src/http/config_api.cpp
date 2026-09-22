#include "http/config_api.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config/field_names.inc.hpp"
#include "config/runtime.hpp"
#include "config/writeback.hpp"
#include "infer/scheduler.hpp"
#include "llm/local_client.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::config;

/// 字段名 -> 标签。表在 `config/field_names.inc.hpp`，那儿存的是中文原文
/// （`SAY_NOOP()` 包着），**这儿查出来之后才真翻**——表是 `constexpr` 的，
/// 就地 `SAY()` 会把语言冻在静态初始化那一刻。
const std::map<std::string, std::string>& field_names() {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> out;
        for (const auto& row : stages::prompt::kFieldNames) {
            out[row[0]] = row[1];
        }
        return out;
    }();
    return m;
}

/// 字段名 -> {配置节, 节内键名}。
const std::map<std::string, std::pair<std::string, std::string>>&
setting_sections() {
    static const std::map<std::string, std::pair<std::string, std::string>> m =
        [] {
            std::map<std::string, std::pair<std::string, std::string>> out;
            for (const auto& row : stages::prompt::kSettingSections) {
                out[row[0]] = {row[1], row[2]};
            }
            return out;
        }();
    return m;
}

/// 报「已应用 min_frame_similarity」不如报「与首帧相似度下限」。
/// 界面上的标签是中文，回执用英文字段名，用户得自己对着猜是哪一项。
json labels_for(const std::vector<std::string>& keys) {
    json out = json::array();
    for (const auto& k : keys) {
        const auto it = field_names().find(k);
        out.push_back(it != field_names().end() ? SAY(it->second) : k);
    }
    return out;
}

// 形状收在 `doctor::to_json` 里，见那儿的说明。这一处只取 checks 那一半。
json checks_json(const doctor::Report& r) {
    return doctor::to_json(r).at("checks");
}

void forbid_extra(const json& body, const std::set<std::string>& allowed,
                  const char* where) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体要是一个对象"));
    for (const auto& kv : body.items()) {
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(std::string(where) + kv.key(),
                                    "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 取一个数，类型不对抛 422。
double need_num(const json& v, const std::string& key) {
    if (!v.is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", v,
                                "float_type");
    }
    return v.get<double>();
}

int need_int(const json& v, const std::string& key) {
    if (!v.is_number_integer()) {
        throw unprocessable_top(key, "Input should be a valid integer", v,
                                "int_type");
    }
    return v.get<int>();
}

std::string need_string(const json& v, const std::string& key) {
    if (!v.is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", v,
                                "string_type");
    }
    return v.get<std::string>();
}

bool need_bool(const json& v, const std::string& key) {
    if (!v.is_boolean()) {
        throw unprocessable_top(key, "Input should be a valid boolean", v,
                                "bool_type");
    }
    return v.get<bool>();
}

/// 把 validate() 收集的错误压成一句人话，字段名换成中文。
///
/// 原样抛出去的话用户看到的是一串英文字段名，
/// 真正有用的那句提示埋在中间，等于没提示。
std::string readable(const std::vector<std::string>& errs) {
    std::string out;
    for (std::size_t i = 0; i < errs.size(); ++i) {
        if (i) out += SAY("；");
        std::string line = errs[i];
        // 错误消息形如 "llm.temperature 必须在 0 和 2 之间"，
        // 把前面那个字段名换成中文标签
        const std::size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            std::string field = line.substr(0, sp);
            const std::size_t dot = field.rfind('.');
            const std::string bare =
                dot == std::string::npos ? field : field.substr(dot + 1);
            const auto it = field_names().find(bare);
            if (it != field_names().end()) {
                line = SAY(it->second) + line.substr(sp);
            }
        }
        out += line;
    }
    return out;
}

/// patch 里那个 key 归哪个配置节。connections 的键都是 `节_字段` 的形式。
std::pair<std::string, std::string> split_conn_key(const std::string& key) {
    const std::size_t us = key.find('_');
    if (us == std::string::npos) return {key, ""};
    return {key.substr(0, us), key.substr(us + 1)};
}

}  // namespace

ApiResult get_connections() {
    const Settings s = runtime().snapshot();
    const std::string key = s.llm.api_key;

    // 只回一个"设了没有"和一个掐头去尾的提示。回明文的话它会进浏览器的
    // 网络面板、进前端的状态、进任何一次截图。
    //
    // 头尾按**字符**取，不按字节。密钥正常是 ASCII，但粘贴时带进一个
    // 全角空格落在两端并不稀奇——按字节掐会留下半个字符，这个 hint
    // 序列化成 JSON 时 nlohmann 抛 type_error.316，整个连接设置页回 500，
    // 而且用户看不出是密钥两端多了个字符。
    const auto chars = text::utf8_chars(key);
    std::string hint = "***";
    if (chars.size() > 6) {
        hint = chars[0] + chars[1] + "***" + chars[chars.size() - 2] +
               chars.back();
    }

    json env = json::object();
    for (const auto& kv : env_overridden()) env[kv.first] = kv.second;

    return {200, {
        // 大模型跑在哪：remote（打 API）或 command（跑本机的命令行）。
        // 界面上那个「服务」下拉靠它决定摆哪一组输入框。
        {"llm_backend", s.llm.backend},
        {"llm_command", s.llm.command},
        {"llm_command_args", s.llm.command_args},
        {"llm_command_timeout_s", s.llm.command_timeout_s},
        {"llm_base_url", s.llm.base_url},
        {"llm_model", s.llm.model},
        {"llm_api_key_set", !key.empty()},
        {"llm_api_key_hint", hint},
        {"llm_temperature", s.llm.temperature},
        // 进程内那条：让不让模型先想再写，以及现在指着哪份权重（只读，
        // 换权重走模型窗那条下载的路）。
        {"llm_thinking", s.llm.thinking},
        // 远端那条想多久。空串 = 不发这个字段（随服务默认）。
        {"llm_reasoning_effort", s.llm.reasoning_effort},
        {"llm_local_model", s.models.llm},
        // 提示词日志：开关 + 正文文件的磁盘上限（MB）。
        // **走接口而不是只留在 config.toml 里**，理由是这两项是「随手要动」
        // 的：拷项目给别人之前先关掉、磁盘快满了先调小——让人为这个去改 toml
        // 再重启，多半就不关了。
        // ⚠️ **界面上还没有这两个控件**（`webapp/client/src` 里搜不到
        // `call_log`），所以现在只有直接打 `/api/connections` 这一条路。
        // 补界面的时候记着：`call_log_max_mb` 填离谱的数会被 `validate()`
        // 挡在 400 上（见 settings.cpp 那段），控件给个数字框加上下界就够。
        {"llm_call_log", s.llm.call_log},
        {"llm_call_log_max_mb", s.llm.call_log_max_mb},
        {"tts_backend", s.tts.backend},
        {"tts_base_url", s.tts.base_url.value_or("")},
        {"vram_gb_override", s.vram_gb_override.has_value()
                                 ? json(*s.vram_gb_override)
                                 : json(nullptr)},
        {"config_file", paths::to_utf8(user_config_path())},
        // 被环境变量顶住的字段，改了也是白改，界面要说出来
        {"env_locked", env},
    }};
}

ApiResult post_connections(const json& body, const DoctorFn& check) {
    forbid_extra(body, {"patch", "persist"}, "");
    const auto pit = body.find("patch");
    if (pit == body.end() || !pit->is_object()) {
        // input 是整个请求体，不是 null。见别的 need_str 那几处的注释。
        throw unprocessable_top("patch", "Field required", body, "missing");
    }
    const json& patch = *pit;
    static const std::set<std::string> kAllowed = {
        "llm_base_url", "llm_model", "llm_api_key", "llm_temperature",
        "llm_call_log", "llm_call_log_max_mb",
        "llm_backend", "llm_command", "llm_command_args", "llm_command_timeout_s",
        "llm_thinking", "llm_reasoning_effort",
        "tts_backend", "tts_base_url", "vram_gb_override"};
    forbid_extra(patch, kAllowed, "patch.");

    // 正在跑的时候换机器会把这一章跑坏：前半章是一台机器出的，
    // 后半章是另一台，画风对不上。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, SAY("正在跑，这时候换机器会把这一章跑坏"));
    }

    // 值为 null 的当作没给，对应 pydantic 的 exclude_none=True
    std::map<std::string, json> data;
    for (const auto& kv : patch.items()) {
        if (!kv.value().is_null()) data[kv.key()] = kv.value();
    }
    if (data.empty()) throw ApiError(400, SAY("没有要改的项"));

    Settings s = runtime().snapshot();
    std::vector<std::string> changed;

    const auto note = [&changed](const std::string& key, bool differs) {
        if (differs) changed.push_back(key);
    };

    for (const auto& kv : data) {
        const std::string& key = kv.first;
        const json& v = kv.second;
        if (key == "vram_gb_override") continue;   // 下面单独处理

        const auto [section, field] = split_conn_key(key);
        if (section == "llm") {
            if (field == "base_url") {
                const auto x = need_string(v, key);
                note(key, s.llm.base_url != x);
                s.llm.base_url = x;
            } else if (field == "model") {
                const auto x = need_string(v, key);
                note(key, s.llm.model != x);
                s.llm.model = x;
            } else if (field == "api_key") {
                const auto x = need_string(v, key);
                note(key, s.llm.api_key != x);
                s.llm.api_key = x;
            } else if (field == "temperature") {
                const auto x = need_num(v, key);
                note(key, s.llm.temperature != x);
                s.llm.temperature = x;
            } else if (field == "call_log") {
                const auto x = need_bool(v, key);
                note(key, s.llm.call_log != x);
                s.llm.call_log = x;
            } else if (field == "call_log_max_mb") {
                const auto x = need_num(v, key);
                note(key, s.llm.call_log_max_mb != x);
                s.llm.call_log_max_mb = x;
            } else if (field == "backend") {
                const auto x = need_string(v, key);
                note(key, s.llm.backend != x);
                s.llm.backend = x;
            } else if (field == "thinking") {
                const auto x = need_bool(v, key);
                note(key, s.llm.thinking != x);
                s.llm.thinking = x;
            } else if (field == "reasoning_effort") {
                // 档位对不对由 `LLMConfig::validate()` 统一判——这儿只收字符串。
                // 两处各写一份判据的话，迟早接口放行的值起服务时起不来。
                const auto x = need_string(v, key);
                note(key, s.llm.reasoning_effort != x);
                s.llm.reasoning_effort = x;
            } else if (field == "command") {
                const auto x = need_string(v, key);
                note(key, s.llm.command != x);
                s.llm.command = x;
            } else if (field == "command_args") {
                // **整份替换，不合并。** 参数的顺序和配对（`--model` 跟着
                // 它的值）是一体的，合并出来的组合谁也说不清。
                if (!v.is_array()) {
                    throw unprocessable("patch", key,
                                        SAY("要是一个字符串数组"), v,
                                        "list_type");
                }
                std::vector<std::string> xs;
                for (const auto& one : v) {
                    if (!one.is_string()) {
                        throw unprocessable("patch", key,
                                            SAY("数组里每一项都要是字符串"), v,
                                            "string_type");
                    }
                    xs.push_back(one.get<std::string>());
                }
                note(key, s.llm.command_args != xs);
                s.llm.command_args = xs;
            } else if (field == "command_timeout_s") {
                const auto x = need_num(v, key);
                note(key, s.llm.command_timeout_s != x);
                s.llm.command_timeout_s = x;
            }
        } else if (section == "tts") {
            if (field == "backend") {
                const auto x = need_string(v, key);
                note(key, s.tts.backend != x);
                s.tts.backend = x;
            } else if (field == "base_url") {
                const auto x = need_string(v, key);
                note(key, s.tts.base_url.value_or("") != x);
                // 空串当作没配。留一个空串的话，backend 选 http 时
                // 会拿它去连，报的错是"连不上 "——后面什么都没有。
                s.tts.base_url = x.empty() ? std::optional<std::string>()
                                           : std::optional<std::string>(x);
            }
        }
    }

    if (data.count("vram_gb_override")) {
        const json& v = data.at("vram_gb_override");
        const double x = need_num(v, "vram_gb_override");
        if (s.vram_gb_override.value_or(0.0) != x) {
            changed.push_back("vram_gb_override");
        }
        // 对应 Python 的 `data[...] or None`：0 当作没设
        s.vram_gb_override = x == 0.0 ? std::optional<double>()
                                      : std::optional<double>(x);
    }

    const auto errs = s.validate();
    if (!errs.empty()) throw ApiError(400, readable(errs));
    // 以前这里有意只放 comfy 和 http 过，为的是和 Python 的
    // `if new_tts.backend not in ("comfy", "http")` 一字不差。
    // **ComfyUI 拆掉之后那条限制没有意义了**：comfy 已经不是合法取值，
    // 而 local 是现在的默认。照旧不放别的取值过。
    if (s.tts.backend != "local" && s.tts.backend != "http") {
        throw ApiError(400, SAY("配音后端只能是 local 或 http"));
    }
    if (s.tts.backend == "http" && !s.tts.base_url.has_value()) {
        throw ApiError(400, SAY("配音后端选 http 就必须填地址"));
    }

    // ⚠️ **换了地址就得换钥匙，在这一句之前。**
    //
    // 这一路是"拿快照打补丁再换上去"，而补丁里只有地址、没有密钥时，
    // `s.llm.api_key` **还是上一家那把**——于是换到一家没配过的服务，
    // 请求真带着上一家的钥匙发了出去。那不是 401，那是**把一家的密钥送到
    // 另一家的服务器上**。
    //
    // 2026-09-21 实撞：配好 DeepSeek 之后切到月之暗面（从没配过），
    // 设置页说「已经填过 sk***a8」，而那是 DeepSeek 那把。
    //
    // 补丁里自带密钥的不动它（换一家 + 填新钥匙是同一个动作里的事，
    // 而那把还没落盘）。没配过那一家就留空——那时候空着才是实情。
    if (std::find(changed.begin(), changed.end(), "llm_base_url") != changed.end() &&
        std::find(changed.begin(), changed.end(), "llm_api_key") == changed.end()) {
        s.llm.api_key = config::read_api_key_for(s.llm.base_url);
    }

    runtime().replace(s);
    // 模型窗里把编剧「跑在哪」切成本机：把大模型那个槽当场挂上调度器
    //（起服务时配的是远端的话那一步什么都没注册）。注册过就不会再注册。
    llm::register_llm_slot([] { return runtime().snapshot(); }, runtime().profile());

    // **改成外接配音就把显存里那份放掉。**
    //
    // 用户切这一项多半就是为了腾显存——显存不够时，体检和
    // out_of_vram_message 都在劝他走这条路（"配音这一步不必占显存：
    // 把 [tts].backend 改成 http…"）。可光改配置的话，进程内那份权重
    // 还原封不动占着，要等到下一次腾地方才顺带被卸掉。用户照着提示做了、
    // 显存没少，只会以为这一项没生效。
    //
    // 正在念的时候 evict 返回 false，不强卸——那会让正在跑的那次合成
    // 段错误。它已经不会再被借，下一次腾地方时就走了。
    //
    // 先看装着没有再动手：evict 的语义是"事后不装着就算成功"，
    // 槽压根没装也返回 true。见 Scheduler::evict。
    if (s.tts.backend != "local" &&
        infer::scheduler().loaded(infer::Slot::TTS)) {
        infer::scheduler().evict(infer::Slot::TTS);
    }

    json saved_to = nullptr;
    if (opt_bool(body, "persist", true) && !changed.empty()) {
        json payload = json::object();
        for (const auto& key : changed) {
            if (key == "vram_gb_override") {
                payload["vram_gb_override"] =
                    s.vram_gb_override.has_value() ? json(*s.vram_gb_override)
                                                   : json("");
                continue;
            }
            const auto [section, field] = split_conn_key(key);
            // **密钥不进 config.toml**，单独一个文件，见
            // config::user_api_key_path 上那段注释。这里 continue 掉，
            // 写文件那一下在循环外面。
            if (section == "llm" && field == "api_key") continue;
            json value;
            if (section == "llm") {
                if (field == "base_url") value = s.llm.base_url;
                else if (field == "model") value = s.llm.model;
                else if (field == "temperature") value = s.llm.temperature;
                else if (field == "call_log") value = s.llm.call_log;
                else if (field == "call_log_max_mb") value = s.llm.call_log_max_mb;
                else if (field == "backend") value = s.llm.backend;
                else if (field == "thinking") value = s.llm.thinking;
                else if (field == "reasoning_effort")
                    value = s.llm.reasoning_effort;
                else if (field == "command") value = s.llm.command;
                else if (field == "command_args") value = s.llm.command_args;
                else if (field == "command_timeout_s") value = s.llm.command_timeout_s;
            } else if (section == "tts") {
                if (field == "backend") value = s.tts.backend;
                // 对应 Python 的 "" if value is None else value
                else if (field == "base_url") value = s.tts.base_url.value_or("");
            }
            // **漏一个键在这儿，症状是"这次改好了，重启就没了"。**
            //
            // 上面那张白名单（kAllowed）和这儿是两份表：前者管"收不收"，
            // 后者管"写不写得回文件"。2026-09-18 加命令行后端时就漏了——
            // 白名单放行了，改也落到内存里了（/api/connections 读出来是对的），
            // 而这儿没有对应分支，`value` 是个 null，写进 config.toml 变成
            //     backend = ""
            //     command = ""
            // 于是重启之后不但设置没了，连 validate 都过不去。
            //
            // 静默是最坏的部分：那一下界面还弹了「换好了」。所以这儿宁可当场
            // 500——两份表对不上是代码的错，不是用户的错，而这句话直接指到
            // 该改的地方。
            if (value.is_null()) {
                throw ApiError(
                    500,
                    SAYF("内部错误：%1 在白名单里放行了，但 persist 那段没有"
                         "对应分支，写回去会是个空值。改 config_api.cpp 里"
                         "这个循环。",
                         key));
            }
            payload[section][field] = value;
        }
        try {
            // payload 可能是空的——这次只改了密钥的话，上面那个循环把它
            // continue 掉了，而 save_user_config 对空 patch 不该留下痕迹。
            if (!payload.empty()) {
                saved_to = paths::to_utf8(save_user_config(payload));
            }
        } catch (const std::exception& e) {
            throw ApiError(500, SAYF("配置写不进去：%1", e.what()));
        }

        // **密钥单独写它自己那个文件。**
        //
        // 不跟 config.toml 放一起：那个文件会被整包 tar 到服务器、
        // 会被贴进聊天窗口排查问题。密钥混在里面的话每一次都是一次泄漏，
        // 而且泄漏时没有任何迹象。见 config::user_api_key_path。
        if (std::find(changed.begin(), changed.end(), "llm_api_key") !=
            changed.end()) {
            try {
                // **存到这次改完之后的那个地址名下**，不是改之前的。
                // 同一个 patch 里既换服务又填新密钥是常事（模型窗里换一家
                // 服务，密钥框就在旁边），存到旧地址上的话，新家还是没钥匙
                // ——发出去 401，而 401 长得像"这个模型不让你用"。
                // `s` 这时已经吃过整个 patch 了，`s.llm.base_url` 就是新的。
                const auto p = config::write_api_key_for(s.llm.base_url,
                                                         s.llm.api_key);
                // 老的单文件也跟着写一份：它是"没给地址时"的兜底，也是
                // 降级回旧版本时唯一读得到的那份。
                config::write_api_key_file(s.llm.api_key);
                if (saved_to.is_null()) saved_to = paths::to_utf8(p);
            } catch (const std::exception& e) {
                throw ApiError(500, SAYF("密钥写不进去：%1", e.what()));
            }
        }
    }

    // 改完立刻重新体检。比让用户自己去点一下体检按钮有用得多——
    // 大部分人改完就走了，不通要到跑流水线时才发现。
    const doctor::Report report = check(s);

    const auto locked = env_overridden();
    json env_changed = json::array();
    for (const auto& key : changed) {
        if (locked.count(key)) env_changed.push_back(key);
    }
    std::sort(env_changed.begin(), env_changed.end());

    return {200, {
        {"changed", changed},
        {"labels", labels_for(changed)},
        {"saved_to", saved_to},
        // 写进文件了，但下次启动环境变量还是会把它顶回去
        {"env_locked", env_changed},
        {"can_run", report.can_run()},
        {"checks", checks_json(report)},
    }};
}

ApiResult post_settings(const json& body) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体要是一个对象"));

    // 正在跑的时候改参数会让这一章前后不一致：前十个镜头一种码率，
    // 后十个另一种，拼起来能看出接缝。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, SAY("正在跑，改参数会让这一章前后不一致"));
    }

    // 老写法是把字段直接摊在请求体里，新写法包在 patch 里。两种都收，
    // 免得刷新慢一步的页面点一下就报 422。
    json patch;
    bool persist = false;
    if (body.contains("patch")) {
        forbid_extra(body, {"patch", "persist"}, "");
        if (!body.at("patch").is_object()) {
            throw unprocessable_top("patch", "Input should be a valid dictionary",
                                    body.at("patch"), "dict_type");
        }
        patch = body.at("patch");
        persist = opt_bool(body, "persist", false);
    } else {
        patch = body;
    }

    static const std::set<std::string> kAllowed = {
        "draft_width", "draft_height", "draft_steps",
        "final_width", "final_height", "final_steps",
        "fps", "crf", "subtitle_font", "subtitle_max_chars_per_line",
        "subtitle_max_lines", "scene_transition_s",
        "gates_enabled", "max_attempts_per_shot", "min_pixel_std",
        "min_frame_similarity", "max_audio_drift_s", "target_lufs",
        "fallback_on_exhausted"};
    forbid_extra(patch, kAllowed, "patch.");

    std::map<std::string, json> data;
    for (const auto& kv : patch.items()) {
        if (!kv.value().is_null()) data[kv.key()] = kv.value();
    }

    std::vector<std::string> changed;
    Settings s = runtime().snapshot();
    models::HardwareProfile profile = runtime().profile();

    // ---- 画质档位 ----
    //
    // **既在进程内生效，也写回 [tiers]。** 以前有意不写回，理由是"档位是
    // 按显存推的，写死等于把这台机器的显存刻进配置"。那个理由站不住：
    // 这个文件里 [models] 全是这台机器的模型路径，本来就是机器专属的。
    // 真实后果是**设完重启就丢**——用户把成片档调成 1280×704 跑了一章，
    // 重启回到 960×544，而界面上没有任何提示。
    // 没填的项在 [tiers] 里是 0，照旧按显存推。
    for (const auto& [tier, prefix] :
         std::vector<std::pair<models::Tier, std::string>>{
             {models::Tier::DRAFT, "draft"}, {models::Tier::FINAL, "final"}}) {
        const auto tit = profile.tiers.find(tier);
        if (tit == profile.tiers.end()) continue;
        models::TierSpec spec = tit->second;
        bool touched = false;
        for (const char* field : {"width", "height", "steps"}) {
            const std::string key = prefix + "_" + field;
            const auto dit = data.find(key);
            if (dit == data.end()) continue;
            const int v = need_int(dit->second, key);
            // 分辨率必须是 32 的倍数，否则 Wan 的潜空间对不齐
            if (std::string(field) == "width" && v % 32) {
                throw ApiError(400, SAY("宽度必须是 32 的倍数"));
            }
            if (std::string(field) == "height" && v % 32) {
                throw ApiError(400, SAY("高度必须是 32 的倍数"));
            }
            if (std::string(field) == "width") spec.width = v;
            else if (std::string(field) == "height") spec.height = v;
            else spec.steps = v;
            // 同时落到配置对象上，下面 persist 那一步会写进文件
            const bool is_draft = tier == models::Tier::DRAFT;
            if (std::string(field) == "width") {
                (is_draft ? s.tiers.draft_width : s.tiers.final_width) = v;
            } else if (std::string(field) == "height") {
                (is_draft ? s.tiers.draft_height : s.tiers.final_height) = v;
            } else {
                (is_draft ? s.tiers.draft_steps : s.tiers.final_steps) = v;
            }
            changed.push_back(key);
            touched = true;
        }
        if (touched) runtime().set_tier_override(tier, spec);
    }

    // ---- 装配、闸门、配音 ----
    const auto take_int = [&](const char* key, int& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_int(it->second, key);
        changed.push_back(key);
    };
    const auto take_num = [&](const char* key, double& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_num(it->second, key);
        changed.push_back(key);
    };
    const auto take_str = [&](const char* key, std::string& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_string(it->second, key);
        changed.push_back(key);
    };
    const auto take_bool = [&](const char* key, bool& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_bool(it->second, key);
        changed.push_back(key);
    };

    // **只写不读的那两个：记下改之前的值，等下判"人是不是真动了它"。**
    //
    // 下面那两条 notes 原来判的是"这个键在不在请求里"——而设置页那次保存
    // 是**把 params 整份发过来**（`saveParams` 遍历 params 的每一项），
    // 而 params 是 `{...s.assembly, ...s.gates}` 摊平来的，这两个键一直在
    // 里面（界面上没有它们的控件，值却跟着走）。照"在不在"判的话，每按一次
    // 「保存参数」都会弹一条 12 秒的黄字，说一件人根本没做过的事。
    // 改成比值：真改了才说。
    const double before_transition_s = s.assembly.scene_transition_s;
    const double before_similarity = s.gates.min_frame_similarity;

    take_int("fps", s.assembly.fps);
    take_int("crf", s.assembly.crf);
    take_str("subtitle_font", s.assembly.subtitle_font);
    take_int("subtitle_max_chars_per_line", s.assembly.subtitle_max_chars_per_line);
    take_int("subtitle_max_lines", s.assembly.subtitle_max_lines);
    take_num("scene_transition_s", s.assembly.scene_transition_s);

    take_int("max_attempts_per_shot", s.gates.max_attempts_per_shot);
    take_num("min_pixel_std", s.gates.min_pixel_std);
    take_num("min_frame_similarity", s.gates.min_frame_similarity);
    take_num("max_audio_drift_s", s.gates.max_audio_drift_s);
    take_num("target_lufs", s.gates.target_lufs);
    take_bool("fallback_on_exhausted", s.gates.fallback_on_exhausted);
    take_bool("gates_enabled", s.gates.enabled);


    // **帧率在这儿就纠回去，别等到 replace 里。**
    //
    // `Runtime::replace` 本来就会调 `normalize_fps_for_model`（MiniMax-H3
    // 只出 24fps，填别的整片会变速），但它纠的是**它自己那份拷贝**——这儿
    // 这个 `s` 一个字不动，然后下面 persist 那一步把**没纠过的那个数**写进
    // 配置文件。于是：文件里写着 30，跑起来是 24，设置页重读之后显示 24。
    // 那个 30 从此永远是死的，还骗下一个打开这个文件的人。
    //
    // 纠回来的理由要说给人听。`replace` 里那句只 fprintf 到 stderr，而
    // 双击启动的人根本看不到 stderr——他看到的是：填了 30，弹一句「参数已
    // 保存到配置文件」，然后那一格自己变回 24，一个字的解释都没有。
    // 跟着响应回去，界面照实说一句。
    std::vector<std::string> notes;
    if (std::string note = normalize_fps_for_model(s); !note.empty()) {
        notes.push_back(std::move(note));
    }

    // **「已应用 场景转场」是这份代码里最后一处还在说它管用的地方。**
    //
    // `scene_transition_s` 今天**没有任何一处读它**：装配走
    // `-f concat -c copy`，全程硬切，引擎里一处 xfade / acrossfade / fade=
    // 都没有（media/assemble.cpp 里那段还写着为什么连"按转场往回挪起点"
    // 都不能做——模拟一个不渲染的东西，字幕会比画面早，而且逐镜累积）。
    //
    // 别处都已经说清楚了：settings.hpp 那个字段头上有一整段 ⚠️，两份配置
    // 模板里各写着「⚠️ 转场目前不生效……改了不会有任何变化」，设置页上那个
    // 输入框 2026-09-14 也撤了。漏的是这儿——字段还在上面的白名单里收着，
    // 而回执里 `labels_for` 会把它翻成中文「场景转场」，拼出来就是
    // 「已应用 场景转场」。拿 curl 或者老客户端改它的人，得到的是一句
    // 和别处四处说明**正好相反**的确认。
    //
    // 不拒收（老配置里有这一行，`/api/settings` 的对拍语料也钉着它），
    // 而是在**真的被改动**时说一句。`notes` 这条通道本来就是为「照你填的
    // 没法做」留的。**判据是"值变了"不是"键在不在"**，理由见上面
    // before_transition_s 那段。
    if (s.assembly.scene_transition_s != before_transition_s) {
        notes.push_back(
            SAY("[assembly].scene_transition_s 存下来了，但它现在不生效："
                "装配是 -f concat -c copy 直接拼，全程硬切，引擎里一处转场"
                "都没渲染。改这个数不会有任何变化。"));
    }
    // 同上：闸门那一组里也有一个只写不读的。**没有「与首帧比相似度」这道
    // 闸门**——gates/checks.cpp 里读 `min_frame_similarity` 的一处都没有
    // （见 settings.hpp 那个字段头上那段）。设置页上那个输入框 2026-09-15
    // 撤了，但白名单还收它（老客户端、curl、老配置）。同上：**改了才说**，
    // 光是跟着一整份 params 发过来不算。
    if (s.gates.min_frame_similarity != before_similarity) {
        notes.push_back(
            SAY("[gates].min_frame_similarity 存下来了，但它现在不生效："
                "引擎里没有「和首帧比相似度」这道闸门，一处都没读过这个数。"
                "拦画面跑飞的是「片中亮度剧烈跳变」那一条，它有自己写死的"
                "阈值。"));
    }

    // 校验不过就整体回滚——半套改动比不改更糟，用户看到"已应用"
    // 但配置是残的。
    const auto errs = s.validate();
    if (!errs.empty()) throw ApiError(400, readable(errs));
    runtime().replace(s);

    json saved_to = nullptr;
    if (persist && !changed.empty()) {
        json payload = json::object();
        for (const auto& key : changed) {
            // 档位六项不在 kSettingSections 里（那张表是和 Python 共用的），
            // 单独落到 [tiers]。**不写回的话设完重启就丢**。
            static const std::map<std::string, int TiersConfig::*> kTierKeys = {
                {"draft_width", &TiersConfig::draft_width},
                {"draft_height", &TiersConfig::draft_height},
                {"draft_steps", &TiersConfig::draft_steps},
                {"final_width", &TiersConfig::final_width},
                {"final_height", &TiersConfig::final_height},
                {"final_steps", &TiersConfig::final_steps},
            };
            if (const auto tk = kTierKeys.find(key); tk != kTierKeys.end()) {
                payload["tiers"][key] = s.tiers.*(tk->second);
                continue;
            }
            const auto it = setting_sections().find(key);
            if (it == setting_sections().end()) continue;
            const auto& [section, field] = it->second;
            json value;
            if (section == "assembly") {
                if (field == "fps") value = s.assembly.fps;
                else if (field == "crf") value = s.assembly.crf;
                else if (field == "subtitle_font") value = s.assembly.subtitle_font;
                else if (field == "subtitle_max_chars_per_line")
                    value = s.assembly.subtitle_max_chars_per_line;
                else if (field == "subtitle_max_lines")
                    value = s.assembly.subtitle_max_lines;
                else if (field == "scene_transition_s")
                    value = s.assembly.scene_transition_s;
            } else if (section == "gates") {
                if (field == "enabled") value = s.gates.enabled;
                else if (field == "max_attempts_per_shot")
                    value = s.gates.max_attempts_per_shot;
                else if (field == "min_pixel_std") value = s.gates.min_pixel_std;
                else if (field == "min_frame_similarity")
                    value = s.gates.min_frame_similarity;
                else if (field == "max_audio_drift_s")
                    value = s.gates.max_audio_drift_s;
                else if (field == "target_lufs") value = s.gates.target_lufs;
                else if (field == "fallback_on_exhausted")
                    value = s.gates.fallback_on_exhausted;
            } else if (section == "tts") {

            }
            if (!value.is_null()) payload[section][field] = value;
        }
        if (!payload.empty()) {
            try {
                saved_to = paths::to_utf8(save_user_config(payload));
            } catch (const std::exception& e) {
                throw ApiError(500, SAYF("配置写不进去：%1", e.what()));
            }
        }
    }

    return {200, {
        {"changed", changed},
        {"labels", labels_for(changed)},
        {"saved_to", saved_to},
        // 「照你填的没法做，按这个来了」那一类。今天两条会进来：帧率被
        // 模型纠回去，和「场景转场收下了但不生效」。
        // 空数组也照发，前端拿 `?? []` 兜着就行。
        {"notes", notes},
    }};
}

}  // namespace changji::http
