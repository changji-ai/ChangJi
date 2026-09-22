#!/bin/bash
# 在一台新 GPU 机器上编两份二进制。
#
#     bash build_gpu_box.sh [源码目录=/root/changji] [构建根=/root/autodl-tmp] [架构号=120]
#
# 架构号：Ada（L20/4090）= 89，Blackwell（5080/5090）= 120。填错编出来的
# 东西在卡上跑不了，而且要跑到加载模型那一步才报错。
#
# **构建目录别放系统盘**：AutoDL 这类机器 `/` 只有 30 G，而 FetchContent
# 拉的那堆（llama.cpp + sd.cpp + ggml + Crow + asio…）加上 CUDA 的目标文件
# 轻松几个 G，编到一半没空间比编不过还难查。
#
# 架构号 **120**：Blackwell（5090）。CUDA 12.8 是第一个支持 sm_120 的版本，
# 这台正好是 12.8。填 89（Ada）编出来的东西在这张卡上跑不了。
#
# 两份分开编：
#   build-worker —— sd.cpp + CUDA，出图出片
#   build-coord  —— llama.cpp + CUDA，编排 + 进程内配音 + 发 webapp
# 合成一份也行，但分开的好处是重编一份不影响另一份跑着的那个。
set -u
SRC="${1:-/root/changji}"
OUT="${2:-/root/autodl-tmp}"
ARCH="${3:-120}"
LOG="/root/build.log"
exec > >(tee -a "$LOG") 2>&1
say() { echo "[$(date '+%H:%M:%S')] $*"; }
cd "$SRC" || exit 1

# **FetchContent 要从 GitHub 拉 json / toml++ / httplib / asio / Crow /
# sd.cpp / llama.cpp。这台机器直连 GitHub 不通**（git ls-remote 25 秒无响应），
# 克隆会一直挂着，日志里什么都不打——看起来像"编译很慢"。
# AutoDL 自带学术加速，开了就通。**它会让 pip / apt 变慢**，所以只在
# 构建这一段开，下模型那个脚本不要 source 它（hf-mirror 本来就是国内的）。
if [ -f /etc/network_turbo ]; then
    . /etc/network_turbo >/dev/null 2>&1
    echo "[$(date '+%H:%M:%S')] 学术加速已开（GitHub 直连不通）"
fi

# **协调者那份要一个 Python**：CMakeLists 里 find_package(Python3 REQUIRED)，
# 用来给 llama.cpp 打 FP8 补丁。找不到就当场停（那是故意的，见那行上面的
# 注释：配置期悄悄过的话，编译期会报一堆"找不到 GGML_TYPE_F8_E4M3"，
# 那时候没人会想到是这儿）。
# 这台机器上 python 在 miniconda 里、没进 PATH，显式指给 CMake。
PY3=""
for c in "$(command -v python3)" /root/miniconda3/bin/python3 /opt/conda/bin/python3; do
    [ -x "$c" ] && { PY3="-DPython3_EXECUTABLE=$c"; break; }
done
[ -n "$PY3" ] && echo "Python：${PY3#-DPython3_EXECUTABLE=}" || echo "⚠ 找不到 Python，协调者那份会配置失败"

COMMON="$PY3 -G Ninja -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
        -DCHANGJI_CUDA_ARCH=$ARCH -DCMAKE_CUDA_ARCHITECTURES=$ARCH
        -DCHANGJI_BUILD_TESTS=OFF"

say "== 先编测试目标（纯 CPU，最快能发现编译器问题） =="
# g++ 11.4 比上一台（13.3）老，C++20 有些地方可能不认。先用不带 CUDA 的
# 测试目标探一下——它编得过，说明我们自己的代码没问题，剩下的锅归 CUDA。
cmake -S cpp -B $OUT/build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCHANGJI_SD=OFF -DCHANGJI_LLAMA=OFF >/tmp/cfg_tests.log 2>&1
say "配置 $?"
cmake --build $OUT/build-tests --target changji_tests -j 64 2>&1 \
  | grep -E "error:|Error " | head -10
say "构建 ${PIPESTATUS[0]}"
if [ -x $OUT/build-tests/changji_tests ]; then
  $OUT/build-tests/changji_tests 2>&1 | tail -4
fi

say "== 工作进程（sd.cpp + CUDA） =="
cmake -S cpp -B $OUT/build-worker $COMMON \
      -DCHANGJI_SD=ON -DCHANGJI_SD_CUDA=ON -DCHANGJI_LLAMA=OFF \
      >/tmp/cfg_worker.log 2>&1
say "配置 $?"; tail -3 /tmp/cfg_worker.log
cmake --build $OUT/build-worker --target changji -j 64 2>&1 \
  | grep -E "error:|Error " | head -10
say "构建 ${PIPESTATUS[0]}"

say "== 协调者（llama.cpp + CUDA，进程内配音） =="
cmake -S cpp -B $OUT/build-coord $COMMON \
      -DCHANGJI_SD=OFF -DCHANGJI_LLAMA=ON -DGGML_CUDA=ON \
      >/tmp/cfg_coord.log 2>&1
say "配置 $?"; tail -3 /tmp/cfg_coord.log
cmake --build $OUT/build-coord --target changji -j 64 2>&1 \
  | grep -E "error:|Error " | head -10
say "构建 ${PIPESTATUS[0]}"

ls -la $OUT/build-worker/changji $OUT/build-coord/changji 2>&1
df -h "$OUT" | tail -1
say "== 编完 =="
