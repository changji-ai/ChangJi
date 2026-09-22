#!/usr/bin/env bash
# 就地编一个带 CUDA 的 changji。
#
# **为什么需要它**：发布的 Linux/Windows 二进制是纯 CPU 的（workflow 里
# 没开 GGML_CUDA），在 N 卡机器上整条流水线跑在 CPU 上。2026-09-11 在
# 一台 5090 上实测：CPU 版 nvidia-smi 是 0 MiB / 0%，CUDA 版是 24 GB / 100%。
#
# **这个脚本不写死任何和机器有关的值**，全部现场探：
#   - CUDA 在哪（PATH 里没有就找 /usr/local/cuda*）
#   - 这张卡的算力是多少（nvidia-smi 问，不猜）
#   - Python 在哪（llama.cpp 要就地打补丁）
#   - 依赖源码在哪（已经下过就复用，别重下 1.7 GB）
#   - 编译并发度（按 CPU 核数）
#
# 用法：
#   bash changji/cpp/tools/build_cuda.sh [输出目录]
# 环境变量：
#   CHANGJI_DEPS=/path/to/_deps   复用已下好的依赖
#   CHANGJI_ARCH=90               手动指定算力，不填就问显卡

set -euo pipefail

BUILD_DIR="${1:-build-cuda}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { printf '\033[31m错误:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

# ---- CUDA ----
if ! command -v nvcc >/dev/null 2>&1; then
  for d in /usr/local/cuda/bin /usr/local/cuda-*/bin; do
    [ -x "$d/nvcc" ] && export PATH="$d:$PATH" && break
  done
fi
command -v nvcc >/dev/null 2>&1 || die "找不到 nvcc。装了 CUDA 的话把它的 bin 加进 PATH。"
export CUDACXX="$(command -v nvcc)"
info "nvcc: $CUDACXX ($(nvcc --version | sed -n 's/.*release \([0-9.]*\).*/\1/p' | tail -1))"

# ---- 算力：问显卡，不猜 ----
# 写死一个值就是"换台机器就不行"。compute_cap 报的是 8.9 这种，
# CMake 要的是 89。
ARCH="${CHANGJI_ARCH:-}"
if [ -z "$ARCH" ]; then
  command -v nvidia-smi >/dev/null 2>&1 || die "没有 nvidia-smi，问不到算力。用 CHANGJI_ARCH=xx 手动指定。"
  ARCH="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1 | tr -d ' .')"
  [ -n "$ARCH" ] || die "问不到算力。用 CHANGJI_ARCH=xx 手动指定。"
fi
info "目标算力: sm_$ARCH"

# ---- Python：llama.cpp 要就地打补丁 ----
PY=""
for c in python3 python "$HOME/miniconda3/bin/python3" /opt/conda/bin/python3; do
  command -v "$c" >/dev/null 2>&1 && PY="$(command -v "$c")" && break
  [ -x "$c" ] && PY="$c" && break
done
[ -n "$PY" ] || die "找不到 python3。llama.cpp 拉下来要用它打补丁。"
info "python: $PY"

# ---- 依赖：下过就复用 ----
DEPS="${CHANGJI_DEPS:-}"
EXTRA=()
if [ -z "$DEPS" ]; then
  for d in "$SRC_DIR"/build*/_deps "$SRC_DIR"/../../build*/_deps; do
    [ -d "$d/llama-src" ] && DEPS="$d" && break
  done
fi
if [ -n "$DEPS" ] && [ -d "$DEPS/llama-src" ]; then
  info "复用已下好的依赖: $DEPS"
  for n in ASIO:asio CROW:crow DOCTEST:doctest HTTPLIB:httplib JSON:json \
           LLAMA:llama STABLE_DIFFUSION_CPP:stable_diffusion_cpp TOMLPLUSPLUS:tomlplusplus; do
    var="${n%%:*}"; dir="${n##*:}"
    [ -d "$DEPS/$dir-src" ] && EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_$var=$DEPS/$dir-src")
  done
else
  info "没找到现成依赖，会从网上拉（国内可能很慢）"
fi

JOBS="$(nproc 2>/dev/null || echo 4)"
info "并发: $JOBS"

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHANGJI_SD=ON -DCHANGJI_LLAMA=ON -DCHANGJI_BUILD_TESTS=OFF \
  -DGGML_NATIVE=OFF -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="$ARCH" \
  -DPython3_EXECUTABLE="$PY" \
  "${EXTRA[@]}"

cmake --build "$BUILD_DIR" -j "$JOBS"

info "编好了: $BUILD_DIR/changji"
info "验一眼它真带 CUDA："
if strings "$BUILD_DIR/changji" 2>/dev/null | grep -qiE "ggml-cuda|cudaMalloc"; then
  printf '\033[32m  有 CUDA 符号\033[0m\n'
else
  printf '\033[31m  没有 CUDA 符号——这一版还是纯 CPU 的，别用\033[0m\n'; exit 1
fi
