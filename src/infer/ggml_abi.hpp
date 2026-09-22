#pragma once

// ggml 的 ABI 自检。
//
// **这一项防的是"编得过、链得过、跑起来踩内存"。**
//
// sd.cpp 要求 `GGML_MAX_NAME=160`，上游 ggml 默认 64。这个宏决定
// `ggml_tensor` 里名字数组的长度，也就决定了整个结构体的大小。
// 两边按不同的值编出来，**链接器一声不吭**——符号名一样，只是同一个
// 结构体在库里和在我们的代码里大小不同。之后每一次读写张量字段
// 都在踩别人的内存，而现场离原因很远。
//
// sd.cpp 自己的 CMakeLists 里有 `add_definitions(-DGGML_MAX_NAME=160)`，
// 但 `add_definitions` 只作用于它之后的目录作用域——加不到别处已经编好的
// ggml 上。开了 `CHANGJI_LLAMA` 之后 ggml 由 llama.cpp 提供，
// 那个 add_definitions 就够不着它了，所以顶层要再定义一次。
// **删掉顶层那一行，编译和链接全都照过**，只有这个探针会发现。
//
// 前置验证工程（verify/main.cpp）里的同名检查是这一条的来源，
// 那份报告称它是"这次验证最有价值的一条"。搬进 `/api/doctor` 是因为
// 验证工程是一次性的，而这个风险跟着二进制走：谁改了构建脚本、
// 谁换了 ggml 的来源，都可能悄悄把它打破。
//
// 门面的写法同 sd_backend.hpp：**头文件不带 #ifdef、不 include ggml**。

#include <string>

namespace changji::infer {

/// ggml 有没有链进来（sd.cpp 或 llama.cpp 任一带进来的）。
bool ggml_available();

struct GgmlAbi {
    bool ok = false;
    /// 本程序编译时的 GGML_MAX_NAME。没链 ggml 时是 0。
    int compiled_max_name = 0;
    /// 写进去 100 个字符，读回来几个。
    int probe_read_back = 0;
    /// 给人看的一句话。
    std::string detail;
};

/// 真的建一个张量、设一个 100 字符的名字再读回来。
///
/// **必须是运行时探针，不能只比宏。** 比宏只能证明"我们这边编的时候是
/// 多少"，证明不了库是按多少编的——而不一致恰恰全在库那一侧。
GgmlAbi check_ggml_abi();

}  // namespace changji::infer
