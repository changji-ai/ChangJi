#include "util/say.hpp"
#include "models/hardware.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

#include "util/paths.hpp"
#include "util/proc.hpp"

// NVML（显卡驱动自带的那个库）**运行时加载**，不在链接期依赖它。
// 见下面 Nvml 上面那段。Mac 上根本没有这条路，整块编译掉。
#if !defined(__APPLE__)
#if defined(_WIN32)
// **这两个必须在 windows.h 之前。** 不定 NOMINMAX 的话它会把 min/max
// 定义成宏，这个文件下面那些 std::min / std::max 当场编不过
// （报的是"':' 右边的非法标记"，看不出跟 windows.h 有关系）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace changji::models {

namespace {

/// Python 的 round(x, 1)：先按 1 位小数做银行家舍入。
///
/// 不能写成 std::round(x*10)/10——那是"远离零"的舍入，
/// 在 .05 结尾的数上和 Python 不一致。std::nearbyint 用的是当前舍入模式，
/// IEEE 754 默认就是取偶，与 Python 相同。
double round1(double x) {
    return std::nearbyint(x * 10.0) / 10.0;
}

// 按显存分的档位表。第一个元素是该档位要求的最低显存 GB。
// 显存越小，分辨率越保守，否则会 OOM 或者慢到不可用。
//
// 阈值全部比标称容量低 0.5，因为驱动和固件会占掉一部分，
// nvidia-smi 报出来的永远小于标称值。一张 16GB 的卡通常报 15.9，
// 按 16.0 卡阈值会把它错判成 12GB 档。
//
// ---
//
// **成片档的步数四行填的是同一个数，这是故意的。**
//
// 显存决定得了分辨率——装不下就得降；但决定不了步数：同一个模型在 32 GB
// 卡和 140 GB 卡上要的步数一样，只是快慢不同。原来这一列是 30/30/28/25，
// 换大卡会"自动"拿到不同的步数，那没有道理，而且 30 这个数是 Wan 时代
// 留下的，换模型时没人跟着改（和 kMaxFrames = 121 同一个来历）。
//
// 20 是 MiniMax-H3 的标准步数。**官方没有公布推荐值**（diffusers 的参数表里
// num_inference_steps 没有 defaults 标注，示例代码也都不传），20 来自社区
// 的 baseline（res_multistep + simple）。挂 Turbo 那类蒸馏 LoRA 时不走这个数
// ——effective_spec 会换成 6，见那里的注释。
//
// 草稿档的步数是**我们自己为了省时间定的**，和模型的标准值无关：
// 草稿只用来看构图和叙事，糊一点无所谓。
//
// **中间那档（preview）2026-09-13 删了。** 表里原来还有一列
// 1280×704/20、960×544/20、768×432/18、640×352/16，整棵树没有一处用
// 过它。留着的唯一效果是 /api/hardware 多回一档：实测 5090 上报
// 1280×704 / 20 步 / 207 秒，比成片档的 544×928 / 6 步 / 96.6 秒又大
// 又慢——因为成片档出去之前会被项目画幅和 Turbo 盖掉，而它不会。
struct TierRow {
    double threshold;
    int draft[3];    // w, h, steps
    int final_[3];
};

const TierRow kTierTable[] = {
    {23.5, {768, 432, 10},  {1920, 1088, 20}},
    {15.5, {640, 352, 10},  {1280, 704, 20}},
    {11.5, {512, 288, 8},   {960, 544, 20}},
    {7.5,  {448, 256, 8},   {768, 432, 20}},
};
constexpr int kTierRows = static_cast<int>(sizeof(kTierTable) / sizeof(kTierTable[0]));

/// 基准档在表里的下标。耗时估算以它为原点，从表里取而不是另写一个数字，
/// 避免改表后两处对不上。
constexpr int kReferenceIndex = 1;

const int* row_for(const TierRow& row, Tier t) {
    switch (t) {
        case Tier::DRAFT:   return row.draft;
        case Tier::FINAL:   return row.final_;
    }
    return row.draft;
}

/// 该档位在 RTX 5080 16GB 上的实测单镜耗时。
double reference_seconds(Tier t) {
    switch (t) {
        case Tier::DRAFT:   return 27.0;
        case Tier::FINAL:   return 392.0;
    }
    return 0.0;
}

/// 粗估单镜耗时。
///
/// 仅用于给用户一个数量级预期和排产估算，不是承诺。真实数字要靠标定
/// 在目标机器上实测。扩散模型耗时大致正比于像素数乘步数。
double estimate_seconds(Tier tier, int w, int h, int steps, double vram_gb) {
    const int* ref = row_for(kTierTable[kReferenceIndex], tier);
    const double ref_work =
        static_cast<double>(round32(ref[0])) * round32(ref[1]) * ref[2];
    const double work = static_cast<double>(w) * h * steps;
    double base = reference_seconds(tier) * (work / ref_work);

    // 比基准档更小的机器通常算力也弱，且要频繁换入换出，给一个惩罚系数。
    // 基准档自身不吃惩罚，否则实测值会被凭空放大。
    const double ref_vram = kTierTable[kReferenceIndex].threshold;
    if (vram_gb < ref_vram) {
        base *= 1.0 + (ref_vram - vram_gb) * 0.08;
    }
    return round1(base);
}

std::string fmt1(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string fmt0(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

/// 左对齐补空格到指定宽度，对应 Python 的 f"{s:8s}"。
std::string pad_right(const std::string& s, std::size_t width) {
    std::string out = s;
    while (out.size() < width) out.push_back(' ');
    return out;
}

}  // namespace

const char* to_string(Tier v) {
    switch (v) {
        case Tier::DRAFT:   return "draft";
        case Tier::FINAL:   return "final";
    }
    return "?";
}

const std::vector<Tier>& all_tiers() {
    static const std::vector<Tier> kAll = {Tier::DRAFT, Tier::FINAL};
    return kAll;
}

int round32(int n) {
    // nearbyint 走当前舍入模式，IEEE 754 默认取偶，与 Python 的 round() 一致。
    const double r = std::nearbyint(static_cast<double>(n) / 32.0) * 32.0;
    return std::max(32, static_cast<int>(r));
}

TierSpec TierSpec::scaled_to(const std::string& aspect_ratio) const {
    TierSpec out = *this;
    if (aspect_ratio == "9:16") {
        const int long_side = std::max(width, height);
        const int short_side = std::min(width, height);
        out.width = round32(short_side);
        out.height = round32(long_side);
    } else if (aspect_ratio == "1:1") {
        // 注意 Python 用的是整除 //，先截断再规整
        const int side = round32((width + height) / 2);
        out.width = side;
        out.height = side;
    } else {  // 16:9
        const int long_side = std::max(width, height);
        const int short_side = std::min(width, height);
        out.width = round32(long_side);
        out.height = round32(short_side);
    }
    return out;
}

#if defined(__APPLE__)
// **必须写绝对路径。** sysctl 在 /usr/sbin，而子进程拿到的最小 PATH 里
// 没有 /usr/sbin（实测是 /usr/gnu/bin:/usr/local/bin:/bin:/usr/bin:.）。
// 只写名字的话 proc::run 找不到它，探测直接失败——**而失败是静默的**，
// 表现就是"未探测到显卡，按 12 GB 估算"，一台 128 GB 的 Mac 被当成 12 GB。
// 2026-09-11 就是这么查出来的：Metal 明明起来了，显存却一直是 12 GB。
constexpr const char* kSysctl = "/usr/sbin/sysctl";
constexpr const char* kVmStat = "/usr/bin/vm_stat";

/// Apple Silicon 的「显存」。
///
/// **这台机器上没有 nvidia-smi，而以前的代码只认它**：探不到就退回
/// "按 12 GB 估算"。于是一台 128 GB 的 Mac 被当成 12 GB，档位、权重放哪、
/// "显存够就不用清理"全部按 12 GB 算——而这**不报错**，只是什么都跑不大。
///
/// 苹果芯片是统一内存：CPU 和 GPU 共用同一块，没有独立显存这回事。
/// GPU 能一次占住多少由 Metal 说了算（`recommendedMaxWorkingSetSize`），
/// **所以直接问它**，见 hardware_metal.mm。
///
/// ⚠️ **原来这里是拿 sysctl 算的，而那个算法从头到尾没生效过。**
/// 它查 `iogpu.wired_limit_pct`，而那个 OID 在 macOS 26 上已经不存在
/// （现在叫 `iogpu.wired_limit_mb`）——sysctl 报 unknown oid，代码静默
/// 退回写死的 75%。这台 128 GB 的 M3 Max 上：算出 96 GB，Metal 说 107.5 GB。
/// 少认 11.5 GB，没有任何报错。**兜底比例永远是这样：它不会响，只会错。**
///
/// **Intel Mac 不走这条**：那些机器要么是独显（另说），要么核显性能
/// 根本跑不动这套东西。所以先卡 brand_string 那道门，再问 Metal——
/// 顺序不能反：Intel Mac 上 Metal 照样答得出一个数，只是那个数
/// 和这套东西能不能跑没关系。
std::optional<GPUInfo> detect_apple_gpu() {
    const bool dbg = !paths::env("CHANGJI_DEBUG_HW").empty();
    const auto sysctl_num = [dbg](const char* key) -> std::optional<std::uint64_t> {
        auto r = proc::run(kSysctl, {"-n", key}, 5000);
        if (dbg) {
            std::fprintf(stderr, "[hw] sysctl %s: launched=%d exit=%d out=[%s]\n",
                         key, static_cast<int>(r.launched), r.exit_code,
                         r.out.c_str());
        }
        if (!r.launched || r.exit_code != 0) return std::nullopt;
        std::string t;
        for (char c : r.out) {
            if (std::isdigit(static_cast<unsigned char>(c))) t += c;
        }
        if (t.empty()) return std::nullopt;
        try {
            return std::stoull(t);
        } catch (...) {
            return std::nullopt;
        }
    };

    // 芯片名。拿不到就给个能看的兜底，不影响算数。
    std::string name = "Apple Silicon";
    auto br = proc::run(kSysctl, {"-n", "machdep.cpu.brand_string"}, 5000);
    if (dbg) {
        std::fprintf(stderr, "[hw] brand: launched=%d exit=%d out=[%s]\n",
                     static_cast<int>(br.launched), br.exit_code, br.out.c_str());
    }
    if (auto& r = br; r.launched && r.exit_code == 0) {
        std::string t = r.out;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                              t.back() == ' ')) {
            t.pop_back();
        }
        // 只认苹果自家的芯片。Intel Mac 的 brand_string 是 "Intel(R) Core..."，
        // 那种机器不该按统一内存算。
        if (t.rfind("Apple", 0) != 0) return std::nullopt;
        if (!t.empty()) name = t;
    } else {
        return std::nullopt;
    }

    // 整机物理内存。这一项是**给人看的**："我买的是 128 GB"。
    // 预算不按它算，按下面 Metal 给的那条线。
    const auto total = sysctl_num("hw.memsize");
    if (!total.has_value() || *total == 0) return std::nullopt;

    GPUInfo g;
    g.count = 1;
    g.unified_mb = static_cast<int>(*total / (1024ull * 1024ull));

    // **先问 Metal。** 它答得出就到此为止，一个兜底比例都不用猜。
    if (const auto m = metal_memory(); m.has_value()) {
        if (!m->name.empty()) name = m->name;
        g.name = SAYF("%1（统一内存）", name);
        g.vram_mb = static_cast<int>(m->max_working_set / (1024ull * 1024ull));
        // 万一是独显（Intel Mac 上的 AMD 卡），那就不是统一内存，
        // 整机内存这一项对它没意义，清掉免得界面上显示一个误导的数。
        if (!m->unified) g.unified_mb = 0;
        if (dbg) {
            std::fprintf(stderr,
                         "[hw] metal: working_set=%llu allocated=%llu unified=%d\n",
                         static_cast<unsigned long long>(m->max_working_set),
                         static_cast<unsigned long long>(m->allocated),
                         static_cast<int>(m->unified));
        }
        return g;
    }

    // 退路：连 Metal 设备都没有的机器（10.11 以后基本不存在，但探测这一层
    // 不该因为"基本不存在"就没有下限）。
    //
    // **OID 是 `iogpu.wired_limit_mb`，不是 `..._pct`。** 后者是老系统的
    // 名字，在 macOS 26 上查它只会得到 unknown oid。0 表示"系统自己定"。
    // 都读不到就按 84% —— 这个数是本机 M3 Max 上从 Metal 读回来反推的
    // （107.5 / 128），**不是苹果承诺的比例**，所以只配当兜底。
    double usable_bytes = static_cast<double>(*total) * 0.84;
    if (const auto mb = sysctl_num("iogpu.wired_limit_mb");
        mb.has_value() && *mb > 0) {
        usable_bytes = static_cast<double>(*mb) * 1024.0 * 1024.0;
    }
    g.name = SAYF("%1（统一内存）", name);
    g.vram_mb = static_cast<int>(usable_bytes / (1024.0 * 1024.0));
    return g;
}

/// 这台机器的 GPU 最多能占多少（GB）。**算一次存着。**
///
/// 只有一个用处：给 free_vram_gb 里那条 vm_stat 退路当上限。所以可以缓存
/// ——Metal 那条线在一次运行里不会变，而这个函数的调用点在借槽的路径上。
std::optional<double> apple_working_set_gb() {
    static const std::optional<double> cached = []() -> std::optional<double> {
        if (const auto m = metal_memory(); m.has_value()) {
            return static_cast<double>(m->max_working_set) /
                   (1024.0 * 1024.0 * 1024.0);
        }
        if (const auto g = detect_apple_gpu(); g.has_value()) return g->vram_gb();
        return std::nullopt;
    }();
    return cached;
}
#endif

#if !defined(__APPLE__)
/// 定义在下面 Nvml 那个类后面（它要用到那个类，而这里比它早）。
std::optional<GPUInfo> detect_gpu_nvml();
#endif

std::optional<GPUInfo> detect_gpu() {
#if defined(__APPLE__)
    // 苹果机器优先按统一内存算。**放在 nvidia-smi 之前**：有人在 Mac 上
    // 装过 nvidia 的工具链，那时候 nvidia-smi 在但没有 N 卡，
    // 探出来的是空的，反而把统一内存那条盖掉。
    if (auto apple = detect_apple_gpu(); apple.has_value()) return apple;
#else
    // **先问 NVML，问不到再 fork nvidia-smi。**
    //
    // 这条和 Mac 那条是同一个毛病的两面：**总量和空闲来自两个不同的源。**
    // 空闲那条（free_vram_gb / vram_totals_gb）早就走 NVML 了，只有这条
    // "整卡多大"还在 fork nvidia-smi。于是：
    //
    //   - **nvidia-smi 不在 PATH 上就等于没有显卡。** 装了驱动没装 toolkit、
    //     容器里只挂了 /dev/nvidia*、Windows 上 PATH 没带那个 bin 目录——
    //     都会静默退回"按 12 GB 估算"。一张 96 GB 的卡被当成 12 GB 用，
    //     而这**不报错**，只是什么都跑不大（Mac 上那个 75% 一模一样）。
    //   - 两个数来自两套接口，口径对不上时没人发现。
    //   - fork 一次一百毫秒上下，而这个进程 CUDA 映射最满的时候 fork
    //     是 NVIDIA 明确不支持的做法（理由写在 Nvml 那个类上面）。
    //
    // NVML 拿不到才走老路——**没拆任何东西，只是把更稳的那条放在前面**。
    if (auto g = detect_gpu_nvml(); g.has_value()) return g;
#endif
    if (!proc::which("nvidia-smi")) return std::nullopt;

    auto r = proc::run("nvidia-smi",
                       {"--query-gpu=name,memory.total,driver_version",
                        "--format=csv,noheader,nounits"},
                       15000);
    if (!r.launched) return std::nullopt;
    return parse_gpu_query(r.out);
}

std::optional<double> parse_free_vram(const std::string& out) {
    // **这儿收到的已经是"自己那张卡"的一行了，挑卡不在这一层。**
    //
    // ⚠️ 这段注释原来写着「一张卡一行，只认第一行（卡 0）」「多卡时这个数
    // 可能是错的」「**上多卡之前必须先把这里改对**……现在不动，是因为手上
    // 这台是单卡」。**那三句现在全是过期的**：挑卡后来是在调用方做的，
    // `free_vram_gb()` 和 `vram_totals_gb()` 都先算
    // `visible_device_index()`（认不出来就直接回 nullopt，绝不猜），
    // 再用 `nth_gpu_line(输出, idx)` 取自己那一行，把**单独一行**交到这里。
    // 头文件里 parse_visible_devices 上面那段说的「实时空闲那条已经按绑定
    // 的卡取了」指的就是这件事。
    //
    // 留着那三句的代价是实打实的：它把一个**已经处理过**的 OOM 隐患说成
    // 「上多卡之前必须先改」，下一个读到的人要么白改一遍，要么真在这儿加
    // 一道"多卡就回 nullopt"——而这一层根本看不到几张卡，加完只会把两个
    // 已经对的调用方弄坏。
    //
    // 这个函数今天只干一件事：把 "12873" / "12873 MiB" 这样的一截解析成
    // GB，解不出来（空、N/A、0）回 nullopt。下面这句 substr 是**容忍行尾
    // 换行**，不是"挑第一张卡"。
    //
    // 还没做的那一半仍然在 `detect_gpu()`：名字和整卡容量还取第一行，
    // 混插不同型号的卡时会偏乐观。见头文件 parse_visible_devices 上面那段。
    const std::string first = out.substr(0, out.find('\n'));
    std::string trimmed;
    for (char c : first) {
        if (!std::isspace(static_cast<unsigned char>(c))) trimmed += c;
    }
    if (trimmed.empty()) return std::nullopt;
    // 末尾可能带 " MiB"，nounits 时没有；两种都吃。
    std::size_t digits = 0;
    while (digits < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[digits]))) {
        ++digits;
    }
    if (digits == 0) return std::nullopt;
    try {
        const double mib = std::stod(trimmed.substr(0, digits));
        if (mib <= 0) return std::nullopt;
        return mib / 1024.0;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> parse_vm_stat(const std::string& out) {
    if (out.empty()) return std::nullopt;

    // 头一行： "Mach Virtual Memory Statistics: (page size of 16384 bytes)"
    std::uint64_t page = 0;
    if (const auto pos = out.find("page size of"); pos != std::string::npos) {
        std::string digits;
        for (std::size_t i = pos; i < out.size() && out[i] != ')'; ++i) {
            if (std::isdigit(static_cast<unsigned char>(out[i]))) digits += out[i];
        }
        if (!digits.empty()) {
            try {
                page = std::stoull(digits);
            } catch (...) {
                page = 0;
            }
        }
    }
    if (page == 0) return std::nullopt;   // 读不到页大小就别猜

    const auto pages_of = [&out](const char* label) -> std::uint64_t {
        const auto pos = out.find(label);
        if (pos == std::string::npos) return 0;
        std::string digits;
        for (std::size_t i = pos + std::strlen(label); i < out.size(); ++i) {
            const char ch = out[i];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                digits += ch;
            } else if (!digits.empty()) {
                break;      // 数字读完了（后面是那个句点）
            } else if (ch == '\n') {
                break;      // 这一行压根没有数
            }
        }
        if (digits.empty()) return 0;
        try {
            return std::stoull(digits);
        } catch (...) {
            return 0;
        }
    };

    // 见头文件：只数 free 是不够的。
    const std::uint64_t usable = pages_of("Pages free:") +
                                 pages_of("Pages inactive:") +
                                 pages_of("Pages purgeable:") +
                                 pages_of("Pages speculative:");
    if (usable == 0) return std::nullopt;
    return static_cast<double>(usable) * static_cast<double>(page) /
           (1024.0 * 1024.0 * 1024.0);
}

namespace {
#if !defined(__APPLE__)

/// NVML 那个内存结构。**自己声明，不引 nvml.h**：那个头跟着 CUDA toolkit
/// 走，而我们要的是"装了驱动就能用"，不是"装了 toolkit 才编得过"。
/// 字段顺序和类型是 NVML 的 ABI，动不得。
struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

/// nvmlUtilization_t。同上，自己声明，字段顺序是 ABI。
struct NvmlUtilization {
    unsigned int gpu;      ///< 过去一段采样窗口里有内核在跑的时间占比
    unsigned int memory;   ///< 显存读写占比
};

/// 直接问驱动要显存，**不 fork**。
///
/// 原来只有一条路：fork + exec 去跑 nvidia-smi。两个毛病：
///
///   1. **这个进程初始化 CUDA 之后映射着几十 GB**，在这种进程里 fork 是
///      NVIDIA 明确不支持的做法。问不到的时候调度器就退回保守估算，
///      于是"显存够就不清理"在真机上可能等于从来没生效过——2026-09-11
///      在 96 GB 卡上看到的正是"每次出片都把大模型踢掉"。
///   2. **一次一百毫秒上下。** 它在每次借槽、每次采样第一步的路径上。
///
/// NVML 是驱动自带的库（装了 NVIDIA 驱动就有，不用装 CUDA toolkit），
/// 进程内调，微秒级，不 fork。
///
/// **运行时加载、链接期不依赖**：Mac、纯 CPU 的 Linux、没显卡的 Windows
/// 都得照样起得来。加载不上就回 nullopt，调用方退回 nvidia-smi 那条老路
/// ——这里只是**加了一条更快更稳的路，没有拆掉任何东西**。
class Nvml {
public:
    /// 总量和空闲，**一次问出来的**。加载不上、初始化失败、拿不到卡，
    /// 一律 nullopt。
    /// idx = 这个进程绑在哪张卡上（物理编号）。见 visible_device_index。
    static std::optional<VramTotals> totals(unsigned int idx) {
        // **每次都看一眼那个开关，不是只在构造时看。** 只在构造时看的话，
        // 第一次问过之后再设就没用了——而"出了岔子一键关掉"要的正是
        // 随时能关。getenv 比 dlopen 便宜得多，放在这条路上不心疼。
        if (!paths::env("CHANGJI_NO_NVML").empty()) return std::nullopt;
        const Nvml& one = instance();
        if (!one.ok_) return std::nullopt;
        void* dev = nullptr;
        if (one.handle_(idx, &dev) != 0 || dev == nullptr) return std::nullopt;
        NvmlMemory mem{};
        if (one.mem_(dev, &mem) != 0) return std::nullopt;
        // total == 0 说明这结构没被填上，别拿它当"空闲 0 字节"用——
        // 那会让调度器以为卡满了，每次都去卸模型。
        if (mem.total == 0) return std::nullopt;
        constexpr double kGb = 1024.0 * 1024 * 1024;
        return VramTotals{static_cast<double>(mem.total) / kGb,
                          static_cast<double>(mem.free) / kGb};
    }

    /// 每张卡此刻的负载。顶栏那三个小表两秒问一次，所以这条也得是
    /// 进程内、微秒级的。哪一项问不到就留空（利用率 -1、名字空串），
    /// 整个库加载不上就回空。**不按 CUDA_VISIBLE_DEVICES 换算**：
    /// 这是给人看整台机器的，不是给调度器判某一张卡的。
    static std::vector<GpuLive> live() {
        std::vector<GpuLive> out;
        if (!paths::env("CHANGJI_NO_NVML").empty()) return out;
        const Nvml& one = instance();
        if (!one.ok_ || !one.count_) return out;
        unsigned int n = 0;
        if (one.count_(&n) != 0) return out;
        if (n > 16) n = 16;   // 不像是卡数，别当真
        for (unsigned int i = 0; i < n; ++i) {
            void* dev = nullptr;
            if (one.handle_(i, &dev) != 0 || dev == nullptr) continue;
            GpuLive g;
            g.index = i;
            if (one.name_) {
                char buf[96] = {};   // NVML_DEVICE_NAME_V2_BUFFER_SIZE
                if (one.name_(dev, buf, sizeof(buf)) == 0) g.name = buf;
            }
            if (one.util_) {
                NvmlUtilization u{};
                if (one.util_(dev, &u) == 0) g.util_percent = static_cast<int>(u.gpu);
            }
            NvmlMemory mem{};
            if (one.mem_(dev, &mem) == 0 && mem.total > 0) {
                constexpr double kGb = 1024.0 * 1024 * 1024;
                g.vram_total_gb = static_cast<double>(mem.total) / kGb;
                g.vram_used_gb = static_cast<double>(mem.used) / kGb;
            }
            out.push_back(std::move(g));
        }
        return out;
    }

private:
    static const Nvml& instance() {
        static Nvml one;   // C++11 起，局部静态的初始化是线程安全的
        return one;
    }

    using InitFn = int (*)();
    using HandleFn = int (*)(unsigned int, void**);
    using MemFn = int (*)(void*, NvmlMemory*);
    using CountFn = int (*)(unsigned int*);
    using UtilFn = int (*)(void*, NvmlUtilization*);
    using NameFn = int (*)(void*, char*, unsigned int);

#if defined(_WIN32)
    using LibHandle = HMODULE;
    static LibHandle open_lib() {
        if (LibHandle h = ::LoadLibraryA("nvml.dll")) return h;
        // 老驱动把它装在这儿，不在 System32。
        return ::LoadLibraryA(
            "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
    template <class T>
    T sym(const char* n) const {
        return reinterpret_cast<T>(::GetProcAddress(lib_, n));
    }
#else
    using LibHandle = void*;
    static LibHandle open_lib() {
        // 带版本号那个才是驱动装的实文件；不带的是 -dev 包里的软链，
        // 没装开发包的机器上不存在。两个都试。
        if (LibHandle h = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL)) {
            return h;
        }
        return ::dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
    }
    template <class T>
    T sym(const char* n) const {
        return reinterpret_cast<T>(::dlsym(lib_, n));
    }
#endif

    Nvml() {
        lib_ = open_lib();
        if (!lib_) return;
        // _v2 是现在的名字；老驱动上只有不带后缀那个。
        InitFn init = sym<InitFn>("nvmlInit_v2");
        if (!init) init = sym<InitFn>("nvmlInit");
        handle_ = sym<HandleFn>("nvmlDeviceGetHandleByIndex_v2");
        if (!handle_) handle_ = sym<HandleFn>("nvmlDeviceGetHandleByIndex");
        mem_ = sym<MemFn>("nvmlDeviceGetMemoryInfo");
        // 下面三个是给顶栏的小表用的，**缺了不算加载失败**：老驱动上
        // 没有也照样能问显存。
        count_ = sym<CountFn>("nvmlDeviceGetCount_v2");
        if (!count_) count_ = sym<CountFn>("nvmlDeviceGetCount");
        util_ = sym<UtilFn>("nvmlDeviceGetUtilizationRates");
        name_ = sym<NameFn>("nvmlDeviceGetName");
        if (!init || !handle_ || !mem_) return;
        // **不配 nvmlShutdown。** 这个对象活到进程结束；中途关掉的话
        // 下次问又要重新初始化（几十毫秒），而它在借槽的关键路径上。
        ok_ = init() == 0;
    }

    LibHandle lib_ = nullptr;
    HandleFn handle_ = nullptr;
    MemFn mem_ = nullptr;
    CountFn count_ = nullptr;
    UtilFn util_ = nullptr;
    NameFn name_ = nullptr;
    bool ok_ = false;
};

#endif  // !__APPLE__
}  // namespace

#if !defined(__APPLE__)
/// 用 NVML 答出 detect_gpu 要的那三件事：名字、卡 0 的总显存、有几张卡。
///
/// `live()` 一次就把这三样都给了（它本来是给顶栏那三个小表用的），
/// 所以这里不用再写一遍 dlopen。驱动版本它给不了，留空——
/// 界面上没有谁在显示驱动版本，为它单独再加载一个符号不值当。
///
/// ⚠️ **这个定义必须在匿名 namespace 外面**，和上面那句前置声明同一个
/// 作用域。放进去过一次，macOS 上编得过（整段被 __APPLE__ 挑掉了），
/// Linux 上是链接期的
/// `undefined reference to changji::models::detect_gpu_nvml()`——
/// 声明在 changji::models 里，定义在匿名 namespace 里，是两个符号。
/// 它用到的 Nvml 类在匿名 namespace 里没关系：同一个翻译单元，看得见。
std::optional<GPUInfo> detect_gpu_nvml() {
    const auto cards = Nvml::live();
    if (cards.empty()) return std::nullopt;
    // **看的是这个进程绑的那张卡**，不是第一张。理由同 free_vram_gb：
    // 多卡机器上拿错卡的容量，下游一路算下去都是错的。
    // 认不出编号（CUDA_VISIBLE_DEVICES 写的是 UUID）就退回 0 号——
    // 这一条只影响"这台机器多大"，不像空闲那条会直接导致 OOM。
    const unsigned int idx = visible_device_index().value_or(0u);
    const auto& card = idx < cards.size() ? cards[idx] : cards.front();
    if (card.vram_total_gb <= 0.0) return std::nullopt;

    GPUInfo g;
    g.name = card.name.empty() ? "NVIDIA GPU" : card.name;
    g.vram_mb = static_cast<int>(card.vram_total_gb * 1024.0);
    g.count = static_cast<int>(cards.size());
    return g;
}
#endif

std::vector<GpuLive> gpu_live() {
#if defined(__APPLE__)
    // **顶栏上那块表在 Mac 上原来整个是空的**（这里以前就一句 `return {}`）。
    // 用户 2026-09-12：「实际显存/内存容量大小」没显示——就是这条。
    // NVML 在这台机器上当然没有，但 Metal 答得出同样的两个数。
    //
    // ⚠️ **"已用"的口径和 N 卡那边不一样，得说清楚。** NVML 报的是**整张卡**
    // 上所有进程占的；Metal 的 currentAllocatedSize 是**本进程**通过它分配
    // 的那些。统一内存上这反而是更该显示的数——别的程序占的那份在"内存"
    // 那块表里，重复算进"显存"只会让两块表加起来超过整机内存。
    //
    // 利用率留 -1（界面显示 "—"）：Metal 没有 NVML 那种现成的
    // "过去一段时间有内核在跑的时间占比"，要走 IOReport 那套私有接口。
    // 宁可显示"—"，也别拿个算出来的数冒充实测。
    const auto m = metal_memory();
    if (!m.has_value()) return {};
    constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
    GpuLive g;
    g.index = 0;
    g.name = m->name.empty() ? "Apple GPU" : m->name;
    g.util_percent = -1;
    g.vram_total_gb = static_cast<double>(m->max_working_set) / kGb;
    g.vram_used_gb = static_cast<double>(m->allocated) / kGb;
    return {g};
#else
    return Nvml::live();
#endif
}

std::optional<unsigned int> parse_visible_devices(const std::string& raw) {
    // 没设 = 全部可见，进程里的 0 号就是物理 0 号。
    std::string first;
    for (char c : raw) {
        if (c == ',') break;
        if (!std::isspace(static_cast<unsigned char>(c))) first += c;
    }
    if (first.empty()) return 0u;
    // **UUID 那种形式认不出来。** "GPU-xxxx" 是合法写法，但没法换算成
    // NVML 的下标。不猜——猜错了是问到别的卡上去，而那个方向会 OOM。
    for (char c : first) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        const unsigned long v = std::stoul(first);
        if (v > 64) return std::nullopt;   // 不像是卡号，别当真
        return static_cast<unsigned int>(v);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<unsigned int> visible_device_index() {
    return parse_visible_devices(paths::env("CUDA_VISIBLE_DEVICES"));
}

/// nvidia-smi 一张卡一行，取第 idx 行（从 0 数）。取不到返回空串。
/// 理由同 visible_device_index：绑了卡就不能只看第一行。
std::string nth_gpu_line(const std::string& out, unsigned int idx) {
    std::size_t i = 0;
    unsigned int seen = 0;
    while (i <= out.size()) {
        const std::size_t nl = out.find('\n', i);
        const std::size_t end = nl == std::string::npos ? out.size() : nl;
        const std::string line = out.substr(i, end - i);
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            if (seen == idx) return line;
            ++seen;
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    return {};
}

std::optional<double> free_vram_gb() {
#if defined(__APPLE__)
    // **先问 Metal：`能占的上限 - 已经占了的`。**
    //
    // 这条比底下那条 vm_stat 强在两处：一是不 fork（这个函数在每次借槽
    // 的路径上），二是**口径和总量同源**——两个数来自同一个 MTLDevice，
    // 不会出现"空闲比总量还大"。
    //
    // 那种事真发生过：总量走 sysctl 的 75% 兜底算出 96 GB，空闲走 vm_stat
    // 算出 106 GB，于是 sd_image.cpp 里 `used = total - free` 是 -10，
    // 卡在 `if (used_gb > 0.0)` 上——**Mac 上显存实测标定一次都没记下过**，
    // 而且没有任何日志说它被跳过了。
    if (const auto m = metal_memory(); m.has_value()) {
        // **和 GPUInfo::vram_mb 一样按 MB 取整，而且往小了取。**
        // 那边是 `字节 / 1MiB` 直接截断，这边要是按原始字节算，空闲会比
        // 报出去的总量大那么一丁点（实测 20 KB，全是取整造成的），
        // 于是 `free <= total` 这条不变量在小数点后第五位上破掉——
        // 而下游拿它们相减（见 sd_image.cpp 的 record_measured_vram）。
        // 总量向下取、已占用向上取，差值永远落在总量里面。
        constexpr double kMb = 1024.0 * 1024.0;
        const double total_mb = std::floor(static_cast<double>(m->max_working_set) / kMb);
        const double used_mb = std::ceil(static_cast<double>(m->allocated) / kMb);
        return total_mb > used_mb ? (total_mb - used_mb) / 1024.0 : 0.0;
    }

    // 退路：vm_stat。**它的口径不是"显存"**——free + inactive + speculative
    // 是"系统还能腾出多少内存给 CPU 用"，比 Metal 肯 wire 给 GPU 的那条线
    // 大得多。所以**必须夹住**，宁可报小：报小只是多卸一次模型，
    // 报大是拿不存在的空间去判"够，不卸"，下一步就是换页。
    if (auto r = proc::run(kVmStat, {}, 5000);
        r.launched && r.exit_code == 0) {
        if (auto gb = parse_vm_stat(r.out); gb.has_value()) {
            const auto cap = apple_working_set_gb();
            return cap.has_value() ? std::min(*gb, *cap) : *gb;
        }
    }
    return std::nullopt;
// **这里必须是 #else，不能是 #endif。** Apple 那一支已经 return 了，
// 看着后面的代码"跑不到"，但它照样要**编译**——而 Nvml 那个类整个
// 在 !__APPLE__ 里，Mac 上根本不存在。2026-09-11 就是这么挂的：
// Windows 和 Linux 全绿，macOS 单元测试编不过（undeclared identifier）。
#else
    // **先算清楚这个进程绑的是哪张卡。** 认不出来就当问不到——猜错卡
    // 是拿别人卡上的空闲去判"够，不卸"，下一步 OOM。见 visible_device_index。
    const auto idx = visible_device_index();
    if (!idx.has_value()) return std::nullopt;
    // **先问 NVML**（进程内、不 fork、微秒级），问不到再走老路。
    // 理由写在 Nvml 上面。
    if (auto t = Nvml::totals(*idx); t.has_value()) return t->free_gb;
    if (!proc::which("nvidia-smi")) return std::nullopt;
    // **超时要短。** 这个函数在每次借槽的路径上，卡住比问不到更糟；
    // 问不到只是退回静态估算。
    auto r = proc::run(
        "nvidia-smi",
        {"--query-gpu=memory.free", "--format=csv,noheader,nounits"}, 5000);
    if (!r.launched || r.exit_code != 0) return std::nullopt;
    // 一张卡一行，取自己那一行，不是第一行。
    const std::string line = nth_gpu_line(r.out, *idx);
    if (line.empty()) return std::nullopt;
    return parse_free_vram(line);
#endif
}

std::optional<VramTotals> vram_totals_gb() {
#if defined(__APPLE__)
    // 统一内存这边没有"整卡多大"这个说法对得上 NVML 的语义，
    // 就不硬凑一个。调用方拿不到就走原来那条两次问的老路。
    return std::nullopt;
#else
    const auto idx = visible_device_index();
    if (!idx.has_value()) return std::nullopt;   // 理由同 free_vram_gb
    if (auto t = Nvml::totals(*idx); t.has_value()) return t;
    // 退路：一次 nvidia-smi 同时要两个数。**一次，不是两次**——
    // 分两次问的话两个数来自两个时刻，差值就不是这个槽占的。
    if (!proc::which("nvidia-smi")) return std::nullopt;
    auto r = proc::run("nvidia-smi",
                       {"--query-gpu=memory.total,memory.free",
                        "--format=csv,noheader,nounits"},
                       5000);
    if (!r.launched || r.exit_code != 0) return std::nullopt;
    const std::string first = nth_gpu_line(r.out, *idx);
    const std::size_t comma = first.find(',');
    if (comma == std::string::npos) return std::nullopt;
    const auto total = parse_free_vram(first.substr(0, comma));
    const auto free = parse_free_vram(first.substr(comma + 1));
    if (!total.has_value() || !free.has_value() || *total <= 0.0) {
        return std::nullopt;
    }
    return VramTotals{*total, *free};
#endif
}

std::optional<GPUInfo> parse_gpu_query(const std::string& out) {
    if (out.empty()) return std::nullopt;
    // **卡数 = 非空行数。** nvidia-smi 一张卡一行。
    // 只读第一行取显存是对的（那是卡 0 的），但"有几张"以前根本没人问。
    int gpu_count = 0;
    for (std::size_t i = 0; i < out.size();) {
        const std::size_t nl = out.find('\n', i);
        const std::size_t end = nl == std::string::npos ? out.size() : nl;
        const std::string line = out.substr(i, end - i);
        if (line.find_first_not_of(" \t\r") != std::string::npos) ++gpu_count;
        if (nl == std::string::npos) break;
        i = nl + 1;
    }

    const std::string first = out.substr(0, out.find('\n'));
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= first.size()) {
        const std::size_t comma = first.find(',', start);
        const std::size_t end = comma == std::string::npos ? first.size() : comma;
        std::string p = first.substr(start, end - start);
        // strip
        const auto b = p.find_first_not_of(" \t\r\n");
        const auto e = p.find_last_not_of(" \t\r\n");
        parts.push_back(b == std::string::npos ? "" : p.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() < 2) return std::nullopt;

    GPUInfo info;
    info.name = parts[0];
    try {
        info.vram_mb = static_cast<int>(std::stod(parts[1]));
    } catch (...) {
        return std::nullopt;
    }
    if (parts.size() > 2 && !parts[2].empty()) info.driver = parts[2];
    info.count = gpu_count > 0 ? gpu_count : 1;
    return info;
}

std::map<Tier, TierSpec> tiers_for_vram(double vram_gb) {
    int idx = kTierRows - 1;
    for (int i = 0; i < kTierRows; ++i) {
        if (vram_gb >= kTierTable[i].threshold) { idx = i; break; }
    }
    const TierRow& row = kTierTable[idx];

    std::map<Tier, TierSpec> specs;
    for (Tier t : all_tiers()) {
        const int* v = row_for(row, t);
        // 统一规整到 32 的倍数。表里手写的数字可能不合规，
        // 而不是 32 的倍数会导致 Wan 的潜空间对不齐。
        const int w = round32(v[0]);
        const int h = round32(v[1]);
        TierSpec spec;
        spec.tier = t;
        spec.width = w;
        spec.height = h;
        spec.steps = v[2];
        spec.measured_seconds = estimate_seconds(t, w, h, v[2], row.threshold);
        specs[t] = spec;
    }
    return specs;
}

HardwareProfile HardwareProfile::detect(std::optional<double> override_vram_gb) {
    HardwareProfile p;
    p.gpu = detect_gpu();
    if (override_vram_gb.has_value()) {
        p.vram_gb = *override_vram_gb;
        p.detected = false;
    } else if (p.gpu.has_value()) {
        p.vram_gb = p.gpu->vram_gb();
        p.detected = true;
    } else {
        // 探测不到就按 12GB 这个偏保守的假设走，并明确标记未探测到
        p.vram_gb = 12.0;
        p.detected = false;
    }
    p.tiers = tiers_for_vram(p.vram_gb);
    // 表里那个数单独留一份，见 HardwareProfile::table_final_steps。
    // 外面会把 tiers[FINAL].steps 换成实跑的值，换完就找不回来了。
    const auto fin = p.tiers.find(Tier::FINAL);
    if (fin != p.tiers.end()) p.table_final_steps = fin->second.steps;
    return p;
}

std::string HardwareProfile::describe() const {
    std::string head;
    if (gpu.has_value()) {
        head = SAYF("%1，显存 %2 GB", gpu->name, fmt1(gpu->vram_gb()));
        // 多卡的时候说一声。**说清楚显存是单卡的**——
        // 不然看到"8 张卡"很容易以为那 48 GB 是总数。
        if (gpu->count > 1) {
            head += SAYF("（共 %1 张，显存是单卡的）",
                         std::to_string(gpu->count));
        }
    } else if (detected) {
        head = SAYF("显存 %1 GB", fmt1(vram_gb));
    } else {
        head = SAYF("未探测到显卡，按 %1 GB 估算", fmt1(vram_gb));
    }

    std::ostringstream os;
    os << head;
    for (Tier t : all_tiers()) {
        const auto it = tiers.find(t);
        if (it == tiers.end()) continue;
        const TierSpec& spec = it->second;
        const std::string secs =
            spec.measured_seconds.has_value()
                ? SAYF("约 %1 秒", fmt0(*spec.measured_seconds))
                : SAY("未标定");
        os << "\n  " << pad_right(to_string(t), 8) << " " << spec.width << "x"
           << spec.height << "  "
           << SAYF("%1 步  单镜 %2", std::to_string(spec.steps), secs);
    }
    return os.str();
}

std::optional<double> HardwareProfile::estimate_episode(int shot_count,
                                                        Tier tier) const {
    const auto it = tiers.find(tier);
    if (it == tiers.end() || !it->second.measured_seconds.has_value()) {
        return std::nullopt;
    }
    return *it->second.measured_seconds * shot_count;
}

}  // namespace changji::models
