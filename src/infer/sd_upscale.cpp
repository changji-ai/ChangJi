#include "util/say.hpp"
#include "infer/sd_upscale.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"
#include "util/say.hpp"

#ifdef CHANGJI_HAVE_SD
#include "stable-diffusion.h"
#endif

namespace fs = std::filesystem;

namespace changji::infer {

namespace {

/// 用 ffprobe 问一段视频的宽高帧率。
struct Probe {
    int width = 0;
    int height = 0;
    int fps = 24;
};

Probe probe_video(const fs::path& in, const config::AssemblyConfig& assembly) {
    const auto exe = proc::which("ffprobe");
    if (!exe.has_value()) {
        throw SdError(SAY("找不到 ffprobe。超分要先问出这段视频的宽高帧率"));
    }
    const proc::Result r = proc::run(
        *exe,
        {"-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,r_frame_rate", "-of", "csv=p=0",
         paths::to_utf8(in)},
        // **单位是毫秒，不是秒。** 写 60 的话 ffprobe 在 60 毫秒里跑不完，
        // 被杀掉之后 r.out 是空的，报的错是"读不出视频信息："后面什么都没有
        // ——指向完全错的地方。
        60000);
    if (!r.launched || r.exit_code != 0) {
        throw SdError(SAYF("ffprobe 读不出视频信息：%1", r.out));
    }
    Probe p;
    p.fps = assembly.fps;
    // 形如 "960,544,24/1"
    int w = 0, h = 0, num = 0, den = 1;
    if (std::sscanf(r.out.c_str(), "%d,%d,%d/%d", &w, &h, &num, &den) >= 2) {
        p.width = w;
        p.height = h;
        if (num > 0 && den > 0) p.fps = num / den;
    }
    if (p.width <= 0 || p.height <= 0) {
        throw SdError(SAYF("ffprobe 给的宽高不对：%1", r.out));
    }
    return p;
}

/// 把视频解成一整块 rgb24 裸数据。
///
/// 一段三秒的 960×544 是 73 帧 × 1.5 MB ≈ 115 MB，整块读进来没问题；
/// 换成成片长度的片子要改成流式。
std::vector<unsigned char> decode_to_raw(const fs::path& in,
                                        const config::AssemblyConfig& assembly,
                                        const fs::path& tmp) {
    const auto exe = proc::which(assembly.ffmpeg_path);
    if (!exe.has_value()) throw SdError(SAY("找不到 ffmpeg"));
    const proc::Result r = proc::run(
        *exe,
        {"-y", "-i", paths::to_utf8(in), "-f", "rawvideo", "-pix_fmt", "rgb24",
         paths::to_utf8(tmp)},
        0);
    if (!r.launched || r.exit_code != 0) {
        throw SdError(SAYF("ffmpeg 解码失败：%1", r.out));
    }
    std::ifstream f(tmp, std::ios::binary);
    if (!f) throw SdError(SAYF("读不了解出来的裸数据：%1", paths::to_utf8(tmp)));
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

}  // namespace

std::vector<std::string> encode_args(const fs::path& raw_up, int big_w,
                                     int big_h, const fs::path& src,
                                     int out_w, int out_h, int fps,
                                     const config::AssemblyConfig& assembly,
                                     const fs::path& out) {
    return {
        "-y",
        // 0 号输入：放大后的裸帧。裸数据不带宽高帧率，都要在这里补上。
        "-f", "rawvideo",
        "-pixel_format", "rgb24",
        "-video_size", std::to_string(big_w) + "x" + std::to_string(big_h),
        "-framerate", std::to_string(fps),
        "-i", paths::to_utf8(raw_up),
        // 1 号输入：原片。**只为了把它的音轨带过来。**
        //
        // 上一版这里写的是 `-an`，出来的是一段哑片。而 doctor 里那句
        // "要 2K 就出完再跑 changji --upscale" 指的正是**成片**——按它
        // 做一遍，配音和音效全没了，而且中间一句提示都没有。
        //
        // 和烧字幕那条（assemble.cpp 的 burn_args）一个道理：这一步只
        // 碰画面，音轨原样拷贝就行，重编码一次是白白多一次有损压缩。
        //
        // `1:a:0?` 末尾那个问号是"没有就算了"——分镜级的片段本来就
        // 可能没有音轨，不能因此整条命令失败。
        "-i", paths::to_utf8(src),
        "-map", "0:v:0",
        "-map", "1:a:0?",
        "-vf",
        "scale=" + std::to_string(out_w) + ":" + std::to_string(out_h) +
            ":flags=lanczos",
        "-c:v", assembly.video_codec,
        "-crf", std::to_string(assembly.crf),
        "-pix_fmt", assembly.pix_fmt,
        "-r", std::to_string(fps),
        "-c:a", "copy",
        // **不加 -shortest。** 画面和音轨是同一段源解出来的，长度本来
        // 就一样；加了反而会在两边差半帧时截掉尾巴。assemble.hpp 里那
        // 条同样的告诫是踩出来的。
        paths::to_utf8(out),
    };
}

#ifdef CHANGJI_HAVE_SD

void upscale_video(const fs::path& in, const fs::path& out,
                   const fs::path& model, int out_w, int out_h,
                   const config::AssemblyConfig& assembly) {
    if (!fs::is_regular_file(in)) {
        throw SdError(SAYF("要超分的视频不在：%1", paths::to_utf8(in)));
    }
    if (!fs::is_regular_file(model)) {
        throw SdError(SAYF("超分模型不在：%1", paths::to_utf8(model)));
    }
    sd_log_to_stderr();

    const Probe p = probe_video(in, assembly);
    fs::path raw_in = out;
    raw_in += ".in.raw";
    fs::path raw_up = out;
    raw_up += ".up.raw";

    const std::vector<unsigned char> raw =
        decode_to_raw(in, assembly, raw_in);
    const std::size_t frame_bytes =
        static_cast<std::size_t>(p.width) * p.height * 3;
    const std::size_t frames = frame_bytes ? raw.size() / frame_bytes : 0;
    if (frames == 0) throw SdError(SAY("解出来一帧都没有"));
    // `[超分]` 这一族走 stderr，是日志不是界面，不包 `SAY()`。
    std::fprintf(stderr, SAY_NEVER("[超分] %d×%d %zu 帧\n"), p.width, p.height, frames);

    // **超分上下文只建一次。** 每帧重建的话 67 MB 的权重要反复加载，
    // 73 帧就是 73 次——这类"每次都重来"的浪费在这套代码里栽过好几回。
    upscaler_ctx_t* ctx = ::new_upscaler_ctx(paths::to_utf8(model).c_str(),
                                             false, -1, 0, "", "");
    if (ctx == nullptr) {
        throw SdError(SAYF("超分模型加载失败：%1\n看一眼上面 sd.cpp 打的日志",
                           paths::to_utf8(model)));
    }
    const int factor = ::get_upscale_factor(ctx);
    std::fprintf(stderr, SAY_NEVER("[超分] 模型倍率 %d，先放大再压到 %d×%d\n"), factor,
                 out_w, out_h);

    // **先算中间文件要多大，不够就当场说。**
    //
    // 放大后每帧是原来的 factor² 倍：544×928 的一帧 1.5 MB，x4 之后
    // 24 MB。一分钟的片子（1483 帧）就是 36 GB 写在 `out` 旁边。而
    // doctor 里那句"要 2K 就出完再跑 changji --upscale"指的正是成片，
    // 所以这个量级是常态，不是极端情况。
    //
    // 整段解进内存、整段写盘的做法要改成流式（见 decode_to_raw 上面
    // 那句）。在那之前，**至少不能跑满五十分钟再死在磁盘满上**——
    // 那时候 GPU 的时间已经花掉了，报的还是 ffmpeg 的一句英文。
    const std::size_t big_frame_bytes =
        frame_bytes * static_cast<std::size_t>(factor) * factor;
    const std::size_t need = big_frame_bytes * frames;
    const double need_gb = static_cast<double>(need) / (1024.0 * 1024 * 1024);
    std::error_code space_ec;
    const fs::space_info space = fs::space(fs::absolute(out).parent_path(), space_ec);
    const double free_gb =
        space_ec ? -1.0 : static_cast<double>(space.available) / (1024.0 * 1024 * 1024);
    std::fprintf(stderr, SAY_NEVER("[超分] 中间文件要 %.1f GB（%zu 帧 × %.1f MB）\n"),
                 need_gb, frames,
                 static_cast<double>(big_frame_bytes) / (1024.0 * 1024));
    if (!space_ec && space.available < need) {
        const auto gb = [](double v) {
            char b[32];
            std::snprintf(b, sizeof b, "%.1f", v);
            return std::string(b);
        };
        const std::string msg = SAYF(
            "磁盘不够：放大到 %1×%2 要先写 %3 GB 的中间文件（%4 帧 × %5 MB），"
            "而 %6 上只剩 %7 GB。\n腾出空间，或者把片子切成几段分开跑。",
            std::to_string(p.width * factor), std::to_string(p.height * factor),
            gb(need_gb), std::to_string(frames),
            gb(static_cast<double>(big_frame_bytes) / (1024.0 * 1024)),
            paths::to_utf8(fs::absolute(out).parent_path()), gb(free_gb));
        ::free_upscaler_ctx(ctx);
        // 解出来的裸帧（原尺寸，也有几个 G）先删掉再报——
        // 磁盘本来就满了，不能再占着。
        std::error_code rm_ec;
        fs::remove(raw_in, rm_ec);
        throw SdError(msg);
    }

    std::ofstream up(raw_up, std::ios::binary | std::ios::trunc);
    if (!up) throw SdError(SAYF("写不了中间文件：%1", paths::to_utf8(raw_up)));

    for (std::size_t i = 0; i < frames; ++i) {
        sd_image_t src{};
        src.width = static_cast<std::uint32_t>(p.width);
        src.height = static_cast<std::uint32_t>(p.height);
        src.channel = 3;
        // sd.cpp 不会改输入，但接口要非 const 指针。
        src.data = const_cast<unsigned char*>(raw.data() + i * frame_bytes);

        sd_image_t* got = nullptr;
        int n = 0;
        if (!::upscale(ctx, src, static_cast<std::uint32_t>(factor), &got, &n) ||
            got == nullptr || n < 1) {
            ::free_upscaler_ctx(ctx);
            throw SdError(SAYF("第 %1 帧超分失败", std::to_string(i + 1)));
        }
        up.write(reinterpret_cast<const char*>(got->data),
                 static_cast<std::streamsize>(
                     static_cast<std::size_t>(got->width) * got->height * 3));
        const int big_w = static_cast<int>(got->width);
        const int big_h = static_cast<int>(got->height);
        ::free_sd_images(got, n);
        if (i == 0) {
            std::fprintf(stderr, SAY_NEVER("[超分] 放大后每帧 %d×%d\n"), big_w, big_h);
        }
        if ((i + 1) % 10 == 0 || i + 1 == frames) {
            std::fprintf(stderr, SAY_NEVER("[超分] %zu/%zu\n"), i + 1, frames);
        }
    }
    up.close();
    ::free_upscaler_ctx(ctx);

    // 放大后的尺寸按模型倍率算，再让 ffmpeg 用 lanczos 压到目标。
    const int big_w = p.width * factor;
    const int big_h = p.height * factor;
    const auto exe = proc::which(assembly.ffmpeg_path);
    if (!exe.has_value()) throw SdError(SAY("找不到 ffmpeg"));
    const proc::Result r =
        proc::run(*exe,
                  encode_args(raw_up, big_w, big_h, in, out_w, out_h, p.fps,
                              assembly, out),
                  0);
    std::error_code ec;
    fs::remove(raw_in, ec);
    fs::remove(raw_up, ec);
    if (!r.launched || r.exit_code != 0) {
        throw SdError(SAYF("ffmpeg 压回目标尺寸失败：%1", r.out));
    }
}

#else

void upscale_video(const fs::path&, const fs::path&, const fs::path&, int, int,
                   const config::AssemblyConfig&) {
    throw SdError(SAY("这个二进制没编进 sd.cpp，做不了超分"));
}

#endif

}  // namespace changji::infer
