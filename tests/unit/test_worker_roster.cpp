// 「派给谁」的策略。
//
// **这段以前没有测试，而它是多卡跑批里后果最重的一段。**
// 8×L20 上真发生过：一个工作进程 CUDA OOM 崩了，systemd 正在重启它的那
// 几十秒里，十一个镜头连着挑中它，每个的 attempts 加到 3 直接降级
// ——池子里另外七个好好的，一个都没被试过。一条 16 镜的预告片最后 11 镜
// 根本没出成，而日志里只有一行"连不上工作进程"。
//
// 要钉住的就是那件事不会再发生：**一台机器坏了，活换一台跑，
// 而不是把这一镜的重试次数烧光。**

#include <doctest/doctest.h>

#include <chrono>
#include <set>

#include "infer/worker_roster.hpp"

using namespace changji::infer;
using Clock = WorkerRoster::Clock;

TEST_CASE("借出去的会标忙，还回来能再借") {
    WorkerRoster r(2);
    const auto t0 = Clock::now();

    const auto a = r.take({}, t0);
    REQUIRE(a.has_value());
    CHECK(r.busy(*a));

    const auto b = r.take({}, t0);
    REQUIRE(b.has_value());
    CHECK(*b != *a);            // 两个任务不能撞同一个工作进程

    CHECK_FALSE(r.take({}, t0).has_value());   // 两个都借出去了

    r.give_back(*a);
    const auto c = r.take({}, t0);
    REQUIRE(c.has_value());
    CHECK(*c == *a);
}

TEST_CASE("连不上的那个跳过去，换一个") {
    // 这一条就是那 11 个镜头的病根。
    WorkerRoster r(3);
    const auto t0 = Clock::now();

    const auto first = r.take({}, t0);
    REQUIRE(first.has_value());
    r.mark_bad(*first, t0);
    r.give_back(*first);
    CHECK(r.bad(*first));

    // 同一个任务换一个：坏的那个既在 skip 里、又还在冷却，两重都挡着
    const std::set<std::size_t> tried{*first};
    const auto second = r.take(tried, t0);
    REQUIRE(second.has_value());
    CHECK(*second != *first);
}

TEST_CASE("坏了的过了冷却会被放回来") {
    // 不放回来的话，一次抖动就等于永久少一张卡。
    WorkerRoster r(1);
    const auto t0 = Clock::now();

    r.mark_bad(0, t0);
    CHECK_FALSE(r.usable(0, {}, t0));
    CHECK_FALSE(r.usable(0, {}, t0 + kWorkerBadCooldown - std::chrono::seconds(1)));
    CHECK(r.usable(0, {}, t0 + kWorkerBadCooldown));
}

TEST_CASE("全都坏着也要挑一个出来试，不能卡死") {
    // 八个工作进程一起抖（比如机器刚重启），如果一个都不挑，
    // 整批活会停在那儿等冷却——而它们可能已经好了。
    WorkerRoster r(2);
    const auto t0 = Clock::now();
    r.mark_bad(0, t0);
    r.mark_bad(1, t0);

    CHECK(r.worth_waiting({}));            // 值得醒过来
    const auto got = r.take({}, t0);       // 冷却没过也照样挑一个
    REQUIRE(got.has_value());
}

TEST_CASE("每一个都试过了就该挑不出来——那才轮到判这一镜失败") {
    WorkerRoster r(2);
    const auto t0 = Clock::now();
    const std::set<std::size_t> tried{0, 1};

    CHECK_FALSE(r.worth_waiting(tried));   // 别等了，等下去不会有新的
    CHECK_FALSE(r.take(tried, t0).has_value());
}

TEST_CASE("跑成了就把坏标记清掉") {
    WorkerRoster r(1);
    const auto t0 = Clock::now();
    r.mark_bad(0, t0);
    CHECK(r.bad(0));
    r.mark_ok(0);
    CHECK_FALSE(r.bad(0));
    CHECK(r.usable(0, {}, t0));            // 立刻就能再用，不等冷却
}

TEST_CASE("忙着的不会被挑走，哪怕它是唯一没坏的") {
    // 挑走正在跑的那个，两个任务会撞进同一个进程，对面回 409。
    WorkerRoster r(2);
    const auto t0 = Clock::now();
    r.mark_bad(1, t0);

    const auto a = r.take({}, t0);
    REQUIRE(a.has_value());
    CHECK(*a == 0);                        // 挑没坏的那个

    const auto b = r.take({}, t0);         // 0 忙着，只剩坏的 1
    REQUIRE(b.has_value());
    CHECK(*b == 1);
    CHECK_FALSE(r.take({}, t0).has_value());
}

TEST_CASE("pool_lanes：跨机的每台多一路，用来边取产物边派下一镜") {
    using changji::infer::pool_lanes;
    // 全是本机拉起的：一进程一路，没有传输这回事
    CHECK(pool_lanes(1, 0) == 1);
    CHECK(pool_lanes(8, 0) == 8);
    // 一台跨机：两路——一路在拉上一镜的首帧，另一路已经把下一镜派出去
    CHECK(pool_lanes(1, 1) == 2);
    // 本机 + 一台跨机
    CHECK(pool_lanes(2, 1) == 3);
    // 远端数不会超过进程数（防御：算错了也不至于开出一堆空转的线程）
    CHECK(pool_lanes(2, 5) == 4);
}
