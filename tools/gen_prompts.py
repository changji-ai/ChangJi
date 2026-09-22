r"""把 prompts.toml 里的提示词生成成一个 C++ 头，构建期跑。

跑法（CMake 会自己调，手跑只是为了看输出）：
    python cpp/tools/gen_prompts.py cpp/prompts.toml <输出路径>.hpp

**提示词的出处是 cpp/prompts.toml，不是任何 .hpp。** 生成出来的头在构建
目录里（build/generated/stages/prompts.inc.hpp），每次 prompts.toml 或本
脚本一变就重新生成，进不了版本库，改了也会被冲掉。

---- 为什么是构建期生成，而不是像 bundled_webapp.inc.hpp 那样进版本库 ----

前端那份进了版本库，代价是有一个"谁忘了跑脚本"的窗口：改了前端、没重新
生成，编出来的二进制装的是旧界面，而没有任何地方会报错。提示词改得比前端
勤得多（一天几次），这个窗口开着迟早出事——"改了提示词模型没反应"
排查起来会先怀疑模型。所以这里让 CMake 管：prompts.toml 是
add_custom_command 的 DEPENDS，改它就重编。

---- TOML 的子集 ----

这个脚本**不依赖 tomllib**（Python 3.11 才有；ubuntu-22.04 那格 runner 的
python3 是 3.10）。自己带一个只认下面这些东西的解析器：

    [表]                  一级表 → 一个 C++ 命名空间
    [表.字典]             二级表 → 一张 (键, 值) 对表，键值都得是字符串
    键 = "单行"            基本字符串，认 \n \t \" \\ \uXXXX 这几种转义
    键 = \"\"\"多行\"\"\"       多行基本字符串；开头紧跟的那个换行不算内容
    键 = '单行' / '''多行'''   字面字符串，不转义
    键 = 123 / 1.5        整数 → std::size_t，小数 → double
    键 = ["a", "b"]       字符串数组，可以跨行，末尾允许多一个逗号
    # 注释                整行或值后面

装着 tomllib 的机器上会再用它解析一遍、要求两边结果一模一样——自带的
解析器有偏差时在那儿就会被抓住，而不是悄悄生成一份错的提示词。

---- 生成规则 ----

    [script]  seg0 = "…"          →  namespace script { inline constexpr const char* kSeg0 = R"CJ(…)CJ"; }
    [script]  act_keys = [...]    →  inline constexpr const char* kActKeys[] = {…};
    [script]  prev_max_chars = 4000  →  inline constexpr std::size_t kPrevMaxChars = 4000;
    [compose.shot_size] ecu = "大特写"  →  namespace compose { inline constexpr std::pair<const char*, const char*> kShotSize[] = {{"ecu", R"CJ(大特写)CJ"}, …}; }

键名 snake_case 转 CamelCase 再加 k：hint_realistic → kHintRealistic。
字符串走原始字面量，分隔符 CJ；内容里出现 )CJ" 会当场报错，理由同
gen_webapp.py。单条超过 16 KB 的按 UTF-8 边界切成相邻字面量拼接
（MSVC 的 C2026），超过 60 KB 报错（拼完还有 65535 的上限）。
"""
from __future__ import annotations

import io
import re
import sys
from pathlib import Path

# Windows 上标准输出默认不是 UTF-8，理由见 gen_webapp.py 开头。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

DELIM = "CJ"
CHUNK = 16000
MAX_ONE = 60000


class TomlError(Exception):
    pass


# ---------------------------------------------------------------------------
# 解析器
# ---------------------------------------------------------------------------

_BARE_KEY = re.compile(r"[A-Za-z0-9_-]+")
_NUMBER = re.compile(r"[+-]?(?:\d[\d_]*)(\.\d[\d_]*)?([eE][+-]?\d+)?")
_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", '"': '"', "\\": "\\"}


class _Parser:
    def __init__(self, text: str):
        self.s = text
        self.i = 0
        self.doc: dict = {}
        self.cur: dict = self.doc
        self.seen_tables: set[tuple[str, ...]] = set()

    # ---- 位置和报错 ----
    def line(self) -> int:
        return self.s.count("\n", 0, self.i) + 1

    def fail(self, msg: str):
        raise TomlError(f"第 {self.line()} 行：{msg}")

    def peek(self, n: int = 1) -> str:
        return self.s[self.i : self.i + n]

    def eof(self) -> bool:
        return self.i >= len(self.s)

    # ---- 空白和注释 ----
    def skip_ws(self):
        while not self.eof() and self.s[self.i] in " \t":
            self.i += 1

    def skip_comment(self):
        if self.peek() == "#":
            while not self.eof() and self.s[self.i] != "\n":
                self.i += 1

    def skip_blank_lines(self):
        """跳过空白、注释、换行，停在下一个有内容的地方。"""
        while not self.eof():
            self.skip_ws()
            self.skip_comment()
            if self.peek() == "\r":
                self.i += 1
                continue
            if self.peek() == "\n":
                self.i += 1
                continue
            break

    def expect_eol(self):
        self.skip_ws()
        self.skip_comment()
        if self.eof():
            return
        if self.peek() == "\r":
            self.i += 1
        if self.peek() != "\n":
            self.fail(f"值后面多了东西：{self.s[self.i:self.i+20]!r}")
        self.i += 1

    # ---- 顶层 ----
    def parse(self) -> dict:
        while True:
            self.skip_blank_lines()
            if self.eof():
                return self.doc
            if self.peek() == "[":
                self.parse_table_header()
            else:
                self.parse_keyval()

    def parse_table_header(self):
        self.i += 1  # [
        if self.peek() == "[":
            self.fail("不认表数组 [[…]]")
        parts: list[str] = []
        while True:
            self.skip_ws()
            m = _BARE_KEY.match(self.s, self.i)
            if not m:
                self.fail("表名只能是字母、数字、下划线、连字符")
            parts.append(m.group())
            self.i = m.end()
            self.skip_ws()
            if self.peek() == ".":
                self.i += 1
                continue
            if self.peek() == "]":
                self.i += 1
                break
            self.fail("表头没闭合")
        key = tuple(parts)
        if len(key) > 2:
            self.fail(f"最多两级：[{'.'.join(parts)}]")
        if key in self.seen_tables:
            self.fail(f"表 [{'.'.join(parts)}] 定义了两次")
        self.seen_tables.add(key)
        node = self.doc
        for p in parts:
            existing = node.get(p)
            if existing is None:
                existing = {}
                node[p] = existing
            elif not isinstance(existing, dict):
                self.fail(f"{p} 已经是一个值，不能再当表")
            node = existing
        self.cur = node
        self.expect_eol()

    def parse_keyval(self):
        m = _BARE_KEY.match(self.s, self.i)
        if not m:
            self.fail(f"看不懂：{self.s[self.i:self.i+20]!r}")
        key = m.group()
        self.i = m.end()
        self.skip_ws()
        if self.peek() != "=":
            self.fail(f"键 {key} 后面要跟 =")
        self.i += 1
        self.skip_ws()
        value = self.parse_value()
        if key in self.cur:
            self.fail(f"键 {key} 重复")
        self.cur[key] = value
        self.expect_eol()

    # ---- 值 ----
    def parse_value(self):
        c = self.peek()
        if self.peek(3) == '"""':
            return self.parse_ml_basic()
        if c == '"':
            return self.parse_basic()
        if self.peek(3) == "'''":
            return self.parse_ml_literal()
        if c == "'":
            return self.parse_literal()
        if c == "[":
            return self.parse_array()
        if self.s.startswith("true", self.i):
            self.i += 4
            return True
        if self.s.startswith("false", self.i):
            self.i += 5
            return False
        m = _NUMBER.match(self.s, self.i)
        if m and m.end() > self.i:
            raw = m.group().replace("_", "")
            self.i = m.end()
            if m.group(1) or m.group(2):
                return float(raw)
            return int(raw)
        self.fail(f"看不懂这个值：{self.s[self.i:self.i+20]!r}")

    def parse_escape(self) -> str:
        """self.i 指在反斜杠上。"""
        self.i += 1
        c = self.peek()
        if c in _ESCAPES:
            self.i += 1
            return _ESCAPES[c]
        if c == "u" or c == "U":
            n = 4 if c == "u" else 8
            hexs = self.s[self.i + 1 : self.i + 1 + n]
            if len(hexs) != n or not re.fullmatch(r"[0-9A-Fa-f]+", hexs):
                self.fail("\\u 后面要跟 4 位十六进制，\\U 跟 8 位")
            self.i += 1 + n
            return chr(int(hexs, 16))
        self.fail(f"不认的转义：\\{c}")

    def parse_basic(self) -> str:
        self.i += 1
        out = []
        while True:
            if self.eof():
                self.fail("字符串没闭合")
            c = self.s[self.i]
            if c == '"':
                self.i += 1
                return "".join(out)
            if c == "\\":
                out.append(self.parse_escape())
                continue
            if c == "\n":
                self.fail('单行字符串里不能有换行，要换行用 """')
            if ord(c) < 0x20 and c != "\t":
                self.fail("字符串里有控制字符")
            out.append(c)
            self.i += 1

    def parse_ml_basic(self) -> str:
        self.i += 3
        # 开头紧跟的换行不算内容
        if self.peek(2) == "\r\n":
            self.i += 2
        elif self.peek() == "\n":
            self.i += 1
        out = []
        while True:
            if self.eof():
                self.fail('多行字符串没闭合（缺 """）')
            c = self.s[self.i]
            if c == '"':
                j = self.i
                while j < len(self.s) and self.s[j] == '"':
                    j += 1
                run = j - self.i
                if run >= 3:
                    extra = run - 3
                    if extra > 2:
                        self.fail("连续的引号太多，多出来的要转义成 \\\"")
                    out.append('"' * extra)
                    self.i = j
                    return "".join(out)
                out.append('"' * run)
                self.i = j
                continue
            if c == "\\":
                # 行尾反斜杠：把后面的空白和换行全吃掉
                k = self.i + 1
                while k < len(self.s) and self.s[k] in " \t":
                    k += 1
                if k < len(self.s) and self.s[k] in "\r\n":
                    while k < len(self.s) and self.s[k] in " \t\r\n":
                        k += 1
                    self.i = k
                    continue
                out.append(self.parse_escape())
                continue
            if c == "\r" and self.peek(2) == "\r\n":
                # 文件被换成 CRLF 也不该改变提示词的内容
                self.i += 2
                out.append("\n")
                continue
            if ord(c) < 0x20 and c not in "\t\n":
                self.fail("字符串里有控制字符")
            out.append(c)
            self.i += 1

    def parse_literal(self) -> str:
        self.i += 1
        j = self.s.find("'", self.i)
        if j < 0:
            self.fail("字面字符串没闭合")
        val = self.s[self.i : j]
        if "\n" in val:
            self.fail("单行字面字符串里不能有换行，要换行用 '''")
        self.i = j + 1
        return val

    def parse_ml_literal(self) -> str:
        self.i += 3
        if self.peek(2) == "\r\n":
            self.i += 2
        elif self.peek() == "\n":
            self.i += 1
        j = self.s.find("'''", self.i)
        if j < 0:
            self.fail("多行字面字符串没闭合")
        # 紧贴闭合符的多余单引号归内容（最多两个）
        k = j + 3
        extra = 0
        while k < len(self.s) and self.s[k] == "'" and extra < 2:
            k += 1
            extra += 1
        val = self.s[self.i : j] + "'" * extra
        self.i = k
        return val.replace("\r\n", "\n")

    def parse_array(self) -> list:
        self.i += 1
        out: list = []
        while True:
            self.skip_blank_lines()
            if self.eof():
                self.fail("数组没闭合")
            if self.peek() == "]":
                self.i += 1
                return out
            out.append(self.parse_value())
            self.skip_blank_lines()
            if self.peek() == ",":
                self.i += 1
                continue
            if self.peek() == "]":
                self.i += 1
                return out
            self.fail("数组元素之间要用逗号")


def parse_toml(text: str) -> dict:
    if text.startswith("﻿"):
        text = text[1:]
    doc = _Parser(text).parse()
    # 有 tomllib 就对一遍。两边不一样说明自带的解析器有偏差——那是本脚本
    # 的 bug，不能让它悄悄生成一份错的提示词。
    try:
        import tomllib  # type: ignore
    except ImportError:
        return doc
    ref = tomllib.loads(text)
    if ref != doc:
        raise TomlError("自带的解析器和 tomllib 解析结果不一样，本脚本有 bug："
                        + _first_diff(ref, doc))
    return doc


_SHARED_REF = re.compile(r"\{\{\s*shared\.([A-Za-z0-9_-]+)\s*\}\}")


def expand_shared(doc: dict) -> dict:
    """把 `{{shared.键}}` 换成 `[shared]` 表里那段文字，然后把 `[shared]` 拿掉。

    ---- 为什么要有这个 ----

    同一条规矩在好几张表里各抄一遍是这份文件最容易出错的地方：「只输出
    JSON，不要任何解释文字。」有九处，画风那两句各四处，「必须沿用这些已有
    角色」三处。改一处忘了另外两处，出来的东西不一致，而**没有任何地方会
    报错**——提示词差一句话，模型只是写得不一样，接口照样返回 200。

    ---- 为什么在生成期展开，而不是让 C++ 去拼 ----

    展开之后生成出来的头**一个字节都没变**。这很要紧：[script] [bible]
    [storyboard] 这几张表被「和 Python 逐字节一样」的语料钉着，任何"顺手
    整理一下"都会把那几条用例弄红，然后人就会去改语料——而改语料是应该
    留给"真的想改提示词"那一刻的动作。让 C++ 去拼的话，拼接顺序就成了第二
    个要维护的地方，而它没有任何东西钉着。

    ---- 规矩 ----

    · `[shared]` 里只能放字符串，而且**不能再引用别的共用段**：一层就够用，
      两层之后"这句话到底长什么样"要跳三个地方才看得出来。
    · 引用了不存在的键当场报错。写错一个字就静默留下 `{{shared.jsno_only}}`
      在提示词里的话，模型会把它当正文读。
    · 定义了没人用的键也报错。多半是改名时漏了一处，而留着的那份会让下一个
      人以为它还在生效。
    """
    shared = doc.get("shared")
    if shared is None:
        return doc
    if not isinstance(shared, dict):
        raise TomlError("[shared] 要是一张表")
    for k, v in shared.items():
        if not isinstance(v, str):
            raise TomlError(f"[shared] {k}：只能放字符串，共用段是文字不是旋钮")
        if _SHARED_REF.search(v):
            raise TomlError(f"[shared] {k}：共用段里不能再引用共用段")

    used: set[str] = set()

    def sub(text: str, where: str) -> str:
        def one(m: "re.Match[str]") -> str:
            k = m.group(1)
            if k not in shared:
                raise TomlError(f"{where}：[shared] 里没有 {k}")
            used.add(k)
            return shared[k]

        return _SHARED_REF.sub(one, text)

    def walk(value, where: str):
        if isinstance(value, str):
            return sub(value, where)
        if isinstance(value, list):
            return [walk(x, where) for x in value]
        if isinstance(value, dict):
            return {k: walk(v, f"{where} {k}") for k, v in value.items()}
        return value

    out = {}
    for tname, table in doc.items():
        if tname == "shared":
            continue
        out[tname] = walk(table, f"[{tname}]")

    dead = sorted(set(shared) - used)
    if dead:
        raise TomlError("[shared] 里这几条没人引用，是不是改名时漏了："
                        + "、".join(dead))
    return out


def _first_diff(a, b, path: str = "") -> str:
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                return f"{path}/{k} 只在一边有"
            d = _first_diff(a[k], b[k], f"{path}/{k}")
            if d:
                return d
        return ""
    if a != b:
        return f"{path}: tomllib={a!r} 自带={b!r}"
    return ""


# ---------------------------------------------------------------------------
# 生成
# ---------------------------------------------------------------------------

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def camel(key: str) -> str:
    return "".join(w[:1].upper() + w[1:] for w in key.split("_") if w)


def cxx_name(key: str, where: str) -> str:
    if not re.fullmatch(r"[a-z0-9_]+", key):
        raise TomlError(f"{where}：键 {key!r} 要用小写字母、数字和下划线")
    name = "k" + camel(key)
    if not _IDENT.fullmatch(name) or name[1:2].isdigit():
        raise TomlError(f"{where}：键 {key!r} 转不成 C++ 标识符")
    return name


def split_utf8(data: bytes, limit: int) -> list[bytes]:
    out = []
    i = 0
    while i < len(data):
        end = min(i + limit, len(data))
        while end > i and end < len(data) and (data[end] & 0xC0) == 0x80:
            end -= 1
        out.append(data[i:end])
        i = end
    return out or [b""]


def raw_literal(s: str, where: str) -> str:
    if f"){DELIM}\"" in s:
        raise TomlError(f"{where}：内容里出现了 ){DELIM}\"，原始字面量会在那儿断掉")
    data = s.encode("utf-8")
    if len(data) > MAX_ONE:
        raise TomlError(f"{where}：一条 {len(data)} 字节，超过 MSVC 的字面量上限，拆成两条")
    pieces = [p.decode("utf-8") for p in split_utf8(data, CHUNK)]
    return " ".join(f'R"{DELIM}({p}){DELIM}"' for p in pieces)


def emit_scalar(lines: list[str], key: str, value, where: str):
    name = cxx_name(key, where)
    lines.append(f"// {where}")
    if isinstance(value, bool):
        lines.append(f"inline constexpr bool {name} = {'true' if value else 'false'};")
    elif isinstance(value, int):
        if value < 0:
            lines.append(f"inline constexpr long long {name} = {value};")
        else:
            lines.append(f"inline constexpr std::size_t {name} = {value};")
    elif isinstance(value, float):
        r = repr(value)
        if "e" not in r and "." not in r:
            r += ".0"
        lines.append(f"inline constexpr double {name} = {r};")
    elif isinstance(value, str):
        lines.append(f"inline constexpr const char* {name} =")
        lines.append(f"    {raw_literal(value, where)};")
    elif isinstance(value, list):
        if not value or not all(isinstance(v, str) for v in value):
            raise TomlError(f"{where}：数组只能装字符串，而且不能是空的")
        lines.append(f"inline constexpr const char* {name}[] = {{")
        for v in value:
            lines.append(f"    {raw_literal(v, where)},")
        lines.append("};")
    else:
        raise TomlError(f"{where}：不认这种值 {type(value).__name__}")
    lines.append("")


def emit_dict(lines: list[str], key: str, table: dict, where: str):
    name = cxx_name(key, where)
    if not table:
        raise TomlError(f"{where}：字典是空的")
    lines.append(f"// {where}  （键 → 值）")
    lines.append(f"inline constexpr std::pair<const char*, const char*> {name}[] = {{")
    for k, v in table.items():
        if not isinstance(v, str):
            raise TomlError(f"{where}.{k}：二级表里只能放字符串")
        lines.append(f"    {{{raw_literal(k, where)}, {raw_literal(v, where)}}},")
    lines.append("};")
    lines.append("")


def generate(doc: dict, src_name: str) -> str:
    lines = [
        f"// 由 cpp/tools/gen_prompts.py 从 {src_name} 生成，**不要手改**。",
        "// 它在构建目录里，每次构建都会重新生成，改了也会被冲掉。",
        f"// 要改提示词，改 {src_name}；每条上面的注释就是它在那份文件里的位置。",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <utility>",
        "",
        "namespace changji::stages::prompt {",
        "",
    ]
    for tname, table in doc.items():
        if not isinstance(table, dict):
            raise TomlError(f"顶层不能直接放值（{tname}），先开一个 [表]")
        if not _IDENT.fullmatch(tname):
            raise TomlError(f"[{tname}]：表名要能当 C++ 命名空间")
        lines.append(f"namespace {tname} {{")
        lines.append("")
        for key, value in table.items():
            where = f"[{tname}] {key}"
            if isinstance(value, dict):
                emit_dict(lines, key, value, f"[{tname}.{key}]")
            else:
                emit_scalar(lines, key, value, where)
        lines.append(f"}}  // namespace {tname}")
        lines.append("")
    lines.append("}  // namespace changji::stages::prompt")
    lines.append("")
    return "\n".join(lines)


def check_no_hash_lines(doc: dict) -> list[str]:
    """揪出**以 # 打头的正文行**。

    TOML 的三引号串里 `#` 不是注释，是正文。2026-09-16 撞到过：
    storyboard_rules 那个三引号**里面**写着一段讲道理的话，以为是注释——
    359 个字原样发给了模型，夹在第 5 条和第 6 条中间。那一轮"把讲道理搬进
    注释"的压缩于是白做了大半：规则少了 482 字，又塞回 359 字，还让模型在
    一串规矩中间读到一段讨论提示词工程的元文字。

    **这一条报在构建期**，不是测试里：提示词写错了就该编不过去，而不是等
    某个用例跑到。讲道理的话写在表外面、或者键与键之间，那儿才是真注释。
    """
    bad = []
    for table, entries in doc.items():
        if not isinstance(entries, dict):
            continue
        for key, val in entries.items():
            if not isinstance(val, str):
                continue
            for i, line in enumerate(val.splitlines(), 1):
                if line.lstrip().startswith("#"):
                    bad.append(f"[{table}].{key} 第 {i} 行：{line.strip()[:40]}")
    return bad


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("用法：gen_prompts.py <prompts.toml> <输出.hpp>", file=sys.stderr)
        return 2
    src = Path(argv[1])
    dest = Path(argv[2])
    try:
        doc = expand_shared(parse_toml(src.read_text(encoding="utf-8")))
        bad = check_no_hash_lines(doc)
        if bad:
            print(
                f"{src}: 提示词正文里有 {len(bad)} 行以 # 打头——TOML 的三引号串里 "
                "# 不是注释，这些字会原样发给模型。讲道理的话写到表外面去：",
                file=sys.stderr,
            )
            for b in bad[:20]:
                print("  " + b, file=sys.stderr)
            return 1
        out = generate(doc, src.name)
    except TomlError as e:
        print(f"{src}: {e}", file=sys.stderr)
        return 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    io.open(dest, "w", encoding="utf-8", newline="\n").write(out)
    n = sum(len(t) for t in doc.values() if isinstance(t, dict))
    print(f"提示词：{src.name} → {dest}（{len(doc)} 个表，{n} 条）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
