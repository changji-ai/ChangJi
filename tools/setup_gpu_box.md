# 一台新 GPU 机器上把 changji 跑起来

8×L20 那台上踩完一遍之后整理的。**每一条都写了为什么**——不写的话下次
换机器又会照着抄一遍错的。

## 0. 先看显存够不够

一进程一张卡，**不做跨卡切分**，所以只看单卡容量：

**算的时候别忘了计算缓冲。** 权重只是一半：生成时还要一块 buffer，
1280×704 的 VAE 解码实测 **6.6 GB**。权重把显存占满就出不来图，
而且在 sd.cpp 的日志接上之前只报一句"出图失败"。

| 阶段 | 权重 | 加缓冲 |
|---|---|---|
| 出片 | Wan 5B fp16 9.4 + umt5-xxl 11 + VAE 1.4 ≈ **22 GB** | ≈29 GB |
| 出首帧（Q8_0） | Edit Q8_0 21.8 + Qwen2.5-VL Q8_0 8 + 视觉塔 1.3 | **权重要常驻得 ≥36.0 GB** |
| 出首帧（**Q6_K**） | Edit Q6_K 16.8 + Qwen2.5-VL Q8_0 8 + 视觉塔 1.3 | **权重要常驻得 ≥30.5 GB** |

首帧这两行的门槛**不是手加出来的，是问程序要的**：`setup::recommend` 和
界面上那句「≥N GB 可常驻」用的都是 `ModelsConfig::image_weights_for`，
它自己算哪个组件进显存哪个留内存。手加权重再加一个缓冲的估法在这一族上
会偏（视觉塔和编码器多半不常驻），照着估会得出"32 GB 装不下 Q6_K"的结论，
而程序算出来是装得下的——`recommend(31.8)` 挑的就是 Q6_K。

⚠️ **首帧那一族 2026-09-15 换成了 Qwen-Image-Edit 2509**（下载目录
`setup/catalog.cpp` 和 `fetch_models.sh` 都是这一族）。它是**图像编辑**
模型：角色三视图和空景图会当参考图一起喂进去，跨镜头才是同一张脸。
两点跟着变：

- **没有 fp8 这一档**——上游只放了 GGUF 梯队，原来那条"fp8 +
  `weights=te=cpu,vae=cpu`"的路没有了。
- **必须多带一个视觉塔**（`image_text_encoder_vision`，1.3 GB）。
  2509 起不带它不报错，只在日志里说一句 `vision disabled`，然后参考图
  只有一半进得去、出来的图和参考对不上。

- **≥40 GB**（L20 48 GB / A100）：随便配，bf16 编码器也行，Q8_0 也放得下。
- **32 GB**（5090）：**`Qwen-Image-Edit-2509-Q6_K.gguf`（16.8 GB）+
  `weights = "auto"`**。再往上 Q8_0 是 21.8 GB，加上编码器就常驻不下了。
  - 文本编码器用 `Qwen2.5-VL-7B-Instruct-Q8_0.gguf`（8 GB）。
    **不要用 Comfy 的 `fp8_scaled`**——见下面第 1 节第 2 条。
  - 下面那张实测表是**基础版 Qwen-Image** 量的。Edit 2509 是同一个 20B、
    同一套量化，文件大小逐档一字不差（Q6_K 两边都是 16824990240 字节），
    所以"放得下放不下"照搬没问题；**每张多少秒没有重新量过**，参考图多
    一路编码，只会比表里慢一点。

  实测（5090，704×1280 首帧）：

  | | 权重放哪 | 利用率 | 每张 | 峰值 |
  |---|---|---|---|---|
  | fp8 + `weights=cpu` | 全在内存 | **26%** | 136 秒 | 21 GB |
  | **fp8 + `weights=te=cpu,vae=cpu`** | 扩散常驻，编码器/VAE 在内存 | **82%** | **35 秒** | **31.9 GB** |
  | Q6_K + `weights=auto` | 扩散模型常驻显存 | **89%** | 34 秒 | ≈28 GB |

  fp8 常驻能成立靠两件事，缺一不可：(1) 组件规格把 7 GB 的编码器权重
  赶回内存（sd.cpp 的 `--params-backend` 语法）；(2) **出图那条路的 VAE
  分块解码**——以前只有出片那条路填了分块参数，出图整图解码要 6.6 GB
  缓冲，19.5 GB 常驻之后挤不出来，报 `vae decode compute failed`。
  分块之后解码只要 2.9 秒、不到 1 GB。

  cpu 模式每一步都要把 19.5 GB 从内存搬过 PCIe，卡就在那儿等
  （逐秒采样是 2-3-2-3 的锯齿），所以慢 4 倍。
- **≤24 GB**：`weights = "cpu"`，权重放系统内存、用到才搬进显存。能跑，
  但每张卡大约 1 秒忙 2 秒闲（实测 35% 利用率），瓶颈在 PCIe。

## 1. 模型

放一个目录（下面按 `/data/models`），`[models].dir` 指过去。

    # 出片：Wan 2.2 TI2V-5B。注意 VAE 是 2.2 的，不是 2.1 的（只有 5B 用它）
    wan2.2_ti2v_5B_fp16.safetensors          9.4 GB
    Wan2.2_VAE.safetensors                   1.4 GB
    umt5_xxl_fp16.safetensors                 11 GB

    # 出片（更好的图生视频）：Wan 2.2 I2V-A14B，双专家。VAE 换成 2.1 那份
    Wan2.2-I2V-A14B-HighNoise-Q8_0.gguf     15.4 GB
    Wan2.2-I2V-A14B-LowNoise-Q8_0.gguf      15.4 GB
    wan_2.1_vae.safetensors                 0.25 GB

    # 出首帧：Qwen-Image-**Edit 2509**（图像编辑，收参考图）
    Qwen-Image-Edit-2509-Q8_0.gguf          21.8 GB   # ≥40 GB 卡
    Qwen-Image-Edit-2509-Q6_K.gguf          16.8 GB   # 32 GB 卡；weights=auto
    qwen_image_vae.safetensors               243 MB
    qwen_2.5_vl_7b_bf16.safetensors           16 GB   # ≥40 GB 卡用这份
    Qwen2.5-VL-7B-Instruct-Q8_0.gguf         ~8 GB   # 32 GB 卡用这份
    Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf  1.3 GB   # 视觉塔，2509 非它不可

    # 配音：Qwen3-TTS
    Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf       3.3 GB
    mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf 639 MB

**两个坑，都真栽过：**

1. **别用 `qwen_image_edit_*`。** 那是图生图**编辑**模型，上游文档每条命令都带
   `-r 参考图`。拿它做文生图出来的是**彩色雪花**，而且方差比真图还大——
   靠"方差不为零"判"不是空图"会一路绿灯。抽帧看一眼，十秒钟的事。
2. **文本编码器别用 Comfy 的 `fp8_scaled` 那份。** 那种格式带 scale 张量，
   sd.cpp 的加载器里没有一行处理 scaled，会按普通 fp8 读——不报错，
   只是文本条件全是垃圾。用 `bf16`（大卡）或 `Q8_0` 的 GGUF（小卡），
   后者正是上游 `docs/qwen_image.md` 命令行里用的那份。

国内拉权重：`aria2c -x 8 -s 8 -k 4M -o <名字> https://hf-mirror.com/...`。
单连接只有 1～2 MB/s，八连接能跑满带宽。

### 出片模型选哪个

`TI2V-5B` 是 Wan 家族里最小的一档，多家评测点名它的动作质量不如专门的
14B 图生视频模型。想要更好的图生视频就换 **I2V-A14B**：它是混合专家，
高噪声专家跑前几步定构图和运动，低噪声专家跑后几步出细节。

    [models]
    video            = "Wan2.2-I2V-A14B-LowNoise-Q8_0.gguf"
    video_high_noise = "Wan2.2-I2V-A14B-HighNoise-Q8_0.gguf"
    video_vae        = "wan_2.1_vae.safetensors"   # 不是 2.2 那份
    video_cfg        = 3.5                          # 上游给 A14B 的值，5B 是 6.0

**32 GB 的卡装不下两个专家**：15.4 + 15.4 + umt5 11.4 + VAE 0.25 = 42.5 GB。
而 sd.cpp 的组件规格里两个专家都算 `diffusion`，没法只把其中一份赶去内存
（见 `core/ggml_extend_backend.cpp` 的 `parse_backend_module`）。所以要
`weights = "cpu"`——上游 `docs/wan.md` 给 A14B 的命令行自己就带
`--offload-to-cpu`，那个开关就是这个意思。代价是每步都要搬权重，慢。

想让权重常驻显存就降量化：Q5_K_M 每份 10.8 GB，两份 21.6 GB，配
`weights = "te=cpu"` 能装下，留约 10 GB 给计算缓冲。画质比 Q8 低一档。

**MiniMax-H3** 是另一条路：画面和立体声一起生成。它在 sd.cpp 里用的参数位
和 Wan 不一样，编码器走 `video_llm`（裁过的 Qwen3-VL-32B）不是
`video_text_encoder`，另外多一个 `video_audio_vae`。**授权有地域限制**：
美国、欧盟、英国、韩国的创作者不能分发用开源权重生成的视频。

    [models]
    video           = "minimax_h3_fl2va-Q4_K_M.gguf"       # 18.8 GB
    video_llm       = "qwen3vl_32b_minimax_h3-Q4_K_M.gguf" # 18.2 GB，放内存
    video_vae       = "minimax_h3_video_vae_fp16.safetensors"
    video_audio_vae = "minimax_h3_audio_vae_fp32.safetensors"

### 权重放哪：让它自己按显存决定

`[models].weights = "smart"` 是**跨机器最省心的一档**，展开规则：

| 这张卡的显存 | 展开成 | 扩散模型 | 文本编码器 | VAE |
|---|---|---|---|---|
| < `vae_vram_min_gb`（默认 40 GB） | `te=cpu,vae=cpu` | 显存 | 内存 | 内存 |
| ≥ 40 GB | `te=cpu` | 显存 | 内存 | **显存** |

两条规则都是量出来的，不是拍的（5090 32.6 GB + MiniMax-H3）：

- **文本编码器永远放内存。** 它是最大的一块（Qwen3-VL-32B 18.9 GB），
  但每镜只跑一次，实测 8 到 9 秒。放显存换那几秒不值。
- **VAE 看卡。** 放内存时每镜解码 71 秒（5.5 GB 每次搬过 PCIe），
  放显存只要 8 秒，**一镜省 63 秒**。但 32 GB 上放不下：扩散 17.9 +
  VAE 5.5 = 23.4 GB 权重，加扩散自己约 9 GB 的计算缓冲，**差 112 MB**
  （`need 142.17 MB device, available 29.75 MB device`）。
  调小 `video_vae_tile` 救不了——缺的不是分块缓冲，是扩散模型那块没还。

### MiniMax-H3 的提速：Turbo LoRA

网上说 ComfyUI 上 H3 快 500%，就是这个。实测（5090，960×544，73 帧）：

| | 采样 | VAE 解码 | 每镜合计 |
|---|---|---|---|
| 28 步，无 LoRA | 164 秒 | 71 秒 | **242 秒** |
| **6 步 + Turbo LoRA** | **41 秒** | 71 秒 | **124 秒** |

**默认就开着，不用配。** `[models].video_lora` 的默认值就指着
`loras/minimax_h3_turbo_v4_step600_ema.safetensors`，挂上之后步数自动按
6 走。文件不在时按"没配"处理并在日志里说一声——没下过 LoRA 的机器照样
出片，只是慢几倍。

sd.cpp 认这个给 ComfyUI 做的 LoRA（日志里有 `apply lora at runtime`）。
**判据是耗时，不是"没报错"**——不认时只是加载不上，画面照出、耗时照旧。
画质：用户看过 6 步和 28 步的同一镜，看不出差别；后来又比过 6 步和 48 步，
说 48 步"细节确实多"，但换成 1280×704 之后 6 步就够了。

挂上 LoRA 之后瓶颈换人了：VAE 解码从 29% 变成 57%。所以大卡上
`weights = "smart"` 的收益比小卡大得多——41 + 8 = 49 秒一镜。

**草稿档 2026-09-10 起默认不跑。** 两档都挂 Turbo 之后画质拉不开差距，
那一遍就是白跑（一章 54 分钟）。想要的话 `POST /api/run` 传
`skip_draft: false`。

### 画幅和清晰度写在项目里

**不在全局设置里。** 一台机器上可以同时有一部横屏的正片和一批竖版的物料
（竖屏预告、花絮），画幅是这部电影的属性，不是这台机器的属性。
项目目录的 `changji.toml`：

    [video]
    orientation = "portrait"   # 或 landscape
    quality     = "720p"       # 或 2k

宽高由这两项算出来，用户不填数字。**720p 的短边是 704 不是 720**：
720 ÷ 32 = 22.5 除不尽，而 32 对齐是 Wan 那一族的硬约束（不对齐出图直接
失败，日志里指不到那儿）。这一条第一版就写错了，是单元测试抓住的。

**2K 一张 32 GB 的卡跑不动**（1280×704 时计算缓冲已 19.7 GB，2K 是四倍
像素）。**不会悄悄降档**——选了 2K 拿到 720p 且没有提示，比失败更糟。
那时候要么换大卡，要么出 720p 再 `changji --upscale`。

## 2. 配置

`~/.config/changji/config.toml`（工作进程的 systemd 单元里要设
`Environment=HOME=/root` 和 `XDG_CONFIG_HOME=/root/.config`，
**systemd 不会自己给 HOME**，缺了它工作进程读的是内置默认值然后报"没配模型"）。

    # 只影响档位表（分辨率/步数），不影响真正装了什么。
    # Wan 2.2 TI2V-5B 的训练分辨率是 1280×704，成片档超出去只会更慢不会更好，
    # 所以哪怕卡有 48 GB 也填 20 —— 20 落在 ≥15.5 那一档：
    #   草稿 640×352/10 步，预览 960×544/20 步，成片 1280×704/30 步
    vram_gb_override = 20

    [models]
    engine = "sd"
    dir = "/data/models"
    # 权重放哪。auto = 交给 sd.cpp 按这张卡真实的空闲显存决定，装得下就常驻；
    # cpu = 全放系统内存（小卡唯一的选择，慢）
    weights = "auto"
    # 出片：MiniMax-H3（编码器走 video_llm 不是 video_text_encoder，
    # cfg 1.0、rng cpu——三处和 Wan 不一样，错了都不报错）
    video = "minimax_h3_fl2va-Q4_K_M.gguf"
    video_vae = "minimax_h3_video_vae_fp16.safetensors"
    video_audio_vae = "minimax_h3_audio_vae_fp32.safetensors"
    video_llm = "qwen3vl_32b_minimax_h3-Q4_K_M.gguf"
    video_cfg = 1.0
    video_rng = "cpu"
    # 加速 LoRA 默认就指着这个路径，不用写；文件不在会在日志里说一声
    # video_lora = "loras/minimax_h3_turbo_v4_step600_ema.safetensors"
    image = "Qwen-Image-Edit-2509-Q8_0.gguf"
    image_vae = "qwen_image_vae.safetensors"
    image_text_encoder = "qwen_2.5_vl_7b_bf16.safetensors"
    # 2509 不带视觉塔就是"参考图只进去一半"，而且不报错
    image_text_encoder_vision = "Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf"
    tts = "Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf"
    tts_decoder = "mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf"
    # 首帧按哪个档位出。首帧是跨镜头一致性的锚点、又会当起始图喂给出片
    # 那一步，出小了等于把锚点放大再用。显存够就填 final。
    frame_tier = "final"
    # 大模型：进程内跑
    llm = "llm/Qwen3-14B-Q4_K_M.gguf"

    [tts]
    backend = "local"          # 进程内 Qwen3-TTS，不用起别的服务

    [workers]
    gpu = 0                    # 主进程自己（配音、大模型）绑哪张卡
    # endpoints 留空：本机多卡时主进程会自己拉起每张卡一个工作进程。
    # 只有跨机才填：endpoints = ["http://别的机器:9001", ...]

    [llm]
    backend = "local"          # 进程内跑，不用另起 llama-server
    # backend = "remote" 时才用下面这几个
    base_url = "http://127.0.0.1:8081/v1"
    model = "qwen3-14b"
    api_key = "none"
    timeout_s = 1800.0

    [tiers]
    # 画质档位，不填按显存推。设置页改了会写回这里。
    final_width = 1280
    final_height = 704
    final_steps = 6            # 挂了 Turbo LoRA 就 6 步

## 3. 进程

### 单机：一个进程就够

**推荐这条。** 编一份全开的二进制（SD + CUDA + LLAMA），把
`[workers].endpoints` 留空，什么都在进程内跑：

    cmake -S cpp -B build-all -DCMAKE_BUILD_TYPE=Release       -DCHANGJI_SD=ON -DCHANGJI_SD_CUDA=ON -DCHANGJI_LLAMA=ON       -DCHANGJI_CUDA_ARCH=120 -DCMAKE_CUDA_ARCHITECTURES=120       -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
    cmake --build build-all --target changji -j 64

**编完先验一句**，别急着跑：

    strings build-all/changji | grep -c ggml_cuda      # 要几千，0 就是没编进去

**0 的后果是"能跑但慢一百倍"，而且哪一层都不报错。** 2026-09-10 在这台
5090 上栽过：手敲配置命令漏了 `-DCMAKE_CUDA_COMPILER`，而这台机器的 nvcc
不在 PATH 上。CMake 配置成功、编译成功、服务起得来、任务开跑，
1280×704 的一镜跑了五个多小时才两步——进程占着 52 GB 内存、395% CPU，
而 `nvidia-smi` 上一个计算进程都没有。现在 CMake 会在配置期直接停下来。

    # 配置里
    [workers]
    endpoints = []          # 留空 = 全部进程内

    [llm]
    backend = "local"       # 不用另起 llama-server

    ./build-all/changji --port 8080 --host 0.0.0.0

出图、出片、配音、大模型、webapp、接口全在这一个进程里。
**判据是 `pgrep -c changji` 等于 1。**

这条能成立靠两件事：`run_deps.cpp` 里 `make_worker_pool` 拿到空列表时
返回空，上面那三行进程内的默认原样生效；以及大模型是调度器的一个槽
（`Slot::LLM`），出片要显存时按**实时空闲显存**决定要不要让开。

### 多卡：也是一个进程，它自己拉起每张卡

**多卡不用你手动起一堆进程。** `[workers].endpoints` 留空时，主进程探到
N 张卡就自己拉起 N 个 `--worker --gpu i`，第 i 张卡监听 `base_port + i`
（默认 9001 起），退出时一起收掉。启动的还是一个命令：

    ./build-all/changji --port 8080 --host 0.0.0.0

日志里会逐张卡报 `[多卡] 卡 N 就绪：http://127.0.0.1:900N`。
某张卡起不来（模型载不动、显存不够）就跳过它并说清是哪张，其余照跑；
一张都起不来时退回单卡进程内——**多卡拉不起来该继续干活，不是让服务起不来**。

关掉自动拉起：`[workers].auto_spawn = false`。

**为什么是子进程而不是进程内开 N 个上下文。** sd.cpp 的进度回调是全局的
（`sd_set_progress_callback` 没有 user data 能区分上下文），进程内并发跑
两张卡时两路进度会串到一起——而进度是判断"卡住没有"的唯一依据。
CUDA 设备也是按进程选的。子进程把这两件事都绕开，而且工作进程协议本来
就有（跨机部署用的是同一套）。

### 跨机：填 endpoints

一张卡一个工作进程，协调者一个，大模型一个。

    # 工作进程（systemd 模板，%i 是卡号）
    Environment=CUDA_VISIBLE_DEVICES=%i
    Environment=HOME=/root
    Environment=XDG_CONFIG_HOME=/root/.config
    ExecStart=/root/changji/build-worker/changji --worker --gpu %i --port 900$((%i+1))

    # 协调者。--host 0.0.0.0 才能从外面开 webapp（端口就是这个 --port）。
    # **这套接口没有鉴权**，连上就能读项目、改分镜、起流水线。
    ExecStart=/root/changji/build-coord/changji --port 8080 --host 0.0.0.0
    Environment=CHANGJI_WORKSPACE=/root/.local/share/changji/projects

    # 大模型：找一张没被工作进程占的卡。
    # **单机不需要这个**，[llm].backend = "local" 就在进程内跑。
    CUDA_VISIBLE_DEVICES=6 llama-server -m Qwen3-14B-Q4_K_M.gguf \
      --port 8081 --host 127.0.0.1 -ngl 99 -c 65536 \
      --chat-template-kwargs '{"enable_thinking":false}'

**别加 `--reasoning-format none`**：加了它空的 `<think></think>` 会原样留在
content 里；默认（auto）会剥到 `reasoning_content`，content 才干净。

## 4. 构建

**单机就编一份全开的**（见上面第 3 节）。下面这个脚本编的是多卡那套的
两份——工作进程和协调者各关掉一半，那是为了多卡上一张卡一个进程。

    bash cpp/tools/build_gpu_box.sh /root/changji /root/autodl-tmp 120

架构号：Ada（L20/4090）= 89，Blackwell（5080/5090）= **120**。填错编出来的
东西在卡上跑不了，而且要跑到加载模型那一步才报错。

脚本先编一份**纯 CPU 的测试目标**（最快能发现编译器问题：新机器的 g++ 可能
比你上次用的老，C++20 有些地方不认），再编工作进程（sd.cpp + CUDA）和
协调者（llama.cpp + CUDA，进程内配音）。

**构建目录别放系统盘。** AutoDL 这类机器 `/` 只有 30 G，而 FetchContent 拉的
那堆（llama.cpp + sd.cpp + ggml + Crow + asio…）加上 CUDA 目标文件轻松几个 G，
编到一半没空间比编不过还难查。

**国内机器上 GitHub 可能直连不通。** `git ls-remote https://github.com/...`
会挂着不返回，FetchContent 就卡在克隆那一步，**日志里一个字都不打**——
看起来像"编译很慢"，实际是永远不会好。AutoDL 自带 `/etc/network_turbo`，
`source` 一下就通（脚本会自动找它）。**它会让 pip / apt 变慢**，
所以只在构建时开；下模型别开，hf-mirror 本来就是国内的。


## 5. 跑之前先看一眼画面

**先看画面，再量速度。** 在 L20 那台上先量了 39 分钟的速度，才发现量的是雪花。

    # 只出首帧，抽一张出来看。要带 -H：引擎只收 JSON（不带的话 curl 发的是表单，回 415）
    curl -X POST :8080/api/run -H 'Content-Type: application/json' \
         -d '{"project":"...","episode_id":"ep01","stages":["frames"],"force":true}'
    # 然后真的把 frames/*.png 打开看

分不出来的时候用上游的 `sd-cli` 拿同一批权重跑一遍，能把"我们的代码"
和"模型/旋钮"分开。

## 6. 已知的坑

- **`pkill -f "..."` 在 ssh 里会杀掉自己**（模式匹配到 `bash -c` 那行命令行）。
  用 `pkill -x` 或锚定 `^`。
- **后台起进程要 `setsid nohup … </dev/null >log 2>&1 &`**，否则 ssh 会挂在
  pty 上直到超时；掉线还会把部署打断到一半。
- **`tar` 同步源码带的是本机 mtime**，两边时钟不一致时 cmake 会认为目标文件
  比源码新、**静默不重编**。解压后 `touch` 改过的文件再编。
- **正在跑的二进制不能原地重编**（链接时 ETXTBSY），先停服务或另起构建目录。
- **显存占用不能当"这张卡在忙"的证据**——工作进程跑完不卸模型。看 utilization。
- 云厂商的安全组要放行 webapp 那个端口，机器里绑了 0.0.0.0 不等于外面能连。
