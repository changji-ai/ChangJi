#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""量一遍：引擎里还有多少句给人看的中文没包 `SAY()`。

    python3 cpp/tools/say_scan.py                  # 整个 cpp/src 排个序
    python3 cpp/tools/say_scan.py --all            # 连不翻的那几族一起列
    python3 cpp/tools/say_scan.py cpp/src/http/run.cpp   # 单个文件，逐行列出
    python3 cpp/tools/say_scan.py --missing cpp/src/http/run.cpp
    python3 cpp/tools/say_scan.py --plurals        # 带数的那一族还剩几句
                                                   # 包了 SAY() 而十一份表里还没有的

**这是把尺子，不是闸门。** 它数出来的每一处都还得人看一眼：是界面，
还是日志、是给模型看的提示词、是解析用的词表。判过了的写进下面
`不翻的` 那张表，连理由一起——下一个人才不用重判一遍。

⚠️ 写这把尺子栽过两次，都在"什么算一个字面量"上：

  1. **相邻的字面量 C++ 会粘成一句，注释夹在中间也粘**
     （`"a" /*注*/ "b"` 是一句）。不粘的话 `main.cpp` 的 `--help`
     会报 24 个假阳性。
  2. **字符字面量里的引号会把后面全带偏**（`'"'`）。不认它的话
     `llm/call_log.cpp` 数出来是 32 处，认了之后是 1 处。
  3. **数字分隔符那一撇不是字符字面量**（`60'000`）。当成字面量的话它
     一路吞到下一个撇号——`lan/sense.cpp` 里是整整一百五十行：那一段的
     注释被当成了代码，于是注释里带引号的四个词（"还用着呢""没票"
     "敲过门""在线"）被数成了"没包的中文"，而那一百五十行里真正的
     字面量反倒一个都没数到。**假阳性和假阴性是同一个 bug 的两面。**
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# 判过的：这些文件里的中文不是界面，别再数进"还欠多少"里。
NOT_UI = {
    "cpp/src/util/say_tables.inc.hpp": "表本身",
    "cpp/src/http/bundled_webapp.inc.hpp": "网页那一套，定稿冻住了",
    # ⚠️ 2026-09-22 起这一条**不是整份了**：那几个 `*_read` 回的正文
    # 有两个读者（模型 + `/api/peek` 那五格），走 `SAY_TO(to, …)`。
    # 剩下的才真是只有模型读得到的。守着这一条的是 test_say.cpp 里那句
    # 「agent/tools.cpp 里不许出现光秃秃的 SAY(」。
    "cpp/src/agent/tools.cpp": "给模型看的，翻了就是改了模型收到的东西"
                               "（`*_read` 那几个除外，见 SAY_TO）",
    # ⚠️ 这一条 2026-09-22 缩小过。原来整个文件都排除，理由是"toml 注释"——
    # 可 `validate()` 那 36 句是**界面上看得见的**（`config_api.cpp` 的
    # `readable()` 把它们回给设置页）。翻完那 36 句之后剩下的才真是注释。
    "cpp/src/config/settings.cpp": "剩下的是写进 changji.toml 的注释模板，不是界面",
    "cpp/src/setup/catalog.cpp": "剩下的 note / family_note 只有网页那一套在画",
    # 这一族是**数据不是字面量**：翻译发生在出 json 的那一处，这把尺子
    # 看不到。守着它们的是 `test_i18n.cpp` 里按数据走的那两条用例。
    "cpp/src/http/llm_providers.inc.hpp":
        "一整块 JSON，翻译在 http/llm_info.cpp 的出口，守卫见 test_i18n.cpp",
    "cpp/src/models/character.cpp": "性别词表，是解析器不是界面",
    "cpp/src/util/done_words.hpp": "解析用的词表",
    "cpp/src/util/cancel_words.hpp": "解析用的词表",
    "cpp/src/util/chapter_word.hpp": "解析用的词表",
    # 整份都是中文数字和「第…章」那几个字，用来把粘进来的正文切成章。
    # 翻一个字，中文稿子就再也切不出章来，而且不报错。
    "cpp/src/util/text.cpp": "切章用的数字和章回词表，是解析器不是界面",
    "cpp/src/stages/shot_schema.inc.hpp": "给模型看的",
}
# `stages/` 整族都是发给模型的提示词——**但不是每一个都是**。
#
# ⚠️ 2026-09-22 挖出两个判错了的：`storyboard.cpp` 的 `check_coverage`
# 那两句诊断、`tts_backends.cpp` 里整条 `why` 链（配音为什么起不来）。
# 它们一个字都不进提示词，唯一的去处是界面。判的是**这句话给谁看**，
# 不是它躺在哪个目录。这两个文件仍在这张排除名单里（里头还有没翻完的），
# 该翻的那几句就地包了 `SAY()`，不影响"还欠多少"。
NOT_UI_DIR = "cpp/src/stages/"

# 外层仓库的 agent/ 替掉的那几份（2026-09-23 起对话代理在外层，见
# cpp/CMakeLists.txt「编哪一份」）。外层有 agent/ 时这几份是没编进去的旧版，
# 扫它们等于数错地方。
AGENT_OUTER = ROOT / "agent"
AGENT_REPLACED_DIR = "cpp/src/agent/"
AGENT_REPLACED = {"cpp/src/http/chat_api.cpp"}
NOT_UI["agent/tools.cpp"] = NOT_UI["cpp/src/agent/tools.cpp"]


def sources():
    """编进去的那几份 .cpp / .hpp：cpp/src，外层有 agent/ 时换成它的。"""
    outer = (AGENT_OUTER / "loop.cpp").exists()
    for p in sorted((ROOT / "cpp/src").rglob("*")):
        if p.suffix not in (".cpp", ".hpp"):
            continue
        r = rel(p)
        if outer and (r.startswith(AGENT_REPLACED_DIR) or r in AGENT_REPLACED):
            continue
        yield p
    if outer:
        for p in sorted(AGENT_OUTER.glob("*")):
            if p.suffix in (".cpp", ".hpp"):
                yield p
NOT_UI_DIR_KEEP = {"cpp/src/stages/render.cpp", "cpp/src/stages/frames.cpp"}

LIT = re.compile(r'"(?:[^"\\]|\\.)*"')
HAN = re.compile(r"[一-鿿]")


def strip_comments(src):
    """去掉注释，顺带记下每个留下来的字符原来在第几行。"""
    out, lines, i, n, line = [], [], 0, len(src), 1

    def keep(a, b):
        nonlocal line
        for k in range(a, min(b, n)):
            out.append(src[k])
            lines.append(line)
            if src[k] == "\n":
                line += 1

    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            keep(i, j + 1)
            i = j + 1
            continue
        if c == "'":
            # 见文件头第 2、3 条。
            #
            # **数字分隔符不是字符字面量**：`60'000` 里那一撇。当成字面量
            # 的话它一路吞到下一个撇号，中间那一百五十行原样留下——注释里
            # 带引号的那些词就都被当成句子数了进来，而真正的字面量反倒
            # 藏起来了。判据照 C++：两边都是（十六进制）数字才是分隔符。
            HEX = "0123456789abcdefABCDEF"
            if (i > 0 and src[i - 1] in HEX
                    and i + 1 < n and src[i + 1] in HEX):
                keep(i, i + 1)
                i += 1
                continue
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    break
                j += 1
            keep(i, j + 1)
            i = j + 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 2
            # 整段抹掉，什么都不填——见文件头第 1 条，两边的字面量还要粘上
            continue
        keep(i, i + 1)
        i += 1
    return "".join(out), lines


def literals(clean):
    """相邻的粘成一句。返回 (起, 止, 文本)。"""
    merged = []
    for m in LIT.finditer(clean):
        piece = [m.start(), m.end(), m.group()[1:-1]]
        if merged and clean[merged[-1][1]:piece[0]].strip() == "":
            merged[-1][1] = piece[1]
            merged[-1][2] += piece[2]
        else:
            merged.append(piece)
    return merged


def unescape(raw):
    return (raw.replace("\\n", "\n").replace("\\t", "\t")
               .replace('\\"', '"').replace("\\\\", "\\"))


WRAPPED = re.compile(
    r'(?:SAY(?:F|N|_NOOP|_NEVER)?\(\s*'
    # `SAY_TO(to, "…")` / `SAYF_TO(to, "…")` / `SAYN_TO(to, "…", n)`：句子是
    # **第二个参数**，第一个是"说给谁听"。见 util/say.hpp 的 i18n::Audience。
    r'|SAY[FN]?_TO\(\s*[A-Za-z_][A-Za-z0-9_:.]*\s*,\s*)'
    r'(?:"(?:[^"\\]|\\.)*"\s*)*$')


def wrapped_at(clean, start):
    """这一句判过了没有。

    `SAY(` / `SAYF(` / `SAYN(` 是要翻的（`SAYN` 是带数分档的那一族，
    见 util/say.hpp 的 i18n::Plural），`SAY_TO(` / `SAYF_TO(` / `SAYN_TO(`
    是**两个读者**那一种
    （模型拿原话、人拿译文），`SAY_NOOP(` 是晚点翻，`SAY_NEVER(` 是判过
    永远不翻——都算"判过了"，都不进"还欠多少"。
    """
    return WRAPPED.search(clean[max(0, start - 600):start]) is not None


def scan(path):
    clean, lines = strip_comments(path.read_text(encoding="utf-8"))
    hits = []
    for s, _e, txt in literals(clean):
        if HAN.search(txt) and not wrapped_at(clean, s):
            hits.append((lines[s], unescape(txt)))
    return hits


def said(path):
    """这个文件里包了 SAY()/SAYF()/SAYN()/SAY_NOOP()/SAY_*_TO() 的句子，
    按出现顺序、去重。

    ⚠️ **`SAYN(` 那一族的键在表里对着的是一族复数，不是一句话**——数它的时候
    和别的一样一句算一句，值长什么样是 `gen_say.py` 管的事。"""
    clean, _ = strip_comments(path.read_text(encoding="utf-8"))
    keys = []
    # ⚠️ **`SAY_NEVER(` 不收**：它判的是"永远不翻"，收进来的话十一份表里
    # 会多出一堆没人说的句子，第 4 条守卫当场红。
    for m in re.finditer(
            r'(?:SAY(?:F|N|_NOOP)?\(\s*|SAY[FN]?_TO\(\s*[A-Za-z_][A-Za-z0-9_:.]*\s*,\s*)'
            r'((?:"(?:[^"\\]|\\.)*"\s*)+)', clean):
        txt = unescape("".join(x[1:-1] for x in LIT.findall(m.group(1))))
        if txt not in keys:
            keys.append(txt)
    return keys


def rel(p):
    return str(p.relative_to(ROOT)).replace("\\", "/")


# ---------------------------------------------------------------------------
# 带数的那一族（`SAYN` / `i18n::Plural`）
#
# 判据是「`%N` 后面直接跟一个**可能为 1** 的名词」，拿英语那份最好认。
# **不是每一句都错**：有的那个数结构上就 ≥2，有的名词本来就不随数变，还有
# 的干脆是这把尺子看错了。所以剩下的每一句都得**写明白为什么不动**——
# 否则「还剩 N 句」这个数下一个人还得从头判一遍。
#
#     python3 cpp/tools/say_scan.py --plurals
#
# 判过要动的那些走 `SAYN`，键里的数写成 `%n`，值是一族（见 gen_say.py）。
# 它们**不在这张单子上**：`%n` 的键在表里对着的是对象，不是字符串，
# 这一档只扫还是字符串的那些。

PLURAL_NOUNS = (
    "shots", "chapters", "characters", "bytes", "things", "machines", "items",
    "rounds", "lines", "files", "models", "times", "scenes", "steps", "frames",
    "threads", "cards", "pixels", "tokens", "matches", "takes", "copies",
    "groups", "words",
)

# 判过**不动**的，一句一条理由。
PLURAL_OK = {
    "%1 不符合 anyOf 中的任何一种结构":
        "`%1` 是 json 里的路径，不是数——这把尺子把英文里的 matches 当成名词了",
    "%1 出了 %2 条，留第 %3 条":
        "调用处挡着 `results.size() > 1`（render.cpp，只出一条就不说这句）",
    "%1 步  单镜 %2":
        "步数来自档位表，写死的是 8 / 10 / 20，没有 1（hardware.cpp 的 kTierTable）",
    "%1 超出这个模型的画布上限 %2 像素（%3 倍）":
        "`%2` 是模型的像素上限，几十万",
    "%1 超长：%2 字，最多 %3 字":
        "两个数结构上都 ≥2：超长了才报这句，而上限也不会是 1 个字",
    "premise 超长：%1 字，最多 2000 字":
        "同上，`%1` 必然大于 2000",
    "写好了 %1 · %2 字":
        "一章的字数，闸门有下限（几百字起），到不了 1",
    "出片 · %1 章":
        "调用处上面就有 `queue.size() == 1` 的岔路单说一句（run.cpp）",
    "我连着查了 %1 轮还没想清楚，先停在这儿。你说得再具体一点？":
        "产品里只用默认值 8（agent/loop.hpp），只有测试传过别的",
    "提示词太长：%1 个 token 加上要生成的 %2 个，超过这个模型的上下文 %3。"
    "把 [llm].context_tokens 调大，或者换一个上下文更长的模型。":
        "token 数，上千",
    "磁盘不够：放大到 %1×%2 要先写 %3 GB 的中间文件（%4 帧 × %5 MB），"
    "而 %6 上只剩 %7 GB。\n腾出空间，或者把片子切成几段分开跑。":
        "GB / MB 是单位缩写，十一种语言里都不随数变；`%4` 是整段片子的帧数",
    "试了 %1 次都没下全。":
        "那个数是常量 `kMaxAttempts = 3`（setup/downloader.cpp）",
    "这一章拆成 %1 场，粘回来的只有 %2 段。每一场之间要留着复制出去时那一行"
    "「===== 第 N/M 场 …… =====」":
        "调用处挡着 `want > 1`（http/planning.cpp）",
    "（共 %1 张，显存是单卡的）":
        "两处调用都挡着 `count > 1`（doctor.cpp、hardware.cpp）",
}


def plurals():
    """英语表里还有哪些「数 + 可能为 1 的名词」没判过。"""
    import json
    rows = json.loads((ROOT / "cpp/i18n/engine_en.json").read_text(encoding="utf-8"))
    pat = re.compile(r"%\d\s+(?:" + "|".join(PLURAL_NOUNS) + r")\b")
    # 值还是字符串的才扫：改成 `SAYN` 的那些值是一族对象，早判过了。
    hit = [k for k, v in rows.items() if isinstance(v, str) and pat.search(v)]
    stale = [k for k in PLURAL_OK if k not in hit]
    todo = [k for k in hit if k not in PLURAL_OK]
    for k in sorted(todo):
        print("  没判过：" + k[:70].replace("\n", "\\n"))
    for k in sorted(stale):
        print("  单子上多出来的（源码里已经没这一句了）：" + k[:60].replace("\n", "\\n"))
    print(f"带数的：扫出 {len(hit)} 句，判过不动 {len(hit) - len(todo)} 句，"
          f"还没判 {len(todo)} 句")
    return 1 if (todo or stale) else 0


CJK_PUNCT = re.compile(r"^[：；、，。！？「」『』（）〈〉《》【】·／～]+$")


def joiners(path):
    """只由中文标点组成的字面量：拿来把两段已经翻过的话接起来的那种。

    **这把尺子的盲区。** `has_han` 只认汉字，抓不到「：」「；」这些标点
    ——而两头的话都翻了、中间那个标点没翻，在德语句子里就那么立着。
    2026-09-22 在真引擎上看见过两次（`run_deps.cpp`、`story_api.cpp`）。

    解析器里这种标点是正当的（切句、认场头），所以这一档**只列出来给人看**，
    不算进"还欠多少"。
    """
    clean, lines = strip_comments(path.read_text(encoding="utf-8"))
    out = []
    for s, _e, txt in literals(clean):
        if CJK_PUNCT.match(txt) and not wrapped_at(clean, s):
            out.append((lines[s], txt))
    return out


def main(argv):
    if "--joiners" in argv:
        rows = []
        for p in sources():
            r = rel(p)
            if r in NOT_UI or (r.startswith(NOT_UI_DIR) and r not in NOT_UI_DIR_KEEP):
                continue
            for ln, txt in joiners(p):
                rows.append((r, ln, txt))
        print(f"没包的中文标点 {len(rows)} 处（解析器里的是正当的，人看一眼）")
        for r, ln, txt in rows:
            print(f"  {r}:{ln}  {txt}")
        return 0

    if "--plurals" in argv:
        return plurals()

    if "--missing" in argv:
        f = ROOT / argv[argv.index("--missing") + 1]
        tbl = json.loads((ROOT / "cpp/i18n/engine_en.json").read_text("utf-8"))
        keys = said(f)
        miss = [k for k in keys if k not in tbl]
        print(f"{rel(f)}：包了 {len(keys)} 句，十一份表里缺 {len(miss)} 句")
        for k in miss:
            print("  " + json.dumps(k, ensure_ascii=False))
        return 0

    files = [a for a in argv[1:] if not a.startswith("-")]
    if files:
        for a in files:
            p = ROOT / a
            hits = scan(p)
            print(f"{rel(p)}：没包的中文 {len(hits)} 处")
            for ln, txt in hits:
                print(f"  {ln:5d}  {txt[:110]}")
        return 0

    show_all = "--all" in argv
    rows, skipped = [], 0
    for p in sources():
        r = rel(p)
        hits = scan(p)
        if not hits:
            continue
        why = NOT_UI.get(r)
        if why is None and r.startswith(NOT_UI_DIR) and r not in NOT_UI_DIR_KEEP:
            why = "给模型看的提示词"
        if why and not show_all:
            skipped += len(hits)
            continue
        rows.append((len(hits), r, why))
    rows.sort(reverse=True)
    total = sum(n for n, _, _ in rows)
    print(f"还欠 {total} 处，摊在 {len(rows)} 个文件上"
          + (f"（判过不翻的另有 {skipped} 处，见 NOT_UI）" if not show_all else ""))
    for n, r, why in rows:
        print(f"{n:5d}  {r}" + (f"   ← {why}" if why else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
