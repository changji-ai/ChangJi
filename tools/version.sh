#!/usr/bin/env sh
# 这一版叫什么。
#
#     sh cpp/tools/version.sh            # → v2.2-7
#
# 规矩（用户 2026-09-21 定的）：
#
#     版本号 = <前缀>-<前缀定下来之后的第几个提交>
#
# 前缀住在 `cpp/CMakeLists.txt` 里那一行 `CHANGJI_VERSION_PREFIX`。
# **一改它，后面那个计数从 0 重新开始**——因为计数是"从写进那个前缀的那一个
# 提交数到现在"，改前缀那一下自己就是第 0 个。
#
# ⚠️ **计数是数出来的，不是编的。** 同一个提交在谁手上算出来都是同一个数：
# 不靠 CI 的 run 编号（那个换条线就不一样，而且永远不会归零），也不靠人手工
# 往上加（那种数迟早和实际对不上）。
#
# ⚠️ **CI 上要 `fetch-depth: 0`。** `actions/checkout` 默认只拉一个提交，
# 那样 `git log` 里根本没有写前缀那一笔——数出来永远是 0，而且一声不响。

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CMAKE="$HERE/../CMakeLists.txt"

PREFIX=$(sed -n 's/^set(CHANGJI_VERSION_PREFIX "\([^"]*\)".*/\1/p' "$CMAKE" | head -1)
[ -n "$PREFIX" ] || { echo "CMakeLists.txt 里找不到 CHANGJI_VERSION_PREFIX" >&2; exit 1; }

# 不在 git 里（下载的 tar 包、CI 上忘了 fetch-depth）就只报前缀，别编一个数。
if ! git -C "$HERE" rev-parse --git-dir >/dev/null 2>&1; then
    echo "$PREFIX"
    exit 0
fi

# 哪一个提交把这个前缀写进来的。`-S` 是"哪一次改动让这个串出现/消失"。
#
# ⚠️ **只认改了那一行的提交（`--diff-filter=M`），整个文件加进来、删掉的不算。**
# 外层仓库里 cpp/ 先是逐文件跟着、2026-09-22 变成 gitlink（文件整个「删掉」）、
# 2026-09-25 又变回逐文件（整个「加回来」）——不筛的话最近那一笔是加回来的那次，
# 计数从 0 重来，版本号往回掉，而且一声不响。一次 M 都没有（前缀从仓库第一天
# 就是这个）时退回**最早**那次加进来的。
S="CHANGJI_VERSION_PREFIX \"$PREFIX\""
BORN=$(git -C "$HERE" log -1 --format=%H --diff-filter=M -S"$S" -- "$CMAKE" 2>/dev/null || true)
if [ -z "$BORN" ]; then
    BORN=$(git -C "$HERE" log --format=%H --diff-filter=A -S"$S" -- "$CMAKE" 2>/dev/null | tail -1 || true)
fi

if [ -z "$BORN" ]; then
    # 找不到（浅克隆、或者这一行还没提交过）：报 0，**但说一句**。
    # 不说的话它和"真的就是第 0 个"长得一模一样。
    echo "$PREFIX-0"
    echo "（数不出来：这个前缀还没提交过，或者是浅克隆——CI 上记得 fetch-depth: 0）" >&2
    exit 0
fi

N=$(git -C "$HERE" rev-list --count "$BORN..HEAD" 2>/dev/null || echo 0)
echo "$PREFIX-$N"
