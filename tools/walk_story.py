#!/usr/bin/env python3
"""从零走一遍写作这条链，一步一步查。

    建项目 → 出大纲 → 采用 → 一章一章写 → 读故事 → 采用 → 章节计划 → 落成章节

**为什么要有这个。** 这条链上好几处坏了是不报错的：接口回 200、界面
不红、文件也落了盘，只是里面少了东西。2026-09-11 一天里撞见三处：

  * 「读故事」的提示词是整本正文，超过 n_batch 直接 abort()——这个功能
    大概从来没成功过一次，而失败的样子是整个服务没了，不是一句报错。
  * `locations` 和 `relations` 没写进 schema 的 required，于是从大纲到
    分析全程是空数组。设定页的「场景」那一格永远空着，而**谁也不会觉得
    那是 bug**——看着就像"这个故事没什么场景"。
  * 每章的出场人物是 `['陈默','林景明','陈默','林景明','苏婉']`：大纲写
    过一份，分析又往上堆了一份。

单元测试抓不到这些：它们都要真的大模型、真的一万多字提示词、真的走完
前一步才暴露。所以有了这个脚本——**它查的不是状态码，是内容**。

用法：

    python cpp/tools/walk_story.py                  # 默认打本机 8080
    python cpp/tools/walk_story.py --base http://x:8080 --name walk_1
    python cpp/tools/walk_story.py --keep           # 跑完不删项目

跑完不为零就是有问题；每一条不对的都会打出来。全程要十来分钟（大头是
一章一两分钟的正文）。
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_PREMISE = (
    "外卖员在暴雨夜送错一单，收件人是三年前把他开除的前老板，"
    "而那份餐里夹着一张不属于任何人的病历单。"
)

problems = []


def log(*a):
    print(*a, flush=True)


def bad(what):
    """记一条不对的。**不当场退出**——后面的步骤往往能顺带说明原因。"""
    problems.append(what)
    log("    ✗", what)


def call(base, path, payload=None, query=None, cap=1800):
    url = base + path
    if query:
        url += "?" + urllib.parse.urlencode(query)
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST" if data is not None else "GET",
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=cap) as r:
            return r.status, json.loads(r.read().decode("utf-8")), time.time() - t0
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        try:
            body = json.loads(body)
        except ValueError:
            pass
        return e.code, body, time.time() - t0
    except Exception as e:  # 连不上、超时、对面没了
        return 0, {"detail": "%s: %s" % (type(e).__name__, e)}, time.time() - t0


def step(base, name, path, payload=None, query=None, cap=1800):
    code, body, took = call(base, path, payload, query, cap)
    ok = 200 <= code < 300
    log("%s %-12s %3s  %6.1fs" % ("✓" if ok else "✗", name, code, took))
    if not ok:
        detail = body.get("detail") if isinstance(body, dict) else body
        log("   ", json.dumps(detail, ensure_ascii=False)[:800])
        # 这一步都没过，后面全是连锁反应，没有继续的意义
        log("\n走不下去了。")
        sys.exit(1)
    return body


def check_story_shape(story, who, want_chapters=True, per_chapter=True):
    """一份故事该有的东西。**这几项都曾经悄悄是空的。**"""
    chs = story.get("chapters") or []
    if want_chapters and not chs:
        bad(f"{who}：一章都没有")
    if not (story.get("locations") or []):
        bad(f"{who}：一个场景都没有——设定页的「场景」那一格会是空的，"
            f"空景图无从谈起，排分镜时也不知道戏发生在哪儿")
    if not (story.get("relations") or []):
        bad(f"{who}：一条人物关系都没有——定妆时"
            f"「想复仇的人」和「想赎罪的人」眼神不一样，靠的就是它")

    # 每章的名单**只查「读故事」那一份**：大纲阶段那两项是可选的，
    # 逼它写会把生成拖到 token 上限（试过，出一份大纲从 35 秒变 278 秒，
    # 最后截断在半截 JSON 上）。照着正文读出来的那份才准。
    if not per_chapter:
        return
    loc_names = {l.get("name") for l in (story.get("locations") or [])}
    char_names = {c.get("name") for c in (story.get("characters") or [])}
    for c in chs:
        cid = c.get("chapter_id")
        for field, known in (("characters", char_names), ("locations", loc_names)):
            got = c.get(field) or []
            if not got:
                bad(f"{who} {cid}：{field} 是空的")
                continue
            if len(got) != len(set(got)):
                bad(f"{who} {cid}：{field} 里有重名 {got}")
            unknown = [n for n in got if n not in known]
            if unknown:
                bad(f"{who} {cid}：{field} 里有没登记过的名字 {unknown}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="http://localhost:8080")
    ap.add_argument("--name", default=None, help="项目名，默认按时间起一个")
    ap.add_argument("--premise", default=DEFAULT_PREMISE)
    ap.add_argument("--keep", action="store_true", help="跑完不删项目")
    args = ap.parse_args()

    base = args.base.rstrip("/")
    name = args.name or "walk_" + time.strftime("%m%d_%H%M%S")
    log("项目:", name, " 引擎:", base)
    t_all = time.time()

    made = step(base, "建项目", "/api/new", {"path": name, "style_line": "realistic"})
    proj = made.get("root") or made.get("path")
    log("   ->", proj)

    draft = step(base, "出大纲", "/api/story/outline",
                 {"project": proj, "premise": args.premise, "scale": "short"})
    story = draft["story"]
    log("   -> %d 章 / %d 人 / %d 场景 / %d 关系"
        % (len(story.get("chapters") or []), len(story.get("characters") or []),
           len(story.get("locations") or []), len(story.get("relations") or [])))
    check_story_shape(story, "大纲", per_chapter=False)

    step(base, "采用大纲", "/api/story/adopt", {"project": proj, "story": story})

    target = None
    for c in story.get("chapters") or []:
        cid = c["chapter_id"]
        got = step(base, "写 " + cid, "/api/story/chapter",
                   {"project": proj, "chapter_id": cid, "overwrite": True})
        chars = got.get("chars") or 0
        target = got.get("target_chars") or target
        log("   ->", chars, "字")
        # **字数是这一步唯一能自动判的质量。** 模型很会"写个梗概交差"：
        # 2026-09-11 实测三章里两章只写了一百来字，而接口一样回 200。
        if target and chars < target * 0.5:
            bad(f"{cid}：只写了 {chars} 字，目标 {target}——多半是拿梗概交了差")

    an = step(base, "读故事", "/api/story/analyze", {"project": proj})
    st2 = an["story"]
    log("   -> %d 人 / %d 场景 / %d 关系"
        % (len(st2.get("characters") or []), len(st2.get("locations") or []),
           len(st2.get("relations") or [])))
    for c in st2.get("chapters") or []:
        log("      ", c["chapter_id"], "人物", c.get("characters"),
            "地点", c.get("locations"))
    check_story_shape(st2, "读故事")
    # 读一遍正文不该把大纲已经有的东西读没了
    if len(st2.get("relations") or []) < len(story.get("relations") or []):
        bad("读故事之后关系变少了——采用那一步会拿它盖掉大纲写好的那几条")

    step(base, "采用分析", "/api/story/adopt",
         {"project": proj, "story": st2, "overwrite": True})

    plan = step(base, "章节计划", "/api/story/plan", {"project": proj})
    eps = plan.get("story", {}).get("plan") or []
    log("   ->", len(eps), "章")
    if len(eps) < len(st2.get("chapters") or []):
        bad("章节计划比章还少——一章一条，一条都不能漏")
    no_hook = [e.get("episode_id") for e in eps if not (e.get("hook") or "").strip()]
    # 钩子取的是这一章最后一场的 turn。最后一章停在全片结尾，没有钩子是正常
    # 的；中间那几章缺，就是那一场的 turn 根本没写出来
    if len(no_hook) > 1:
        bad(f"{len(no_hook)} 章没有钩子，只有最后一章该没有：{no_hook}")

    made_eps = step(base, "落成章节", "/api/story/episodes", {"project": proj})
    log("   -> 新建", len(made_eps.get("created") or []),
        "更新", len(made_eps.get("updated") or []))
    if len(made_eps.get("created") or []) != len(eps):
        bad("落成的章数和章节计划对不上")

    if not args.keep:
        # confirm_name 要和目录名一字不差——那道闸就是防手滑的
        step(base, "删项目", "/api/project/delete",
             {"path": proj, "confirm_name": name})

    log("\n用了 %.0f 秒。" % (time.time() - t_all))
    if problems:
        log("有 %d 处不对：" % len(problems))
        for p in problems:
            log("  -", p)
        sys.exit(1)
    log("全过了。" + ("项目留在 " + proj if args.keep else ""))


if __name__ == "__main__":
    main()
