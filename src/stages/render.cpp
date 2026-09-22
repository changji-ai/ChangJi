#include "infer/worker_pool.hpp"
#include "stages/render.hpp"
#include "stages/storyboard.hpp"  // defuse_motion

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include "gates/checks.hpp"
#include "util/human_time.hpp"
#include "pipeline/task_board.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"
#include "infer/scheduler.hpp"

namespace fs = std::filesystem;

namespace changji::stages {
namespace {
/// 同 frames.cpp：把腾显存的结论包成"（……）"挂在进度文案后面。
/// 两处各写一份是因为它们各自在匿名命名空间里，抽到公共头去只为这三行
/// 不划算；措辞那一份在 Scheduler::room_note，不会分叉。
std::string room_note_suffix(infer::Slot slot) {
    const std::string note = infer::scheduler().room_note(slot);
    // 括号也跟着语言换：西文里是半角的，中间还要一个空格。
    return note.empty() ? std::string{} : SAYF("（%1）", note);
}
}  // namespace

using namespace changji::models;

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/// 把闸门给的几条理由拼成一句。分隔符照抄 gates 那边的全角分号——
/// 这句会原样进 gate_notes，人在界面上读的就是它。
std::string join_reasons(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += SAY("；");
        out += v[i];
    }
    return out;
}

}  // namespace

int frames_for(double duration_s, int fps) {
    // 格子和上限都来自配置（Wan 是 4n+1 / 121，MiniMax-H3 是 17k+5 / 360）。
    // 写死过一版 4n+1，换模型之后就一直在给 sd.cpp 递不在格子上的数——
    // 它自己向上对齐，不报错，表现是成片比分镜表长一点点。
    return video_limits().frames_for(duration_s, fps);
}

std::int64_t render_seed(const std::string& shot_id, int attempts) {
    // 注意这里**没有前缀**，对齐 Python 的 hash(shot.shot_id)。
    // 首帧那个是 hash("frame:" + shot_id)——两者不同正是为了让
    // 同一个镜头的首帧和视频落在不同的种子上。
    const std::string hex = text::sha1_hex(shot_id);
    const std::uint32_t base =
        static_cast<std::uint32_t>(std::stoul(hex.substr(0, 8), nullptr, 16));
    const std::int64_t b = static_cast<std::int64_t>(base % 2147483648u);
    return (b + static_cast<std::int64_t>(attempts) * 7919) % 2147483648LL;
}

RenderPlan make_plan(const Shot& shot, const TierSpec& spec,
                     const PromptComposer& composer,
                     const std::string& aspect_ratio, int fps) {
    RenderPlan p;
    p.shot_id = shot.shot_id;
    p.tier = spec.tier;
    p.spec = spec.scaled_to(aspect_ratio);
    p.frames = frames_for(shot.duration_s, fps);
    p.prompts = composer.compose(shot);
    p.motion = composer.motion_prompt(shot);
    p.style_line = composer.style_line();
    return p;
}

bool is_hero_shot(const Shot& shot, bool first, bool last) {
    if (first || last) return true;
    // beat 现在是枚举（钩子 / 高潮 / 反转…），老表里可能是自由文本，两种都认。
    //
    // ⚠️ **这张表是解析器，不是界面，一个字都不许翻。** 它比的是存在项目
    // 里的 `shot.beat`，那些字是中文写进磁盘的；翻了之后一条都匹配不上，
    // 而表现是"关键镜头不再多出一条"，没有任何报错。同 `models/character.cpp`
    // 里的性别词表。
    for (const char* w : {SAY_NEVER("钩"), SAY_NEVER("扣"),
                          SAY_NEVER("反转"), SAY_NEVER("高潮"),
                          SAY_NEVER("揭"), SAY_NEVER("真相")}) {
        if (shot.beat.find(w) != std::string::npos) return true;
    }
    return false;
}

bool take_good_enough(const gates::GateResult& r) {
    if (!r.ok()) return false;
    if (r.metrics.count("cut_inside") != 0) return false;
    const auto it = r.metrics.find("motion_mean");
    if (it == r.metrics.end()) return true;   // 闸门没量运动：过了就算干净
    return it->second >= 0.8 && it->second <= 15.0;
}

std::size_t pick_take(const std::vector<gates::GateResult>& results) {
    const auto score = [](const gates::GateResult& r) {
        double s = r.ok() ? 100.0 : 0.0;
        if (r.metrics.count("cut_inside") != 0) s -= 60.0;
        const auto it = r.metrics.find("motion_mean");
        if (it != r.metrics.end()) {
            const double v = it->second;
            if (v < 0.8) {
                s -= 15.0;                       // 几乎不动
            } else if (v <= 15.0) {
                s += 20.0 + std::min(v, 10.0);   // 像回事
            } else {
                s += 5.0;                        // 太猛，多半是乱动
            }
        }
        return s;
    };
    std::size_t best = 0;
    double best_score = -1e9;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const double s = score(results[i]);
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }
    return best;
}

std::string video_positive(const RenderPlan& plan) {
    // **提示词里的分隔符，不是界面文案。** 换成别的语言的逗号，出图
    // 模型读到的就是另一段提示词。
    const std::string sep =
        plan.style_line == StyleLine::ANIME ? ", " : SAY_NEVER("，");
    std::vector<std::string> parts;
    for (const std::string* p : {&plan.prompts.video_scene, &plan.motion,
                                 &plan.prompts.style_layer}) {
        if (!text::strip_ws(*p).empty()) parts.push_back(*p);
    }
    // 老计划（工作进程那头版本旧、没有 video_scene）退回整段 positive。
    if (plan.prompts.video_scene.empty() && plan.prompts.style_layer.empty()) {
        if (plan.motion.empty()) return plan.prompts.positive;
        if (plan.prompts.positive.empty()) return plan.motion;
        return plan.prompts.positive + sep + plan.motion;
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::vector<RenderOutcome> render_batch(std::vector<Shot*>& shots,
                                        const AssetLibrary& assets,
                                        const TierSpec& spec,
                                        const ProjectPaths& paths,
                                        const VideoRenderer& render,
                                        pipeline::JobProgress& progress,
                                        pipeline::CancelToken& tok, int fps,
                                        int concurrency, const GateHooks& gate,
                                        const pipeline::ShotCommit& commit,
                                        const RenderExtras& extras) {
    const PromptComposer composer(assets);
    const std::string stage_name =
        spec.tier == Tier::FINAL ? "final" : "draft";

    const int total = static_cast<int>(shots.size());

    /// 一镜跑完之后的东西。**只装数据，不碰调用方的 Shot。**
    ///
    /// `shot` 是那份本地副本，闸门循环改的就是它。收的时候整份写回去——
    /// 一次赋值，比逐个字段抄少一类"新加了字段忘了抄"的错。
    struct Done {
        bool ok = false;
        std::string error;
        std::string rel_path;
        double elapsed_s = 0.0;
        bool skipped = false;  ///< 取消了，没跑
        /// 这一格的副本装过东西没有。**写回的唯一凭据。**
        ///
        /// ⚠️ 2026-09-17 丢过一章的数据：收尾那一段原来只看 `skipped`，
        /// 而这个数组是按镜头数**默认构造**出来的——一格没跑过、又没被
        /// 标成 skipped 的话，`*shot = std::move(done[i].shot)` 写回去的
        /// 是一个**空壳 Shot**（shot_id 是空串、没有台词、status 回到
        /// planned）。当天就是这么把 ep07 的 17 镜整章抹平的：流水那道
        /// 排位闸（wait_slack）在取消时让每一路直接 return，一格都没标
        /// skipped，于是 17 格全被空壳盖掉，最后那次 save 落了盘。
        ///
        /// 光修那条 return 不够——**这一类错误的代价太大**，只要将来某条
        /// 新路径再漏标一次，同样的事会再来一遍。所以判据换成正面的：
        /// 装过东西才写回，而不是"没被标成跳过就写回"。
        bool ran = false;
        Shot shot;
        bool committed = false;   ///< 已经写回去了，收尾时别再写一遍
    };
    std::vector<Done> done(shots.size());
    std::mutex commit_mu;
    // 流水时锁用两层共用的那把，理由见 frames.cpp 同一处
    std::mutex& commit_lock =
        extras.flow ? extras.flow->commit_mutex() : commit_mu;

    // 开跑前的预计来自一张按显存推的静态表，实测能差一倍。
    // 跑起来之后用真实耗时重算，等的人才知道还要等多久。
    const double stage_started = now_seconds();

    // 同时跑几镜。**上限是镜头数**——池里八个而只有三镜时，
    // 起八个线程只是白占。
    const int lanes = std::max(1, std::min(concurrency, total));

    // 每一镜在任务账本上占一行，**开工之前就全部登记**。理由和 frames.cpp
    // 那处一样：任务页面那一栏问的是"还排着哪几镜"。
    // `stage_name` 是 draft / final，标题里写清楚是哪一档。
    const std::string project_path = paths::to_utf8(paths.root());
    const std::string tier_cn =
        stage_name == "draft" ? SAY("草稿档") : SAY("成片档");
    std::vector<std::unique_ptr<pipeline::Task>> tasks;
    tasks.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        tasks.push_back(std::make_unique<pipeline::Task>(
            "video", SAYF("出片%1 · %2", tier_cn, shots[i]->shot_id),
            project_path));
        tasks.back()->token().link(&tok);
    }

    std::atomic<int> next{0};
    std::atomic<int> finished{0};

    // 「排队中」那份名单不在这儿管：它归登记它的那个阶段管
    //（JobState::pending_stage），首帧那层报的完成划不掉出片这一份。
    const auto lane = [&] {
        // 流水：首帧那层还有下一张可派时，这几路一个位置都不抢
        //（在条件变量上等，不占池）。见 pipeline/shot_flow.hpp。
        //
        // **等不到就往下走，不要 return。** 直接 return 的话这几路一格都
        // 不领，`done` 里每一格都停在默认值上——收尾那一段照着走就把整章
        // 写成空壳（见 Done::ran 上那段）。走下面那个循环，每一格老老实实
        // 标成 skipped。
        const bool slack = !extras.flow || extras.flow->wait_slack(tok);
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= total) return;
            if (!slack || tok.cancelled()) {
                done[i].skipped = true;
                continue;
            }

            Shot* shot = shots[i];
            const int index = i + 1;
            // 流水：这一镜的首帧还没写回就等。等的是 ShotFlow 的条件变量，
            // 不是工作进程——池里的位置留给正在出首帧的那几路。
            if (extras.flow && !extras.flow->wait_ready(shot->shot_id, tok)) {
                done[i].skipped = true;
                continue;
            }
            const double started = now_seconds();

            // **本地副本。** 重试要改 attempts，而 attempts 进种子——
            // 不换种子的重试就是把同一张牌再打一遍。改副本是为了让
            // 并行那一段仍然一个字节都不往 shots 里写。
            pipeline::Task& task = *tasks[i];
            if (task.cancelled()) { done[i].skipped = true; continue; }
            // **领到了不等于轮到了**：这几路比真正的位置多开一路（见
            // infer::pool_lanes），多出来那一路会先把下一镜领走、再在池子上
            // 等。报进度那一刻它自己变成"在画"，见 Task::begin。
            task.begin(/*working=*/false);

            Shot local = *shot;
            done[i].ran = true;   // 副本装上了，收尾那一段才敢写回

            {
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "progress";
                e.current = index;
                e.total = total;
                e.shot_id = local.shot_id;
                e.message = SAYF("出视频 %1", local.shot_id);
                progress.report(e);
            }

            // `shot_step` / `shot_steps` 是**这一镜自己**的进度，
            // 和 current/total（整章第几镜）是两回事。镜头墙上每张牌
            // 画的是前者——拿后者画的话，正在跑的那一镜从头到尾都显示
            // 21/22 那个百分比。默认 0，只有采样回调那条会填。
            const auto say = [&](const char* kind, const std::string& msg,
                                 int shot_step = 0, int shot_steps = 0,
                                 infer::Phase phase = infer::Phase::Sample) {
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = kind;
                e.current = index;
                e.total = total;
                e.shot_id = local.shot_id;
                e.message = msg;
                e.shot_step = shot_step;
                e.shot_steps = shot_steps;
                // 只有带步数、或者明说在准备／在等机器的那条才标阶段；
                // 别的事件（shot_done、warn）没有阶段这回事。
                if (shot_steps > 0 || phase == infer::Phase::Prep ||
                    phase == infer::Phase::Wait) {
                    e.shot_phase = infer::phase_name(phase);
                }
                progress.report(e);
            };

            // 重试超限，用能用的东西顶上。**不是停下来**——
            // 无人值守时停下来等于整章废掉。
            const auto fallback = [&](const std::string& reason) {
                local.status = ShotStatus::FALLBACK;
                local.gate_notes = {reason};
                // Python 的 _fallback 发的这条**不带 current/total**——
                // 它不在循环那几条的行列里。照抄，别顺手"补全"。
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "warn";
                e.shot_id = local.shot_id;
                e.message = SAYF("%1 重试超限，降级处理：%2", local.shot_id,
                                 reason);
                progress.report(e);
            };

            const ShotStatus want_after = spec.tier == Tier::FINAL
                                              ? ShotStatus::FINAL_DONE
                                              : ShotStatus::DRAFT_DONE;

            for (;;) {
                if (tok.cancelled()) { done[i].skipped = true; break; }

                fs::path dest;
                RenderPlan plan;
                // 多条 take 时闸门已经在挑的时候过了一遍，结果留在这儿。
                std::optional<gates::GateResult> picked;
                try {
                    plan = make_plan(local, spec, composer,
                                     assets.style.aspect_ratio, fps);
                    plan.keep_ambient = extras.keep_ambient;
                    // **被夹短了要说出来。** 镜长从分镜到出片要过四道夹子
                    // （模型上限、显存、内核、项目 max_shot_s）再对齐到帧格，
                    // 原来一路没有一句话——成片比分镜短一截，界面上一个字
                    // 都不提（8f00579 只是让人能查，没在这儿拦）。
                    if (local.duration_s - plan.duration_s() > 0.5) {
                        char want[32], got[32];
                        std::snprintf(want, sizeof want, "%.1f", local.duration_s);
                        std::snprintf(got, sizeof got, "%.1f", plan.duration_s());
                        say("warn",
                            SAYF("%1 分镜排的是 %2 秒，实际只能出 %3 秒（被单镜"
                                 "上限夹住了，运动描述按实际时长截）",
                                 local.shot_id, want, got));
                    }
                    // **参考图还原成绝对路径。**
                    //
                    // 和 frames.cpp 里同名的那一段是同一件事，2026-09-13 在
                    // 首帧那条修过，出片这条漏了：`compose` 交出来的是相对
                    // 项目根的路径（refs/xxx.png），跨机派活时 ship_input 拿
                    // 它去读文件，读的是工作目录——报「读不了输入文件：
                    // refs/c_zeng_laoban_front.png」，而那个文件明明在项目里。
                    //
                    // **一直没露出来，是因为上一版分镜全是 ECU**，而大特写
                    // 整档不带参考图（见 PromptComposer::compose_with）。
                    // 景别修好、镜头重新带上参考图的当天，这条就断了。
                    // 消息里那个相对路径本身就是线索：这一层的报错一律该是
                    // 绝对路径，看见相对的就说明哪儿漏了还原。
                    for (std::string& r : plan.prompts.reference_images) {
                        r = paths::to_utf8(paths.abs(r));
                    }

                    // 首帧是这一镜的起点。没有的话退回纯文生视频——
                    // 那样跨镜头一致性会掉一大截，但总比整条流水线卡住强。
                    std::optional<fs::path> start;
                    if (local.frame_path.has_value() &&
                        !local.frame_path->empty()) {
                        const fs::path frame = paths.abs(*local.frame_path);
                        std::error_code ec;
                        if (fs::is_regular_file(frame, ec)) {
                            start = frame;
                        } else {
                            say("warn",
                                SAYF("%1 记着首帧但文件不在，这一镜退回纯文生"
                                     "视频",
                                     local.shot_id));
                        }
                    }

                    // 按档位分目录。草稿和成片混在一起的话，重跑成片时
                    // 分不清哪个 mp4 是哪一档的，而它们文件名只差一个后缀。
                    dest = paths.shots(stage_name) /
                           paths::from_utf8(local.shot_id + ".mp4");

                    // **并发时几镜同时报**，靠 Event 里的 shot_id 分得开；
                    // JobProgress::report 自己有锁。
                    const auto on_step = [&](int step, int steps, double,
                                             infer::Phase phase) {
                        // **对面开口了 = 开始为这一镜干活了。**
                        // 读权重那一分钟也算（见 Task::working_now）。
                        task.working_now();
                        // 任务页面那条进度条要的是这两个数。**只画采样那一
                        // 段**：读权重和解码不是一个量级（几百个张量 vs 几步），
                        // 画在同一条上会像"跑到头又倒回去了"。
                        if (phase == infer::Phase::Sample) {
                            task.set_progress(step, steps);
                        }
                        // 三种阶段的文案在 infer::phase_note 里，和 frames.cpp
                        // 共用。第一个采样步上把"卸没卸大模型"带出来，那是
                        // 用户点完出片最想知道的一件事，而它以前只在设置页上。
                        say("progress",
                            SAYF("出视频 %1", local.shot_id) +
                                infer::phase_note(phase, step, steps) +
                                (phase == infer::Phase::Sample && step == 1
                                     ? room_note_suffix(infer::Slot::Video)
                                     : std::string{}),
                            step, steps, phase);
                    };

                    // ---- 尾帧串镜 ----
                    //
                    // 标了紧接上一镜的，拿上一镜真出来的最后一帧当首帧，动作
                    // 才接得上（行业标准做法）。只在串行时做：并行时上一镜
                    // 可能还在跑。抽不到就用自己的首帧，说一声。
                    if (extras.chain_frames && local.continuous_with_prev &&
                        lanes == 1 && extras.last_frame) {
                        std::optional<fs::path> prev_video;
                        std::string prev_id;
                        if (extras.prev_video) {
                            prev_video = extras.prev_video(local);
                        } else if (i > 0 && done[i - 1].ok &&
                                   done[i - 1].shot.video_path.has_value()) {
                            prev_video = paths.abs(*done[i - 1].shot.video_path);
                            prev_id = done[i - 1].shot.shot_id;
                        }
                        std::error_code cec;
                        if (prev_video.has_value() &&
                            fs::is_regular_file(*prev_video, cec)) {
                            const fs::path chained =
                                paths.frames() /
                                paths::from_utf8(local.shot_id + "_chain.png");
                            if (extras.last_frame(*prev_video, chained)) {
                                start = chained;
                                say("info", SAYF("%1 接着上一镜的最后一帧起拍",
                                                 local.shot_id));
                            } else {
                                say("warn",
                                    SAYF("%1 标了紧接上一镜，但抽不到上一镜的"
                                         "最后一帧，用自己的首帧",
                                         local.shot_id));
                            }
                        } else {
                            // **上一镜根本还没出片。** 上面那句注释说的
                            // 「抽不到就用自己的首帧，说一声」，原来只在
                            // "有片但抽不出帧"那一支说了；这一支一声不吭。
                            //
                            // 撞得到：单独重出中间某一镜（`shot_ids` 只点了
                            // 它），而它上一镜还没跑过——`extras.prev_video`
                            // 按 order 找得到那一镜，但它没有 video_path，
                            // 回的是 nullopt。出来的片子接不上上一镜，而
                            // 屏幕上没有任何东西说为什么，人只会反复重出。
                            say("warn",
                                SAYF("%1 标了紧接上一镜，而上一镜还没出片——"
                                     "这一镜按自己的首帧起拍，动作接不上。先把"
                                     "上一镜跑出来再重出这一镜",
                                     local.shot_id));
                        }
                    }

                    // ---- 尾帧 ----
                    if (local.end_frame_path.has_value() &&
                        !local.end_frame_path->empty()) {
                        const fs::path ef = paths.abs(*local.end_frame_path);
                        std::error_code eec;
                        if (fs::is_regular_file(ef, eec)) plan.end_image = ef;
                    }

                    // ---- 关键镜头第一条不干净时多出几条挑 ----
                    //
                    // 每条换一个种子（attempts 拉开 100，别和重试的 +1 撞上），
                    // 各过一遍闸门，按 pick_take 留一条。要闸门在才有依据挑。
                    // **第一条就干净的话到此为止**（take_good_enough）：
                    // 无条件出两条是整章时间翻倍。
                    const int takes =
                        (gate.check &&
                         is_hero_shot(local, i == 0, i == total - 1))
                            ? std::max(1, extras.hero_takes)
                            : 1;
                    if (takes > 1) {
                        std::vector<gates::GateResult> results;
                        std::vector<fs::path> files;
                        for (int k = 0; k < takes; ++k) {
                            Shot take = local;
                            take.attempts = local.attempts + k * 100;
                            const fs::path d =
                                dest.parent_path() /
                                paths::from_utf8(local.shot_id + "_take" +
                                                 std::to_string(k + 1) + ".mp4");
                            // 第一条不说"1/2"——多半到它就完了；补出来的才说
                            say("progress",
                                SAYF("出视频 %1", local.shot_id) +
                                    (k == 0 ? std::string{}
                                            : SAYF("（补第 %1 条）",
                                                   std::to_string(k + 1))));
                            render(take, plan, start, d, task.token(), on_step);
                            files.push_back(d);
                            results.push_back(gate.check(take, d, plan));
                            if (take_good_enough(results.back())) break;
                        }
                        const std::size_t best = pick_take(results);
                        std::error_code rec;
                        fs::remove(dest, rec);
                        fs::rename(files[best], dest, rec);
                        if (rec) {
                            fs::copy_file(files[best], dest,
                                          fs::copy_options::overwrite_existing,
                                          rec);
                        }
                        for (const fs::path& f : files) fs::remove(f, rec);
                        picked = results[best];
                        // 只出了一条就没什么可说的，闸门那句自己会说
                        if (results.size() > 1) {
                            say("info",
                                SAYF("%1 出了 %2 条，留第 %3 条", local.shot_id,
                                     std::to_string(results.size()),
                                     std::to_string(best + 1)) +
                                    gates::motion_note(*picked));
                        }
                    } else {
                        render(local, plan, start, dest, task.token(), on_step);
                    }
                    local.video_path = paths.rel(dest);
                } catch (const std::exception& e) {
                    // **整池连不上不会到这儿**：池自己在队列里等机器回来
                    // （worker_pool.cpp run_task），卡片上是 Phase::Wait 那句。
                    // 到这儿的是真的渲染失败——换台机器也一样的那种。
                    //
                    // 一镜失败不拖垮后面几镜。跑一晚上，早上发现第三镜挂了
                    // 导致后面三十镜都没动，那这一晚上就白熬了。
                    local.attempts += 1;
                    done[i].error = e.what();
                    say("warn",
                        SAYF("%1 渲染失败：%2", local.shot_id, done[i].error));
                    if (local.attempts >= gate.max_attempts) {
                        fallback(SAYF("渲染连续失败：%1", e.what()));
                        break;
                    }
                    continue;
                }

                if (!gate.check) {
                    // 没配闸门。这是加这个参数之前的行为。
                    local.status = want_after;
                    done[i].ok = true;
                    done[i].rel_path = *local.video_path;
                    say("shot_done", SAYF("%1 完成", local.shot_id));
                    break;
                }

                const gates::GateResult res =
                    picked.has_value() ? *picked : gate.check(local, dest, plan);
                if (res.ok()) {
                    local.status = want_after;
                    local.gate_notes.clear();
                    done[i].ok = true;
                    done[i].rel_path = *local.video_path;
                    say("shot_done", SAYF("%1 通过闸门", local.shot_id) +
                                         gates::motion_note(res));
                    break;
                }

                const gates::Verdict verdict = gate.decide(res, local);
                local.gate_notes = res.reasons;
                // 没过的也把运动量带上：亮度跳变 / 纯色那几条常常和「中途
                // 硬切」「几乎不动」是同一件事，两个数放一起才看得出来。
                say("gate", res.describe() + gates::motion_note(res));

                if (verdict == gates::Verdict::Retry) {
                    local.attempts += 1;
                    continue;
                }
                if (verdict == gates::Verdict::Fallback) {
                    fallback(join_reasons(res.reasons));
                    break;
                }
                // 片中硬切：问题在首帧（和提示词对不上），退回到"等出首帧"
                // 那一档，attempts 加一让首帧换个种子。下一次跑首帧和出片
                // 就会把这一镜重做；这一轮装配会点名它缺了。
                if (res.metrics.count("cut_inside") != 0) {
                    // **重试之前先把会把主体带出画的动作摘掉。**
                    //
                    // 闸门自己那句话就写着「换种子重出视频没用」，而这儿
                    // 原来做的正是换种子：attempts 加一、退回重出首帧，
                    // 运动描述一个字不动。那句「走向门口」还在，下一轮
                    // 照样半路换成另一场戏（2026-09-16 实测 ep04_sh009）。
                    //
                    // 镜头少演一个动作，比整镜换成另一场戏强得多。摘不到
                    // 东西就照旧换种子，行为和以前一样。
                    const std::string defused =
                        defuse_motion(local.motion_prompt);
                    // 角色的 action 也要摘：它会被重新接回运动提示词，
                    // 只摘 motion_prompt 等于没摘，闸门会反复退回同一镜。
                    const bool actions_changed = defuse_actions(local.characters);
                    if (defused != local.motion_prompt || actions_changed) {
                        say("info",
                            SAYF("%1 的运动描述里有会把主体带出画的动作，已经"
                                 "摘掉再重出：%2",
                                 local.shot_id, defused));
                        local.motion_prompt = defused;
                    }
                    local.status = ShotStatus::AUDIO_DONE;
                    local.attempts += 1;
                    done[i].error = join_reasons(res.reasons);
                    break;
                }
                // Regress：重跑没用，标记后交给人。
                local.status = spec.tier == Tier::FINAL
                                   ? ShotStatus::FINAL_REJECTED
                                   : ShotStatus::DRAFT_REJECTED;
                done[i].error = join_reasons(res.reasons);
                break;
            }

            done[i].shot = std::move(local);

            done[i].elapsed_s = now_seconds() - started;
            if (!done[i].ok && !done[i].error.empty()) task.fail(done[i].error);
            // 边跑边结账，见 frames.cpp 同一处。
            tasks[i].reset();

            // **出完一镜就落一次盘。** 一章二十二镜、一镜两分钟，
            // 不落的话这一个小时里镜头墙上看到的还是开跑那一刻：
            // 没有能点开看的视频，而那正是用户要的。
            // 进程被杀掉时更糟——mp4 躺在磁盘上，project.json 一条没记。
            //
            // **拷贝，不是移动**：下面收尾那一段还要用 done[i].shot。
            if (commit) {
                std::lock_guard<std::mutex> lg(commit_lock);
                *shots[i] = done[i].shot;
                done[i].committed = true;
                commit();
            }

            // 剩余时间按**已经跑完的这几镜**推，不按静态表。
            // 失败的那几镜也算进去：它们也花了时间（而且往往花得更多，
            // 失败通常发生在跑完大半之后）。
            //
            // 分母用**跑完的个数**而不是序号：并发时序号先到的未必先跑完，
            // 而 `墙上时间 / 跑完个数` 恰好就是吞吐的倒数——几路都对。
            const int did = finished.fetch_add(1) + 1;
            const int left = total - did;
            if (left > 0) {
                const double per = (now_seconds() - stage_started) / did;
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "eta";
                e.current = did;
                e.total = total;
                e.message = SAYN("还剩 %n 个镜头，按目前速度约 %1", left,
                                 util::human_time(per * left));
                progress.report(e);
            }
        }
    };

    if (lanes == 1) {
        lane();  // 串行那条路一个线程都不起
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(lanes));
        for (int k = 0; k < lanes; ++k) pool.emplace_back(lane);
        for (auto& t : pool) t.join();
    }

    // ---- 收。**在调用线程上顺序改 Shot** ----
    //
    // 上面那一段一个字节都没往 Shot 里写。写回集中在这儿，
    // 单线程、按镜头原顺序——存盘的那份 project.json 因此仍然只有一个写者。
    std::vector<RenderOutcome> outcomes;
    for (int i = 0; i < total; ++i) {
        // **没跑过的一格都不碰。** `ran` 是正面判据，见它的注释。
        if (done[i].skipped || !done[i].ran) continue;
        Shot* shot = shots[i];
        RenderOutcome out;
        out.shot_id = shot->shot_id;
        out.elapsed_s = done[i].elapsed_s;
        // 整份写回。状态、attempts、gate_notes、video_path 都在副本里，
        // 闸门循环已经按 Python 的判定改好了——这儿只负责搬。
        // 逐镜落盘那条路已经搬过了，别再搬一次（那份已经被移走了）。
        if (!done[i].committed) *shot = std::move(done[i].shot);
        out.ok = done[i].ok;
        out.path = done[i].rel_path;
        out.error = done[i].error;
        outcomes.push_back(out);
    }
    return outcomes;
}

}  // namespace changji::stages
