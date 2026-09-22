#pragma once

// 两个真配音后端：独立 HTTP 服务、ComfyUI 的 TTS 节点。
//
// 移植自 src/changji/stages/audio.py 的 HttpTTSBackend 和 ComfyTTSBackend。
//
// ---
//
// **这里最重要的一条不是怎么调，是怎么发现"调成功了但没出声"。**
//
// ComfyUI 的 TTS 节点内部捕获异常之后，会输出一个一秒的空音频然后正常返回，
// 服务端的执行状态报的是 success。只信状态码的客户端会被完全骗过，
// 拿到一堆静音文件还以为配音成功了。实测撞上过：节点缺 librosa 报错，
// 任务状态却是成功。
//
// 所以不能只信状态，必须验证产出物本身——见 reject_silent_audio。

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "models/character.hpp"
#include "models/project.hpp"
#include "stages/audio.hpp"

namespace changji::stages {


/// 一秒以内的音频几乎不可能是一句正常台词，多半是节点失败后的占位输出。
inline constexpr double kMinPlausibleDurationS = 1.05;

/// 检查产出的音频是不是真的有声音。不合格就抛。
///
/// 判据是**两条下限都过才放行**：绝对下限（1.05 秒）和相对下限
/// （按语速估算的 35%）。只用相对下限的话，估算本身偏得厉害时挡不住。
///
/// ⚠️ 副作用：一个字的台词（"嗯。"估约 0.94 秒）配出来不到 1.05 秒时会被
/// 误判成空音频。**照抄 Python，不在这里悄悄放宽**——放宽的代价是
/// 一整章静音文件被当成配音成功，而那比偶尔误杀一句严重得多。
/// 真撞上了要改的是那个常数本身，并且要有实测支撑。
void reject_silent_audio(const std::filesystem::path& path, double duration_s,
                         const std::string& text);

/// 读音频时长。**wav 自己读，别的格式退回 ffprobe。**
///
/// 大部分 TTS 出的就是 wav，那条路不该为一个时长起子进程；
/// 而 mp3/flac 这些又只能靠 ffprobe。没有 ffmpeg 时非 wav 直接抛，
/// 话里说清是缺 ffmpeg，不是文件坏了。
double probe_audio_duration(const std::filesystem::path& path,
                            const std::optional<media::FFmpeg>& ff);

/// 指向一个独立的 HTTP 配音服务。
///
/// 很多 TTS 项目自带 api 服务，比 ComfyUI 节点更省事，
/// 而且可以跑在另一台机器上。
TTSBackend http_tts_backend(const std::string& base_url, double timeout_s,
                            llm::HttpPost post,
                            const std::optional<media::FFmpeg>& ff);


/// 进程内配音（阶段 9）。**要 CHANGJI_LLAMA=ON 编出来的二进制。**
///
/// 和另外两个后端的区别在于**模型只载一次**：1.5 GB 的权重，
/// 每句台词重载一遍的话，一章几十句就是几十次载入。
/// 所以这里握着一个 shared_ptr，跟着 TTSBackend 的生命周期走。
///
/// 载不起来返回 nullopt——调用方按"这条路搭不起来"处理，退回估算后端，
/// 和另外两个后端连不上时是同一套逻辑。
///
/// ⚠️ 这条路径**从来没跑过**，见 infer/llama_tts.hpp 开头。
std::optional<TTSBackend> local_tts_backend(
    const std::filesystem::path& backbone, const std::filesystem::path& decoder,
    bool use_gpu, const std::optional<media::FFmpeg>& ff, std::string& why);

/// 按配置搭一个配音后端。搭不起来退回估算后端（那会写出等长静音）。
///
/// **顺带把调度器里的配音槽注册上。** 注册是 local_tts_backend 做的事，
/// 而借槽的人（render_voice_take、AudioStage）自己不注册——引擎刚重启、
/// 还没跑过任何一章时直接去借，报的是「槽 配音 还没注册」，指不到根因。
/// 2026-09-13 实测撞到：八个种子全部摇不出来，就是这一条。
///
/// 抽出来是因为 tts_api.cpp 和 voices.cpp 都要它，而这套代码里"同一段
/// 逻辑抄两份"迟早分叉（见 http/reset.hpp 开头那段）。
/// 只把**进程内**那条搭起来，顺带注册调度器里那个配音槽。
///
/// 比下面那个窄一号，是因为有些调用方（摇音色）本来就只认进程内这一条，
/// 而**宽的那个要一个 HTTP poster**——`llm::default_http_post` 定义在
/// client_http.cpp 里，那个文件链 httplib，CMakeLists 专门把它排除在
/// 单元测试的链接之外。在这儿调它，测试目标就链不上了
/// （2026-09-13 栽过：undefined reference to default_http_post）。
std::optional<TTSBackend> ensure_local_tts(
    const config::Settings& s, const std::optional<media::FFmpeg>& ff,
    std::string& why);

/// **poster 由调用方给**，不在这儿调 `llm::default_http_post()`：
/// 理由见上面那段。
TTSBackend pick_tts_backend(const config::Settings& s,
                            const std::optional<media::FFmpeg>& ff,
                            const llm::HttpPost& post);

/// 一个预置音色：一个种子，外加那次实测的基频。
struct PresetVoice {
    unsigned int seed = 0;
    /// 实测基频（Hz）。**给界面当"摇之前的提示"用**，不是承诺——
    /// 真正显示的是摇完当场量的那个。
    ///
    /// **这个数只在配 kVoiceText 那一段文本时成立。** 音色是
    /// (种子, 文本) 的函数，换一段文本同一个种子就是另一个人
    /// （2026-09-13 实测，见 tts_backends.cpp 里 kVoiceText 上面那段）。
    int hz = 0;
};

/// 预置音色。**是一组固定的种子，不是一组下载来的音频。**
///
/// 我们这条运行时（llama.cpp 的 mtmd）只实现了 Qwen3-TTS 的 Base 模式，
/// 也就是参考音频克隆；自带 9 个说话人的 CustomVoice 和用文字描述造音色
/// 的 VoiceDesign 都不在里面（上游 PR #26254）。而不给参考音频时说话人是
/// 被采样出来的，**种子和文本定死了，摇出来的人就定死了**——所以固定
/// 几个种子就等于固定几个音色，每个人在每台机器上拿到的是同一套。
const std::vector<PresetVoice>& preset_voices();

/// 摇音色念的那一段。预置表里那些 hz 都是配它量出来的。
const char* voice_sample_text();

/// **给一个还没有音色的角色定一个，并把参考音频落到项目里。**
///
/// 返回相对项目根的路径；角色已经有音色了就原样返回，不重出。
///
/// **为什么必须有这一步。** 不给参考音频时，每次合成都重新采样一个说话
/// 人，而音色是 (种子, 文本) 的函数——换一句台词就是换一个人。
/// 2026-09-13 在 walk_c 上量到的：同一个角色 c_lin_hao 的**同一句话被
/// 拆成两半**，前半句 136 Hz、后半句 338 Hz，一句话说到一半换了个人。
/// 整章每个角色每句话都是不同的人，而且全程不报错。
///
/// 按角色的 voice_gender 从预置表里挑（男声挑低的、女声挑高的），
/// 用 voice_order 错开，免得两个男角色撞成同一个人。
std::string ensure_character_voice(const models::ProjectStore& store,
                                   models::Character& c);

/// 按种子出一段**不带参考音色**的语音——「制作音色」那条路专用。
///
/// **为什么不走 TTSBackend::synthesize。** 那个签名上没有 seed，加上去要
/// 动三个后端和每一个调用方；而这条路本来就只对进程内那一条有意义——
/// 外部配音服务的音色是它自己管的名字，不是我们生成出来的片段。
///
/// **种子就是音色。** 不给参考音频时，说话人是和内容一起被采样出来的，
/// 换一个种子就是换一个人。Qwen3-TTS 那边"设固定种子以减少音色漂移"
/// 说的是同一件事的反面。所以「制作音色」= 摇种子 → 试听 → 满意了把这
/// 一段存成参考音频，从此它被克隆锁死，再也不会变。
///
/// 返回时长（秒）。进程内配音没装起来、或者出来是静音，抛 AudioError。
double render_voice_take(const std::string& text,
                         const std::filesystem::path& out, unsigned int seed);

}  // namespace changji::stages
