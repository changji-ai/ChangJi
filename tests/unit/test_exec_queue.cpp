// 「本机这张卡该轮到谁」的策略。
//
// 要钉住的三件事，每一件错了都不报错：
//
//   一，**本地的活排在外来的前面**。反了的话，别人派一批活过来就能让我
//       自己的一章排到后天——而界面上只会显示"排队中"。
//   二，**已经在跑的不被抢占**。抢了的话那一镜白跑，而且 sd.cpp 那边
//       取消要等到下一次采样回调才生效，中间那段是纯浪费。
//   三，**取号一定要还**。漏一个，后面所有人永远排不到头，表现是整条
//       流水线停在那里不动，日志里一行都没有。

#include <doctest/doctest.h>

#include "infer/exec_queue.hpp"

using namespace changji::infer;

TEST_CASE("一次只有一个在跑") {
    ExecQueue q;
    const auto a = q.join(Origin::Local);
    const auto b = q.join(Origin::Local);

    CHECK(q.ready(a));
    CHECK_FALSE(q.ready(b));      // 还没轮到

    q.begin(a);
    CHECK(q.running());
    CHECK_FALSE(q.ready(b));      // a 在跑，b 还得等

    q.end();
    CHECK(q.ready(b));
}

TEST_CASE("本地的排在外来的前面，哪怕外来的先到") {
    ExecQueue q;
    const auto peer = q.join(Origin::Peer);
    const auto local = q.join(Origin::Local);

    CHECK(q.ready(local));
    CHECK_FALSE(q.ready(peer));
    CHECK(q.ahead_of(peer) == 1);
    CHECK(q.ahead_of(local) == 0);

    q.begin(local);
    q.end();
    CHECK(q.ready(peer));         // 本地跑完了才轮到它
}

TEST_CASE("同一档里按先来后到") {
    ExecQueue q;
    const auto a = q.join(Origin::Local);
    const auto b = q.join(Origin::Local);
    const auto c = q.join(Origin::Local);

    CHECK(q.ahead_of(a) == 0);
    CHECK(q.ahead_of(b) == 1);
    CHECK(q.ahead_of(c) == 2);

    q.begin(a);
    q.end();
    CHECK(q.ready(b));
    CHECK_FALSE(q.ready(c));
}

TEST_CASE("在跑的那一件不会被后来的本地活抢掉") {
    ExecQueue q;
    const auto peer = q.join(Origin::Peer);
    q.begin(peer);                // 外来的先跑上了

    const auto local = q.join(Origin::Local);
    CHECK_FALSE(q.ready(local));  // 等它跑完，不抢

    q.end();
    CHECK(q.ready(local));
}

TEST_CASE("退票之后轮到下一个") {
    ExecQueue q;
    const auto a = q.join(Origin::Local);
    const auto b = q.join(Origin::Local);

    q.leave(a);                   // 取消了、或者等超时了
    CHECK(q.ready(b));
    CHECK(q.waiting() == 1);
}

TEST_CASE("ready 不为真时 begin 不生效") {
    // 防的是"两件活同时被认为在跑"——那正是 sd.cpp 的全局进度回调
    // 会串起来的那个场景。
    ExecQueue q;
    const auto a = q.join(Origin::Local);
    const auto b = q.join(Origin::Local);

    q.begin(b);                   // 还没轮到它
    CHECK_FALSE(q.running());
    CHECK(q.ready(a));            // a 仍然是队头

    q.begin(a);
    CHECK(q.running());
    CHECK(q.waiting() == 1);      // b 还在等
}

TEST_CASE("说得出位子被谁占着") {
    // 界面上那句「排队中，等「别的机器的活」用完」靠它。外来的那种
    // 用户可以去关掉，本机自己的只能等——两者对他的意义不一样。
    ExecQueue q;
    CHECK_FALSE(q.running_origin().has_value());

    const auto peer = q.join(Origin::Peer);
    q.begin(peer);
    REQUIRE(q.running_origin().has_value());
    CHECK(*q.running_origin() == Origin::Peer);

    q.end();
    CHECK_FALSE(q.running_origin().has_value());

    const auto local = q.join(Origin::Local);
    q.begin(local);
    REQUIRE(q.running_origin().has_value());
    CHECK(*q.running_origin() == Origin::Local);
}

TEST_CASE("队列空了之后再来的立刻能跑") {
    ExecQueue q;
    const auto a = q.join(Origin::Local);
    q.begin(a);
    q.end();
    CHECK(q.waiting() == 0);

    const auto b = q.join(Origin::Peer);
    CHECK(q.ready(b));            // 没人跟它抢
    CHECK(q.ahead_of(b) == 0);
}
