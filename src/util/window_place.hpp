#pragma once

// 上次窗口摆在哪儿，这次还能不能照着摆。
//
// 「窗口位置大小记住」是方案第七节「外壳杂事」里的一条。**难的不是存，是
// 存下来那一份可能已经不作数了**：
//
//   · 昨天接着外接屏，窗口摆在右边那块上；今天拔了线——照着摆的话窗口落在
//     一块不存在的屏幕上，**屏幕上什么都没有**，而程序自己觉得开着；
//   · 屏幕换了分辨率、或者换了一台小的，窗口有一大半在外面；
//   · 标题栏跑到屏幕上边缘外头——这一档最坏：**窗口看得见，但拖不动**
//     （能拖的那条边不在屏幕上），人只能去改配置文件。
//
// 所以存下来那一份要先过一道：**够不够得着**。够不着就当没存过，回默认。
//
// ⚠️ **判据是"够得着"，不是"完全在屏幕里"。** 人自己把窗口拖到探出去一半是
// 常事，那是他要的样子，不该每次开机替他摆正。

#include <algorithm>
#include <vector>

namespace changji::util {

/// 一块矩形（窗口或者屏幕）。
struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// 两块的交集有多大。不相交就是 0×0。
inline Box overlap_of(const Box& a, const Box& b) {
    const int x0 = std::max(a.x, b.x);
    const int y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w);
    const int y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

/// 存下来那一块还够不够得着。
///
/// `need` 是"至少要露出这么大一块"（横竖都算）。默认 120：比标题栏高，
/// 也够手去抓一把。
///
/// 两个条件都要满足：
///
/// 1. **有一块屏幕上露着够大的一角**；
/// 2. **上边缘没跑到那块屏幕的上边缘外头**——标题栏在那儿，它出去了就再也
///    拖不回来。左右探出去无所谓，横着拖得回来。
inline bool box_is_reachable(const Box& saved, const std::vector<Box>& screens,
                             int need = 120) {
    if (saved.w <= 0 || saved.h <= 0) return false;
    for (const Box& s : screens) {
        const Box seen = overlap_of(saved, s);
        if (seen.w < need || seen.h < need) continue;
        if (saved.y < s.y) continue;
        return true;
    }
    return false;
}

}  // namespace changji::util
