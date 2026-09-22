#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用 ACE-Step 1.5 出一条配乐，给 changji 的 [sound].music_command 用。

    python music_ace_step.py --prompt "cinematic instrumental ..." --seconds 62 --out /path/ep01_music.wav

引擎那边只约定三个占位符（{prompt} {seconds} {out}），别的都在这儿：

  * ACE-Step 装在哪：环境变量 ACESTEP_HOME 指向 ACE-Step-1.5 的仓库目录
    （`uv sync` 装好的那个），本脚本把它加进 sys.path；也可以直接用那个
    仓库的 python 来跑本脚本，那就不用设。
  * 权重在哪：ACESTEP_CHECKPOINTS（默认 $ACESTEP_HOME/checkpoints），
    第一次跑会自己下。
  * 用哪个模型：ACESTEP_CONFIG（默认 acestep-v15-turbo，最快，出配乐够用）。

输出一律 wav（ffmpeg 转，PATH 上要有 ffmpeg；引擎本来就要它）。
退出码非零 = 失败，stderr 里说原因——引擎会把最后几行带进「这一章没有配乐」
那句里。

API 按 ACE-Step-1.5 的 docs/en/INFERENCE.md（2026-09）写的：
    from acestep.handler import AceStepHandler
    from acestep.llm_inference import LLMHandler
    from acestep.inference import GenerationParams, GenerationConfig, generate_music
上游改了签名的话这里跟着改；报错会原样打出来。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def die(msg: str, code: int = 2) -> None:
    sys.stderr.write(msg.rstrip() + "\n")
    sys.exit(code)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prompt", required=True, help="配乐描述（英文标签为主）")
    ap.add_argument("--seconds", type=float, required=True, help="时长，秒")
    ap.add_argument("--out", required=True, help="输出 wav 路径")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    home = os.environ.get("ACESTEP_HOME", "")
    if home and home not in sys.path:
        sys.path.insert(0, home)
    checkpoints = os.environ.get(
        "ACESTEP_CHECKPOINTS", os.path.join(home or ".", "checkpoints"))
    config_name = os.environ.get("ACESTEP_CONFIG", "acestep-v15-turbo")

    try:
        from acestep.handler import AceStepHandler  # type: ignore
        from acestep.llm_inference import LLMHandler  # type: ignore
        from acestep.inference import (  # type: ignore
            GenerationConfig, GenerationParams, generate_music)
    except Exception as e:  # noqa: BLE001
        die("导不进 acestep：%s\n"
            "设 ACESTEP_HOME 指向 ACE-Step-1.5 的仓库目录，或者用那个仓库的 "
            "python 跑本脚本" % e)

    seconds = max(5.0, min(float(args.seconds), 240.0))
    out_dir = tempfile.mkdtemp(prefix="changji_music_")
    try:
        dit = AceStepHandler()
        llm = LLMHandler()
        dit.initialize_service(project_root=home or os.getcwd(),
                               config_path=config_name, device="cuda")
        # 5Hz 的小语言模型是可选的（它管歌词对齐）；纯器乐不需要，
        # 装不上也别拦着。
        try:
            llm.initialize(checkpoint_dir=checkpoints,
                           lm_model_path="acestep-5Hz-lm-0.6B",
                           backend="pt", device="cuda")
        except Exception as e:  # noqa: BLE001
            sys.stderr.write("语言模型没起来（纯器乐不需要它）：%s\n" % e)
            llm = None

        params = GenerationParams(
            caption=args.prompt,
            lyrics="[Instrumental]",
            duration=seconds,
            instrumental=True,
            seed=args.seed,
        )
        config = GenerationConfig(batch_size=1, audio_format="wav")
        result = generate_music(dit, llm, params, config, save_dir=out_dir)
        if not getattr(result, "success", False) or not result.audios:
            die("ACE-Step 没出音频：%s" % getattr(result, "error", "（没说原因）"))
        produced = result.audios[0]["path"]
    except SystemExit:
        raise
    except Exception as e:  # noqa: BLE001
        die("ACE-Step 出错：%r" % (e,))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    if produced.lower().endswith(".wav"):
        shutil.move(produced, args.out)
    else:
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            die("出的是 %s，转 wav 要 ffmpeg，PATH 上没有" % produced)
        r = subprocess.run([ffmpeg, "-y", "-i", produced, "-ar", "48000", "-ac", "2",
                            args.out], capture_output=True, text=True)
        if r.returncode != 0:
            die("ffmpeg 转 wav 失败：%s" % r.stderr[-1200:])
    shutil.rmtree(out_dir, ignore_errors=True)
    if not os.path.isfile(args.out) or os.path.getsize(args.out) == 0:
        die("没有写出 %s" % args.out)
    print(args.out)


if __name__ == "__main__":
    main()
