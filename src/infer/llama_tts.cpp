#include "infer/llama_tts.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

// **在 `#ifdef` 外面**：下面没链 llama 那一支也要说话。
#include "util/say.hpp"

#ifdef CHANGJI_HAVE_LLAMA
#include <fstream>

#include "common.h"   // common_cpu_get_num_math
#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include "util/paths.hpp"
#endif

namespace changji::infer {

#ifdef CHANGJI_HAVE_LLAMA

namespace {

/// 这台机器上跑 ggml 该开几个线程。0 = 问不出来，调用方保留上游默认值。
///
/// **骨干、解码器、体检那句话三处共用一份**：三处各调一次的话，将来谁改了
/// 其中一处，屏幕上写的线程数和真正在跑的就对不上——而那正是这一行原来的
/// 毛病（体检印 `mtmd_context_params_default().n_threads`、跑的是别的数）。
///
/// `common_cpu_get_num_math()` 不是"逻辑核数"：x86 混合核上它只数性能核，
/// 苹果芯片上同理，别的情况回物理核数。拿逻辑核数（`hardware_concurrency`）
/// 去跑 ggml 通常比物理核数还慢。
int ggml_threads() {
    const int n = common_cpu_get_num_math();
    return n > 0 ? n : 0;
}


/// llama.cpp 的后端只需要初始化一次，而且**不在这里 free**。
///
/// `llama_backend_free()` 是全局的。配音后端被销毁时调它，会把
/// 同一个进程里 sd.cpp 那边正在用的 ggml 后端一起拆掉——两个库共用
/// 同一份 ggml，这不是理论风险。进程退出时操作系统会收，够了。
void ensure_backend() {
    static const bool once = [] {
        llama_backend_init();
        return true;
    }();
    (void)once;
}

}  // namespace

bool llama_tts_available() { return true; }

LlamaTtsProbe probe_llama_tts() {
    LlamaTtsProbe out;

    // 两个纯函数，没有副作用，也不碰模型文件。
    // 叫得动就说明 mtmd 真在这个二进制里。
    const mtmd_context_params p = mtmd_context_params_default();
    const char* marker = mtmd_default_marker();

    if (marker == nullptr) {
        out.detail = SAY("mtmd 链进来了，但 mtmd_default_marker() 返回空");
        return out;
    }
    out.ok = true;
    // **报真正会用的那个数，不是上游的默认值。**
    // 这里原来印的是 `mtmd_context_params_default().n_threads`，也就是
    // 写死的 4——而跑起来用的是下面 make() 里按核数设的那个。两个数不一样，
    // 体检上写着 4、实际跑 16，这一行就是在骗人。
    const int n = ggml_threads();
    out.detail = SAYN("mtmd 已链入，媒体标记 %1，%n 线程", n > 0 ? n : p.n_threads,
                      marker);
    return out;
}

struct LlamaTts::Impl {
    llama_model* model = nullptr;
    llama_context* lctx = nullptr;
    mtmd_context* mctx = nullptr;
    llama_sampler* smpl = nullptr;
    int sample_rate = 0;

    /// **一次只许念一句。** 和 sd_image.cpp 里那把 run_mu 是同一件事：
    /// 上下文、解码器、采样器都只有一份，而 synthesize() 每句都要把采样器
    /// free 掉重建——两条线程进来就是一个正在用、另一个给 free 了。
    ///
    /// 调度器挡不住：同一个槽可以被借好几次（多个租约共用一份已加载的
    /// 权重）。"一次一件"只能由用它的人自己管，而这个对象是共享的，
    /// 那就该它自己管，谁调都不会漏。
    std::mutex run_mu;

    ~Impl() {
        if (smpl != nullptr) llama_sampler_free(smpl);
        if (mctx != nullptr) mtmd_free(mctx);
        if (lctx != nullptr) llama_free(lctx);
        if (model != nullptr) llama_model_free(model);
        // 有意不调 llama_backend_free()，见 ensure_backend 上面那段。
    }
};

LlamaTts::LlamaTts() : impl_(std::make_unique<Impl>()) {}
LlamaTts::~LlamaTts() = default;

int LlamaTts::sample_rate() const { return impl_->sample_rate; }

std::unique_ptr<LlamaTts> LlamaTts::load(const std::filesystem::path& backbone,
                                         const std::filesystem::path& mmproj,
                                         bool use_gpu, std::string& why) {
    ensure_backend();

    std::error_code ec;
    if (!std::filesystem::is_regular_file(backbone, ec)) {
        why = SAYF("找不到骨干模型：%1", paths::to_utf8(backbone));
        return nullptr;
    }
    if (!std::filesystem::is_regular_file(mmproj, ec)) {
        why = SAYF("找不到解码器：%1", paths::to_utf8(mmproj));
        return nullptr;
    }

    // unique_ptr 而不是裸 new：下面每一步都可能失败，
    // 而每一步之后都已经有句柄要收。
    std::unique_ptr<LlamaTts> self(new LlamaTts());
    Impl& im = *self->impl_;

    llama_model_params mp = llama_model_default_params();
    // 骨干模型只有 1.7B，全放显卡上；放不下时 llama.cpp 自己会退。
    mp.n_gpu_layers = use_gpu ? 999 : 0;
    im.model = llama_model_load_from_file(paths::to_utf8(backbone).c_str(), mp);
    if (im.model == nullptr) {
        why = SAYF("骨干模型载不起来：%1", paths::to_utf8(backbone));
        return nullptr;
    }

    llama_context_params cp = llama_context_default_params();
    // **必须开 embeddings。** 生成的每一帧都要把骨干的隐状态喂给解码器，
    // 关着的话 llama_get_embeddings_ith 返回空，症状是第一帧就失败。
    cp.embeddings = true;

    // **n_ctx 按这一步真正要用多少定，两个默认值都不能用。**
    //
    // `llama_context_default_params()` 给的是 **512**——太小：骨干是自回归
    // 跑的，提示词的 token 加上最多 512 帧都要占位置，一句长台词就撑爆，
    // 症状是生成中途失败、指不到"上下文开小了"。
    //
    // 参考实现（`tools/tts/tts.cpp` 走 common）用的是 **0**，llama.cpp 里
    // 0 表示"用模型训练时的长度"（`params.n_ctx == 0 ? hparams.n_ctx_train
    // : params.n_ctx`）。这里原来照抄了 0——**而 Qwen3-TTS 的训练长度是
    // 32768，KV cache 按整个窗口预分配，一次要 3584 MiB**，在这张卡上直接
    // OOM，然后静默退回 estimate 后端，成片无声。算账和现场都记在
    // kTtsContextTokens 头上。
    //
    // 参考实现敢用 0，是因为它是个命令行工具、独占整张卡；这条流水线上
    // 配音要和大模型、出图、出片抢显存，多要的每一 GB 都是从别人那儿抢的。
    cp.n_ctx = kTtsContextTokens;

    // **要逐 token 的隐状态，所以不能池化。**
    //
    // 默认是 UNSPECIFIED，llama.cpp 会去看模型的 hparams，拿不到才退成
    // NONE。Qwen3 骨干上大概率就是 NONE，但这里依赖的是
    // `llama_get_embeddings_ith(ctx, -1)` 拿最后一个 token 的隐状态——
    // 一旦哪个模型的 hparams 带了池化类型，这条路就悄悄取到别的东西。
    // 显式写死，不赌默认值。
    cp.pooling_type = LLAMA_POOLING_TYPE_NONE;

    // **线程数按这台机器的核数来，别用 llama 的默认值。**
    //
    // `llama_context_default_params()` 给的 n_threads 和 n_threads_batch
    // 都是 `GGML_DEFAULT_N_THREADS`，也就是**写死的 4**（ggml.h:232，
    // 上游自己在那一行标着 `TODO: better default`）。这里原来一个都没覆盖。
    //
    // 有显卡时无所谓——层全 offload 到 GPU（上面 n_gpu_layers = 999）。
    // **没显卡时这就是实打实的四分之一**：2026-09-15 在一台 16 核的机器上
    // 实测，配音那一步只有 4 个核在动，而同一台机器出图那条走的是 sd.cpp
    // 的 `n_threads = -1`，被解析成物理核数（sd.cpp 的
    // `n_threads > 0 ? n_threads : sd_get_num_physical_cores()`），满载。
    // 同一个进程里两条路差着四倍，纯粹是因为这儿漏了一行。
    //
    // 而发布的六个 CPU 包面向的正是没有显卡的机器——这一行对它们不是优化，
    // 是把本来就该用上的算力用上。
    //
    // 用 common 那个而不是 `hardware_concurrency()`：后者数的是逻辑核，
    // 超线程机器上拿逻辑核数去跑 ggml 通常比物理核数还慢。
    if (const int n = ggml_threads(); n > 0) {
        cp.n_threads = n;
        cp.n_threads_batch = n;
    }

    im.lctx = llama_init_from_model(im.model, cp);
    if (im.lctx == nullptr) {
        why = SAY("建不出 llama context");
        return nullptr;
    }

    mtmd_context_params mtp = mtmd_context_params_default();
    mtp.use_gpu = use_gpu;
    // **解码器这半也要，理由同上面那段。** 骨干和解码器是两个上下文，
    // 各自带一份 n_threads，都默认 4。只改骨干那一个的话，一句话里
    // 前半段满载、后半段还在四个核上爬。
    if (const int n = ggml_threads(); n > 0) mtp.n_threads = n;
    im.mctx = mtmd_init_from_file(paths::to_utf8(mmproj).c_str(), im.model, mtp);
    if (im.mctx == nullptr) {
        why = SAYF("解码器载不起来：%1", paths::to_utf8(mmproj));
        return nullptr;
    }

    const mtmd_gen_audio_info info = mtmd_gen_audio_get_info(im.mctx);
    if (info.type == MTMD_GEN_AUDIO_TYPE_NONE) {
        // 拿错文件了——比如把两份 GGUF 的位置对调。这句话要说得具体，
        // 否则用户看到的是"配音失败"，然后去查显卡。
        why = SAYF("这份 mmproj 不支持音频生成：%1。配音要的是 tokenizer/"
                   "解码器那一份，不是骨干那一份。",
                   paths::to_utf8(mmproj));
        return nullptr;
    }
    im.sample_rate = info.sample_rate;

    return self;
}

bool LlamaTts::synthesize(const LlamaTtsRequest& req, double& out_duration_s,
                          std::string& why) {
    Impl& im = *impl_;
    std::lock_guard<std::mutex> only_one(im.run_mu);   // 见 Impl::run_mu
    if (req.text.empty()) {
        why = SAY("台词是空的");
        return false;
    }

    // 采样器每句重建：种子要按请求来，而链是不可变的。
    if (im.smpl != nullptr) llama_sampler_free(im.smpl);
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    im.smpl = llama_sampler_chain_init(sp);
    // 顺序照 HF 的 generate：重复惩罚先改 logits，再 top_k / top_p 截断，
    // 再温度，最后按分布抽。四个数的来历见 LlamaTtsRequest。
    // 惩罚窗口给整段上下文（HF 的 repetition_penalty 看的是整个已生成
    // 序列）。**不能传 -1**：common 那层把 -1 当"上下文长度"，而这里直接
    // 调的是核心采样器，它做的是 `max(last_n, 0)`——-1 变 0，0 = 关掉，
    // 于是这一道悄悄不存在。上下文本来就是按 kTtsContextTokens 开的，
    // 窗口给这个数就是"全部"。只惩罚"重复"这一项，频率/存在惩罚官方没开。
    if (req.repetition_penalty > 0.0f && req.repetition_penalty != 1.0f) {
        const llama_vocab* vocab = llama_model_get_vocab(im.model);
        llama_sampler_chain_add(
            im.smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab),
                                                  kTtsContextTokens,
                                                  req.repetition_penalty, 0.0f,
                                                  0.0f));
    }
    llama_sampler_chain_add(im.smpl, llama_sampler_init_top_k(req.top_k));
    llama_sampler_chain_add(im.smpl, llama_sampler_init_top_p(req.top_p, 1));
    if (req.temperature > 0.0f) {
        llama_sampler_chain_add(im.smpl, llama_sampler_init_temp(req.temperature));
    }
    llama_sampler_chain_add(im.smpl, llama_sampler_init_dist(req.seed));

    mtmd::bitmap_ptr speaker;
    if (req.speaker_ref.has_value()) {
        auto wrapper = mtmd_helper_bitmap_init_from_file(
            im.mctx, paths::to_utf8(*req.speaker_ref).c_str(), false,
            mtmd_helper_init_opt_default());
        if (wrapper.bitmap == nullptr) {
            // **把认得的格式列出来。** mtmd 那边走 miniaudio，只认这三种
            // （mtmd-helper.h 里写着 "audio: formats supported by miniaudio:
            // wav, mp3, flac"）。拿一个 m4a 或者 ogg 过来是很常见的事，
            // 而只说"读不了"的话，用户会去查路径对不对、文件坏没坏。
            why = SAYF("参考音色读不了：%1（只认 wav / mp3 / flac）",
                       paths::to_utf8(*req.speaker_ref));
            return false;
        }
        speaker.reset(wrapper.bitmap);
    }

    // **每次合成前把 KV 缓存清干净。**
    //
    // 少了这一句，**第二句台词就配不出来**——`step_prompt` 返回负数，
    // 报"跑提示词时失败"。2026-09-08 跑真流水线时撞到的：一章里第一句
    // 出得好好的，第二句必挂；重启服务之后那一句又能出。
    //
    // 原因是下面 `inp.seq_id = 0` 每次都用同一个序列，而这个
    // LlamaTts 实例在整条流水线上是**复用**的（模型 1 GB 出头，
    // 每句重载一遍不现实，见头文件）。不清的话第二句的 token 接在
    // 第一句后面，上下文越堆越长。
    //
    // **`--say` 那条路发现不了**：它建一次、用一次、进程就退了。
    // 头文件里"已知没验过：并发调用"那一条说的就是这一类，
    // 只是真正的破绽不是并发，是**顺序复用**。
    llama_memory_clear(llama_get_memory(im.lctx), true);

    mtmd_helper::gen_audio gen(im.lctx, im.mctx);
    mtmd_helper_gen_audio_inp inp{};
    inp.seq_id = 0;
    inp.prompt = req.text.c_str();
    inp.prompt_len = req.text.size();
    inp.speaker_ref = speaker.get();
    inp.lang = req.lang.empty() ? nullptr : req.lang.c_str();
    inp.top_k = req.top_k;
    inp.top_p = req.top_p;
    inp.seed = req.seed;
    inp.out_type = MTMD_HELPER_GEN_AUDIO_OUTTYPE_WAV;

    if (gen.set_input(&inp) != 0) {
        why = SAY("喂不进去这句台词");
        return false;
    }

    // 第一阶段：把提示词过一遍骨干模型。
    for (;;) {
        const int32_t left = gen.step_prompt(512);
        if (left < 0) {
            why = SAY("跑提示词时失败");
            return false;
        }
        if (left == 0) break;
    }

    const auto sample = [&] {
        const llama_token t = llama_sampler_sample(im.smpl, im.lctx, -1);
        llama_sampler_accept(im.smpl, t);
        return t;
    };

    // 第二、三阶段：语义码 → 声学细节 → 波形。
    // **循环由调用方驱动**——mtmd 那套 API 是无状态的，头文件里写着
    // "caller must handle state management and audio frame accumulation"。
    llama_token sampled = sample();
    const float* h_state = llama_get_embeddings_ith(im.lctx, -1);
    if (h_state == nullptr) {
        // 几乎一定是 context 没开 embeddings。上面开了，所以走到这儿
        // 说明上游改了行为——把话说明白，别让人去查模型文件。
        why = SAY("拿不到骨干模型的隐状态（context 的 embeddings 没开？）");
        return false;
    }

    int frames = 0;
    bool stop = false;
    // 上下文是按 kTtsMaxFrames 开的，调用方把 max_frames 设大了也不能超——
    // 超了是 llama 那边报错，指不到这儿。
    const int frame_cap = std::min(req.max_frames, kTtsMaxFrames);
    while (!stop && frames < frame_cap) {
        const float* h_next = nullptr;
        if (gen.step_gen(sampled, h_state, &h_next, &stop) != 0) {
            why = SAYF("第 %1 帧生成失败", std::to_string(frames));
            return false;
        }
        if (h_next == nullptr) break;  // 停了，且这一帧没产出
        ++frames;
        h_state = h_next;
        sampled = sample();
    }
    if (frames == 0) {
        why = SAY("一帧都没生成出来");
        return false;
    }

    int32_t rate = 0;
    const char* data = nullptr;
    size_t len = 0;
    std::int64_t samples = 0;
    if (gen.get_output(&rate, &data, &len, &samples) != 0) {
        why = SAY("取不到合成结果");
        return false;
    }
    if (data == nullptr || len == 0) {
        why = SAY("合成结果是空的");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(req.out.parent_path(), ec);

    // **不能用 fopen(const char*)。** Windows 上那个路径按 ANSI 代码页
    // 解释，中文项目名直接写不进去，而且报的错是"写文件失败"，
    // 看不出是编码问题。`sd_image.cpp` 里写 PNG 那段已经踩过一次并写了
    // 注释，这里是我几天前新写的代码，又踩了同一个坑。
    //
    // ofstream 吃 fs::path 就没这个问题：MSVC 上它走宽字符那条路。
    std::ofstream out(req.out, std::ios::binary | std::ios::trunc);
    if (!out) {
        why = SAYF("写不了 %1", paths::to_utf8(req.out));
        return false;
    }
    out.write(data, static_cast<std::streamsize>(len));
    if (!out) {
        why = SAYF("写 %1 时出错（磁盘满了？）", paths::to_utf8(req.out));
        return false;
    }
    out.close();

    out_duration_s = rate > 0 ? static_cast<double>(samples) / rate : 0.0;
    return true;
}

#else

bool llama_tts_available() { return false; }

LlamaTtsProbe probe_llama_tts() {
    LlamaTtsProbe out;
    out.ok = true;  // 没编进来不算故障，配音走外部后端
    out.detail = SAY("没编进来（CHANGJI_LLAMA=OFF），配音走外部后端");
    return out;
}

struct LlamaTts::Impl {};
LlamaTts::LlamaTts() : impl_(std::make_unique<Impl>()) {}
LlamaTts::~LlamaTts() = default;
int LlamaTts::sample_rate() const { return 0; }

std::unique_ptr<LlamaTts> LlamaTts::load(const std::filesystem::path&,
                                         const std::filesystem::path&, bool,
                                         std::string& why) {
    why = SAY("这个二进制没编进程内配音（CHANGJI_LLAMA=OFF）");
    return nullptr;
}

bool LlamaTts::synthesize(const LlamaTtsRequest&, double&, std::string& why) {
    why = SAY("这个二进制没编进程内配音（CHANGJI_LLAMA=OFF）");
    return false;
}

#endif

}  // namespace changji::infer
