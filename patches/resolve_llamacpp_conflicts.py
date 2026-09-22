#!/usr/bin/env python3
"""解掉 leejet 扩展补丁落到 llama.cpp 的 ggml 上时的 6 个冲突 hunk。

用法（在 llama.cpp 仓库根目录）：

    git apply --reject --directory=ggml .../leejet-ggml-extensions.patch
    python3 .../resolve_llamacpp_conflicts.py
    find ggml -name '*.rej' -delete

六个冲突全是纯新增上的上下文漂移，但其中两处不能照抄 leejet 的原文，
因为上游改了数据结构——见 CUBLASLT_DTOR 和 cublaslt_handles 两段注释。

每一步都断言锚点唯一存在。上游把锚点挪走时这里会直接抛异常，
而不是静默插到错误的位置。
"""
import pathlib

ROOT = pathlib.Path(".")
changed = []


def edit(relpath, old, new, marker, count=1, tag=""):
    """marker 必须是**新增内容里独有**的字符串，不能是锚点里就有的。

    第一版把 new 的首行当幂等标记，而 new 通常是"锚点 + 新增"，首行就是
    锚点首行，于是每次都误判成已处理，一处都不改还报成功。这个断言防它。
    """
    p = ROOT / relpath
    s = p.read_text(encoding="utf-8")
    assert marker not in old, f"{relpath}: marker 出现在锚点里，要选新增内容独有的"
    if marker in s:
        print(f"  skip {tag or relpath} (already done)")
        return
    n = s.count(old)
    assert n == count, f"{relpath}: anchor found {n} times, expected {count}\n{old[:200]}"
    p.write_text(s.replace(old, new), encoding="utf-8")
    changed.append(tag or relpath)
    print(f"  [ok] {tag or relpath}")


# ── 1. ggml-rpc.h：算子数断言 ────────────────────────────────────────────
#
# 补丁给 ggml 加了一个算子 GGML_OP_QUANTIZE_I8_CONVROT，GGML_OP_COUNT 从 101
# 变成 102，ggml-rpc.h 那条 static_assert 要跟着改，否则编译失败。
#
# 不能照抄 leejet 的 hunk：他基于的上游是 RPC_PROTO_MAJOR_VERSION 5，
# llama.cpp 这边已经是 6，断言基数也不同。按 llama.cpp 的实际值改。
edit("ggml/include/ggml-rpc.h",
     "#define RPC_PROTO_PATCH_VERSION    0",
     "#define RPC_PROTO_PATCH_VERSION    1",
     marker="RPC_PROTO_PATCH_VERSION    1",
     tag="ggml-rpc.h: PATCH_VERSION 0->1")

edit("ggml/include/ggml-rpc.h",
     "static_assert(GGML_OP_COUNT == 101,",
     "static_assert(GGML_OP_COUNT == 102,",
     marker="GGML_OP_COUNT == 102",
     tag="ggml-rpc.h: GGML_OP_COUNT 101->102")


# ── 2. ggml-cuda/CMakeLists.txt：cuBLASLt FP8 开关 ──────────────────────
#
# leejet 挂在 GGML_CUDA_PEER_MAX_BATCH_SIZE 那行之前，llama.cpp 这边那个锚点
# 已经没了。改挂在 ggml_add_backend_library 之后——目标刚定义完，是加编译
# 定义和链接库最自然的位置。
CMAKE_ANCHOR = """    ggml_add_backend_library(ggml-cuda
                             ${GGML_HEADERS_CUDA}
                             ${GGML_SOURCES_CUDA}
                            )
"""
CMAKE_ADD = CMAKE_ANCHOR + """
    # leejet 的 FP8 矩阵乘走 cuBLASLt。11.8 起才有需要的接口。
    # 静态链接那一支下面已经带上了 cublasLt_static，这里只在非静态时补链接。
    if (CUDAToolkit_VERSION VERSION_GREATER_EQUAL "11.8")
        target_compile_definitions(ggml-cuda PRIVATE GGML_CUDA_USE_CUBLASLT_FP8)
        if (NOT GGML_STATIC OR WIN32)
            target_link_libraries(ggml-cuda PRIVATE CUDA::cublasLt)
        endif()
    endif()
"""
edit("ggml/src/ggml-cuda/CMakeLists.txt", CMAKE_ANCHOR, CMAKE_ADD,
     marker="GGML_CUDA_USE_CUBLASLT_FP8",
     tag="ggml-cuda/CMakeLists.txt: cuBLASLt FP8 switch")


# ── 3. common.cuh：cublasLt 句柄成员 ────────────────────────────────────
#
# 注意维度。llama.cpp 的 cublas_handles 已经是二维 [设备][流]（上游加了
# per-stream 句柄），leejet 那边是一维。cuBLASLt 句柄与流无关——流是调用
# cublasLtMatmul 时传的——所以这里保持一维 [设备]，不跟着变二维。
edit("ggml/src/ggml-cuda/common.cuh",
     "    cublasHandle_t cublas_handles[GGML_CUDA_MAX_DEVICES][GGML_CUDA_MAX_STREAMS] = {nullptr};\n",
     "    cublasHandle_t cublas_handles[GGML_CUDA_MAX_DEVICES][GGML_CUDA_MAX_STREAMS] = {nullptr};\n"
     "#ifdef GGML_CUDA_USE_CUBLASLT_FP8\n"
     "    // 一维：cuBLASLt 句柄不绑定流，流在 cublasLtMatmul 调用时传入\n"
     "    cublasLtHandle_t cublaslt_handles[GGML_CUDA_MAX_DEVICES] = {nullptr};\n"
     "#endif\n",
     marker="cublaslt_handles[GGML_CUDA_MAX_DEVICES]",
     tag="common.cuh: cublaslt_handles member")


# ── 4. common.cuh：cublasLt 句柄访问器 ──────────────────────────────────
CUBLAS_ACCESSOR_END = """        return cublas_handles[device][curr_stream_no];
    }

    // pool
"""
CUBLASLT_ACCESSOR = """        return cublas_handles[device][curr_stream_no];
    }

#ifdef GGML_CUDA_USE_CUBLASLT_FP8
    cublasLtHandle_t cublaslt_handle(int device) {
        if (cublaslt_handles[device] == nullptr) {
            ggml_cuda_set_device(device);
            CUBLAS_CHECK(cublasLtCreate(&cublaslt_handles[device]));
        }
        return cublaslt_handles[device];
    }

    cublasLtHandle_t cublaslt_handle() {
        return cublaslt_handle(device);
    }
#endif

    // pool
"""
edit("ggml/src/ggml-cuda/common.cuh", CUBLAS_ACCESSOR_END, CUBLASLT_ACCESSOR,
     marker="cublasLtHandle_t cublaslt_handle(int device)",
     tag="common.cuh: cublaslt_handle() accessors")


# ── 5. ggml-cuda.cu：析构 ───────────────────────────────────────────────
#
# 这一处必须改写，不能照抄。llama.cpp 的析构是 [i][j] 双层循环，
# leejet 的 cublaslt_handles 是一维 [i]。照抄会把同一个句柄销毁
# GGML_CUDA_MAX_STREAMS 次——double free。所以放在**外层**循环里。
CUBLASLT_DTOR_ANCHOR = """            if (cublas_workspaces[i][j] != nullptr) {
                CUDA_CHECK(cudaFree(cublas_workspaces[i][j]));
            }
        }
    }
"""
CUBLASLT_DTOR = """            if (cublas_workspaces[i][j] != nullptr) {
                CUDA_CHECK(cudaFree(cublas_workspaces[i][j]));
            }
        }
#ifdef GGML_CUDA_USE_CUBLASLT_FP8
        // 在外层：cublaslt_handles 是每设备一个，不是每流一个。
        // 放进内层会把同一个句柄销毁 GGML_CUDA_MAX_STREAMS 次。
        if (cublaslt_handles[i] != nullptr) {
            CUBLAS_CHECK(cublasLtDestroy(cublaslt_handles[i]));
        }
#endif
    }
"""
edit("ggml/src/ggml-cuda/ggml-cuda.cu", CUBLASLT_DTOR_ANCHOR, CUBLASLT_DTOR,
     marker="cublasLtDestroy",
     tag="ggml-cuda.cu: destructor (moved to outer loop)")


# ── 6. ggml-vulkan.cpp：三个 pipeline 成员 ──────────────────────────────
edit("ggml/src/ggml-vulkan/ggml-vulkan.cpp",
     "    vk_pipeline pipeline_quantize_q8_1_x4;\n",
     "    vk_pipeline pipeline_quantize_q8_1_x4;\n"
     "    vk_pipeline pipeline_quantize_i8_convrot;\n"
     "    vk_pipeline pipeline_mul_mat_i8_tensorwise;\n"
     "    vk_pipeline pipeline_mul_mat_i8_tensorwise_cm1;\n",
     # 标记要用**声明**形式。光写 pipeline_quantize_i8_convrot 会命中补丁
     # 已经落地的使用处（ggml_vk_create_pipeline 调用和 ctx->device-> 取用），
     # 于是误判成已处理，三个成员一个都不加，编译才发现。
     marker="vk_pipeline pipeline_quantize_i8_convrot;",
     tag="ggml-vulkan.cpp: three pipeline members")


print(f"\nresolved {len(changed)} conflicts")
