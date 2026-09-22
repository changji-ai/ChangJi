#include "util/say.hpp"
#include "infer/sd_video.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>

#include "infer/local_exec.hpp"
#include "infer/scheduler.hpp"
#include "pipeline/activity.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

namespace fs = std::filesystem;

namespace changji::infer {

namespace {

/// 临时的裸帧文件。放在目标旁边而不是系统临时目录：
/// 121 帧 448x768 的 RGB 是 125 MB，系统盘可能没那么多空闲，
/// 而项目目录所在的盘本来就要装成片。
fs::path raw_temp_for(const fs::path& dest) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream os;
    os << ".changji_raw_" << std::hex << rng() << ".rgb";
    return dest.parent_path() / paths::from_utf8(os.str());
}

/// 用完就删。异常路径上也要删——125 MB 的垃圾留在项目目录里，
/// 用户下次看到的是"我的项目怎么这么大"。
struct TempFile {
    fs::path path;
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

}  // namespace

std::vector<std::string> encode_args(const fs::path& raw_path, int width,
                                     int height, int fps,
                                     const config::AssemblyConfig& assembly,
                                     const fs::path& dest,
                                     const std::optional<fs::path>& audio,
                                     double duration_s) {
    std::vector<std::string> args = {
        "-y",                       // 覆盖。重跑一镜时不该卡在"要覆盖吗"上
        "-f", "rawvideo",
        "-pixel_format", "rgb24",   // sd.cpp 吐的就是 RGB24
        "-video_size", std::to_string(width) + "x" + std::to_string(height),
        "-framerate", std::to_string(fps),
        "-i", paths::to_utf8(raw_path),
    };
    if (audio.has_value()) {
        // 模型自己出的原生音轨（H3 的环境声和动效）。以前这里是 `-an`，
        // 那条声音在这一步被原地丢掉，成片里台词之间是数字静音。
        // 装配那边按 [sound].ambient 决定用不用它——这里只负责别丢。
        args.insert(args.end(), {"-i", paths::to_utf8(*audio)});
    }
    args.insert(args.end(), {
        "-c:v", assembly.video_codec,
        "-crf", std::to_string(assembly.crf),
        // **像素格式必须显式给。** 不给的话 ffmpeg 会保留 rgb24，
        // 而 H.264 的 rgb24 变体一大半播放器打不开，包括浏览器里的
        // <video>——而审片就是在浏览器里做的。
        "-pix_fmt", assembly.pix_fmt,
        // 拼接环节要求各镜头规格一致。这里和成片用同一套参数，
        // 拼的时候才能直接 concat 而不是重编码——重编码是白白多一次有损压缩。
        "-r", std::to_string(fps),
    });
    if (audio.has_value()) {
        args.insert(args.end(), {
            "-c:a", assembly.audio_codec,
            "-b:a", assembly.audio_bitrate,
            "-ar", std::to_string(assembly.audio_sample_rate),
            "-ac", std::to_string(assembly.audio_channels),
        });
        // 按画面截齐，**不用 -shortest**：那个会在音轨略短时把画面也截掉。
        if (duration_s > 0.0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", duration_s);
            args.insert(args.end(), {"-t", buf});
        }
    } else {
        args.push_back("-an");      // 没有声音就明说没有，别让 ffmpeg 猜
    }
    args.push_back(paths::to_utf8(dest));
    return args;
}

void encode_raw_to_mp4(const fs::path& raw_path, int width, int height, int fps,
                       const config::AssemblyConfig& assembly,
                       const fs::path& dest,
                       const std::optional<fs::path>& audio,
                       double duration_s) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    const auto exe = proc::which(assembly.ffmpeg_path);
    if (!exe.has_value()) {
        throw SdError(SAYF("找不到 ffmpeg（%1）。装配和出片都要它，"
                           "装好之后在配置里填 assembly.ffmpeg_path",
                           assembly.ffmpeg_path));
    }

    // 不限时。一镜的编码在低配机器上可能要几十秒，
    // 而超时把它杀掉留下的是一个半截的 mp4——比慢更糟。
    const proc::Result r = proc::run(
        *exe,
        encode_args(raw_path, width, height, fps, assembly, dest, audio,
                    duration_s),
        0);
    if (!r.launched || r.exit_code != 0) {
        throw SdError(SAYF("ffmpeg 编码失败（退出码 %1）：\n%2",
                           std::to_string(r.exit_code), r.out));
    }
    if (!fs::is_regular_file(dest, ec)) {
        throw SdError(
            SAYF("ffmpeg 说成功了但没有产出文件：%1", paths::to_utf8(dest)));
    }
}

namespace {

/// 出片那一段的公共实现。`seed_override` 有值就用它。
stages::VideoRenderer make_video_renderer(
    const config::Settings& settings,
    std::optional<std::int64_t> seed_override, Origin origin) {
    const config::AssemblyConfig assembly = settings.assembly;
    const std::string lora_tiers = settings.models.video_lora_tiers;
    // **这一份是「这一章」的设置**（出片每跑一章都 load_settings(项目目录)
    // 重读一遍），而建 SD 上下文用的是全局那份。采样旋钮跟着请求走才对得上，
    // 见 SamplingKnobs。
    const SamplingKnobs knobs = sampling_knobs_for(settings, ModelRole::Video);
    // 留不留模型自己出的声音是这部电影的属性（[sound].ambient）。计划里带了
    // （派活方那部电影的设置）就按计划的，没带按本机的。
    const bool keep_ambient_default = settings.sound.ambient;
    // 合并时两边各往这个 lambda 里加了捕获，都要：knobs / keep_ambient 是
    // 「这一章」那一份设置，origin 是"这活谁派的"（见下面 local_exec().enter）。
    return [assembly, seed_override, lora_tiers, origin, knobs, keep_ambient_default](
               const models::Shot& shot, const stages::RenderPlan& plan,
                      const std::optional<fs::path>& start_image,
                      const fs::path& dest, pipeline::CancelToken& tok,
                      const StepCallback& on_step) {
        // 先拿执行位再借显存槽，同 frames.cpp。**出图和出片是两个
        // SdContext、两把各自的 run_mu，互相挡不住**，而 sd.cpp 的进度
        // 回调是全局的——真同时跑起来，两边的步数会串到一起。
        auto hold = local_exec().enter(origin, pipeline::note_queued, &tok);
        // 每一镜借一次视频槽。跨阶段的显存回收由调度器决定。
        // 同 frames.cpp：借之前先说一句。视频模型更大，卸大模型 + 读盘
        // 这一段更长，而 sd.cpp 的进度回调要等它跑起来才有。
        if (!scheduler().loaded(Slot::Video)) {
            on_step(0, 0, 0.0, Phase::Prep);
        }
        // **借之前先说这一镜多大。** 画幅从 544×928 到 2560×1440 差七倍多，
        // 帧数也不一样，而以前量到的显存是不带这个的——在 720p 量到的数
        // 拿去给 2K 判"够，不卸"，赌输了就是 CUDA OOM。
        // 见 Scheduler::record_measured_vram。
        // 借不到就排队等。成片是最贵的一步，为了"另一边正在写字"整镜
        // 报错，等于把已经跑完的前几步全扔了。
        Scheduler::AcquireOptions opt;
        opt.work = static_cast<std::size_t>(plan.spec.width) * plan.spec.height *
                   std::max(1, plan.frames);
        opt.wait = kAcquireWait;
        opt.on_queued = pipeline::note_queued;
        auto lease = scheduler().acquire(Slot::Video, opt);
        auto ctx = current_video_context();
        if (!ctx) throw SdError(SAY("出视频上下文没准备好"));

        VideoRequest req;
        req.positive = stages::video_positive(plan);
        req.negative = plan.prompts.negative_video;
        req.width = plan.spec.width;
        req.height = plan.spec.height;
        req.steps = plan.spec.steps;
        req.frames = plan.frames;
        req.tag = shot.shot_id;   // 预览挂到哪一格，见 VideoRequest::tag

        req.fps = assembly.fps;
        req.knobs = knobs;
        req.seed = seed_override
                       ? *seed_override
                       : stages::render_seed(shot.shot_id, shot.attempts);
        req.start_image = start_image;
        req.end_image = plan.end_image;
        // LoRA 按档位挂。上下文是草稿和成片共用的，所以这个决定只能
        // 落在每次请求上——建上下文的时候还不知道这一镜跑哪一档。
        const std::string& tiers = lora_tiers;
        req.use_lora =
            tiers == "both" ||
            (tiers == "draft" && plan.spec.tier == models::Tier::DRAFT) ||
            (tiers == "final" && plan.spec.tier == models::Tier::FINAL);

        const fs::path raw = raw_temp_for(dest);
        TempFile guard{raw};
        // 声音那份 wav 和裸帧同名不同后缀，用完一起删。
        const fs::path wav = audio_path_for(raw);
        TempFile wguard{wav};
        ctx->generate_video(req, raw, tok, on_step);
        std::optional<fs::path> audio;
        std::error_code ec;
        const bool keep_ambient = plan.keep_ambient.value_or(keep_ambient_default);
        if (keep_ambient && fs::is_regular_file(wav, ec)) audio = wav;
        encode_raw_to_mp4(raw, req.width, req.height, req.fps, assembly, dest,
                          audio,
                          static_cast<double>(req.frames) /
                              static_cast<double>(std::max(1, req.fps)));
    };
}

}  // namespace

stages::VideoRenderer sd_video_renderer(const config::Settings& settings) {
    return make_video_renderer(settings, std::nullopt, Origin::Local);
}

stages::VideoRenderer sd_video_renderer_with_seed(
    const config::Settings& settings, std::int64_t seed, Origin origin) {
    return make_video_renderer(settings, seed, origin);
}

}  // namespace changji::infer
