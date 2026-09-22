r"""把水印图嵌进二进制。

跑法（在 changji/ 目录下）：
    python3 brand/render.py            # 先出图
    python3 cpp/tools/gen_watermark.py

产出：
    cpp/src/media/bundled_watermark.inc.hpp

**为什么嵌进去而不是放一个文件。** 水印是「产物上必须有」的东西，磁盘上的
文件意味着删掉它就没水印了——那不是个配置项，是个攻击面。嵌进二进制之后，
装配时解到那一轮自己的 .work 目录里喂给 ffmpeg，用完随 .work 一起删。
两张加起来 42 KB，这个代价换"删不掉"很划算。

**图的源头在外层仓库**（brand/png/watermark-*.png，由 brand/render.py 出），
而 cpp/ 是单独对外的那个仓库——所以生成出来的这个头**要提交进 cpp/**，
和 bundled_webapp.inc.hpp 一个道理：那边的源也在外层（webapp/）。
代价是有"谁改了图忘了跑脚本"的窗口；提示词那份之所以走构建期生成
（见 CMakeLists 里 gen_prompts 那一段），是因为它一天改几次，而水印基本不动。
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# Windows 的控制台默认不是 UTF-8，下面的进度是中文——理由同 gen_webapp.py
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = Path(__file__).resolve()
CPP = HERE.parents[1]
REPO = CPP.parent
PNG = REPO / "brand" / "png"
DEST = CPP / "src" / "media" / "bundled_watermark.inc.hpp"
META = PNG / "watermark.json"

LAYOUTS = [("Landscape", "landscape"), ("Portrait", "portrait")]


def as_array(name: str, blob: bytes) -> str:
    # 十进制不带 0x：每个字节最多 4 个字符，比 "0x00," 省三成源文件。
    # 一行 24 个，编辑器里还翻得动。
    rows = []
    for i in range(0, len(blob), 24):
        rows.append("    " + "".join(f"{b}," for b in blob[i:i + 24]))
    return (f"inline constexpr unsigned char {name}Png[] = {{\n"
            + "\n".join(rows) + "\n};\n")


def main() -> int:
    if not META.is_file():
        print(f"找不到 {META}——先跑 python3 brand/render.py", file=sys.stderr)
        return 1
    meta = json.loads(META.read_text(encoding="utf-8"))

    parts = [
        "// 自动生成，别手改。改水印要改 brand/render.py，再跑：\n"
        "//     python3 brand/render.py && python3 cpp/tools/gen_watermark.py\n"
        "//\n"
        "// 灰度 + alpha 的 PNG（墨迹是白的、发光是黑的，RGB 三通道白费），\n"
        "// ffmpeg 读得了。为什么嵌进来见 cpp/tools/gen_watermark.py。\n"
        "#pragma once\n\n"
        "namespace changji::media::bundled {\n\n"
        "/// 图里标那一块的像素高。引擎按它算缩放：目标标高 / 这个数 = 倍率。\n"
        f"inline constexpr int kWatermarkBaseMarkH = {meta['base_mark_h']};\n\n"
    ]
    total = 0
    for sym, key in LAYOUTS:
        info = meta["images"][key]
        blob = (PNG / info["file"]).read_bytes()
        total += len(blob)
        parts.append(f"// {info['file']}  {info['w']}x{info['h']}\n")
        parts.append(as_array("kWatermark" + sym, blob))
        parts.append(f"inline constexpr int kWatermark{sym}W = {info['w']};\n")
        parts.append(f"inline constexpr int kWatermark{sym}H = {info['h']};\n\n")
    parts.append("}  // namespace changji::media::bundled\n")

    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_text("".join(parts), encoding="utf-8")
    print(f"wrote {DEST.relative_to(REPO)}  "
          f"{total / 1024:.1f} KB 图 → {DEST.stat().st_size / 1024:.0f} KB 源文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
