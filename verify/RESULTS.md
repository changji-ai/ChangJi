# 前置验证结论

对应方案 [第四节](../../docs/C++重构方案.md) 的六项验证。这份文件记的是
**实测出来的事实和它们对方案的影响**，不是操作日志。

验证环境（2026-09-07）：

| 项 | 值 |
|---|---|
| GPU | RTX 2060 6GB，compute 7.5（Turing） |
| 内存 | 32GB |
| 驱动 | 610.88 |
| MSVC | v14.44.35207（VS 2022 Build Tools 17.14.37614） |
| Windows SDK | 10.0.26100 |
| CMake | 4.4.3 |
| CUDA | 13.3 |
| llama.cpp | `5202104`（2026-09-07），ggml 0.23.0 |
| sd.cpp | `d8fb10c`（2026-09-07），master-846 |

> ⚠️ **这台不是选型结论那台。** [选型结论.md](../../docs/选型结论.md) 里的产能
> 数据是在 RTX 5080 16GB（sm_120，Blackwell）上测的。这里是 2060 6GB
> （sm_75，Turing）。编译期的结论（能不能编、能不能链、ABI 对不对）
> 与显卡型号无关，在这台上成立即成立；**Blackwell sm_120 的兼容性验不到**，
> 那一条本来就在选型结论的"待核实"清单里，仍然待核实。
>
> 好的一面：2060 的 6GB 正是"6GB 卡跑 16GB 模型"这个立项理由的目标场景，
> 第三项（`--offload-to-cpu`）在这台上验比在 5080 上更有说服力。

---

## 一、统一 ggml：编译不过（结论：风险二成立，且比方案写的更硬）

> **本节推翻了一个中间结论。** 在只读 CMakeLists 还没编译的阶段，我判断
> "上游已经预留了复用的路，风险二可以降级"。编译之后证明是错的。
> 保留这段过程，因为它说明为什么这个验证工程必须真的编一次，
> 光读构建脚本会得出相反的结论。

### 配置期：看起来很顺

sd.cpp 的 CMakeLists 确实预留了复用：

```cmake
# Only add ggml if it hasn't been added yet
if (NOT TARGET ggml)
    if (SD_USE_SYSTEM_GGML)
        find_package(ggml REQUIRED)
        add_library(ggml ALIAS ggml::ggml)
    else()
        add_subdirectory(ggml)
    endif()
endif()
```

先 `add_subdirectory(llama.cpp)` 再加 sd.cpp，它自己就跳过自带的那份，
配置期干净通过：

```
-- ggml version: 0.23.0
-- ggml commit:  5202104              ← llama.cpp 的 HEAD
-- stable-diffusion.cpp version master-846-d8fb10c
```

### 编译期：撞墙

```
ggml_extend.hpp(1062): error C3861: "ggml_mul_mat_i8_tensorwise": 找不到标识符
ggml_extend.hpp(2777): error C2065: "GGML_TYPE_F8_E4M3": 未声明的标识符
ggml_extend.hpp(2777): error C2065: "GGML_TYPE_F8_E5M2": 未声明的标识符
ggml_extend.hpp(2811): error C3861: "ggml_quantize_i8_convrot": 找不到标识符
```

这些符号的分布：

| 符号 | leejet/ggml | llama.cpp 的 ggml |
|---|---|---|
| `ggml_mul_mat_i8_tensorwise` | 2 个文件 | **0** |
| `GGML_TYPE_F8_E4M3` / `F8_E5M2` | 8 个文件 | **0** |
| `ggml_quantize_i8_convrot` | 2 个文件 | **0** |

**sd.cpp 硬依赖 [leejet/ggml](https://github.com/leejet/ggml) 这个 fork 的私有扩展**：
FP8 张量类型、int8 convrot 量化、i8 tensorwise 矩阵乘。而且调用点
**没有 `#ifdef` 守卫**——不是可以关掉的可选特性，是主代码路径。
sd.cpp 的 `docs/int8_convrot.md` 就是讲这套东西的。

所以方案风险二里"退路是 fork 其中一个"这句**要保留，而且它不是退路，
是唯一的路**。三个可能的方向，按代价从低到高：

1. **反过来，让 llama.cpp 用 leejet/ggml。** llama.cpp 有
   `option(LLAMA_USE_SYSTEM_GGML "Use system libggml" OFF)`，接口是现成的。
   问题是 leejet 的 fork 相对 llama.cpp 需要的 ggml 落后多少——llama.cpp
   对自己那份 ggml 的耦合很紧，新算子加得很频繁。**这是下一个要做的实验。**
2. **把 leejet 的扩展移植到 llama.cpp 的 ggml 上**，长期维护一个补丁集。
   代价是每次两个上游前进都要重新对齐，正是方案里写的那笔长期维护成本。
3. **放弃 sd.cpp 的 FP8 / int8 convrot 路径**，自己改 sd.cpp 把那些调用点
   换成通用实现。等于 fork sd.cpp。

无论走哪条，"两个库各自 vendor 不同 commit 的 ggml，同时链接会出问题"
这个判断是对的，只是出问题的形式不是符号冲突，而是**其中一方的私有扩展
在另一方那里根本不存在**。

### 反方向也编不过：让 llama.cpp 用 leejet/ggml

两边的 CMakeLists 都用 `if (NOT TARGET ggml)` 包着自己那份，llama.cpp 那边
还明写了 `# ... otherwise assume ggml is added by a parent CMakeLists.txt`。
所以父工程先把 leejet/ggml 加进来，两个库都会复用它——**配置期干净通过，
退出码 0**。

编译期失败，这次轮到 llama.cpp：

```
error C2039: "mmap_support": 不是 "ggml_backend_dev_caps" 的成员
error C2660: "ggml_ssm_scan": 函数不接受 9 个参数
error C3861: "ggml_flash_attn_ext_set_n_kv_max": 找不到标识符
error C3861: "ggml_rope_set_offset": 找不到标识符
error C3861: "ggml_swiglu_clamp": 找不到标识符
```

leejet 的 fork 自报 0.19.0，llama.cpp 的 ggml 是 0.23.0，四个小版本的差距是
真的。（中途我抽查过几个 API 觉得"并不落后"，那是抽样太浅——抽到的正好
是两边都有的。这里再记一次教训：ggml 这种改动频繁的库，判断版本差距要靠
编译，不要靠抽查符号。）

### 结论：当前两个上游的 HEAD 上，统一 ggml 双向不可行

| 方向 | 结果 | 原因 |
|---|---|---|
| 用 llama.cpp 的 ggml（0.23.0） | 编不过 | sd.cpp 硬依赖 leejet fork 的 FP8 类型、int8 convrot、i8 tensorwise matmul |
| 用 leejet 的 ggml（0.19.0） | 编不过 | llama.cpp 需要 0.19 之后新增的算子和结构体字段 |

四条出路，按代价排：

1. **放弃"进程内链接 llama.cpp"。** 只链 sd.cpp，文本推理走 HTTP（llama.cpp
   自带的 server 就是 OpenAI 兼容的）。这样根本没有两份 ggml，问题不存在。
   代价是"严格单一二进制"在文本推理这一环打折——而这两条正是决策台账里
   第 2 项和第 5 项定下来的。**这个验证结果直接把那两个决策推回到桌面上。**
2. **钉住旧版 llama.cpp**，找 ggml 还是 0.19.x 那个时期的版本。代价是文本
   推理的能力和性能停在那个时间点，且以后 sd.cpp 更新 fork 时要重新找。
3. **把 leejet 的三组扩展移植到新版 ggml 上**，自己维护一个补丁集。
   两个上游各自前进时都要重新对齐。这是方案风险二里写的那笔长期维护成本，
   实际比原文估计的重——不是"找一个能编的 commit"，是持续移植。
4. **fork sd.cpp**，把 FP8 / int8 convrot 那些调用点改成通用实现。
   等于放弃 leejet 在量化上的工作，而那正是低显存跑大模型的关键。

### 已定：走"移植 leejet 扩展 + 维护补丁集"（决策 17）

四条出路里选了第 3 条。选完立刻量了工作量，结论是**这条路比方案原文估计的
轻**，理由有三：

**一、分叉点很近，改动几乎纯增量。** leejet/ggml 相对上游的分叉点是
`8846b79e6`（2026-08-12），到 HEAD 是 **31 文件、+2564 −17、79 个 hunk**。
只删 17 行——补丁集最怕改动散布在上游会变的代码里，纯新增的东西 rebase 轻得多。
那个"0.19 vs 0.23"的版本号差距是 fork 里没跟着改的陈迹，实际只落后四周。

**二、73/79 个 hunk 能直接落到 llama.cpp 的 ggml 上。** 实测 `git apply --reject`
到 llama.cpp `5202104`：

| 需手工处理 | hunk | 性质 |
|---|---|---|
| `include/ggml-rpc.h` | 1 | `GGML_OP_COUNT` 断言基数不同 |
| `src/ggml-cuda/CMakeLists.txt` | 1 | 纯新增的 cuBLASLt FP8 块 |
| `src/ggml-cuda/common.cuh` | 2 | 加 `cublaslt_handles` 成员和 getter |
| `src/ggml-cuda/ggml-cuda.cu` | 1 | 析构里加 `cublasLtDestroy`（这文件 +699 行只冲突这一处） |
| `src/ggml-vulkan/ggml-vulkan.cpp` | 1 | struct 里加三个 pipeline 成员声明 |

**六个全是纯新增上的上下文漂移，零语义冲突。**

**三、上游白送了三个测试。** `test-fp8-cast`、`test-fp8-matmul`、
`test-int8-convrot` 共 581 行，正好用来验证移植对不对，不用自己造。

两个不算冲突但要手工处理的：

- **Metal shader 的布局变了。** 上游已把 Metal shader 拆进
  `ggml-metal/kernels/`，leejet 的 fork 还是单体 `ggml-metal.metal`。
  那 66 行 FP8 shader 要手工搬进新布局。（`ggml-metal-device.m` 的 3 行干净落地。）
- **`tests/CMakeLists.txt` 无处安放。** llama.cpp 不 vendor ggml 的独立 tests 目录，
  三个测试要另找挂载点。

补丁和维护约定见 [../patches/](../patches/)。**核心约定：上游一动就要重跑，
冲突 hunk 数是这条路的健康度指标**——从 6 涨到几十就说明上游在这些区域动了
大手术，该重新评估。

### 补丁集实施完成：统一 ggml 跑通了

六个冲突 hunk 全部解掉，脚本见
[../patches/resolve_llamacpp_conflicts.py](../patches/resolve_llamacpp_conflicts.py)。
**其中两处不能照抄 leejet 的原文，因为上游改了数据结构**：

- `ggml-cuda.cu` 的析构：llama.cpp 已改成 `[i][j]` 双层循环（上游加了
  per-stream 的 cuBLAS 句柄），而 `cublaslt_handles` 是一维的——cuBLASLt 句柄
  不绑定流，流是调用 `cublasLtMatmul` 时传的。照抄会把同一个句柄销毁
  `GGML_CUDA_MAX_STREAMS` 次，double free。**必须挪到外层循环。**
- `ggml-rpc.h` 的 `static_assert(GGML_OP_COUNT == N)`：补丁加了算子
  `GGML_OP_QUANTIZE_I8_CONVROT`，但 leejet 的基数（5.x 时代）和 llama.cpp 的
  （现在 101）不同，要按本地实际值改成 102，顺带 bump `RPC_PROTO_PATCH_VERSION`。

**这两处说明了为什么这是"移植"不是"打补丁"**：79 个 hunk 里 73 个确实是机械的，
但剩下的需要读懂上游为什么改了结构。补丁集的长期成本主要在这里。

#### 构建与运行结果

llama.cpp + sd.cpp 在同一份打过补丁的 ggml 上编出一个二进制：

```
ggml-base.dll  ggml-cpu.dll  ggml.dll  llama.dll  stable-diffusion.lib
```

`cjverify.exe` 全部通过：

```
[1] 编译器            MSVC _MSC_VER = 1944
[2] ggml ABI 一致性   写入 100 字符的名字，读回 100 字符 —— 库和程序都是 160
[3] ggml 后端设备     1 个（CPU，符合 CJV_CUDA=OFF 的预期）
[4] llama.cpp         llama_backend_init 正常，系统信息可读
[5] sd.cpp            sd_version = master-846-d8fb10c
```

**第 2 项是这次验证最有价值的一条。** 它不是"编过了"，是运行时探针：写一个
100 字符的张量名再读回来，库自己按 GGML_MAX_NAME 多少编的、名字就在多少字节
处被截断。读回 100 说明库和程序都是 160；如果顶层那行
`add_compile_definitions(GGML_MAX_NAME=160)` 被删掉，这里会读回 63，
而**编译和链接都不会报错**。

#### CUDA 构建：三处手工解的 CUDA 冲突已验证

CPU 构建根本不编 `ggml-cuda`，所以上面那次通过**证明不了**我手工解的三处
CUDA 冲突是对的。补了 CUDA 构建：

```
ggml-base.dll  ggml-cpu.dll  ggml-cuda.dll  ggml.dll  llama.dll
llama.cpp 报告：CUDA : ARCHS = 750 | USE_GRAPHS = 1
设备枚举：0. [GPU] CUDA0 — NVIDIA GeForce RTX 2060 / 1. [CPU]
```

`GGML_CUDA_USE_CUBLASLT_FP8` 在构建日志里出现 143 次，宏确实生效，
那三处改动真的被编译了。

**析构那一处单独验证过。** 它是这次移植最险的改动——llama.cpp 的析构是
`[设备][流]` 双层循环而 `cublaslt_handles` 是一维，照抄会 double free，
我把它挪到了外层。但**光枚举设备不会创建 `ggml_backend_cuda_context`**，
所以前面的检查跑不到析构。为此在 cjverify 里加了第 6 项：真的
`ggml_backend_dev_init` 出一个 CUDA0 后端再 `ggml_backend_free` 掉。

```
[6] GPU 后端构造与析构
  已创建：CUDA0
  [ok] 后端析构   构造再销毁一次没崩——cublasLtDestroy 的循环层次是对的
```

**结论：验证第二项从"不可行"改为"经补丁后可行"，CPU 和 CUDA 两条链都通。**
决策 17 的补丁集方案成立。

#### 一个咬了两次的环境坑

CUDA 装完后，MSBuild 的 `CUDA <版本>.targets` 查的是**版本化**的
`CUDA_PATH_V13_3`，不是 `CUDA_PATH`——两个都要在进程环境里。缺了只报

```
error : The CUDA Toolkit directory '' does not exist.
```

这句话把人往"CUDA 没装好"的方向带，实际是环境变量没传进去。

## 二、真正的坑是一个编译期常量，不是符号冲突

sd.cpp 的 CMakeLists：

```cmake
if (NOT SD_USE_SYSTEM_GGML)
    add_definitions(-DGGML_MAX_NAME=160)    # 上游 ggml 默认是 64
endif()
```

`add_definitions` 只作用于**它之后的目录作用域**，加不到已经在别处编好的
llama.cpp 的 ggml 上。结果是 ggml 按 64 编、sd.cpp 按 160 编，两边看到的
`ggml_tensor` 结构体大小不一致。

**链接照样通过，错在运行时踩内存。** 这比符号冲突难查得多，因为它不报错。

解法是在顶层 `add_compile_definitions(GGML_MAX_NAME=160)`，让所有目标用
同一个值。[CMakeLists.txt](CMakeLists.txt) 里那一行就是干这个的，删掉它
编译一样过。

[main.cpp](main.cpp) 的 `check_ggml_abi()` 是这一条的运行时探针：写一个
100 字符的张量名再读回来，**库自己按多少编的，名字就在多少字节处被截断**。
测的是库的编译期常量，不是本程序的。

## 三、上游不能被拖进 C++20（结论：changji/cpp/CMakeLists.txt 要改）

三个项目声明的 C++ 标准：

| 项目 | 声明 |
|---|---|
| llama.cpp | `target_compile_features(llama PRIVATE cxx_std_17)  # don't bump` |
| ggml | `set(CMAKE_CXX_STANDARD 17)` + 同样的 `# don't bump` |
| sd.cpp | `set(CMAKE_CXX_STANDARD 17)` |
| **changji/cpp** | **`set(CMAKE_CXX_STANDARD 20)`** |

`target_compile_features(... cxx_std_17)` 声明的是**下限不是上限**。父作用域给
`CMAKE_CXX_STANDARD 20`，"至少 17"被满足，上游整个按 C++20 编。

然后在 MSVC 上炸：

```
llama-chat.cpp(557): error C2088: 内置运算符"<<"无法应用于 std::stringstream
llama-chat.cpp(561): error C2280: operator<<(..., const char8_t*) 尝试引用已删除的函数
```

根因比表面更精确——llama.cpp 其实处理过 C++20：

```cpp
#if __cplusplus >= 202000L
    #define LU8(x) (const char*)(u8##x)
#else
    #define LU8(x) u8##x
#endif
```

但 **MSVC 不加 `/Zc:__cplusplus` 时 `__cplusplus` 永远是 `199711L`**，这个守卫
静默失效，走 `#else` 拿到 `const char8_t*`，而 C++20 删除了
`operator<<(ostream&, const char8_t*)`。**只在「MSVC + C++20」这个组合下出现，
GCC 上编不出来**——阶段 0 用 MinGW 编所以从没遇到。

**对方案的影响**：[changji/cpp/CMakeLists.txt](../CMakeLists.txt) 现在设的就是
`CMAKE_CXX_STANDARD 20`（注释写着为了 `std::format` 和 designated initializer）。
阶段 5 把 sd.cpp 和 llama.cpp 加进去时会原样撞上。改法是全局停在 17，
只用 `target_compile_features(changji PRIVATE cxx_std_20)` 把自己的目标抬上去。

给 MSVC 加 `/Zc:__cplusplus` 能修好这一个点，但上游那句 `# don't bump` 说明
他们并不保证 C++20 下的其它地方，不要走这条路。

## 三点五、验证第一项：MSVC + CUDA —— 通过

**结论：通过。** 用 MSVC + CUDA 13.3 把 sd.cpp 那条链（ggml + ggml-cuda +
stable-diffusion）完整编出来了，零错误。

| 产物 | 大小 |
|---|---|
| `stable-diffusion.lib` | 135 MB |
| `ggml-cuda.dll` | 50 MB |
| `ggml-base.dll` / `ggml-cpu.dll` / `ggml.dll` | 641K / 811K / 66K |

编译架构 `CMAKE_CUDA_ARCHITECTURES=75`（sm_75，本机 RTX 2060 是 Turing）。
单架构编译是有意的：全架构编译产物有好几个 G，验证阶段没必要。

方案第四节里"MSVC + CUDA 能编"这一项可以划掉。**阶段 1 之前切 MSVC 这个
决定没有障碍。**

### 版本兼容性


nvcc 的 host_config.h 里那道门：

```c
#if defined(_MSC_VER)
#if _MSC_VER < 1920 || _MSC_VER >= 1960
```

合法区间是 `_MSC_VER` ∈ [1920, 1959]。本机 MSVC 14.44（VS 2022 17.14）是
**1944**，落在区间内。CUDA 13.3.73 + VS 2022 17.14 这个组合**合法**，
不需要为了 nvcc 去降 MSVC 版本——这是切 MSVC 之前最担心的一件事。

一个只会咬一次但很费时间的坑：CUDA 安装器把 VS 集成
（`BuildTools/MSBuild/Microsoft/VC/v170/BuildCustomizations/CUDA 13.3.targets`）
装好了，`CUDA_PATH` 也写进了机器级环境变量，但**装之前就已经起着的 shell
拿不到它**。CMake 探测 CUDA 编译器会失败并报

```
error : The CUDA Toolkit directory '' does not exist.
```

这句话指向"CUDA 没装好"，实际是"环境变量没进程内"。装完 CUDA 要开新 shell，
或者显式把 `CUDA_PATH` 传进去。

## 三点六、Metal 移植：已在 Apple M1 上验证通过

验证机：`wangchao@192.168.20.14`，macOS 26.6.2 / Apple M1 / 16GB / Xcode 21。

补丁里 Metal 那 68 行有一半不能机械落地——上游已把 Metal shader 拆进
`ggml-metal/kernels/`，leejet 的 fork 还是单体 `ggml-metal.metal`。搬迁脚本见
[../patches/port_metal_fp8.py](../patches/port_metal_fp8.py)。

**结果：上游 ggml + 补丁 + 移植脚本，编译零错误，`test-fp8-cast` 退出码 0，
四个 FP8 cast pipeline 在 M1 上编译加载，与 leejet 原版输出一致。**

这个测试是穷举的：遍历 E4M3 / E5M2 全部 256 个字节值，逐个与参考实现比对
并检查 NaN，任一不符即退出码 1。所以它证明的是整个输入域都正确，不是"能加载"。

三个附带结论：

**一、Metal 后端不需要 Xcode 的离线 Metal 工具链。** 这台机器上
`xcrun -sdk macosx metal` 报 "missing Metal Toolchain"（新版 Xcode 把它拆成了
2–3GB 的单独下载组件），但 `GGML_METAL_EMBED_LIBRARY` 默认 ON，嵌的是
`kernels/*.metal` 源码，由 `newLibraryWithSource:` 在运行时交给 macOS 的 Metal
框架编译。**省掉了一个原以为必需的大件依赖。**

**二、Metal 只得到 FP8 cast，得不到量化加速。** leejet 补丁里 Metal 部分只实现了
cast，i8 convrot 和 FP8 matmul 只有 CUDA 和 Vulkan 版本。同机实测：
`test-fp8-matmul` 整个包在 `#ifdef GGML_USE_CUDA` 里，非 CUDA 直接 `return 0`
（**退出码 0 是跳过不是通过**）；`test-int8-convrot` 只找名字含 "CUDA"/"Vulkan"
的 GPU 后端，Metal 不在列，回落到 CPU。所以 Mac 上 sd.cpp 能跑，但 int8/FP8
矩阵乘走 CPU。

**三、补丁的基准分支是 `sd.cpp` 不是 master。** leejet/ggml 的 master 只是上游
镜像（HEAD `30bf868`，提交信息 "ggml : bump version to 0.19.0 (#1581)"，PR 号
都是上游的），**里面没有任何 FP8 / convrot 代码**。按 master 生成补丁会得到
空补丁且不报错。这一条已写进 [../patches/README.md](../patches/README.md)。

## 三点七、CUDA 运行时是动态依赖，与"零运行时依赖"冲突

编出来的 `sd-cli.exe` 直接跑会以 `0xC0000135`（STATUS_DLL_NOT_FOUND）退出，
把 `%CUDA_PATH%in` 加进 PATH 才能起来。ggml 那几个库是静态链接进去的
（构建目录里根本没有 ggml*.dll），缺的是 CUDA 自己的运行时——
`cudart64_*.dll`、`cublas64_*.dll`、`cublasLt64_*.dll` 这一批。

这跟 [cpp/README.md](../README.md) 里"单一二进制、零运行时依赖"那条论证
直接冲突。阶段 0 没发现是因为那时还没链 CUDA。三个选项：

1. **随包分发那几个 CUDA DLL。** 最省事，但 exe 旁边要跟一堆 DLL，
   "拷一个 exe 过去就能跑"不再成立，而那正是当初打开 `CHANGJI_STATIC_RUNTIME`
   的理由
2. **静态链接 CUDA 运行时**（`CUDA::cudart_static` / `cublas_static`）。
   ggml 的 `GGML_STATIC` 走的就是这条，Windows 上 12.3.1 之前没有静态
   cublas，现在的 13.3 有。体积会涨不少
3. **接受这个折扣**，在文档里把"零运行时依赖"改成"零 Python/Node 运行时依赖，
   但需要 CUDA 运行时"

这一条要在阶段 5 之前定，因为它影响 `CHANGJI_STATIC_RUNTIME` 那一整套论证。

## 三点八、验证第三、四项：offload 与 VAE 分块 —— 都通过

在 RTX 2060 6GB 上跑 Wan 2.2 TI2V-5B（Q4_K_M），640x352、9 帧、4 步。

### 第三项：`--offload-to-cpu` 不复现崩溃（通过）

方案风险四引的上游 [Issue #1483](https://github.com/leejet/stable-diffusion.cpp/issues/1483)
说这个选项会导致进程终止。**在 sd.cpp `d8fb10c` 上不复现。**

```
total params memory = 11285.94MB (VRAM 0.00MB, RAM 11285.94MB)
  text_encoders 6664.20MB(RAM)  diffusion_model 3277.49MB(RAM)  vae 1344.24MB(RAM)
Wan2.2-TI2V-5B compute buffer size: 68.64 MB(VRAM)
4/4 - 1.02s/it   sampling completed, taking 8.05s
```

**11.3GB 的权重，显存常驻 0，采样时显存峰值 68.64 MB。** 方案的第一条立项理由
——逐层换入换出让大模型在小显存上跑——在 6GB 卡上实测成立，不是纸面推论。

### 第四项：VAE 分块存在且可用，但默认参数对 Wan 无效（通过，附重要前提）

方案风险三担心的是"视频路径上是否接通未确认……如果确实缺失，需要自己实现
并回馈上游"。**实测是接通的，不用自己写。** 但默认参数救不了场，必须显式调块大小。

四次实验只改分块参数，其余完全一致：

| 配置 | VAE 解码所需显存 | 结果 |
|---|---|---|
| 不分块 | 11747.59 MB | 失败（可用 5081 MB） |
| `--vae-tiling`（默认 32x32） | 9610.67 MB | 失败 |
| 再加 `--temporal-tiling` | 10321.64 MB | 失败（反而更高） |
| `--vae-tile-size 16x11 --vae-tile-overlap 0.25` | — | **成功，44.73s** |

三条要点：

**一、默认块大小对 Wan 是白切。** 默认 32x32 的块碰上 40x22 的潜变量，
切出来是 `num tiles : 2, 1`、重叠率 **0.75**——两块几乎完全重叠，
所以只降了 18%。改成 16x11、重叠 0.25 之后是 6 块，直接过。

**二、`--temporal-tiling` 是独立开关，且在低帧数下是空操作。**
sd.cpp 把时间维分块和空间分块分成了两个参数。但 9 帧视频只压成
**3 个潜变量帧**，而分块粒度 `tile_frames=4` 比总数还大：

```
Wan VAE stateful temporal tiling: tile_frames=4, total latent frames=3, tiles=1
```

切不动，还引入了有状态分块自身的开销，显存反而从 9610 涨到 10321 MB。

**三、显存大头不是帧数，是空间和通道维的解码展开。** 潜变量 `40x22x3x48`
只有 3 个时间帧，却要 11.7GB——瓶颈在 40x22 展开成 640x352 的过程。
这一条纠正了一个容易有的直觉（"少出几帧就能省显存"）。

### 对方案的影响

- **风险三改写**：不是"分块可能缺失"，是"分块存在，但默认参数对 Wan 无效"。
  自己实现并回馈上游那条兜底不需要了。
- **阶段 5 的必备参数**：`--offload-to-cpu`、`--vae-tiling`、
  **`--vae-tile-size` 必须按潜变量尺寸算**，不能用默认值。
  块大小要随分辨率自适应——这是编排层要做的事，写死一个值在换分辨率时会失效。
- **风险四（offload 崩溃）可以关闭**，在这个上游版本上不复现。

产物 `out-tile16.mp4.avi`（326K，640x352x9 帧）。注意 sd-cli 会在 `-o` 给的
文件名后面再补一个 `.avi`。

## 三点九、阶段 0 的代码在 MSVC 下有三个 bug（已修）

切到 MSVC 之后，阶段 0 那份 MinGW 编译通过的代码暴露出三个问题。它们全都
和"窄字符串到底是什么编码"有关，而且**都不会在 MinGW 上出现**。

### 1. `fs::path(std::string)` 按 ANSI 代码页解释，不是 UTF-8

`proc::which()` 里写的是 `fs::path(dir)`，`dir` 来自 `paths::env("PATH")`——
那个函数已经正确地把宽字符转成了 UTF-8。但 MSVC 上 `fs::path(std::string)`
会把窄字符串按**当前 ANSI 代码页**（这台机器是 936/GBK）解释，UTF-8 的中文
字节序列在 GBK 里往往非法，直接抛 `std::system_error`。

只要 PATH 里有一个目录名带非 ASCII 字符，`--doctor` 整个崩掉。
项目目录本来就允许是 `E:\AI短剧\`，用户名也可能是中文，这不是边缘情况。

同一份代码在 `paths.cpp` 里还有 8 处同样的写法。全部换成新的
`paths::from_utf8()`（内部走 `std::filesystem::u8path`）。反方向的
`path.string()` 也是对称的坑，换成 `paths::to_utf8()`（走 wstring，不经过
ANSI 代码页，永不失败）。

### 2. 异常穿到 std::terminate，报成"栈缓冲区溢出"

上面那个异常没人接，一路走到 `std::terminate` → `abort` → `__fastfail`，
进程以 `0xC0000409` 消失，**一个字都不打印**。而那个错误码的字面意思是
STATUS_STACK_BUFFER_OVERRUN，排查方向被带向"哪里写越界了"，
实际跟缓冲区毫无关系。

修了两层：`main()` 加顶层 try/catch；`run_checks()` 里每一项检查单独兜异常，
单项失败降级成一条 WARN 而不是拖垮整个体检——**体检的意义就是环境不对时
告诉你哪里不对，它自己最不能因为环境不对而崩掉**。

### 3. `__cplusplus` 在 MSVC 上永远是 199711L

体检里那行"运行时"报的是 `C++97`，实际编的是 C++20。原因是 MSVC 不加
`/Zc:__cplusplus` 就不更新这个宏。

这不只是显示问题——**任何拿 `__cplusplus` 做条件编译的第三方头文件都会在
MSVC 上静默走错分支**，llama.cpp 的 `LU8` 宏就是这么炸的（见第三节）。
已给 MSVC 加上 `/Zc:__cplusplus`，doctor 现在正确报 `C++20`。

### 对方案的影响

阶段 1 的完成标志是"能读 Python 写的项目文件（含中文路径和中文内容）"。
这三个 bug 说明**中文路径的问题不在读文件那一步，在更早的地方**——
配置加载、PATH 查找、目录创建，任何一处把 UTF-8 字符串塞进 `fs::path`
都会炸。移植时的规矩：

> **项目里的 `std::string` 一律是 UTF-8。`std::string` 和 `fs::path` 之间
> 只能走 `paths::to_utf8()` / `paths::from_utf8()`，禁止 `path.string()`
> 和 `fs::path(str)`。**

## 三点十、验证第五项：Vulkan 在 Pi 5 上可用（通过）

验证机：`root@192.168.20.91`，Raspberry Pi 5 Model B Rev 1.0，
Debian 13 (trixie)，内核 6.18.39，Cortex-A76 四核，7.9GB 内存，425GB 磁盘。

方案里写的是"VideoCore VII 的 Vulkan 驱动成熟度未知"。实际情况比预期好：

```
GPU0: V3D 7.1.7.0    驱动 V3DV Mesa 26.2.0    Vulkan 1.3.354   ← 硬件
GPU1: llvmpipe       软件回退
```

ggml 的 Vulkan 后端**编译零错误**，产出 `libggml-vulkan.so.0.23.0`，
运行时也认到了设备：

```
ggml_vulkan: Found 1 Vulkan devices:
ggml_vulkan: 0 = V3D 7.1.7.0 (V3DV Mesa) | uma: 1 | fp16: 1 | bf16: 0 |
             warp size: 16 | shared memory: 32768 | int dot: 1 | matrix cores: none
Backend 1/2: Vulkan0    Device memory: 4096 MB (4096 MB free)
```

`test-backend-ops -o ADD` 全部通过。

### 三条要记的能力细节

- **`uma: 1`** —— 统一内存，显存是从系统内存里划的，不是独立的
- **`fp16: 1` 但 `bf16: 0`** —— 半精度可以，bfloat16 不行。选量化格式时要避开 bf16
- **`matrix cores: none`** —— 没有张量核心，矩阵乘走通用着色器

### ⚠️ 最重要的数字：Vulkan 只看得到 4096 MB

Pi 有 8GB 内存，但 **Vulkan 设备内存是 4096 MB**（CMA 划给 GPU 的那部分）。
这个数字直接影响第六项，见下一节。

### Debian 13 上编 ggml Vulkan 的两个依赖坑

两个都缺时 CMake 的报错都不告诉你该装哪个包：

1. `find_package(Vulkan COMPONENTS glslc)` 要的 `glslc` 在 **`glslc` 包**里，
   不在 `glslang-tools` 里（后者给的是 `glslangValidator`）。
   报错是 `Could NOT find Vulkan (missing: glslc)`——看着像 Vulkan 没装，
   实际 libvulkan 就在那儿。
2. 再往下要 `spirv-headers` 提供 `SPIRV-HeadersConfig.cmake`。

## 三点十一、验证第六项：Pi 5 跑不了 Wan 2.2（失败）

**结论：Raspberry Pi 5（8GB）跑不了 Wan 2.2 TI2V-5B，一步采样都没开始。**

### 怎么失败的

```
MESA: error: Failed to allocate device memory for BO
terminate called after throwing an instance of vk::OutOfHostMemoryError
  what():  vk::CommandBuffer::end: ErrorOutOfHostMemory
EXIT=134
```

崩在 Vulkan 驱动分配缓冲区那一步。进度：825 个张量流式加载走了 16 批
（每批 27 个，约 430 个），**采样进度行数为 0**——连第一步去噪都没到。

### 这次的条件已经拉满

失败不是因为没优化。跑之前做了：

| 手段 | 状态 |
|---|---|
| `--offload-to-cpu` | 开 |
| `--vae-tiling --vae-tile-size 16x11` | 开 |
| `--backend clip=cpu` | 开（绕开 V3D 跑不了 T5 嵌入表的问题） |
| 磁盘交换 | **+8GB**（另有 2GB zram） |
| Docker / containerd | 停 |
| 桌面会话（lightdm 全家） | 停 |
| 电源 | 换过，全程 `throttled=0x0` |

起跑时可用内存 7.4GB、交换 10GB 全空。

### 为什么 8GB 交换救不了

这一条值得单独记：**GPU 缓冲区要物理驻留的内存**（DMA 可达），
换页出去的部分对 Vulkan 驱动不可用。所以交换能延后 CPU 侧的 OOM，
**挡不住 GPU 侧的分配失败**。

崩溃前的内存实况是 7.68GB 已用 / 380MB 可用 / 3.1GB 交换已用——
CPU 侧靠交换撑住了，但 Vulkan 要的那块连续物理内存拿不出来。

### 数字对得上

`total params memory size = 11285.94MB` 与 Windows 上完全一致
（文本编码器 6664MB + 扩散模型 3277MB + VAE 1344MB）。
所以这不是 ARM 上的什么特殊行为，就是**11.3GB 的工作集塞不进 8GB 机器**。

### 顺带测到的两条

**V3D 跑不了 T5 的嵌入表。** 第一次尝试崩在

```
ggml-backend.cpp:930: pre-allocated tensor (text_encoders.t5xxl.transformer.shared.weight)
in a buffer (Vulkan0) that cannot run the operation (NONE)
```

ggml 直接 abort，不回落到 CPU。要显式 `--backend clip=cpu` 才能绕过。
**文本编码器在 Pi 上只能跑 CPU。**

**瓶颈是 GPU 不是 CPU。** 扩散阶段只有一个核 100%（Vulkan 提交线程自旋），
另三核空闲，V3D 满频 960MHz。文本编码器阶段四核都用上了
（三个工作线程各积累约 2 分钟 CPU 时间）。V3D 没有矩阵核心
（`matrix cores: none`），矩阵乘走通用着色器——加核数没用。

### 对决策 4 的影响

决策 4 选的是"树莓派跑全流程，慢也认"。**这个选项在 8GB 的 Pi 5 上
不成立**——不是慢，是跑不起来。方案里"约束顺序：能跑 > 画质 > 速度"
的第一条就没满足。

三个方向，要重新定：

1. **Pi 只做编排 + 界面，推理打到局域网里的 Windows 机器。**
   代价是 sd.cpp 不必链进同一个二进制，方案第一条立项理由
   （逐层换入换出让大模型在小显存上跑）就只对 Windows 成立。
2. **换更小的模型跑 Pi。** 文本编码器占了 11.3GB 里的 6.7GB，
   是最大的一块。换更激进的 umt5 量化或别的编码器可能压得下来，
   但那是另一条选型线，要重新验画质。
3. **等 16GB 的 Pi 5。** 官方有 16GB 版本，11.3GB 装得下。
   但 V3D 没有矩阵核心这条不变，速度仍然是问题。

## 四、Windows 长路径（工程约束，不影响方案）

llama.cpp 现在带了个 Svelte 写的 Web UI，`tools/ui/src/lib/components/app/chat/
ChatAttachments/ChatAttachmentsList/ChatAttachmentsListItem/...` 这种路径超过
Windows 的 260 字符上限。浅克隆到较深的目录时 **checkout 静默失败 3600 个文件**，
git 只在末尾提一句，`git clone` 的退出码看不出问题。

两个应对，都要：

- 克隆时带 `-c core.longpaths=true`
- 构建目录用短路径（本次用 `C:\cjv`）

这和 [cpp/README.md](../README.md) 里记的中文路径坑是同一类问题的两个面：
Windows 的路径处理会在你完全想不到的地方安静地失败。

---

## 待填

- [x] **第一项：MSVC + CUDA —— 通过**（版本兼容 + 真编一次，零错误）
- [x] 第二项：统一 ggml —— **双向都编不过**，见上
- [x] 第二项善后：出路已定（决策 17，补丁集），工作量已量化
- [x] Metal shader 搬家 —— 已在 M1 上验证通过
- [x] **移植实施完成**：6 个 hunk 全解，两库共用一份 ggml 编出二进制，cjverify 全过
- [x] 统一 ggml 的 CUDA 构建 —— 通过，析构路径单独验证过
- [ ] 第二项后半：两个库各跑一次真实推理
- [x] **第三项：`--offload-to-cpu` —— 通过**，Issue #1483 不复现
- [x] **第四项：VAE 分块 —— 通过**，但默认块大小对 Wan 无效，必须显式设 `--vae-tile-size`
- [x] **第五项：Pi 5 的 Vulkan —— 通过**，V3D + V3DV Mesa，但设备内存只有 4096 MB
- [x] **第六项：Pi 5 跑不了 Wan 2.2 —— 失败**，Vulkan 缓冲区分配不出来，一步采样都没到
