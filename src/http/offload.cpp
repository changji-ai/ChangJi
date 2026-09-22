#include "http/offload.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace changji::http {

struct Offload::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> threads;
    std::size_t busy = 0;
    bool stopping = false;
};

Offload::Offload() : impl_(new Impl) {
    for (std::size_t i = 0; i < kThreads; ++i) {
        impl_->threads.emplace_back([this] {
            Impl& im = *impl_;
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lk(im.mu);
                    im.cv.wait(lk,
                               [&im] { return im.stopping || !im.queue.empty(); });
                    if (im.stopping && im.queue.empty()) return;
                    job = std::move(im.queue.front());
                    im.queue.pop_front();
                    ++im.busy;
                }
                // **兜住一切。** 这条线程死了就永远少一条，而少了不会有
                // 任何报错——表现是"点了没反应"，而且越用越频繁。
                try {
                    job();
                } catch (...) {
                }
                {
                    std::lock_guard lg(im.mu);
                    --im.busy;
                }
            }
        });
    }
}

Offload::~Offload() {
    stop();
    delete impl_;
}

Offload& Offload::instance() {
    static Offload one;
    return one;
}

void Offload::post(std::function<void()> fn) {
    {
        std::lock_guard lg(impl_->mu);
        if (!impl_->stopping) {
            impl_->queue.push_back(std::move(fn));
            impl_->cv.notify_one();
            return;
        }
    }
    // 已经在关了：当场干掉。**不能默默扔了**——调用方那头有人在等一个
    // 结果，扔了的话那边会一直等到超时。
    try {
        fn();
    } catch (...) {
    }
}

void Offload::stop() {
    {
        std::lock_guard lg(impl_->mu);
        if (impl_->stopping) return;
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    for (auto& t : impl_->threads) {
        if (t.joinable()) t.join();
    }
    impl_->threads.clear();
}

std::size_t Offload::busy() const {
    std::lock_guard lg(impl_->mu);
    return impl_->busy;
}

std::size_t Offload::queued() const {
    std::lock_guard lg(impl_->mu);
    return impl_->queue.size();
}

}  // namespace changji::http
