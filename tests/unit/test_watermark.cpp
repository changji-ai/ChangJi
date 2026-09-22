// 产物角标（标 + 「场记AI 生成」）。
//
// 这一族全是"错了不当场报错"的：滤镜段拼错只会让 ffmpeg 报一句语法错、
// 版式挑错只会让竖屏的水印铺出小半个画面宽、尺寸算错只会让那行字小到
// 读不出来——**而读不出来的标识不叫标识**。所以按纯函数钉死。
//
// 钉的是「发出去的参数长什么样」，不是像素：像素要人看，那一步在
// brand/ 那边用真片抽帧比过（四档底，角落亮度 82 / 157 / 180 / 238）。
//
// **水印加在单镜出片那一步**（infer::encode_args），不在装配。装配是
// `-c copy` 往下走的，那儿烧进去的水印一路带到章成片、整部电影和预告。
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
#include "infer/sd_video.hpp"
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

TEST_CASE("水印：单镜编码那一步烧进去") {
    media::WatermarkPlan plan;
    plan.image = "/tmp/cj_watermark.png";
    plan.width = 166;
    plan.height = 55;
    plan.margin = 32;

    config::AssemblyConfig cfg;
    const auto args = infer::encode_args("raw.rgb", 1280, 720, 24, cfg, "out.mp4",
                                         std::nullopt, 0.0, plan);
    const std::string vf = arg_value(args, "-vf");
    CHECK(has(vf, "scale=166:55[cjwm]"));
    CHECK(has(vf, "[cjwm_base][cjwm]overlay=W-w-32:H-h-32"));
    // 这一步是 rawvideo → mp4 的**唯一一次**编码，水印不该多花一次重编码
    CHECK(std::count(args.begin(), args.end(), "-i") == 1);
}

TEST_CASE("水印：单镜不给就一个字都不多") {
    config::AssemblyConfig cfg;
    const auto plain = infer::encode_args("raw.rgb", 1280, 720, 24, cfg, "out.mp4");
    const auto same = infer::encode_args("raw.rgb", 1280, 720, 24, cfg, "out.mp4",
                                         std::nullopt, 0.0, media::WatermarkPlan{});
    CHECK(plain == same);
    CHECK(arg_value(same, "-vf").empty());
}

TEST_CASE("装配那一步默认不加水印") {
    // 2026-09-22 起水印在单镜那一层烧。装配是 `-c copy` 往下走的，这儿再加
    // 一层就是**两个角标**——同一章里镜头尺寸不一致时还会错位重影。
    config::AssemblyConfig cfg;
    media::NormalizeOptions opt;
    opt.extra_vf = "gblur=sigma=0.4";
    const std::string vf =
        arg_value(media::normalize_args("in.mp4", 1920, 1080, cfg, "out.mp4", opt),
                  "-vf");
    CHECK_FALSE(has(vf, "overlay"));
    CHECK_FALSE(has(vf, "cjwm"));
    CHECK_FALSE(has(vf, "movie="));
}

TEST_CASE("装配补水印：只给会毁掉它的那两种后期兜底") {
    config::LookConfig look;      // 默认 film，letterbox = 0
    config::UpscaleConfig up;     // 默认 command 空 = 不放大

    SUBCASE("什么都没开：不补") {
        CHECK(media::assembly_watermark_upscale(look, up, 1920, 1080) == 0);
    }
    SUBCASE("遮幅：补标准尺寸的（旧的被裁掉了，不用管盖不盖得住）") {
        look.letterbox = 2.39;
        CHECK(media::assembly_watermark_upscale(look, up, 1920, 1080) == 1);
    }
    SUBCASE("竖屏不遮幅，所以也不补") {
        // look_filters 里 `target_w > target_h` 才遮。这儿的判据要跟它一致，
        // 不然竖屏项目会白补一层——而那就是两个角标。
        look.letterbox = 2.39;
        CHECK(media::assembly_watermark_upscale(look, up, 1080, 1920) == 0);
    }
    SUBCASE("look 整个关掉：遮幅也不生效，不补") {
        look.preset = "off";
        look.letterbox = 2.39;
        CHECK(media::assembly_watermark_upscale(look, up, 1920, 1080) == 0);
    }
    SUBCASE("放大：按倍数补，才盖得住被一起放大的那个") {
        up.command = "esrgan %{in} %{out}";
        up.scale = 2;
        CHECK(media::assembly_watermark_upscale(look, up, 3840, 2160) == 2);
        up.scale = 4;
        CHECK(media::assembly_watermark_upscale(look, up, 3840, 2160) == 4);
    }
    SUBCASE("两个都开：算遮幅那一档") {
        // 旧的先被放大、再被裁掉，还是没了——补标准尺寸的就行
        look.letterbox = 2.39;
        up.command = "esrgan %{in} %{out}";
        up.scale = 2;
        CHECK(media::assembly_watermark_upscale(look, up, 3840, 2160) == 1);
    }
}

TEST_CASE("补的那层要盖得住被放大的旧水印") {
    const fs::path dir = temp_root("放大");

    // 832×480 的镜头放大 2 倍。旧水印是按 480 短边烧的（下限顶到 32），
    // 跟着放大就是 64；按最终的 960 短边算只有 53——小 17%，旧的会露边。
    const auto fit = media::stage_watermark(dir / "u.png", 1664, 960, 2);
    const auto std_ = media::stage_watermark(dir / "s.png", 1664, 960);
    CHECK(mark_height_of(fit, false) == 64);
    CHECK(mark_height_of(std_, false) == 53);
    CHECK(fit.width > std_.width);
    // 边距也要跟着乘，不然位置对不齐
    CHECK(fit.margin == 14 * 2);

    // 短边够大时两者自然相等（下限不再起作用），差 1px 是取整
    const auto big = media::stage_watermark(dir / "b.png", 3840, 2160, 2);
    CHECK(mark_height_of(big, false) == 118);
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
    const auto land = media::stage_watermark(dir / "a.png", 1920, 1080);
    const auto port = media::stage_watermark(dir / "b.png", 1080, 1920);
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
    CHECK(mark_height_of(media::stage_watermark(dir / "c.png", 1280, 720), false) == 40);
    // 1080p：59
    CHECK(mark_height_of(media::stage_watermark(dir / "a.png", 1920, 1080), false) == 59);
    // 480p：短边 480 × 5.5% = 26，低于下限，顶到 32——
    // 低于 32「AI 生成」那几个字掉到 15 px 以下就读不出来了
    CHECK(mark_height_of(media::stage_watermark(dir / "d.png", 854, 480), false) == 32);
    // 再小也不会更小
    CHECK(mark_height_of(media::stage_watermark(dir / "e.png", 320, 240), false) == 32);
}

TEST_CASE("水印：图解到给的目录里，是一张真 PNG") {
    const fs::path dir = temp_root("落盘");
    const auto plan = media::stage_watermark(dir / "a.png", 1920, 1080);
    REQUIRE_FALSE(plan.empty());
    REQUIRE(fs::is_regular_file(plan.image));
    // 文件名由调用方给：出片是多镜并行的，共用一个名字会互相踩
    CHECK(plan.image == dir / "a.png");

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
    CHECK(media::stage_watermark(dir / "f.png", 0, 0).empty());
    CHECK(media::stage_watermark(dir / "g.png", -1, 100).empty());
}

