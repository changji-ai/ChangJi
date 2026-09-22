#!/usr/bin/env python3
"""把 leejet 的 ggml 扩展打到 llama.cpp 自带的那份 ggml 上。

原来这一套是 README 里的**手工步骤**（git apply --reject、跑冲突解决脚本、
删 .rej）。手工步骤在 verify/ 那个一次性的验证工程里够用，但主工程要靠
FetchContent 自动拉源码，没人会在中间插一次手。这个脚本把那三步包起来，
给 CMake 的 PATCH_COMMAND 用。

**必须幂等。** FetchContent 在什么情况下重跑 PATCH_COMMAND 并不好预测
（改了 GIT_TAG、清了 _deps、换了生成器都可能），而 `git apply` 打第二遍
会失败、冲突解决脚本打第二遍会把三个成员加两次。所以先看戳文件，
打完再写；已经打过就直接退出。

用法：

    python apply_to_llamacpp.py <llama.cpp 源码目录>

在那个目录下要能看到 ggml/ 子目录（llama.cpp 是 vendor 进去的，不是子模块）。
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

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


HERE = pathlib.Path(__file__).resolve().parent
PATCH = HERE / "leejet-ggml-extensions.patch"
RESOLVE = HERE / "resolve_llamacpp_conflicts.py"
STAMP = ".changji-ggml-patched"


def main() -> int:
    if len(sys.argv) < 2:
        print("要给 llama.cpp 的源码目录", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()

    ggml = root / "ggml"
    if not ggml.is_dir():
        print(f"{root} 下没有 ggml/：这不像 llama.cpp 的源码树", file=sys.stderr)
        return 2

    stamp = ggml / STAMP
    if stamp.is_file():
        print(f"[ggml 补丁] 已经打过了（{stamp.name}），跳过")
        return 0

    if not PATCH.is_file():
        print(f"找不到补丁 {PATCH}", file=sys.stderr)
        return 2

    # git apply --reject：73/79 个 hunk 直接落，剩下 6 个留 .rej，
    # 由下面那个脚本按锚点手工解。**返回码非零是预期的**，
    # 有 .rej 就会非零——所以不能用 check=True。
    print("[ggml 补丁] git apply --reject …")
    # **输出必须收进来，不能让它直接淌到构建工具眼前。**
    #
    # git apply --reject 会把"这个 hunk 没打上"写到 stderr，措辞是
    # `error: patch failed: ggml/include/ggml-rpc.h:8`。那 6 条是**预期的**
    # （下面有脚本按锚点手工解），可 MSBuild 会照错误格式去解析工具的
    # stderr，于是把它们记成 CUSTOMBUILD : error，整个自定义生成步骤判失败、
    # 退出码 1 —— 而这个脚本明明 return 0。
    # Linux/macOS 走 Make/Ninja，stderr 不会变成构建错误，所以只有
    # Windows 那两格挂。2026-09-11 六平台流水线卡在这儿。
    applied = subprocess.run(
        ["git", "apply", "--reject", "--directory=ggml", str(PATCH)],
        cwd=root, check=False,
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    # 还是要能看，只是换成普通输出、并且不带 "error:" 那个前缀，
    # 免得下一个构建工具又把它当成错误。查问题时这几行是关键。
    for line in (applied.stderr or "").splitlines():
        print("    | " + line.replace("error:", "note:"))
    print(f"[ggml 补丁] git apply 退出码 {applied.returncode}"
          f"（有 .rej 时非零是正常的）")

    print("[ggml 补丁] 解那 6 个冲突 …")
    done = subprocess.run([sys.executable, str(RESOLVE)], cwd=root, check=False)
    if done.returncode != 0:
        # 冲突解决脚本每一步都断言锚点唯一存在。它失败通常意味着
        # **上游把锚点挪走了**，那时候必须停下来重新对齐，
        # 而不是留一棵打了一半的源码树继续编——那会编出很难解释的错误。
        print("\n[ggml 补丁] 冲突解决脚本失败了。这一般意味着上游动了那几处代码，\n"
              "            补丁集要重新对齐。**不要绕过这一步继续编。**\n"
              "            对齐办法见 patches/README.md。", file=sys.stderr)
        return 1

    rejects = list(ggml.rglob("*.rej"))
    for r in rejects:
        r.unlink()
    print(f"[ggml 补丁] 清掉 {len(rejects)} 个 .rej")

    stamp.write_text("changji: leejet ggml extensions applied\n", encoding="utf-8")
    print("[ggml 补丁] 好了")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
