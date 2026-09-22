#pragma once

// stable-diffusion.cpp 的门面。
//
// 这一层存在的理由是**把编译期的可选性关在一个文件里**。
// sd.cpp 是可以不链的（CHANGJI_SD=OFF，交叉编译到某些平台时用得上），
// 而"能不能出图"这件事流水线各处都要问。没有这一层的话，
// `#ifdef CHANGJI_HAVE_SD` 会散布到调用点上，每加一处就多一份漏写的机会。
//
// 所以：**这个头文件不带任何 #ifdef，也不 include sd.cpp 的头**。
// 没链的时候 available() 返回 false，别的函数返回空值。调用方照常写。

#include <functional>
#include <string>
#include <vector>

namespace changji::infer {

/// sd.cpp 链进来了没有。编译期决定，运行期不变。
bool sd_available();

/// 版本号。没链时返回空串。
std::string sd_version();

/// 编译进去的后端和 CPU 特性，形如
/// "AVX2 = 1 | CUDA = 1 | ..."。诊断用，原样显示。
std::string sd_system_info();

/// 可用的采样器名字。界面上要给人选。
std::vector<std::string> sd_sample_methods();

/// 可用的调度器名字。
std::vector<std::string> sd_schedulers();

/// sd.cpp 的日志接到这里来。
///
/// 不接的话它默认往 stderr 打，而那是**流水线跑起来之后最吵的一路输出**
/// ——每个采样步一行。接过来才能按 job 分流到 WebSocket，
/// 以及在不需要的时候关掉。
///
/// level: 0=debug 1=info 2=warn 3=error
using SdLogSink = std::function<void(int level, const std::string& text)>;
void sd_set_log_sink(SdLogSink sink);

/// 装一个把 sd.cpp 的日志写到 stderr 的 sink。
///
/// **不装的话一条都不会落地**：trampoline 拿不到 sink 就把消息丢了，
/// 而出图失败时抛的是"看一眼上面 sd.cpp 打的日志"——上面什么都没有。
///
/// `min_level` 是压过之后的四档（0 debug / 1 info / 2 warn / 3 error）。
/// 默认 1：debug 那一档是逐层加载张量的流水账，一次几百行。
void sd_log_to_stderr(int min_level = 1);

}  // namespace changji::infer
