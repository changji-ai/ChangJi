#pragma once

// 硬件探测与画质档位推导。
//
// 这个文件存在的理由是可移植性。画质档位绝不能写死成某一台机器的数字，
// 必须由运行时探测到的实际显存推导出来。
//
// 档位的基准点来自 RTX 5080 16GB 上对 Wan 2.2 TI2V 5B 的实测：
// 草稿 640x352 10 步 27 秒，预览 960x544 20 步 120 秒，成片 1280x704 30 步 392 秒。
// 其他显存档位按分辨率和步数缩放，首次运行时会用实测校准。
//
// 移植自 src/changji/hardware.py。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/json_compat.hpp"

namespace changji::models {

/// 画质档位。分级生成靠它。
///
/// **只有两档。** 中间那档 `preview` 2026-09-13 删了：整棵树除了这个
/// 文件自己没有任何地方用过它，能选的只有 draft 和 final
/// （`config/settings.cpp` 里 `frame_tier`、`video_lora_tiers` 的校验
/// 都只认这两个）。留着的唯一效果是 `/api/hardware` 多回一档看不懂的数
/// ——实测 5090 上它报 1280×704 / 20 步 / 207 秒，比成片档（544×928 /
/// 6 步 / 96.6 秒）又大又慢，因为成片档会被项目画幅和 Turbo 盖掉，
/// 而它不会。一个没人用、还自相矛盾的数字，删掉比解释便宜。
enum class Tier { DRAFT, FINAL };

/// **DRAFT 必须排在第一个。** nlohmann 这个宏的 from_json 对认不出的
/// 取值回退到列表首项（json.hpp 里 `(it != end) ? it : begin(m)`）。
/// 老版本的工作进程可能还会在任务里发 `"tier": "preview"`，回退成
/// DRAFT 正好也是 `worker_proto` 那个字段的默认值——不报错，也不会
/// 悄悄当成成片档去跑。
NLOHMANN_JSON_SERIALIZE_ENUM(Tier, {
    {Tier::DRAFT, "draft"},
    {Tier::FINAL, "final"},
})

const char* to_string(Tier v);

/// 两个档位的固定顺序。遍历时用它，别依赖 map 的顺序。
const std::vector<Tier>& all_tiers();

/// 规整到 32 的倍数。分辨率不是 32 的倍数会导致 Wan 的潜空间对不齐。
///
/// ⚠️ 必须用**银行家舍入**（四舍六入五取偶），因为 Python 的 round() 就是那样。
/// C++ 的 std::round 是"四舍五入远离零"，两者在恰好 .5 时不同：
/// round(12.5) Python 给 12、std::round 给 13。当前档位表里只有 432/32=13.5
/// 命中半整数（两边碰巧都给 14），但 scaled_to 的 1:1 分支算的是 (w+h)/2，
/// 那是任意值，迟早会撞上。
int round32(int n);

/// 一个档位的生成参数。
struct TierSpec {
    Tier tier = Tier::DRAFT;
    int width = 0;
    int height = 0;
    int steps = 0;
    /// 本机实测的单镜耗时，未标定时为空
    std::optional<double> measured_seconds;
    /// 步数是人在 [tiers].final_steps 里钉死的，不是表推的。
    ///
    /// 跟着 spec 走是因为它要**跨机**：派活那台按自己盘上有没有 Turbo
    /// LoRA 定步数，而 LoRA 在干活那台——2026-09-15 实测，Mac 上没有
    /// LoRA 就按 20 步派给 L20，那边挂着 Turbo 跑 20 步，一段 462 秒还
    /// 过锐。干活那台要自己重定（config::steps_on_node），而"人钉死的
    /// 别动"这条它得知道。
    bool steps_pinned = false;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        TierSpec, tier, width, height, steps, measured_seconds, steps_pinned)

    /// 按画幅调整。分辨率必须是 32 的倍数，否则 Wan 的潜空间对不齐。
    TierSpec scaled_to(const std::string& aspect_ratio) const;
};

/// Metal 报的内存。**只有 macOS 上有这个东西。**
///
/// 两个字段都是字节。`max_working_set` 是 `recommendedMaxWorkingSetSize`：
/// Metal 肯让 GPU 一次性占住多少。**它不是整机内存**——128 GB 的 M3 Max 上
/// 它是 107.5 GB（84%），剩下的系统留着自己用。超过这条线不会当场失败，
/// 但系统开始压缩换页，那比少用几 GB 惨得多。
///
/// `allocated` 是 `currentAllocatedSize`：此刻已经占了多少。**包含 ggml
/// 那边占的**（同一个 MTLDevice 对象，实测验过），所以
/// `max_working_set - allocated` 就是"还能拿多少"。
struct MetalMemory {
    std::string name;
    std::uint64_t max_working_set = 0;
    std::uint64_t allocated = 0;
    bool unified = false;
};

#if defined(__APPLE__)
/// 实现在 hardware_metal.mm。问不到（没有 Metal 设备）返回空。
std::optional<MetalMemory> metal_memory();
#endif

struct GPUInfo {
    std::string name;
    int vram_mb = 0;
    std::optional<std::string> driver;
    /// 这台机器上有几张卡。**探测不到时是 1，不是 0**——
    /// 有一张卡在跑这个程序，0 会让"起几个工作进程"算出 0 个。
    ///
    /// **和 vram_mb 是两个维度，别混。** vram_mb 是**卡 0** 的显存，
    /// 决定单个镜头能跑多大（一个模型跑在一张卡上）；
    /// count 决定能同时跑几个镜头。八张 48 GB 加起来去查档位表，
    /// 会算出一张卡根本跑不动的分辨率。
    int count = 1;

    /// 统一内存机器上，**整机物理内存**（MB）。0 = 不是统一内存，或者问不到。
    ///
    /// **和 vram_mb 不是一回事，界面上两个都要显示。** 128 GB 的 Mac 上
    /// vram_mb 是 Metal 肯给的那 107.5 GB，这一项才是 128 GB。只显示前者，
    /// 用户看到的是"我明明买的 128"；只显示后者，预算又会按 128 算，
    /// 而超过 107.5 系统就开始换页。
    int unified_mb = 0;

    /// 这块「显存」是不是和系统内存同一块。
    ///
    /// **影响的不只是显示。** 独显上"权重放内存"是拿速度换显存（每步走
    /// 一次 PCIe）；统一内存上根本没有那次搬运，也没有另一个内存池——
    /// 放内存既不省地方也不会更快，只是把计算赶去了 CPU。
    /// 见 ModelsConfig::weights_for。
    bool unified() const { return unified_mb > 0; }

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GPUInfo, name, vram_mb, driver,
                                                count, unified_mb)

    double vram_gb() const { return static_cast<double>(vram_mb) / 1024.0; }
};

/// 解析 `nvidia-smi --query-gpu=name,memory.total,driver_version
/// --format=csv,noheader,nounits` 的输出。
///
/// **抽出来是为了能测。** 开发机只有一张卡，多卡那条路一行都跑不到；
/// 而这段要是错了，表现是"八个进程全挤在卡 0 上，看着在并行实际在排队"，
/// 一声不吭。
///
/// 显存取**第一行**（卡 0 的），卡数是非空行数——两个维度，别混。
std::optional<GPUInfo> parse_gpu_query(const std::string& out);

/// 探测本机显卡。探测不到返回空，由调用方决定怎么办。
///
/// 只用 nvidia-smi，不引入 torch 依赖。这个包是编排引擎，
/// 真正的推理可能根本不在同一台机器上。
std::optional<GPUInfo> detect_gpu();

/// 现在这张卡还空着多少显存（GB）。**每次调用都真去问一遍。**
///
/// 和 `detect_gpu()` 报的总量不是一回事：总量是静态的，这个是实时的，
/// 会随着别的进程、别的槽的占用变化。调度器拿它决定"要不要卸掉别的模型"
/// ——静态估算说装不下、而实际空着一大块的时候，卸载是纯浪费：
/// 一次重新加载是几十秒到几分钟。
///
/// 问不到（没有 nvidia-smi、多卡、解析失败）就回 nullopt，
/// 调用方退回原来的静态估算。**不要把"问不到"当成"没有空间"**。
std::optional<double> free_vram_gb();

/// 这张卡的总量和空闲量，**同一时刻、同一次问出来的**。
///
/// 为什么要有这个而不是分两次问：算"这个槽实际占了多少"用的是
/// 总量 − 空闲。分两次问的话，两个数来自两个时刻、两条不同的路
/// （总量走 HardwareProfile::detect，空闲走 free_vram_gb），中间模型
/// 可能已经装进去了——差值就不是这个槽占的，而是两次采样之间的变化。
/// 而那个差值会被当成实测值记下来，之后每一镜都拿它判要不要卸模型。
///
/// 顺带把采样那条路上的 fork 去掉：detect 会跑一遍完整硬件探测
/// （fork nvidia-smi、查 PATH、读 CPU 信息），而它是在 sd.cpp 的采样
/// 回调里调的——那正是这个进程 CUDA 映射最满、最不该 fork 的时候。
/// 这个进程实际在用第几张卡（**NVML / nvidia-smi 的物理编号**）。
///
/// **NVML 不认 `CUDA_VISIBLE_DEVICES`。** 工作进程正是靠它绑卡的
/// （见 worker_server.cpp），于是进程里 CUDA 的 0 号可能是物理的 3 号，
/// 而 NVML 的 0 号永远是物理 0 号。不换算的话，绑在 3 号上的工作进程
/// 问到的是 0 号的空闲——两张卡的忙闲毫无关系。
///
/// 问小了只是多卸一次模型（慢）；**问大了是拿别人卡上的空闲去判
/// "够，不卸"，下一步就是 OOM**，走 GGML_ASSERT 把整个进程带走。
///
/// 规则：没设这个变量就是 0 号；设了就取第一项（进程里的 0 号对应的
/// 就是列表里第一个）。认不出来（比如写的是 UUID）返回 nullopt——
/// 调用方**别猜**，当成问不到、退回保守那条。
std::optional<unsigned int> visible_device_index();

/// 从 `CUDA_VISIBLE_DEVICES` 的原文算上面那个编号。拆出来是为了能测——
/// 真去改进程的环境变量再测，会互相踩。
///
/// **还没做的那一半：`detect_gpu()` 报的名字和整卡容量仍然取第一行**
/// （也就是物理 0 号）。卡型号一样的机器上这没问题——容量都一样；
/// 混插不同型号的卡才会错，而那时 set_total_vram 拿到的整卡容量偏大，
/// "问不到卡时按总量推算空闲"这条会偏乐观。上混卡机器之前要先处理。
/// 实时空闲那条（决定要不要卸模型的那个数）已经按绑定的卡取了。
std::optional<unsigned int> parse_visible_devices(const std::string& raw);

/// `nvidia-smi --query-gpu=...` 一张卡一行，取第 idx 行（从 0 数，空行不算）。
/// 取不到返回空串，调用方当成"问不到"。
///
/// **绑了卡就不能只看第一行**，理由同 visible_device_index。
std::string nth_gpu_line(const std::string& out, unsigned int idx);

struct VramTotals {
    double total_gb = 0.0;
    double free_gb = 0.0;
};
std::optional<VramTotals> vram_totals_gb();

/// 每张卡此刻的负载：利用率和显存用量。顶栏那三个小表用。
///
/// **NVML 进程内问，不 fork。** 两秒一问的东西不能走 nvidia-smi 那条：
/// 一次一百毫秒，而且这个进程映射着几十 GB 的 CUDA 内存，fork 是 NVIDIA
/// 明确不支持的。问不到（没驱动、Mac、CHANGJI_NO_NVML）回空，调用方
/// 自己决定要不要退回 nvidia-smi（见 util/sysstat.cpp，那边限了频率）。
struct GpuLive {
    unsigned int index = 0;
    std::string name;
    int util_percent = -1;      ///< -1 = 这个驱动问不到
    double vram_used_gb = 0.0;
    double vram_total_gb = 0.0;
};
std::vector<GpuLive> gpu_live();

/// 从 `nvidia-smi --query-gpu=memory.free` 的输出里解析空闲显存。
/// 单独拆出来是为了能测——测试里不该真去跑 nvidia-smi。
std::optional<double> parse_free_vram(const std::string& out);

/// 从 `vm_stat` 的输出里算出「还能用多少内存」（GB）。
///
/// **苹果机器上这就是"还剩多少显存"**：统一内存，CPU 和 GPU 共用一块。
///
/// 不能只看 "Pages free"——这台 16 GB 的 iMac 上它只有 4124 页（67 MB），
/// 拿它当依据的话调度器会以为一点空间都没有，每次都卸模型。macOS 真正
/// 能拿来用的是 free + inactive + purgeable + speculative：inactive 是
/// 有主但随时可以回收的，purgeable 是明说可以丢的。
///
/// 页大小从输出头一行 "(page size of N bytes)" 里读——**别写死 4096**，
/// 苹果芯片是 16384。写死的话算出来差四倍。
std::optional<double> parse_vm_stat(const std::string& out);

/// 按显存推导三个档位的参数。
std::map<Tier, TierSpec> tiers_for_vram(double vram_gb);

/// 本机硬件画像。配置里可以覆盖，方便推理服务在别的机器上时手动指定。
struct HardwareProfile {
    std::optional<GPUInfo> gpu;
    double vram_gb = 12.0;
    std::map<Tier, TierSpec> tiers;
    bool detected = false;

    /// **成片档在"被 Turbo 压扁之前"的步数。**
    ///
    /// `tiers[FINAL].steps` 到了外面已经不是表里那个数了：
    /// `Runtime::profile()` 会先用 `effective_spec` 把它换成实跑的值
    /// （挂着 Turbo 就是 6），好让 /api/hardware 显示的规格和磁盘上的
    /// 成片对得上。那是对的。
    ///
    /// 问题是**还有人要那个原始值**：首帧的步数。出图那一步没有 Turbo
    /// LoRA，跟着跑 6 步就是裸跑 6 步（见 effective_spec 里那段）。
    /// 而 `apply_project_spec` 拿 `tiers[FINAL].steps` 当"表里的值"再
    /// 算一次——**同一个变换套了两次**，首帧步数从 20 塌成 6。
    /// 2026-09-13 实机撞到：进度条写着"出首帧（第 1/6 步）"。
    ///
    /// 所以把原始值单独留一份。0 表示"没记"，那时候退回读 tiers。
    int table_final_steps = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        HardwareProfile, gpu, vram_gb, tiers, detected, table_final_steps)

    static HardwareProfile detect(std::optional<double> override_vram_gb = std::nullopt);

    std::string describe() const;

    /// 估算一章的纯生成时间，单位秒。未标定返回空。
    std::optional<double> estimate_episode(int shot_count, Tier tier) const;
};

}  // namespace changji::models
