#!/bin/sh
# 多卡机器上机第一件事：把三件没法在单卡上验的事验掉。
#
# 跑法（在服务器上）：
#     sh cpp/tools/gpu_preflight.sh
#
# **为什么要有这个脚本。** 这三件事在开发机（单卡 RTX 2060）上一行都跑不到，
# 而其中第一件错了的表现是"看着在并行，实际在排队，还不报错"——
# 后面所有并行收益都建立在它上面。不先验，后面测出来的吞吐数字全是假的。

set -e

echo "==================== 一、有几张卡 ===================="
nvidia-smi --query-gpu=index,name,memory.total,driver_version \
           --format=csv,noheader,nounits
COUNT=$(nvidia-smi --query-gpu=name --format=csv,noheader | grep -c .)
echo "  -> 数出来 $COUNT 张"
echo
echo "  这个数要和 parse_gpu_query 数出来的一致。单卡机器上那段逻辑"
echo "  一行都跑不到，只在用例里拿假输出验过。"

echo
echo "==================== 二、CUDA_VISIBLE_DEVICES 到底管不管用 ===================="
echo
echo "  **这是地基。** 工作进程靠它绑卡（worker_server.cpp 里那句 set_env）。"
echo "  要是对 ggml 的 CUDA 后端不生效，八个进程会全挤在卡 0 上——"
echo "  看着在并行，实际在排队，而且一声不吭。"
echo
echo "  下面每一行应该只看得见一张卡，而且是不同的那张："
i=0
while [ "$i" -lt "$COUNT" ] && [ "$i" -lt 8 ]; do
    SEEN=$(CUDA_VISIBLE_DEVICES=$i nvidia-smi --query-gpu=index,name \
           --format=csv,noheader 2>/dev/null | tr '\n' ' ')
    echo "    CUDA_VISIBLE_DEVICES=$i  ->  $SEEN"
    i=$((i + 1))
done
echo
echo "  ⚠️ **上面这个测试是没用的**：nvidia-smi 按设计就不认这个变量"
echo "     （它是管理工具不是 CUDA 应用，要过滤得用 -i）。实测三个值"
echo "     看到的 UUID 一模一样，差点判成'不生效'。"
echo "     真判据要拿一个 CUDA 程序问，见下面第五节。"
echo "     真正的判据是：起两个工作进程绑不同的卡，各派一个任务，"
echo "     跑的时候 nvidia-smi 上两张卡都该有显存占用。见脚本末尾那段。"

echo
echo "==================== 三、编译链和 CUDA ===================="
echo "  nvcc:"
nvcc --version 2>/dev/null | tail -2 | sed 's/^/    /' || echo "    没有 nvcc"
echo "  gcc:  $(gcc --version 2>/dev/null | head -1 || echo 没有)"
echo "  cmake:$(cmake --version 2>/dev/null | head -1 || echo ' 没有')"
echo "  ninja:$(ninja --version 2>/dev/null || echo ' 没有')"
echo
echo "  编的时候必须显式给架构，L20 是 Ada = 89："
echo "    cmake -S cpp -B build -DCHANGJI_SD_CUDA=ON -DCHANGJI_CUDA_ARCH=89"
echo "  不给的话默认值可能不含 89，CUDA 退回 PTX JIT——**不报错**，"
echo "  但每个内核第一次跑都要现编，表现是'能跑但莫名其妙地慢'。"

echo
echo "==================== 四、磁盘和内存 ===================="
df -h / | tail -1 | awk '{print "  根分区: "$2" 总, "$4" 可用"}'
free -g 2>/dev/null | awk 'NR==2 {print "  内存: "$2" GB 总, "$7" GB 可用"}'
echo
echo "  全尺寸档权重 70.5 GB（cpp/tools/download_models.ps1 -Preset full 里那八个）。"
echo "  加构建和产物，250 GB 舒服，160 GB 是下限。"
echo
echo "  **内存这一项要留意**：真上 bf16 全尺寸不砍编码器的话，"
echo "  每个进程 53.5 GB 常驻内存 × 8 = 428 GB。配不够会 swap 到死。"
echo "  砍成 fp8 编码器（47 GB/卡）就不用 offload，也就不吃这么多内存。"

echo
echo "==================== 接下来 ===================="
cat <<'NEXT'
  1. 装编译链（缺什么装什么）
  2. 拉代码，-DCHANGJI_SD_CUDA=ON -DCHANGJI_CUDA_ARCH=89 编一遍
     （Linux 上第一次带 CUDA 编，可能还有漏 include 的，
       和 subtitles.cpp 那个 <cmath> 一样——MSVC 惯出来的）
  3. 下全尺寸档
  4. **绑卡的真判据**：起两个工作进程
        ./changji --worker --gpu 0 --port 9001 &
        ./changji --worker --gpu 1 --port 9002 &
     各派一个出图任务，跑的时候看
        nvidia-smi --query-gpu=index,memory.used --format=csv
     **两张卡都该有占用**。只有卡 0 有 = CUDA_VISIBLE_DEVICES 对 ggml 没生效，
     后面的并行全是假的。
  5. 补今天欠的：工作进程的产物和进程内的**逐字节比**
     （单卡上出一张图 20 分钟，跑两遍太久，一直没验）
NEXT
