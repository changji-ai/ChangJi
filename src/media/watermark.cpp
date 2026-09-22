#include "media/watermark.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "media/assemble.hpp"          // escape_filter_path
#include "media/bundled_watermark.inc.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::media {

namespace fs = std::filesystem;

namespace {

/// 标高占**短边**的比例。
///
/// ⚠️ **短边，不是画面高。** 横屏两者是一回事，竖屏差一大截：1080×1920 上
/// 按画面高算，标高 106 px，水印横着铺出 32% 的画面宽。
constexpr double kMarkRatio = 0.055;

/// 标高的下限。
///
/// 低于它，「AI 生成」那几个字（标高的 0.46 倍）掉到 15 px 以下——拿真片
/// 抽帧实测过，20 px 时整行糊成一团，而**读不出来的标识不叫标识**。
/// 480p 的成片上水印会因此占得比 5.5% 大，这个取舍是故意的：宁可显眼。
constexpr int kMinMarkH = 32;

/// 边距，同样按短边算。两边一样宽才像一个角。
constexpr double kMarginRatio = 0.030;

int scaled(int v, double k) { return std::max(1, static_cast<int>(std::lround(v * k))); }

}  // namespace

WatermarkPlan stage_watermark(const fs::path& image_path, int target_w,
                              int target_h, int upscale) {
    const int up = std::max(1, upscale);
    // 放大过的画面：按**放大前**的短边算好，再乘回去。直接拿最终尺寸算的话
    // 补出来的比被一起放大的那个旧水印小（32px 下限不是按比例走的），
    // 旧的会从左上角露出来。
    const int shorter = std::min(target_w, target_h) / up;
    if (shorter <= 0) return {};

    const bool portrait = target_h > target_w;
    const unsigned char* png =
        portrait ? bundled::kWatermarkPortraitPng : bundled::kWatermarkLandscapePng;
    const std::size_t size = portrait ? sizeof(bundled::kWatermarkPortraitPng)
                                      : sizeof(bundled::kWatermarkLandscapePng);
    const int src_w = portrait ? bundled::kWatermarkPortraitW : bundled::kWatermarkLandscapeW;
    const int src_h = portrait ? bundled::kWatermarkPortraitH : bundled::kWatermarkLandscapeH;

    const fs::path dest = image_path;
    {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream f(dest, std::ios::binary | std::ios::trunc);
        if (!f) {
            throw std::runtime_error(
                SAYF("写不了水印图：%1", paths::to_utf8(dest)));
        }
        f.write(reinterpret_cast<const char*>(png), static_cast<std::streamsize>(size));
        if (!f) {
            throw std::runtime_error(
                SAYF("水印图没写完：%1", paths::to_utf8(dest)));
        }
    }

    const int mark_h =
        std::max(kMinMarkH, static_cast<int>(std::lround(shorter * kMarkRatio))) * up;
    const double k = static_cast<double>(mark_h) / bundled::kWatermarkBaseMarkH;

    WatermarkPlan plan;
    plan.image = dest;
    plan.width = scaled(src_w, k);
    plan.height = scaled(src_h, k);
    plan.margin = static_cast<int>(std::lround(shorter * kMarginRatio)) * up;
    return plan;
}

std::string with_watermark(const std::string& chain, const WatermarkPlan& plan) {
    if (plan.empty()) return chain;

    // 标签带 cjwm_ 前缀：这条链要和 look_filters 拼在一起，而那边的
    // split/blend 用的是 o / g / g2。撞名的话 ffmpeg 报的是「滤镜语法错误」，
    // 看不出是两处各自取的标签重了。
    const std::string m = std::to_string(plan.margin);
    return chain + "[cjwm_base];movie='" + escape_filter_path(plan.image) +
           "',scale=" + std::to_string(plan.width) + ":" +
           std::to_string(plan.height) + "[cjwm];[cjwm_base][cjwm]overlay=W-w-" +
           m + ":H-h-" + m;
}

}  // namespace changji::media
