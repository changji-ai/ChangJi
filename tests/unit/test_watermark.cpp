// 产物角标（标 + 「场记AI 生成」）。
//
// 这一族全是"错了不当场报错"的：滤镜段拼错只会让 ffmpeg 报一句语法错、
// 版式挑错只会让竖屏的水印铺出小半个画面宽、尺寸算错只会让那行字小到
// 读不出来——**而读不出来的标识不叫标识**。所以按纯函数钉死。
//
// 钉的是「发出去的参数长什么样」，不是像素：像素要人看，那一步在
// brand/ 那边用真片抽帧比过（四档底，角落亮度 82 / 157 / 180 / 238）。
//
// 改了水印要**真装配一次再看**，别只看这里绿：构造一个 media::Timeline
// 指向一段 mp4，拿 media::Assembler 跑 assemble()，再从成片抽帧看右下角。
// 横竖两种画幅各跑一次——版式是按短边挑的，只跑横屏验不到两行那一版。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "media/assemble.hpp"
#include "media/bundled_watermark.inc.hpp"
#include "media/watermark.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;
using namespace changji;

namespace {

std::string arg_value(const std::vector<std::string>& args,
                      const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || it + 1 == args.end()) return {};
    return *(it + 1);
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_水印_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// 图里标那一块的高，按比例反推出来——用来验尺寸规则。
int mark_height_of(const media::WatermarkPlan& plan, bool portrait) {
    const int src_h = portrait ? media::bundled::kWatermarkPortraitH
                               : media::bundled::kWatermarkLandscapeH;
    return static_cast<int>(
        std::lround(static_cast<double>(plan.height) *
                    media::bundled::kWatermarkBaseMarkH / src_h));
}

}  // namespace

TEST_CASE("水印：没有就一个字都不多") {
    config::AssemblyConfig cfg;
    const auto plain = media::normalize_args("in.mp4", 1920, 1080, cfg, "out.mp4");
    media::NormalizeOptions opt;   // watermark 默认空
    const auto same =
        media::normalize_args("in.mp4", 1920, 1080, cfg, "out.mp4", opt);
    CHECK(plain == same);
    CHECK_FALSE(has(arg_value(same, "-vf"), "overlay"));

    // with_watermark 自己也得是恒等的
    CHECK(media::with_watermark("scale=2:2", media::WatermarkPlan{}) == "scale=2:2");
}

TEST_CASE("水印：接在后期链的最后一段后面") {
    media::WatermarkPlan plan;
    plan.image = "/tmp/cj_watermark.png";
    plan.width = 166;
    plan.height = 55;
    plan.margin = 32;

    config::AssemblyConfig cfg;
    media::NormalizeOptions opt;
    opt.watermark = plan;
    // 遮幅（crop+pad）和颗粒都在后期链里。水印排在遮幅前面会被裁掉，
    // 排在柔化前面会被糊掉——所以位置必须在 extra_vf 之后。
    opt.extra_vf = "crop=1920:804:0:138,pad=1920:1080:0:138,noise=c0s=10:c0f=t+u";
    const std::string vf =
        arg_value(media::normalize_args("in.mp4", 1920, 1080, cfg, "out.mp4", opt),
                  "-vf");

    const auto post = vf.find("noise=c0s=10");
    const auto over = vf.find("overlay=");
    REQUIRE(post != std::string::npos);
    REQUIRE(over != std::string::npos);
    CHECK(post < over);
    // 主链要先收进一个标签，水印才好接上去
    CHECK(has(vf, "noise=c0s=10:c0f=t+u[cjwm_base];"));
    CHECK(has(vf, "scale=166:55[cjwm]"));
    CHECK(has(vf, "[cjwm_base][cjwm]overlay=W-w-32:H-h-32"));
}

TEST_CASE("水印：标签不和 look 的 split/blend 撞") {
    // look_filters 强度 <1 时返回的是带标签的多条链（split[o][g] … blend），
    // 水印要拼在它后面。两处各自取标签，撞了 ffmpeg 报的是「滤镜语法错误」,
    // 看不出是重名。
    config::LookConfig look;
    look.preset = "film";
    look.lut_strength = 0.6;
    const std::string chain = media::look_filters(look, 1920, 1080, "/p");
    REQUIRE(has(chain, "split[o][g]"));

    media::WatermarkPlan plan;
    plan.image = "/tmp/w.png";
    plan.width = 10;
    plan.height = 10;
    plan.margin = 4;
    const std::string full = media::with_watermark(chain, plan);
    for (const char* tag : {"[cjwm_base]", "[cjwm]"}) {
        CHECK_FALSE(has(chain, tag));       // look 那半截里没有这两个名字
        CHECK(has(full, tag));
    }
}

TEST_CASE("水印：按短边算，不按画面高") {
    const fs::path dir = temp_root("短边");

    // ⚠️ 这一条是实测出来的：按画面高算，1080×1920 上水印横着铺出 32% 的
    // 画面宽。横屏两者是一回事，竖屏差一大截。
    const auto land = media::stage_watermark(dir, 1920, 1080);
    const auto port = media::stage_watermark(dir, 1080, 1920);
    REQUIRE_FALSE(land.empty());
    REQUIRE_FALSE(port.empty());

    // 两个的短边都是 1080，所以标一样高、边距一样宽
    CHECK(mark_height_of(land, false) == mark_height_of(port, true));
    CHECK(land.margin == port.margin);

    // 竖屏换两行的版式：同样的标高，宽度要窄一截
    CHECK(port.width < land.width);
    CHECK(port.height > land.height);
}

TEST_CASE("水印：标高有下限，小片子上宁可占得大") {
    const fs::path dir = temp_root("下限");

    // 720p：短边 720 × 5.5% = 40
    CHECK(mark_height_of(media::stage_watermark(dir, 1280, 720), false) == 40);
    // 1080p：59
    CHECK(mark_height_of(media::stage_watermark(dir, 1920, 1080), false) == 59);
    // 480p：短边 480 × 5.5% = 26，低于下限，顶到 32——
    // 低于 32「AI 生成」那几个字掉到 15 px 以下就读不出来了
    CHECK(mark_height_of(media::stage_watermark(dir, 854, 480), false) == 32);
    // 再小也不会更小
    CHECK(mark_height_of(media::stage_watermark(dir, 320, 240), false) == 32);
}

TEST_CASE("水印：图解到给的目录里，是一张真 PNG") {
    const fs::path dir = temp_root("落盘");
    const auto plan = media::stage_watermark(dir, 1920, 1080);
    REQUIRE_FALSE(plan.empty());
    REQUIRE(fs::is_regular_file(plan.image));
    CHECK(plan.image.parent_path() == dir);

    std::ifstream f(plan.image, std::ios::binary);
    char sig[8] = {};
    f.read(sig, 8);
    // PNG 的魔数。写了个半截文件的话这里先红
    CHECK(static_cast<unsigned char>(sig[0]) == 0x89);
    CHECK(std::string(sig + 1, 3) == "PNG");
    CHECK(fs::file_size(plan.image) == sizeof(media::bundled::kWatermarkLandscapePng));
}

TEST_CASE("水印：路径按滤镜的规矩转义") {
    media::WatermarkPlan plan;
    // 冒号是滤镜的参数分隔符，单引号会提前收尾——两个都得转义，
    // 规矩和 subtitles= 那边一份（escape_filter_path）
    plan.image = "/tmp/a'b/c:d/w.png";
    plan.width = 4;
    plan.height = 4;
    plan.margin = 1;
    const std::string vf = media::with_watermark("null", plan);
    CHECK(has(vf, "c\\:d"));
    CHECK(has(vf, "a'\\''b"));
}

TEST_CASE("水印：画面尺寸不成立就不加，不抛") {
    const fs::path dir = temp_root("零尺寸");
    CHECK(media::stage_watermark(dir, 0, 0).empty());
    CHECK(media::stage_watermark(dir, -1, 100).empty());
}

