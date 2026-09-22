#pragma once

// 跑一章：按阶段把镜头过一遍。
//
// **阶段之间是分批的，不是按镜头串行的。** 四十个镜头的首帧全出完，
// 再统一出视频。这不是风格问题——预算只装得下一个模型时，
// 按镜头串行是加载 80 次，按阶段分批是 2 次。
// test_scheduler 里有一条用例专门钉这个差距。

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "media/ffmpeg.hpp"
#include "stages/audio.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"

namespace changji::pipeline {

/// 流水线的阶段。**也是断点续跑的锚点**——每个阶段跑完就存盘，
/// 中途断了下次从没跑完的那个阶段接着来。
enum class Stage {
    Audio,      ///< 配音。**在生成任何画面之前跑完**——它决定镜头时长
    Frames,     ///< 逐镜首帧
    Draft,      ///< 草稿档视频
    Final,      ///< 成片档视频
    Assemble,   ///< 拼成这一章的成片
};

const char* to_string(Stage s);
/// 认不出返回 false。
bool stage_from_string(const std::string& s, Stage& out);

/// 跑一章的结果。
struct RunReport {
    std::string episode_id;
    std::vector<stages::ShotAudioPlan> audio;
    std::vector<stages::FrameOutcome> frames;
    std::vector<stages::RenderOutcome> draft;
    std::vector<stages::RenderOutcome> final_;
    /// 成片的路径。没跑装配阶段时是空的。
    std::optional<std::string> output;
    std::vector<std::string> errors;
    double elapsed_s = 0.0;

    bool ok() const { return errors.empty(); }
};

/// 跑一章要用的东西。
struct RunOptions {
    std::string episode_id;
    /// 只跑草稿档就停。快速验证叙事时用——成片档一个镜头几分钟，
    /// 而叙事对不对看草稿就够了。
    bool skip_final = false;
    /// 跳过草稿档，首帧出完直接上成片档。
    ///
    /// **两档画质拉不开差距的时候，草稿档就是白跑一遍。** 挂 Turbo LoRA
    /// 之后就是这个局面：草稿 6 步、成片 6 步，同一个分辨率，出来几乎一样，
    /// 而每镜多花两分钟。用户 2026-09-10 的原话："草稿档和成片档一样，
    /// 现在根本不需要草稿档了还浪费时间"。
    ///
    /// 跳过之后成片档要收 FRAME_DONE 那批镜头——它们没经过草稿档，
    /// 状态停在首帧完成。这一条在 render_entry_states 里。
    /// **这一层默认假，接口层默认真。** 不是矛盾：
    ///
    /// RunOptions 是"跑哪些阶段"的完整描述，默认值该是"都跑"——
    /// 直接用这个结构体的（测试、将来的批处理工具）想要哪一段自己说。
    /// 而 `POST /api/run` 面对的是人，人现在不需要草稿档：挂 Turbo LoRA
    /// 之后两档画质拉不开差距，那一遍就是白跑（一章 54 分钟）。
    ///
    /// 把"人不需要"翻译成结构体默认值的话，波及的是所有直接调用方——
    /// 14 个测试用例当场变红，而它们测的正是草稿档那条路，没有一个是错的。
    bool skip_draft = false;

    /// 只跑这几个镜头。**空表示整章。**
    ///
    /// 新界面上每个镜头自己有一个"重新生成"按钮——用户看着某一镜不对，
    /// 想重跑的就是那一个。没有这一项的话只能整章重跑，而整章是一小时。
    ///
    /// 和 `force` 是两回事：这一项管"跑哪几个"，force 管"已完成的要不要
    /// 重来"。重跑单镜通常两个都要给。
    std::set<std::string> only_shots;
    /// 无视状态，全部重跑。
    bool force = false;
    /// 只做这一章的前 n 秒（0 = 整章，老行为）。
    ///
    /// **这一层只是把那个数带下去，真正的编排在 pipeline/preview.hpp。**
    /// `run_episode` 本身不认它——那儿是"按阶段把镜头过一遍"，而前 n 分钟
    /// 要的是"配音之后重挑一次、跑完量一次、不够再来一轮"，那是外面一层的
    /// 事。`run_preview` 拿它挑前缀，再把前缀写进 `only_shots` 调这儿。
    double preview_s = 0.0;

    /// 只跑这几个阶段。
    ///
    /// **没给（nullopt）表示跑全流程，给了一个空的表示一个阶段都不跑。**
    /// 这两者不是一回事，而且都是前端能触发的：Python 那边判的是
    /// `if req.stages:`，所以 `stages: []` 走全流程（空列表是假值），
    /// `stages: ["  "]` 走"只跑指定阶段"、而指定的阶段过滤完是空的。
    /// 合成一个空 vector 的话，后一种会变成把整章重跑一遍。
    std::optional<std::vector<Stage>> only;
};

/// 出图和出片的后端。注入的，理由同各阶段：
/// 真跑一章要几十分钟，而阶段编排的逻辑（谁先谁后、哪些镜头要跑、
/// 什么时候存盘）能在几毫秒内测死。
struct Backends {
    stages::FrameRenderer frame;
    stages::VideoRenderer video;
    /// 配音。留空就用估算后端——只算时长不出声音，
    /// 成片是静音的，但整条流水线能跑通。
    std::optional<stages::TTSBackend> tts;
    /// 装配和闸门要用。留空就跳过装配阶段——没装 ffmpeg 的机器上
    /// 前面几步照样能跑，而"跑到最后一步才说缺 ffmpeg"最气人。
    std::optional<media::FFmpeg> ffmpeg;
    /// 出首帧的后端叫什么，只用来拼那句开始的话。
    ///
    /// Python 那边这句里带后端名是有用的：它有三条路
    /// （image_model / video_model / comfy），传了参考图但走的是视频模型
    /// 的话那些图一张都不会被用上，不写出来用户只会以为是模型不听话。
    /// 出图出片同时跑几镜。**1 = 逐镜串行，和以前一模一样。**
    ///
    /// 进程内那条路只能是 1：sd.cpp 的进度回调是全局的，同进程两个生成
    /// 会互相串。走工作进程池时取池的大小——那时候每个生成在各自的进程、
    /// 各自的卡上，互不干扰。
    int render_lanes = 1;

    /// 配音同时跑几镜。**1 = 逐镜串行**（本机那条 TTS 就是 1：一个进程
    /// 一份模型，并发只会让每句都变慢）。走别人家的池时取池的位置数——
    /// 一台双卡机报两个位置，串行就是空着一张卡（2026-09-17 用户报的）。
    int audio_lanes = 1;

    std::string frame_backend_name = "sd.cpp";

    /// 只是让某些东西活到渲染结束。
    ///
    /// **`frame` 和 `video` 是 std::function，捕获的是裸指针**——
    /// 工作进程池那种"造出来交给它俩用"的东西，没有这个的话
    /// 出了 `backends()` 那个作用域就析构了，而 lambda 还留着指针。
    /// 表现是跑到第一镜就崩，且崩在池的析构里，看不出和配置有什么关系。
    std::vector<std::shared_ptr<void>> keepalive;
};

/// 跑一章。
///
/// 每个阶段结束后**立刻存盘**。不存的话中途断电或者点了停止，
/// 前面几十分钟的产出全部作废——文件还在磁盘上，但项目文件里没记，
/// 下次跑会当成没跑过。
RunReport run_episode(const models::ProjectStore& store,
                      const models::HardwareProfile& profile,
                      const config::Settings& settings, const RunOptions& opts,
                      const Backends& backends, JobProgress& progress,
                      CancelToken& tok);

/// 挑出这一阶段要跑的镜头。按 `order` 排，不按在数组里的位置。
///
/// **挑漏一个状态是最难查的一类错**：那一镜永远轮不到它——不报错、
/// 不重试、日志里一行都没有，表现是"成片里少了一个镜头"，
/// 而你会先怀疑分镜、怀疑渲染、怀疑装配。
/// 所以它和下面那个不留在匿名 namespace 里，好让语料够得着。
/// `only_shots` 非空时只认这几个镜头（按 shot_id）。**先筛这一层**：
/// 不筛的话 force 会把整章都拉进来，而用户点的是某一镜的"重新生成"。
std::vector<models::Shot*> pick(models::Episode& ep,
                                const std::set<models::ShotStatus>& want,
                                bool force,
                                const std::set<std::string>& only_shots = {});

/// 出首帧这一段要跑哪些镜头。
///
/// **不能只按状态挑。** 入口状态是 `AUDIO_DONE`（时长锁死了才能定帧数），
/// 可界面上那个「只出首帧（差 N）」按钮是按**有没有首帧文件**数的，
/// 两边对不上：一镜首帧失败之后照样会被出片那一段收走（走纯文生视频），
/// 于是它停在 `FINAL_DONE` / `FALLBACK` 而手里一张首帧都没有——
/// 界面把它算进「差 N」，引擎一个都挑不到，**点了跑完还是差 N，
/// 而且一句话都没有**。
///
/// 2026-09-13 全盘数过：203 个缺首帧的镜头里，12 个停在 `FINAL_DONE`、
/// 11 个停在 `FALLBACK`，都够不着。（`PLANNED` 那 202 个不在此列——
/// 按钮发的是 `["audio","frames"]`，配音那一段会把它们推到 `AUDIO_DONE`，
/// 同一轮里首帧就挑得到了。）
///
/// 所以判据是两条：
///   * `AUDIO_DONE` —— 正常流程里的入口，照旧；
///   * 已经过了配音、又**没有能用的首帧文件**的——`PLANNED` 除外
///     （它的时长还没锁，带着错的时长去出首帧是另一种错），
///     `LOCKED` 除外（人工锁定的不动）。
///
/// 「能用」是连磁盘一起看的：`frame_path` 记着而文件不在，一样要重出。
///
/// **重出首帧会让已经出过片的那几镜退回 `FRAME_DONE`**，因为那几条视频
/// 不是照这张新首帧生成的。这不是新行为——「重出首帧」那条路（force）
/// 本来就是这样；这里只是让不带 force 的那条和它一致。调用方要把这件事
/// 说给用户听，见 run_episode 里出首帧那一段的 start 消息。
std::vector<models::Shot*> pick_for_frames(
    models::Episode& ep, const models::ProjectPaths& paths, bool force,
    const std::set<std::string>& only_shots = {});

/// 这一镜能不能进成片：既出了片（video_path 有值）又过了闸门。
///
/// **导出来是为了让它只有一份。** 同一条判据有两个用处——挑谁进成片、
/// 告诉人谁没进去——两处各写一遍的话迟早对不上，而对不上的表现是
/// 「装配 16 个镜头」后面跟着一句「没进去的：（空）」。
bool assembly_usable(const models::Shot& s);

/// 没能进成片的镜头，一镜一行：`ep01_sh001：配音完成，还没出片`。
/// 全都能进就返回空表。
///
/// 装配那道筛选本来是**静默**的：不可用的镜头直接不进成片，唯一的线索是
/// 「装配 16 个镜头」这个数，人得自己记得这一章有 18 镜才看得出来。
/// 2026-09-13 实机撞到：walk_c ep01 两镜因为重跑过配音退回 audio_done，
/// 成片从 18 镜 61.8 秒变成 16 镜 54.3 秒，而消息一个字都没提。
/// **镜号单列一格。** 原来只回一行拼好的话（`ep01_sh003：配音完成，还没
/// 出片`），过滤"只做前 n 分钟"那一步要**回头去找那个全角冒号**把镜号切
/// 出来——那句话翻了之后冒号就不是「：」了，切出来的是整行，于是过滤全落空。
/// 靠一句给人看的话回推结构，多语言下必错；镜号跟着一起回来就没这回事。
struct LeftOut {
    std::string shot_id;
    std::string line;   ///< 给人看的那一整句
};

std::vector<LeftOut> assembly_left_out(const models::Episode& ep);

/// 装配的可选项。**默认值 = 这一章的正片**，和以前逐字节一样。
///
/// 加这一层是为了让「只做前 n 分钟」那条（pipeline/preview.hpp）用**同一个**
/// 装配，而不是抄一份。抄一份的坏法是不报错的那种：闸门自检、字幕自检、
/// 降级点名、响度——正片这边后来加的每一样，预告那边都会悄悄少一样。
struct AssembleOptions {
    /// 只装这几镜（空 = 这一章所有能用的）。**位置和顺序照旧按 `order`**，
    /// 这里只是把别的镜头滤掉。
    std::set<std::string> only_shots;
    /// 输出文件名，相对 `output/`。空 = `<episode_id>.mp4`。
    ///
    /// **可以带子目录**（`preview/ep01.mp4`）：字幕会跟着落到 `subtitles/`
    /// 下同样的子目录里（见 media::Assembler::assemble）。预告靠这条既躲开
    /// `media::episode_of_output` 那张认领表，又不会拿 `ep01.ass` 盖掉正片
    /// 的字幕——两份字幕同名，播放器挂上的会是后写的那一份。
    std::string out_name;
    /// 配乐文件的后缀（`stages::music_path_for` 的那个）。
    ///
    /// **预告必须给一个自己的。** 配乐是"文件在就沿用"，而 ffmpeg 那头短了
    /// 不循环：预告先出一条两分钟的 `ep01_music.wav`，整章再跑就沿用它，
    /// 后面几分钟**静悄悄没有配乐**。
    std::string music_suffix;
    /// 清掉这一章留在 `output/` 顶层的孤儿成片。预告那条不清：它根本不在
    /// 顶层，而那段清理认的是正片的命名。
    bool sweep_orphans = true;
    /// 装出来多长（秒）。预告要拿它判"够不够 n 分钟"——判据落在**装出来
    /// 的长度**上，不落在计划长度上。
    double* total_s = nullptr;
};

/// 装配成片。回成片的路径。
///
/// 一镜都装不了时抛 `std::runtime_error`（"没有可装配的镜头"）。
std::string assemble_episode(const models::ProjectStore& store,
                             const config::Settings& settings,
                             models::Episode& ep, const media::FFmpeg& ff,
                             JobProgress& progress,
                             const AssembleOptions& opts = {});

/// 一个档位的入口状态。
///
/// 草稿档收 `AUDIO_DONE` 是关键：首帧失败的镜头状态停在那里，
/// 不收的话它永远出不了片；收了就退回纯文生视频。
/// `skip_draft` 为真时成片档还要收 FRAME_DONE 那批——它们没走草稿档。
std::set<models::ShotStatus> render_entry_states(models::Tier tier,
                                                 bool skip_draft = false);

/// 首帧该按什么规格出。
///
/// 两件事在这儿汇到一起，各自都**不会报错**、只会让成片"看着不太行"：
///
/// 1. **档位**：`[models].frame_tier`，默认 `final`。画幅（`[video]`）
///    只盖成片档，首帧走草稿档就会拿 512×288 去配 704×1280 的视频。
/// 2. **步数**：成片档的步数在挂了 Turbo LoRA 之后会被压到 6 步，
///    但那个 LoRA 只挂在**视频**模型上。首帧要用挂 LoRA 之前的步数，
///    也就是 `[models].frame_steps`（run.cpp 在压步数时顺手存下来的，
///    用户也能自己填）。
///
/// 单拎出来是为了能测：这两条错了都得盯着出来的图才看得见。
models::TierSpec frame_spec(const models::HardwareProfile& profile,
                            const config::Settings& settings);

/// 把**这部电影自己的** changji.toml 盖进档位表，顺手把首帧步数填进 settings。
///
/// 档位表是按这张卡的显存推出来的（5090 上是 1920×1088 / 30 步），而画幅是
/// 这部电影的属性，写在项目目录的 `[video]` 里。不过这一道的话，出来的图是
/// 1088×1920 而这部电影的每一镜是 544×928——**不报任何错**，只是慢一倍，
/// 而且参考图和成片对不上。
///
/// 步数同理：`[tiers].final_steps` 那个数假设的是不挂 Turbo 的模型，挂了
/// 就该压到 6 步；但那个 LoRA 只挂在视频模型上，首帧要用压之前的步数。
/// 两件事都在 `config::effective_spec` 里，这里只负责盖回去。
///
/// **凡是要出图的路径都得先过这一道**：跑流水线是一条，设定页出参考图是
/// 另一条。各写一遍的话两条路会分叉，而分叉的表现是"参考图和成片长得不
/// 一样"，没有任何报错。
void apply_project_spec(config::Settings& settings,
                        models::HardwareProfile& profile);

}  // namespace changji::pipeline
