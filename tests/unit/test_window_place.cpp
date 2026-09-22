// 上次窗口摆在哪儿，这次还能不能照着摆。
//
// 坏掉的样子分三档，**都是"程序觉得开着，人看不见"**：屏幕拔了、分辨率
// 变小了、标题栏跑到屏幕上头（这一档最坏——看得见但拖不动）。判据写在
// `util/window_place.hpp` 头上。

#include <doctest/doctest.h>

#include "util/window_place.hpp"

using changji::util::Box;
using changji::util::box_is_reachable;
using changji::util::overlap_of;

namespace {
/// 一块 1710×1107 的笔记本屏（这台机器的可用区域，去掉菜单栏和 Dock）。
const Box kLaptop{0, 0, 1710, 1107};
/// 右边接一块 2560×1440 的。
const Box kSecond{1710, -200, 2560, 1440};
}  // namespace

TEST_CASE("窗口摆哪儿：正常那一档照着摆") {
    CHECK(box_is_reachable({120, 80, 1040, 720}, {kLaptop}));
    // 探出去一半也算数——那是人自己拖成那样的，不该每次开机替他摆正。
    CHECK(box_is_reachable({1200, 80, 1040, 720}, {kLaptop}));
}

TEST_CASE("窗口摆哪儿：屏幕拔了就不算数") {
    // 昨天摆在右边那块上，今天只剩笔记本这一块。
    const Box on_second{2000, 300, 1040, 720};
    CHECK(box_is_reachable(on_second, {kLaptop, kSecond}));
    CHECK_FALSE(box_is_reachable(on_second, {kLaptop}));
}

TEST_CASE("窗口摆哪儿：只露一条边不算够得着") {
    // 右边只剩 40 像素——抓不住。
    CHECK_FALSE(box_is_reachable({1670, 300, 1040, 720}, {kLaptop}));
    // 下边只剩 40 像素。
    CHECK_FALSE(box_is_reachable({300, 1067, 1040, 720}, {kLaptop}));
}

TEST_CASE("窗口摆哪儿：标题栏跑到屏幕上头就不算数") {
    // **这一档最坏：窗口看得见，但拖不动**——能拖的那条边不在屏幕上。
    // 露出来的那一块很大（够 120×120），单看第一条会放行。
    const Box above{300, -80, 1040, 720};
    CHECK(overlap_of(above, kLaptop).h > 500);
    CHECK_FALSE(box_is_reachable(above, {kLaptop}));
    // 正好齐平算数。
    CHECK(box_is_reachable({300, 0, 1040, 720}, {kLaptop}));
}

TEST_CASE("窗口摆哪儿：没存过 / 存坏了都回默认") {
    CHECK_FALSE(box_is_reachable({0, 0, 0, 0}, {kLaptop}));
    CHECK_FALSE(box_is_reachable({100, 100, 1040, 720}, {}));
    CHECK_FALSE(box_is_reachable({100, 100, -5, 720}, {kLaptop}));
}

TEST_CASE("交集：不相交就是空的") {
    CHECK(overlap_of({0, 0, 10, 10}, {20, 20, 10, 10}).w == 0);
    CHECK(overlap_of({0, 0, 10, 10}, {5, 5, 10, 10}).w == 5);
    // 贴边不算相交。
    CHECK(overlap_of({0, 0, 10, 10}, {10, 0, 10, 10}).w == 0);
}
