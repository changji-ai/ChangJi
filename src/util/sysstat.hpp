// 机器此刻的负载：CPU、内存、每张卡。顶栏那三个小表用（GET /api/system）。
//
// 用户 2026-09-11：「在顶部导航栏增加 GPU、CPU、内存的使用情况实时显示」。
//
// 两秒一问，所以每一条都得便宜：CPU 读 /proc/stat 差值（Windows 走
// GetSystemTimes），内存读 /proc/meminfo（容器里按 cgroup 上限算），
// 显卡走 NVML 进程内问。唯一会 fork 的是 nvidia-smi 那条退路，限了五秒一次。

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"

namespace changji::sysstat {

using GpuLoad = models::GpuLive;

struct Load {
    double cpu_percent = -1;   ///< -1 = 还没有差值（第一次采样）或者这平台问不到
    double mem_used_gb = 0;
    double mem_total_gb = 0;
    std::vector<GpuLoad> gpus;
    /// 这份读数是多久以前采的（秒）。当场采的是 0。
    ///
    /// **卡忙的时候这个数会变大**，而那正是它存在的理由：显卡满负荷时问
    /// NVML 会被驱动挂住好几秒（实测服务器上大模型生成时 13 秒都有），
    /// 那几秒里顶栏拿到的必然是旧读数。不说的话用户看到的是一个不动的
    /// 数字——2026-09-11 就为这个来问过一次「GPU 基本 0、显存基本 22.1
    /// 都没变过」。说出来，"读不动"和"真没在动"才分得开。
    double age_s = 0;
};

/// 采一次样。CPU 是和上一次采样之间的差值，第一次调回 -1。
///
/// ⚠️ **可能很慢**：问 NVML 在显卡满负荷时会被驱动挂住好几秒。服务里不要
/// 直接调它，走 latest()。
Load sample();

/// 起一条后台采样线程。**服务起来时调一次。**
///
/// 为什么非要一条自己的线程：问卡慢的时候，谁调 sample() 谁被挂住。以前
/// 两个地方各自调——每两秒推一次的那条 WebSocket，和 `/api/system`——于是
/// 大模型一开始生成，`/api/system` 就从 1 毫秒变成十几秒（实测 13.4 秒，
/// 还有一次超过 20 秒），而推送那条同样被挂住、界面上那块表直接空掉
/// （前端八秒没消息就当断了）。一条线程采、所有人读缓存，问卡再慢也只
/// 拖住它自己。
void start_sampler();
/// 停掉并等它退出。优雅关停时调。
void stop_sampler();

/// 最近一次采到的。采样器没起（命令行、测试）就当场采一次。
Load latest();

nlohmann::json to_json(const Load& load);

// ---- 纯函数，给测试用 ----

struct CpuTicks {
    unsigned long long idle = 0;
    unsigned long long total = 0;
};

/// /proc/stat 第一行。idle 含 iowait，total 是前八列之和。
std::optional<CpuTicks> parse_proc_stat(const std::string& text);

/// /proc/meminfo 的 MemTotal 和 MemAvailable，单位 GB。{total, available}。
std::optional<std::pair<double, double>> parse_meminfo(const std::string& text);

/// cgroup 的内存上限（memory.max / memory.limit_in_bytes 的内容）。
/// "max" 或者大得离谱（没设上限时 v1 写的是 2^63 附近的数）回 nullopt。
std::optional<unsigned long long> parse_cgroup_limit(const std::string& text);

/// `nvidia-smi --query-gpu=index,name,utilization.gpu,memory.used,memory.total
/// --format=csv,noheader,nounits` 的输出，一张卡一行。
std::vector<GpuLoad> parse_nvidia_smi(const std::string& out);

}  // namespace changji::sysstat
