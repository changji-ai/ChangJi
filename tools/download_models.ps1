# 把进程内推理要用的权重下齐。
#
# 阶段 5（出图出片）、阶段 7（配音+装配）、阶段 9（进程内配音）的实机判据
# 都卡在这些文件上。**下之前先把大小和剩余空间摆出来**，因为这套东西
# 加起来 10 GB，而这台机器上为它腾过一次地方。
#
# ⚠️ **要 GGUF，不要 safetensors。** 仓库里原来的 download_models.ps1 下的是
# fp16 safetensors（16.9 GB），那套是给 ComfyUI 的，**sd.cpp 根本吃不了**。
# 配音那边当初也是同一个漏。这个脚本只下 GGUF（VAE 除外，它本来就是
# safetensors，sd.cpp 认）。
#
# 文件名要和配置模板里 [models] 那几项对得上，见 settings.cpp 的 kDefaultToml。

param(
    [string]$Dest = "$PSScriptRoot\..\..\..\models",
    # min  = 最小档，验通路（约 21 GB）。6 GB 卡上只能用这个。
    # full = 全尺寸，看真实效果（约 70 GB）。48 GB 卡起步。
    #
    # **别叫 $Profile**：那是 PowerShell 的自动变量（指向 profile 脚本），
    # 拿它当参数名不报错，但取到的永远不是你传进来的值——
    # 表现是 -Profile full 静默地下了最小档。
    [ValidateSet("min", "full")]
    [string]$Preset = "min",
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

# 名字 / URL / 期望大小（MB，2026-09-08 用 HEAD 查的真实值）/ 落盘文件名
$files = @(
    @{ n='扩散主模型 Wan2.2-TI2V-5B Q4_K_M'; mb=3274
       u='https://huggingface.co/QuantStack/Wan2.2-TI2V-5B-GGUF/resolve/main/Wan2.2-TI2V-5B-Q4_K_M.gguf'
       f='Wan2.2-TI2V-5B-Q4_K_M.gguf' },
    @{ n='文本编码器 umt5-xxl Q5_K_M';        mb=3953
       u='https://huggingface.co/city96/umt5-xxl-encoder-gguf/resolve/main/umt5-xxl-encoder-Q5_K_M.gguf'
       f='umt5-xxl-encoder-Q5_K_M.gguf' },
    @{ n='VAE Wan2.2（safetensors，sd.cpp 认）'; mb=1344
       u='https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors'
       f='Wan2.2_VAE.safetensors' },
    @{ n='配音骨干 Qwen3-TTS 12Hz 1.7B Q4_K_M'; mb=987
       u='https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf'
       f='Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf' },
    @{ n='配音解码器 mmproj Q8_0';             mb=425
       u='https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf'
       f='mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf' },
    # 首帧用的图像模型。
    #
    # ⚠️ **要 2509，不要初版。** 多参考图是 2509 加的，而这套流水线一镜常常
    # 要喂三张（在场每个角色的三视图 + 这个场景的空景图）；初版只收一张。
    # 下载目录（setup/catalog.cpp）里装的也是这一族，两条路要一致。
    #
    # ⚠️ **两个仓库的文件名规矩不一样**：初版那个仓库里叫 Qwen_Image_Edit-*
    # （下划线），2509 这个仓库里叫 Qwen-Image-Edit-2509-*（连字符）。
    # 拼错一个字符就是 404。
    #
    # ⚠️ **6.8 GB，不是方案里写的"约 4 GB"。** Qwen-Image-Edit 是 20B 的模型，
    # 整个量化梯队最小的 Q2_K 也有 6.8 GB——**都超过这台机器 6 GB 的显存**，
    # 所以一定要靠 offload，出一张首帧会比 Wan 出一段视频还慢。
    # 字节数 2026-09-15 在 HF 和魔搭上各查了一遍，两边一致。
    # **先用最小的档验通路。** 画质要求更高的档等通了再换，换档只是改这一行
    # 加配置里的文件名。
    @{ n='图像模型 Qwen-Image-Edit 2509 Q2_K（最小档，先验通路）'; mb=6816
       u='https://huggingface.co/QuantStack/Qwen-Image-Edit-2509-GGUF/resolve/main/Qwen-Image-Edit-2509-Q2_K.gguf'
       f='Qwen-Image-Edit-2509-Q2_K.gguf' },
    # 视觉塔在**初版那个仓库**下面——2509 的仓库里只有扩散权重。
    # 2509 起不带它就是"参考图只进去一半"，而且不报错，日志里只有一句
    # vision disabled。
    @{ n='图像模型的视觉塔 Qwen2.5-VL mmproj'; mb=1291
       u='https://huggingface.co/QuantStack/Qwen-Image-Edit-GGUF/resolve/main/mmproj/Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf'
       f='Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf' },
    # **Qwen-Image-Edit 的文本编码器不是 umt5，是 Qwen2.5-VL。**
    # 一开始只下了扩散模型就以为够了，是因为 C++ 那边图像那条路复用了视频的
    # video_vae 和 video_text_encoder——而那两个是 Wan 的，喂给 Qwen 是错的。
    # sd.cpp 的文档写得很清楚（docs/qwen_image_edit.md）：
    #   --diffusion-model Qwen_Image_Edit-*.gguf
    #   --vae             qwen_image_vae.safetensors   ← 不是 Wan 的 VAE
    #   --llm             Qwen2.5-VL-7B-Instruct.gguf  ← 不是 umt5，走 llm_path
    # 也就是说图像那条路一直没接对，只是从来没跑过所以没人发现。
    @{ n='图像模型的文本编码器 Qwen2.5-VL 7B Q2_K（最小档）'; mb=2876
       u='https://huggingface.co/mradermacher/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct.Q2_K.gguf'
       f='Qwen2.5-VL-7B-Instruct-Q2_K.gguf' },
    @{ n='图像模型的 VAE（Qwen 自己的，不是 Wan 的）'; mb=242
       u='https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/vae/qwen_image_vae.safetensors'
       f='qwen_image_vae.safetensors' }
)

# ---- 全尺寸档 ----
#
# **扩散模型走 bf16，文本编码器走 fp8。** 不是省事，是显存算出来的：
#   图像 bf16 38.05 GB + 它的文本编码器 bf16 15.45 GB = 53.5 GB > 48 GB，
#   一张 L20 装不下。而 8 路并行是**每个进程在自己那张卡上放满一套**，
#   不是八张卡分着放。
# 文本编码器换成 fp8_scaled（8.74 GB）之后是 47 GB，刚好塞得下。
# L20 是 Ada，有原生 FP8 张量核，这一档不是模拟出来的。
#
# 画质由扩散模型决定，文本编码器的精度影响小得多。真要对比，
# 单独再下一份 bf16 的编码器（15.45 GB），配置里换一行 image_text_encoder
# 就切，别的都不用重下。
#
# **写剧本那个 27.5 GB 的没放进来**：到今天为止流水线一次都没用过它
# （剧本和分镜走外部 Ollama 或者手写分镜表）。要进程内跑再单独加。
$fullFiles = @(
    @{ n='图像 Qwen-Image-Edit bf16（全尺寸）'; mb=38963
       u='https://huggingface.co/Comfy-Org/Qwen-Image-Edit_ComfyUI/resolve/main/split_files/diffusion_models/qwen_image_edit_bf16.safetensors'
       f='qwen_image_edit_bf16.safetensors' },
    @{ n='图像的文本编码器 Qwen2.5-VL fp8（装得下的那一档）'; mb=8950
       u='https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/text_encoders/qwen_2.5_vl_7b_fp8_scaled.safetensors'
       f='qwen_2.5_vl_7b_fp8_scaled.safetensors' },
    @{ n='图像 VAE'; mb=246
       u='https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/vae/qwen_image_vae.safetensors'
       f='qwen_image_vae.safetensors' },
    @{ n='视频 Wan2.2-TI2V-5B fp16（全尺寸）'; mb=9534
       u='https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/diffusion_models/wan2.2_ti2v_5B_fp16.safetensors'
       f='wan2.2_ti2v_5B_fp16.safetensors' },
    @{ n='视频 VAE'; mb=1344
       u='https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors'
       f='Wan2.2_VAE.safetensors' },
    @{ n='视频的文本编码器 umt5-xxl fp16（全尺寸）'; mb=10844
       u='https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/text_encoders/umt5_xxl_fp16.safetensors'
       f='umt5_xxl_fp16.safetensors' },
    @{ n='配音骨干 Qwen3-TTS bf16（全尺寸）'; mb=3308
       u='https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf'
       f='Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf' },
    @{ n='配音解码器 mmproj bf16'; mb=635
       u='https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf'
       f='mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf' }
)

if ($Preset -eq "full") { $files = $fullFiles }


$dest = [System.IO.Path]::GetFullPath($Dest)
# **只算还差多少，不是算一共多少。**
#
# 原来这里是把 $files 里所有 mb 加起来跟剩余空间比。加到第 9 个文件时目录
# 变成 21 GB，而盘上只剩 20 GB——于是它拒绝下载，可实际上只差 3 GB，
# 别的早就下好了。**目录一旦超过盘的大小，这个检查就永远不让你下**，
# 哪怕一个字节都不缺。
#
# 改成按"这个文件还差多少"算：没有就算全量，下了一半就算剩下那半。
$need = 0
foreach ($x in $files) {
    $p = Join-Path $dest $x.f
    $haveMb = 0
    if (Test-Path $p) { $haveMb = [math]::Round((Get-Item $p).Length / 1MB) }
    $left = $x.mb - $haveMb
    if ($left -gt 0) { $need += $left }
}
$total = $need
# Windows PowerShell 5.1 没有 ?? 和三元运算符，别用
$qualifier = (Split-Path $dest -Qualifier) -replace ':', ''
$freeGb = [math]::Round((Get-PSDrive $qualifier).Free / 1GB, 1)

Write-Host "落盘目录：$dest"
Write-Host "$($files.Count) 个文件，还差 $([math]::Round($total/1024,1)) GB；该盘剩 $freeGb GB"
Write-Host ""
foreach ($x in $files) { "  {0,6} MB  {1}" -f $x.mb, $x.n | Write-Host }
Write-Host ""

if ($total/1024 -gt $freeGb - 5) {
    Write-Host "空间不够（要留 5 GB 余量）。腾出空间再来。" -ForegroundColor Red
    exit 1
}
if ($WhatIf) { Write-Host "（-WhatIf，不实际下载）"; exit 0 }

New-Item -ItemType Directory -Force -Path $dest | Out-Null

foreach ($x in $files) {
    $out = Join-Path $dest $x.f
    if (Test-Path $out) {
        $haveMb = [math]::Round((Get-Item $out).Length / 1MB)
        # 差一成以内就算下全了。HF 的 content-length 和落盘大小会有零头
        if ([math]::Abs($haveMb - $x.mb) -lt ($x.mb * 0.1)) {
            Write-Host "已有，跳过：$($x.f)（$haveMb MB）" -ForegroundColor DarkGray
            continue
        }
        # **别删。** 下面 curl 带 -C -，接着下就行。
        # 第一版这里直接 Remove-Item，等于把断点续传废掉了——
        # 一个 4 GB 的文件断在 627 MB，重跑一次从零开始。
        # 只有比目标还大才删：那说明文件是坏的，续传接不上。
        if ($haveMb -gt $x.mb * 1.1) {
            Write-Host "比该有的还大（$haveMb MB > $($x.mb) MB），删掉重下：$($x.f)" -ForegroundColor Yellow
            Remove-Item $out -Force
        } else {
            Write-Host "下了一半（$haveMb / $($x.mb) MB），接着下：$($x.f)" -ForegroundColor Yellow
        }
    }
    Write-Host "下载 $($x.n) … $($x.mb) MB"
    # curl 而不是 Invoke-WebRequest：后者会把整个响应读进内存，
    # 3 GB 的文件能把机器拖垮。curl 直接落盘，还能断点续传。
    #
    # ⚠️ **必须 -sS 把进度条关掉。** curl 的进度条走的是 stderr，而
    # Windows PowerShell 5.1 一旦把原生程序的 stderr 收进管道（调用方写
    # `2>&1` 就会），每一行都被包成 ErrorRecord，配上
    # $ErrorActionPreference='Stop' 就是**下第一个文件时当场中止**——
    # 而报出来的是 NativeCommandError，看着像 curl 挂了。
    # -sS 保留真正的错误信息，只去掉进度。
    # **整个文件重试，不只靠 curl 自己的 --retry。**
    # 几 GB 的文件在 Windows 的 schannel 上常见 `curl: (56) server closed
    # abruptly (missing close_notify)`——TLS 连接中途断掉。curl 的 --retry
    # 管的是"请求失败"，对传到一半断线不一定重来。
    # 好在 -C - 会续传，所以从外面再叫一次就是接着下。
    # **按"有没有进展"决定要不要接着试，不是数固定次数。**
    # 这台机器上 HF 的连接很不稳（schannel 的 (35) 握手失败、(56) 对端
    # 没发 close_notify 就断），但每次都能往前推几百 MB。定死 4 次的话
    # 一个 4 GB 的文件必然下不完，而它其实只是需要多接几次。
    # 判停的条件改成"连着几次一个字节都没多"——那才是真的卡住了。
    $ok = $false
    $stall = 0
    $try = 0
    while (-not $ok -and $stall -lt 5) {
        $try++
        $before = 0
        if (Test-Path $out) { $before = (Get-Item $out).Length }
        if ($try -gt 1) {
            Write-Host ("  第 {0} 次（已有 {1} MB，续传）" -f $try, [math]::Round($before/1MB)) -ForegroundColor DarkGray
            Start-Sleep -Seconds 5
        }
        & curl.exe -sS -L --fail --retry 3 --retry-delay 5 --retry-all-errors `
            -C - -o "$out" $x.u
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }

        $after = 0
        if (Test-Path $out) { $after = (Get-Item $out).Length }
        if ($after -gt $before) { $stall = 0 } else { $stall++ }
    }
    if (-not $ok) {
        Write-Host "下载失败：$($x.f)（连着 5 次没有进展，试了 $try 次）" -ForegroundColor Red
        exit 1
    }
    $gotMb = [math]::Round((Get-Item $out).Length / 1MB)
    if ([math]::Abs($gotMb - $x.mb) -gt ($x.mb * 0.1)) {
        Write-Host "下完大小不对：有 $gotMb MB，该 $($x.mb) MB" -ForegroundColor Red
        exit 1
    }
    Write-Host "  好了：$gotMb MB" -ForegroundColor Green
}

Write-Host ""
Write-Host "全下完了。配置里这么填（changji.toml 的 [models]）：" -ForegroundColor Cyan
Write-Host @"
[models]
engine = "sd"
dir = "$($dest -replace '\\','/')"
video = "Wan2.2-TI2V-5B-Q4_K_M.gguf"
video_vae = "Wan2.2_VAE.safetensors"
video_text_encoder = "umt5-xxl-encoder-Q5_K_M.gguf"
tts = "Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf"
tts_decoder = "mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf"
"@
