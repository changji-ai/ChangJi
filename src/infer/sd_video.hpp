#pragma once

#include <cstdint>

// 用 sd.cpp 出视频，再交给 ffmpeg 编码成 mp4。
//
// sd.cpp 的 generate_video 吐的是**裸帧数组**，不是编码好的视频
// （ComfyUI 那条路由 SaveVideo 节点负责编码）。所以这里多一步。
//
// 编码走的是"裸帧写临时文件 + ffmpeg -f rawvideo"，不是管道：
// Windows 上父进程写 stdin、子进程写 stderr，两边都不读对方就会
// 互相阻塞死锁，而那个死锁只在帧数多到填满管道缓冲时才出现——
// 也就是只在真实负载下出现，测试里撞不到。
// 也不是先编码成 PNG 再喂给 ffmpeg：121 帧的 PNG 编码要好几秒 CPU，
// 而裸帧直接落盘只是一次顺序写。

#include <filesystem>
#include <string>

#include "config/settings.hpp"
#include "infer/exec_queue.hpp"
#include "infer/sd_image.hpp"
#include "media/watermark.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "stages/render.hpp"

namespace changji::infer {

/// 用 sd.cpp 出视频的那个 renderer，接给 stages::render_batch。
///
/// assembly 里的编码参数（编码器、crf、像素格式）从配置来——
/// 每一镜的片段和最后拼起来的成片必须用同一套规格，
/// 否则拼接环节要重编码，那是白白多一次有损压缩。
stages::VideoRenderer sd_video_renderer(const config::Settings& settings);

/// 同上，但**种子由外面给**。工作进程用这个——它拿不到 `attempts`，
/// 自己算的种子和串行跑的不一样，而且不会有任何报错。
stages::VideoRenderer sd_video_renderer_with_seed(
    const config::Settings& settings, std::int64_t seed,
    Origin origin = Origin::Local);

/// 把裸 RGB 帧编码成 mp4。
///
/// 单独暴露是为了能测：编码参数拼错了不会当场报错，
/// 只会让产出的 mp4 在某些播放器上打不开，或者拼接时被重编码。
///
/// raw_path 里是连续的 RGB24 帧。`audio` 给了就把那条 wav 一起封进去
/// （模型自己出的原生音轨，见 SdContext::generate_video），按视频的
/// 时长截齐——`duration_s` 就是帧数 / 帧率，声音多出来的尾巴不要。
void encode_raw_to_mp4(const std::filesystem::path& raw_path, int width,
                       int height, int fps,
                       const config::AssemblyConfig& assembly,
                       const std::filesystem::path& dest,
                       const std::optional<std::filesystem::path>& audio = std::nullopt,
                       double duration_s = 0.0);

/// 拼给 ffmpeg 的参数。测试拿它检查规格，不用真跑 ffmpeg。
///
/// **产物的标识在这一步加**：`watermark` 是画面上那个角标（空 = 不加，参数
/// 和加它之前逐字节一样），`tags` 是写进元数据的隐式那一半。
///
/// 为什么是这儿：这是一镜从 rawvideo 变成 mp4 的**唯一一次编码**，加在这里
/// 不多一次重编码；而进程内出片和走工作进程出片都落到这个函数上
/// （`run_deps.cpp` / `task_run.cpp` 两条路都走 sd_video），没有第三条路。
std::vector<std::string> encode_args(const std::filesystem::path& raw_path,
                                     int width, int height, int fps,
                                     const config::AssemblyConfig& assembly,
                                     const std::filesystem::path& dest,
                                     const std::optional<std::filesystem::path>& audio = std::nullopt,
                                     double duration_s = 0.0,
                                     const media::WatermarkPlan& watermark = {},
                                     const std::vector<std::string>& tags = {});

}  // namespace changji::infer
