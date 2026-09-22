cmake_minimum_required(VERSION 3.20)
project(changji C CXX)

# **版本号从外面给。** 发布时 CI 用 git tag 填（见
# .github/workflows/release.yml），本地编就是 dev。
# 不给默认值的话本地构建会印一个空串，而"版本号是空的"和
# "这个二进制是自己编的"看着一样——dev 至少说清楚了。
# **版本前缀住在这儿。** 一改它，后面那个计数从 0 重新开始。
#
# 版本号长这样：`<前缀>-<这一版是前缀定下来之后的第几个提交>`，
# 比如 `v2.2-0`（改前缀那一下）、`v2.2-7`（往后七个提交）。
#
# ⚠️ **计数不是随手编的号，是数出来的**：`cpp/tools/version.sh` 去问 git
# ——「这个前缀是哪一个提交写进来的，从那儿到现在隔了几个提交」。所以
# 同一个提交在谁手上算出来都是同一个数，不靠 CI 的 run 编号（那个换条线
# 就不一样了，而且永远不会归零）。
#
# 改版本：把下面这个串改掉，提交。那一下起计数就是 0。
set(CHANGJI_VERSION_PREFIX "v2.2" CACHE STRING "版本前缀；一改它后面那个计数从 0 重来")

set(CHANGJI_VERSION "dev" CACHE STRING "写进二进制的版本号，--version 打印它")
# C 也要启用：ggml 是 C 写的，而且 sd.cpp 声明了 target_compile_features(c_std_11)。
# 只写 CXX 的话 configure 会在 generate 阶段报 "No known features for C compiler"，
# 而那句话看不出和缺 C 语言有关系。

# 全局停在 C++17，只把自己的目标抬到 20。
#
# 原本这里写的是 CMAKE_CXX_STANDARD 20。前置验证工程实测发现那样到阶段 5
# 会炸：llama.cpp 和 ggml 写的是 target_compile_features(... cxx_std_17)
# 加一句 "# don't bump"，而那声明的是**下限不是上限**——父作用域给 20 时
# "至少 17"照样满足，于是上游整个按 C++20 编，然后在 MSVC 上撞 char8_t
# （llama-chat.cpp 的 LU8 宏用 __cplusplus 做守卫，而 MSVC 不加
# /Zc:__cplusplus 时那个宏永远是 199711L，守卫静默失效）。
# 只在 MSVC + C++20 这个组合下出现，MinGW 编不出来，所以阶段 0 没遇到。
#
# 详见 verify/RESULTS.md。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 静态 CRT 要在**拉依赖之前**定，它得传染给所有子项目——
# 一半 /MT 一半 /MD 的话链接时报 RuntimeLibrary 不匹配，
# 而那个错误信息指向的是某个 .obj，看不出是运行库选项的问题。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    # ggml 默认开 OpenMP，那会拖进 VCOMP140.DLL——**MSVC 的 OpenMP 运行时
    # 只有动态版**，/MT 也去不掉它。关掉之后 ggml 用自带的线程池，
    # CPU 推理会慢一点，但这个项目的目标是 GPU 推理，
    # 而"拷过去就能跑"比那点 CPU 性能重要。
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- 依赖 ----
#
# 全部走 FetchContent 而不是 vcpkg/conan：交叉编译到树莓派时，
# 包管理器的三元组配置是另一套要维护的东西，而这几个库都是
# header-only 或极小的，直接拉源码编最省心。
#
# 版本一律钉死。这几个库都在活跃开发，不钉版本会出现
# "昨天能编今天编不过" 的情况。

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON)

FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW ON)

FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW ON)

# ⚠️ **httplib 那三个"有就用"的开关必须显式关掉，然后我们自己把库编进去。**
#
# cpp-httplib 的 CMakeLists 里 HTTPLIB_USE_{OPENSSL,ZLIB,BROTLI}_IF_AVAILABLE
# 默认都是 ON：它 find_package 一遍，**构建机上装了谁就动态链谁**。
#
# 后果是"编出来的二进制依赖哪几个 .dylib，取决于那台构建机上装了什么"。
# macOS runner 上装着 Homebrew，发出去的包就带上了
#
#     /opt/homebrew/opt/brotli/lib/libbrotlicommon.1.dylib
#     /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib
#
# 这种绝对路径。构建机上跑得好好的（库就在那儿），CI 那步"验一下它能起来"
# 也照样绿，用户解开压缩包一跑是
#
#     dyld: Library not loaded: /opt/homebrew/opt/brotli/lib/libbrotlicommon.1.dylib
#
# 没装 Homebrew 的人一辈子不会有那个路径——而这行报错看着像"包坏了"。
#
# **关掉不等于不要这三个功能。** 下面按这个项目一贯的办法：拉源码、钉版本、
# 静态编进二进制。用户机器上不用装任何东西，六个平台编出来的是同一套东西。
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)

# ⚠️ **这一行必须排在 brotli 前面。**
#
# brotli 的 CMakeLists 里是 `option(BUILD_SHARED_LIBS "..." ON)`——
# 缓存里没这一项的话它**自己建一项，而且是 ON**，缓存又是全局的：
# 之后拉进来的 llama.cpp / sd.cpp / ggml 会跟着编成动态库。
# 先钉成 OFF，option() 就不会去动一个已经存在的缓存项了。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# brotli：httplib 的 `Accept-Encoding: br` 走它。
# BUNDLED_MODE 关掉它自己的 install 规则——我们只要那三个静态库。
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(BROTLI_BUNDLED_MODE  ON CACHE BOOL "" FORCE)
FetchContent_Declare(brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.1.0
    GIT_SHALLOW ON)

# Crow 依赖 asio。用 standalone 版而不是 boost 版，
# 少拉一个巨大的依赖，交叉编译也简单得多。
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
    GIT_SHALLOW ON)

FetchContent_MakeAvailable(json tomlplusplus httplib Crow brotli)

# ---- zlib：**只下源码，它的 CMakeLists 不要** ----
#
# zlib 1.3.1 的 CMakeLists 第一行是 `cmake_minimum_required(VERSION 2.4.4...)`，
# 而 CMake 4 把"兼容 3.5 以下"整个删掉了——windows-11-arm 那台 runner 上
# 装的正是 CMake 4.x，add_subdirectory 进去是当场配置错误。它还会顺手编一个
# 动态库和一堆示例程序，我们一个都不要。
#
# 所以走 doctest 那个办法：SOURCE_SUBDIR 指一个不存在的目录，FetchContent
# 就只下载不 add_subdirectory，源码自己列。
#
# 列的是核心那十个文件，**gz*.c 一个没要**：httplib 用的是 deflate/inflate
# 流，gz 那几个是文件接口，而它们要 unistd.h、大文件宏那一套平台判断——
# 正是在 Windows 和交叉编译上最容易出事的部分，而我们一行都用不到。
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW ON
    SOURCE_SUBDIR 这个目录不存在-所以只下载不构建)
FetchContent_MakeAvailable(zlib)

add_library(changji_z STATIC
    "${zlib_SOURCE_DIR}/adler32.c"
    "${zlib_SOURCE_DIR}/compress.c"
    "${zlib_SOURCE_DIR}/crc32.c"
    "${zlib_SOURCE_DIR}/deflate.c"
    "${zlib_SOURCE_DIR}/inffast.c"
    "${zlib_SOURCE_DIR}/inflate.c"
    "${zlib_SOURCE_DIR}/inftrees.c"
    "${zlib_SOURCE_DIR}/trees.c"
    "${zlib_SOURCE_DIR}/uncompr.c"
    "${zlib_SOURCE_DIR}/zutil.c")
target_include_directories(changji_z SYSTEM PUBLIC "${zlib_SOURCE_DIR}")
if(MSVC)
    # zlib 是 C89 写的，/W4 下几十条 C4996 会淹掉真正的警告。
    target_compile_definitions(changji_z PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(changji_z PRIVATE -w)
endif()

# ---- OpenSSL ----
#
# **这一个不走 FetchContent：OpenSSL 不拿 CMake 构建**（是 perl Configure +
# make/nmake 那一套）。所以这里改成"必须有人给我一份静态库"：CI 里由
# .github/workflows/release.yml 里的 openssl 那个 job 现编一份（版本钉死、
# 带缓存），再用 -DOPENSSL_ROOT_DIR 指过来。
#
# **只认静态库**，找不到就当场停。悄悄退回动态链接的话，我们就又回到了
# 上面那个 dyld 报错——而且是在没人注意的时候回去的。
# 本机想快编一版又不关心 https：-DCHANGJI_SSL=OFF。
option(CHANGJI_SSL "编进 OpenSSL（静态），httplib 才发得了 https" ON)
if(CHANGJI_SSL)
    set(OPENSSL_USE_STATIC_LIBS ON)
    # 不用 REQUIRED：它印的是 CMake 那句"try to set OPENSSL_ROOT_DIR"，
    # 看的人还得自己想"那我该装什么"。自己报，把三条路一次说清。
    find_package(OpenSSL 3.0.0 QUIET)
    if(NOT OpenSSL_FOUND)
        message(FATAL_ERROR
            "找不到 OpenSSL ≥ 3.0（httplib 发 https 要它）。三条路：\n"
            "  Linux  apt install libssl-dev\n"
            "  macOS  brew install openssl@3，再加 "
            "-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)\n"
            "  这一版不要 https：-DCHANGJI_SSL=OFF\n"
            "发布用的那份由 CI 自己编（见 .github/workflows/release.yml "
            "里的 openssl 那个 job），不靠构建机上装了什么。")
    endif()
    # find_package 找不到静态库时会**退回动态库**，不报错。自己核一遍。
    # （Windows 上这条弱一些：动态库的导入库也是 .lib。那边的兜底是 CI 里
    # 编的本来就是 no-shared，装出来的目录里根本没有 dll。）
    foreach(_lib "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}")
        get_filename_component(_ext "${_lib}" LAST_EXT)
        if(NOT _ext STREQUAL "${CMAKE_STATIC_LIBRARY_SUFFIX}")
            message(FATAL_ERROR
                "OpenSSL 找到的是动态库：${_lib}\n"
                "发出去的包必须自带 OpenSSL——链到构建机上那份 .dylib/.so，"
                "换台机器就是 Library not loaded。\n"
                "给一份静态的：-DOPENSSL_ROOT_DIR=<装好的前缀>；"
                "或者这一版不要 https：-DCHANGJI_SSL=OFF")
        endif()
    endforeach()
    message(STATUS "OpenSSL（静态）${OPENSSL_VERSION}：${OPENSSL_SSL_LIBRARY}")
endif()

# Crow 1.2.0 的状态码表里没有 422，会把它改写成 500。
# 所有 pydantic 风格的校验错误都受影响，而单元测试看不见——
# 那边不经过 Crow 的写响应那一步。详见脚本里的注释。
include("${CMAKE_CURRENT_LIST_DIR}/cmake/patch_crow_422.cmake")
changji_patch_crow_422("${crow_SOURCE_DIR}/include")

# ⚠️ **httplib 的头文件目录不能是 SYSTEM。**
#
# sd.cpp 把自己的 thirdparty 目录（里面有一份 httplib.h 0.28.0）作为
# INTERFACE 包含目录导出，那是普通的 -I。而 MSVC 的 SYSTEM 目录
# （-external:I）**永远排在普通 -I 之后**——所以只要 httplib 是 SYSTEM，
# sd.cpp 那份就必赢，我们钉的 0.15.3 一行都用不上。
#
# 这不是假想：llm/client_http.cpp 和 doctor.cpp 从链上 sd.cpp 那天起
# 编的一直是 0.28.0。两边碰巧 API 兼容，所以从来没报过错，直到 comfy
# 这边用到 multipart 才炸出来（0.28.0 把 MultipartFormDataItems 改了名）。
# **一个版本号写在构建脚本里、实际编的是另一个版本**，比编不过更危险。
#
# 清掉 SYSTEM 标记之后它就是普通 -I，再配合下面的 BEFORE 排到最前面。
# 代价是 httplib 的警告会进 /W4 的视野，所以用到它的地方包一层
# util/httplib.hpp 把警告按下去。
if(TARGET httplib)
    set_property(TARGET httplib PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
endif()

# ---- stable-diffusion.cpp ----
#
# 阶段 5 的核心依赖。出首帧、出视频都靠它。
#
# **暂时不统一 ggml。** 方案的风险二说统一 ggml 是一笔长期维护成本，
# 但那笔账只在 sd.cpp 和 llama.cpp **同时链接**时才需要付——两个库各自
# vendor 了不同 commit 的 ggml，同时进来才有符号冲突。
#
# 决策 4 之后大模型走远端（[llm] 配置段），llama.cpp 还没进程内化，
# 所以现在 sd.cpp 可以带着自己那份 ggml 单独链。patches/ 下的补丁集
# 留着不动，等真要把 llama.cpp 链进来时再用。
#
# 这不是把问题推给未来：补丁集已经在 CUDA 和 Metal 上都验证过了
# （见 verify/RESULTS.md），只是现在用不上。提前统一等于现在就开始付
# 那笔"两个上游各自前进"的维护成本，而收益要等到 llama.cpp 进来才有。
option(CHANGJI_SD "链接 stable-diffusion.cpp" ON)
# **一次只能开一个。** 四个开关对应 ggml 的四个后端目录，
# 同时开两个的下场见下面 "挑一个 GPU 后端" 那一段。
#
#   CUDA    N 卡原生。最快，也是这个项目唯一在实机上长期跑过的一条。
#   HIP     A 卡原生（ROCm）。编的是 ggml-cuda 那批 .cu，用 hipcc 过一遍。
#   SYCL    Intel 原生（oneAPI）。要 icpx，产物**不是单文件**，
#           跑的机器上得有 oneAPI 运行时。
#   VULKAN  N/A/I 通吃。只要驱动自带的 Vulkan 运行时，产物接近单文件；
#           A 卡和 Intel 卡想"下载解开就能用"的话走这条。
option(CHANGJI_SD_CUDA   "GPU 后端：CUDA（NVIDIA）"            OFF)
option(CHANGJI_SD_HIP    "GPU 后端：HIP / ROCm（AMD）"         OFF)
option(CHANGJI_SD_SYCL   "GPU 后端：SYCL / oneAPI（Intel）"    OFF)
option(CHANGJI_SD_VULKAN "GPU 后端：Vulkan（N/A/I 都能用）"    OFF)

# 名字里带 SD_ 是历史原因（当初只有 sd.cpp 用 GPU）。**它们管的是 ggml，
# 不只是 sd.cpp**——开了 CHANGJI_LLAMA 时进程内配音和文本模型走的是同一个
# 后端。改名要连着 README、doctor 的提示、CI 一起改，收益不抵风险，留着。

# 编给哪些显卡架构。**必须显式给**。
#
# 不给的话交由 CMake/nvcc 的默认值决定，而默认值经常不含新卡的架构。
# 那时候不会报错——CUDA 退回 PTX JIT，第一次跑每个内核现编一遍。
# 表现是"能跑，但莫名其妙地慢"，而且慢在哪儿完全看不出来。
#
# 常见的几张：
#   L20 / L40S / RTX 4090   → 89（Ada）
#   A100                    → 80
#   H100                    → 90
#   RTX 3090 / A10          → 86
#   RTX 5090                → 120（Blackwell，要 CUDA ≥ 12.8）
#   RTX 2060（开发机）       → 75（Turing）
#
# 多种卡混着用就写 "75;86;89"，编译时间和产物大小会跟着涨。
# 带后缀的写法（CMake ≥ 3.18）在发布包里很有用：
#   `89-real`    只出 sm_89 的机器码
#   `75-virtual` 只出 compute_75 的 PTX，**比它新的卡都能 JIT 上去**
# 发布包用的那一串在 .github/workflows/release.yml 里，一处改。
#
# `native` 只编当前机器上那张卡的——**在容器里构建时别用**，
# 构建机和跑的机器不一定是同一张卡。
set(CHANGJI_CUDA_ARCH "89" CACHE STRING
    "CUDA 架构号，分号隔开。L20/L40S/4090 是 89，A100 是 80，H100 是 90，5090 是 120")

# **把 CUDA 的运行时库静态链进二进制**，解开就只有一个可执行文件。
#
# 默认关。装了 CUDA 的开发机上动态链本来就跑得好好的，而静态那一下要多花
# 链接时间、峰值内存也高（几百兆的归档往里塞）。**这是发布包才需要的形态**，
# CI 的 linux-x64-cuda 那格开着。
#
# ⚠️ **只有 Linux 做得到。NVIDIA 在 Windows 上根本不提供静态 cuBLAS**——
# 13.4 的 redist 里只有 cublas64_13.dll（51.6 MB）、cublasLt64_13.dll
#（455.7 MB）和几百 KB 的导入库，一个静态库都没有。上游 ggml 的注释说的是
# 同一件事：「As of 12.3.1 CUDA Toolkit for Windows does not offer a static
# cublas library」。所以 Windows 的 CUDA 包必然是 exe 加那两个 DLL，
# 见 release.yml 里「打包（Windows）」那一段。
#
# 开了之后 ldd 上就没有 libcudart / libcublas / libcublasLt 了，只剩
# libcuda.so.1——**那个永远来自显卡驱动**，哪种打包方式都塞不进包里。
option(CHANGJI_CUDA_STATIC "CUDA 运行时静态链进去（发布包用，只 Linux 做得到）" OFF)

# A 卡这边对应的东西叫 gfx 号，不是数字。查自己那张卡：`rocminfo | grep gfx`。
#
#   RX 6800/6900、W6800     → gfx1030（RDNA2）
#   RX 7900 XTX/XT          → gfx1100（RDNA3）
#   RX 7800/7700            → gfx1101
#   RX 7600、780M 核显      → gfx1102
#   RX 9070                 → gfx1201（RDNA4，要 ROCm ≥ 6.4）
#   MI210/MI250             → gfx90a
#   MI300                   → gfx942
#
# **和 CUDA 那边不一样：HIP 没有 PTX 那种"新卡能 JIT 老代码"的兜底。**
# 列表里没有的卡就是跑不了，运行时报 "no kernel image is available"。
# 所以这个列表宁可长一点，代价是编译时间和产物大小。
set(CHANGJI_HIP_ARCH "gfx1030;gfx1100;gfx1101;gfx1102" CACHE STRING
    "AMD GPU 架构号（gfx 开头），分号隔开。`rocminfo | grep gfx` 查自己的")

# ---- llama.cpp（阶段 9：进程内 TTS）----
#
# **默认关。** 开着会让每一次干净构建多编一份 llama.cpp + 一份打过补丁的
# ggml，而阶段 8 之前没有哪条主线功能用得到它。
#
# 为什么现在把它接进来：Qwen3-TTS 已经进了 llama.cpp 主线
# （PR #26254，2026-08-04 合并，走 mtmd），配音的 C++ 化不再需要
# 自己写 codec，但需要 llama.cpp 在进程里。见方案风险一那一节。
#
# **两份 ggml 的问题在前置验证工程里已经解掉了**（决策 17）：
# 统一的那份 ggml 是"llama.cpp 自带的 + leejet 扩展补丁"，
# CPU 和 CUDA 两条链都验过（verify/RESULTS.md）。这里做的是把那套
# 手工步骤自动化——verify/ 是一次性工程，人可以在中间插一次手；
# 主工程靠 FetchContent 拉源码，没人插得进去。
option(CHANGJI_LLAMA "链接 llama.cpp（进程内 TTS）" OFF)

# ---- 挑一个 GPU 后端 ----
#
# **这一段的位置是要紧的：它必须跑在 FetchContent 拉 llama.cpp 和 sd.cpp
# 之前。**
#
# ggml 的后端开关（GGML_CUDA / GGML_HIP / GGML_SYCL / GGML_VULKAN）是 ggml
# 自己的 CMakeLists 读的，读完就决定了 add_subdirectory 哪几个后端目录。
# **谁先把 ggml 带进来，谁那一刻看到的值就说了算**——开了 CHANGJI_LLAMA 时
# 那是 llama.cpp，而它在下面几十行就拉下来了。
#
# 这几行原来写在 `if(CHANGJI_SD)` 里，也就是拉完 llama.cpp 之后。于是
# `CHANGJI_LLAMA=ON` 加任何一个 GPU 后端，**干净配置一次编出来的二进制里
# 根本没有那个后端**。清掉 build 目录重配才复现，因为第二次配置时
# GGML_CUDA 已经躺在缓存里了——"第二次就好了"正是这类故障最难查的地方。
# 症状和下面 nvcc 那一条一模一样：不报错，全在 CPU 上跑。
#
# 四个后端互斥。CUDA 和 HIP 编的是同一批 .cu，物理上不可能同时存在；
# 另外两个技术上叠得起来，但那只是把编译时间翻倍换一个没人用的组合。
set(CHANGJI_GPU_BACKEND "")
foreach(pair "CHANGJI_SD_CUDA;CUDA" "CHANGJI_SD_HIP;HIP"
             "CHANGJI_SD_SYCL;SYCL" "CHANGJI_SD_VULKAN;Vulkan")
    list(GET pair 0 _flag)
    list(GET pair 1 _name)
    if(${_flag})
        if(CHANGJI_GPU_BACKEND)
            message(FATAL_ERROR
                "GPU 后端一次只能开一个，现在 ${CHANGJI_GPU_BACKEND} 和 "
                "${_name} 都开着。\n"
                "  N 卡：-DCHANGJI_SD_CUDA=ON\n"
                "  A 卡：-DCHANGJI_SD_HIP=ON（或者 -DCHANGJI_SD_VULKAN=ON）\n"
                "  Intel：-DCHANGJI_SD_SYCL=ON（或者 -DCHANGJI_SD_VULKAN=ON）")
        endif()
        set(CHANGJI_GPU_BACKEND "${_name}")
    endif()
endforeach()

if(CHANGJI_SD_CUDA)
    set(SD_CUDA ON CACHE BOOL "" FORCE)
    # **llama.cpp 的 ggml 走的是另一个开关。**
    # 开了 CHANGJI_LLAMA 时 ggml 由 llama.cpp 提供，而它的 CUDA 后端
    # 由 GGML_CUDA 控制，不认 SD_CUDA。只设 SD_CUDA 的话编出来的是
    # 一个**完全没有 CUDA 的**二进制——不报错，全在 CPU 上跑，
    # 表现就是"能跑但莫名其妙地慢"。
    # 实测过：那一版 GGML_CUDA:BOOL=OFF，连 libggml-cuda.a 都没有。
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
    set(CMAKE_CUDA_ARCHITECTURES "${CHANGJI_CUDA_ARCH}" CACHE STRING "" FORCE)

    # **配置期就要确认 nvcc 找得到，找不到当场停。**
    #
    # 不停的话整条链子一声不吭：CMake 配置成功、编译成功、服务起得来、
    # 任务开跑——只是 ggml 的 CUDA 后端根本没编进去，全部退到 CPU。
    # 2026-09-10 在 5090 上栽过：手敲配置命令漏了
    # `-DCMAKE_CUDA_COMPILER`，而那台机器的 nvcc 不在 PATH 上。
    # 结果是 1280×704 的一镜跑了五个多小时才两步，进程占着 52 GB 内存、
    # 395% CPU，而 `nvidia-smi` 上一个计算进程都没有。
    # 查出来靠的是 `strings 二进制 | grep -c ggml_cuda` —— 0 对 2348。
    #
    # 已经设过 CMAKE_CUDA_COMPILER 的话就信它（交叉编译、多版本共存
    # 都可能指到非默认位置）；没设就自己找一遍常见路径。
    if(NOT CMAKE_CUDA_COMPILER)
        find_program(CHANGJI_NVCC nvcc
            HINTS ENV CUDA_PATH ENV CUDA_HOME
            PATHS /usr/local/cuda/bin /usr/local/cuda-12/bin
                  /usr/local/cuda-13/bin
            PATH_SUFFIXES bin)
        if(CHANGJI_NVCC)
            set(CMAKE_CUDA_COMPILER "${CHANGJI_NVCC}" CACHE STRING "" FORCE)
            message(STATUS "CUDA 编译器：${CHANGJI_NVCC}（自动找到的）")
        else()
            message(FATAL_ERROR
                "开了 CHANGJI_SD_CUDA 但找不到 nvcc。\n"
                "**不能就这么编下去**：编出来的二进制没有 CUDA 后端，"
                "不报错、只是全在 CPU 上跑，慢一百倍而且很难看出原因。\n"
                "  装了 CUDA 的话显式指一下："
                "-DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc\n"
                "  真不想用 CUDA 就关掉：-DCHANGJI_SD_CUDA=OFF")
        endif()
    endif()

    if(CHANGJI_CUDA_STATIC)
        if(WIN32)
            message(FATAL_ERROR
                "CHANGJI_CUDA_STATIC 在 Windows 上做不到：NVIDIA 不提供静态 "
                "cuBLAS（redist 里只有 cublas64_*.dll 和几百 KB 的导入库）。\n"
                "  Windows 的 CUDA 包就是 exe 加 cublas64_*.dll / "
                "cublasLt64_*.dll，打包那步会把它们带上，别在这儿较劲。")
        endif()
        # GGML_STATIC 换的就是 ggml-cuda 链哪一份 CUDA 库：
        #   关 → CUDA::cudart / CUDA::cublas                    （.so，得跟着包走）
        #   开 → CUDA::cudart_static / cublas_static / cublasLt_static
        #
        # 静态库在 `libcublas-dev` / `cuda-cudart-dev` 里（前者装完 893 MB，
        # 比只有 .so 的运行时包多出来的那 344 MB 就是它们）。这两个包 CI 上
        # 本来就装。缺了的话 CUDA::cublas_static 这个目标压根不存在，
        # CMake 在生成阶段就会说「links to target ... but the target was not
        # found」——响，不会悄悄退回动态。
        #
        # 它顺手还在 ggml/src 那层 add_link_options(-static)。**那行到不了
        # changji**：add_link_options 是目录作用域、不传递，而 ggml 那几个
        # 目标在这个配置下都是静态库，归档时根本不看链接选项。这正是要的
        # 结果——**全静态 + CUDA 是跑不起来的**，libcuda.so.1 必须由显卡
        # 驱动动态提供。
        set(GGML_STATIC ON CACHE BOOL "" FORCE)
        message(STATUS "CUDA 运行时：静态链接（产物是单个可执行文件）")
    endif()

    message(STATUS "CUDA 架构：${CHANGJI_CUDA_ARCH}"
                   "（不对的话会退回 PTX JIT，不报错但很慢）")

elseif(CHANGJI_SD_HIP)
    set(SD_HIPBLAS ON CACHE BOOL "" FORCE)
    set(GGML_HIP ON CACHE BOOL "" FORCE)

    # **架构列表三个名字都给。** ggml 这两年把 AMDGPU_TARGETS 改叫
    # GPU_TARGETS，再由它推出 CMAKE_HIP_ARCHITECTURES；Windows 上又不走
    # enable_language(HIP)、直接读 CMAKE_HIP_ARCHITECTURES。三个都设上，
    # 免得跟着上游改名走——设多了不会错，漏了的表现是**编出来只有默认那
    # 一两个 gfx**，换张卡跑就 "no kernel image is available for execution"。
    set(GPU_TARGETS        "${CHANGJI_HIP_ARCH}" CACHE STRING "" FORCE)
    set(AMDGPU_TARGETS     "${CHANGJI_HIP_ARCH}" CACHE STRING "" FORCE)
    set(CMAKE_HIP_ARCHITECTURES "${CHANGJI_HIP_ARCH}" CACHE STRING "" FORCE)

    # 同 nvcc 那条：找不到工具链当场停，不要等到链接期报一堆找不到
    # hipblas 符号——那时候没人会想到是"ROCm 没装/没在 PATH 上"。
    # **只认 ROCm 自带的那几个名字。** 原来这里还写了 `clang++`，
    # 那会在一台装了普通 clang、没装 ROCm 的机器上找到 /usr/bin/clang++
    # 然后放行——这个检查存在的全部意义就是不放行那种机器。
    find_program(CHANGJI_HIPCC NAMES hipcc hipcc.bat amdclang++
        HINTS ENV HIP_PATH ENV ROCM_PATH
        PATHS /opt/rocm /opt/rocm/llvm
        PATH_SUFFIXES bin)
    if(NOT CHANGJI_HIPCC)
        message(FATAL_ERROR
            "开了 CHANGJI_SD_HIP 但找不到 ROCm 的编译器。\n"
            "**不能就这么编下去**：理由同 CUDA 那条，编出来是个没有 GPU "
            "后端的二进制。\n"
            "  Linux：装 ROCm（rocm-hip-sdk + rocblas-dev + hipblas-dev），"
            "然后 export ROCM_PATH=/opt/rocm\n"
            "  Windows：装 AMD HIP SDK，配置时把 C/C++ 编译器指到 "
            "%HIP_PATH%/bin/clang++.exe\n"
            "  只想用 A 卡但不想折腾 ROCm：改用 -DCHANGJI_SD_VULKAN=ON，"
            "那条只要驱动")
    endif()

    message(STATUS "HIP 架构：${CHANGJI_HIP_ARCH}"
                   "（列表外的卡跑不了，HIP 没有 PTX 那种兜底）")

elseif(CHANGJI_SD_SYCL)
    set(SD_SYCL ON CACHE BOOL "" FORCE)
    set(GGML_SYCL ON CACHE BOOL "" FORCE)

    # **必须是 Intel 的 icx/icpx。** sd.cpp 往 CMAKE_CXX_FLAGS 里直接塞
    # `-fsycl`，MSVC 和 GCC 都不认那个参数。让它编下去的话报的是几百行
    # 模板展开错误，第一行离真正的原因（编译器选错了）差着十万八千里。
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
        message(FATAL_ERROR
            "开了 CHANGJI_SD_SYCL，但 C++ 编译器是 "
            "${CMAKE_CXX_COMPILER_ID}，不是 IntelLLVM。\n"
            "  先进 oneAPI 的环境（Linux：source /opt/intel/oneapi/setvars.sh，"
            "Windows：setvars.bat），再配置：\n"
            "    -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx"
            "（Windows 上是 icx-cl）\n"
            "  只想用 Intel 卡但不想装 oneAPI：改用 -DCHANGJI_SD_VULKAN=ON")
    endif()

    # **这条产物不是单文件。** SYCL 要 oneAPI 的运行时（libsycl、
    # libsvml、MKL 那一串），静态链不了。跑的机器上得有 oneAPI 运行时，
    # 或者把那些库跟二进制放一起。发布包在 release.yml 里是这么处理的。
    message(WARNING
        "SYCL 后端：编出来的二进制**依赖 oneAPI 运行时**，不是单文件。"
        "另外 leejet 那套 ggml 扩展补丁没有覆盖 ggml-sycl，"
        "fp8 权重多半跑不起来——见 patches/README.md。")

elseif(CHANGJI_SD_VULKAN)
    set(SD_VULKAN ON CACHE BOOL "" FORCE)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)

    # ggml-vulkan 编之前要先把几百个 .comp 着色器用 glslc 编成 SPIR-V
    # （它自己建一个 vulkan-shaders-gen host 工具来干这事）。少了 glslc
    # 的表现是构建到一半报"找不到 glslc"，而那时候已经等了十几分钟。
    find_package(Vulkan QUIET COMPONENTS glslc)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR
            "开了 CHANGJI_SD_VULKAN 但找不到 Vulkan SDK。\n"
            "  Linux：装 LunarG 的 vulkan-sdk，或者发行版的 "
            "libvulkan-dev + glslc\n"
            "  Windows：装 https://vulkan.lunarg.com 的 SDK，"
            "装完 VULKAN_SDK 这个环境变量会指到它")
    endif()
    if(NOT Vulkan_glslc_FOUND)
        message(FATAL_ERROR
            "Vulkan 找到了，但没有 glslc。ggml 的着色器编不出来。\n"
            "  Ubuntu：apt install glslc（或者装完整的 vulkan-sdk）")
    endif()
    message(STATUS "Vulkan：${Vulkan_LIBRARY}，glslc：${Vulkan_GLSLC_EXECUTABLE}")
endif()

if(CHANGJI_GPU_BACKEND)
    message(STATUS "GPU 后端：${CHANGJI_GPU_BACKEND}")
else()
    # 不是 FATAL：单元测试和协调者那份就是要 CPU 的。但要说出来——
    # "编好了但出图特别慢"最常见的原因就是这一行没人看见。
    message(STATUS "GPU 后端：没有，全走 CPU。出片会慢到不可用，"
                   "一镜按小时算。要 GPU 就开 CHANGJI_SD_CUDA / "
                   "CHANGJI_SD_HIP / CHANGJI_SD_SYCL / CHANGJI_SD_VULKAN 之一")
endif()

# **GGML_MAX_NAME 要在顶层定义，链了 ggml 就要，不管它是谁带进来的。**
#
# sd.cpp 自己的 CMakeLists 里有 `add_definitions(-DGGML_MAX_NAME=160)`，
# 但 add_definitions 只作用于**它之后的目录作用域**——加到了 ggml 上，
# 没加到我们自己的源文件上。于是默认构建里库是 160、本体是 64，
# **编译和链接全都不报错**。
#
# 这不是推理出来的：doctor 的 ggml ABI 探针接上之后第一次跑就报了 fail，
# "库是按 160 编的，本体是 64"。在此之前它已经这样很久了，
# 只是本体里没有人直接读写 ggml_tensor 的字段，所以还没出事。
if(CHANGJI_SD OR CHANGJI_LLAMA)
    add_compile_definitions(GGML_MAX_NAME=160)
endif()

if(CHANGJI_LLAMA)
    # 上面那行原来在这个块里。留着这段注释是因为它解释了后果：
    # sd.cpp 自己的 CMakeLists 里有 `add_definitions(-DGGML_MAX_NAME=160)`，
    # 而 add_definitions 只作用于它之后的目录作用域——加不到别处已经编好的
    # ggml 上。结果是 ggml 按 64 编、sd.cpp 按 160 编，两边看到的 ggml_tensor
    # 大小不一致，**链接不报错**。所以在顶层统一定义。
    # verify/main.cpp 的 check_ggml_abi() 是这一条的运行时探针：
    # 写一个 100 字符的张量名再读回来，读回 63 就说明这行没生效。

    # 打补丁要一个 Python。找不到就当场停，不要配置期悄悄过、
    # 编译期报一堆"找不到 GGML_TYPE_F8_E4M3"——那时候没人会想到是这儿。
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    foreach(opt LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES
                LLAMA_BUILD_SERVER LLAMA_BUILD_APP
                LLAMA_BUILD_UI LLAMA_OPENSSL)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()

    # **common 要开。** 上面那批全关掉时它也在里面，本来是对的——
    # 配音只要 mtmd。但进程内跑文本大模型要**按 JSON Schema 约束输出**，
    # 而 `json_schema_to_grammar` 只在 common 里，libllama 的公开头文件里
    # 只有吃 GBNF 文本的 `llama_sampler_init_grammar`。
    #
    # 不约束的后果不是"偶尔格式不对"：整条流水线是无人值守跑的，
    # 一次解析失败就是一章卡住，而模型下一次未必吐得更好。
    #
    # mtmd 那边**不能**链 common（它自己的 CMakeLists 里有断言），
    # 我们链在 changji 上，两者不冲突。
    set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)

    # **配音要 mtmd，而 mtmd 在 tools/ 底下。**
    # Qwen3-TTS 的实现（models/qwen3tts-gen.cpp、qwen3tts-spkenc.cpp）
    # 在 mtmd 里，不在 libllama 里。上游正好留了这个钩子：
    # LLAMA_BUILD_MTMD 单独编 mtmd，不用把整个 tools/ 和 common 拖进来。
    set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)

    # **视频支持关掉。** mtmd 的 MTMD_VIDEO 要 LLAMA_SUBPROCESS，
    # 而且运行时要 PATH 里有 ffmpeg 二进制。我们用不到（配音只出音频），
    # 开着等于给二进制加一个用不上的运行时依赖。
    set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        # 钉死，理由同 sd.cpp。这个 commit 是前置验证工程验过的那个。
        GIT_TAG 5202104
        GIT_SHALLOW ON
        # 拉下来就地打补丁。脚本是幂等的——FetchContent 什么时候重跑
        # PATCH_COMMAND 并不好预测，而 git apply 打第二遍会失败。
        PATCH_COMMAND ${CMAKE_COMMAND} -E env
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_to_llamacpp.py"
            "<SOURCE_DIR>")
    FetchContent_MakeAvailable(llama)
endif()

if(CHANGJI_SD)
    if(CHANGJI_LLAMA)
        set(CHANGJI_SD_SUBMODULES "")
    else()
        set(CHANGJI_SD_SUBMODULES "ggml")
    endif()

    # 只要 ggml 那个子模块。webui 是个前端仓库，libwebp/libwebm 我们关掉了
    # 对应的功能——全递归拉下来是几百兆，而且拉一个前端仓库来编后端很荒唐。
    set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SD_WEBP OFF CACHE BOOL "" FORCE)
    set(SD_WEBM OFF CACHE BOOL "" FORCE)
    set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # GPU 后端的开关（SD_CUDA / SD_HIPBLAS / SD_SYCL / SD_VULKAN 和对应的
    # GGML_*）在上面"挑一个 GPU 后端"那一段里就设好了，**不在这儿**。
    # 理由见那一段：晚一步设，开了 CHANGJI_LLAMA 时就赶不上了。

    FetchContent_Declare(stable_diffusion_cpp
        GIT_REPOSITORY https://github.com/leejet/stable-diffusion.cpp.git
        # 钉死。ggml 和 sd.cpp 都在活跃开发，不钉版本会出现
        # "昨天能编今天编不过"，而这两个的编译时间以分钟计。
        GIT_TAG 9cdb6b6fc05edf2d446bfe881a4c462b241b3025
        # 开了 CHANGJI_LLAMA 的时候不拉 ggml 子模块：ggml 目标已经由
        # llama.cpp 建好了，sd.cpp 的 CMakeLists 写着 `if (NOT TARGET ggml)`，
        # 会自己跳过。拉了也是白拉几百兆。
        GIT_SUBMODULES "${CHANGJI_SD_SUBMODULES}"
        GIT_SHALLOW OFF)
    FetchContent_MakeAvailable(stable_diffusion_cpp)

    if(CHANGJI_LLAMA)
        # **sd.cpp 用相对路径拿 ggml 的内部头**：三个 .cpp 里写着
        # `#include "ggml/src/ggml-impl.h"`，那是相对它自己那个 ggml 子模块的。
        # 我们没拉那个子模块（ggml 由 llama.cpp 提供），所以要把 llama.cpp
        # 的源码根加进搜索路径——它底下正好也是 ggml/src/ggml-impl.h。
        #
        # 前置验证工程没撞上这个：它钉的 sd.cpp 是 d8fb10c(master-846)，
        # 而主工程钉的是 9cdb6b6(master-848)，中间那两个提交新增了
        # compute_workspace.cpp。**"验证工程验过"不等于"换个版本还成立"。**
        #
        # 头和库必须同源：库是 llama.cpp 那份打过补丁的 ggml，头也得是它，
        # 否则又回到 GGML_MAX_NAME 那类"编得过、跑起来踩内存"的问题。
        target_include_directories(stable-diffusion PRIVATE "${llama_SOURCE_DIR}")
    endif()
endif()

# ---- 本体 ----

# hardware.cpp 在 macOS 上要的那一个 .mm。两个目标（changji 和
# changji_tests）都编 hardware.cpp，所以两边都要带上它，否则是
# "undefined symbol: metal_memory"——而那条报错看不出和 Metal 有关系。
#
# ⚠️ **不要用 `enable_language(OBJCXX)`。**
#
# 那是第一版的写法，结果是 llama.cpp 整个编不过：
#
#     ggml-metal-context.m:102: error: cannot initialize a variable of type
#     'ggml_metal_t' with an rvalue of type 'void *'
#
# 因为 CMake 的 `CMAKE_OBJCXX_SOURCE_FILE_EXTENSIONS` 里**带着 `m`**：
# 一开 OBJCXX，ggml 那一堆 `.m`（Objective-C，C 的规矩，void* 隐式转换）
# 就全都按 Objective-C++ 编了，而 C++ 不许那么转。**为了自己一个文件，
# 把上游几十个文件的语言改掉了**，而报错指向的是上游的源码。
#
# 所以只给这一个文件指定语言和方言：按 C++ 编，加一句 `-x objective-c++`。
# 全局的语言映射一个字都不动。
#
# ARC 只开在这一个文件上：MTLCreateSystemDefaultDevice 是 Create 系的，
# 手动管引用计数就要记得 release，而开了 ARC 那件事不用想。
if(APPLE)
    set(CHANGJI_METAL_SOURCES src/models/hardware_metal.mm)
    set_source_files_properties(src/models/hardware_metal.mm PROPERTIES
                                LANGUAGE CXX
                                COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
else()
    set(CHANGJI_METAL_SOURCES "")
endif()

# ---- 提示词：prompts.toml → 生成头 ----
#
# 所有喂给大模型和出图模型的提示词都在 prompts.toml 里，构建时由
# tools/gen_prompts.py 生成成一个头（generated/stages/prompts.inc.hpp），
# 编进二进制。生成到构建目录、不进版本库，和 bundled_webapp.inc.hpp 那份
# 刻意不同：前端那份进了版本库，代价是有"谁忘了跑脚本"的窗口（编出旧界面、
# 没有任何地方报错）；提示词一天改几次，这个窗口开着迟早出事，所以让 ninja
# 管依赖——prompts.toml 一变就重新生成，引用它的 .cpp 跟着重编。
#
# 找不到 Python 就在配置期停下。CHANGJI_LLAMA 那一段也要它（打补丁），
# 这里是无条件的：没有提示词什么都编不了。
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(CHANGJI_PROMPTS_TOML "${CMAKE_CURRENT_SOURCE_DIR}/prompts.toml")
set(CHANGJI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CHANGJI_PROMPTS_HPP "${CHANGJI_GENERATED_DIR}/stages/prompts.inc.hpp")
add_custom_command(
    OUTPUT "${CHANGJI_PROMPTS_HPP}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_prompts.py"
            "${CHANGJI_PROMPTS_TOML}" "${CHANGJI_PROMPTS_HPP}"
    DEPENDS "${CHANGJI_PROMPTS_TOML}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_prompts.py"
    COMMENT "提示词：prompts.toml → stages/prompts.inc.hpp"
    VERBATIM)
# 单独一个 target，changji 和 changji_tests 都挂在它上面——同一个生成文件
# 给多个目标用，CMake 文档推荐的就是这个写法，而不是把 OUTPUT 列进每个
# 目标的源文件里。
add_custom_target(changji_prompts DEPENDS "${CHANGJI_PROMPTS_HPP}")

add_library(changji_core STATIC
    src/util/paths.cpp
    src/util/say.cpp
    src/util/proc.cpp
    src/util/cmdline.cpp
    src/util/sysstat.cpp
    src/net/ws_client.cpp
    src/config/settings.cpp
    src/config/model_patch.cpp
    src/config/writeback.cpp
    src/config/runtime.cpp
    src/doctor/doctor.cpp
    src/lan/peers.cpp
    src/lan/sense.cpp
    src/doctor/doctor_json.cpp
    src/models/shot.cpp
    src/models/character.cpp
    src/models/project.cpp
    src/models/story.cpp
    src/models/hardware.cpp
    ${CHANGJI_METAL_SOURCES}
    src/util/text.cpp
    src/util/human_time.cpp
    src/util/fs_time.cpp
    src/stages/json_extract.cpp
    src/stages/bible.cpp
    src/stages/storyboard.cpp
    src/stages/script.cpp
    src/stages/story_plan.cpp
    src/stages/story_reverse.cpp
    src/stages/json_stream.cpp
    src/stages/json_partial.cpp
    src/stages/repetition.cpp
    src/stages/story_revise.cpp
    src/stages/story_outline.cpp
    src/stages/script_story.cpp
    src/stages/story_import.cpp
    src/stages/story_analyze.cpp
    src/stages/story_understand.cpp
    src/stages/web_tools.cpp
    src/stages/story_from_web.cpp
    src/stages/chapter_write.cpp
    src/stages/ref_images.cpp
    src/stages/prompt_compose.cpp
    src/stages/frames.cpp
    src/stages/render.cpp
    src/stages/audio_plan.cpp
    src/stages/audio.cpp
    src/stages/music.cpp
    src/stages/tts_backends.cpp
    src/infer/ggml_abi.cpp
    src/infer/llama_tts.cpp
    src/infer/llama_chat.cpp
    src/infer/scheduler.cpp
    src/infer/sd_backend.cpp
    src/infer/lora_names.cpp
    src/infer/sd_image.cpp
    src/infer/sd_video.cpp
    src/infer/sd_upscale.cpp
    src/llm/client.cpp
    src/llm/thinking.cpp
    src/llm/sse.cpp
    src/llm/client_http.cpp
    src/llm/client_command.cpp
    src/llm/local_client.cpp
    src/llm/call_log.cpp
    src/infer/capability.cpp
    src/infer/blob.cpp
    src/infer/node_pick.cpp
    src/infer/node_prefs.cpp
    src/infer/node_proxy.cpp
    src/infer/node_json.cpp
    src/infer/node_registry.cpp
    src/infer/peer_auth.cpp
    src/infer/node_status.cpp
    src/infer/local_exec.cpp
    src/infer/task_run.cpp
    src/infer/worker_pool.cpp
    src/infer/worker_farm.cpp
    src/infer/worker_server.cpp
    src/infer/worker_proto.cpp
    src/agent/transcript.cpp
    src/agent/tools.cpp
    src/agent/loop.cpp
    src/http/chat_api.cpp
    src/http/rail.cpp
    src/http/enums.cpp
    src/http/flow.cpp
    src/http/film.cpp
    src/pipeline/film_join.cpp
    src/http/webapp.cpp
    src/http/ws.cpp
    src/http/offload.cpp
    src/http/job_stream.cpp
    src/http/prompt_peek.cpp
    src/pipeline/jobs.cpp
    src/pipeline/activity.cpp
    src/pipeline/task_board.cpp
    src/pipeline/episode.cpp
    src/pipeline/preview.cpp
    src/pipeline/storyboard_run.cpp
    src/media/ffmpeg.cpp
    src/media/subtitles.cpp
    src/gates/checks.cpp
    src/media/assemble.cpp
    src/media/watermark.cpp
    src/media/product_tags.cpp
    src/http/readonly.cpp
    src/http/editing.cpp
    src/http/editing_assets.cpp
    src/http/editing_batch.cpp
    src/http/media.cpp
    src/http/upload.cpp
    src/http/tts_api.cpp
    src/http/ref_gen.cpp
    src/http/scripting.cpp
    src/http/story_api.cpp
    src/http/oneclick.cpp
    src/http/planning.cpp
    src/http/batch.cpp
    src/http/episodes.cpp
    src/http/projects.cpp
    src/http/llm_info.cpp
    src/http/config_api.cpp
    src/http/config_api_doctor.cpp
    src/http/reset.cpp
    src/http/run.cpp
    src/http/voices.cpp
    src/http/run_deps.cpp
    src/setup/catalog.cpp
    src/setup/autostart.cpp
    src/setup/update_check.cpp
    src/setup/source.cpp
    src/setup/downloader.cpp
    src/http/setup_api.cpp
    src/http/server.cpp
)

# **可执行只剩一个 `main.cpp`。** 引擎整个编进 `changji_core`，第二个前端
# （`desktop/` 那个 Qt 窗口）链的是同一份 .a——不重编一遍，也就不可能
# 编出两套行为。
#
# **测试目标不链它，那是有意的。** `changji_tests` 自己列的那一百多个源文件
# 一个网络库都不链（Crow / httplib / OpenSSL 全不进来，见那一段的注释）。
# 让它链 core 会把这些全拖进测试二进制：编译时间和"测的到底是不是纯逻辑"
# 两头都变差。
# ---- 局域网感知那一套要的 mDNS ----
#
# **苹果自带**（Bonjour 在 libSystem 里，什么都不用链）。
# **Linux 上是 Avahi 的兼容层**（`libavahi-compat-libdnssd-dev`）：头文件和
# 函数名和苹果那套一模一样，所以 `lan/sense.cpp` 两个平台共用一份代码。
#
# ⚠️ **"一模一样"只对了一半。** 2026-09-22 在树莓派上第一次真编，没编过：
# Avahi 那份头文件停在**老一档 API**，`kDNSServiceFlagsShareConnection`
# 压根没有。苹果那套是「一条共用连接挂住注册/浏览/解析，一个 fd」，
# Avahi 上只能**各起各的连接、各一个 fd**。所以下面再探一次这个常量在不在
# ——按平台写死的话，哪天 Avahi 补上了我们还蒙在鼓里。
#
# ⚠️ **探不到就不开，不是编不过。** 那时候 `Sense::supported()` 回 false，
# 界面上那个开关点不动、接口回 501 照实说——比"编出来一个开了没反应的开关"
# 强。判据写在 `sense.cpp` 开头。
include(CheckCXXSourceCompiles)

# 这一条连接能不能共用。苹果有，Avahi 的兼容层没有。
function(changji_probe_share_connection inc lib)
    set(CMAKE_REQUIRED_INCLUDES "${inc}")
    set(CMAKE_REQUIRED_LIBRARIES "${lib}")
    check_cxx_source_compiles("
        #include <dns_sd.h>
        int main() { return (int)kDNSServiceFlagsShareConnection; }
    " CHANGJI_DNSSD_SHARE)
endfunction()

if(APPLE)
    set(CHANGJI_DNSSD ON)
    set(CHANGJI_DNSSD_LIB "")
    changji_probe_share_connection("" "")
else()
    find_path(DNSSD_INCLUDE_DIR dns_sd.h PATH_SUFFIXES avahi-compat-libdns_sd)
    find_library(DNSSD_LIBRARY NAMES dns_sd)
    if(DNSSD_INCLUDE_DIR AND DNSSD_LIBRARY)
        set(CHANGJI_DNSSD ON)
        set(CHANGJI_DNSSD_LIB ${DNSSD_LIBRARY})
        message(STATUS "局域网感知：找到 dns_sd（${DNSSD_LIBRARY}）")
        changji_probe_share_connection("${DNSSD_INCLUDE_DIR}" "${DNSSD_LIBRARY}")
    else()
        set(CHANGJI_DNSSD OFF)
        set(CHANGJI_DNSSD_LIB "")
        message(STATUS "局域网感知：这台机器上没有 dns_sd，那一档关着")
    endif()
endif()

add_executable(changji src/main.cpp)
target_link_libraries(changji PRIVATE changji_core)

# 自己的代码要 C++20，但只加在自己的目标上，不传染给上游。
# 阶段 5 链 sd.cpp / llama.cpp 时这一点是必须的，见上面 CMAKE_CXX_STANDARD 那段。
target_compile_features(changji_core PUBLIC cxx_std_20)

# ⚠️ **`/bigobj`：翻译表把一个 .obj 撑爆了。**
#
# `util/say.cpp` 里 include 的那份烤好的表（`say_tables.inc.hpp`，十一种语言
# 一万两千多句）编出来的节数超过了 COFF 默认上限，MSVC 报「error C1128：
# 节数超过对象文件格式限制」。**只有 Windows 有这个限制**——Mac 和 Linux 上
# 编得好好的，2026-09-22 在那台 Windows 上第一次真编才撞见。
#
# 加在整个 core 上，不是单个文件上：表还会长，而下一个被撑爆的是谁不会
# 提前告诉你。
if(MSVC)
    target_compile_options(changji_core PRIVATE /bigobj)
endif()

# mDNS 那一套：探到了才开（见上面那一段）。
if(CHANGJI_DNSSD)
    target_compile_definitions(changji_core PRIVATE CHANGJI_HAS_DNSSD)
    if(CHANGJI_DNSSD_SHARE)
        # 苹果那套：注册/浏览/解析全挂在一条连接上，一个 fd。
        target_compile_definitions(changji_core PRIVATE CHANGJI_DNSSD_SHARE_CONNECTION)
    endif()
    if(DNSSD_INCLUDE_DIR)
        target_include_directories(changji_core PRIVATE ${DNSSD_INCLUDE_DIR})
    endif()
    if(CHANGJI_DNSSD_LIB)
        target_link_libraries(changji_core PRIVATE ${CHANGJI_DNSSD_LIB})
    endif()
endif()

target_include_directories(changji_core PUBLIC
    src
    "${CHANGJI_GENERATED_DIR}"    # stages/prompts.inc.hpp，见上面「提示词」那一段
    "${ASIO_INCLUDE_DIR}"
)
add_dependencies(changji_core changji_prompts)
# 我们那份 httplib 排在最前面，见上面 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES 那段。
target_include_directories(changji_core BEFORE PUBLIC "${httplib_SOURCE_DIR}")

target_link_libraries(changji_core PUBLIC
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    httplib::httplib
    Crow::Crow
    # 上面那段拉的三个静态库。httplib 的 CMakeLists 不再去构建机上找它们了，
    # 开关（CPPHTTPLIB_*_SUPPORT）也就得由我们自己给，见下面。
    changji_z
    brotlienc
    brotlidec
    brotlicommon
)

target_compile_definitions(changji_core PUBLIC
    ASIO_STANDALONE
    CPPHTTPLIB_ZLIB_SUPPORT
    CPPHTTPLIB_BROTLI_SUPPORT
    CHANGJI_VERSION="${CHANGJI_VERSION}"
)

if(CHANGJI_SSL)
    target_link_libraries(changji_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(changji_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
    if(APPLE)
        # **静态 OpenSSL 自己不带根证书。**
        # 系统上那份（/etc/ssl/cert.pem）在 macOS 上是给命令行工具用的，
        # 用户装的企业根证书在钥匙串里。httplib 有现成的那一支：打开这个宏，
        # 它走 Security.framework 把钥匙串里的根证书灌进 X509_STORE。
        target_compile_definitions(changji_core PUBLIC
                                   CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
        target_link_libraries(changji_core PUBLIC
            "-framework CoreFoundation" "-framework Security")
    elseif(WIN32)
        # Windows 那一支 httplib 用 CertOpenSystemStoreW 读系统证书库，
        # 它自己 `#pragma comment(lib, "crypt32")`——那条 pragma 只有 MSVC
        # 认，clang-cl 也认，MinGW 不认，所以这里显式再链一次。
        target_link_libraries(changji_core PUBLIC crypt32)
    endif()
endif()

# hardware_metal.mm 要的两个框架。**不能指望 ggml 把它们带进来**：
# CHANGJI_SD 和 CHANGJI_LLAMA 都关掉时（单元测试那一档就是）根本没有 ggml，
# 而 hardware.cpp 照样要问 Metal。
if(APPLE)
    target_link_libraries(changji_core PUBLIC "-framework Metal" "-framework Foundation")
endif()

if(CHANGJI_LLAMA)
    # mtmd 而不只是 llama：Qwen3-TTS 的实现在 mtmd 里。
    # mtmd 的 PUBLIC 链接里已经带上了 llama 和 ggml。
    # mtmd 带来 llama 和 ggml；llama-common 是为了 json_schema_to_grammar。
    target_link_libraries(changji_core PUBLIC mtmd llama-common)
    target_compile_definitions(changji_core PUBLIC CHANGJI_HAVE_LLAMA=1)
    # ggml 的头。开了 CHANGJI_LLAMA 时它由 llama.cpp 提供，
    # 而 doctor 的 ABI 探针要 include "ggml.h"。
    target_include_directories(changji_core PUBLIC "${llama_SOURCE_DIR}/ggml/include")
    # mtmd.h 在 tools/mtmd 下。mtmd 目标自己 PUBLIC 了这个目录，
    # 这一行是为了万一以后改成别的链接方式时不至于悄悄编不到。
    target_include_directories(changji_core PUBLIC "${llama_SOURCE_DIR}/tools/mtmd")
    # common.h 里那个 common_cpu_get_num_math()：配音要按真实核数定线程，
    # 见 llama_tts.cpp 上那段。库（llama-common）本来就链着，这里只是把
    # 头目录补上，不新增依赖。
    target_include_directories(changji_core PUBLIC "${llama_SOURCE_DIR}/common")
endif()

if(CHANGJI_SD)
    target_link_libraries(changji_core PUBLIC stable-diffusion)
    target_compile_definitions(changji_core PUBLIC CHANGJI_HAVE_SD=1)
    # 没开 CHANGJI_LLAMA 时 ggml 是 sd.cpp 自带的那份，头在它的子模块里。
    if(NOT CHANGJI_LLAMA)
        target_include_directories(changji_core PUBLIC
            "${stable_diffusion_cpp_SOURCE_DIR}/ggml/include")
    endif()
    # stb 那两个头在 sd.cpp 的 thirdparty 里，它自己没 export 这个目录。
    # 用它们是为了在**内存里**编解码 PNG——stb 自带的 fopen 版本在
    # Windows 上把路径按 ANSI 代码页解释，中文项目名直接写不进去。
    # ⚠️ **不要把 sd.cpp 的 thirdparty 目录摊在包含路径上。**
    #
    # 那里面有一份 httplib.h（0.28.0）和一份 json.hpp，会盖住我们钉的版本。
    # 而且盖得**必然**：httplib::httplib 的头文件是以 SYSTEM 身份进来的
    # （MSVC 上是 -external:I），SYSTEM 目录永远排在普通 -I 之后。
    # 于是 llm/client_http.cpp 和 doctor.cpp 一直编的是 0.28.0，不是 0.15.3——
    # 两边碰巧 API 兼容所以从来没报过错，直到 comfy 这边用到 multipart
    # （0.28.0 把 MultipartFormDataItems 改了名）才炸出来。
    #
    # 现在只把 sd.cpp 的源码根目录留在路径上，stb 走
    # `#include <thirdparty/stb_image.h>`。多打几个字，换的是
    # "谁也不会再意外捡到别人的 httplib"。
endif()

if(WIN32)
    target_compile_definitions(changji_core PUBLIC
        _WIN32_WINNT=0x0A00   # Windows 10。asio 不给这个宏会警告并退化行为
        NOMINMAX              # windows.h 的 min/max 宏会和 std::min/max 打架
        WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE      # 路径要走宽字符，中文目录名才不会乱码
    )
    # shell32：CommandLineToArgvW（paths::utf8_args 用它拿宽字符参数）
    target_link_libraries(changji_core PUBLIC ws2_32 mswsock shell32)
    if(MSVC)
        # /utf-8       源码是 UTF-8，中文注释和中文字符串字面量都靠它
        # /Zc:__cplusplus
        #     不加这个的话 MSVC 的 __cplusplus 永远是 199711L，不管实际用的
        #     是哪个标准。后果有两层：一是自己的代码（doctor 报的 C++ 版本）
        #     显示成 "C++97"；二是任何拿 __cplusplus 做条件编译的第三方头文件
        #     都会在 MSVC 上静默走错分支——llama.cpp 的 LU8 宏就是这么炸的，
        #     见 verify/RESULTS.md。
        target_compile_options(changji_core PRIVATE /utf-8 /Zc:__cplusplus /W4)
    endif()
else()
    target_compile_options(changji_core PRIVATE -Wall -Wextra -Wpedantic)
    # **clang 家族要把 constexpr 求值的步数上限抬高。**
    #
    # `bundled_webapp.inc.hpp` 是一个 constexpr 数组，几十万字符的字面量，
    # 每个 `string_view` 的长度都要在编译期一个字符一个字符数出来。clang
    # 默认上限（-fconstexpr-steps，约 100 万步）对 icpx（Intel 那个 clang）
    # 不够，报出来的是
    #     error: constexpr variable 'kBundledWebappChunks' must be
    #            initialized by a constant expression
    #     note: in call to 'length(&"…")'
    # ——看起来像是那个生成的头文件坏了，其实只是数不完。前端一大，
    # 别的 clang 迟早也会撞上，所以这里按编译器给，不按平台。
    # clang-cl 走的是 MSVC 那套参数（windows-arm64 那格用它），不能塞
    # GNU 风格的 -f 选项进去，所以把它排除掉。
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|IntelLLVM"
       AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(changji_core PRIVATE -fconstexpr-steps=50000000)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(changji_core PUBLIC Threads::Threads)
    # NVML 是运行时 dlopen 的，链接期不依赖它——但 dlopen 本身在老一点的
    # glibc 上要链 libdl（2.34 之后并进了 libc）。CMAKE_DL_LIBS 在不需要
    # 的平台上是空的。见 models/hardware.cpp 里的 Nvml。
    target_link_libraries(changji_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# 静态链接 GCC 运行时。默认打开，因为"单一二进制、零运行时依赖"
# 是这个后端存在的理由之一：
#
#   Windows: 不静态链接的话，exe 要 libstdc++-6.dll、libgcc_s_seh-1.dll、
#            libwinpthread-1.dll 三个 MinGW 的 DLL 才能起来，
#            拷到没装 MinGW 的机器上直接闪退。
#   树莓派:  省掉目标机 GCC 版本不匹配导致的 "GLIBCXX_3.4.xx not found"。
#
# MSVC 走 /MT 是另一套机制，这些 flag 对它无效，所以只对 GNU/Clang 生效。
option(CHANGJI_STATIC_RUNTIME "静态链接 libstdc++/libgcc" ON)

# MSVC 走 /MT（静态 CRT），机制和 GCC 那套完全不同，所以单独一段。
#
# 不做的话 exe 依赖 MSVCP140.dll、VCRUNTIME140.dll 那一串，
# 拷到没装 VC++ 运行库的机器上直接弹"缺少 DLL"。而"零运行时依赖"
# 是这个后端的立项理由之一。
#
# **必须在 FetchContent 之前设**：这个变量要传染给所有子项目，
# 一半 /MT 一半 /MD 链接时会报 RuntimeLibrary 不匹配。
# 这里放在文件末尾只是为了和上面那段放一起读——CMake 的
# CMAKE_MSVC_RUNTIME_LIBRARY 是在生成阶段读的，位置不影响。
#
# **自己拉源码编的那几个静态库要跟着一起改。** /MT 的 changji 链 /MD 的
# 静态库，MSVC 报的是 LNK2038 RuntimeLibrary 不匹配，而那行报错完全看不出
# 是"哪个库编错了运行时"。brotli 那个命令行工具（我们用不到，但
# `cmake --build build` 不带 --target 时会编）也一并改，否则轮到它链接时挂。
# CI 里 openssl 那个 job 编的静态库本来就是 /MT（OpenSSL 的 no-shared
# 默认就走静态 CRT），和这里对得上。
if(CHANGJI_STATIC_RUNTIME AND MSVC)
    foreach(_t changji changji_core changji_z brotlicommon brotlienc brotlidec brotli)
        if(TARGET ${_t})
            set_property(TARGET ${_t} PROPERTY
                         MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
endif()

if(CHANGJI_STATIC_RUNTIME AND NOT MSVC)
    if(WIN32)
        # MinGW 上必须用 -static 而不是 -static-libgcc -static-libstdc++。
        # 后者漏掉 libwinpthread-1.dll——std::mutex 和 std::thread 走的就是它，
        # 结果是 exe 在开发机上能跑，拷到别的机器上闪退且没有任何提示。
        target_link_options(changji PRIVATE -static)
    elseif(APPLE)
        # **macOS 上这一项无声地跳过。**
        #
        # Apple 的工具链不提供静态 libc++：`-static-libstdc++` 只是
        # "argument unused"（clang 收下但什么都不做），而 `-static-libgcc`
        # 直接报 unsupported option——**且是在链接那一步才报**，
        # 前面 97% 的编译全白跑（2026-09-11 在 iMac 上实测撞到）。
        #
        # 这里本来也不需要：libc++ 和 libSystem 是系统自带的，
        # 任何一台 macOS 上都在，没有 Linux 那个"编译机和运行机
        # libstdc++ 版本不一样"的问题。
        #
        # 做成静默跳过而不是报错，是为了让六个平台共用同一行构建命令——
        # 报错的话 CI 矩阵里 macOS 那两格就得单独写一份参数。
        message(STATUS "macOS：CHANGJI_STATIC_RUNTIME 跳过（系统自带 libc++）")
    elseif(CHANGJI_SD_SYCL OR CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
        # **SYCL 那一档也跳过。** icpx 的 -fsycl 和 -static-libstdc++ 互斥，
        # 它直接拒：
        #     icpx: error: invalid argument '-static-libstdc++'
        #                  not allowed with '-fsycl'
        # 而且**是在链接那一步才报**，前面一整趟编译全白跑（2026-09-14 在
        # CI 的 linux-x64-sycl 那格实测撞到）。
        # 这一档本来也不指望"零运行时依赖"：SYCL 的包要求机器上装着 oneAPI
        # 运行时（MKL 那些按 G 算，塞不进一个 Release 资产），libstdc++
        # 跟着系统走不多一条依赖。
        message(STATUS "SYCL：CHANGJI_STATIC_RUNTIME 跳过（-fsycl 不让静态链 libstdc++）")
    else()
        # Linux 上不做全静态：glibc 静态链接会在 getaddrinfo 这类
        # 依赖 NSS 的调用上出问题，而我们要连 LLM 和推理服务，全是域名解析。
        target_link_options(changji PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

install(TARGETS changji RUNTIME DESTINATION bin)


# ---- 桌面端（Qt Quick）----
#
# 一个聊天窗，链的是上面那份 changji_core。见 ../desktop/CMakeLists.txt 里
# 「为什么默认不编」，和 docs/桌面端方案.md。
#
# ⚠️ **桌面端 2026-09-22 起在仓库根上**（`changji/desktop/`），不在 `cpp/` 底下
# ——核心和壳分开。而 CMake 的源码根仍然是 `cpp/`（CI 和文档里都写着
# `cmake -S cpp`），所以这儿要 `add_subdirectory` 一个**在源码树外面**的目录：
# 那种调用**必须显式给第二个参数**（产物目录），不给当场配置失败。
# 名字照旧叫 `desktop`，构建目录里还是 `<build>/desktop/`——三份打包脚本
# 按那个路径找产物（`package_macos.sh` 里的 `$BUILD/desktop/…`）。
option(CHANGJI_DESKTOP "编桌面端（要 Qt 6.5+）" OFF)
if(CHANGJI_DESKTOP)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../desktop" desktop)
endif()


# ---- 测试 ----
#
# doctest 而不是 Catch2：单头文件、编译快。树莓派上本机编译时 Catch2 的
# 编译开销受不了（Pi 5 编 Crow 那几个头文件已经很吃力了）。
option(CHANGJI_BUILD_TESTS "编单元测试" ON)

if(CHANGJI_BUILD_TESTS)
    enable_testing()

    # 只取源码，不跑它的 CMakeLists。
    #
    # doctest v2.4.11 的 CMakeLists 头一行是 cmake_minimum_required(VERSION 3.0)，
    # 而 CMake 4 把 "低于 3.5" 从警告改成了硬错误，配置直接失败。
    # 但 doctest 是单头文件，压根不需要它的构建脚本——SOURCE_SUBDIR 指向一个
    # 不存在的目录，FetchContent 就只下载不 add_subdirectory。
    #
    # 比 CMAKE_POLICY_VERSION_MINIMUM 那种全局降级干净：那个会把所有依赖的
    # 策略一起放宽，掩盖别的问题。
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW ON
        SOURCE_SUBDIR does-not-exist)
    FetchContent_MakeAvailable(doctest)

    add_executable(changji_tests
        tests/unit/main.cpp
        tests/unit/test_agent.cpp
        tests/unit/test_rail.cpp
        tests/unit/test_enums.cpp
        tests/unit/test_half_written.cpp
        tests/unit/test_qml_textformat.cpp
        tests/unit/test_qml_names.cpp
        tests/unit/test_done_words.cpp
        tests/unit/test_lap_time.cpp
        tests/unit/test_qml_anim.cpp
    tests/unit/test_qml_icons.cpp
    tests/unit/test_qml_round.cpp
    tests/unit/test_qml_float.cpp
    tests/unit/test_qml_effort.cpp
    tests/unit/test_thinking.cpp
    tests/unit/test_workflow_paths.cpp
    tests/unit/test_say.cpp
    tests/unit/test_i18n.cpp
    tests/unit/test_qml_dead.cpp
    tests/unit/test_lan_peers.cpp
        tests/unit/test_when_words.cpp
        tests/unit/test_tool_slot.cpp
        tests/unit/test_task_slot.cpp
        tests/unit/test_chat_id.cpp
        tests/unit/test_window_place.cpp
        tests/unit/test_paper_merge.cpp
        tests/unit/test_worker_proto.cpp
        tests/unit/test_worker_roster.cpp
        tests/unit/test_exec_queue.cpp
        tests/unit/test_capability.cpp
        tests/unit/test_peer_auth.cpp
        tests/unit/test_blob.cpp
        tests/unit/test_node_pick.cpp
        tests/unit/test_node_table.cpp
        tests/unit/test_node_prefs.cpp
        tests/unit/test_assets_shape.cpp
        tests/unit/test_fs_dirs.cpp
        tests/unit/test_models_pick.cpp
        tests/unit/test_llm_command.cpp
        tests/unit/test_llm_call_log.cpp
        tests/unit/test_flow.cpp
        tests/unit/test_web_story.cpp
        tests/unit/test_webapp.cpp
        tests/unit/test_shot.cpp
        tests/unit/test_character.cpp
        tests/unit/test_project.cpp
        tests/unit/test_hardware.cpp
        tests/unit/test_readonly.cpp
        tests/unit/test_editing.cpp
        tests/unit/test_editing_assets.cpp
        tests/unit/test_editing_batch.cpp
        tests/unit/test_reset.cpp
        tests/unit/test_media.cpp
        tests/unit/test_upload.cpp
        tests/unit/test_ref_gen.cpp
        tests/unit/test_jobs.cpp
        tests/unit/test_shot_flow.cpp
        tests/unit/test_models_config.cpp
        tests/unit/test_lora_names.cpp
        tests/unit/test_bible.cpp
        tests/unit/test_storyboard.cpp
        tests/unit/test_scheduler.cpp
        tests/unit/test_script.cpp
        tests/unit/test_llm_client.cpp
        tests/unit/test_sse.cpp
        tests/unit/test_scripting.cpp
        tests/unit/test_planning.cpp
        tests/unit/test_batch.cpp
        tests/unit/test_oneclick.cpp
        tests/unit/test_episodes.cpp
        tests/unit/test_llm_info.cpp
        tests/unit/test_writeback.cpp
        tests/unit/test_config_api.cpp
        tests/unit/test_projects.cpp
        tests/unit/test_prompt_compose.cpp
        tests/unit/test_sd_image.cpp
        tests/unit/test_frames.cpp
        tests/unit/test_render.cpp
        tests/unit/test_sd_video.cpp
        tests/unit/test_episode.cpp
        tests/unit/test_preview.cpp
        tests/unit/test_run.cpp
        tests/unit/test_ws_client.cpp
        tests/unit/test_job_scope.cpp
        tests/unit/test_think_split.cpp
        tests/unit/test_ws_hub.cpp
        tests/unit/test_util_time.cpp
        tests/unit/test_proc.cpp
        tests/unit/test_paths.cpp
        tests/unit/test_facades.cpp
        tests/unit/test_voices.cpp
        tests/unit/test_ffmpeg.cpp
        tests/unit/test_subtitles.cpp
        tests/unit/test_gates.cpp
        tests/unit/test_assemble.cpp
        tests/unit/test_film_join.cpp
        tests/unit/test_finish.cpp
        tests/unit/test_watermark.cpp
        tests/unit/test_product_tags.cpp
        tests/unit/test_scenes.cpp
        tests/unit/test_upscale.cpp
        tests/unit/test_audio_plan.cpp
        tests/unit/test_audio.cpp
        tests/unit/test_tts_backends.cpp
        tests/unit/test_setup.cpp
        tests/unit/test_story.cpp
        tests/unit/test_story_outline.cpp
        tests/unit/test_story_reverse.cpp
        tests/unit/test_story_revise.cpp
        tests/unit/test_json_stream.cpp
        tests/unit/test_json_partial.cpp
        tests/unit/test_activity.cpp
        tests/unit/test_scheduler_queue.cpp
        tests/unit/test_offload.cpp
        tests/unit/test_repetition.cpp
        tests/unit/test_ref_images.cpp
        tests/unit/test_sysstat.cpp

        # 被测的源码。刻意不包含 doctor.cpp、http/server.cpp、
        # llm/client_http.cpp、comfy/client_http.cpp、http/run_deps.cpp
        # 和 http/tts_api.cpp ——它们链 httplib、asio 或 Crow，而各自的
        # 逻辑部分都已经拆到不依赖网络库的文件里了。
        # 体检报告那段序列化就是这么拆出来的（同 infer/node_json.cpp）。
        src/doctor/doctor_json.cpp
        src/lan/peers.cpp
        # ⚠️ **`sense.cpp` 不进测试目标。** 它要 `llm::default_http_get()`，
        # 而那一层只在主程序里（`client_http.cpp` 才 include httplib，
        # 测试目标不链它——见 client.hpp 上那句）。用例钉的是 `peers.hpp`
        # 那一半：谁在表上、谁被允许了什么、票认不认。网络那一半靠真开两个
        # 引擎验（见那几个 commit）。
        src/infer/worker_proto.cpp
        src/infer/worker_farm.cpp
        # 执行位：frames / sd_video / ref_gen 三个都用它，而它们都在这个目标里
        src/infer/local_exec.cpp
        src/infer/capability.cpp
        src/infer/peer_auth.cpp
        src/infer/blob.cpp
        src/infer/node_pick.cpp
        # **这两个是 test_node_table / test_node_prefs 要的。**
        # 缺了它们 changji_tests 链不起来（undefined reference 到
        # `nodes_json` / `load_node_prefs` / `save_node_prefs` /
        # `node_prefs_path`）——三个 test_node_*.cpp 都在上面的列表里编，
        # 实现却只带了 node_pick.cpp 一个。
        # node_registry.cpp **不能加进来**：它 include 了 httplib，
        # 而这一列上面那句话是「一个网络库都不链」。表怎么画那一半
        # 已经拆到 node_json.cpp 去了，测试要的就是它。
        src/infer/node_json.cpp
        src/infer/node_prefs.cpp
        src/agent/transcript.cpp
        src/agent/tools.cpp
        src/agent/loop.cpp
        src/http/chat_api.cpp
        src/http/rail.cpp
        src/http/enums.cpp
        src/http/flow.cpp
        src/http/film.cpp
        src/pipeline/film_join.cpp
        src/http/webapp.cpp
        src/models/shot.cpp
        src/models/character.cpp
        src/models/project.cpp
        src/models/story.cpp
        src/models/hardware.cpp
        ${CHANGJI_METAL_SOURCES}
        src/util/paths.cpp
    src/util/say.cpp
        src/util/proc.cpp
        src/util/cmdline.cpp
        src/util/sysstat.cpp
        src/net/ws_client.cpp
        src/util/text.cpp
        src/util/human_time.cpp
        src/util/fs_time.cpp
        src/stages/json_extract.cpp
        src/stages/bible.cpp
        src/stages/storyboard.cpp
        src/stages/script.cpp
        src/stages/story_plan.cpp
    src/stages/story_reverse.cpp
    src/stages/json_stream.cpp
    src/stages/json_partial.cpp
    src/stages/repetition.cpp
    src/stages/story_revise.cpp
        src/stages/story_outline.cpp
        src/stages/script_story.cpp
        src/stages/story_import.cpp
        src/stages/story_analyze.cpp
        src/stages/story_understand.cpp
        src/stages/web_tools.cpp
        src/stages/story_from_web.cpp
        src/stages/chapter_write.cpp
        src/stages/ref_images.cpp
        src/stages/prompt_compose.cpp
        src/stages/frames.cpp
        src/stages/render.cpp
        src/stages/audio_plan.cpp
        src/stages/audio.cpp
        src/stages/music.cpp
        src/stages/tts_backends.cpp
        # 测试目标编的是没开 CHANGJI_LLAMA 的那份存根（返回"没编进来"）。
        # 少了它 tts_backends.cpp 里的 local_tts_backend 会链不上。
        # ws.cpp 进得来：它只 include <crow/websocket.h>（头文件库），
        # 不碰 asio 的运行时，也不链 Crow 的 app。Hub 里那个
        # connection* 纯粹当键用，从不解引用——所以拿假指针就能测。
        src/http/ws.cpp
        src/http/offload.cpp
        src/http/job_stream.cpp
        # 体检的"当前配置用得上哪些依赖"是纯判断，不碰网络，所以进得来。
        # doctor.cpp 本身仍然不进——它链 httplib。
        # 这两个都是可选依赖的门面，测试目标里编的是没链上游时那一支
        # （CHANGJI_HAVE_SD / CHANGJI_HAVE_LLAMA 只给 changji 目标定义）。
        # test_facades.cpp 验的就是那一支的行为。
        src/infer/ggml_abi.cpp
        src/infer/llama_tts.cpp
        src/infer/llama_chat.cpp
        src/infer/scheduler.cpp
        src/infer/sd_backend.cpp
        src/infer/lora_names.cpp
        src/infer/sd_image.cpp
        src/infer/sd_video.cpp
        src/infer/sd_upscale.cpp
        src/pipeline/jobs.cpp
        src/pipeline/activity.cpp
        src/pipeline/task_board.cpp
        src/pipeline/episode.cpp
        src/pipeline/preview.cpp
        src/pipeline/storyboard_run.cpp
        src/media/ffmpeg.cpp
        src/media/subtitles.cpp
        src/gates/checks.cpp
        src/media/assemble.cpp
        src/media/product_tags.cpp
        src/media/watermark.cpp
        src/llm/client.cpp
        src/llm/thinking.cpp
        src/llm/sse.cpp
        src/llm/client_command.cpp
        src/llm/local_client.cpp
        src/llm/call_log.cpp
        src/config/settings.cpp
        src/config/model_patch.cpp
        src/config/writeback.cpp
        src/config/runtime.cpp
        src/http/readonly.cpp
        src/http/editing.cpp
        src/http/editing_assets.cpp
        src/http/editing_batch.cpp
        src/http/media.cpp
        src/http/upload.cpp
    src/http/ref_gen.cpp
        src/http/prompt_peek.cpp
        src/http/scripting.cpp
        src/http/story_api.cpp
        src/http/oneclick.cpp
        src/http/planning.cpp
        src/http/batch.cpp
        src/http/episodes.cpp
        src/http/projects.cpp
        src/http/llm_info.cpp
        src/http/config_api.cpp
        src/http/reset.cpp
        src/http/run.cpp
        src/http/voices.cpp
        # 首次运行那一页。**进得来**：清单是常量表，下载器只拉外部进程
        # （走 util/proc），接口层碰的是 config 和 catalog——三个文件
        # 一个网络库都不链。
        src/setup/catalog.cpp
        src/setup/autostart.cpp
        src/setup/update_check.cpp
        src/setup/source.cpp
        src/setup/downloader.cpp
        src/http/setup_api.cpp
    )
    # Crow 的头目录：ws.cpp 要 <crow/websocket.h>。Crow 是纯头文件库，
    # 加个包含路径就够，不用链它的 app（测试里只碰 Hub 那个纯数据结构）。
    target_include_directories(changji_tests PRIVATE src "${CHANGJI_GENERATED_DIR}"
                               "${ASIO_INCLUDE_DIR}" "${crow_SOURCE_DIR}/include")
    add_dependencies(changji_tests changji_prompts)
    target_link_libraries(changji_tests PRIVATE
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus)
    # jobs.cpp 起线程。MinGW/Linux 上不显式链 pthread 会在运行期崩，
    # 而不是链接期报错——症状是 std::thread 构造时抛 system_error。
    if(NOT MSVC)
        find_package(Threads REQUIRED)
        target_link_libraries(changji_tests PRIVATE Threads::Threads)
        target_link_libraries(changji_tests PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(WIN32)
        target_link_libraries(changji_tests PRIVATE shell32)
    endif()
    if(APPLE)
        # 理由同 changji 那边：测试目标也编 hardware.cpp。
        target_link_libraries(changji_tests PRIVATE
                              "-framework Metal" "-framework Foundation")
    endif()
    target_include_directories(changji_tests PRIVATE "${doctest_SOURCE_DIR}")

    # 一个只会把参数打回来的小程序，给 proc::run 的引用规则当靶子。
    # **必须是真的 .exe**：Windows 跑 .bat 一定经过 cmd.exe，
    # 那测出来的是 cmd 的分隔规则，不是 CreateProcessW 的。
    add_executable(changji_argecho tests/tools/argecho.cpp)
    target_compile_definitions(changji_tests PRIVATE
        CHANGJI_ARGECHO="$<TARGET_FILE:changji_argecho>")
    add_dependencies(changji_tests changji_argecho)
    target_compile_features(changji_tests PRIVATE cxx_std_20)

    # 金语料的位置。这些 JSON 当初由 Python 侧真实的函数导出，
    # Python 引擎删掉之后它们就是冻住的答案——单元测试直接读文件。
    target_compile_definitions(changji_tests PRIVATE
        CHANGJI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
    # 那条「枚举值一个都不许漏」的守卫要读 models/shot.hpp 的源码。
    target_compile_definitions(changji_tests PRIVATE
        CHANGJI_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
    # 那条「图标条那几格，桌面端一格都不许漏」的守卫要读 QML 的源码。
    target_compile_definitions(changji_tests PRIVATE
        CHANGJI_DESKTOP_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../desktop/qml")

    if(MSVC)
        target_compile_options(changji_tests PRIVATE /utf-8 /Zc:__cplusplus /W4)
    else()
        target_compile_options(changji_tests PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME unit COMMAND changji_tests)
endif()

# ---- 前端嵌入头：显式声明依赖 ----
#
# **改了这个头，必须让 webapp.cpp 重编。** 它是 gen_webapp.py 生成的，
# 里面是整个前端的内容；不重编的话，本地编出来的二进制装的是**旧界面**，
# 而版本号、接口、C++ 全是新的——没有任何地方会报错。
#
# 靠编译器扫 #include 在这儿不管用：2026-09-11 实测，改了这个头、
# 连 touch 都试过，ninja 仍然说 "no work to do"，webapp.cpp.obj 的时间戳
# 停在二十分钟前，而 changji.exe 照样重链了一次（别的文件变了）。
# 排查起来很难：二进制是新的、页面是旧的、日志干干净净。
#
# CI 不受影响（每次都是干净构建），这条是给本地开发兜底的。
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/http/webapp.cpp
             APPEND PROPERTY OBJECT_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/src/http/bundled_webapp.inc.hpp)
