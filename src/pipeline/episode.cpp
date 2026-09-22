#include "util/say.hpp"
#include "pipeline/episode.hpp"

#include "pipeline/shot_flow.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <thread>
#include <chrono>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <system_error>

#include "gates/checks.hpp"
#include "util/paths.hpp"
#include "media/assemble.hpp"
#include "stages/audio_plan.hpp"
#include "stages/storyboard.hpp"
#include "stages/music.hpp"
#include "stages/tts_backends.hpp"
#include "util/human_time.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::pipeline {

using namespace changji::models;

/// 挑出这一阶段要跑的镜头。
///
/// **按 order 排序**，不是按在数组里的顺序。分镜表被手工改过之后，
/// 数组顺序和 order 可能对不上，而画面的连贯性是按 order 来的。
///
/// 返回的是**指针**，因为各阶段要就地改状态。Python 那边
/// `episode.sorted_shots()` 返回的是同一批对象的引用，
/// 而 C++ 侧那个函数返回的是拷贝——照抄名字会让所有状态改动写进临时对象，
/// 存盘时一个字段都没变。
std::vector<Shot*> pick(Episode& ep, const std::set<ShotStatus>& want,
                        bool force,
                        const std::set<std::string>& only_shots) {
    std::vector<Shot*> all;
    all.reserve(ep.shots.size());
    for (auto& s : ep.shots) all.push_back(&s);
    // stable_sort：order 相同的镜头保持原有先后，对齐 Python 的 sorted()。
    std::stable_sort(all.begin(), all.end(),
                     [](const Shot* a, const Shot* b) { return a->order < b->order; });

    std::vector<Shot*> todo;
    for (Shot* s : all) {
        // 指定了镜头就只认这几个。**先筛这一层**：不筛的话
        // force 会把整章都拉进来，而用户点的是某一镜的"重新生成"。
        if (!only_shots.empty() && only_shots.count(s->shot_id) == 0) continue;
        if (force || want.count(s->status)) todo.push_back(s);
    }
    return todo;
}

namespace {

/// 这一镜手里有没有一张能用的首帧。
///
/// **连磁盘一起看。** 只看 `frame_path` 记没记的话，人把 frames/ 删掉
/// 之后引擎还以为有——出片那一段会拿一个不存在的路径去当起点，
/// 而那一支只会 warn 一句"记着首帧但文件不在"然后退回纯文生视频。
bool has_usable_frame(const Shot& s, const ProjectPaths& paths) {
    if (!s.frame_path.has_value() || s.frame_path->empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(paths.abs(*s.frame_path), ec);
}

/// 配音是不是已经落定了。落定了时长才锁死，帧数才定得下来。
///
/// PLANNED 不算：配音失败的镜头停在那里，带着**估的**时长，
/// 照它出首帧等于把错的时长焊进画面。
/// LOCKED 不算：人工确认过的不动。
bool audio_settled(ShotStatus st) {
    switch (st) {
        case ShotStatus::AUDIO_DONE:
        case ShotStatus::FRAME_DONE:
        case ShotStatus::DRAFT_DONE:
        case ShotStatus::DRAFT_REJECTED:
        case ShotStatus::FINAL_DONE:
        case ShotStatus::FINAL_REJECTED:
        case ShotStatus::FALLBACK:
            return true;
        case ShotStatus::PLANNED:
        case ShotStatus::LOCKED:
            return false;
    }
    return false;
}

}  // namespace

std::vector<Shot*> pick_for_frames(Episode& ep, const ProjectPaths& paths,
                                   bool force,
                                   const std::set<std::string>& only_shots) {
    std::vector<Shot*> all;
    all.reserve(ep.shots.size());
    for (auto& s : ep.shots) all.push_back(&s);
    std::stable_sort(all.begin(), all.end(),
                     [](const Shot* a, const Shot* b) { return a->order < b->order; });

    std::vector<Shot*> todo;
    for (Shot* s : all) {
        if (!only_shots.empty() && only_shots.count(s->shot_id) == 0) continue;
        if (force) { todo.push_back(s); continue; }
        // 正常流程的入口：配音刚跑完，这一轮就该出它的首帧。
        if (s->status == ShotStatus::AUDIO_DONE) { todo.push_back(s); continue; }
        // 补漏：配音早就落定了，可手里没有能用的首帧。见头文件里那一大段。
        if (audio_settled(s->status) && !has_usable_frame(*s, paths)) {
            todo.push_back(s);
        }
    }
    return todo;
}

models::TierSpec frame_spec(const models::HardwareProfile& profile,
                            const config::Settings& settings) {
    const auto tier =
        settings.models.frame_tier == "draft" ? Tier::DRAFT : Tier::FINAL;
    auto it = profile.tiers.find(tier);
    // 档位表里没有这一档就退回另一档。**不抛**：档位表是推出来的，
    // 少一档也该照样出片，而不是让整章跑不起来。
    if (it == profile.tiers.end()) {
        it = profile.tiers.find(tier == Tier::DRAFT ? Tier::FINAL : Tier::DRAFT);
    }
    if (it == profile.tiers.end()) return {};

    models::TierSpec spec = it->second;
    if (settings.models.frame_steps > 0) spec.steps = settings.models.frame_steps;
    return spec;
}

void apply_project_spec(config::Settings& settings,
                        models::HardwareProfile& profile) {
    auto it = profile.tiers.find(models::Tier::FINAL);
    if (it == profile.tiers.end()) return;
    // **要的是"表里那个数"，不是 tiers 里现在躺着的那个。**
    //
    // `Runtime::profile()` 已经先用 effective_spec 把 tiers[FINAL].steps
    // 换成实跑的值了（挂着 Turbo 就是 6），好让 /api/hardware 和磁盘上的
    // 成片对得上。拿它再算一次就是**同一个变换套两次**：final_steps 还是
    // 6（那一支幂等），可 frame_steps = table_final_steps 不幂等，
    // 首帧步数从 20 塌成 6。
    //
    // 出图那一步没有 Turbo LoRA，6 步就是裸跑 6 步——首帧糊，而首帧是
    // 每一镜的起始图和跨镜头一致性的锚点，糊了后面全糊，全程不报错。
    // effective_spec 里那段注释早就写着这一条，是这条调用链把它架空了。
    // 2026-09-13 实机撞到：进度条写着"出首帧（第 1/6 步）"。
    const int table_steps = profile.table_final_steps > 0
                                ? profile.table_final_steps
                                : it->second.steps;
    const auto eff = config::effective_spec(settings, table_steps);
    it->second.width = eff.width;
    it->second.height = eff.height;
    it->second.steps = eff.final_steps;
    it->second.steps_pinned = eff.steps_pinned;
    settings.models.frame_steps = eff.frame_steps;
}

/// 一个档位的入口状态。
///
/// 首帧失败的镜头状态还停在 AUDIO_DONE（配音接上之前是 PLANNED）。
/// **它们不该被跳过**，而是退回纯文生视频——画面一致性差一些，
/// 但整章不会卡在这里。
std::set<ShotStatus> render_entry_states(Tier tier, bool skip_draft) {
    if (tier == Tier::FINAL) {
        std::set<ShotStatus> in{ShotStatus::DRAFT_DONE,
                                ShotStatus::FINAL_REJECTED};
        // **跳过草稿档时，成片档要收首帧刚做完的那一批。**
        // 不收的话它们卡在 FRAME_DONE 上，成片阶段一个镜头都挑不到，
        // 而且不报错——表现是"跑完了，什么都没出"。
        if (skip_draft) {
            in.insert(ShotStatus::FRAME_DONE);
            in.insert(ShotStatus::DRAFT_REJECTED);
            in.insert(ShotStatus::AUDIO_DONE);
        }
        return in;
    }
    return {ShotStatus::FRAME_DONE, ShotStatus::DRAFT_REJECTED,
            ShotStatus::AUDIO_DONE};
}

bool assembly_usable(const Shot& s) {
    // 只装配**已经出片而且过了闸门**的镜头。
    // 「哪些状态算有片」收在 `models::status_has_film`（那儿写着为什么），
    // 这儿只多加一条：视频文件真的在。
    return s.video_path.has_value() && !s.video_path->empty() &&
           models::status_has_film(s.status);
}

std::vector<LeftOut> assembly_left_out(const Episode& ep) {
    // **存字符串，不存指针。** `sorted_shots()` 按值返回，range-for 里那份
    // 临时 vector 出了循环就析构了——存 `const Shot*` 的话整张表立刻悬空，
    // 后面拼消息时读到的是已释放内存里的 std::string，实机表现是
    // `std::bad_alloc` 把整个装配打断（2026-09-13 撞到，就在这里）。
    std::vector<LeftOut> out;
    for (const auto& s : ep.sorted_shots()) {
        if (assembly_usable(s)) continue;
        std::string line = SAYF("%1：%2", s.shot_id, models::status_zh(s.status));
        // 状态说得过去、片子却不在，是另一回事（多半是文件被删了），
        // 光报状态会让人以为是状态卡住了。
        if (!s.video_path.has_value() || s.video_path->empty()) {
            line += SAY("（还没有视频文件）");
        }
        out.push_back({s.shot_id, std::move(line)});
    }
    return out;
}

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool wants(const RunOptions& o, Stage s) {
    if (!o.only.has_value()) return true;   // 没给就是全跑
    return std::find(o.only->begin(), o.only->end(), s) != o.only->end();
}

/// 「全部跳过」那句挂在哪个阶段名下：只跑了某几段就是最后那一段，
/// 全流程就是装配。顶栏按阶段名写标签，挂错了会写成「出片」。
const char* last_stage_name(const RunOptions& o) {
    if (!o.only.has_value() || o.only->empty()) return "assemble";
    return to_string(o.only->back());
}

void emit(JobProgress& p, const char* stage, const char* kind,
          const std::string& message, int current = 0, int total = 0) {
    Event e;
    e.stage = stage;
    e.kind = kind;
    e.message = message;
    e.current = current;
    e.total = total;
    p.report(e);
}

/// 一批镜头的 id。给 JobProgress::set_pending 用——每个阶段开工时登记
/// "这一轮还有哪几镜没落定"，界面刷新之后靠它把「排队中」重新点亮。
std::vector<std::string> ids_of(const std::vector<Shot*>& shots) {
    std::vector<std::string> out;
    out.reserve(shots.size());
    for (const Shot* s : shots) out.push_back(s->shot_id);
    return out;
}

}  // namespace

/// 装配成片。
///
/// 单独一个函数只是为了让 run_episode 里那一段短一点——它已经有五个阶段了。
/// 可选项见 `AssembleOptions`：默认是这一章的正片，预告那条（preview.cpp）
/// 传一份别的进来，走的是**同一段代码**。
std::string assemble_episode(const ProjectStore& store,
                             const config::Settings& settings, Episode& ep,
                             const media::FFmpeg& ff, JobProgress& progress,
                             const AssembleOptions& aopts) {
    std::vector<Shot> shots;
    for (const auto& s : ep.sorted_shots()) {
        if (!aopts.only_shots.empty() &&
            aopts.only_shots.count(s.shot_id) == 0) {
            continue;
        }
        if (assembly_usable(s)) shots.push_back(s);
    }
    if (shots.empty()) {
        throw std::runtime_error(SAY("没有可装配的镜头。先跑渲染阶段"));
    }

    emit(progress, "assemble", "start",
         SAYN("装配 %n 个镜头", static_cast<long long>(shots.size())), 0,
         static_cast<int>(shots.size()));

    // **漏下的镜头要点名。** 和降级那条一样：不拦（剩下的镜头照样能拼成片），
    // 但要说出是谁、为什么、怎么办。判据见 assembly_left_out。
    //
    // 只装一部分时（预告）**只点名这一部分里漏下的**：整章那份会把"这一轮
    // 根本没打算做的"也念一遍，而那不是问题，是这次就没要。
    std::vector<LeftOut> left_out = assembly_left_out(ep);
    if (!aopts.only_shots.empty()) {
        std::vector<LeftOut> kept;
        for (const auto& one : left_out) {
            if (aopts.only_shots.count(one.shot_id)) kept.push_back(one);
        }
        left_out = std::move(kept);
    }
    if (!left_out.empty()) {
        const std::size_t want = aopts.only_shots.empty()
                                     ? ep.shots.size()
                                     : aopts.only_shots.size();
        std::string msg =
            aopts.only_shots.empty()
                ? SAYN("这一章有 %n 个镜头，只有 %1 个进了成片。没进去的：",
                       static_cast<long long>(want), std::to_string(shots.size()))
                : SAYN("这一轮要装 %n 个镜头，只有 %1 个进了成片。没进去的：",
                       static_cast<long long>(want), std::to_string(shots.size()));
        for (std::size_t i = 0; i < left_out.size() && i < 3; ++i) {
            msg += "\n  " + left_out[i].line;
        }
        if (left_out.size() > 3) {
            msg += SAYN("\n  …… 还有 %n 个",
                        static_cast<long long>(left_out.size() - 3));
        }
        msg += SAY("\n这几镜出完片再装配一次，成片才是完整的。");
        emit(progress, "assemble", "warn", msg);
    }

    // **装配前先查音画能不能装下**，装完再发现就得重做整章。
    // 查出问题只报不拦：拦下来的话一句台词长了一点整章就出不来，
    // 而那一点多半是听不出来的。
    for (const auto& shot : shots) {
        if (shot.dialogue.empty()) continue;
        const auto r = gates::gate_audio_sync(
            shot, store.paths().abs(*shot.video_path), ff, settings.gates);
        if (!r.ok()) {
            Event e;
            e.stage = "assemble";
            e.kind = "gate";
            e.shot_id = shot.shot_id;
            e.message = r.describe();
            progress.report(e);
        }
    }

    const media::Timeline timeline =
        media::build_timeline(shots, store.paths(), settings.assembly,
                              [&ff](const std::filesystem::path& v) {
                                  return ff.probe(v).duration_s;
                              });

    // **字幕自检。** `media::subtitle_problems` 早就写好了，头文件里注明
    // 「装配后闸门要用」，可 2026-09-13 查下来**全代码库一处调用都没有**
    // ——只有单元测试碰过它。写了不接等于没写。
    //
    // 和上面音画那条一样只报不拦：一条字幕短了 0.1 秒不该让整章出不来，
    // 而人看见了可以自己回去改。
    for (const std::string& note :
         media::subtitle_problems(timeline, settings.assembly)) {
        Event e;
        e.stage = "assemble";
        e.kind = "gate";
        e.message = SAYF("字幕：%1", note);
        progress.report(e);
    }

    // ---- 配乐 ----
    //
    // 一章一条器乐，装配时压在台词底下。文件在就沿用（想重出就删掉它）。
    // 没配命令只说一声，不拦装配。
    std::optional<fs::path> music;
    if (settings.sound.music) {
        if (text::strip_ws(settings.sound.music_command).empty()) {
            emit(progress, "assemble", "info",
                 SAY("配乐开着，但全局配置里没有 [sound].music_command，"
                     "这一章没有配乐。ACE-Step 的包装脚本在 "
                     "tools/music_ace_step.py"));
        } else {
            const fs::path want = stages::music_path_for(
                store.paths(), ep.episode_id, aopts.music_suffix);
            std::error_code mec;
            if (!fs::is_regular_file(want, mec)) {
                emit(progress, "assemble", "progress", SAY("生成这一章的配乐"));
            }
            const auto m =
                stages::ensure_music(settings, store.paths(), ep,
                                     timeline.total_duration_s(),
                                     aopts.music_suffix);
            if (m.ok) {
                music = m.path;
                emit(progress, "assemble", "info",
                     m.reused
                         ? SAYF("沿用已有的配乐 %1",
                                paths::to_utf8(m.path.filename()))
                         : SAYF("配乐已生成 %1",
                                paths::to_utf8(m.path.filename())));
            } else {
                emit(progress, "assemble", "warn",
                     SAYF("这一章没有配乐：%1", m.error));
            }
        }
    }

    media::Assembler assembler(ff, settings.assembly, store.paths(),
                               settings.gates.target_lufs,
                               settings.gates.max_true_peak_db);
    // 后期链（柔化、调色、颗粒）、环境声（各镜的原生音轨）、配乐、放大。
    // 全是 2026-09-14 加的，见 docs/电影质感方案.md；开关在项目的
    // changji.toml 的 [look] / [sound]，放大和配乐命令在全局 [upscale] /
    // [sound]。
    media::FinishOptions finish;
    finish.look = settings.look;
    finish.sound = settings.sound;
    finish.upscale = settings.upscale;
    finish.project_root = store.root();
    finish.music = music;
    finish.warn = [&progress](const std::string& msg) {
        emit(progress, "assemble", "warn", msg);
    };
    assembler.set_finish(finish);
    {
        std::string how = SAY("后期：");
        how += settings.look.preset == "off"     ? SAY("不调色")
               : settings.look.preset == "clean" ? SAY("柔化加颗粒")
                                                 : SAY("胶片（柔化、调色、颗粒）");
        how += settings.sound.ambient ? SAY("；环境声：各镜原生音轨")
                                      : SAY("；环境声：关");
        how += music.has_value() ? SAY("；配乐：有") : SAY("；配乐：无");
        if (settings.upscale.enabled()) {
            how += SAYF("；放大 %1×", std::to_string(settings.upscale.scale));
        }
        emit(progress, "assemble", "info", how);
    }
    // **这台的 ffmpeg 没编 libass 就别烧字幕，成片照出。**
    //
    // 2026-09-16 实撞：`brew install ffmpeg` 出来的 9.0.1 没有 `subtitles`
    // 滤镜，于是一章十七镜全渲完、装配整个失败、输出目录空的——为了一条
    // 可选的烧录，把整章扔了。而 ffmpeg 报的还是一句误导的
    // 「No option name near '/Users/…'」，人会去查路径里的空格
    // （macOS 默认目录里那个 "Application Support" 永远带空格）。
    //
    // 字幕本来就有外挂这条路：.ass 仍然写在 subtitles/ 下，播放器挂上就能看。
    // 说一句就够了，不用拦。
    const bool can_burn = ff.has_filter("subtitles");
    if (!can_burn) {
        emit(progress, "assemble", "info",
             SAY("这台的 ffmpeg 没编 libass（没有 subtitles 滤镜），"
                 "字幕没烧进画面。"
                 "成片照常出，字幕另存在 subtitles/ 下，播放器里挂上就能看。"
                 "要烧进画面的话装一个带 libass 的 ffmpeg。"));
    }
    // **一章一个文件，这儿不切。**
    //
    // 三次拍板叠出来的结论：
    //   · 2026-09-16 用户定「只留章模式」——那之前 [assembly].episode_s 有个
    //     0 的岔口表示"不切、整章一集"，两条路两套提示词，内容大半重合。
    //   · 2026-09-17 用户定「集是最后按用户选的时长切的」，于是这儿更不能切：
    //     同一段片子切两次，第二次的刀正落在第一次的缝上。
    //   · 2026-09-18 定成电影平台，成片就是一部完整的电影、一个文件，整条
    //     切段链（media::split_into_episodes、pipeline/series_cut）拔掉。
    //
    // 所以这一步只做一件事：把这一章的时间线装成 <episode_id>.mp4。
    // （预告那条传的是 `preview/<episode_id>.mp4`，见 AssembleOptions。）
    const std::string name =
        aopts.out_name.empty() ? ep.episode_id + ".mp4" : aopts.out_name;
    const std::filesystem::path output = assembler.assemble(timeline, name, can_burn);
    if (aopts.total_s != nullptr) *aopts.total_s = timeline.total_duration_s();
    // 成片检查同样只报不拦：成片已经出来了，人可以自己看一眼再决定。
    for (const auto& reason :
         gates::gate_episode(output, ff, settings.gates,
                             timeline.total_duration_s()).reasons) {
        Event e;
        e.stage = "assemble";
        e.kind = "gate";
        e.message = SAYF("成片：%1", reason);
        progress.report(e);
    }
    // ---- 清掉老项目留下的孤儿成片 ----
    //
    // 2026-09-18 之前装配有两套名字：不切时 `ep01.mp4`，按每集时长切成几集时
    // `ep01_01.mp4` / `ep01_02.mp4`。切几集是内容的结果、会变，于是磁盘上
    // 同时躺着三份、两份过时（2026-09-17 实见）。今天一章只出一个文件，
    // `_NN` 不再产出——**但老项目的磁盘上还躺着**，这段清理还得认得出它们。
    //
    // **只删这一章自己那两种名字、而且不是这一轮刚写的那个。** 别的章、
    // 别的文件一个不碰。
    //
    // 预告那条整个跳过（`sweep_orphans = false`）：它的产物根本不在 output/
    // 顶层，而这段认的是正片的命名——照跑一遍只会把正片的孤儿在预告跑完时
    // 顺手删掉，一件和这次操作无关的事。
    //
    // **字幕跟着成片一起清。** 外挂字幕和成片同名（media/assemble.cpp 里
    // `final_path.stem() + ".ass"`），孤儿的形状一模一样：磁盘上实见
    // ep01.ass 和 ep01_01.ass / ep01_02.ass 并存。只清成片的话，播放器
    // 挂字幕时挑到的可能正是那份过时的。
    if (aopts.sweep_orphans) {
        int gone = 0;
        for (const auto& [dir, ext] :
             {std::pair{store.paths().output(), std::string(".mp4")},
              std::pair{store.paths().subtitles(), std::string(".ass")}}) {
            std::error_code ec;
            for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
                if (f.path().extension() != ext) continue;
                // 判法只有一份，在 media::episode_of_output 上（那儿能测）
                const auto owner =
                    media::episode_of_output(paths::to_utf8(f.path().stem()));
                if (!owner || *owner != ep.episode_id) continue;
                // 这一轮刚写出去的那个当然留着；字幕同名换后缀
                const std::string stem = paths::to_utf8(f.path().stem());
                if (stem + ".mp4" == name) continue;
                std::error_code rm;
                std::filesystem::remove(f.path(), rm);
                if (!rm) ++gone;
            }
        }
        if (gone > 0) {
            emit(progress, "assemble", "info",
                 // 说法跟成片页那一句对齐（FilmView.vue：「上一版按时长切
                 // 出来的几段（盘上叫「第01集.mp4」这种名字）」）。**要带上
                 // 盘上的名字**，不然人在文件夹里对不上号：这儿报「清掉 3 个
                 // 文件」，他打开 output/ 看到的是 第01集.mp4 这种名字。
                 SAYN("清掉老项目留下的 %n 个文件（成片和字幕）："
                      "那是上一版按时长切出来的几段"
                      "（盘上叫「第01集.mp4」这种名字），"
                      "今天这一章只出一个 %1",
                      gone, name));
        }
    }

    // **降级的镜头要在最后这句里说出来。**
    //
    // 「重试超限，降级处理」那一句是在出片那一档报的，等装配跑完早滚出
    // 屏幕了；而人真正读的是最后这一行。2026-09-13 实测：walk_c ep01
    // 十八镜里有一镜降级、ep02 十五镜里有一镜降级，这两章的成片和全程顺利
    // 那一章说的是同一句「成片已生成」——**看不出这一章里有一镜是没过闸门
    // 的**。
    //
    // 降级本身是对的（gates.fallback_on_exhausted：保证整章能出片，而不是
    // 卡在某一镜上）。不对的是它悄悄发生。
    //
    // **说清楚降级到底做了什么。** 上一版这句写的是"降级成了静帧加运镜，
    // 这几镜没有真正的运动"——两句都是假的：render.cpp 里的 fallback 只改
    // 状态、写一句备注，**留下的就是最后那一版视频**，全代码库一处
    // zoompan 都没有。实测 ep01_sh004 逐帧 YAVG 从 23.1 平滑降到 19.1，
    // 是真视频不是冻帧。人照着那句话去找"哪一镜不动"，永远找不到。
    std::vector<const models::Shot*> degraded;
    for (const models::Shot& s : shots) {
        if (s.status == models::ShotStatus::FALLBACK) degraded.push_back(&s);
    }
    if (!degraded.empty()) {
        std::string msg =
            SAYN("这一章有 %n 个镜头重试超限，用的是最后那一版——没过闸门，"
                 "但片子在，也进了成片。闸门当时说的是：",
                 static_cast<long long>(degraded.size()));
        // **把闸门当时说的原话带上，不要猜原因。**
        //
        // 上一版这里写的是"多半是出片时显存不够"——那是写的时候只见过
        // 显存那一种。实测至少还有一种：walk_c ep01_sh004 是一个按要求
        // 压暗的镜头（车内只有手机蓝光），被"近乎纯色"那道闸门误判，
        // 和显存毫无关系。一句自信的错误归因，比不给原因更糟：人照着
        // 去腾显存，腾完还是降级。
        //
        // 每一镜的 gate_notes 里存着当时的原话，直接给它。
        // 只列前三个：二十镜里降了十个的时候，列全了没人会读。
        for (std::size_t i = 0; i < degraded.size() && i < 3; ++i) {
            msg += "\n  " + degraded[i]->shot_id;
            if (!degraded[i]->gate_notes.empty()) {
                msg += SAYF("：%1", degraded[i]->gate_notes.front());
            }
        }
        if (degraded.size() > 3) {
            msg += SAYN("\n  …… 还有 %n 个",
                        static_cast<long long>(degraded.size() - 3));
        }
        msg += SAY("\n单独重出这几镜看看；一直降级的话，按上面那句说的原因处理。");
        emit(progress, "assemble", "warn", msg);
    }

    emit(progress, "assemble", "done",
         SAYF("成片已生成：%1", paths::to_utf8(output)),
         static_cast<int>(shots.size()), static_cast<int>(shots.size()));
    return paths::to_utf8(output);
}

const char* to_string(Stage s) {
    switch (s) {
        case Stage::Audio:    return "audio";
        case Stage::Frames:   return "frames";
        case Stage::Draft:    return "draft";
        case Stage::Final:    return "final";
        case Stage::Assemble: return "assemble";
    }
    return "?";
}

bool stage_from_string(const std::string& s, Stage& out) {
    if (s == "audio")    { out = Stage::Audio;    return true; }
    if (s == "frames")   { out = Stage::Frames;   return true; }
    if (s == "draft")    { out = Stage::Draft;    return true; }
    if (s == "final")    { out = Stage::Final;    return true; }
    if (s == "assemble") { out = Stage::Assemble; return true; }
    return false;
}

RunReport run_episode(const ProjectStore& store,
                      const HardwareProfile& profile,
                      const config::Settings& settings, const RunOptions& opts,
                      const Backends& backends, JobProgress& progress,
                      CancelToken& tok) {
    RunReport report;
    report.episode_id = opts.episode_id;
    const double started = now_seconds();

    Project project = store.load_project();
    // **不是 const**：配音那一段会给没有音色的角色各定一个，写回 voice_id
    // 再存盘（见下面 ensure_character_voice 那一块）。
    AssetLibrary assets = store.load_assets();

    // **比例跟画幅对一次账。**
    //
    // 出首帧和出片那两层（run_frames / render_batch）拿不到 Settings，
    // 它们从 `assets.style.aspect_ratio` 取比例把档位表的宽高转过来。
    // 而那份拷贝在 2026-09-14 之前是能单独改的，老项目里可能和画幅不一致
    // ——那样出来是首帧竖的、成片横的，全程不报错。
    //
    // **只改内存里这一份，不回写盘。** 跑一章不该顺手改项目文件；真要
    // 落盘由存画面那个接口做（见 /bff/project/video）。
    //
    // **对老项目来说这一句已经是恒等的**，而那正是它该有的样子：没写
    // `[video]` 的项目，`settings.video.orientation` 本来就是
    // `load_settings` 从这份 assets.json 推回来的。2026-09-18 默认翻成横屏
    // 之前，这儿会把一个出了两百镜 544×928 的项目就地翻成 16:9，下一镜
    // 出 928×544，同一章两种画幅而全程不报错。
    assets.style.aspect_ratio = settings.video.aspect_ratio();

    Episode* ep = project.episode_by_id(opts.episode_id);
    if (ep == nullptr) {
        throw std::runtime_error(
            SAYF("项目里没有这一章 %1", opts.episode_id));
    }
    if (ep->shots.empty()) {
        throw std::runtime_error(
            SAYF("这一章 %1 还没有分镜表", opts.episode_id));
    }

    progress.set_episode_id(opts.episode_id);

    // **开跑之前把「降级了却一个视频都没有」的那些归位。**
    //
    // `fallback` 的意思是"重试用尽，留最后那一版"，所以它算终态、下一轮
    // 挑镜头时跳过。可**根本没产出过任何一版时，没有什么可留的**——
    // 那是"没跑成"，不是"跑过了、质量不行"。
    //
    // 2026-09-17 实撞：唯一那台工作机在出片中途掉线，17 镜里 16 镜被标成
    // fallback（每一镜都只是撞了同一堵墙，一帧都没渲出来）。之后再点出片，
    // 这 16 镜被当成终态跳过，人永远等不到它们被重跑；而镜头页那颗主按钮
    // 还写着「这一章出完了」——零个视频，按钮说做完了。
    //
    // 掉线那条已经不再这么标了（池在队列里等机器回来，不往上抛），但盘上存下的
    // 那些还在，别的原因也可能留下没有视频的 fallback。
    //
    // **归位到配音完成，不是从头来过**：配音和首帧可能是好的，只有出片那
    // 一步没成。退到 AUDIO_DONE 之后，正常的入口判据会带着它往下走，
    // 有首帧就用首帧。**这儿只改状态、不删任何文件。**
    //
    // 只在这儿做、不在 pick 里做：pick 是每个阶段都叫的，在那儿放行会让
    // 「草稿没渲出来的镜头不该在同一轮里被推去出成片」那条规矩失效
    // （test_episode 的「跑全流程时成片档不吃 force」钉着它）。
    {
        int repaired = 0;
        for (Shot& s : ep->shots) {
            if (s.status != ShotStatus::FALLBACK) continue;
            if (s.video_path.has_value() && !s.video_path->empty()) continue;
            s.status = ShotStatus::AUDIO_DONE;
            s.attempts = 0;
            ++repaired;
        }
        if (repaired > 0) {
            emit(progress, "audio", "info",
                 SAYN("有 %n 镜标着已降级却一个视频都没有"
                      "（多半是上一轮所有工作进程都掉线了），这一轮把它们接着跑",
                      repaired));
        }
    }

    // 每个阶段跑完立刻存盘。不存的话中途断电或者点了停止，
    // 前面几十分钟的产出全部作废——文件还在磁盘上，但项目文件里没记，
    // 下次跑会当成没跑过。
    //
    // ⚠️ **存的是"重新读一份、只把这一章的镜头换上去"，不是手里这份整份
    // 写回。**
    //
    // 手里这份 `project` 是**开跑那一刻**读的，而一轮要几十分钟到几小时。
    // 整份写回去的话，这期间界面上改的东西全被静默盖掉：另一章的镜头抽屉
    // 存的那一笔、改过的章名、手动加的一章、写好的剧本。全程 200，屏幕上
    // 什么都不会说——http 那边 guard_not_running 的注释描述的就是这个形状
    // （「新名字被静默盖回旧的，界面上看着像改名没生效」），只是那道闸只
    // 拦了删项目和改项目名两条路，别的写接口一条没拦。
    //
    // 这一轮真正拥有的只有这一章的 shots（状态、产出路径、重试次数、拆出
    // 来的新镜头、重排过的时长），所以只换这一格。
    //
    // 同一章的镜头在跑的过程中被人改了，仍然会被这一轮盖掉——那是真冲突，
    // 不在这儿解决。
    bool gone_said = false;
    const auto save = [&] {
        Project latest = store.load_project();
        Episode* target = latest.episode_by_id(opts.episode_id);
        if (target == nullptr) {
            // 跑着跑着这一章被删了。**不能照旧那份写回去**——那等于把用户
            // 的删除撤销掉，而且撤销出来的是一份几十分钟前的快照。
            if (!gone_said) {
                gone_said = true;
                report.errors.push_back(
                    SAY("这一章在跑的过程中被删掉了，"
                        "这一轮的进度没有写回项目文件"
                        "（已经出来的文件还在磁盘上）"));
            }
            return;
        }
        target->shots = ep->shots;
        store.save_project(latest);
    };

    // 这一轮到底跑了东西没有。见末尾那句「全部跳过」。
    bool ran = false;

    // 渲染一个档位。草稿和成片只差三个东西：入口状态、档位参数、事件名。
    const auto render_tier = [&](Tier tier, bool force,
                                 pipeline::ShotFlow* flow = nullptr,
                                 const std::vector<Shot*>& coming = {}) {
        const char* stage_name = tier == Tier::FINAL ? "final" : "draft";
        auto todo = pick(*ep, render_entry_states(tier, opts.skip_draft), force,
                         opts.only_shots);
        // 名单登在这一档的名下：流水时首帧那层同时在报完成，别让它划掉
        if (flow) {
            // 流水时首帧那批也算进来：它们此刻还停在配音完成上，按状态挑
            // 挑不到，可首帧一写回就该出片。已经在 todo 里的不重复。
            for (Shot* s : coming) {
                if (std::find(todo.begin(), todo.end(), s) == todo.end()) {
                    todo.push_back(s);
                }
            }
            std::stable_sort(todo.begin(), todo.end(),
                             [](const Shot* a, const Shot* b) {
                                 return a->order < b->order;
                             });
        }
        progress.set_pending(ids_of(todo), stage_name);
        if (todo.empty()) return std::vector<stages::RenderOutcome>{};
        ran = true;

        // 消息里报的是**表里的**分辨率，不是按画幅缩放后的。
        // Python 就是这样，而且这样才对得上设置页上显示的数字——
        // 用户在那儿填的是 640x352，看到日志里写 448x768 会以为设置没生效。
        const TierSpec& spec = profile.tiers.at(tier);
        // 步数不写在这儿：跨机时由干活那台按自己有没有 Turbo 定
        // （config::steps_on_node），这台算的数可能是错的——2026-09-16
        // 这句写着"20 步"、远程实际跑 6 步。每镜的进度条上有真数。
        std::string msg =
            SAYN("%1 档渲染 %n 个镜头，%2x%3",
                 static_cast<long long>(todo.size()), models::to_string(tier),
                 std::to_string(spec.width), std::to_string(spec.height));
        if (const auto est = profile.estimate_episode(
                static_cast<int>(todo.size()), tier)) {
            msg += SAYF("，粗估 %1", util::human_time(*est));
        }
        // **流水时这一句得说清"还没开始"。** 这一档和首帧同时开着，但按
        // 排位（shot_flow.hpp）：首帧还有下一张可派时，出片一个位置都不抢。
        // 照旧那句写法，顶栏在第 0 秒就写着「final 档渲染 17 个镜头」，
        // 而那一刻跑的全是首帧。
        if (flow) msg += SAY("。先让首帧吃满卡，腾出一张就开出片");
        emit(progress, stage_name, "start", msg, 0,
             static_cast<int>(todo.size()));

        // ---- 闸门 ----
        //
        // 以前这儿没有。`gate_video` 和 `decide_next` 移植了、也和 Python
        // 一条不差地对过，但真二进制里从来没人调——出完片直接置
        // DRAFT_DONE。也就是 C++ 从不拦废片、从不重试、从不降级，
        // 而 Python 默认每一镜都过。整套质量控制在这边是空的。
        stages::GateHooks gate;
        // 上限**不管闸门开没开都生效**：渲染抛错的重试也数它。
        gate.max_attempts = settings.gates.max_attempts_per_shot;
        if (settings.gates.enabled) {
            if (backends.ffmpeg) {
                const media::FFmpeg& ff = *backends.ffmpeg;
                const config::GateConfig& gcfg = settings.gates;
                const int fps = settings.assembly.fps;
                // Python：f"{spec.tier.value} 档闸门"
                const std::string gate_name =
                    SAYF("%1 档闸门", models::to_string(tier));
                gate.check = [&ff, &gcfg, fps, gate_name](
                                 const Shot& shot,
                                 const std::filesystem::path& video,
                                 const stages::RenderPlan& plan) {
                    // 期望时长按帧数反推，不按 duration_s——帧数是 4n+1
                    // 截过的，真片长就是它。Python 也是这么算的。
                    const double expected =
                        static_cast<double>(
                            stages::frames_for(shot.duration_s, fps)) /
                        fps;
                    return gates::gate_video(
                        shot, video, ff, gcfg, expected,
                        std::make_pair(plan.spec.width, plan.spec.height),
                        gate_name);
                };
                gate.decide = [&gcfg](const gates::GateResult& r,
                                      const Shot& s) {
                    return gates::decide_next(r, s, gcfg);
                };
            } else {
                // Python 这时候会在 gate_video 里炸。这边选择说一声然后
                // 不过闸门——没装 ffmpeg 的机器上前几步照样能跑，
                // 而"跑到闸门才说缺 ffmpeg"最气人。装配那一步也是这么处理的。
                emit(progress, stage_name, "warn",
                     SAY("闸门开着但没找到 ffmpeg，这一档不过闸门"));
            }
        }

        // ---- 关键镜头多出几条、尾帧串镜 ----
        //
        // 都是这部电影的属性（[video].hero_takes / chain_frames）。串镜要抽
        // 上一镜的最后一帧，没有 ffmpeg 就不串；上一镜按镜头顺序找，不按这一批
        // 的顺序——单跑几镜时前一镜不在这一批里。
        stages::RenderExtras extras;
        extras.flow = flow;
        extras.hero_takes = settings.video.hero_takes;
        extras.chain_frames = settings.video.chain_frames;
        extras.keep_ambient = settings.sound.ambient;
        if (backends.ffmpeg) {
            const media::FFmpeg& ff = *backends.ffmpeg;
            const int fps = settings.assembly.fps;
            extras.last_frame = [&ff, fps](const std::filesystem::path& video,
                                           const std::filesystem::path& dest) {
                try {
                    const double dur = ff.probe(video).duration_s;
                    // 最后一帧的时间点：片尾往回一帧半，`-ss` 落在最后
                    // 一帧之后会抽不到东西。
                    const double at =
                        std::max(0.0, dur - 1.5 / std::max(1, fps));
                    ff.extract_frame(video, dest, at);
                    std::error_code ec;
                    return std::filesystem::is_regular_file(dest, ec);
                } catch (const std::exception&) {
                    return false;
                }
            };
        }
        {
            Episode* episode = ep;
            const models::ProjectPaths ppaths = store.paths();
            extras.prev_video =
                [episode, ppaths](const Shot& s)
                -> std::optional<std::filesystem::path> {
                for (const Shot& other : episode->shots) {
                    if (other.order != s.order - 1) continue;
                    if (!other.video_path.has_value() || other.video_path->empty()) {
                        return std::nullopt;
                    }
                    return ppaths.abs(*other.video_path);
                }
                return std::nullopt;
            };
        }

        // fps 走配置，不是写死的 24。Python 那边是
        // `RenderStage(..., fps=self.settings.assembly.fps)`；这儿以前
        // 漏了这个参数吃了默认值，`[assembly].fps = 30` 时两边算出来的
        // 帧数不同——**片长会不一样**，而没有任何一层会报错。
        return stages::render_batch(todo, assets, spec, store.paths(),
                                    backends.video, progress, tok,
                                    settings.assembly.fps,
                                    backends.render_lanes, gate, save, extras);
    };

    try {
        // ---- 配音 ----
        //
        // **在生成任何画面之前跑完。** 它决定镜头时长，而时长决定帧数。
        // 顺序反过来的话，配音出来装不进已经渲好的视频里。
        if (wants(opts, Stage::Audio) && !tok.cancelled()) {
            auto todo = pick(*ep, {ShotStatus::PLANNED}, opts.force, opts.only_shots);
            progress.set_pending(ids_of(todo), "audio");
            if (!todo.empty()) {
                ran = true;
                const stages::TTSBackend backend =
                    backends.tts.value_or(stages::estimate_backend());
                // **后端名字对用户没有意义，要说清楚这次到底出不出声音。**
                // 只说 "estimate" 的话，用户跑完一整章才发现成片是静音的。
                const std::string how =
                    backend.name == "estimate"
                        ? SAY("只算时长不出声音，成片会是静音")
                    : backend.name == "http" ? SAY("走独立配音服务")
                                             : backend.name;
                emit(progress, "audio", "start",
                     SAYN("给 %n 个镜头配音，%1",
                          static_cast<long long>(todo.size()), how),
                     0, static_cast<int>(todo.size()));

                // **先给没有音色的角色各定一个。**
                //
                // 不给参考音频时，每次合成都重新采样一个说话人，而音色是
                // (种子, 文本) 的函数——换一句台词就是换一个人。
                // 2026-09-13 在 walk_c 上量到的：同一个角色的**同一句话被
                // 拆成两半**，前半句 136 Hz、后半句 338 Hz，说到一半换了
                // 个人。整章每个角色每句都是不同的人，而且全程不报错。
                //
                // 定一次就落成一段参考音频存进 voices/，之后每句都克隆它。
                // 一个角色只花一次（约八秒），而且人可以随时去角色页换掉。
                //
                // **只管这一批要配音的镜头里真出场的那几个**：整个资产库
                // 都摸一遍的话，没戏份的角色也白占八秒。
                if (backend.name == "local") {
                    std::set<std::string> speaking;
                    for (const Shot* s : todo) {
                        for (const auto& line : s->dialogue) {
                            if (line.char_id.has_value() && !line.char_id->empty()) {
                                speaking.insert(*line.char_id);
                            }
                        }
                    }
                    int made = 0;
                    for (const std::string& id : speaking) {
                        const auto it = assets.characters.find(id);
                        if (it == assets.characters.end()) continue;
                        if (it->second.voice_id.has_value() &&
                            !it->second.voice_id->empty()) {
                            continue;
                        }
                        try {
                            emit(progress, "audio", "progress",
                                 SAYF("给 %1 定一个音色", it->second.name));
                            stages::ensure_character_voice(store, it->second);
                            ++made;
                        } catch (const std::exception& e) {
                            // 定不出来不拦着整章：退回原来那条（每句随机一个
                            // 说话人），难听但出得来。说一声就行。
                            emit(progress, "audio", "warn",
                                 SAYF("%1 的音色没定上：%2"
                                      "。这个角色的每句台词会是不同的声音",
                                      it->second.name, e.what()));
                        }
                    }
                    if (made > 0) {
                        // **同上面那个 save()：重新读一份，只把刚定下来的
                        // 音色写进去。** `assets` 也是开跑那一刻读的，而定
                        // 音色发生在几分钟之后——整份写回去会把这期间在设定
                        // 页改的外观、提示词、参考图路径全盖掉。
                        //
                        // `ensure_character_voice` 只改一个字段（voice_id），
                        // 所以只搬这一个；这期间人自己挑了音色的就不顶。
                        AssetLibrary latest = store.load_assets();
                        for (const std::string& id : speaking) {
                            const auto src = assets.characters.find(id);
                            if (src == assets.characters.end()) continue;
                            if (!src->second.voice_id.has_value()) continue;
                            auto dst = latest.characters.find(id);
                            if (dst == latest.characters.end()) continue;
                            if (dst->second.voice_id.has_value() &&
                                !dst->second.voice_id->empty()) {
                                continue;
                            }
                            dst->second.voice_id = src->second.voice_id;
                        }
                        store.save_assets(latest);
                        emit(progress, "audio", "done",
                             // 「角色页」2026-09-11 合成「设定」了。
                             SAYN("给 %n 个角色定了音色，存在项目的 voices/ 里。"
                                  "不满意可以去设定页的「人物」点开这个角色换"
                                  "一个，换完回镜头墙点这一镜的「配音」重跑",
                                  made));
                    }
                }

                stages::AudioStage stage(backend, settings.tts, store.paths());
                // 同时跑几镜：走别人家的池时是池的位置数，本机那条是 1。
                report.audio = stage.run(todo, assets, progress, tok,
                                            backends.audio_lanes);
                save();

                // 台词太多装不下的镜头，在这里拆成连着的几镜。
                //
                // **拆在配音之后、出首帧之前**：音频已经有了、画面还没生成，
                // 拆开不浪费任何一次渲染。
                const std::size_t before = ep->shots.size();
                ep->shots = stages::split_overlong_shots(
                    ep->shots, stages::max_shot_duration_s(settings.assembly.fps));
                const std::size_t added = ep->shots.size() - before;
                if (added > 0) {
                    emit(progress, "audio", "warn",
                         SAYN("有 %n 处台词一镜装不下，已拆成新的镜头。"
                              "这一章现在是 %1 个镜头",
                              static_cast<long long>(added),
                              std::to_string(ep->shots.size())));
                }

                // **配音把时长定下来之后，再平衡一次。**
                //
                // rebalance_durations 的注释写着「有台词的镜头不动，因为
                // 它们的时长是由配音定的」——那句话的前提就是配音已经跑过。
                // 而它原来只在分镜那一步调（planning / episodes / batch 三处），
                // 那时候还没配音，有台词的镜头时长还没锁定，那句话是空的。
                // **它被调在了错的时机。**
                //
                // 实测（walk_c ep01，目标 60 秒）：分镜排出来是 60 秒，配音
                // 把有台词的十镜从语音 30.6 秒撑到 48 秒——每一镜都要向上
                // 吸附到视频模型能生成的档位，光量化就多出 17.4 秒——整章
                // 变成 80 秒。超出的部分只能摊到那十个无台词的过渡镜上。
                //
                // **量的是成片长度，不是 planned_duration_s()。** 后者是
                // 分镜表上那串名义值的和；模型按格子出帧，名义 4 秒出来是
                // 4.458 秒。上一版这里用的就是名义值，于是 rebalance 报
                // 「已经压到 57 秒」而成片是 62.5 秒——压错了对象，见
                // stages::real_total_s。
                const int fps = settings.assembly.fps;
                const double before_s = stages::real_total_s(ep->shots, fps);
                // **不按目标时长挤。** 这一章多长由内容定，整部电影是最后把
                // 各章接起来，不在这儿也不在装配时切——在这儿把它压回一个
                // 目标尺寸，就是把用户说的「超了就压」从剧本挪到分镜。
                //
                // 2026-09-16 之前这儿是个 if/else：集模式那一边会跑
                // rebalance_durations 把整章压回目标时长。那条路当天删了，
                // else 那一整块（连同它那两句"压不到目标"的提示）跟着删。
                {
                    emit(progress, "audio", "info",
                         SAYF("这一章多长由它自己的内容定：按配音定下时长后"
                              "不再压回目标尺寸，这一章 %1",
                              util::human_time_precise_as(before_s, before_s)));
                }
                save();

                emit(progress, "audio", "done", stages::summarize(report.audio),
                     static_cast<int>(todo.size()),
                     static_cast<int>(todo.size()));
            }
        }

        // **跑全流程时成片档不吃 force，只有单跑这个阶段时才吃。**
        // 照抄 Python：run() 里给的是 force=False，run_stages() 里
        // 给的是 force=force。
        //
        // 差别在草稿失败的那几镜：force 重跑草稿之后它们状态没变，
        // 成片阶段跳过它们是对的——拿一个没渲出来的草稿去出成片，
        // 出来的是另一段没有首帧参考的片子，混在成片目录里最难发现。
        const bool force_final = opts.only.has_value() ? opts.force : false;
        // 流水里已经跟着首帧跑过的那一档，下面不再跑一遍
        bool did_draft = false;
        bool did_final = false;

        // ---- 首帧 ----
        if (wants(opts, Stage::Frames) && !tok.cancelled()) {
            // **入口状态只有 AUDIO_DONE。** 配音跑完锁了时长才出首帧——
            // 时长决定帧数，帧数决定这一镜的画面，顺序反过来的话
            // 配音出来装不进已经渲好的视频里。
            //
            // 阶段 5 时这里临时放宽收了 PLANNED（那会儿配音还没移植），
            // 现在配音接上了，收回来。配音失败的镜头状态停在 PLANNED，
            // 于是自动被挡在首帧之外——那是对的，它们带着错的时长。
            auto todo = pick_for_frames(*ep, store.paths(), opts.force,
                                        opts.only_shots);
            progress.set_pending(ids_of(todo), "frames");
            if (!todo.empty()) {
                ran = true;
                // **已经出过片的那几镜要说一声。**
                //
                // 它们会因为重出首帧退回 FRAME_DONE——那几条视频不是照这张
                // 新首帧生成的，留着状态等于说"这一镜是拿它生的"，不是实话。
                // 但这是**花钱的**（一镜几分钟），所以不能闷声干。
                int had_video = 0;
                for (const Shot* s : todo) {
                    if (s->video_path.has_value() && !s->video_path->empty()) {
                        ++had_video;
                    }
                }
                std::string head =
                    SAYN("给 %n 个镜头出首帧（%1）",
                         static_cast<long long>(todo.size()),
                         backends.frame_backend_name);
                if (had_video > 0) {
                    head += SAYF("。其中 %1 镜已经出过片了："
                                 "那几条视频不是照这张新首帧生成的，"
                                 "所以它们会退回「等出片」，要重新跑一次出片",
                                 std::to_string(had_video));
                }
                emit(progress, "frames", "start", head, 0,
                     static_cast<int>(todo.size()));

                // 首帧那批跑完之后说的话（首帧完成几个、失败几个、哪几个
                // 还留着旧首帧）。**流水时在首帧那条线程里说**，不然要等到
                // 整章出完片才轮到它，人看到的顺序是"成片完成"在"首帧完成"
                // 前面。读的是 todo 里各镜的 frame_path，而出片那层会整份
                // 写回 Shot——流水时调用方拿着 flow 的锁再叫它。
                const auto say_frames_done = [&] {

                    const int n = static_cast<int>(report.frames.size());
                    int failed = 0;
                    // **失败之后不一定就是纯文生视频。**
                    //
                    // 出首帧失败时只记 attempts，`frame_path` 原样留着（见
                    // stages/frames.cpp 的 apply）。只要那个旧文件还在，
                    // 出片阶段照样会拿它当起点——于是这一句"会退回纯文生视频"
                    // 是错的，而错得很隐蔽：用户以为这几镜是文生的，实际是
                    // 拿一张**上一次的、可能还是另一个画幅的**图生出来的。
                    // 2026-09-13 就是这么撞上的：项目从 720p 改成 hd 之后，
                    // 首帧全部失败，成片却拿 544×928 的旧图生出了 704×1280。
                    int stale = 0;
                    for (const auto& o : report.frames) {
                        if (o.ok) continue;
                        ++failed;
                        const auto it = std::find_if(
                            todo.begin(), todo.end(), [&](const models::Shot* s) {
                                return s->shot_id == o.shot_id;
                            });
                        if (it == todo.end()) continue;
                        const models::Shot* s = *it;
                        if (!s->frame_path.has_value() || s->frame_path->empty()) {
                            continue;
                        }
                        std::error_code ec;
                        if (fs::is_regular_file(store.paths().abs(*s->frame_path),
                                                ec)) {
                            ++stale;
                        }
                    }
                    std::string done_msg =
                        SAYF("首帧完成 %1 个", std::to_string(n - failed));
                    if (failed > 0) {
                        done_msg += SAYF("，失败 %1 个", std::to_string(failed));
                        if (stale > 0) {
                            done_msg += SAYF(
                                "。其中 %1 个还留着上一次出的首帧，"
                                "出片会直接拿它当起点"
                                "——如果这中间改过画幅或者改过画面描述，出来的"
                                "东西不是你现在要的，先把这几镜的首帧重出一遍",
                                std::to_string(stale));
                        }
                        if (failed > stale) {
                            done_msg += SAYF("。另外 %1 个没有可用的首帧，"
                                             "会退回纯文生视频",
                                             std::to_string(failed - stale));
                        }
                    }
                    emit(progress, "frames", "done", done_msg, n, n);
                };

                // ---- 首帧和出片同时开（流水）----
                //
                // 见 pipeline/shot_flow.hpp 开头。哪一档跟着首帧走：要草稿就是
                // 草稿档，不要草稿就是成片档；两档都要时成片档仍在草稿之后
                // 串着跑（它收的是 DRAFT_DONE，得等草稿落定）。只出首帧
                //（skip 两档）时没有流水，和以前一样。
                //
                // **只有池里不止一个位置时才流水。** 单卡单进程时按阶段分批
                // 只在图像模型和视频模型之间切 2 次，逐镜交错要切 2N 次，
                // 而那台 6GB 的机器会把绝大部分时间花在加载模型上（用例
                // 「阶段之间分批」钉的就是这一条）；一个位置也没有第二张
                // 卡可填，流水没有任何收益。多个位置时才是"最后几张首帧在
                // 跑、别的卡闲着"这种局面。
                const bool flow_worth = backends.render_lanes > 1;
                const std::optional<Tier> flow_tier =
                    !flow_worth ? std::nullopt
                    : (!opts.skip_draft && wants(opts, Stage::Draft))
                        ? std::optional<Tier>(Tier::DRAFT)
                    : (!opts.skip_final && wants(opts, Stage::Final))
                        ? std::optional<Tier>(Tier::FINAL)
                        : std::nullopt;
                if (flow_tier && !tok.cancelled()) {
                    std::set<std::string> waiting;
                    for (const Shot* s : todo) waiting.insert(s->shot_id);
                    pipeline::ShotFlow flow(std::move(waiting));
                    std::exception_ptr frames_err;
                    std::thread frames_thread([&] {
                        try {
                            report.frames = stages::run_frames(
                                todo, assets, frame_spec(profile, settings),
                                store.paths(), backends.frame, progress, tok,
                                backends.render_lanes, save, &flow);
                            std::lock_guard<std::mutex> lg(flow.commit_mutex());
                            save();
                            say_frames_done();
                        } catch (...) {
                            frames_err = std::current_exception();
                        }
                        // 正常、取消、抛异常都要放行，别让出片那层吊着
                        flow.close();
                    });
                    std::vector<stages::RenderOutcome> outs;
                    try {
                        outs = render_tier(*flow_tier,
                                           *flow_tier == Tier::FINAL ? force_final
                                                                     : opts.force,
                                           &flow, todo);
                    } catch (...) {
                        frames_thread.join();
                        throw;
                    }
                    frames_thread.join();
                    if (frames_err) std::rethrow_exception(frames_err);
                    if (*flow_tier == Tier::FINAL) {
                        report.final_ = std::move(outs);
                        did_final = true;
                    } else {
                        report.draft = std::move(outs);
                        did_draft = true;
                    }
                    save();
                } else {
                report.frames = stages::run_frames(
                    todo, assets, frame_spec(profile, settings),
                    store.paths(), backends.frame, progress, tok,
                    // 同时跑几镜。**没有池就是 1**，行为和以前一样。
                    // 有池就取池的大小——这一层不知道有几张卡，
                    // 但它知道池里有几个工作进程。
                    backends.render_lanes,
                    // 每出完一张就存一次。见 pipeline::ShotCommit——
                    // 不存的话这一批跑完之前镜头墙上一张缩略图都没有。
                    save);
                save();
                    say_frames_done();
                }
            }
        }

        // ---- 草稿档 ----
        //
        // skip_draft：两档拉不开差距时它就是白跑一遍（挂 Turbo LoRA 之后
        // 正是这个局面）。跳过之后成片档会收 FRAME_DONE 那批，
        // 见 render_entry_states。
        if (!did_draft && !opts.skip_draft && wants(opts, Stage::Draft) &&
            !tok.cancelled()) {
            report.draft = render_tier(Tier::DRAFT, opts.force);
            save();
        }

        // ---- 成片档 ----
        //
        // skip_final 是为了快速验证叙事：成片档一个镜头几分钟，
        // 而叙事对不对看草稿就够了。
        if (!did_final && !opts.skip_final && wants(opts, Stage::Final) &&
            !tok.cancelled()) {
            report.final_ = render_tier(Tier::FINAL, force_final);
            save();
        }

        // ---- 装配 ----
        if (wants(opts, Stage::Assemble) && !tok.cancelled()) {
            if (!backends.ffmpeg.has_value()) {
                // 没装 ffmpeg 时前面几步照样跑完了。**跑到最后一步才说
                // 缺 ffmpeg 最气人**，所以这里说清楚缺的是什么、产物在哪。
                emit(progress, "assemble", "warn",
                     SAY("没有 ffmpeg，跳过装配。各镜头的视频已经在 shots/ 下，"
                         "装好之后单跑 assemble 阶段即可"));
            } else {
                report.output = assemble_episode(store, settings, *ep,
                                             *backends.ffmpeg, progress);
                ran = true;   // 见末尾那句「全部跳过」
            }
        }
    } catch (const std::exception& e) {
        report.errors.push_back(e.what());
        emit(progress, "assemble", "error", e.what());
    }

    // **什么都没跑到才说一声。** 以前是每一段各说一句「配音已完成，跳过」
    // 「首帧已完成，跳过」——只出首帧时顶栏先冒出来的是「配音已完成」，
    // 用户问"配音完成是什么鬼"（2026-09-15）。跑了东西的，那些东西自己
    // 会说话；一样都没跑的，才需要这一句，不然看着像"点了开始，立刻就完成
    // 了"，分不清是续跑跳过了还是根本没跑起来。
    if (!ran && !tok.cancelled() && report.errors.empty()) {
        emit(progress, last_stage_name(opts), "done",
             SAY("这一章要的都已经出好了，这次没有要跑的，全部跳过"));
    }

    // 对齐 Python 的 finally：无论成功、失败还是中途停止都存一次。
    // 上面各阶段已经存过了，这里再存一次是为了兜住"还没跑到任何一个
    // 阶段就被取消"和"阶段中间抛异常"这两种情况——那时候镜头的
    // attempts 可能已经加过了，不存的话下次重跑的重试计数是错的。
    try {
        save();
    } catch (const std::exception& e) {
        // 存不进去也要说，但不能盖掉真正的错误——所以是追加不是替换。
        report.errors.push_back(SAYF("存盘失败：%1", e.what()));
    }

    report.elapsed_s = now_seconds() - started;
    return report;
}

}  // namespace changji::pipeline
