#include "infer/llama_chat.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// **在 `#ifdef` 外面**：没链 llama 那一支也要说话。
#include "util/say.hpp"

#include "infer/think_split.hpp"
#include "util/paths.hpp"

#ifdef CHANGJI_HAVE_LLAMA
#include "chat.h"      // common：套对话模板（jinja），认 enable_thinking
#include "common.h"    // common_cpu_get_num_math
#include "llama.h"
#endif

namespace fs = std::filesystem;

namespace changji::infer {

#ifdef CHANGJI_HAVE_LLAMA

bool llama_chat_available() { return true; }

namespace {

/// llama.cpp 的后端只该初始化一次，而且**不在这里 free**。
///
/// 配音那边（llama_tts.cpp）也有一份同样的哨兵，理由写在那儿：
/// `llama_backend_free()` 是全局的，拆掉的是同一个进程里 sd.cpp 也在用的
/// ggml 后端。进程退出时操作系统会收。
void ensure_backend() {
    static const bool once = [] {
        llama_backend_init();
        return true;
    }();
    (void)once;
}

/// `s` 去掉尾部空白之后是不是以 `tag` 结尾。
bool ends_with_tag(const std::string& s, const std::string& tag) {
    if (tag.empty()) return false;
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == ' ' || s[end - 1] == '\t' ||
                       s[end - 1] == '\r')) {
        --end;
    }
    return end >= tag.size() && s.compare(end - tag.size(), tag.size(), tag) == 0;
}

}  // namespace

struct LlamaChat::Impl {
    llama_model* model = nullptr;
    /// 同一份权重上的几个上下文。**权重共用，KV cache 各自一份。**
    ///
    /// 为什么不是一个上下文跑多路：llama.cpp 的单 context 不支持并发
    /// decode，两路同时进去会把进程带走（2026-09-11 端到端实跑时撞过一次，
    /// 日志停在一个请求上，没有任何错误，进程直接没了）。
    std::vector<llama_context*> ctxs;
    int n_ctx = 0;
    /// 从 GGUF 元数据里读出来的对话模板（jinja）。认不出来时 common 会退回
    /// chatml，那正好是 Qwen 那一族的写法。
    common_chat_templates_ptr tmpls;
    bool supports_thinking = false;

    // 空闲上下文的取还。池空了就等——**等，不是失败**：同时编两个项目时，
    // 第二个人宁可多等十几秒，也不该看见一句"忙，稍后再试"。
    std::mutex mu;
    std::condition_variable cv;
    std::vector<llama_context*> idle;

    ~Impl() {
        for (llama_context* c : ctxs) {
            if (c != nullptr) llama_free(c);
        }
        if (model != nullptr) llama_model_free(model);
    }

    /// 借一个上下文，出作用域自动还回去。
    ///
    /// 用 RAII 而不是手动还：中间任何一条 return 或者抛异常忘了还，那个槽
    /// 就永久少一个，表现是并发度悄悄降到 0 然后全部卡死——而那时候没有
    /// 任何报错。放在 Impl 里面是因为 Impl 是 LlamaChat 的私有嵌套类型，
    /// 外面的类写不出它的名字。
    class Lease {
    public:
        Lease(Impl& im, pipeline::CancelToken& tok) : im_(im) {
            std::unique_lock lk(im_.mu);
            // 每 200ms 醒一次查取消：排在前面那一路可能要跑几十秒，
            // 用户按了停，界面上得真的停下来。
            while (im_.idle.empty()) {
                if (tok.cancelled()) return;
                im_.cv.wait_for(lk, std::chrono::milliseconds(200));
            }
            ctx_ = im_.idle.back();
            im_.idle.pop_back();
        }

        ~Lease() {
            if (ctx_ == nullptr) return;
            {
                std::lock_guard lg(im_.mu);
                im_.idle.push_back(ctx_);
            }
            im_.cv.notify_one();
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        llama_context* get() const { return ctx_; }

    private:
        Impl& im_;
        llama_context* ctx_ = nullptr;
    };
};

LlamaChat::LlamaChat() : impl_(std::make_unique<Impl>()) {}
LlamaChat::~LlamaChat() = default;

std::unique_ptr<LlamaChat> LlamaChat::load(const fs::path& model, bool use_gpu,
                                           std::string& why, int parallel,
                                           int n_ctx) {
    if (model.empty()) {
        why = SAY("[models].llm 没填。进程内跑大模型要一份 GGUF 权重");
        return nullptr;
    }
    std::error_code ec;
    if (!fs::is_regular_file(model, ec)) {
        why = SAYF("大模型权重不在：%1", paths::to_utf8(model));
        return nullptr;
    }
    ensure_backend();

    std::unique_ptr<LlamaChat> self(new LlamaChat());
    Impl& im = *self->impl_;

    llama_model_params mp = llama_model_default_params();
    // 999 = 全放显卡；放不下时 llama.cpp 自己会把剩下的留在内存。
    // 调用方传 false 就是纯 CPU——显存被出图出片占满时的退路。
    mp.n_gpu_layers = use_gpu ? 999 : 0;
    im.model = llama_model_load_from_file(paths::to_utf8(model).c_str(), mp);
    if (im.model == nullptr) {
        why = SAYF("大模型载不起来：%1", paths::to_utf8(model));
        return nullptr;
    }

    // **套模板用 common 那套（跑 jinja），不用 llama_chat_apply_template。**
    // 后者只按特征串认一批内置模板、不跑 jinja，于是 `enable_thinking` 这种
    // 模板参数它一个都传不进去——Qwen3 的思考开关正是靠这个参数。
    try {
        im.tmpls = common_chat_templates_init(im.model, "");
    } catch (const std::exception& e) {
        why = SAYF("这个模型的对话模板读不出来：%1", e.what());
        return nullptr;
    }
    if (!im.tmpls) {
        why = SAY("这个模型的对话模板读不出来");
        return nullptr;
    }
    im.supports_thinking = common_chat_templates_support_enable_thinking(im.tmpls.get());

    llama_context_params cp = llama_context_default_params();
    // **n_ctx = 0 表示"用模型训练时的长度"**，而默认值是 512。
    // 写剧本的提示词轻松过千 token，512 会被静默截断——症状是模型
    // 答非所问，指不到"上下文开小了"。配音那边栽过同一条，见 llama_tts.cpp。
    //
    // 但"训练时的长度"也不是白拿的：Qwen3-14B 是 40960，一路 KV cache
    // 就是 6.4 GB，两路比权重本身还大。所以调用方给多少就开多少，
    // 给 0 才退回训练长度。见 config::LLMConfig::context_tokens。
    cp.n_ctx = static_cast<std::uint32_t>(n_ctx < 0 ? 0 : n_ctx);
    // 线程按真实的算术核数定，理由同 llama_tts.cpp 的 ggml_threads()：
    // 默认那个 4 在退回 CPU 跑的时候会让一章写上一个钟头。
    cp.n_threads = common_cpu_get_num_math();
    cp.n_threads_batch = cp.n_threads;

    const int want = parallel < 1 ? 1 : parallel;
    for (int i = 0; i < want; ++i) {
        llama_context* c = llama_init_from_model(im.model, cp);
        if (c == nullptr) {
            // **第一个开不出来才算失败。** 后面的开不出来只说明显存只够这么
            // 多路——那正是"显存不够就排队"该有的样子，不是错误。
            if (i == 0) {
                why = SAY("建不出 llama context");
                return nullptr;
            }
            // `[llm]` 这一族走 stderr，是日志不是界面，不包 `SAY()`。
            // 同 `sd_image.cpp` 里 `[出片]` 那几条。
            std::fprintf(stderr,
                         SAY_NEVER("[llm] 只开出 %d 个上下文（要 %d 个）："
                                   "显存不够，同时跑的路数按这个来\n"),
                         i, want);
            break;
        }
        im.ctxs.push_back(c);
    }
    im.idle = im.ctxs;
    im.n_ctx = static_cast<int>(llama_n_ctx(im.ctxs.front()));
    return self;
}

int LlamaChat::context_tokens() const { return impl_->n_ctx; }

int LlamaChat::slots() const { return static_cast<int>(impl_->ctxs.size()); }

bool LlamaChat::supports_thinking() const { return impl_->supports_thinking; }

bool LlamaChat::complete(const std::string& prompt, const ChatRun& run,
                         pipeline::CancelToken& tok, std::string& out,
                         std::string& why, bool& truncated) {
    out.clear();
    truncated = false;
    Impl& im = *impl_;
    const llama_vocab* vocab = llama_model_get_vocab(im.model);

    // ---- 套对话模板 ----
    //
    // **不套模板等于把 instruct 模型当补全模型用。** Qwen3 这种指令模型训练时
    // 每一轮都裹着 `<|im_start|>role ... <|im_end|>`，裸喂一段中文它只是在
    // "续写"，时好时坏——实测症状是把 JSON Schema 里的字段说明原样当内容吐
    // 回来（title 填成"标题"），或者整串 XXXX。
    common_chat_templates_inputs in;
    {
        common_chat_msg m;
        m.role = "user";
        m.content = prompt;
        in.messages.push_back(std::move(m));
    }
    in.add_generation_prompt = true;
    in.use_jinja = true;
    in.enable_thinking = run.thinking;
    common_chat_params params;
    try {
        params = common_chat_templates_apply(im.tmpls.get(), in);
    } catch (const std::exception& e) {
        why = SAYF("套对话模板失败：%1", e.what());
        return false;
    }
    const std::string& templated = params.prompt;

    // 思考用什么标签，模板说了算；老模板没说的退回 <think>。
    const std::string think_start =
        params.thinking_start_tag.empty() ? std::string("<think>") : params.thinking_start_tag;
    const std::string think_end = params.thinking_end_tags.empty()
                                      ? std::string("</think>")
                                      : params.thinking_end_tags.front();
    // DeepSeek-R1 那一族的模板会替模型把 <think> 先写进提示词末尾，模型
    // 一开口就在思考里；Qwen3 是模型自己写 <think>。两种都要认。
    const bool forced_open = run.thinking && ends_with_tag(templated, think_start);
    ThinkSplitter split(think_start, think_end, forced_open);
    split.on_thinking(run.on_thinking);
    split.on_content(run.on_piece);

    // ---- 提示词切词 ----
    // parse_special 必须是 true，否则 `<|im_start|>` 会被当成普通文字切碎，
    // 模板等于白套。
    //
    // add_special：模板里已经写了 BOS 的（Llama-3 那一族的 jinja 自带
    // `<|begin_of_text|>`）不能再加一个，两个 BOS 开头模型会发懵。
    bool add_special = true;
    if (llama_vocab_get_add_bos(vocab)) {
        char bos_buf[64];
        const int n = llama_token_to_piece(vocab, llama_vocab_bos(vocab), bos_buf,
                                           sizeof(bos_buf), 0, true);
        if (n > 0 && templated.compare(0, static_cast<std::size_t>(n), bos_buf,
                                       static_cast<std::size_t>(n)) == 0) {
            add_special = false;
        }
    }
    const int n_prompt = -llama_tokenize(vocab, templated.c_str(),
                                         static_cast<int32_t>(templated.size()),
                                         nullptr, 0, add_special, true);
    if (n_prompt <= 0) {
        why = SAY("提示词切不出 token");
        return false;
    }
    std::vector<llama_token> toks(static_cast<std::size_t>(n_prompt));
    if (llama_tokenize(vocab, templated.c_str(),
                       static_cast<int32_t>(templated.size()), toks.data(),
                       n_prompt, add_special, true) < 0) {
        why = SAY("提示词切词失败");
        return false;
    }
    // **max_tokens ≤ 0 = 上下文里剩多少就写多少。**
    //
    // 原来调用方写死一个数（8192，后来 12288），而这个数和 schema 要的输出量
    // 是两条各自在变的线：一章从"一堆段落"改成"几场戏"之后输出量一路涨。
    // 两次撞上上限的表现**都不是"写短了"而是"解析失败"**——输出停在半截
    // JSON 上，那一章落成 0 字，看报错根本想不到是这儿。与其再拍一个魔数，
    // 不如让它跟着上下文走。留 64 个 token 的余量给模板尾巴和收尾。
    //
    // 思考也从这份额度里出：想得越多正文剩得越少。这是上下文的物理事实，
    // 不是这儿能改的——上下文开小了会在这儿报"提示词太长"那句能读懂的话。
    int max_tokens = run.max_tokens;
    if (max_tokens <= 0) max_tokens = std::max(1, im.n_ctx - n_prompt - 64);

    // **先查长度再跑。** 超了的话 llama.cpp 会截断而不是报错，
    // 出来的东西看着像模型没听懂，其实是提示词根本没喂全。
    if (n_prompt + max_tokens > im.n_ctx) {
        why = SAYF("提示词太长：%1 个 token 加上要生成的 %2 个，超过这个模型"
                   "的上下文 %3。把 [llm].context_tokens 调大，或者换一个"
                   "上下文更长的模型。",
                   std::to_string(n_prompt), std::to_string(max_tokens),
                   std::to_string(im.n_ctx));
        return false;
    }

    // ---- 采样链 ----
    //
    // 没有语法采样了（理由见头文件）。**也没有重复惩罚和 DRY 了。**
    //
    // 当年那两道（penalties 1.10 / 512、DRY 0.8 / 1.75 / 6）是配着 GBNF 来
    // 的：语法钉住形状，惩罚管复读。2026-09-19 拿 Qwen3-4B Q4 真写一章
    // 看到的：JSON 里 "where" "pov" "paragraphs" 这些键每一场都要重写，
    // 惩罚把它们和常用字一起压下去，模型只好往冷门 token 上躲——正文越写
    // 越往繁体和错字上漂（「捏著」「表盤內側」「指縀」），到第三场已经不像
    // 中文小说了。远端那条从来没发过这两个参数，各家服务默认也都是关的。
    //
    // 复读今天由收稿那头的守卫抓（check_repetition），抓到了走改稿那一轮
    // ——那是"告诉它哪句重了"，比在采样上盲压每一个 token 准得多。
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init(sp);
    struct ChainGuard {
        llama_sampler* p;
        ~ChainGuard() { llama_sampler_free(p); }
    } guard{chain};
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(
        chain, llama_sampler_init_temp(static_cast<float>(run.temperature)));
    // LLAMA_DEFAULT_SEED = 随机种子。重掷那一次得真的是另一把骰子。
    llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    // ---- 跑 ----
    //
    // 借一个空闲上下文。**同一个上下文同一时刻只有一路**——llama.cpp 的
    // 单 context 不支持并发 decode。池里有空的就直接开跑（真并发），
    // 全忙着就在这儿等（排队）。
    Impl::Lease lease(im, tok);
    if (lease.get() == nullptr) {
        why = SAY("已取消");
        return false;
    }
    llama_context* lctx = lease.get();

    llama_memory_clear(llama_get_memory(lctx), true);

    // ---- 先把提示词喂进去，**分批喂** ----
    //
    // ⚠️ **llama_decode 一次最多吃 n_batch 个 token，超了不是报错是 abort。**
    //
    //     llama-context.cpp:1722: GGML_ASSERT(n_tokens_all <= cparams.n_batch)
    //
    // 那是 GGML_ASSERT，整个进程当场没——用户正在批量写正文，写到一半
    // 界面连同引擎一起消失（2026-09-12 实撞）。n_batch 的默认值是 2048，
    // 而上面那道长度检查比的是 n_ctx。分批喂就没有这条限制：
    // llama_batch_get_one 不带位置，位置由上下文按 KV 里已有的长度顺着排。
    const int n_batch = std::max(1, static_cast<int>(llama_n_batch(lctx)));
    for (int i = 0; i < n_prompt; i += n_batch) {
        // 提示词几千个 token 时这一步也要几秒，取消要能在这儿生效。
        if (tok.cancelled()) return true;
        const int n = std::min(n_batch, n_prompt - i);
        llama_batch part = llama_batch_get_one(toks.data() + i, n);
        if (llama_decode(lctx, part) != 0) {
            why = SAYF("llama_decode 失败（喂提示词，第 %1 个 token 起，"
                       "这一批 %2 个）",
                       std::to_string(i), std::to_string(n));
            return false;
        }
    }

    bool ended = false;   // 模型自己收的尾（EOG），不是写到上限被截的
    for (int produced = 0; produced < max_tokens; ++produced) {
        // 取消在**每个 token 之间**查一次。写一章要几分钟，
        // 不查的话点了停止要等它自己写完。
        if (tok.cancelled()) {
            split.finish();
            out = split.content();
            return true;
        }

        // **别再 accept 一次。** `llama_sampler_sample` 内部已经调过
        // `llama_sampler_accept`。再手动接一次的话采样状态被推进两遍。
        llama_token id = llama_sampler_sample(chain, lctx, -1);
        if (llama_vocab_is_eog(vocab, id)) {
            ended = true;
            break;
        }

        char buf[256];
        const int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n < 0) {
            why = SAY("token 转不回文字");
            return false;
        }
        // 思考和正文在这儿分家：思考走 on_thinking，正文走 on_piece 并攒进 out。
        split.feed(std::string(buf, static_cast<std::size_t>(n)));

        // 把刚采出来的这个喂回去，下一轮才采得出下一个。
        llama_batch one = llama_batch_get_one(&id, 1);
        if (llama_decode(lctx, one) != 0) {
            why = SAYF("llama_decode 失败（第 %1 个 token）",
                       std::to_string(produced));
            return false;
        }
    }
    split.finish();
    out = split.content();
    truncated = !ended;
    return true;
}

#else

bool llama_chat_available() { return false; }

struct LlamaChat::Impl {};
LlamaChat::LlamaChat() : impl_(std::make_unique<Impl>()) {}
LlamaChat::~LlamaChat() = default;

std::unique_ptr<LlamaChat> LlamaChat::load(const fs::path&, bool,
                                           std::string& why, int, int) {
    why = SAY("这个二进制没编进程内大模型（构建时 CHANGJI_LLAMA=OFF）。"
              "用 [llm].base_url 指向一个兼容 OpenAI 接口的服务");
    return nullptr;
}
int LlamaChat::context_tokens() const { return 0; }
int LlamaChat::slots() const { return 0; }
bool LlamaChat::supports_thinking() const { return false; }
bool LlamaChat::complete(const std::string&, const ChatRun&, pipeline::CancelToken&,
                         std::string&, std::string& why, bool& truncated) {
    truncated = false;
    why = SAY("这个二进制没编进程内大模型");
    return false;
}

#endif

}  // namespace changji::infer
