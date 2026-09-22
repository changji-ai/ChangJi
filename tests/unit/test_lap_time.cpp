// 「已经跑了多久」那个秒表串。
//
// 为什么值得一条用例：界面上它只是一个不起眼的小数字，**错了没人会去查**
// ——最多觉得"时间有点怪"。而它全是边界（跨分、跨时、负数、零头），
// 和 `when_words` 那一族是同一个道理。

#include <cmath>

#include <doctest/doctest.h>

#include "util/lap_time.hpp"

using namespace changji;

TEST_CASE("秒表串：分秒补零，时钟不补") {
    CHECK(util::lap_time(0) == "0:00");
    CHECK(util::lap_time(7) == "0:07");
    CHECK(util::lap_time(59) == "0:59");

    SUBCASE("跨分那一下") {
        // **59 和 60 必须长得不一样。** 这个数摆出来的全部意义就是它在动。
        CHECK(util::lap_time(60) == "1:00");
        CHECK(util::lap_time(61) == "1:01");
        CHECK(util::lap_time(725) == "12:05");
    }

    SUBCASE("跨时那一下：分钟补零，小时不补") {
        CHECK(util::lap_time(3599) == "59:59");
        CHECK(util::lap_time(3600) == "1:00:00");
        CHECK(util::lap_time(3802) == "1:03:22");
        // 一章出片一个钟头起，两位数的小时真会出现。
        CHECK(util::lap_time(36000) == "10:00:00");
    }

    SUBCASE("零头往下取，不四舍五入") {
        // 59.9 秒写成「1:00」的话，这个数会在到点之前先跳一下，
        // 而下一拍又跳回去——一个来回摆的秒表比不动的还怪。
        CHECK(util::lap_time(59.9) == "0:59");
        CHECK(util::lap_time(0.9) == "0:00");
    }

    SUBCASE("负数和 NaN 按 0 算") {
        // 两台机器的时钟差几秒、引擎那头还没开始计时，都会算出负数。
        // 写「-0:03」比写「0:00」糟得多。
        CHECK(util::lap_time(-1) == "0:00");
        CHECK(util::lap_time(-3600) == "0:00");
        CHECK(util::lap_time(std::nan("")) == "0:00");
    }
}
