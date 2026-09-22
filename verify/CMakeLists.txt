# 前置验证工程：MSVC + CUDA 能不能编，sd.cpp 和 llama.cpp 能不能共用一份 ggml。
#
# 这不是产品代码，是方案第四节那六项验证里前两项的载体。结论记在
# 同目录的 RESULTS.md 里。验完可以删，但先别删——上游一动就要重跑。

cmake_minimum_required(VERSION 3.20)
project(cjverify LANGUAGES C CXX)

# 全局停在 C++17，只把自己的目标抬到 20。
#
# 这一条是踩出来的。顶层设 CMAKE_CXX_STANDARD 20 会往下传染：
# llama.cpp 和 ggml 写的是 `target_compile_features(... cxx_std_17) # don't bump`，
# 那声明的是**下限不是上限**，父作用域给 20 时"至少 17"照样满足，
# 于是上游整个按 C++20 编。
#
# 然后在 MSVC 上炸：llama-chat.cpp 里有
#
#     #if __cplusplus >= 202000L
#         #define LU8(x) (const char*)(u8##x)
#     #else
#         #define LU8(x) u8##x
#     #endif
#
# MSVC 不加 /Zc:__cplusplus 时 __cplusplus 永远是 199711L，守卫静默失效，
# 走 #else 拿到 const char8_t*，而 C++20 把 operator<<(ostream&, const char8_t*)
# 删了。GCC 上不会出现，只在 MSVC + C++20 这个组合下出现。
#
# changji/cpp/CMakeLists.txt 现在设的就是 CMAKE_CXX_STANDARD 20，
# 阶段 5 把这两个库加进去的时候会原样撞上，要按这里的写法改。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CJV_CUDA "带 CUDA 后端编" ON)

# 用哪一份 ggml。这是本工程要回答的核心问题。
#
#   llama  —— 用 llama.cpp 自带的（ggml 0.23.0，上游 ggml-org 血统）
#             已验证：编不过。sd.cpp 硬依赖 leejet fork 的私有扩展
#             （GGML_TYPE_F8_E4M3/E5M2、ggml_mul_mat_i8_tensorwise、
#             ggml_quantize_i8_convrot），且调用点没有 ifdef 守卫。
#
#   leejet —— 用 sd.cpp 那个 fork（自报 0.19.0，但抽查 API 并不落后）
#             反过来让 llama.cpp 迁就 sd.cpp。两边的 CMakeLists 都写着
#             `if (NOT TARGET ggml)`，llama.cpp 那边还明写了
#             "otherwise assume ggml is added by a parent CMakeLists.txt"，
#             所以父工程先定义 ggml 目标，两个库都会复用。
set(CJV_GGML_SOURCE "leejet" CACHE STRING "哪一份 ggml：llama 或 leejet")
set_property(CACHE CJV_GGML_SOURCE PROPERTY STRINGS llama leejet)

# ---- GGML_MAX_NAME ----
#
# sd.cpp 的 CMakeLists 里写着：
#
#     if (NOT SD_USE_SYSTEM_GGML)
#         add_definitions(-DGGML_MAX_NAME=160)   # 上游默认 64
#     endif()
#
# 那个 add_definitions 只作用于它之后的目录作用域，加不到别处编好的 ggml 上。
# 结果是 ggml 按 64 编、sd.cpp 按 160 编，两边看到的 ggml_tensor 大小不一致，
# 链接不报错，运行时踩内存。所以在顶层统一定义。
#
# 删掉这一行编译照样过，但会出很难查的运行时 bug。main.cpp 的
# check_ggml_abi() 是这一条的运行时探针。
add_compile_definitions(GGML_MAX_NAME=160)

if(CJV_CUDA)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
    # 只编本机架构。全架构编译产物有好几个 G，这台是 2060（Turing，sm_75）。
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 75 CACHE STRING "" FORCE)
    endif()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- 先定义 ggml 目标（leejet 模式） ----
if(CJV_GGML_SOURCE STREQUAL "leejet")
    set(GGML_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ggml_leejet
        GIT_REPOSITORY https://github.com/leejet/ggml.git
        GIT_TAG e20c3a1
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(ggml_leejet)
endif()

# ---- llama.cpp ----
# 只要库，不要 server / tools / examples / WebUI，也不要 OpenSSL。
foreach(opt LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES
            LLAMA_BUILD_SERVER LLAMA_BUILD_APP LLAMA_BUILD_COMMON
            LLAMA_BUILD_UI LLAMA_OPENSSL)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG 5202104
    GIT_SHALLOW ON)

# ---- sd.cpp ----
# GIT_SUBMODULES 留空：ggml 子模块不需要拉，上面已经决定用哪一份了。
# libwebp / libwebm 也不需要。
set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SD_WEBP OFF CACHE BOOL "" FORCE)
set(SD_WEBM OFF CACHE BOOL "" FORCE)
if(CJV_CUDA)
    set(SD_CUDA ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(sd
    GIT_REPOSITORY https://github.com/leejet/stable-diffusion.cpp.git
    GIT_TAG d8fb10c
    GIT_SHALLOW ON
    GIT_SUBMODULES "")

FetchContent_MakeAvailable(llama sd)

# ---- 验证程序 ----
add_executable(cjverify main.cpp)
target_link_libraries(cjverify PRIVATE llama stable-diffusion)

# 自己的代码要 C++20，但只加在自己的目标上，不传染给上游
target_compile_features(cjverify PRIVATE cxx_std_20)

# sd.cpp 的公开头文件
target_include_directories(cjverify PRIVATE "${sd_SOURCE_DIR}")

# 让程序知道构建时开没开 CUDA，否则 CPU 构建里"只有 CPU 设备"会被误报成失败
target_compile_definitions(cjverify PRIVATE CJV_CUDA_REQUESTED=$<BOOL:${CJV_CUDA}>)

if(MSVC)
    target_compile_options(cjverify PRIVATE /utf-8 /W4)
endif()

message(STATUS "cjverify: ggml 来源 = ${CJV_GGML_SOURCE}, CUDA = ${CJV_CUDA}")
