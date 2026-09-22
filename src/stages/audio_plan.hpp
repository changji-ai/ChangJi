#pragma once

// 配音的时长估算与拆分。
//
// 移植自 src/changji/stages/audio.py 里不依赖 TTS 引擎的那一半。
// 引擎那一半（HTTP / ComfyUI 后端、AudioStage 编排）单独一层。
//
// ---
//
// **这一层错了的后果是成片里两个人同时说话。**
//
// 单镜时长有硬上限，来自视频模型能生成的最大帧数（4n+1，上限 121 帧）。
// 一个镜头里塞了两三句台词、配出来七八秒而镜头只有五秒，混音时后面的
// 声音就盖到下一镜上去了。而这件事在装配之前没有任何迹象——
// 分镜表看着正常，每一段音频单独听也正常。

#include <set>
#include <string>
#include <vector>

#include "models/shot.hpp"

namespace changji::stages {

/// 中文语速，字/秒。
///
/// 4.6 是电影对白的经验值。播音腔更慢，日常对话更快；
/// 估高了镜头会留白太多，估低了配音装不进去——后者更糟。
inline constexpr double kCharsPerSecond = 4.6;
/// 每句前后的呼吸留白。不留的话拼接处听起来是抢话。
inline constexpr double kLeadInS = 0.15;
inline constexpr double kTailS = 0.25;

/// 估算一句中文台词的时长。
///
/// **标点不发音但产生停顿**，所以要摘出来单独计：逗号短一点，
/// 句号问号感叹号长一点。把标点当成字算的话，一句"什么？！"会被
/// 估成四个字的长度，而它实际上要停顿快一秒。
double estimate_speech_duration(const std::string& text);

/// 一句台词能占的最长时间。
///
/// 超过单镜上限的台词，配出来的音频装不进任何一个镜头。留出尾巴的余量。
double max_line_seconds(int fps = 24);

/// 把过长的台词按标点切成几句，每句都装得进一个镜头。
///
/// **先在句末标点处切，切不够再退到逗号分号。** 硬切字数是最后手段，
/// 那样会把词切断，听起来很别扭，但总好过整句被下一镜的声音盖住。
std::vector<std::string> split_long_text(const std::string& text,
                                         double max_seconds);

/// 给拆出来的新镜取一个**整章没用过**的编号。
///
/// 编号要跟整章比对着发。同一章重跑一次配音会再拆一次，只按本次的序号
/// 取名的话第二次又会取出一个 sh001_b，于是一章里出现两个同名镜头：
/// 按 id 找镜头只能找到头一个，音频和首帧的文件名也会互相覆盖。
std::string free_shot_id(const std::string& base,
                         const std::set<std::string>& used);

/// 把一个镜头的台词按时长打包，每包都装得进一个镜头。
std::vector<std::vector<models::DialogueLine>> group_lines(
    const models::Shot& shot, double max_seconds);

/// 把装不下自己台词的镜头拆成连着的几镜。
///
/// **在配音之后、出首帧之前拆。** 这时候音频已经有了、画面还没生成，
/// 拆开不浪费任何一次渲染。原镜留着第一组台词和已有的音频，
/// 后面几组各起一个新镜从头生成。
std::vector<models::Shot> split_overlong_shots(std::vector<models::Shot> shots,
                                               double max_seconds);

/// 一个镜头的配音结果与时长决策。
struct ShotAudioPlan {
    std::string shot_id;
    double speech_duration_s = 0.0;
    double locked_duration_s = 0.0;
    double slack_s = 0.0;
    int lines = 0;

    /// 留白小于半秒。**这类镜头在装配时不能再压缩。**
    bool is_tight() const { return slack_s < 0.5; }
};

/// 这一段送给 TTS 会不会念出东西来。
///
/// 全是标点 / 括号 / 空白就是假。`split_long_text` 拿它决定"这一截要不要
/// 并回前一段"——一段纯标点进合成器，出来多长完全不可预期（实测一个「！」
/// 出过 41 秒，而那一镜只有 5 秒，声音盖住后面好几镜）。
///
/// **导出来是为了测得到。** 它原来在 audio_plan.cpp 的匿名命名空间里，
/// 于是 test_audio_plan 里那条「拆句不能留下只有标点的碎片」只好自己抄了
/// 一张标点表——而抄的那张比这儿**少**了 `－〈〉‥﹏` 和全角空格，也就是
/// 说用例比它守的那条规则还松：真漏出一个只有「〈〉」的碎片，用例判它
/// "能念"，一路绿灯。判据只该有一份。
bool has_speakable(const std::string& text);

/// 配音结果概览。
std::string summarize(const std::vector<ShotAudioPlan>& plans);

}  // namespace changji::stages
