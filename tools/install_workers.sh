#!/bin/bash
# 把工作进程装成 systemd 服务，一张卡一个。
#
# 跑法：
#     bash install_workers.sh /root/changji/build-worker/changji [卡数]
#
# **为什么不用 nohup / setsid。** 试过，不可靠：SSH 断开之后进程经常跟着没，
# 而且 8 个进程手工起手工看日志很快就乱。systemd 管重启、管日志、管开机自启，
# 这本来就是它的活。
#
# 端口约定：卡 k → 9001 + k。协调者那边 [workers].endpoints 照这个填。

set -eu
EXE="${1:-/root/changji/build-worker/changji}"
COUNT="${2:-$(nvidia-smi --query-gpu=name --format=csv,noheader | grep -c . || echo 1)}"

[ -x "$EXE" ] || { echo "找不到可执行文件：$EXE" >&2; exit 1; }
echo "可执行文件：$EXE"
echo "卡数：$COUNT"

for i in $(seq 0 $((COUNT - 1))); do
    port=$((9001 + i))
    cat > "/etc/systemd/system/changji-worker@${i}.service" <<UNIT
[Unit]
Description=changji 工作进程（卡 ${i}）
After=network.target

[Service]
Type=simple
# **绑卡靠 CUDA_VISIBLE_DEVICES**，在建任何 ggml 上下文之前生效。
# 实测过：设了之后这个进程只看得见一张卡，而且在它眼里就是设备 0。
Environment=CUDA_VISIBLE_DEVICES=${i}
# **HOME 必须显式给。** systemd 不一定给服务设 HOME，而配置是按
# $HOME/.config/changji/config.toml 找的。少了它 worker 读的是内置默认值，
# 表现是"配置里明明填了 [models].video，worker 却说没配"——
# 实机上就这么撞过一次，八个 worker 同时报同一句莫名其妙的话。
# 好在自检是几毫秒就拒的，八张卡一点算力没浪费。
Environment=HOME=/root
Environment=XDG_CONFIG_HOME=/root/.config
ExecStart=${EXE} --worker --gpu ${i} --port ${port}
Restart=always
RestartSec=3
# 出图出片吃内存（权重常驻内存按需搬进显存），别让 OOM killer 先挑它
OOMScoreAdjust=-500
StandardOutput=append:/var/log/changji-worker-${i}.log
StandardError=append:/var/log/changji-worker-${i}.log

[Install]
WantedBy=multi-user.target
UNIT
done

systemctl daemon-reload
for i in $(seq 0 $((COUNT - 1))); do
    systemctl enable --now "changji-worker@${i}" >/dev/null 2>&1
done

sleep 6
echo
echo "==== 状态 ===="
ok=0
for i in $(seq 0 $((COUNT - 1))); do
    port=$((9001 + i))
    h=$(curl -s --max-time 5 "http://127.0.0.1:${port}/health" || echo "")
    if echo "$h" | grep -q '"ok":true'; then
        echo "  卡 ${i} 端口 ${port}  ✓  $h"
        ok=$((ok + 1))
    else
        echo "  卡 ${i} 端口 ${port}  ✗  $(systemctl is-active changji-worker@${i})"
    fi
done
echo
echo "  ${ok}/${COUNT} 个活着"
echo
echo "协调者配置里这么填："
echo "[workers]"
printf 'endpoints = ['
for i in $(seq 0 $((COUNT - 1))); do
    [ "$i" -gt 0 ] && printf ', '
    printf '"http://127.0.0.1:%d"' $((9001 + i))
done
echo ']'
