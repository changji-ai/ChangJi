#pragma once

// 一件活**已经跑了多久**，写成秒表的样子：`0:07` / `12:05` / `1:03:22`。
//
// ⚠️ **别拿 `human_time` 来写这一处。** 那个答的是"还得等多久"，一分钟
// 以上只留整分钟——于是 60 秒到 119 秒之间它一直写着「1 分钟」。而摆在
// 「在想…」前面的这个数，全部意义就是**它一直在动**：一个整整一分钟纹丝
// 不动的数字和一个冻住的数字长得一模一样，而这个程序刚在同一件事上栽过
//（「想了 N 字」那个数从第一拍起就没变过第二次，截图上看着完全正常，
// 直到 2026-09-21 量 CPU 时顺出来）。秒表每一秒都在走。
//
// **一个字都不写，所以不走 SAY。** 冒号和数字在哪种语言里都是这个意思；
// 这个串一秒一变，翻译一遍只会多一份要跟着改的东西。

#include <cstdio>
#include <string>

namespace changji::util {

/// 秒 -> `0:07` / `12:05` / `1:03:22`。
///
/// **负数和 NaN 按 0 算**：两台机器的时钟差几秒、或者引擎那头还没开始计时，
/// 都会算出一个负数——那时候写 `-0:03` 比写 `0:00` 糟得多。
///
/// 分秒**补零**（`0:07` 不是 `0:7`），时钟不补（`1:03:22` 不是 `01:03:22`）：
/// 补了的话这个数的宽度每跨一档就跳一次，而它左边紧挨着一句话。
inline std::string lap_time(double seconds) {
    // `!(seconds > 0)` 而不是 `seconds <= 0`：NaN 两个比较都是假，
    // 写成后者的话 NaN 会一路走到底下的转换里，`static_cast` 的结果未定义。
    long long total = !(seconds > 0) ? 0 : static_cast<long long>(seconds);
    const long long h = total / 3600;
    total -= h * 3600;
    const long long m = total / 60;
    const long long s = total - m * 60;

    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, s);
    } else {
        std::snprintf(buf, sizeof buf, "%lld:%02lld", m, s);
    }
    return buf;
}

}  // namespace changji::util
