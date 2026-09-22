#pragma once

// 「派给谁」这件事的策略，**不含任何网络代码**。
//
// 拆出来是为了能测。`worker_pool.cpp` 链 httplib，按 CMakeLists 里那条既定
// 做法不进单元测试目标——理由是"逻辑部分都已拆到不依赖网络库的文件里"。
// 换机重试这段逻辑以前没拆，于是从来没被测过，而它恰恰是多卡跑批里
// 最常触发、后果最重的一段：
//
// **8×L20 上真发生过。** 一个工作进程 CUDA OOM 崩了，systemd 正在重启它的
// 那几十秒里，十一个镜头连着挑中它，每个的 attempts 加到 3 直接降级
// ——而池子里另外七个好好的，一个都没被试过。一条 16 镜的预告片最后
// 11 镜根本没出成，日志里只有一行"连不上工作进程"。
//
// 这里管三件事：借一个能用的、把连不上的标坏、坏的过一阵放回来。

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <vector>

namespace changji::infer {

/// 坏掉的工作进程多久之后再试一次。
///
/// systemd 重启一个大概就是这个量级。不放回来的话，一次抖动就等于
/// 永久少一张卡；放得太快则是每次都白等一个连接超时。
inline constexpr std::chrono::seconds kWorkerBadCooldown{30};

/// 谁忙着、谁坏了。**只有下标，没有地址**——地址是池那一层的事。
class WorkerRoster {
public:
    using Clock = std::chrono::steady_clock;

    explicit WorkerRoster(std::size_t n) : slots_(n) {}

    std::size_t size() const { return slots_.size(); }

    /// 这个下标现在能不能派活。
    ///
    /// `skip` 是**这一个任务**已经试过、连不上的那几个：换一个的意思
    /// 就是别再换回它。
    bool usable(std::size_t i, const std::set<std::size_t>& skip,
                Clock::time_point now) const {
        const Slot& s = slots_[i];
        if (s.busy || skip.count(i)) return false;
        if (s.bad_since && now - *s.bad_since < kWorkerBadCooldown) return false;
        return true;
    }

    /// 有没有值得等的：不忙、且不在 skip 里。
    ///
    /// **和 usable 的区别是不看冷却。** 全都坏着的时候也要醒过来去试，
    /// 否则一次全体抖动会让整批活卡在条件变量上。
    bool worth_waiting(const std::set<std::size_t>& skip) const {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].busy && !skip.count(i)) return true;
        }
        return false;
    }

    /// 挑一个借走并标忙。挑不出回空。
    ///
    /// 先挑没坏的；一个没有就挑"坏着但不忙"的——试一次的代价只是一个
    /// 连接超时，而不试的代价是这一镜白白降级。
    std::optional<std::size_t> take(const std::set<std::size_t>& skip,
                                    Clock::time_point now) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (usable(i, skip, now)) {
                slots_[i].busy = true;
                return i;
            }
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].busy && !skip.count(i)) {
                slots_[i].busy = true;
                return i;
            }
        }
        return std::nullopt;
    }

    void give_back(std::size_t i) { slots_[i].busy = false; }

    void mark_bad(std::size_t i, Clock::time_point now) {
        slots_[i].bad_since = now;
    }

    /// 跑成了就把坏标记清掉——它显然是好的。
    void mark_ok(std::size_t i) { slots_[i].bad_since.reset(); }

    bool busy(std::size_t i) const { return slots_[i].busy; }
    bool bad(std::size_t i) const { return slots_[i].bad_since.has_value(); }

private:
    struct Slot {
        /// **一个进程一次只跑一个任务**——它那边也会拒（409），
        /// 这里只是不去撞而已。
        bool busy = false;
        /// 连不上的时刻。有值表示当前被跳过。
        std::optional<Clock::time_point> bad_since;
    };
    std::vector<Slot> slots_;
};

/// 阶段那一层给一个池开几路并发。
///
/// 每个工作进程一路是底线：一路对一个槽，借不到就等。**跨机的每台再
/// 加一路**：跨机那条路上一镜跑完还要把产物拉回来，跨境公网实测
/// 30 KB/s、一张首帧二三十秒，这段时间那台的显卡是闲的。多出来的那
/// 一路在槽刚还回来的那一刻就把下一镜派出去，传输和采样叠着走；同机
/// 的不用——同一个文件系统，没有传输这回事。
inline std::size_t pool_lanes(std::size_t workers, std::size_t remote) {
    return workers + std::min(workers, remote);
}

}  // namespace changji::infer
