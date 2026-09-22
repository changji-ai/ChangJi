// 「多久之前」那几个字：全是边界，而界面上只看得见一句话。
//
// 错了的表现是"时间有点怪"——没人会去查，所以在这儿钉住。

#include <doctest/doctest.h>

#include "util/when_words.hpp"

using namespace changji;

namespace {
constexpr std::int64_t kMin = 60'000;
constexpr std::int64_t kHour = 3600'000;
constexpr std::int64_t kDay = 86400'000;
}  // namespace

TEST_CASE("多久之前：越近说得越细") {
    const std::int64_t now = 1'700'000'000'000;

    CHECK(util::when_word(now, now) == "刚刚");
    CHECK(util::when_word(now - 59'000, now) == "刚刚");
    CHECK(util::when_word(now - kMin, now) == "1 分钟前");
    CHECK(util::when_word(now - 59 * kMin, now) == "59 分钟前");
    CHECK(util::when_word(now - kHour, now) == "1 小时前");
    CHECK(util::when_word(now - 23 * kHour, now) == "23 小时前");
    CHECK(util::when_word(now - kDay, now) == "昨天");
    CHECK(util::when_word(now - 2 * kDay, now) == "2 天前");
    CHECK(util::when_word(now - 29 * kDay, now) == "29 天前");
    CHECK(util::when_word(now - 60 * kDay, now) == "2 个月前");
    CHECK(util::when_word(now - 400 * kDay, now) == "1 年前");

    SUBCASE("往后的时间当刚刚") {
        // 机器时钟往回调过、两台机器差几秒，都会让 `at` 比 `now` 大。
        // **说「-1 分钟前」比说「刚刚」糟得多。**
        CHECK(util::when_word(now + 5'000, now) == "刚刚");
        CHECK(util::when_word(now + kDay, now) == "刚刚");
    }

    SUBCASE("没有时间就什么都不说") {
        // 老对话文件里可能没有 `at`——**空着**，别写一个「1970 年」出来。
        CHECK(util::when_word(0, now).empty());
        CHECK(util::when_word(-1, now).empty());
    }
}
