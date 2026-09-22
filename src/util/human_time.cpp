#include "util/human_time.hpp"

#include "util/say.hpp"

#include <algorithm>
#include <cstdio>

namespace changji::util {

namespace {

/// printf 的 %.Nf 用的是 IEEE 754 的默认舍入（就近取偶），
/// 和 Python 的 format spec 一致。所以这里不能自己写
/// `int(x + 0.5)` 那种四舍五入——恰好在 .5 上两边会给出不同的数。
std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

}  // namespace

/// ⚠️ **别的语言用的是缩写（s / min / h），不是整词。**
///
/// 一句一个 key、空位写成 `%1` 这套记不下复数：英语 1 second / 2 seconds、
/// 俄语 1 секунда / 2 секунды / 5 секунд、阿拉伯语还有双数。时长这种地方
/// 缩写本来就是通行写法（`3 min`、`1.5 h`），拿它绕开复数既省事又不别扭。
/// 中日韩照旧写整词——那几种语言本来就不分复数。
std::string human_time(double seconds) {
    seconds = std::max(0.0, seconds);
    if (seconds < 60.0) return SAYF("%1 秒", fmt("%.0f", seconds));
    const double minutes = seconds / 60.0;
    if (minutes < 60.0) return SAYF("%1 分钟", fmt("%.0f", minutes));
    return SAYF("%1 小时", fmt("%.1f", minutes / 60.0));
}

std::string human_time_precise_as(double seconds, double scale_ref) {
    seconds = std::max(0.0, seconds);
    scale_ref = std::max(0.0, scale_ref);
    if (scale_ref < 60.0) return SAYF("%1 秒", fmt("%.1f", seconds));
    if (scale_ref < 3600.0) {
        const int mins = static_cast<int>(seconds / 60.0);
        const double rest = seconds - mins * 60.0;
        // 整分钟就别拖个 " 0.0 秒" 的尾巴。
        if (rest < 0.05) return SAYF("%1 分", std::to_string(mins));
        return SAYF("%1 分 %2 秒", std::to_string(mins), fmt("%.1f", rest));
    }
    const int hours = static_cast<int>(seconds / 3600.0);
    const int mins = static_cast<int>((seconds - hours * 3600.0) / 60.0);
    if (mins == 0) return SAYF("%1 小时", std::to_string(hours));
    return SAYF("%1 小时 %2 分", std::to_string(hours),
                std::to_string(mins));
}

std::string human_time_precise(double seconds) {
    return human_time_precise_as(seconds, seconds);
}

}  // namespace changji::util
