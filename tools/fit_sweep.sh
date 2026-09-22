#!/usr/bin/env bash
# **十一种语言 × 几屏 × 几种宽度，量一遍字放不放得下。**
#
#     sh cpp/tools/fit_sweep.sh <项目目录> [宽x高 …]
#     sh cpp/tools/fit_sweep.sh ~/…/projects/互联 1240x820 880x780 700x760
#
# 靠的是桌面端自检的第七个口子 `CHANGJI_DESKTOP_FIT=1`（见 desktop/main.cpp
# 的 `scan_overflow`）：画完一帧之后走一遍画面上那棵树，量每一句话放不放
# 得下。判错的（伸出框、装不下又不许截不许折）当场退 1；真截了的只报一声。
#
# ⚠️ **一屏的绝对数没用。** 聊天标题、章名、路径这些**用户自己写的**东西
# 截了是对的，而且每一种语言下都截。有用的是最后那一段差——**中文没截而
# 别的语言截了的**，那才是「宽度是照中文量的」。
#
# ⚠️ **项目要有东西。** 空项目上大半屏是空状态，字本来就少，扫出来一片绿
# 说明不了什么。拿一个有几章、有镜头、有人物、出过片的项目扫。
#
# ⚠️ **这把尺子看不见"被盖住"。** 它量的是"伸没伸出框"，而
# 「被旁边的面板盖住半句」是遮挡——2026-09-22 那两条例子被切就是遮挡，
# **造这把尺子的起因它自己抓不到**。那种还得靠眼睛，见方案里那一条。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# 编出来的桌面端在哪：**构建目录不在仓库里**（在仓库上一层），而这两层
# 关系将来可能变，所以往上找几级，别写死一条。
APP=""
for up in ../.. ../../.. ../../../..; do
    one="$HERE/$up/build/desktop/changji-desktop.app/Contents/MacOS/changji-desktop"
    [ -x "$one" ] && { APP="$one"; break; }
done
[ -n "$APP" ] || { echo "找不到编好的桌面端（在 <构建目录>/desktop 底下）"; exit 1; }
PROJECT="${1:-}"
[ -n "$PROJECT" ] || { echo "用法：sh cpp/tools/fit_sweep.sh <项目目录> [宽x高 …]"; exit 1; }
shift
SIZES=${*:-1240x820}
LANGS="zh_CN en zh_TW ja ko es fr de pt_BR ru ar hi"
SCREENS="story script shots assets film"
OUT="$(mktemp -d)"
# 桌面端记"上次开的是哪个项目"走的是 macOS 自己那套设置，**HOME 顶不掉**。
defaults write "com.changji.场记" lastProject -string "$PROJECT" 2>/dev/null || true

bad=0
for size in $SIZES; do
  for L in $LANGS; do
    for g in $SCREENS set; do
      if [ "$g" = set ]; then
        extra="CHANGJI_DESKTOP_TAP=齿轮 CHANGJI_DESKTOP_TAP_MS=3800"
        ms=7500
      else
        extra="CHANGJI_DESKTOP_SHOT_OPEN=$g"
        ms=5500
      fi
      env CHANGJI_DESKTOP_LANG="$L" CHANGJI_DESKTOP_FIT=1 \
          CHANGJI_DESKTOP_SHOT="$OUT/${size}_${L}_$g.png" CHANGJI_DESKTOP_SHOT_MS=$ms \
          CHANGJI_DESKTOP_SIZE="$size" QML_DISABLE_DISK_CACHE=1 $extra \
          "$APP" > /dev/null 2> "$OUT/${size}_${L}_$g.err" || {
            bad=$((bad+1)); echo "✗ $size $L/$g"
            grep -E '装不下|伸出' "$OUT/${size}_${L}_$g.err" | head -3
          }
    done
  done
  echo "== $size：中文没截、别的语言截了的 =="
  for g in $SCREENS set; do
    zh=$(grep '截了：' "$OUT/${size}_zh_CN_$g.err" 2>/dev/null \
         | sed 's/^.*截了：「//; s/」.*$//' | sort -u)
    for L in $LANGS; do
      [ "$L" = zh_CN ] && continue
      f="$OUT/${size}_${L}_$g.err"; [ -f "$f" ] || continue
      grep '截了：' "$f" | sed 's/^.*截了：「//; s/」.*$//' | sort -u | while read -r one; do
        [ -z "$one" ] && continue
        echo "$zh" | grep -qxF "$one" || echo "  $L/$g：$one"
      done
    done
  done
done
echo "—— 判错的 $bad 趟；图和日志在 $OUT ——"
[ "$bad" -eq 0 ] || exit 1
