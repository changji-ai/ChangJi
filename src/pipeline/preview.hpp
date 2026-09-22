#pragma once

// 只做这一章的前 n 分钟。
//
// **这是什么**：挑出从第一镜起、累计够 n 分钟的那几镜，只把它们做出来，
// 装成一条单独的片子（`output/preview/<章号>.mp4`）。人要的是"先花二十
// 分钟看看这部片子长什么样"，而整章是一个钟头起。
//
// ---
//
// **为什么不是"做一镜、量一下、不够再做下一镜"。**
//
// 配音跑完之后，每一镜在时间轴上占多久是**确定的**——
// `VideoLimits::real_duration_s` 是个纯函数（帧数对齐到模型的格子，再除
// 以 fps），装配那头排时间轴用的就是它，而转场只是记下来的意图、不吃时间
// （见 media/assemble.cpp 里那段）。所以"做一个量一下"没有任何新消息：
// 挑中的镜头和一镜一镜试出来的**完全一样**，而单卡机上一镜一镜做要在
// 图像模型和视频模型之间切 2N 次（episode.hpp 开头那段：按阶段分批是
// 加载 2 次，按镜头串行是 80 次）。
//
// 真正不确定的只有一件事：**这一镜会不会被闸门退回、渲染失败**——那种
// 镜头进不了装配（`assembly_usable`），于是"计划够 2 分钟"可能装出来只有
// 1 分 40 秒。这一条两种跑法都得等它跑完才知道，所以放在**一轮跑完之后**
// 判：装出来不够就把用不了的那几镜跳过去、往后再取几镜，再跑一轮。
// 判据落在**装出来的长度**上，不落在计划长度上。
//
// ---
//
// **配音要跑在挑之前，而且要反复挑。** 配音会把时长改短（一句两秒的台词
// 能把计划 5 秒的镜头锁成 3 秒，见 stages::AudioStage::lock_duration），
// 也会把装不下台词的镜头**拆成两镜**（`split_overlong_shots` 会凭空多出
// 一个 `sh003_b`）。所以前缀不能在开跑前用一份 shot_ids 冻死：冻死的话
// 拆出来的新镜不在名单里，首帧和成片会整个跳过它——成片里那一段有画面
// 没声音，而且一句话都不会说。

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/episode.hpp"
#include "pipeline/jobs.hpp"
#include "stages/limits.hpp"

namespace changji::pipeline {

/// 前 n 分钟是哪几镜。
struct PreviewPick {
    /// 按 `order` 排的前缀。
    std::vector<std::string> shot_ids;
    /// 这几镜按格子算一共多长。
    double planned_s = 0.0;
    /// 够不够 n 分钟。
    bool enough = false;
    /// 整章都算上还是不够——这时 shot_ids 是**全章**。
    bool whole_episode = false;

    std::set<std::string> id_set() const {
        return {shot_ids.begin(), shot_ids.end()};
    }
};

/// 挑前缀：按 `order` 走，累计到 **≥** `target_s` 为止（跨过那条线的
/// 那一镜也要，所以结果通常比 n 稍长——用户要的是"不少于 n 分钟"）。
///
/// **累计用 `limits.real_duration_s(duration_s, fps)`，不是把 `duration_s`
/// 加起来。** 后者是名义值：模型只能按格子出帧（Wan 4n+1、MiniMax-H3
/// 17k+5），名义 4 秒出来是 4.458 秒。按名义值挑的话，每一镜差的那几百
/// 毫秒逐镜累积，二十镜之后能差出一整镜来。
///
/// **`limits` 要传对那部电影的那一份**（单镜上限是 `[video].max_shot_s`，
/// 一部电影一个值）。只读接口走 `http/readonly.cpp` 的 `limits_for_project`，
/// 跑的那头走 `stages::video_limits()`（出片前 `config::apply_video_limits`
/// 刚按这个项目设过）。两处传的不是同一份的话，按钮上写「前 7 镜」而引擎
/// 做了 8 镜。
///
/// **已经出过片的镜头照样计入长度。** 它们已经在成片里占着那段时间——
/// 不计的话，第二次点「只做前 2 分钟」会一路往后延，每点一次多做两分钟。
///
/// `counts` 给了就只有它说真的镜头才计入长度（**位置照旧占着**，只是不
/// 算时间）。用来跳过"跑过一轮还是进不了成片"的那几镜：它们在片子里是个
/// 空档，得往后再取几镜补上。不给 = 每一镜都算。
///
/// `target_s <= 0` 回一个空的前缀（`enough = false`）——这个函数只在预告
/// 那条路上调，调用方保证给的是正数。
PreviewPick pick_preview_prefix(
    const models::Episode& ep, double target_s,
    const stages::VideoLimits& limits, int fps,
    const std::function<bool(const models::Shot&)>& counts = {});

/// 跑一章的前 n 分钟。
///
/// `opts.preview_s` 是那个 n（秒）。这一条**自己调 `run_episode`**，一轮
/// 一轮地跑，每轮之间重挑前缀：
///
///   1. 配音：挑前缀 → 给里面还没配音的配 → 重挑（时长变了、可能拆了镜）
///      → 再挑到没有没配音的为止；
///   2. 首帧 + 成片：只跑前缀那几镜（多卡机上照旧走 ShotFlow 流水）；
///   3. 量**能进装配的**那几镜一共多长。够了就装配；不够就把这一轮跑完
///      还是用不了的记下来、往后再取几镜，回到第 1 步。最多再跑两轮，
///      之后照实说装出来多长、谁没进去——**不无限循环**。
///   4. 装成 `output/preview/<章号>.mp4`，字幕落在 `subtitles/preview/` 下。
///
/// 装配走的是**预告自己那条**：不碰这一章的正片、不清这一章的孤儿成片、
/// 配乐也用自己的文件名（配乐"文件在就沿用"，预告先出一条两分钟的，
/// 整章再跑就会沿用它，后面几分钟静悄悄没有配乐）。
RunReport run_preview(const models::ProjectStore& store,
                      const models::HardwareProfile& profile,
                      const config::Settings& settings, const RunOptions& opts,
                      const Backends& backends, JobProgress& progress,
                      CancelToken& tok);

/// 预告片落在哪：`output/preview/<章号>.mp4` 里的那个 `preview/<章号>.mp4`。
///
/// **不能叫 `<章号>.mp4` 摆在 `output/` 顶层。** 那儿是正片的地盘，而
/// `media::episode_of_output` 认名字——预告一摆进去就会被 `film_join` 当成
/// "这一章出片了"，半章被接进整部电影；项目库那句「N 章已出片」也跟着错。
/// 放进子目录，`get_outputs`（不递归）和那两处判法都看不见它，页面上单独
/// 摆出来、标清是前多少秒。
std::string preview_output_name(const std::string& episode_id);

}  // namespace changji::pipeline
