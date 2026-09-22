#pragma once

#include <filesystem>
#include <string>

namespace changji::media {

/// 产物右下角那个角标：标 + 「场记AI 生成」。
///
/// **品牌和 AI 生成标识是同一个东西**，不是两个能分别开关的图层——所以这里
/// 没有 enabled 之类的开关，配置里也不该出现一个。
///
/// **图嵌在二进制里**（bundled_watermark.inc.hpp），装配时解到那一轮自己的
/// `.work` 目录，用完随 .work 一起删。磁盘上不留常驻文件：留了就等于留下
/// 一个「删掉它就没水印」的开关，而那种开关不会报错，只会让产物静悄悄干净。
struct WatermarkPlan {
    /// 解出来的那张 PNG（在 .work 里）。
    std::filesystem::path image;
    /// 整张图缩到多大。按标高算好的确切像素，不让 ffmpeg 去猜比例。
    int width = 0;
    int height = 0;
    /// 离画面右下角多远。
    int margin = 0;

    bool empty() const { return image.empty() || width <= 0 || height <= 0; }
};

/// 按画面尺寸挑版式、算大小，并把图解到 `dir` 底下。
///
/// 横屏排一行、竖屏排两行——**不是好看，是宽度**：「场记AI 生成」六个字
/// 单行占到 1080×1920 画面宽的 24.4%，两行是 17.7%。
///
/// 解不出来（写不了盘）**抛异常**，不是跳过。水印缺了不能当没事发生：
/// 那正是「删掉它就没水印」那条路的另一个入口。
WatermarkPlan stage_watermark(const std::filesystem::path& dir, int target_w,
                              int target_h);

/// 把水印叠在一条滤镜链后面。`plan` 空就原样返回 `chain`（一个字都不多，
/// 这样"没水印"和改动前逐字节一样）。
///
/// 接在**最后**，在 look 之后：遮幅是 crop+pad，水印排在它前面会被裁掉；
/// 柔化会把水印糊掉，调色会把白的改成奶油色；颗粒加在水印上会让它看着
/// 像印在画面里，而水印要的是被看见。
std::string with_watermark(const std::string& chain, const WatermarkPlan& plan);

}  // namespace changji::media
