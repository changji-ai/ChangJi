#pragma once

// 进程内配音（阶段 9）的门面。
//
// Qwen3-TTS 在 2026-08-04 进了 llama.cpp 主线（PR #26254，走 mtmd），
// 所以配音的 C++ 化不再需要自己写 codec——但**实现在 mtmd 里，不在
// libllama 里**（`tools/mtmd/models/qwen3tts-gen.cpp`、`qwen3tts-spkenc.cpp`）。
// mtmd 挂在 `tools/` 下面，上游留了 `LLAMA_BUILD_MTMD` 这个钩子可以单独编，
// 不用把整个 tools/ 和 common 拖进来。
//
// ---
//
// **2026-09-08：真跑通了。** 权重下齐之后第一次执行这条路径：
//
//     changji.exe --say "雨下了一整夜。" --tts-model ... --tts-decoder ...
//     载好了，采样率 24000 Hz
//     出声了：say_test.wav，1.92 秒
//
// 而且**验过不是静音**——峰值 3984（满幅的 12.2%）、RMS 756、
// 66.6% 的样本在噪声底之上。这一步必须验：这套代码里
// `reject_silent_audio` 存在的全部理由就是"成功了但没出声"和
// "成功了"在返回值上一模一样。只看退出码就宣布成功，等于重犯
// 那个错。
//
// 一次成功不等于稳。已知没验过的：长文本、参考音色（--voice）、
// 并发调用、显存不够时的表现。
//
// 参照的是 llama.cpp 自带的 `tools/tts/tts.cpp`，那个文件自己开头就写着
// "this is NOT a production-ready binary"。
//
// 而且 mtmd 的音频生成 API 在头文件里明写着
// `EXPERIMENTAL API for audio generation, subjected to breaking changes`。
// 钉死的 llama.cpp commit 是 `5202104`，升级时**这一段要重新对照上游的
// tts.cpp 看一遍**，不要假设签名没变。
//
// 门面写法同 sd_backend.hpp：头文件不带 #ifdef、不 include mtmd。

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace changji::infer {

/// mtmd（也就是进程内配音的实现）编进来了没有。编译期决定。
bool llama_tts_available();

struct LlamaTtsProbe {
    bool ok = false;
    /// 给人看的一句话。
    std::string detail;
};

/// 真的调一次 mtmd，确认它不只是"编过了"，而是**链进来且叫得动**。
///
/// 为什么要这一下：mtmd 是静态库，我们又还没有任何地方调它的函数，
/// 链接器完全可以把它整个丢掉，而构建日志里照样有一行
/// "Linking CXX static library mtmd.lib"。**"编出来了"不等于"在二进制里"。**
LlamaTtsProbe probe_llama_tts();

/// 一次生成最多多少帧。12.5 Hz 的码率下 512 帧约 41 秒，
/// 比单句台词的上限（见 stages/limits.hpp 的 max_line_seconds）宽得多。
inline constexpr int kTtsMaxFrames = 512;

/// 提示词那头要留多少 token。
///
/// 单句台词最长 200 字（models::DialogueLine 的 maxLength），中文大约
/// 一个字 1~1.5 个 token，300 封顶。这里给到五倍，再算上特殊 token 和
/// 音色条件，绰绰有余。
inline constexpr int kTtsPromptHeadroom = 1536;

/// 上下文开多大。**这个数直接决定 KV cache 占多少显存。**
///
/// 2026-09-13 实跑撞上的：这里原来是 `n_ctx = 0`，llama.cpp 的语义是
/// 「用模型训练时的长度」，而 Qwen3-TTS 的 `context_length` 是 **32768**。
/// KV cache 按整个窗口**预分配**（28 层 × 8 个 KV 头 × (128+128) × 2 字节
/// = 112 KiB/token，× 32768 = **3584 MiB**），于是：
///
///     allocating 3584.00 MiB on device 0: cudaMalloc failed: out of memory
///     failed to initialize the context: failed to allocate buffer for kv cache
///
/// 而调度器给配音槽估的是 3 GB（tts_backends.cpp），腾地方按 3 GB 腾，
/// 真装要 5.5 GB——**腾够了也装不上**。装不上就静默退回 estimate 后端，
/// 表现是「成片会是静音」，而根因在日志几千行之外。
///
/// 真正要用的是「提示词 + 最多 512 帧」，2048 就够，KV 降到 224 MiB，
/// 3 GB 那个估算也跟着变准了。**不能回头写 0**，也不能用
/// llama_context_default_params() 的 512——那个太小，一句长台词就撑爆，
/// 症状是生成中途失败、指不到上下文上。
inline constexpr int kTtsContextTokens = kTtsMaxFrames + kTtsPromptHeadroom;

/// 一次合成要的东西。
struct LlamaTtsRequest {
    std::string text;
    /// 参考音色的音频文件。留空就用模型的默认音色。
    ///
    /// **只认 wav / mp3 / flac**——mtmd 那边走 miniaudio，就这三种
    /// （见 `mtmd-helper.h` 对 `mtmd_helper_bitmap_init_from_buf` 的注释）。
    std::optional<std::filesystem::path> speaker_ref;
    /// 语种提示。Qwen3-TTS 支持十种，留空让它自己判断。
    std::string lang;
    /// 输出到哪个 wav。
    std::filesystem::path out;

    /// 采样旋钮。**默认值抄的是官方仓库随模型发布的 generation_config.json**
    /// （Qwen/Qwen3-TTS-12Hz-1.7B-Base，2026-09-13 核过）：
    ///
    ///     temperature 0.9 · top_k 50 · top_p 1.0 · repetition_penalty 1.05
    ///
    /// 以前是 top_k 40 / top_p 0.9、没有温度（等于 1.0）、没有重复惩罚——
    /// 那是 llama.cpp 的通用默认，不是这个模型的。差在两头：温度 1.0 比
    /// 官方热，top_p 0.9 又比官方窄；而重复惩罚那一项官方是开着的，它管的
    /// 是语义码重复——同一个码连着出就是那种「卡住的音节」。
    ///
    /// 子模型（声学细节那一层）的 top_k / top_p 也传这两个数；它的温度
    /// mtmd 自己钉在 0.9（mtmd.cpp 里那行 `inp.temp = 0.9f`），正好也是
    /// 官方 subtalker 的值，所以不用另给。
    int top_k = 50;
    float top_p = 1.0f;
    float temperature = 0.9f;
    /// 1.0 = 关。作用在骨干模型采语义码那一层，窗口是整段上下文
    /// （HF 的 repetition_penalty 语义就是看整个已生成序列）。
    float repetition_penalty = 1.05f;
    /// UINT32_MAX 表示随机。**默认给一个定值**：配音重跑一次就换一个
    /// 声音的话，用户没法靠重跑修一句坏台词，只能整章重配。
    unsigned int seed = 1234;
    /// 一帧一帧生成的上限。见 kTtsMaxFrames。
    ///
    /// **调大它必须同时调大 kTtsContextTokens**，否则帧还没生成完上下文
    /// 就满了——所以生成那边会按 kTtsMaxFrames 夹一道。
    int max_frames = kTtsMaxFrames;
};

/// 加载好的模型。**加载很贵（1.5 GB 权重），要跨多句台词复用。**
///
/// 不可拷贝：里面握着 llama_model / llama_context / mtmd_context 三个句柄。
class LlamaTts {
public:
    /// backbone 是 talker 那份 GGUF，mmproj 是 tokenizer（解码器）那份。
    /// 载不起来返回 nullptr，`why` 里说清是哪一步。
    static std::unique_ptr<LlamaTts> load(const std::filesystem::path& backbone,
                                          const std::filesystem::path& mmproj,
                                          bool use_gpu, std::string& why);

    ~LlamaTts();
    LlamaTts(const LlamaTts&) = delete;
    LlamaTts& operator=(const LlamaTts&) = delete;

    /// 合成一句，写成 wav。失败返回 false 并填 `why`。
    ///
    /// 出来的音频时长写进 `out_duration_s`——调用方要拿它去锁镜头时长，
    /// 而重新读一遍文件是白读。
    bool synthesize(const LlamaTtsRequest& req, double& out_duration_s,
                    std::string& why);

    /// 采样率，来自模型自己报的（Qwen3-TTS 是 24000）。
    int sample_rate() const;

private:
    LlamaTts();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace changji::infer
