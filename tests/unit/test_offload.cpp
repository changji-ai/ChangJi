// 后台干活的池子。
//
// 它存在的理由是一句实测出来的话：Crow 一条 I/O 线程管着一批连接，handler
// 在它上面跑多久，落在同一条线程上的连接就干等多久。所以这里钉的是**把活
// 交出去这件事本身靠不靠得住**：交了一定会干、干砸了不会带走线程、
// 排的比线程多也一件不少。
//
// 交出去之后怎么把结果送回去（WebSocket）不在这儿测——那需要真起一个
// app。⚠️ 但有一条一定要记住：**不许在这些线程上调 res.end()**，
// 见 offload.hpp 和 server.hpp 里那两段。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "http/offload.hpp"

using changji::http::Offload;
using namespace std::chrono_literals;

namespace {

/// 等到条件成立，或者等够了就放弃。别在测试里死等。
bool until(const std::function<bool()>& ok,
           std::chrono::milliseconds cap = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ok()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return ok();
}

}  // namespace

TEST_CASE("交出去的活会被干") {
    std::atomic<int> ran{0};
    Offload::instance().post([&ran] { ++ran; });
    CHECK(until([&ran] { return ran.load() == 1; }));
}

TEST_CASE("post 立刻回来，不等它干完") {
    // **这才是整层的意义。** post 要是等着，那就等于没挪出 I/O 线程。
    std::atomic<bool> done{false};
    const auto t0 = std::chrono::steady_clock::now();
    Offload::instance().post([&done] {
        std::this_thread::sleep_for(300ms);
        done = true;
    });
    CHECK(std::chrono::steady_clock::now() - t0 < 100ms);
    CHECK(until([&done] { return done.load(); }));
}

TEST_CASE("一件活抛异常，不带走那条线程") {
    // 干活的是接口处理函数，它抛 ApiError 是家常便饭。线程死了不会有任何
    // 报错——表现是"点了没反应"，而且越用越频繁。
    Offload::instance().post([] { throw std::runtime_error("砸了"); });
    std::atomic<int> after{0};
    for (int i = 0; i < 20; ++i) {
        Offload::instance().post([&after] { ++after; });
    }
    CHECK(until([&after] { return after.load() == 20; }));
}

TEST_CASE("排的比线程多，一件不少") {
    const int n = static_cast<int>(Offload::kThreads) * 4;
    std::atomic<int> ran{0};
    for (int i = 0; i < n; ++i) {
        Offload::instance().post([&ran] {
            std::this_thread::sleep_for(10ms);
            ++ran;
        });
    }
    CHECK(until([&ran, n] { return ran.load() == n; }, 10s));
}

TEST_CASE("同时在干的不超过线程数") {
    // 有界是刻意的：无界地开线程，一把点下去几十个请求就是几十条线程，
    // 而它们还都要再排一次显存的队。
    std::atomic<int> now{0};
    std::atomic<int> peak{0};
    std::atomic<int> ran{0};
    const int n = static_cast<int>(Offload::kThreads) * 3;
    for (int i = 0; i < n; ++i) {
        Offload::instance().post([&] {
            const int cur = ++now;
            int seen = peak.load();
            while (cur > seen && !peak.compare_exchange_weak(seen, cur)) {
            }
            std::this_thread::sleep_for(20ms);
            --now;
            ++ran;
        });
    }
    CHECK(until([&ran, n] { return ran.load() == n; }, 10s));
    CHECK(peak.load() <= static_cast<int>(Offload::kThreads));
}

TEST_CASE("好几条线程一起交活，不崩不漏") {
    // 真实情况就是这样：几个请求同时进来，各在各的 I/O 线程上。
    std::atomic<int> ran{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&ran] {
            for (int n = 0; n < 25; ++n) {
                Offload::instance().post([&ran] { ++ran; });
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(until([&ran] { return ran.load() == 200; }, 10s));
}
