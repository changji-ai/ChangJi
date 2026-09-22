#include "util/sysstat.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "util/proc.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif

namespace changji::sysstat {

namespace {

constexpr double kGb = 1024.0 * 1024.0 * 1024.0;

std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::optional<CpuTicks> read_cpu_ticks() {
#if defined(_WIN32)
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!::GetSystemTimes(&idle, &kernel, &user)) return std::nullopt;
    const auto to_u64 = [](const FILETIME& t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) |
               static_cast<unsigned long long>(t.dwLowDateTime);
    };
    CpuTicks c;
    c.idle = to_u64(idle);
    // kernel 那一项**已经包含 idle**，这是 Win32 的定义，不是笔误。
    c.total = to_u64(kernel) + to_u64(user);
    return c;
#elif defined(__linux__)
    return parse_proc_stat(read_file("/proc/stat"));
#elif defined(__APPLE__)
    // Mac 上原来落在下面那个 `return std::nullopt` 里，顶栏 CPU 那块表
    // 永远是"—"。和内存那一项一样，是当初"手上没有 Mac"留下的空缺。
    //
    // host_statistics 的 HOST_CPU_LOAD_INFO 给的是开机以来四档的累计
    // 滴答（user / system / idle / nice），和 /proc/stat 是一回事，
    // 所以上面那个差值算法一个字都不用改。
    host_cpu_load_info_data_t info{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (::host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                          reinterpret_cast<host_info_t>(&info),
                          &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    CpuTicks c;
    c.idle = info.cpu_ticks[CPU_STATE_IDLE];
    c.total = 0;
    for (int i = 0; i < CPU_STATE_MAX; ++i) c.total += info.cpu_ticks[i];
    return c;
#else
    return std::nullopt;
#endif
}

/// 上一次的 CPU 采样。百分比是两次之间的差值，所以要记着上一次。
std::mutex g_cpu_mu;
std::optional<CpuTicks> g_cpu_last;

double cpu_percent_since_last() {
    const auto now = read_cpu_ticks();
    if (!now) return -1;
    std::lock_guard<std::mutex> lk(g_cpu_mu);
    double pct = -1;
    if (g_cpu_last && now->total > g_cpu_last->total && now->idle >= g_cpu_last->idle) {
        const double dt = static_cast<double>(now->total - g_cpu_last->total);
        const double di = static_cast<double>(now->idle - g_cpu_last->idle);
        pct = std::clamp((1.0 - di / dt) * 100.0, 0.0, 100.0);
    }
    g_cpu_last = now;
    return pct;
}

#if defined(__linux__)
/// 容器里 /proc/meminfo 是宿主机的。cgroup 有上限的话按上限算——
/// AutoDL 无卡模式给 2 GiB，而 meminfo 写着 754 GB，看着像永远用不满。
/// {上限, 当前用量}，单位 GB。没上限回 nullopt。
std::optional<std::pair<double, double>> cgroup_memory() {
    // v2 在前：现在的机器基本都是 v2；v1 那对文件只在老内核上有。
    if (auto lim = parse_cgroup_limit(read_file("/sys/fs/cgroup/memory.max"))) {
        const std::string cur = trim(read_file("/sys/fs/cgroup/memory.current"));
        if (cur.empty()) return std::nullopt;
        return std::make_pair(static_cast<double>(*lim) / kGb,
                              std::strtod(cur.c_str(), nullptr) / kGb);
    }
    if (auto lim = parse_cgroup_limit(
            read_file("/sys/fs/cgroup/memory/memory.limit_in_bytes"))) {
        const std::string cur =
            trim(read_file("/sys/fs/cgroup/memory/memory.usage_in_bytes"));
        if (cur.empty()) return std::nullopt;
        return std::make_pair(static_cast<double>(*lim) / kGb,
                              std::strtod(cur.c_str(), nullptr) / kGb);
    }
    return std::nullopt;
}
#endif

void read_memory(Load& out) {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (::GlobalMemoryStatusEx(&m)) {
        out.mem_total_gb = static_cast<double>(m.ullTotalPhys) / kGb;
        out.mem_used_gb = static_cast<double>(m.ullTotalPhys - m.ullAvailPhys) / kGb;
    }
#elif defined(__linux__)
    if (auto mi = parse_meminfo(read_file("/proc/meminfo"))) {
        out.mem_total_gb = mi->first;
        out.mem_used_gb = std::max(0.0, mi->first - mi->second);
    }
    if (auto cg = cgroup_memory()) {
        // 上限比物理内存还大的话那不是限制，是 v1 那种"没设"的写法漏过来了
        if (out.mem_total_gb <= 0 || cg->first < out.mem_total_gb) {
            out.mem_total_gb = cg->first;
            out.mem_used_gb = cg->second;
        }
    }
#elif defined(__APPLE__)
    unsigned long long total = 0;
    std::size_t len = sizeof(total);
    if (::sysctlbyname("hw.memsize", &total, &len, nullptr, 0) == 0) {
        out.mem_total_gb = static_cast<double>(total) / kGb;
    }
    // 用量这一项原来是空着的（注释写的是"手上没有 Mac 验不了"）。表现是
    // 顶栏上内存那块表永远是 `0 / 128 GB`——**而那比不显示更糟**：
    // 一个一直是 0 的数看着像"内存没被用"，不像"这一项没接"。
    // 2026-09-12 在 M3 Max 上补上，下面每个数都和活动监视器对过。
    //
    // **口径跟活动监视器的「已使用内存」走**，因为用户就是拿它对的：
    //
    //     已用 = 应用内存 + 联动内存 + 已压缩
    //          = (internal - purgeable) + wire + compressor
    //
    // ⚠️ **不能用 `total - free`**：macOS 上 free 常年只有几百 MB（空闲内存
    // 都被拿去当缓存了），那样算出来永远是 99%，而机器一点都不紧张。
    // 也不能照搬 hardware.cpp 里 parse_vm_stat 那个式子——那个算的是
    // "还能腾出多少给 GPU"，把 inactive 和 speculative 全当可用，
    // 是另一个问题的答案。
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (::host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
        ::host_statistics64(mach_host_self(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&vm),
                            &count) == KERN_SUCCESS) {
        const double used_pages =
            static_cast<double>(vm.internal_page_count) -
            static_cast<double>(vm.purgeable_count) +
            static_cast<double>(vm.wire_count) +
            static_cast<double>(vm.compressor_page_count);
        if (used_pages > 0) {
            out.mem_used_gb = used_pages * static_cast<double>(page) / kGb;
        }
    }
#endif
}

/// 每张卡。NVML 在前；没有 NVML 才退回 nvidia-smi，而且**五秒才问一次**：
/// fork 一次一百毫秒，两秒一问的话它自己就是负载。
std::vector<GpuLoad> gpus_now() {
    auto live = models::gpu_live();
    if (!live.empty()) return live;

    static std::mutex mu;
    static std::chrono::steady_clock::time_point at;
    static std::vector<GpuLoad> cache;
    static bool tried = false;
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (tried && now - at < std::chrono::seconds(5)) return cache;
    tried = true;
    at = now;
    cache.clear();
    if (!proc::which("nvidia-smi")) return cache;
    auto r = proc::run("nvidia-smi",
                       {"--query-gpu=index,name,utilization.gpu,memory.used,memory.total",
                        "--format=csv,noheader,nounits"},
                       3000);
    if (r.launched && !r.timed_out) cache = parse_nvidia_smi(r.out);
    return cache;
}

}  // namespace

Load sample() {
    Load l;
    l.cpu_percent = cpu_percent_since_last();
    read_memory(l);
    l.gpus = gpus_now();
    return l;
}

namespace {

/// 多久采一次。
///
/// 比推送那条的两秒略快一点，这样每次推送手上都有一份刚采的。再快没意义
/// ——顶栏那三个小表是给人看的，不是给示波器看的。
constexpr int kSampleEveryMs = 1500;

struct Sampler {
    std::mutex mu;
    std::condition_variable cv;
    Load last;
    std::chrono::steady_clock::time_point at{};
    bool have = false;
    bool stop = false;
    std::thread th;
};

Sampler& sam() {
    static Sampler s;
    return s;
}

}  // namespace

void start_sampler() {
    Sampler& s = sam();
    if (s.th.joinable()) return;   // 已经在跑
    {
        std::lock_guard lg(s.mu);
        s.stop = false;
    }
    s.th = std::thread([&s] {
        for (;;) {
            // **在锁外采。** 这一下可能要十几秒（卡满负荷时 NVML 被驱动
            // 挂住），持着锁的话读缓存的人跟着一起卡，等于白做。
            Load l = sample();
            {
                std::lock_guard lg(s.mu);
                if (s.stop) return;
                s.last = std::move(l);
                s.at = std::chrono::steady_clock::now();
                s.have = true;
            }
            std::unique_lock lk(s.mu);
            s.cv.wait_for(lk, std::chrono::milliseconds(kSampleEveryMs),
                          [&s] { return s.stop; });
            if (s.stop) return;
        }
    });
}

void stop_sampler() {
    Sampler& s = sam();
    if (!s.th.joinable()) return;
    {
        std::lock_guard lg(s.mu);
        s.stop = true;
    }
    s.cv.notify_all();
    // **可能要等上十几秒**：它多半正挂在 NVML 上，而那一下打不断。
    // 关停时等着是对的——不等就是在它还在写 last 的时候析构掉它。
    s.th.join();
}

Load latest() {
    Sampler& s = sam();
    {
        std::lock_guard lg(s.mu);
        if (s.have) {
            Load l = s.last;
            l.age_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - s.at)
                          .count();
            return l;
        }
    }
    // 采样器没起：命令行和单元测试走这条，当场采。
    return sample();
}

nlohmann::json to_json(const Load& l) {
    nlohmann::json gpus = nlohmann::json::array();
    for (const auto& g : l.gpus) {
        gpus.push_back({{"index", g.index},
                        {"name", g.name},
                        {"util_percent", g.util_percent},
                        {"vram_used_gb", g.vram_used_gb},
                        {"vram_total_gb", g.vram_total_gb}});
    }
    return {{"cpu_percent", l.cpu_percent},
            {"mem_used_gb", l.mem_used_gb},
            {"mem_total_gb", l.mem_total_gb},
            {"gpus", gpus},
            // 这份读数多旧了。见 Load::age_s——界面靠它把"读不动"和
            // "真没在动"分开。
            {"age_s", l.age_s}};
}

std::optional<CpuTicks> parse_proc_stat(const std::string& text) {
    // 第一行："cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag != "cpu") return std::nullopt;
    unsigned long long v[8] = {};
    int n = 0;
    while (n < 8 && (ls >> v[n])) ++n;
    if (n < 4) return std::nullopt;
    CpuTicks c;
    c.idle = v[3] + v[4];   // idle + iowait
    for (int i = 0; i < n; ++i) c.total += v[i];
    return c;
}

std::optional<std::pair<double, double>> parse_meminfo(const std::string& text) {
    // "MemTotal:       65536000 kB"
    std::optional<double> total;
    std::optional<double> avail;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        unsigned long long kb = 0;
        if (!(ls >> key >> kb)) continue;
        if (key == "MemTotal:") total = static_cast<double>(kb) / (1024.0 * 1024.0);
        else if (key == "MemAvailable:") avail = static_cast<double>(kb) / (1024.0 * 1024.0);
        if (total && avail) break;
    }
    if (!total || !avail) return std::nullopt;
    return std::make_pair(*total, *avail);
}

std::optional<unsigned long long> parse_cgroup_limit(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty() || t == "max") return std::nullopt;
    for (char c : t) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    const unsigned long long v = std::strtoull(t.c_str(), nullptr, 10);
    // v1 没设上限时写的是 2^63 附近的一个数（9223372036854771712）。
    // 超过 2^60（1 EiB）的都当没设。
    if (v == 0 || v > (1ULL << 60)) return std::nullopt;
    return v;
}

std::vector<GpuLoad> parse_nvidia_smi(const std::string& out) {
    // "0, NVIDIA GeForce RTX 5090, 82, 31500, 32607"
    std::vector<GpuLoad> gpus;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> cols;
        std::string cur;
        std::istringstream ls(line);
        while (std::getline(ls, cur, ',')) cols.push_back(trim(cur));
        if (cols.size() < 5) continue;
        GpuLoad g;
        g.index = static_cast<unsigned int>(std::strtoul(cols[0].c_str(), nullptr, 10));
        g.name = cols[1];
        // "[N/A]" 那种直接留 -1
        if (!cols[2].empty() && std::isdigit(static_cast<unsigned char>(cols[2][0]))) {
            g.util_percent = std::atoi(cols[2].c_str());
        }
        g.vram_used_gb = std::strtod(cols[3].c_str(), nullptr) / 1024.0;
        g.vram_total_gb = std::strtod(cols[4].c_str(), nullptr) / 1024.0;
        gpus.push_back(std::move(g));
    }
    return gpus;
}

}  // namespace changji::sysstat
