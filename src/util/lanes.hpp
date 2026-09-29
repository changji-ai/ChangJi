#pragma once

// 几路并着跑同一个函数，等全部跑完。首帧、出片、配音那三层共用。
//
// **一路里抛出来的异常不能从线程体里漏出去**：漏出去就是 std::terminate，
// 整个引擎没了（2026-09-25 审出来：一镜出完落盘那一下 project.json 读不出来、
// 盘满了、片子目录被挪走，都会从某一路里抛出来）。这儿每一路自己兜住，全部
// join 完之后在**调用线程上**把第一个重新抛出去——和只有一路时直接抛是同一个
// 样子，上面那层照常接得住、照常报错。
//
// 一路抛了，别的几路照跑完手上那一件：它们各自有自己的令牌，要停由调用方停。

#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace changji::util {

inline void run_lanes(int lanes, const std::function<void()>& lane) {
    if (lanes <= 1) {
        lane();   // 串行那条路一个线程都不起
        return;
    }
    std::exception_ptr first;
    std::mutex mu;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(lanes));
    for (int k = 0; k < lanes; ++k) {
        pool.emplace_back([&] {
            try {
                lane();
            } catch (...) {
                const std::lock_guard<std::mutex> lk(mu);
                if (!first) first = std::current_exception();
            }
        });
    }
    for (auto& t : pool) t.join();
    if (first) std::rethrow_exception(first);
}

}  // namespace changji::util
