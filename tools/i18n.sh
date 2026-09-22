#!/usr/bin/env bash
# 把界面上那些话抽出来（`.ts`），再编成运行时读的那份（`.qm`）。
#
#     sh cpp/tools/i18n.sh            # 两件都做
#     sh cpp/tools/i18n.sh update     # 只抽（源码改了之后）
#     sh cpp/tools/i18n.sh release    # 只编（翻译改了之后）
#
# ---
#
# **为什么不用 `qt_add_translations`。** Qt 自带那个 CMake 宏在**路径里有
# 空格**时会当场断掉：
#
#     file failed to open for writing (Operation not permitted):
#       /Volumes/9100
#
# ——它把 `/Volumes/9100 PRO/…` 截在空格那儿了（Qt 6.11 的
# `_qt_internal_ensure_ts_file` 没给路径加引号）。这个仓库正好住在那样一条
# 路径上。`lupdate` / `lrelease` 两个程序自己**没有**这个毛病，所以绕开那个
# 宏、直接调它们。
#
# **`.qm` 跟着进版本库。** 这样构建期一个 qttools 都不用装——CI 上少一个
# 模块、少一处"忘了装就配置失败"（WebSockets 那一条刚栽过）。同
# `bundled_webapp.inc.hpp` 那条先例：生成物进库，换来的是构建不依赖工具链。
#
# ⚠️ **改完界面上的话，记得跑一次 `update`**，不然新加的那几句在别的语言里
# 是空的——而**空的那几句会原样显示中文**，不报错。

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DESKTOP="$HERE/../../desktop"
I18N="$DESKTOP/i18n"

# Qt 的 bin 不一定在 PATH 里（macOS 上 brew 装的那份就不在）。
find_tool() {
    command -v "$1" 2>/dev/null && return 0
    for p in /opt/homebrew/opt/qt/bin "$QT_ROOT_DIR/bin" "$(qmake6 -query QT_INSTALL_BINS 2>/dev/null || true)"; do
        [ -x "$p/$1" ] && { echo "$p/$1"; return 0; }
    done
    return 1
}
LUPDATE="$(find_tool lupdate || true)"
LRELEASE="$(find_tool lrelease || true)"

LANGS="en zh_TW ja ko es fr de pt_BR ru ar hi"
WHAT="${1:-all}"

if [ "$WHAT" = "all" ] || [ "$WHAT" = "update" ]; then
    [ -n "$LUPDATE" ] || { echo "找不到 lupdate（装 Qt 的 qttools）"; exit 1; }
    for lang in $LANGS; do
        # `-no-obsolete`：删掉源码里已经没有的那几句。留着的话文件越攒越大，
        # 而且翻译的人分不清哪几句还算数。
        "$LUPDATE" -recursive "$DESKTOP" -ts "$I18N/changji_$lang.ts" -no-obsolete \
            | sed "s/^/  [$lang] /"
    done
fi

if [ "$WHAT" = "all" ] || [ "$WHAT" = "release" ]; then
    [ -n "$LRELEASE" ] || { echo "找不到 lrelease（装 Qt 的 qttools）"; exit 1; }
    for lang in $LANGS; do
        "$LRELEASE" "$I18N/changji_$lang.ts" -qm "$I18N/changji_$lang.qm" \
            | sed "s/^/  [$lang] /"
    done
fi

echo
echo "== 各家翻了多少 =="
for lang in $LANGS; do
    total=$(grep -c "<source>" "$I18N/changji_$lang.ts" || echo 0)
    # 没翻的那几句带着 `type="unfinished"`。
    todo=$(grep -c 'type="unfinished"' "$I18N/changji_$lang.ts" || true)
    todo=${todo:-0}
    done_n=$((total - todo))
    printf "  %-6s %3d / %3d\n" "$lang" "$done_n" "$total"
done
