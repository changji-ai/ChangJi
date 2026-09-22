// 借不到就排队等。
//
// 这一组钉的是"真排队"这四个字：先来的先走、等的人不插队、拿到了要退票、
// 抛异常也要退票。**退票漏一次的后果最难查**：队伍里留一张永远排在最前面
// 的死票，之后所有人一律排到超时，而那时候卡是闲着的——从现象上完全看不
// 出是排队的问题。
//
// 这里一个真模型都不装：load/unload 是注入的回调（见 scheduler.hpp 开头
// 那段）。策略要在接推理之前测死。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infer/scheduler.hpp"

using namespace changji::infer;
using namespace std::chrono_literals;

namespace {

/// 一个占 need 字节、装卸都不花时间的槽。
SlotSpec fake(Slot slot, std::size_t need, Residency res = Residency::Cached) {
    SlotSpec s;
    s.slot = slot;
    s.residency = res;
    s.vram_estimate = need;
    s.load = [] {};
    s.unload = [] {};
    return s;
}

Scheduler::AcquireOptions waiting(std::chrono::milliseconds ms) {
    Scheduler::AcquireOptions o;
    o.wait = ms;
    return o;
}

}  // namespace

TEST_CASE("挡路的还回来之后，排队的自己会醒") {
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::LLM, 8));
    s.register_slot(fake(Slot::Image, 8));

    auto held = s.acquire(Slot::LLM);  // 攥着，两个装不下
    std::atomic<bool> got{false};
    std::thread t([&] {
        auto lease = s.acquire(Slot::Image, waiting(5s));
        got = true;
    });

    // 还攥着的时候不该拿到
    std::this_thread::sleep_for(150ms);
    CHECK_FALSE(got.load());

    held.release();  // 还回去
    t.join();
    // **不靠轮询等到超时，是被叫醒的**：上面那个 5 秒是上限，
    // 真等满 5 秒就说明 give_back 没通知。
    CHECK(got.load());
}

TEST_CASE("等不到就超时，而且说的是排队的话，不是「卡装不下」") {
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::LLM, 8));
    s.register_slot(fake(Slot::Image, 8));

    auto held = s.acquire(Slot::LLM);
    const auto t0 = std::chrono::steady_clock::now();
    try {
        auto lease = s.acquire(Slot::Image, waiting(300ms));
        FAIL("等不到却没抛");
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        // 点名挡路的是谁：用户能据此判断该等还是该去改配置
        CHECK(msg.find("LLM") != std::string::npos);
        CHECK(msg.find("正用着") != std::string::npos);
    }
    // 真等了，不是立刻抛
    CHECK(std::chrono::steady_clock::now() - t0 >= 250ms);
}

TEST_CASE("先来的先走") {
    // **这才是"排队"和"谁醒得早谁拿走"的区别。** 不排的话，一件活可能
    // 一直被后来的插队插到超时，而用户看到的是"我点了半天没反应，
    // 别人点一下就成了"。
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::LLM, 8, Residency::Ephemeral));
    s.register_slot(fake(Slot::Image, 8, Residency::Ephemeral));
    s.register_slot(fake(Slot::Video, 8, Residency::Ephemeral));

    auto held = s.acquire(Slot::LLM);

    std::mutex mu;
    std::vector<std::string> order;
    const auto run = [&](Slot slot, const char* name) {
        auto lease = s.acquire(slot, waiting(5s));
        std::lock_guard lg(mu);
        order.push_back(name);
    };

    std::thread a([&] { run(Slot::Image, "一"); });
    std::this_thread::sleep_for(120ms);  // 让「一」先排上
    std::thread b([&] { run(Slot::Video, "二"); });
    std::this_thread::sleep_for(120ms);

    held.release();
    a.join();
    b.join();

    REQUIRE(order.size() == 2);
    CHECK(order[0] == "一");
    CHECK(order[1] == "二");
}

TEST_CASE("排上队会说前面还有几件") {
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::LLM, 8, Residency::Ephemeral));
    s.register_slot(fake(Slot::Image, 8, Residency::Ephemeral));
    s.register_slot(fake(Slot::Video, 8, Residency::Ephemeral));

    auto held = s.acquire(Slot::LLM);

    std::mutex mu;
    std::vector<int> seen_ahead;
    std::string seen_blocker;

    std::thread a([&] {
        auto o = waiting(5s);
        o.on_queued = [&](int ahead, const std::string& blocker) {
            std::lock_guard lg(mu);
            if (ahead == 0 && !blocker.empty()) seen_blocker = blocker;
        };
        auto lease = s.acquire(Slot::Image, o);
        std::this_thread::sleep_for(200ms);  // 多攥一会儿，让后面那位排着
    });
    std::this_thread::sleep_for(120ms);

    std::thread b([&] {
        auto o = waiting(5s);
        o.on_queued = [&](int ahead, const std::string&) {
            std::lock_guard lg(mu);
            seen_ahead.push_back(ahead);
        };
        auto lease = s.acquire(Slot::Video, o);
    });
    std::this_thread::sleep_for(120ms);

    held.release();
    a.join();
    b.join();

    std::lock_guard lg(mu);
    // 排头轮到了但显存腾不动时，报的是挡路的那个槽的名字——顶栏那句
    // 「排队中，等「LLM」用完」就是拿它拼的。
    CHECK(seen_blocker == "LLM");
    // 后面那位至少被告知过一次"前面还有 1 件"
    bool told_one = false;
    for (const int n : seen_ahead) {
        if (n == 1) told_one = true;
    }
    CHECK(told_one);
    // 而且最后要说一声"不排了"（-1），否则界面上那句"排队中"会一直挂着
    CHECK(seen_ahead.back() == -1);
}

TEST_CASE("装到一半失败也要退票，后面的人照样排得上") {
    // **最难查的那种漏。** 队伍里留一张死票，之后所有人一律排到超时，
    // 而那时候卡是闲着的。
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    SlotSpec bad = fake(Slot::Image, 8, Residency::Ephemeral);
    bad.load = [] { throw std::runtime_error("装不上"); };
    s.register_slot(bad);
    s.register_slot(fake(Slot::Video, 8, Residency::Ephemeral));

    CHECK_THROWS(s.acquire(Slot::Image, waiting(2s)));

    // 队伍要是脏了，这一下会一直等到超时
    const auto t0 = std::chrono::steady_clock::now();
    { auto lease = s.acquire(Slot::Video, waiting(2s)); }
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
}

TEST_CASE("不排队那个签名一点没变：借不到当场抛") {
    // 探一下能不能用的地方走的是它（见 tts_backends 挑后端那段）。
    // 探一下也排队的话，体检页会在别人写字的时候卡上几分钟。
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::LLM, 8));
    s.register_slot(fake(Slot::Image, 8));

    auto held = s.acquire(Slot::LLM);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(s.acquire(Slot::Image), std::runtime_error);
    CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
}

TEST_CASE("借得到的时候不排队，一点不慢") {
    Scheduler s;
    s.set_budget(100);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::Image, 8));

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        auto lease = s.acquire(Slot::Image, waiting(5s));
    }
    // 空队伍那条路上不该有任何等待
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
}

TEST_CASE("一群人同时抢，每个人要么拿到要么超时，不卡死") {
    Scheduler s;
    s.set_budget(10);
    s.set_free_vram_probe([] { return std::nullopt; });
    s.register_slot(fake(Slot::LLM, 8, Residency::Ephemeral));
    s.register_slot(fake(Slot::Image, 8, Residency::Ephemeral));

    std::atomic<int> done{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&, i] {
            for (int n = 0; n < 20; ++n) {
                try {
                    auto lease =
                        s.acquire(i % 2 ? Slot::LLM : Slot::Image, waiting(3s));
                } catch (const std::exception&) {
                }
                ++done;
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(done.load() == 160);
    // 谁也没留着没还
    CHECK(s.lease_count(Slot::LLM) == 0);
    CHECK(s.lease_count(Slot::Image) == 0);
}
