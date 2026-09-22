#pragma once

// 成片：把出了片的章按章序接成**一部完整的电影**，一个文件。
//
// 2026-09-17 这儿曾经是「接成一条，再按用户选的每集时长在镜头边界切成几集」。
// 2026-09-18 用户把产品定位改成电影制作平台：成片就是一部电影、一个文件，
// 整条切段链（media::split_into_episodes、plan_series_cut、CutPart、
// POST /api/film/cut 的 per_episode_s）连根拔掉。
//
// **没片的章跳过，不是停**（用户 2026-09-18：「这一章有片就可以成片了」）。
// 跳过哪几章记在清单里，页面上要说清。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"

namespace changji::pipeline {

/// 成片目录：output/final。每次合成都清空重写。
std::filesystem::path final_dir(const models::ProjectPaths& paths);

/// 人按停之后留下的那句话。**引擎这头和接口那头必须是同一句**：
/// `join_film` 开跑前查一次取消、抛的是它；`POST /api/film/join` 交给
/// `JobTable::start` 当作业停止文案的也是它。逐字抄两份的话，改了一处另一处
/// 静悄悄不跟，而人看见的是哪一句全看停在哪一拍。
///
/// 这一句**不归 util/cancel_words.hpp 管**：那儿收的是界面拿
/// `/已停下|已取消/` 认的那两句（认出来才不弹成红报错）；作业级的停界面靠
/// 自己按下去时立的那面旗子分辨，文案和 `pipeline/jobs.hpp` 的
/// `kRunStoppedMessage` 同族——话里说的是「这一趟停了，留下的是什么」。
inline constexpr const char* kFilmJoinStoppedMessage =
    SAY_NOOP("已手动停止。这一版没合成完。");

/// 还不能合成的话，为什么；空串 = 能。**有一章出了片就能合成。**
std::string film_join_blocker(const models::ProjectStore& store);

/// 这一次合成出来的是什么。GET /api/film 直接照它答。
struct FilmJoinReport {
    /// output/final/成片.mp4
    std::filesystem::path path;
    /// 成片真实时长，ffprobe 量出来的（不是各章时间线加出来的）。
    ///
    /// **量不到就是空的，不是 0。** 0 是"量出来正好是 0 秒"，而这儿要表达
    /// 的是"没量到"——ffprobe 不在配置的那个路径上（`ff.probe` 抛）、容器里
    /// 根本没写 duration（`media::to_double` 照文档回 0，不抛）都会走到这
    /// 一支，而成片好好地躺在 output/final 里。这两个意思一旦挤进同一个
    /// `double`，0 就会顺着 film.json 一路流到成片页上，写成「成片 · 0 秒」。
    std::optional<double> total_s;
    /// 接进去的章、还没出片被跳过的章（都是 chapter_id）。
    std::vector<std::string> chapters;
    std::vector<std::string> skipped;
    /// 接进去多少镜。给人看的数。
    int shots = 0;
};

/// 真干：按章序把各章的成片拼成 output/final/成片.mp4，顺手写一份 film.json。
/// 一章都没出片就抛（消息同 film_join_blocker）。
FilmJoinReport join_film(const models::ProjectStore& store,
                         const config::Settings& settings,
                         const media::FFmpeg& ff, JobProgress& progress);

}  // namespace changji::pipeline
