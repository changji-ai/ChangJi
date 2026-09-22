"""从 Python 的 unicodedata 里导出「东亚宽度是宽的」那些码点区间。

跑法（在 changji/ 下）：
    python cpp/tools/gen_eaw.py

产出：
    cpp/src/media/east_asian_width.inc.hpp

**为什么要生成而不是手写一张表。** 字幕断行按显示宽度算，全角一格半角半格，
判断依据是 `unicodedata.east_asian_width(ch) in "WF"`。手写区间必然和
Python 的表有出入——出入的表现不是报错，是某几句字幕断行位置和 Python 不一样，
而那要逐帧比对成片才看得出来。

扫全部码点由 Python 自己回答，两边就**按构造一致**，不是靠我记得准。
Unicode 版本变了重跑一次即可（顺带会看到区间数变化，那本身就是个信号）。
"""

from __future__ import annotations

import io
import sys
import unicodedata
from pathlib import Path

# **Windows 上标准输出默认不是 UTF-8。**
#
# GitHub 的 windows runner 跑 Python 时控制台编码是 cp1252，而下面那些进度
# 是中文——print 到一半直接 UnicodeEncodeError，脚本非零退出。CMake 那边
# 报的是"patch step 失败"、MSBuild 报 MSB8066，**和真正的原因（编码）
# 差着十万八千里**，日志里要翻到最底下才看得见那行 UnicodeEncodeError。
# 2026-09-11 六平台流水线头一次跑，windows-x64 和 windows-arm64 两格就
# 挂在这儿，另外四个平台全过。
#
# errors="replace"：宁可某个字打成问号，也不能因为一个字让整个构建挂掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # 老 Python 或者被重定向成不支持的对象
        pass


HERE = Path(__file__).resolve()
CPP = HERE.parent.parent
OUT = CPP / "src" / "media" / "east_asian_width.inc.hpp"

MAX_CP = 0x110000


def main() -> int:
    wide: list[tuple[int, int]] = []
    start = -1
    for cp in range(MAX_CP):
        try:
            ch = chr(cp)
        except ValueError:  # pragma: no cover
            ch = None
        is_wide = ch is not None and unicodedata.east_asian_width(ch) in ("W", "F")
        if is_wide and start < 0:
            start = cp
        elif not is_wide and start >= 0:
            wide.append((start, cp - 1))
            start = -1
    if start >= 0:
        wide.append((start, MAX_CP - 1))

    covered = sum(hi - lo + 1 for lo, hi in wide)
    parts = [
        "// 由 cpp/tools/gen_eaw.py 从 Python 的 unicodedata 导出，别手改。",
        "//",
        f"// Unicode {unicodedata.unidata_version}，"
        f"{len(wide)} 个区间，覆盖 {covered} 个码点。",
        "//",
        "// 判据是 east_asian_width 为 W 或 F（宽、全角）。",
        "// 字幕断行按它算显示宽度：宽的算一格，其余算半格。",
        "",
        "// clang-format off",
        "inline constexpr struct { char32_t lo; char32_t hi; } kWideRanges[] = {",
    ]
    for lo, hi in wide:
        parts.append(f"    {{0x{lo:04X}, 0x{hi:04X}}},")
    parts.append("};")
    parts.append("// clang-format on")
    parts.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(parts))
    print(f"Unicode {unicodedata.unidata_version}：{len(wide)} 个区间 -> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
