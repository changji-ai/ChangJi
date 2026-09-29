#include "llm/local_client.hpp"

#include <atomic>
#include <map>
#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "config/runtime.hpp"
#include "infer/llama_chat.hpp"
#include "infer/scheduler.hpp"
#include "llm/call_log.hpp"
#include "llm/schema_validate.hpp"
#include "models/hardware.hpp"
#include "pipeline/activity.hpp"
#include "util/cancel_words.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"
#include "setup/catalog.hpp"

namespace changji::llm {

namespace {

std::mutex g_mu;
std::shared_ptr<infer::LlamaChat> g_chat;
/// 装着的是哪一份。**一条对话可以单独挑一份**（`LLMConfig::local_weights`），
/// 借槽之前比一下，不是同一份就先换。
std::filesystem::path g_loaded;

/// 这条线程借槽时要哪一份。调度器是在借槽的那条线程上调 `load` 的
/// （`Scheduler::acquire`），所以挂在线程上就能精确地交给它，不会被别的
/// 对话同时借槽时改掉。空 = 配置里 `[models].llm` 那一份。
thread_local std::filesystem::path t_want;

std::filesystem::path configured_weights(const config::Settings& s) {
    return s.models.resolve(s.models.llm, s.workspace_path());
}

/// 这一次要装哪一份：对话挑的，没挑就是配置里那一份。
std::filesystem::path wanted_weights(const config::LLMConfig& cfg,
                                     const config::Settings& s) {
    return cfg.local_weights.empty() ? configured_weights(s)
                                     : paths::from_utf8(cfg.local_weights);
}

std::filesystem::path loaded_weights() {
    std::lock_guard lg(g_mu);
    return g_chat ? g_loaded : std::filesystem::path();
}

/// 借到的槽上装着的是 `want` 那一份。
///
/// **两条对话挑了两份不同的权重**时：装着的是别的那份、而且那边还在用，就等它
/// 用完（驱逐不动正在借着的槽）；没人用就卸掉、按这边要的重装。借到之后再核
/// 一遍——卸掉到借到之间另一条对话可能抢先装了它那份。
infer::Lease lease_weights(const std::filesystem::path& want,
                                      const infer::Scheduler::AcquireOptions& opt,
                                      pipeline::CancelToken& tok) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + infer::kAcquireWait;
    for (;;) {
        if (tok.cancelled()) throw LlmError(util::kCancelled);
        const auto have = loaded_weights();
        if (!have.empty() && have != want) infer::scheduler().evict(infer::Slot::LLM);
        struct Want {
            explicit Want(const std::filesystem::path& p) { t_want = p; }
            ~Want() { t_want.clear(); }
        };
        auto lease = [&] {
            const Want w{want};
            return infer::scheduler().acquire(infer::Slot::LLM, opt);
        }();
        if (loaded_weights() == want) return lease;
        lease.release();
        if (std::chrono::steady_clock::now() > deadline) {
            throw LlmError(SAY("另一条对话正拿着另一份本地模型在写，等了五分钟还没轮到。"
                               "等它写完再试，或者这条对话换成同一份"));
        }
        std::this_thread::sleep_for(500ms);
    }
}

std::shared_ptr<infer::LlamaChat> current() {
    std::lock_guard lg(g_mu);
    return g_chat;
}

/// 开一份记录器，构造失败就当作没开日志往下走。理由同 client.cpp 里那个
/// 同名函数（「构造这一下抛了，也不许把这一次生成带走」）。
std::optional<CallLog> open_call_log(const Request& req, const config::LLMConfig& cfg,
                                     const char* kind = "complete_stream") {
    try {
        return std::optional<CallLog>(std::in_place, "local", kind, req,
                                      call_log_options(cfg));
    } catch (...) {
        CallLogOptions off;
        off.enabled = false;
        return std::optional<CallLog>(std::in_place, "local", kind, req, off);
    }
}

/// 权重文件名（不含目录），日志的 endpoint / model 两栏记它。
std::string model_label(const std::filesystem::path& p) {
    // 这个名字进的是提示词日志的 endpoint / model 两栏，**是账不是话**：
    // 翻了之后同一台机器上前后两段日志对不上。不包 `SAY()`。
    return p.empty() ? std::string(SAY_NEVER("（没填 [models].llm）"))
                     : paths::to_utf8(p.filename());
}

}  // namespace

LocalLlmStatus local_llm_status() {
    LocalLlmStatus st;
    auto chat = current();
    if (!chat) return st;
    st.loaded = true;
    st.slots = chat->slots();
    st.context_tokens = chat->context_tokens();
    st.supports_thinking = chat->supports_thinking();
    return st;
}

LocalClient::LocalClient(ConfigProvider cfg) : cfg_(std::move(cfg)) {}

std::string LocalClient::complete(const Request& req, pipeline::CancelToken& tok) {
    return complete(req, tok, {});
}

std::string LocalClient::complete(const Request& req, pipeline::CancelToken& tok,
                                  const OnToken& on_token) {
    const config::LLMConfig cfg = cfg_();
    // 记录器紧跟着配置摆，取消检查之前：「已取消」也是一次调用真实的结束方式。
    auto log_box = open_call_log(req, cfg);
    CallLog& log = *log_box;
    try {
        if (tok.cancelled()) throw LlmError(util::kCancelled);

        // 账上那几栏**先填**，再去借槽：权重载不起来时异常是从借槽那一步
        // 抛出来的，填在后面的话那一行就是一条 endpoint 空、model 空的孤行，
        // 2026-09-19 第一次冒烟就是这么记出来一行 ok=true 的失败。
        const config::Settings settings = config::runtime().snapshot();
        const std::filesystem::path want = wanted_weights(cfg, settings);
        const std::string label = model_label(want);
        const double temperature =
            req.temperature.value_or(cfg.temperature_for(req.schema_name));
        log.set_endpoint("local:" + label);
        log.set_model(label, temperature, req.reasoning_effort);

        // **并发在 LlamaChat 里管**：同一份权重上开了几个上下文，几路就能同时
        // 跑，池满了才在那儿等（见 LlamaChat::load 和 Impl::Lease）。
        //
        // 这里不能再加一把全局互斥——加了就等于把并发按回 1，同时编两个项目时
        // 第二个人干等十几秒。而这一层看不到显存够开几个上下文，那个判断只有
        // LlamaChat 做得了。
        //
        // 借槽。**调度器可能在这一步把出图或出片的模型卸掉腾地方**，
        // 也可能什么都不做（实时空闲显存够的时候）——见
        // Scheduler::set_free_vram_probe。借不到时抛的是那条带出路的消息。
        infer::Scheduler::AcquireOptions opt;
        opt.wait = infer::kAcquireWait;
        opt.on_queued = pipeline::note_queued;
        // 全局配的是远端、这条对话单独挑了本地的时候，槽还没挂上。
        ensure_llm_slot();
        auto lease = lease_weights(want, opt, tok);
        auto chat = current();
        if (!chat) throw LlmError(SAY("大模型没准备好（槽借到了但上下文是空的）"));

        // schema 贴进提示词——和远端那条同一个函数，不另写一份。
        const std::string prompt = req.schema.is_null() || req.schema.empty()
                                       ? req.prompt
                                       : schema_as_prompt(req.prompt, req.schema);
        // 本地没有"模型名"，账上记的是权重文件名——按它分组正好分得出 14B
        // 和 8B 哪个更常被打回。
        log.set_prompt(prompt);

        infer::ChatRun run;
        run.temperature = temperature;
        run.thinking = cfg.thinking;
        // 思考走两处：记录器攒着（析构时落 thinking.txt），界面那条流照转。
        run.on_thinking = [&](const std::string& piece) {
            log.append_thinking(piece);
            if (req.on_thinking) req.on_thinking(piece);
        };
        run.on_piece = [&](const std::string& piece) {
            log.mark_first_token();
            if (on_token) on_token(piece);
        };

        std::string out;
        std::string why;
        bool truncated = false;
        const bool ok = chat->complete(prompt, run, tok, out, why, truncated);
        // **半个字不留。** token 和字不对齐，模型偶尔停在一个多字节字的
        // 中间（取消、截断、或者它自己犯浑）；带着半个字去解析，nlohmann
        // 直接拒收，整份正文作废。换成 U+FFFD 是一个坏字，比一章 0 字强。
        out = text::sanitize_utf8(out);
        // 正文要赶在那几条 throw 之前交给记录器——校验没过的那份正是最想研究的。
        log.set_reply(out);
        if (!ok) throw LlmError(SAYF("进程内大模型失败：%1", why));
        if (tok.cancelled()) throw LlmError(util::kCancelled);
        log.set_finish_reason(truncated ? "length" : "stop");
        if (truncated) {
            // 和远端 require_complete_reason 同一句：写到上限还没收尾，那份
            // 多半是半截 JSON，解析出来的错会指向别处。
            throw LlmError(SAY("大模型输出达到长度上限，返回内容被截断。"
                               "把 [llm].context_tokens 调大，或者缩短这一步"
                               "再试"));
        }
        // 本地校验，和远端那条同一份规矩（client.cpp 的 checked_output）：
        // 结构不对就是这一次没写好，抛出去让上层重掷或改稿。
        if (const auto err = validate_structured_output(out, req.schema)) {
            throw LlmError(SAYF(
                "大模型输出不符合 %1：%2",
                req.schema_name.empty() ? std::string("JSON Schema")
                                        : req.schema_name + " Schema",
                *err));
        }
        return out;
    } catch (const LlmError& e) {
        log.fail(e.what(), e.status());
        throw;
    } catch (const std::exception& e) {
        // 借槽那一步抛的是 runtime_error（「大模型载不起来」「显存不够」），
        // 不是 LlmError。不接这一支的话账上是一行 ok=true 的失败（析构时
        // 没人说它砸了），上层看到的是一句「服务端出错」的 500。翻成
        // LlmError：账记对，界面上是那句能读懂的话。
        log.fail(e.what(), 0);
        throw LlmError(SAYF("进程内大模型：%1", e.what()));
    }
}

ChatReply LocalClient::chat(const std::vector<Message>& messages,
                            const nlohmann::ordered_json& tools, const Request& opts,
                            pipeline::CancelToken& tok) {
    const config::LLMConfig cfg = cfg_();
    auto log_box = open_call_log(opts, cfg, "chat_stream");
    CallLog& log = *log_box;
    try {
        if (tok.cancelled()) throw LlmError(util::kCancelled);
        const config::Settings settings = config::runtime().snapshot();
        const std::filesystem::path want = wanted_weights(cfg, settings);
        const std::string label = model_label(want);
        const double temperature =
            opts.temperature.value_or(cfg.temperature_for(opts.schema_name));
        const std::string effort =
            opts.reasoning_effort.empty() ? cfg.effort_for(opts.schema_name) : opts.reasoning_effort;
        log.set_endpoint("local:" + label);
        log.set_model(label, temperature, effort);
        log.set_tools(tools);

        // ---- 换成 LlamaChat 认的形状 ----
        //
        // tool 那一条要带工具名（有的模板按名字写回话）：从前面那条 assistant 的
        // tool_calls 里按 id 找回来。
        std::vector<infer::ChatMsg> msgs;
        std::map<std::string, std::string> name_of;
        std::string shown;   // 记账用：整段来回拼成一段字
        for (const auto& m : messages) {
            infer::ChatMsg cm;
            cm.role = m.role;
            cm.content = m.content;
            cm.tool_call_id = m.tool_call_id;
            for (const auto& c : m.tool_calls) {
                cm.calls.push_back({c.id, c.name, c.arguments});
                name_of[c.id] = c.name;
            }
            if (m.role == "tool") {
                const auto it = name_of.find(m.tool_call_id);
                if (it != name_of.end()) cm.tool_name = it->second;
            }
            shown += "[" + m.role + "] " + m.content + '\n';
            msgs.push_back(std::move(cm));
        }
        log.set_prompt(shown);
        std::vector<infer::ChatTool> defs;
        if (tools.is_array()) {
            for (const auto& t : tools) {
                const auto& f = t.contains("function") ? t.at("function") : t;
                if (!f.is_object() || !f.contains("name")) continue;
                defs.push_back({f.value("name", std::string()),
                                f.value("description", std::string()),
                                f.contains("parameters") ? f.at("parameters").dump()
                                                         : std::string("{}")});
            }
        }

        ensure_llm_slot();
        infer::Scheduler::AcquireOptions opt;
        opt.wait = infer::kAcquireWait;
        opt.on_queued = pipeline::note_queued;
        auto lease = lease_weights(want, opt, tok);
        auto chat = current();
        if (!chat) throw LlmError(SAY("大模型没准备好（槽借到了但上下文是空的）"));

        infer::ChatRun run;
        run.temperature = temperature;
        // 「不要想」那一档在本地就是关思考（模板认 enable_thinking 的话）。
        run.thinking = cfg.thinking && effort != "off";
        run.on_thinking = [&](const std::string& piece) {
            log.append_thinking(piece);
            if (opts.on_thinking) opts.on_thinking(piece);
        };
        run.on_piece = [&](const std::string& piece) {
            log.mark_first_token();
            if (opts.on_token) opts.on_token(piece);
        };

        infer::ChatAnswer ans;
        std::string why;
        bool truncated = false;
        if (!chat->converse(msgs, defs, run, tok, ans, why, truncated)) {
            throw LlmError(SAYF("进程内大模型失败：%1", why));
        }
        if (tok.cancelled()) throw LlmError(util::kCancelled);

        ChatReply reply;
        reply.content = text::sanitize_utf8(ans.content);
        for (const auto& c : ans.calls) reply.tool_calls.push_back({c.id, c.name, c.arguments});
        reply.finish_reason = !reply.tool_calls.empty() ? "tool_calls"
                              : truncated               ? "length"
                                                        : "stop";
        log.set_finish_reason(reply.finish_reason);
        log.set_reply(reply.content);
        return reply;
    } catch (const LlmError& e) {
        log.fail(e.what(), e.status());
        throw;
    } catch (const std::exception& e) {
        log.fail(e.what(), 0);
        throw LlmError(SAYF("进程内大模型：%1", e.what()));
    }
}

std::shared_ptr<Client> make_local_client(ConfigProvider cfg) {
    if (!infer::llama_chat_available()) return nullptr;
    return std::make_shared<LocalClient>(std::move(cfg));
}

void register_llm_slot(std::function<config::Settings()> provider,
                       const models::HardwareProfile& profile, bool even_if_remote) {
    if (!even_if_remote && provider().llm.backend != "local") return;
    if (!infer::llama_chat_available()) return;   // 装不上的槽不注册，理由见头文件
    // **一个进程只注册一次。** 起服务时注册一次，之后在模型窗里切来切去
    // 会再叫到这儿（config_api / server 那两处）；权重换了也不用重注册——
    // load 那个回调是每次借槽时现读配置的。
    static std::atomic<bool> registered{false};
    if (registered.exchange(true)) return;

    infer::SlotSpec spec;
    spec.slot = infer::Slot::LLM;
    // 常驻：装上之后就不主动卸，显存真不够时才被驱逐。
    //
    // **注意 Cached 不等于"启动就装"**：调度器是借出时才加载的
    // （见 Scheduler::acquire）。
    spec.residency = infer::Residency::Cached;
    // 估值按整份预算算，和出图出片一致——"同时只装得下一个"是保守但安全的
    // 假设。真装得下的时候由下面那个老实数救回来（不会白卸）。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;
    spec.vram_estimate = static_cast<std::size_t>(budget * 1024) * 1024 * 1024;
    // **老实数：真正要占的显存。** 只在问到了卡上空闲显存时才拿来比。
    // 不给的话这条"够就不卸"是单向的——出片时保住了大模型，回头写剧本
    // 借 LLM 槽走的还是整份预算，反过来把图像模型卸掉，两边来回踢。
    // 每次借槽时现算，不存定值：大模型也能在模型窗里换掉，
    // 而槽一个进程只注册一次。见 SlotSpec::live_vram。
    spec.live_vram = [provider]() -> std::size_t {
        const config::Settings s = provider();
        std::error_code ec;
        const auto p = t_want.empty() ? configured_weights(s) : t_want;
        const auto bytes = p.empty() ? 0 : std::filesystem::file_size(p, ec);
        const double model_gb = (!ec && bytes > 0)
                                    ? static_cast<double>(bytes) / (1024.0 * 1024 * 1024)
                                    : 0.0;
        const double live = s.models.llm_live_vram_gb(model_gb);
        return live > 0 ? static_cast<std::size_t>(live * 1024) * 1024 * 1024 : 0;
    };
    // **优先级最低，腾地方时先卸它。** 写一章只跑一次，
    // 出图出片每镜都要——重装大模型的代价摊在一章上，比每镜重装小得多。
    spec.evict_priority = 1;
    // **闲多久就卸**：每次现问（人在设置里改了分钟数，下一拍就按新的算）。
    spec.keep_alive = [provider]() -> std::optional<std::chrono::seconds> {
        const double m = provider().llm.keep_alive_minutes;
        if (m < 0) return std::nullopt;
        return std::chrono::seconds(static_cast<long long>(m * 60));
    };
    spec.load = [provider] {
        const config::Settings s = provider();
        const auto path = t_want.empty() ? configured_weights(s) : t_want;
        // **装之前先记一眼显存。** 装完再记一次，差值就是这份权重实际
        // 占了多少——比"总量减空闲"准得多，那个会把别的槽的账也算进来。
        //
        // **只在没有别的槽同时在装的时候才认这个差值。** acquire 是标完
        // is_loaded 就放锁、再去调 load 的，两个槽完全可能同时在装。那时候
        // 这段窗口里的显存变化里混着别人的账，差值会偏大——而它是只往上记
        // 的高水位，记错一次就一直错下去。
        const auto alone_now = [] {
            const auto v = infer::scheduler().loaded_slots();
            return v.size() == 1 && v.front() == infer::Slot::LLM;
        };
        const bool alone_before = alone_now();
        const auto before = models::free_vram_gb();
        std::string why;
        auto chat = std::shared_ptr<infer::LlamaChat>(
            infer::LlamaChat::load(path, /*use_gpu=*/true, why, s.llm.parallel,
                                   s.llm.context_tokens));
        if (!chat) {
            // **上不了 GPU 就退回 CPU，别整个失败。**
            //
            // use_gpu 传下去是 n_gpu_layers = 999，也就是"所有层都放显存"。
            // 这张卡装得下就最好，装不下 llama.cpp 直接返回失败——而
            // "大模型载不起来"对用户等于整条流水线没了，其实放内存跑就行，
            // 只是慢。**不预先估一个阈值来决定放不放**：估算会错到五倍
            // （见 Scheduler::record_measured_vram）。试一次、不行再退。
            // `[llm]` 走 stderr，是日志不是界面，不包。
            std::fprintf(stderr,
                         SAY_NEVER("[llm] 权重上不了显存（%s），退回内存跑。"
                                   "慢一些，但不影响出片。\n"),
                         why.c_str());
            std::string why_cpu;
            chat = std::shared_ptr<infer::LlamaChat>(
                infer::LlamaChat::load(path, /*use_gpu=*/false, why_cpu,
                                       s.llm.parallel, s.llm.context_tokens));
            if (!chat) {
                throw std::runtime_error(
                    SAYF("大模型载不起来：显存那次是「%1」，内存那次是「%2」",
                         why, why_cpu));
            }
        }
        {
            const auto after = models::free_vram_gb();
            if (alone_before && alone_now() && before.has_value() &&
                after.has_value() && *before > *after) {
                const double used_gb = *before - *after;
                infer::scheduler().record_measured_vram(
                    infer::Slot::LLM,
                    static_cast<std::size_t>(used_gb * 1024) * 1024 * 1024);
            }
        }
        std::lock_guard lg(g_mu);
        g_chat = std::move(chat);
        g_loaded = path;
    };
    spec.unload = [] {
        std::lock_guard lg(g_mu);
        g_chat.reset();
        g_loaded.clear();
    };
    infer::scheduler().register_slot(std::move(spec));
}

std::string loaded_llm_file() {
    std::lock_guard lg(g_mu);
    return g_chat ? paths::to_utf8(g_loaded) : std::string();
}

void ensure_llm_slot() {
    if (!infer::llama_chat_available()) return;
    register_llm_slot([] { return config::runtime().snapshot(); }, config::runtime().profile(),
                      /*even_if_remote=*/true);
}

bool is_llm_weights(const std::string& rel, std::uint64_t bytes) {
    const std::filesystem::path p = paths::from_utf8(rel);
    std::string ext = paths::to_utf8(p.extension());
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".gguf") return false;
    const std::string name = paths::to_utf8(p.filename());
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("mmproj") != std::string::npos) return false;
    // 清单里编剧那一组的文件：**大小要对上**，下到一半的不算。
    for (const auto& g : setup::catalog()) {
        if (g.key != "llm") continue;
        for (const auto& o : g.options) {
            for (const auto& f : o.files) {
                if (paths::to_utf8(paths::from_utf8(f.name).filename()) != name) continue;
                return f.bytes == 0 || bytes == f.bytes;
            }
        }
    }
    // 人自己放进 `llm/` 的：认。别的目录里的 gguf 多半是出片、出图那几组的编码器
    // （Qwen2.5-VL、umt5 那些），挑进来当编剧会写出一堆乱码。
    return rel.rfind("llm/", 0) == 0;
}

}  // namespace changji::llm
