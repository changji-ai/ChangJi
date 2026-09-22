r"""把打包好的 webapp 嵌进二进制。

跑法（在 changji/ 目录下）：
    cd webapp/client && npm run build && cd ../..
    python cpp/tools/gen_webapp.py

产出：
    cpp/src/http/bundled_webapp.inc.hpp

**为什么嵌进去而不是随程序放一个目录。**
"单一二进制、零运行时依赖"是这个后端的立项理由之一。带着一个 `webapp/` 目录走，迟早出现"exe 拷过去了、目录忘了拷"，
而表现是**打开端口一片空白**——比报错更难查，因为服务本身是活的、
接口也都好使。

整个 dist 才 393 KB（28 个文件，全是 html/css/js），嵌进去这点代价很划算。

**用分段的原始字符串，不用字节数组。** MSVC 的字符串字面量有 65535 字节上限，
所以每段切到 16 KB 以下再靠相邻字面量自动拼接。字节数组没有这个限制，
但几十万个元素的初始化列表会让编译慢得离谱。
分隔符用 `CJWEBAPP`，生成前会检查它没在内容里出现过——撞上了就是一个
编不过的文件，而错误信息会指向这个 .inc.hpp 的某一行，很难联想到是这里。
"""
from __future__ import annotations

import io
import sys
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
CPP = HERE.parents[1]
REPO = CPP.parent
DIST = REPO / "webapp" / "client" / "dist"
DEST = CPP / "src" / "http" / "bundled_webapp.inc.hpp"

DELIM = "CJWEBAPP"
CHUNK = 16000   # 单条字面量的上限约 16 KB（MSVC 的 C2026），留余量


def split_utf8(data: bytes, limit: int) -> list[bytes]:
    """按**字节**切，切点落在 UTF-8 的字符边界上。

    按字符切是不行的：界面里有中文，一个字符最多 4 字节，
    按字符切 60000 个最坏是 240 KB，照样超 MSVC 那个上限。
    """
    out = []
    i = 0
    while i < len(data):
        end = min(i + limit, len(data))
        # 往回退到不是续字节（10xxxxxx）的地方
        while end > i and end < len(data) and (data[end] & 0xC0) == 0x80:
            end -= 1
        out.append(data[i:end])
        i = end
    return out or [b""]


def emit_chunks(rel: str, data: bytes) -> list[str]:
    """一个文件切成若干条记录，每条一段。

    **不靠相邻字面量拼接**：MSVC 的 65535 上限管的是拼完之后的整体，
    拼接省不掉。所以每段单独进表，运行时再按文件名拼回去。
    """
    lines = []
    for piece in split_utf8(data, CHUNK):
        text = piece.decode("utf-8")
        if f'){DELIM}"' in text:
            raise SystemExit(f"内容里出现了分隔符 ){DELIM}\"，换一个再来")
        lines.append(f'    {{"{rel}", R"{DELIM}({text}){DELIM}"}},')
    return lines


def main() -> int:
    if not DIST.is_dir():
        print(f"找不到 {DIST}", file=sys.stderr)
        print("先在 webapp/client 里跑 npm run build", file=sys.stderr)
        return 1

    files = sorted(p for p in DIST.rglob("*") if p.is_file())
    if not files:
        print(f"{DIST} 是空的", file=sys.stderr)
        return 1

    parts = [
        "// 由 cpp/tools/gen_webapp.py 生成，**不要手改**。",
        "// 源头是 webapp/client/dist，改前端之后重新 npm run build 再跑一次这个脚本。",
        "//",
        "// 嵌进来的理由见生成脚本的开头：带一个目录走迟早会漏拷，",
        "// 而漏拷的表现是打开端口一片空白，比报错难查。",
        "#pragma once",
        "",
        "#include <string_view>",
        "#include <utility>",
        "",
        "namespace changji::http {",
        "",
        "/// 打包好的前端，**按段存**。第一项是相对 dist 的路径，第二项是一段内容。",
        "///",
        "/// 同一个文件会占连着的好几条——MSVC 的字符串字面量上限是 65535 字节，",
        "/// 而拼接之后仍算一个字面量，所以省不掉切分。运行时按文件名把连着的",
        "/// 几段拼回去（只拼一次），见 webapp.cpp。",
        "inline constexpr std::pair<std::string_view, std::string_view>",
        "    kBundledWebappChunks[] = {",
    ]

    total = 0
    for p in files:
        rel = p.relative_to(DIST).as_posix()
        data = p.read_bytes()
        total += len(data)
        parts.extend(emit_chunks(rel, data))

    parts += [
        "};",
        "",
        "}  // namespace changji::http",
        "",
    ]

    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write("\n".join(parts))
    print(f"写入 {DEST}")
    print(f"  {len(files)} 个文件，{total/1024:.0f} KB")
    for p in files[:6]:
        print(f"    {p.relative_to(DIST).as_posix()}  {p.stat().st_size/1024:.1f} KB")
    if len(files) > 6:
        print(f"    …… 还有 {len(files)-6} 个")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
