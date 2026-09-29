#!/usr/bin/env python3
"""把 `cpp/i18n/engine_<语言>.json` 烤成一个头文件。

    python3 cpp/tools/gen_say.py

**为什么烤进二进制**：引擎是一个文件发出去的（`install.sh` 拉的就是那一个），
旁边再挂十一个 json 的话，少一个就是那一种语言静悄悄不生效。同
`bundled_webapp.inc.hpp` 和 `prompts.toml` 那两处的理由。

json 的形状就是「中文原话 → 那一国的话」：

    { "中文字体": "Chinese font", "未找到": "not found" }

**键里带 `%n` 的那一族值是个对象**，一档一句（见 `util/say.hpp` 的
`i18n::Plural`）：

    { "写完了 %n 章": { "one": "Wrote %n chapter", "other": "Wrote %n chapters" } }

哪几种语言该给哪几档**不另立一张单子**：这个脚本按 `plural_of()` 的规矩把
0…200 跑一遍，跑出来哪几档就要哪几档，给漏了当场不让过。规矩在两处写着
（C++ 一份、这儿一份），所以 `test_say.cpp` 有一条守卫盯着两份对得上。

**键是中文原话本身**，不是另起的代号。代号那种做法要维护第二套名字，而
`SAY("中文字体")` 在代码里读起来就是那句话本身——漏翻的时候显示的也还是
那句话，不是一个看不懂的键名。
"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
I18N = HERE.parent / "i18n"
OUT = HERE.parent / "src" / "util" / "say_tables.inc.hpp"


CATS = ("zero", "one", "two", "few", "many", "other")


def plural_of(lang: str, n: int) -> str:
    """和 `util/say.cpp` 的 `plural_of()` 是同一套规矩。CLDR 的，抄下来的。"""
    base = lang.split("_")[0]
    v = abs(n)
    m10, m100 = v % 10, v % 100
    if base == "ru":
        if m10 == 1 and m100 != 11:
            return "one"
        if 2 <= m10 <= 4 and not (12 <= m100 <= 14):
            return "few"
        return "many"
    if base == "ar":
        if v == 0:
            return "zero"
        if v == 1:
            return "one"
        if v == 2:
            return "two"
        if 3 <= m100 <= 10:
            return "few"
        if 11 <= m100 <= 99:
            return "many"
        return "other"
    if base in ("fr", "pt"):
        return "one" if v in (0, 1) else "other"
    # 印地语跟 Qt 不跟 CLDR（0 算复数）。为什么，见 util/say.cpp 那一段。
    if base in ("en", "de", "es", "hi"):
        return "one" if v == 1 else "other"
    return "other"


def needed(lang: str) -> list:
    """这种语言用得上哪几档——**跑出来的，不是写死的**。"""
    seen = {plural_of(lang, n) for n in range(0, 201)}
    return [c for c in CATS if c in seen]


def esc(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        else:
            out.append(ch)
    return "".join(out)


def main() -> int:
    files = sorted(I18N.glob("engine_*.json"))
    if not files:
        print(f"{I18N} 底下一个 engine_*.json 都没有", file=sys.stderr)
        return 1

    lines = [
        "// 这个文件是生成的，别手改。",
        "//",
        "//     python3 cpp/tools/gen_say.py",
        "//",
        "// 源头是 cpp/i18n/engine_<语言>.json。为什么烤进二进制，见那个脚本。",
        "",
        "// 一句话的几档。下标就是 i18n::Plural 的取值：",
        "// zero one two few many other。没给的那一档是空串。",
        "using Forms = std::array<std::string, 6>;",
        "",
        "struct Table {",
        "    std::string_view lang;",
        "    std::unordered_map<std::string_view, std::string> rows;",
        "    std::unordered_map<std::string_view, Forms> plurals;",
        "};",
        "",
        "// ⚠️ **表是一串串 constexpr 的 string_view，起程序那一下再装进 map。**",
        "// 原来是一整个嵌套的初始化表达式（一万六千对 std::string），MSVC 为它建的",
        "// 语法树太大：2026-09-27 多加二十来句之后 say.cpp 单独编都报 C1060（编译器",
        "// 堆空间不足），-j 1 也一样——Windows 打包那一步跟着编不过。平铺的常量数组",
        "// 编译器几乎不费力气，再长也不怕。",
        "struct RawRow { std::string_view zh, said; };",
        "struct RawForms { std::string_view zh; std::string_view f[6]; };",
        "",
    ]
    total = 0
    many = 0
    adds = []
    for f in files:
        lang = f.stem[len("engine_"):]
        rows = json.loads(f.read_text(encoding="utf-8"))
        want = needed(lang)
        flat, forms = {}, {}
        for zh, said in rows.items():
            if isinstance(said, dict):
                if "%n" not in zh:
                    print(f"{f.name}：「{zh[:40]}」的值是一族复数，"
                          f"可键里没有 %n——那个数得写成 %n 才挑得了档",
                          file=sys.stderr)
                    return 1
                short = [c for c in want if not said.get(c)]
                if short:
                    print(f"{f.name}：「{zh[:40]}」缺 {'、'.join(short)} 这几档"
                          f"（{lang} 用得上 {'、'.join(want)}）", file=sys.stderr)
                    return 1
                extra = [c for c in said if c not in CATS]
                if extra:
                    print(f"{f.name}：「{zh[:40]}」里 {'、'.join(extra)} "
                          f"不是档的名字（只有 {'、'.join(CATS)}）", file=sys.stderr)
                    return 1
                forms[zh] = said
            elif "%n" in zh:
                print(f"{f.name}：「{zh[:40]}」的键里有 %n，可值是一句话"
                      f"——带数的那一族值得写成 {{\"one\": …, \"other\": …}}",
                      file=sys.stderr)
                return 1
            elif said:
                flat[zh] = said          # 没翻的就别进表，查不到自然落回原话
        ident = "".join(ch if ch.isalnum() else "_" for ch in lang)
        rows_name, forms_name = "kRows_" + ident, "kForms_" + ident
        if flat:
            lines.append("inline constexpr RawRow %s[] = {" % rows_name)
            for zh in sorted(flat):
                lines.append('    {"%s", "%s"},' % (esc(zh), esc(flat[zh])))
                total += 1
            lines.append("};")
        if forms:
            lines.append("inline constexpr RawForms %s[] = {" % forms_name)
            for zh in sorted(forms):
                cells = ", ".join('"%s"' % esc(forms[zh].get(c, "")) for c in CATS)
                lines.append('    {"%s", {%s}},' % (esc(zh), cells))
                many += 1
            lines.append("};")
        lines.append("")
        # 没有的那一种给空指针和 0：C++ 里不许有零长的数组。
        adds.append('        add("%s", %s, %s, %s, %s);' % (
            lang,
            rows_name if flat else "nullptr",
            ("std::size(%s)" % rows_name) if flat else "0",
            forms_name if forms else "nullptr",
            ("std::size(%s)" % forms_name) if forms else "0"))
    lines += [
        "inline const std::vector<Table>& all_tables() {",
        "    static const std::vector<Table> t = [] {",
        "        std::vector<Table> out;",
        "        const auto add = [&out](std::string_view lang, const RawRow* rows, std::size_t nr,",
        "                                const RawForms* forms, std::size_t nf) {",
        "            Table one;",
        "            one.lang = lang;",
        "            one.rows.reserve(nr);",
        "            for (std::size_t i = 0; i < nr; ++i) one.rows.emplace(rows[i].zh, std::string(rows[i].said));",
        "            for (std::size_t i = 0; i < nf; ++i) {",
        "                Forms f;",
        "                for (std::size_t k = 0; k < f.size(); ++k) f[k] = std::string(forms[i].f[k]);",
        "                one.plurals.emplace(forms[i].zh, std::move(f));",
        "            }",
        "            out.push_back(std::move(one));",
        "        };",
    ] + adds + [
        "        return out;",
        "    }();",
        "    return t;",
        "}",
        "",
        "#define kTables (all_tables())",
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"{OUT.name}：{len(files)} 种语言，{total} 句"
          f"，另有 {many} 句带数的（每句几档）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
