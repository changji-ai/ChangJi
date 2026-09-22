#pragma once

// 流到一半的 Markdown 记号，别让它以星号的样子待在屏幕上。
//
// 模型是一个字一个字往外吐的，`**第 3 章 · 19 镜**` 这样一段在路上要经过
// 这么几个样子：
//
//     读完了。*
//     读完了。**
//     读完了。**第 3 章
//     读完了。**第 3 章 · 19 镜**      ← 到这一下才是粗的
//
// 中间那三档**每一档都在屏幕上停一会儿**（真模型一次几十毫秒到几百毫秒，
// 一段话里有三处加粗就闪三次）。停着的样子是一对光秃秃的星号——人看见的是
// 「它打错字了」，不是「它在打字」。2026-09-21 拿假模型按 700ms 一片放慢了
// 截的，`读完了。**` 那一屏是真的。
//
// 两条规矩：
//
// 1. **记号刚开了个头、后面还没有字**（正文就停在 `**` 上）：直接把它摘掉。
//    这一档不能靠"补一个收尾"——`**` 补成 `****` 在 CommonMark 里是**四个
//    原样的星号**（空的强调不成立），比原来还难看。
// 2. **开了头、后面已经有字**：补一个收尾，让那几个字**现在就是粗的**。
//    补收尾比摘开头好，差别在真正的那半个到了的那一瞬间：补过的什么都不
//    动，摘过的要从常规跳成粗体，整行跟着重排一次。
//
// **只在还在长的那一条上做。** 长完了的那一条原样显示——模型真写了一对
// 落单的星号是它自己的事，替它猜反而会把后面一整段变成粗体。
//
// 反引号（`` ` ``）同理：报错原文常带着它，而它落单的样子同样是一个记号。

#include <cstddef>
#include <string>

namespace changji::util {

/// 数一数 `mark` 在 `s` 里出现了几次（不重叠）。
inline std::size_t count_marks(const std::string& s, const std::string& mark) {
    std::size_t n = 0;
    for (std::size_t at = s.find(mark); at != std::string::npos;
         at = s.find(mark, at + mark.size())) {
        ++n;
    }
    return n;
}

/// 还在长的那一条正文，收拾成"现在就能看"的样子。
inline std::string close_open_marks(const std::string& s) {
    std::string t = s;

    // 结尾孤零零一个 `*`：那是 `**` 的前半个，刚到一半。
    // **前面那个字符不能也是 `*`**——那样的话它是一对完整的开头，归下面管。
    if (t.size() >= 1 && t.back() == '*' && (t.size() == 1 || t[t.size() - 2] != '*')) {
        t.pop_back();
    }

    for (const std::string mark : {std::string("**"), std::string("`")}) {
        // ⚠️ **围栏（```）不归这儿管。** 三个反引号按单个数永远是奇数，
        // 补一个上去就把围栏拆了。围栏没收尾时中间那几行照旧当代码画，
        // 那是对的——它本来就是代码。
        if (mark == "`" && t.find("```") != std::string::npos) continue;
        if (count_marks(t, mark) % 2 == 0) continue;
        // 正文就停在这个记号上：摘掉（见上面第 1 条）。
        if (t.size() >= mark.size() && t.compare(t.size() - mark.size(), mark.size(), mark) == 0) {
            t.erase(t.size() - mark.size());
            continue;
        }
        // 补在**最后一个非空白之后**：补到换行后面的话，屏幕上会凭空多一行。
        const auto tail = t.find_last_not_of(" \t\r\n");
        if (tail == std::string::npos) continue;  // 全是空白，没什么可收的
        t.insert(tail + 1, mark);
    }
    return t;
}

}  // namespace changji::util
