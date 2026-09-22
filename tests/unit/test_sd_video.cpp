// 视频编码那一层的测试。
//
// sd.cpp 的 generate_video 吐的是**裸帧**不是 mp4，所以多一步 ffmpeg。
// 这里测的是**参数拼得对不对**——编码参数错了不会当场报错，
// 只会让产出的 mp4 在某些播放器上打不开，或者拼接时被重编码
// （白白多一次有损压缩）。这两样都要到很后面才发现。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "infer/sd_video.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 找一个参数的值。找不到返回空串。
std::string arg_value(const std::vector<std::string>& args,
                      const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || it + 1 == args.end()) return {};
    return *(it + 1);
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

}  // namespace

TEST_CASE("ffmpeg 参数按配置来") {
    config::AssemblyConfig a;
    a.fps = 24;
    a.crf = 18;
    a.video_codec = "libx264";
    a.pix_fmt = "yuv420p";

    const auto args = infer::encode_args("C:/tmp/x.rgb", 448, 768, 24, a,
                                         "C:/out/ep01_sh001.mp4");

    // 输入是裸 RGB24，尺寸和帧率必须显式给——rawvideo 没有头，
    // 不给的话 ffmpeg 只能猜，猜错了出来的是花屏。
    CHECK(arg_value(args, "-f") == "rawvideo");
    CHECK(arg_value(args, "-pixel_format") == "rgb24");
    CHECK(arg_value(args, "-video_size") == "448x768");
    CHECK(arg_value(args, "-framerate") == "24");

    CHECK(arg_value(args, "-c:v") == "libx264");
    CHECK(arg_value(args, "-crf") == "18");
    CHECK(arg_value(args, "-pix_fmt") == "yuv420p");

    // 输出路径在最后一个
    CHECK(args.back() == "C:/out/ep01_sh001.mp4");
}

TEST_CASE("像素格式必须显式给 yuv420p") {
    // 不给的话 ffmpeg 会保留 rgb24，而 H.264 的 rgb24 变体一大半播放器
    // 打不开，包括浏览器里的 <video>——而审片就是在浏览器里做的。
    config::AssemblyConfig a;
    const auto args = infer::encode_args("x.rgb", 448, 768, 24, a, "out.mp4");
    CHECK(has_flag(args, "-pix_fmt"));
    CHECK(arg_value(args, "-pix_fmt") == "yuv420p");
    // 而且不能和输入那个 -pixel_format 搞混：两个是不同的参数，
    // 一个说输入是什么，一个说输出要什么。
    CHECK(arg_value(args, "-pixel_format") == "rgb24");
}

TEST_CASE("这一步不带声音") {
    // 配音是后面的阶段。带上的话 ffmpeg 会去找音频流，找不到就报错。
    const auto args =
        infer::encode_args("x.rgb", 448, 768, 24, config::AssemblyConfig{},
                           "out.mp4");
    CHECK(has_flag(args, "-an"));
}

TEST_CASE("覆盖已有文件，不停下来问") {
    // 重跑一镜时不该卡在"要覆盖吗"上——那是个交互提示，
    // 而这个进程的 stdin 是关着的，会一直等到超时。
    const auto args =
        infer::encode_args("x.rgb", 448, 768, 24, config::AssemblyConfig{},
                           "out.mp4");
    CHECK(has_flag(args, "-y"));
}

TEST_CASE("输出帧率和输入一致") {
    // 不显式给 -r 的话 ffmpeg 可能按容器默认帧率重采样，
    // 那会让 121 帧变成别的数量，成片时长就对不上分镜表了。
    for (const int fps : {24, 25, 30}) {
        CAPTURE(fps);
        config::AssemblyConfig a;
        a.fps = fps;
        const auto args =
            infer::encode_args("x.rgb", 448, 768, fps, a, "out.mp4");
        CHECK(arg_value(args, "-framerate") == std::to_string(fps));
        CHECK(arg_value(args, "-r") == std::to_string(fps));
    }
}

TEST_CASE("改了配置里的编码参数要跟着变") {
    // 每一镜的片段和最后拼起来的成片必须用同一套规格，
    // 否则拼接环节要重编码，那是白白多一次有损压缩。
    config::AssemblyConfig a;
    a.video_codec = "libx265";
    a.crf = 28;
    a.pix_fmt = "yuv444p";
    const auto args = infer::encode_args("x.rgb", 448, 768, 24, a, "out.mp4");
    CHECK(arg_value(args, "-c:v") == "libx265");
    CHECK(arg_value(args, "-crf") == "28");
    CHECK(arg_value(args, "-pix_fmt") == "yuv444p");
}

TEST_CASE("中文路径原样传，不做 shell 拼接") {
    // 参数是逐个传给子进程的，不拼成一条命令行——路径里有空格或中文时，
    // 拼字符串再交给 shell 是最经典的一类 bug。
    const fs::path raw = paths::from_utf8("C:/项目 库/雨夜天台/.tmp.rgb");
    const fs::path out = paths::from_utf8("C:/项目 库/雨夜天台/shots/draft/a.mp4");
    const auto args =
        infer::encode_args(raw, 448, 768, 24, config::AssemblyConfig{}, out);

    CHECK(arg_value(args, "-i") == paths::to_utf8(raw));
    CHECK(args.back() == paths::to_utf8(out));
    // 没有引号包裹——那是 shell 的事，逐参数传不需要
    CHECK(args.back().find('"') == std::string::npos);
}

TEST_CASE("找不到 ffmpeg 时说清去哪儿配") {
    config::AssemblyConfig a;
    a.ffmpeg_path = "根本没有这个程序_zzz";
    try {
        infer::encode_raw_to_mp4("x.rgb", 448, 768, 24, a, "out.mp4");
        FAIL("该抛异常");
    } catch (const infer::SdError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("ffmpeg") != std::string::npos);
        // 要说清怎么办，不是只说"找不到"
        CHECK(msg.find("assembly.ffmpeg_path") != std::string::npos);
    }
}
