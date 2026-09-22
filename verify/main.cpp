// 前置验证工程的验证程序。
//
// 回答两个问题：
//   一、MSVC + CUDA 这条工具链能不能把 sd.cpp 和 llama.cpp 编出来
//   二、两个库共用一份 ggml 之后，符号能不能解析、ABI 对不对得上
//
// 第二问里 ABI 那半边不是靠"链接过了"来判断的——GGML_MAX_NAME 不一致
// 的时候链接一样能过，错在运行时。所以这里有一个真的能测出来的探针，
// 见 check_ggml_abi()。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ggml.h"
#include "ggml-backend.h"
#include "llama.h"
#include "include/stable-diffusion.h"

namespace {

int failures = 0;

void ok(const char* what, const std::string& detail) {
    std::printf("  [ok]   %-28s %s\n", what, detail.c_str());
}

void bad(const char* what, const std::string& detail) {
    std::printf("  [FAIL] %-28s %s\n", what, detail.c_str());
    ++failures;
}

// GGML_MAX_NAME 的 ABI 探针。
//
// sd.cpp 要求 GGML_MAX_NAME=160，上游 ggml 默认 64。两边不一致时
// ggml_tensor 的结构体大小不同，链接不报错，运行时踩内存。
//
// 探法：设一个比 64 长的名字再读回来。ggml 库自己是按多少编的，
// 名字就在多少字节处被截断。这测的是**库**的编译期常量，
// 不是本文件的——本文件的值由 CMake 顶层的 add_compile_definitions 决定。
void check_ggml_abi() {
    std::printf("\n[2] ggml ABI 一致性\n");
    std::printf("  本程序编译时的 GGML_MAX_NAME = %d\n", GGML_MAX_NAME);

    ggml_init_params p{};
    p.mem_size   = 1024 * 1024;
    p.mem_buffer = nullptr;
    p.no_alloc   = true;

    ggml_context* ctx = ggml_init(p);
    if (!ctx) {
        bad("ggml_init", "拿不到 context");
        return;
    }

    ggml_tensor* t = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 4);
    if (!t) {
        bad("ggml_new_tensor_1d", "建不出张量");
        ggml_free(ctx);
        return;
    }

    // 100 个 'x'，比上游默认的 64 长，比 sd.cpp 要的 160 短
    const std::string probe(100, 'x');
    ggml_set_name(t, probe.c_str());
    const size_t got = std::strlen(ggml_get_name(t));

    std::printf("  写入 100 字符的名字，读回 %zu 字符\n", got);

    if (got == probe.size()) {
        ok("GGML_MAX_NAME", "库和本程序一致，两边都是 160");
    } else if (got == 63) {
        bad("GGML_MAX_NAME", "库是按 64 编的，本程序按 160——"
                             "ggml_tensor 大小不一致，会踩内存");
    } else {
        bad("GGML_MAX_NAME", "截断在 " + std::to_string(got) + "，两边都对不上");
    }

    ggml_free(ctx);
}

// 设备枚举。CUDA 后端注册上了没有，以及两个库看到的是不是同一份 ggml。
void check_backends() {
    std::printf("\n[3] ggml 后端设备\n");
    const size_t n = ggml_backend_dev_count();
    std::printf("  设备数：%zu\n", n);
    if (n == 0) {
        bad("ggml_backend_dev_count", "一个后端都没注册");
        return;
    }

    bool has_gpu = false;
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t d = ggml_backend_dev_get(i);
        // 必须写 enum：ggml 里 ggml_backend_dev_type 既是枚举名又是函数名，
        // C++ 的名字查找里函数名会隐藏类型名，不加 enum 会被解析成声明。
        const enum ggml_backend_dev_type ty = ggml_backend_dev_type(d);
        const char* tyname = ty == GGML_BACKEND_DEVICE_TYPE_GPU   ? "GPU"
                           : ty == GGML_BACKEND_DEVICE_TYPE_CPU   ? "CPU"
                                                                  : "ACCEL";
        std::printf("    %zu. [%s] %s — %s\n", i, tyname,
                    ggml_backend_dev_name(d), ggml_backend_dev_description(d));
        if (ty == GGML_BACKEND_DEVICE_TYPE_GPU) has_gpu = true;
    }

    if (has_gpu) {
        ok("GPU 后端", "注册成功，能看到 GPU 设备");
    } else if (CJV_CUDA_REQUESTED) {
        bad("CUDA 后端", "开了 CJV_CUDA 却只有 CPU 设备——CUDA 没编进去");
    } else {
        ok("后端枚举", "只有 CPU，符合 CJV_CUDA=OFF 的预期");
    }
}

// 真正创建并销毁一次 GPU 后端。
//
// 这一步专门为了跑到 ggml_backend_cuda_context 的析构函数。移植补丁时
// 那里改动最险：llama.cpp 的析构是 [设备][流] 双层循环，而 leejet 的
// cublaslt_handles 是一维，照抄会把同一个句柄销毁 GGML_CUDA_MAX_STREAMS 次
// （double free）。我把它挪到了外层循环——对不对只有真的构造再析构一次才知道。
//
// 光枚举设备不会创建 context，所以前面那步证明不了这件事。
void check_backend_lifecycle() {
    std::printf("\n[6] GPU 后端构造与析构\n");
    ggml_backend_dev_t gpu = nullptr;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t d = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) { gpu = d; break; }
    }
    if (!gpu) {
        std::printf("  没有 GPU 设备，跳过（CPU 构建下正常）\n");
        return;
    }

    ggml_backend_t backend = ggml_backend_dev_init(gpu, nullptr);
    if (!backend) {
        bad("ggml_backend_dev_init", "GPU 后端初始化失败");
        return;
    }
    std::printf("  已创建：%s\n", ggml_backend_name(backend));

    // 销毁。cublasLtDestroy 在这里被调用；放错循环层的话这一步会崩或报 CUBLAS 错。
    ggml_backend_free(backend);
    ok("后端析构", "构造再销毁一次没崩——cublasLtDestroy 的循环层次是对的");
}

void check_llama() {
    std::printf("\n[4] llama.cpp\n");
    llama_backend_init();
    const char* info = llama_print_system_info();
    ok("llama_backend_init", "没崩");
    std::printf("  系统信息：%s\n", info ? info : "(空)");
    llama_backend_free();
}

void check_sd() {
    std::printf("\n[5] stable-diffusion.cpp\n");
    const char* ver = sd_version();
    ok("sd_version", ver ? ver : "(空)");
    std::printf("  物理核心数：%d\n", sd_get_num_physical_cores());
    const char* info = sd_get_system_info();
    std::printf("  系统信息：%s\n", info ? info : "(空)");
}

}  // namespace

int main() {
    std::printf("changji 前置验证工程\n");
    std::printf("====================\n");

    std::printf("\n[1] 编译器\n");
#if defined(_MSC_VER)
    std::printf("  MSVC _MSC_VER = %d\n", _MSC_VER);
    ok("工具链", "MSVC");
#elif defined(__GNUC__)
    std::printf("  GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    bad("工具链", "是 GCC 不是 MSVC——CUDA 那半边验的不是目标环境");
#else
    bad("工具链", "认不出来");
#endif

    check_ggml_abi();
    check_backends();
    check_llama();
    check_sd();
    check_backend_lifecycle();

    std::printf("\n====================\n");
    if (failures == 0) {
        std::printf("全部通过\n");
        return 0;
    }
    std::printf("%d 项失败\n", failures);
    return 1;
}
