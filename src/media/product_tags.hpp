#pragma once

#include <string>
#include <vector>

namespace changji::media {

/// 产物的**隐式标识**：写进文件元数据的那几栏。
///
/// 画面上那个角标是显式的那一半（media/watermark.hpp），这是另一半。它不占
/// 画面、不影响观感，所以**哪儿都写、永远写**——显式那半将来要是按授权关掉
/// 了，这半也还在。
///
/// 三条实测出来的规矩，每一条都是"错了不报错"：
///
/// ⚠️ **只能用容器认得的标准栏。** mp4 的 muxer 把自定义 key 直接丢掉——
/// `-metadata cj_ai_generated=true` 写完就没了，而 ffmpeg 一声不响。
///
/// ⚠️ **别拿 `creation_time` 记生成时间。** 烧字幕那一步重编码之后它就没了。
/// 时间要写在 comment / description 的正文里才留得住。
///
/// ⚠️ **这几句不走 SAY。** 元数据是给机器读的：跟着界面语言变的话，同一批
/// 产物在不同机器上写出不同的字，谁也解析不了。中英各写一遍，写死。
///
/// 返回的是插在输出文件名**之前**的那几个参数（`-metadata` 是输出选项，
/// 摆到文件名后面 ffmpeg 会当成下一个输出的选项）。
std::vector<std::string> product_tag_args(const std::string& stamp,
                                          const std::string& title = {});

}  // namespace changji::media
