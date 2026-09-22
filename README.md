# changji 引擎

场记引擎。**一个二进制**：界面、接口、编排、推理全在进程内。
方案写在主仓库（`integemjack/changji`）的 `docs/C++重构方案.md` 里——
那份文档**不在这个仓库中**，这里只有引擎本身。

迁移期间它和一份 Python 引擎并存，靠**对拍**逐条比响应保证两边一致。
阶段 8 之后 Python 那一侧连同对拍工具一起删了——留下的安全网是
`tests/golden/` 里那批 JSON：当年由 Python 侧真实函数导出，现在冻在
版本库里，单元测试直接读文件。所以"C++ 有没有改坏"仍然测得出来，
测不出来的是"Python 现在还是不是这样"，而那个问题已经没有意义了。

## 构建

要 CMake ≥ 3.20 和一个 C++17 编译器。Linux/macOS 上装好 cmake、ninja、
git 就能编；发布用的六个平台由主仓库的 `.github/workflows/release.yml`
编——**CI 配置也不在这个仓库中**。

**开了 `CHANGJI_LLAMA` 还要一个 Python 解释器**——llama.cpp 拉下来要就地
打 leejet 的扩展补丁，配置期找不到解释器会当场停。这是构建期依赖，
跑的时候不需要。

**`CHANGJI_SSL`（默认 ON）要一份 OpenSSL ≥ 3.0，而且只认静态库。**
httplib 发 https 靠它。不认动态库是有来历的：链上构建机那份
`libssl.3.dylib` 的包，在构建机上跑得好好的，别人解开压缩包是
`dyld: Library not loaded: /opt/homebrew/opt/openssl@3/...`。

- Linux：`apt install libssl-dev`（里面带 `libssl.a`）
- macOS：`brew install openssl@3`，然后
  `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`
- 找不到静态库就**当场停**，不会悄悄退回动态链接
- 本机只想快编一版、不关心 https：`-DCHANGJI_SSL=OFF`

发布的六个平台不靠任何一台机器上装了什么：release.yml 里 `openssl` 那个
job 自己编一份钉死版本的静态库（带缓存），再 `-DOPENSSL_ROOT_DIR` 指过去。
同一段里 brotli 和 zlib 走的是 FetchContent 拉源码静态编，什么都不用装。

Windows 上是 **MSVC 2022 Build Tools + Ninja**。先进 MSVC 的环境：

```powershell
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' |
  ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] } }
```

> 少了这一步的表现是 `fatal error C1083: 无法打开包括文件: "algorithm"`。
> 那不是代码问题，是 `INCLUDE` 没设。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `CHANGJI_SD` | ON | 链 stable-diffusion.cpp，进程内出图出片 |
| `CHANGJI_SD_CUDA` | OFF | GPU 后端走 CUDA（N 卡）。要 CUDA Toolkit，而且 MSBuild 查的是**版本化**的 `CUDA_PATH_V13_3` 而不是 `CUDA_PATH`，两个都要在进程环境里 |
| `CHANGJI_SD_HIP` | OFF | GPU 后端走 HIP / ROCm（A 卡）。要 ROCm ≥ 6.1（Linux）或 AMD HIP SDK（Windows）。跑的机器上也要有 ROCm 运行时 |
| `CHANGJI_SD_SYCL` | OFF | GPU 后端走 SYCL（Intel 卡）。要 oneAPI 的 `icx`/`icpx`。**产物不是单文件**，跑的机器上要有 oneAPI 运行时 |
| `CHANGJI_SD_VULKAN` | OFF | GPU 后端走 Vulkan，**N/A/I 三家都能用**。构建要 Vulkan SDK（`glslc`），跑的时候只要驱动自带的 Vulkan 运行时。A 卡和 Intel 卡想要"下载解开就能用"的话走这条 |
| `CHANGJI_CUDA_ARCH` | `89` | 编给哪些 N 卡架构，分号隔开。发布包用的那一串在 release.yml 里 |
| `CHANGJI_CUDA_STATIC` | OFF | CUDA 运行时（cudart / cuBLAS / cuBLASLt）静态链进二进制，**产物是单个可执行文件**。发布包用的形态，CI 的 linux-x64-cuda 那格开着。⚠️ **只有 Linux 做得到**：NVIDIA 在 Windows 上不提供静态 cuBLAS，那边的包必然是 exe 加两个 DLL |
| `CHANGJI_HIP_ARCH` | `gfx1030;gfx1100;gfx1101;gfx1102` | 编给哪些 A 卡架构。**列表外的卡直接跑不了**——HIP 没有 CUDA 那种 PTX 兜底 |
| `CHANGJI_LLAMA` | OFF | 链 llama.cpp + mtmd，**进程内配音**（阶段 9）。开着会让干净构建多编一份 llama.cpp 和一份打过补丁的 ggml |
| `CHANGJI_SSL` | ON | 静态编进 OpenSSL，httplib 才发得了 https。**只认静态库**，见上面那段 |
| `CHANGJI_BUILD_TESTS` | ON | 编 `changji_tests.exe` |
| `CHANGJI_STATIC_RUNTIME` | ON | 静态链运行时。"零运行时依赖"是这个后端存在的理由之一。**macOS 上自动忽略**——Apple 的工具链没有静态 libc++ |
| `CHANGJI_VERSION` | `dev` | 写进二进制的版本号，`--version` 打印它。发布时由 CI 填 git tag |

四个 GPU 后端**一次只能开一个**，同时开两个配置期就停。挑哪个：

| 显卡 | 首选 | 备选 |
|---|---|---|
| NVIDIA | `CHANGJI_SD_CUDA` | `CHANGJI_SD_VULKAN` |
| AMD | `CHANGJI_SD_VULKAN`（拿来就能跑） | `CHANGJI_SD_HIP`（快一些，但要装 ROCm） |
| Intel | `CHANGJI_SD_VULKAN` | `CHANGJI_SD_SYCL`（要 oneAPI 运行时） |

> A 卡和 Intel 卡首选 Vulkan 不是因为它快，是因为另外两条的产物都带着
> 一大坨运行时依赖，而且 leejet 那套 ggml 扩展补丁**没有覆盖
> ggml-sycl**（覆盖了 CPU / CUDA / Metal / Vulkan，见
> [patches/README.md](patches/README.md)）——SYCL 上的 fp8 权重多半加载不了。
> 八个 GPU 包由 CI 编，配置是主仓库的 `.github/workflows/release.yml`。

开 `CHANGJI_LLAMA` 时 ggml 由 llama.cpp 提供，并且**就地打上 leejet 的扩展
补丁**（`patches/apply_to_llamacpp.py`，接在 FetchContent 的 `PATCH_COMMAND`
上，幂等）。sd.cpp 硬依赖那些扩展（FP8 类型、int8 convrot、i8 tensorwise
matmul），调用点没有 `#ifdef` 守卫。来龙去脉见
[verify/RESULTS.md](verify/RESULTS.md) 和 [patches/README.md](patches/README.md)。

两个构建目录并存是常态：`build/`（默认）和 `build_llama/`（`-DCHANGJI_LLAMA=ON`）。

## 运行

```bash
./build/changji.exe --version      # 手上这个是哪一版
./build/changji.exe --doctor       # 命令行体检，退出码非 0 表示有必须解决的项
./build/changji.exe --init-config  # 生成带注释的配置模板
./build/changji.exe --port 8080    # 起服务，前端嵌在二进制里，直接开浏览器
```

进程内配音（要 `CHANGJI_LLAMA=ON` 的二进制）：

```bash
# 先取权重：仓库根目录的 download_tts_gguf.ps1，约 1.34 GB
./build_llama/changji.exe --say "雨夜的天台上，他没有回头。"
./build_llama/changji.exe --say "试一句" --tts-model talker.gguf --tts-decoder tok.gguf
```

`--say` 是阶段 9 的实机判据：**不用起服务、不用建项目、不用 ffmpeg**，
出不出得了声一句话就知道。

配置优先级：环境变量 > 项目目录的 `changji.toml` > 用户全局配置 > 内置默认值。

常用环境变量（全部带 `CHANGJI_` 前缀）：`COMFY_BASE_URL`、`LLM_BASE_URL`、
`LLM_MODEL`、`LLM_API_KEY`、`WORKSPACE`、`FFMPEG_PATH`、`VRAM_GB`，
以及 C++ 独有的 `MODELS_DIR` / `MODELS_ENGINE` / `MODELS_VIDEO` /
`MODELS_TTS` / `MODELS_TTS_DECODER` 等。加新的要**在两个地方各写一遍**
（`env_mapping()` 那张表和 `apply_env()`），漏一处不会报错，
`test_models_config.cpp` 里有用例专门拦这个。

## 测试

一条命令把全部四样跑完（在 `changji/` 下）：

```bash
powershell -ExecutionPolicy Bypass -File verify_all.ps1
powershell -ExecutionPolicy Bypass -File verify_all.ps1 -Quick   # 跳过 llama 那一档
```

它替你做了两件容易出错的事：**进 MSVC 环境**（忘了的话报的是
`fatal error C1083: 无法打开包括文件: "algorithm"`，看着像代码问题），
以及**检查用例条数**——退出码为 0 只说明"跑到的都过了"，
有测试文件没编进 CMakeLists 的话剩下的照样全绿。

也可以分开跑：

```bash
./build/changji_tests.exe            # 单元测试
ctest --test-dir build               # 一样的东西，走 ctest
```

**语料读不到会当成失败，不是跳过。** 一条跳过的用例和一条通过的用例在总数里
长得一样，但前者什么都没保证——`CHANGJI_GOLDEN_DIR` 指偏了的时候，
那批读语料的用例会静静地什么都不验。

## 目录

```
src/
├── main.cpp        命令行入口（--doctor / --init-config / --say / 起服务）
├── util/           路径、子进程、文本、时间、东亚字宽
├── config/         配置结构、TOML 读写、环境变量覆盖、校验
├── models/         project / character / shot 与项目库读写
├── http/           Crow 路由 + 各接口（只读、编辑、上传、媒体、LLM、run）
├── llm/            OpenAI 兼容客户端
├── stages/         剧本、圣经、分镜、提示词、首帧、渲染、配音
├── infer/          sd.cpp 门面、显存调度、ggml ABI 探针、进程内配音
├── net/            WebSocket 客户端的协议部分（留给多机互联，暂无调用方）
├── setup/          首次运行那一页：模型清单、下载源、下载器
├── media/          ffmpeg、字幕、装配
├── gates/          质量闸门
├── pipeline/       job 表与整章流水线
└── doctor/         环境体检

tests/
├── unit/           单元测试，读 tests/golden/ 里的语料
└── golden/         金语料，当年由 Python 侧导出，**冻在版本库里**

prompts.toml        **所有提示词**（大模型的、出图的）。构建时生成进二进制，改提示词改这儿
tools/              codegen（提示词、前端、东亚字宽）与假大模型
patches/            leejet/ggml 扩展补丁集 + 自动应用脚本
verify/             前置验证工程（一次性，结论在 RESULTS.md）
```

## 现在到哪儿了

阶段 0–9 走完了。进程内配音真出过声，进程内大模型在 5090 上跑通过，
显存驱逐验过；**阶段 8 已经执行——Python 引擎、对拍工具、以及那 27 个
import 引擎的脚本全删了**，只剩这一个二进制加一层可选的 Node BFF。

| 还没验透 | |
|---|---|
| 真出图、真成片 | 装配那一半验过，画面是占位的。这两样恰恰是重构最难、而且现在再也没有参照物的部分 |
| macOS / arm64 | 由 CI 编出来了，但没有在真机上跑过一整章 |

契约兼容当初的标准是**字段名、嵌套结构、取值、状态码一致，key 顺序不管**；
**提示词的拼接要逐字节一致**；校验错误的**文字**不算契约。
这些标准现在只对着 `tests/golden/` 里那批冻住的语料成立。
