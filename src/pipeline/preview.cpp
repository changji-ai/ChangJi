#include "pipeline/preview.hpp"

#include <algorithm>
#include <set>
#include <string>

#include "util/say.hpp"
#include "util/human_time.hpp"

namespace changji::pipeline {

using models::Episode;
using models::Project;
using models::ProjectStore;
using models::Shot;
using models::ShotStatus;

namespace {

/// 一轮跑完还是进不了成片时，最多再来几轮。
///
/// **不无限循环。** 一直进不去的多半是这几镜本身有问题（闸门每次都判硬切、
/// 参考图不对），再跑十轮也一样，而每一轮是十几分钟。跑到头就照实说装出来
/// 多长、谁没进去，让人自己决定。
constexpr int kMaxExtraRounds = 2;

void emit(JobProgress& p, const char* kind, const std::string& message) {
    Event e;
    e.stage = "preview";
    e.kind = kind;
    e.message = message;
    p.report(e);
}

std::string join_ids(const std::vector<std::string>& ids, std::size_t keep) {
    std::string out;
    for (std::size_t i = 0; i < ids.size() && i < keep; ++i) {
        out += (i ? SAY("、") : std::string{}) + ids[i];
    }
    if (ids.size() > keep) out += "…";
    return out;
}

}  // namespace

std::string preview_output_name(const std::string& episode_id) {
    return "preview/" + episode_id + ".mp4";
}

PreviewPick pick_preview_prefix(
    const Episode& ep, double target_s, const stages::VideoLimits& limits,
    int fps, const std::function<bool(const Shot&)>& counts) {
    PreviewPick out;
    if (!(target_s > 0.0)) return out;

    const std::vector<Shot> ordered = ep.sorted_shots();
    for (const Shot& s : ordered) {
        out.shot_ids.push_back(s.shot_id);
        // 不算数的镜头**位置照旧占着**：它在片子里是个空档，后面那几镜
        // 还是得跑，只是这一镜的时间不能算进"够不够 n 分钟"。
        if (!counts || counts(s)) {
            out.planned_s += limits.real_duration_s(s.duration_s, fps);
        }
        if (out.planned_s >= target_s) {
            out.enough = true;
            break;
        }
    }
    out.whole_episode = out.shot_ids.size() == ordered.size();
    return out;
}

RunReport run_preview(const ProjectStore& store,
                      const models::HardwareProfile& profile,
                      const config::Settings& settings, const RunOptions& opts,
                      const Backends& backends, JobProgress& progress,
                      CancelToken& tok) {
    RunReport report;
    report.episode_id = opts.episode_id;

    const double target = opts.preview_s;
    const int fps = settings.assembly.fps;
    // **这一份格子是刚按这个项目设过的**（出片进门先调 apply_video_limits，
    // 见 http/run.cpp 的任务体）。按钮那头拿的是同一个项目的那一份，两边
    // 才会挑出同一批镜头。
    const stages::VideoLimits& limits = stages::video_limits();

    // 跑过一轮还是进不了成片的。下一轮挑前缀时它们不算时间——片子里那一段
    // 是空的，得往后再取几镜补上。
    std::set<std::string> dead;
    const auto counts = [&dead](const Shot& s) {
        return dead.count(s.shot_id) == 0;
    };

    // 这一趟最后装哪几镜。
    std::set<std::string> assembled_from;

    for (int round = 0; round <= kMaxExtraRounds; ++round) {
        if (tok.cancelled()) break;

        // ---- 1. 配音：挑 → 配 → 重挑 ----
        //
        // 配音会把时长改短、还会拆镜，所以挑一次不算数。每转一圈至少有一镜
        // 新配上音（没有就是前缀里都配过了，退出），所以一定收敛。
        PreviewPick pick;
        // 这一圈已经送去配过音的。⚠️ **上面那句「一定收敛」只在每镜都配得上时成立**：
        // 配音砸了的那一镜（台词过不了模型、参考音色不在、显存不够）照旧是 PLANNED
        //（AudioStage 只记一句警告），下一圈又挑到它——没有这一道就是死循环，
        // 出片那个槽一直占着，整条队列等到有人按停。一圈里没有新的可试了就往下走：
        // 那几镜没配上音，和整章出片时配音砸了是一个下场。
        std::set<std::string> tried;
        for (;;) {
            if (tok.cancelled()) break;
            Project project = store.load_project();
            Episode* ep = project.episode_by_id(opts.episode_id);
            if (ep == nullptr) {
                throw std::runtime_error(
                    SAYF("项目里没有这一章 %1", opts.episode_id));
            }
            pick = pick_preview_prefix(*ep, target, limits, fps, counts);
            if (pick.shot_ids.empty()) {
                throw std::runtime_error(
                    SAYF("这一章 %1 还没有分镜表", opts.episode_id));
            }
            // 前缀里还没配音的。**和配音那一段用的是同一个 pick**
            //（episode.cpp 的 `pick(*ep, {PLANNED}, …)`），不另写一份判据。
            const auto todo =
                ::changji::pipeline::pick(*ep, {ShotStatus::PLANNED},
                                          /*force=*/false, pick.id_set());
            bool fresh = false;
            for (const auto* s : todo) {
                if (tried.insert(s->shot_id).second) fresh = true;
            }
            if (!fresh) break;

            RunOptions one = opts;
            one.only = std::vector<Stage>{Stage::Audio};
            one.only_shots = pick.id_set();
            one.force = false;   // 配音只补没配的，不重配
            const RunReport r = run_episode(store, profile, settings, one,
                                            backends, progress, tok);
            report.audio.insert(report.audio.end(), r.audio.begin(),
                                r.audio.end());
            for (const auto& e : r.errors) report.errors.push_back(e);
        }
        if (tok.cancelled()) break;

        // ---- 2. 前缀定下来了，说清楚这一轮要做哪几镜 ----
        {
            std::string msg =
                SAYN("按前 %1 做：从第一镜起 %n 镜（%2），算下来 %3",
                     static_cast<long long>(pick.shot_ids.size()),
                     util::human_time(target),
                     join_ids(pick.shot_ids, 5),
                     util::human_time_precise(pick.planned_s));
            if (!pick.enough) {
                msg += SAYF("。整章一共就这么长，不够 %1，整章都做了",
                            util::human_time(target));
            }
            emit(progress, round == 0 ? "start" : "progress", msg);
        }

        // ---- 3. 首帧 + 成片，只跑这几镜 ----
        //
        // **草稿档要列进来，哪怕多数时候它被跳过。** 列不列和跑不跑是两件
        // 事：`skip_draft` 决定跑不跑，而 `only` 决定"这一轮允许哪几个阶段"。
        // 不列的话，`skip_draft = false` 的调用方那里草稿档整个消失，而成片
        // 档收的是 `DRAFT_DONE`——于是**一镜都挑不到**，跑完一轮什么都没出，
        // 判据看到"一镜都不能用"，再跑一轮还是一样，三轮之后做了一整章的
        // 首帧、零个视频。（第一版就是这么写的，用例当场抓到。）
        {
            RunOptions one = opts;
            one.only = std::vector<Stage>{Stage::Frames, Stage::Draft,
                                          Stage::Final};
            one.only_shots = pick.id_set();
            const RunReport r = run_episode(store, profile, settings, one,
                                            backends, progress, tok);
            report.frames.insert(report.frames.end(), r.frames.begin(),
                                 r.frames.end());
            report.final_.insert(report.final_.end(), r.final_.begin(),
                                 r.final_.end());
            for (const auto& e : r.errors) report.errors.push_back(e);
        }
        if (tok.cancelled()) break;

        // ---- 4. 装出来够不够 ----
        //
        // **判据落在能进装配的那几镜上，不落在计划长度上。** 闸门退回、
        // 渲染失败的镜头进不了成片（assembly_usable），计划够两分钟的前缀
        // 可能只装出一分四十。
        Project project = store.load_project();
        Episode* ep = project.episode_by_id(opts.episode_id);
        if (ep == nullptr) break;
        const std::set<std::string> in_pick = pick.id_set();
        double usable_s = 0.0;
        std::vector<std::string> missing;
        for (const Shot& s : ep->sorted_shots()) {
            if (in_pick.count(s.shot_id) == 0) continue;
            if (assembly_usable(s)) {
                usable_s += limits.real_duration_s(s.duration_s, fps);
            } else {
                missing.push_back(s.shot_id);
            }
        }
        assembled_from = in_pick;

        if (usable_s >= target || pick.whole_episode || missing.empty()) break;
        if (round == kMaxExtraRounds) break;

        // 这一轮跑完还是用不了的，下一轮不算它们的时间——往后再取几镜补上。
        for (const auto& id : missing) dead.insert(id);
        emit(progress, "warn",
             SAYF("装得出来的只有 %1，不到 %2：%3 出不了片（闸门退回或者渲染"
                  "失败）。往后再取几镜补上。",
                  util::human_time_precise_as(usable_s,
                                              std::min(usable_s, target)),
                  util::human_time_precise_as(target,
                                              std::min(usable_s, target)),
                  join_ids(missing, 5)));
    }

    if (tok.cancelled()) return report;

    // ---- 5. 装成预告 ----
    if (!backends.ffmpeg.has_value()) {
        emit(progress, "warn",
             SAY("没有 ffmpeg，装不出预告。各镜的视频已经在 shots/ 下了"));
        return report;
    }
    Project project = store.load_project();
    Episode* ep = project.episode_by_id(opts.episode_id);
    if (ep == nullptr) {
        report.errors.push_back(SAY("这一章在跑的过程中被删掉了"));
        return report;
    }
    // **一镜都没定下来就别装。** `only_shots` 空的含义是"这一章所有能用的"
    // ——那正好是这条路最不该做的事：半路出岔时把整章装成一条叫「预告」的
    // 片子，而且一声不响。
    if (assembled_from.empty()) {
        report.errors.push_back(SAY("没挑出要做的镜头，预告没装"));
        return report;
    }
    AssembleOptions aopts;
    aopts.only_shots = assembled_from;
    aopts.out_name = preview_output_name(opts.episode_id);
    // 配乐和字幕都得有自己的落点，理由见 AssembleOptions 上那两段。
    aopts.music_suffix = "_preview";
    aopts.sweep_orphans = false;
    double total = 0.0;
    aopts.total_s = &total;
    try {
        report.output =
            assemble_episode(store, settings, *ep, *backends.ffmpeg, progress,
                             aopts);
        const double ref = std::min(total, target);
        std::string msg =
            SAYN("前 %1 的片子做好了：%2，%n 镜。",
                 static_cast<long long>(aopts.only_shots.size()),
                 util::human_time(target),
                 util::human_time_precise_as(total, ref));
        if (total < target) {
            msg += SAYF("没到 %1——上面写着是哪几镜没进去。",
                        util::human_time_precise_as(target, ref));
        }
        // ⚠️ **整章都在这一段预览里的时候，这句话一个字都不该说。**
        // 原来是无条件加的，于是那种情况下屏幕上写着「整章还有 0 镜没做」
        // ——0 那个数要人自己反应一下才读懂"等于没有"，而这儿更糟：
        // 它还接着劝人再按一次「出片」，而根本没有可补的。
        if (const std::size_t left = ep->shots.size() - aopts.only_shots.size();
            left > 0) {
            msg += SAYN("整章还有 %n 镜没做，接着按「出片」就是把这一章补完，"
                        "已经做好的不会重跑。",
                        static_cast<long long>(left));
        }
        emit(progress, "done", msg);
    } catch (const std::exception& e) {
        report.errors.push_back(e.what());
        emit(progress, "error", e.what());
    }
    return report;
}

}  // namespace changji::pipeline
