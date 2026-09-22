#!/bin/bash
# 把一台新 GPU 机器要的权重全下下来。断点续传，可以反复跑。
#
# 用法：
#     bash fetch_models.sh [目录=/data/models] [encoder=q8|bf16]
#
# encoder 选哪个看单卡显存，见 setup_gpu_box.md 第 0 节：
#   q8   —— Qwen2.5-VL Q8_0 GGUF 约 8 GB。**32 GB 的卡（5090）用这个**，
#           配 fp8 图像模型正好 28 GB，装得下。
#   bf16 —— 16 GB。≥40 GB 的卡（L20/A100）用这个，质量略好。
#
# **地址全部是 hf-mirror.com**（HuggingFace 的国内镜像）。2026-09-09 逐个
# 验过 HTTP 200 和文件大小。单连接只有 1～2 MB/s，所以一律走 aria2c 八连接。
#
# 顺序是按"哪一套先凑齐就能先跑起来"排的：先配音（最小，能验进程内 TTS），
# 再出片那一套（能出草稿），最后出首帧那一套（最大）。

set -u
DIR="${1:-/data/models}"
ENC="${2:-q8}"
B=https://hf-mirror.com
mkdir -p "$DIR/llm"

command -v aria2c >/dev/null || { echo "先装 aria2：apt-get install -y aria2"; exit 1; }

# 下一个文件。已经下完（大小对得上）就跳过。
dl() {
  local out="$1" url="$2" want="$3"
  local path="$DIR/$out"
  if [ -f "$path" ]; then
    local have; have=$(stat -c%s "$path" 2>/dev/null || echo 0)
    if [ "$have" = "$want" ]; then echo "  ✓ $out（已有）"; return 0; fi
    echo "  ↻ $out（$have / $want，续传）"
  else
    echo "  ↓ $out（$(( want / 1024 / 1024 )) MB）"
  fi
  aria2c -x 8 -s 8 -k 4M --file-allocation=none --auto-file-renaming=false \
         --continue=true -d "$(dirname "$path")" -o "$(basename "$path")" \
         "$url" >/dev/null 2>&1 || { echo "    ✗ 下失败：$out"; return 1; }
  local now; now=$(stat -c%s "$path" 2>/dev/null || echo 0)
  [ "$now" = "$want" ] || { echo "    ✗ 大小对不上：$now ≠ $want"; return 1; }
  echo "    ✓ 完成"
}

echo "== 配音（Qwen3-TTS，进程内跑，不用起别的服务） =="
dl Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf \
   "$B/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf" \
   3472593760
dl mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf \
   "$B/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf" \
   669081472

echo "== 出片（Wan 2.2 TI2V-5B） =="
# VAE 是 2.2 的，**不是 2.1 的**——只有 TI2V-5B 用这一份，拿错了出来的是花屏
dl wan2.2_ti2v_5B_fp16.safetensors \
   "$B/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/diffusion_models/wan2.2_ti2v_5B_fp16.safetensors" \
   9999658848
dl Wan2.2_VAE.safetensors \
   "$B/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors" \
   1409400960
dl umt5_xxl_fp16.safetensors \
   "$B/Comfy-Org/Wan_2.1_ComfyUI_repackaged/resolve/main/split_files/text_encoders/umt5_xxl_fp16.safetensors" \
   11366399385

echo "== 出首帧（Qwen-Image-Edit 2509） =="
# ⚠️ **Edit 是图生图编辑模型，一定要有参考图。** 没有任何参考图的时候它退化
# 成文生图，出来的是彩色雪花——而且方差比真图还大，靠"方差不为零"判"不是空图"
# 会一路绿灯。这台机器上栽过。
#
# 这套流水线本来就每一镜都喂参考图（在场每个角色的三视图 + 这个场景的空景图，
# 见 stages/prompt_compose.cpp 的 refs），所以走的是它擅长的那条路——跨镜头
# 同一张脸靠的就是它。**代价是定妆和参考图必须先铺开**：一张参考图都没有的
# 镜头会撞上上面那句。
#
# **要 2509 不要初版**：多参考图是 2509 加的，初版只收一张，而一镜常常三张。
# 下载目录（setup/catalog.cpp）里装的也是这一族，两条路要一致。
if [ "$ENC" = "bf16" ]; then
  # ≥40 GB 的卡：整个梯队最好的 Q8_0
  dl Qwen-Image-Edit-2509-Q8_0.gguf \
     "$B/QuantStack/Qwen-Image-Edit-2509-GGUF/resolve/main/Qwen-Image-Edit-2509-Q8_0.gguf" \
     21761817120
else
  # 32 GB 的卡：Q6_K 配 weights="auto"，权重常驻，35 秒一张。
  # 再往上 Q8_0 是 21.8 GB，这张卡上只能放内存，慢五倍。
  dl Qwen-Image-Edit-2509-Q6_K.gguf \
     "$B/QuantStack/Qwen-Image-Edit-2509-GGUF/resolve/main/Qwen-Image-Edit-2509-Q6_K.gguf" \
     16824990240
fi
dl qwen_image_vae.safetensors \
   "$B/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/vae/qwen_image_vae.safetensors" \
   253806246

if [ "$ENC" = "bf16" ]; then
  # ≥40 GB 的卡
  dl qwen_2.5_vl_7b_bf16.safetensors \
     "$B/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/text_encoders/qwen_2.5_vl_7b.safetensors" \
     16584415576
else
  # 32 GB 的卡。**别用 Comfy 的 fp8_scaled**：那种格式带 scale 张量，
  # sd.cpp 的加载器里没有一行处理 scaled，会按普通 fp8 读——不报错，
  # 只是文本条件全是垃圾。上游 docs/qwen_image.md 用的就是这份 Q8_0。
  dl Qwen2.5-VL-7B-Instruct-Q8_0.gguf \
     "$B/unsloth/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct-Q8_0.gguf" \
     8098524032
fi

# 文本编码器的视觉塔。**2509 起非它不可，而且不带也不报错**：sd.cpp 只在日志
# 里说一句 "no vision weights detected, vision disabled" 然后照常跑，参考图只
# 剩 VAE 潜空间那一半进 DiT，出来的图和参考对不上。
# 它在**初版 Edit 那个仓库**下面——2509 那个仓库只放了扩散权重。
dl Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf \
   "$B/QuantStack/Qwen-Image-Edit-GGUF/resolve/main/mmproj/Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf" \
   1354163040

echo "== 编剧用的大模型（llama-server 跑，和出图出片分开一张卡） =="
dl llm/Qwen3-14B-Q4_K_M.gguf \
   "$B/Qwen/Qwen3-14B-GGUF/resolve/main/Qwen3-14B-Q4_K_M.gguf" \
   9001752960

echo
echo "== 盘上现在有 =="
du -sh "$DIR"
ls -la "$DIR" "$DIR/llm" | awk '$5 > 1000000 {printf "  %10.1f GB  %s\n", $5/1073741824, $9}'
echo
echo "配置照着写：cpp/tools/setup_gpu_box.md 第 2 节"
if [ "$ENC" = "bf16" ]; then
  echo '  image = "Qwen-Image-Edit-2509-Q8_0.gguf"'
  echo '  image_text_encoder = "qwen_2.5_vl_7b_bf16.safetensors"'
  echo '  weights = "auto"'
else
  echo '  image = "Qwen-Image-Edit-2509-Q6_K.gguf"'
  echo '  image_text_encoder = "Qwen2.5-VL-7B-Instruct-Q8_0.gguf"'
  echo '  weights = "auto"'
fi
# **这一行不能漏**：2509 不带视觉塔就是"参考图只进去一半"，而且不报错。
echo '  image_text_encoder_vision = "Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf"'
echo '  image_vae = "qwen_image_vae.safetensors"' 
