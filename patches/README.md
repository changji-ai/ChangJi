# leejet/ggml 扩展补丁集

## 这是什么

`leejet-ggml-extensions.patch` 是 [leejet/ggml](https://github.com/leejet/ggml)
相对上游 [ggml-org/ggml](https://github.com/ggml-org/ggml) 的全部私有改动。

sd.cpp 硬依赖这些改动（FP8 张量类型、int8 convrot 量化、i8 tensorwise 矩阵乘），
调用点在 `src/core/ggml_extend.hpp` 里**没有 `#ifdef` 守卫**，不是可以关掉的
可选特性。而 llama.cpp 需要 leejet fork 里还没有的新算子。两个方向都编不过，
详见 [../verify/RESULTS.md](../verify/RESULTS.md)。

结论是：**统一的那份 ggml 必须是"上游 + 这个补丁"**，由本项目维护。

## 生成方式

> ⚠️ **必须用 `sd.cpp` 分支，不是 master。** leejet/ggml 的 master 只是上游
> ggml 的镜像（HEAD 是 `30bf868`，提交信息 "ggml : bump version to 0.19.0
> (#1581)"，连 PR 号都是上游的），**里面没有任何 FP8 / convrot 代码**。
> leejet 的工作在 `sd.cpp` 分支上，也正是 stable-diffusion.cpp 子模块钉的那个。
> 按 master 生成补丁会得到一个空补丁，而且不会报错。

```bash
git clone -b sd.cpp https://github.com/leejet/ggml.git
cd ggml
git remote add upstream https://github.com/ggml-org/ggml.git
git fetch upstream
BASE=$(git merge-base HEAD upstream/master)
git diff "$BASE"..HEAD > leejet-ggml-extensions.patch
```

当前这一份：

| 项 | 值 |
|---|---|
| leejet/ggml 分支 | **`sd.cpp`**（不是 master） |
| leejet/ggml HEAD | `e20c3a14`（2026-08-27，"ggml-cuda : add native FP8 matmul with cuBLASLt"） |
| 与上游的分叉点 | `8846b79e6`（2026-08-12，"cmake : add config version support (#1582)"） |
| 规模 | 31 文件，+2564 −17，79 个 hunk |
| 生成日期 | 2026-09-07 |

**几乎纯增量**（只删 17 行）是这条路可行的根本原因。补丁集最怕改动散布在
上游会变的代码里，纯新增的东西 rebase 起来轻得多。

## 落到 llama.cpp 的 ggml 上

```bash
cd llama.cpp
git apply --reject --directory=ggml path/to/leejet-ggml-extensions.patch
python3 path/to/resolve_llamacpp_conflicts.py
find ggml -name '*.rej' -delete
```

[`resolve_llamacpp_conflicts.py`](resolve_llamacpp_conflicts.py) 解掉那 6 个冲突。
其中**两处不能照抄 leejet 的原文，上游改了数据结构**：

- `ggml-cuda.cu` 的析构：llama.cpp 是 `[i][j]` 双层循环，而 `cublaslt_handles`
  是一维（cuBLASLt 句柄不绑定流，流在 `cublasLtMatmul` 调用时传入）。
  照抄会把同一个句柄销毁 `GGML_CUDA_MAX_STREAMS` 次——double free。
  **必须放外层循环。**
- `ggml-rpc.h` 的 `static_assert(GGML_OP_COUNT == N)`：补丁加了一个算子
  `GGML_OP_QUANTIZE_I8_CONVROT`，llama.cpp 的基数是 101 不是 leejet 的 101→102
  那一对，要按本地实际值改（这里是 101→102），顺带 bump `RPC_PROTO_PATCH_VERSION`。

脚本每一步都断言锚点唯一存在，上游挪走锚点时直接抛异常而不是静默插错位置。

对 llama.cpp `5202104`（ggml 0.23.0）的实测结果：**79 个 hunk 里 73 个干净落地，
6 个需要手工处理，全部是纯新增上的上下文漂移，零语义冲突。**

| 需手工处理 | hunk | 内容 |
|---|---|---|
| `include/ggml-rpc.h` | 1 | `GGML_OP_COUNT` 的 static_assert 基数不同（leejet 加了一个算子），顺带 bump `RPC_PROTO_PATCH_VERSION` |
| `src/ggml-cuda/CMakeLists.txt` | 1 | 一段纯新增的 cuBLASLt FP8 块 |
| `src/ggml-cuda/common.cuh` | 2 | context 里加 `cublaslt_handles` 成员和 getter，`#ifdef GGML_CUDA_USE_CUBLASLT_FP8` 包着 |
| `src/ggml-cuda/ggml-cuda.cu` | 1 | 析构里加 `cublasLtDestroy`（这个文件 +699 行里只有这一个 hunk 冲突） |
| `src/ggml-vulkan/ggml-vulkan.cpp` | 1 | struct 里加三个 pipeline 成员声明 |

另有两个不算冲突但要处理的：

- **`src/ggml-metal/ggml-metal.metal` 在 llama.cpp 里不存在。** 上游已经把 Metal
  shader 拆进 `ggml-metal/kernels/`，leejet 的 fork 还是老的单文件布局。
  那 66 行 FP8 shader 要**手工搬进新布局**，不能机械 patch。
  （`ggml-metal-device.m` 那 3 行是干净落地的。）

  ### Metal：已在 Apple M1 上验证通过

  Metal 的两处改动是**耦合的，不能只取一半**：

  - `ggml-metal-device.m` 的 3 行是在 `ggml_metal_device_supports_op` 里
    **声明** Metal 支持 `GGML_TYPE_F8_E4M3` / `F8_E5M2` 转 F16/BF16（干净落地）
  - `ggml-metal.metal` 的 66 行才是 FP8 的**实现**，需要手工搬迁

  只打前者，Metal 会宣称支持 FP8 然后在运行时产出垃圾；两者都不打，
  `supports_op` 返回 false，ggml 回落到 CPU——慢但正确。所以这两半必须同时到位。

  搬迁由 [`port_metal_fp8.py`](port_metal_fp8.py) 完成。上游的 cpy/cast kernel
  住在 `kernels/quantize.metal`，脚本做三件事：

  1. 把辅助块（FP8 结构体、`fp8_e4m3_to_fp32` / `fp8_e5m2_to_fp32`、
     `cpy_cast` 重载）插到 `kernel_cpy_t_t` 的模板声明之前
  2. 把模板体里的 `dst_data[i00] = (T1) src[0];` 改成
     `dst_data[i00] = cpy_cast<T1>(src[0]);`
  3. 追加四个实例化：`kernel_cpy_f8_{e4m3,e5m2}_{f16,bf16}`

  脚本幂等（检测到已移植就跳过），每步带断言——上游把锚点挪走时会直接报错，
  不会静默搬错地方。**上游 Metal 布局再变时要重跑，可能要改锚点。**

  #### 验证结果（2026-09-07，macOS 26.6.2 / Apple M1 / Xcode 21）

  在上游 ggml 上打补丁 + 跑移植脚本，编译零错误，`test-fp8-cast` 退出码 0：

  ```
  compiling pipeline: kernel_cpy_f8_e4m3_f16   → loaded  th_max = 1024
  compiling pipeline: kernel_cpy_f8_e5m2_f16   → loaded
  compiling pipeline: kernel_cpy_f8_e4m3_bf16  → loaded
  compiling pipeline: kernel_cpy_f8_e5m2_bf16  → loaded
  ```

  与 leejet 原版（`sd.cpp` 分支 `e20c3a1`，老布局）在同一台机器上的输出一致。

  这个测试是**穷举**的：遍历两种格式全部 256 个字节值，逐个与参考实现比对
  并检查 NaN 处理，任一不符即退出码 1，成功时静默。所以退出码 0 意味着
  E4M3 和 E5M2 的整个输入域转 F16 和 BF16 都正确。

  #### Metal 能得到的和得不到的

  leejet 补丁里 Metal 部分**只有 FP8 cast**，没有 i8 convrot 和 FP8 matmul
  ——那两样只有 CUDA 和 Vulkan 实现。同一台 M1 上：

  | 测试 | 结果 |
  |---|---|
  | `test-fp8-cast` | ✅ 真的跑了 Metal，穷举通过 |
  | `test-fp8-matmul` | ⊘ 整个文件包在 `#ifdef GGML_USE_CUDA` 里，非 CUDA 直接 `return 0`——**是跳过不是通过** |
  | `test-int8-convrot` | ⊘ 只找名字含 "CUDA"/"Vulkan" 的 GPU 后端，Metal 不在列，回落 CPU |

  所以结论要说准：**这个补丁给 Mac 带来的是 FP8 权重能在 GPU 上解码，
  不是 leejet 那套量化加速。** Mac 上 sd.cpp 能跑，int8/FP8 矩阵乘走 CPU。

- **`tests/CMakeLists.txt` 在 llama.cpp 里不存在。** llama.cpp 不 vendor ggml 的
  独立 tests 目录。那三个测试（`test-fp8-cast`、`test-fp8-matmul`、
  `test-int8-convrot`，共 581 行）本身是白送的资产，**正好用来验证移植对不对**，
  给它们另找一个挂载点。

## 维护约定

1. **上游一动就要重跑。** 两个上游（llama.cpp 和 leejet/ggml）各自前进时，
   这个补丁都要重新生成并重新测落地。这是这条路的长期成本，不是一次性的。
2. **先跑那三个测试再谈别的。** FP8 转换、FP8 矩阵乘、int8 convrot 各有一个
   上游写好的测试，移植完第一件事是让它们过。
3. **补丁里的 hunk 数是健康度指标。** 冲突 hunk 从 6 涨到几十，说明上游在
   这些区域动了大手术，该重新评估这条路了。
