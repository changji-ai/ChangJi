#pragma once

#include <filesystem>
#include <string>

namespace changji::media {

/// 产物右下角那个角标：标 + 「场记AI 生成」。
///
/// **品牌和 AI 生成标识是同一个东西**，不是两个能分别开关的图层——所以这里
/// 没有 enabled 之类的开关，配置里也不该出现一个。
///
/// 这是标识的**显式**那一半（画面上看得见的）。隐式那一半是写进文件元数据的
/// 几栏，见 `media/product_tags.hpp`：它不占画面，所以哪儿都写、永远写。
///
/// **烧在 `infer::encode_args`**——一镜从 rawvideo 变成 mp4 的那次编码。
/// 那是唯一一次编码，加在那儿不多花一次重编码；而从那儿往下全是 `-c copy`，
/// 一次烧进去，单镜、章成片、整部电影、预告四样全带上。
/// ⚠️ **装配那一层不许再加**（`test_watermark.cpp` 钉着）：再加就是两个角标，
/// 同一章里镜头尺寸不一致时还会错位重影。
///
/// **图嵌在二进制里**（bundled_watermark.inc.hpp），出片时解到那一镜自己的
/// 临时文件，编完就删。磁盘上不留常驻文件：留了就等于留下一个「删掉它就没
/// 水印」的开关，而那种开关不会报错，只会让产物静悄悄干净。
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

/// 按画面尺寸挑版式、算大小，并把图解到 `image_path`。
///
/// ⚠️ **文件名由调用方给**，不是这儿定一个固定的：出片是多镜并行的
/// （多卡机上好几镜同时编码），共用一个文件名就会互相踩。
///
/// 横屏排一行、竖屏排两行——**不是好看，是宽度**：「场记AI 生成」六个字
/// 单行占到 1080×1920 画面宽的 24.4%，两行是 17.7%。
///
/// 解不出来（写不了盘）**抛异常**，不是跳过。水印缺了不能当没事发生：
/// 那正是「删掉它就没水印」那条路的另一个入口。
WatermarkPlan stage_watermark(const std::filesystem::path& image_path,
                              int target_w, int target_h);

/// 把水印叠在一条滤镜链后面。`plan` 空就原样返回 `chain`（一个字都不多，
/// 这样"没水印"和改动前逐字节一样）。
///
/// `chain` 是水印**之前**那一段滤镜；没有别的滤镜就传 `"null"`（空串会拼出
/// `[cjwm_base];…`，那是个语法错）。
std::string with_watermark(const std::string& chain, const WatermarkPlan& plan);

}  // namespace changji::media
