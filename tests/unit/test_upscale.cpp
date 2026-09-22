// 超分那条命令行的测试。
//
// **这里测的不是超分本身，是最后那道 ffmpeg 的参数。** 音轨丢没丢由它
// 决定，而丢了不报错——2026-09-13 实测：544×928 h264+aac 进去，
// 1088×1920 h264 出来，一条音频流都没有。doctor 又恰好建议"要 2K 就
// 出完再跑 changji --upscale"，指的正是成片。

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "infer/sd_upscale.hpp"

using namespace changji;

namespace {

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

/// 取 `flag` 后面跟的那个值；没有就返回空串。
std::string arg_value(const std::vector<std::string>& args,
                      const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || it + 1 == args.end()) return {};
    return *(it + 1);
}

/// `flag` 出现过的所有取值，按顺序。-map 会出现两次。
std::vector<std::string> arg_values(const std::vector<std::string>& args,
                                    const std::string& flag) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) out.push_back(args[i + 1]);
    }
    return out;
}

std::vector<std::string> sample() {
    return infer::encode_args("f.up.raw", 2176, 3712, "src.mp4", 1088, 1920, 24,
                              config::AssemblyConfig{}, "out.mp4");
}

}  // namespace

TEST_CASE("超分要把原片的音轨带过来") {
    // 这条是踩出来的：上一版最后那道 ffmpeg 写的是 `-an`，
    // 而 doctor 里建议用户"出完再跑 changji --upscale"，
    // 照做一遍拿到的是一段哑片，中间一句提示都没有。
    const auto args = sample();

    CHECK_FALSE(has_flag(args, "-an"));
    CHECK(arg_value(args, "-c:a") == "copy");

    SUBCASE("原片是第二个输入，只取它的音轨") {
        const auto inputs = arg_values(args, "-i");
        REQUIRE(inputs.size() == 2);
        CHECK(inputs[0] == "f.up.raw");  // 放大后的裸帧
        CHECK(inputs[1] == "src.mp4");   // 原片

        const auto maps = arg_values(args, "-map");
        REQUIRE(maps.size() == 2);
        CHECK(maps[0] == "0:v:0");
        // 末尾的问号是"没有音轨就算了"——分镜级的片段可能真没有，
        // 不能因此整条命令失败。
        CHECK(maps[1] == "1:a:0?");
    }

    SUBCASE("不加 -shortest") {
        // 画面和音轨是同一段源解出来的，长度本来就一样；
        // 加了反而会在两边差半帧时把尾巴截掉。
        CHECK_FALSE(has_flag(args, "-shortest"));
    }
}

TEST_CASE("超分的裸帧输入要自带宽高帧率") {
    // rawvideo 里没有这些信息，漏一个 ffmpeg 就按默认值解，
    // 出来是花屏而不是报错。
    const auto args = sample();
    CHECK(arg_value(args, "-f") == "rawvideo");
    CHECK(arg_value(args, "-pixel_format") == "rgb24");
    CHECK(arg_value(args, "-video_size") == "2176x3712");
    CHECK(arg_value(args, "-framerate") == "24");
}

TEST_CASE("超分压回目标尺寸用 lanczos") {
    const auto args = sample();
    CHECK(arg_value(args, "-vf") == "scale=1088:1920:flags=lanczos");
    CHECK(args.back() == "out.mp4");
    // 不带 -y 的话 ffmpeg 遇到已存在的输出会问"覆盖吗"，
    // 而这个进程的 stdin 是关着的，它会一直等到超时。
    CHECK(has_flag(args, "-y"));
}

TEST_CASE("超分的画面编码跟着装配配置走") {
    config::AssemblyConfig cfg;
    cfg.video_codec = "libx265";
    cfg.crf = 20;
    cfg.pix_fmt = "yuv420p10le";
    const auto args = infer::encode_args("a.raw", 100, 100, "s.mp4", 50, 50, 30,
                                         cfg, "o.mp4");
    CHECK(arg_value(args, "-c:v") == "libx265");
    CHECK(arg_value(args, "-crf") == "20");
    CHECK(arg_value(args, "-pix_fmt") == "yuv420p10le");
    CHECK(arg_value(args, "-r") == "30");
}
