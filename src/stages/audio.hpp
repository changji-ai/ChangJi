#pragma once

// 配音编排：跑 TTS 并反推锁定镜头时长。
//
// 移植自 src/changji/stages/audio.py 的 AudioStage 那一半。
// 估时长和拆分在 audio_plan.hpp。
//
// ---
//
// **后端是注入的函数对象**，和 frames / render 两个阶段一样。
// 一是分层，二是很实际：这一层真正难的是**重切循环**——
// 估算的语速和引擎实测差得很远，估 4.8 秒出来 5.8 秒是真实发生过的，
// 那 1 秒会盖到下一镜上去。这条逻辑要能在毫秒级反复跑才测得动。

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/character.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "stages/audio_plan.hpp"

namespace changji::stages {

class AudioError : public std::runtime_error {
public:
    explicit AudioError(const std::string& what) : std::runtime_error(what) {}
};

/// 一句台词的配音结果。
struct SynthesisResult {
    double duration_s = 0.0;
    /// 没出音频时是空的（只估时长的那种后端）。
    std::optional<std::filesystem::path> audio_path;

    bool is_real_audio() const { return audio_path.has_value(); }
};

/// 合成一句台词，写到 out_path。
using Synthesizer = std::function<SynthesisResult(
    const std::string& text, const std::filesystem::path& out_path,
    const std::optional<std::string>& voice_id, const std::string& emotion,
    double intensity)>;

/// 服务端有哪些参考音色。返回空表示问不到——**那时候不要自作主张**，
/// 照角色资产里填的来。
using VoiceLister = std::function<std::vector<std::string>()>;

/// 配音后端。
struct TTSBackend {
    std::string name = "estimate";
    Synthesizer synthesize;
    /// 可以留空：那表示问不到音色列表。
    VoiceLister list_voices;
};

/// 只算时长不出音频的后端。
///
/// 用途有两个：在没装 TTS 的机器上把整条流水线跑通，以及排产时估算总时长。
/// **产出的是等长静音 wav**，所以后面的装配环节不用为它写特例。
TTSBackend estimate_backend(int sample_rate = 24000);

/// 写一段静音 wav。
void write_silence(const std::filesystem::path& path, double seconds,
                   int sample_rate);

/// 读 wav 的时长。
///
/// **不走 ffprobe**：配音每句都要读一次，为它多起一个子进程不划算，
/// 而且这样在没装 ffmpeg 的机器上配音那条路照样能跑。
double probe_wav_duration(const std::filesystem::path& path);

/// 估一段 16 位 PCM wav 的基频，单位 Hz。测不出来（不是 wav、全静音、
/// 没有周期性）返回空。
///
/// **为什么要有它。** 「制作音色」那条路是摇种子——不给参考音频时种子
/// 决定音色，摇出来一段就是一个新声音。可摇出来之后它叫什么？我没法
/// 听，也不该替用户断言"这是沉稳中年男声"。基频是**能量出来的客观数**：
/// 男声大致 85~180 Hz，女声 165~255 Hz，标出来比编一个形容词诚实，
/// 而且和角色资产里那个 voice_gender 对得上。
///
/// 做法是逐帧自相关取中位数。中位数不是均值：浊音段里偶尔会有一帧
/// 落到倍频或者半频上，均值会被拽走，中位数不会。
std::optional<double> estimate_wav_f0(const std::filesystem::path& path);

/// 读 16 位 PCM wav 的峰值（最大采样绝对值 / 满幅，0..1）。
///
/// 给"疑似空音频"那道检查用：ComfyUI 节点失败时吐的占位音频是**全零**，
/// 而一句真的短台词（「苏晚！」1.04 秒）峰值有满幅的一成。只看时长
/// 分不开这两种，看峰值一眼就分开了。不是 16 位 PCM 或者读不出来就
/// 回空——那时候检查退回只看时长。
std::optional<double> wav_peak_ratio(const std::filesystem::path& path);

/// 跑配音并锁定镜头时长。
/// 参考音色：项目内的相对路径还原成绝对的，别的原样放过。
///
/// **为什么要有它。** 进程内配音把 `voice_id` 当**一段音频的路径**用
/// （tts_backends.cpp 里 `req.speaker_ref`），而资产库里存的是相对项目根的
/// `voices/c_xxx.wav`——那是有意的，项目整个拷到别的机器上还能读。不还原
/// 的话它解析到**引擎进程的当前目录**（服务器上是 /root），必然打不开，
/// 而报的错只是一句"参考音色读不了"。2026-09-13 参考图那个坑是同一个形状。
///
/// **判据是"项目里真有这个文件"，不是"看着像路径"。** 外部配音服务那条路
/// 上 voice_id 是音色名（"zh-CN-XiaoxiaoNeural"），按后缀猜、按有没有斜杠
/// 猜都会猜错；而"项目里有这个文件"是一查就知道的事实。
///
/// 出片那条（AudioStage）和试听那条（/api/tts/say）都要过它——两处各写
/// 一份的话，迟早只改一边，而那种 bug 的表现是"镜头里能用、试听不出声"。
std::optional<std::string> resolve_voice(const std::optional<std::string>& voice,
                                         const models::ProjectPaths& paths);

class AudioStage {
public:
    AudioStage(TTSBackend backend, config::TTSConfig config,
               models::ProjectPaths paths);

    /// 给这些镜头配音，然后反推锁定时长。
    ///
    /// `concurrency` 是同时跑几镜。**1 就是原来的行为**（逐镜串行），
    /// 逐字节一样。
    ///
    /// 这儿原来写死串行，理由两条：「配音服务通常就一张卡，并发只会让每句
    /// 都变慢」和「shot.dialogue 被就地改，两条线程改同一个镜头就乱了」。
    /// 2026-09-17 第一条不成立了——配音早就走池（run_deps.cpp 里
    /// `tts_pool`），一台双卡机报两个位置，串行就是**空着一张卡**。
    /// 用户：「配音是不是没有走多 gpu」。
    ///
    /// 第二条是真的，所以照 frames / render 那两层的规矩来：并行那一段
    /// **一个字节都不往 Shot 里写**，每一路在自己的副本上跑；写回等全部
    /// 收完，在调用线程上按镜头原顺序做。
    std::vector<ShotAudioPlan> run(std::vector<models::Shot*>& shots,
                                   const models::AssetLibrary& assets,
                                   pipeline::JobProgress& progress,
                                   pipeline::CancelToken& tok,
                                   int concurrency = 1);

    /// 处理一个镜头。公开是为了能单独测重切循环。
    ShotAudioPlan process_shot(models::Shot& shot,
                               const models::AssetLibrary& assets,
                               pipeline::CancelToken& tok);

    /// 服务端不认的音色。**收集起来报给用户**——
    /// 不然自动换了声音他不知道，只会觉得"这个角色听着不像上次那个"。
    const std::set<std::string>& unknown_voices() const { return unknown_; }

private:
    /// 过长的台词先按字数估一遍切开。
    void split_long_lines(models::Shot& shot) const;
    std::optional<std::string> voice_for(const models::DialogueLine& line,
                                         const models::AssetLibrary& assets);
    const std::vector<std::string>& available_voices();
    /// 由配音时长反推镜头时长。
    double lock_duration(models::Shot& shot, double speech_s) const;

    TTSBackend backend_;
    config::TTSConfig config_;
    models::ProjectPaths paths_;
    std::optional<std::vector<std::string>> voices_;
    /// 认不出的音色名。**并行时多路一起写**，所以带锁（见 unknown_mu_）。
    std::set<std::string> unknown_;
    /// 只护 unknown_。voices_ 不靠它——开跑前先预热一次，
    /// 那之后就只读（见 run 里那句 available_voices()）。
    std::mutex unknown_mu_;
};

/// 一句台词最多重切几次。
///
/// 估算不准是常态，但切三次还装不下的话多半是引擎那边出了别的问题，
/// 再切下去只是把话剁碎。
inline constexpr int kMaxResplits = 3;

}  // namespace changji::stages
