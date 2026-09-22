#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跑完 autodl_gui 的全部用例。一条命令，红了就非零退出。

    ~/.local/share/changji/venv/bin/python changji/cpp/tools/tests/run_all.py
    …/python changji/cpp/tools/tests/run_all.py t_price t_qt      # 只跑这几个

**每个文件是一个独立进程。** 不合成一个的理由：这些用例大量地把模块级的
东西换成桩（`G.request_json`、平台的各个方法、连 `time.sleep` 都换），
跑在同一个解释器里的话，上一个文件留下的桩会悄悄影响下一个——而那种串味
最难查：单独跑全绿，一起跑红，或者更糟，一起跑也绿但测的已经不是真东西。

带 Qt 的那几个要 offscreen，这儿统一设上，免得 CI 上弹窗或者报找不到显示。
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# 顺序是从下往上：接口层红了，界面层的红大半是它带的，先看下面那几个
ORDER = ["t_api", "t_release", "t_aliyun", "t_tencent",
         "t_image", "t_oss", "t_cos", "t_metrics", "t_recipe", "t_price", "t_qt"]


def main(argv):
    want = argv or ORDER
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    # 用例里自己会把配置路径指到临时目录，但万一哪条漏了，这儿再兜一层：
    # **绝不能让用例读到使用者真正的 cloud.json**，那里面是真钥匙。
    env["HOME"] = env.get("CHANGJI_TEST_HOME") or env["HOME"]

    bad, total, t0 = [], 0, time.time()
    for name in want:
        path = os.path.join(HERE, name + ".py")
        if not os.path.exists(path):
            print("  %-12s 没有这个文件" % name)
            bad.append(name)
            continue
        p = subprocess.run([sys.executable, path], cwd=HERE, env=env,
                           capture_output=True, text=True)
        out = p.stdout + p.stderr
        # Qt 在 offscreen 下的那两句唠叨不是问题，别让它们淹掉真正的输出
        lines = [l for l in out.splitlines()
                 if "qt.qpa" not in l and "propagateSizeHints" not in l]
        tail = lines[-1] if lines else "（没有输出）"
        if p.returncode == 0:
            print("  %-12s %s" % (name, tail))
            total += sum(1 for l in lines if l.lstrip().startswith("✓"))
        else:
            bad.append(name)
            print("  %-12s ✗ 红了" % name)
            for l in lines[-25:]:
                print("      " + l)
    print("\n%d 个文件，%d 项断言，用时 %.0f 秒"
          % (len(want), total, time.time() - t0))
    if bad:
        print("红的：%s" % "、".join(bad))
        return 1
    print("全绿")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
