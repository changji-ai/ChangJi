// 问 Metal 要「这台机器的 GPU 能用多少内存」。**只在 macOS 上编进来。**
//
// 为什么要单开一个 .mm：这两个数只有 Metal 知道，而 Metal 是 Objective-C 的
// 接口，C++ 里拿不到。整个文件就一个函数，剩下的都写在 hardware.cpp 里。
//
// **为什么不继续用 sysctl 算。** 原来的办法是 `hw.memsize × iogpu.wired_limit_pct`，
// 而那个 OID 在 macOS 26 上**已经不存在了**（现在叫 `iogpu.wired_limit_mb`）——
// sysctl 报 unknown oid，代码静默退回写死的 75%。这台 128 GB 的 M3 Max 上：
//
//     hw.memsize                    137438953472 B = 128.0 GiB
//     75% 兜底（原来显示的）          96.0 GiB
//     recommendedMaxWorkingSetSize  115448725504 B = 107.5 GiB（= 84%）
//
// 少认了 11.5 GB，而且**没有任何报错**——和当年"探不到 nvidia-smi 就按
// 12 GB 估算"是同一个形状的故障：数字看着正常，只是什么都跑不大。
//
// **为什么不问 ggml。** `ggml_backend_dev_memory()` 给的就是这两个数
// （ggml-metal-device.m 里读的正是这两个字段），但 ggml 的 Metal 设备
// 一初始化就要把那二十个 shader 库编出来——本机实测 **16 秒**。
// 我们只想读两个数，不值当。`MTLCreateSystemDefaultDevice()` 不编 shader。
//
// **currentAllocatedSize 算得上 ggml 分配的那些。** 实测过：
// `MTLCreateSystemDefaultDevice()` 每次返回的是**同一个对象**，拿其中一个
// 句柄分配 2 GB，另一个句柄读到的 currentAllocatedSize 同样涨 2048 MB。
// 所以这里读到的「已占用」包含推理那边占的，不是只有我们自己的。

#import <Metal/Metal.h>

#include "models/hardware.hpp"

namespace changji::models {

std::optional<MetalMemory> metal_memory() {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) return std::nullopt;

        MetalMemory m;
        if (NSString* n = [dev name]) m.name = [n UTF8String];
        m.unified = [dev hasUnifiedMemory];
        m.max_working_set = [dev recommendedMaxWorkingSetSize];
        m.allocated = [dev currentAllocatedSize];
        // 读到 0 就当没读到：拿它当"显存 0 字节"用的话，档位推导和调度器
        // 会一起认为这台机器什么都跑不了。
        if (m.max_working_set == 0) return std::nullopt;
        return m;
    }
}

}  // namespace changji::models
