#pragma once

// 「本机这张卡该轮到谁」的策略，**不含任何锁、线程和推理代码**。
//
// 拆出来是为了能测，理由和 `worker_roster.hpp` 一样：带锁的那一半要真起
// 线程才跑得起来，而排队顺序错了的表现是"某一件活永远轮不到"——不报错、
// 不超时、日志里一行都没有。
//
// ---
//
// **为什么需要这一层。** `SdContext::generate` 里那把 `run_mu` 已经保证了
// "一次一件"，但它是一把裸锁，有三件事它做不到：
//
//   一，**没有优先级**。谁先到谁先跑。接上别的机器之后，外来的活会和
//       本机自己的一章抢同一张卡，而本机的活理应先跑——这台是我自己在用。
//   二，**说不出"前面还有几件"**。裸锁上等着的人，界面上那个百分比就停在
//       0 不动，和"卡死了"长得一模一样。调度器那边的排队早就有这句话了
//       （`pipeline::note_queued`），这里也该有。
//   三，**图像和视频是两个 SdContext，两把各自的 run_mu**。而 sd.cpp 的
//       进度回调是全局的（`ActiveGeneration` 单例），两边真同时跑起来
//       进度就串了。粒度取在"本机一个执行位"才对得上那个全局回调。
//
// **不抢占。** 已经在跑的那一件跑完为止——一镜一两分钟，等得起；中途掐断
// 的代价是那一镜白跑，而且 sd.cpp 那边取消要等到下一步采样回调才生效。
// 所以"本地优先"的准确含义是：**排队里本地的排在外来的前面**，
// 不是"本地一来就把外来的踢下去"。
//
// **范围：只管 sd.cpp 那一路**（出图、出片、参考图）。llama.cpp 那边
// （大模型、配音）是能并行跑好几路的，把它们也塞进一个执行位纯属自损。
// 等接上别的机器、要给外来的活限额时，那部分另算。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace changji::infer {

/// 这件活是谁派的。
enum class Origin {
    /// 本机自己的流水线、界面上点的那些。
    Local,
    /// 别的机器派过来的。
    Peer,
};

inline const char* to_string(Origin o) {
    return o == Origin::Local ? "local" : "peer";
}

/// 谁在跑、谁在等、下一个该轮到谁。
///
/// 调用方负责把它护在锁里——这个类自己一个锁都没有（见文件头）。
class ExecQueue {
public:
    using Ticket = std::uint64_t;

    /// 取号。**取了号就一定要还**：拿到执行位的调用 `begin`，
    /// 不等了的调用 `leave`。漏一个的后果是后面的人永远排不到头。
    Ticket join(Origin o) {
        const Ticket t = next_++;
        waiting_.push_back(Entry{t, o, seq_++});
        return t;
    }

    /// 不等了。超时、取消、中途抛异常都走这儿。
    void leave(Ticket t) {
        waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(),
                                      [t](const Entry& e) { return e.ticket == t; }),
                       waiting_.end());
    }

    /// 轮到 t 了吗：没人在跑，而且 t 排在最前面。
    bool ready(Ticket t) const {
        if (running_) return false;
        const Entry* head = front();
        return head != nullptr && head->ticket == t;
    }

    /// 开跑。**只有 `ready(t)` 为真时才允许调**——不是的话什么也不做，
    /// 免得两件活同时被认为在跑。
    void begin(Ticket t) {
        if (!ready(t)) return;
        const Entry* e = find(t);
        running_origin_ = e != nullptr ? e->origin : Origin::Local;
        leave(t);
        running_ = true;
    }

    /// 跑完了，放给下一个。
    void end() { running_ = false; }

    /// 此刻占着执行位的是谁派的活。没人在跑就是 nullopt。
    ///
    /// 界面上那句「排队中，等「…」用完」要它：等着的人有权知道卡被
    /// 本机自己的活占着，还是被别的机器派来的活占着——后者是可以去
    /// 关掉的（那张表上把这个能力的勾去掉），前者只能等。
    std::optional<Origin> running_origin() const {
        if (!running_) return std::nullopt;
        return running_origin_;
    }

    /// t 前面还有几件。不在队列里（已经在跑或者退票了）返回 0。
    int ahead_of(Ticket t) const {
        const Entry* me = find(t);
        if (me == nullptr) return 0;
        int n = 0;
        for (const Entry& e : waiting_) {
            if (e.ticket != t && earlier(e, *me)) ++n;
        }
        return n;
    }

    bool running() const { return running_; }
    std::size_t waiting() const { return waiting_.size(); }

private:
    struct Entry {
        Ticket ticket = 0;
        Origin origin = Origin::Local;
        /// 取号顺序。同一档里按它先来后到。
        std::uint64_t seq = 0;
    };

    /// a 是不是排在 b 前面。
    ///
    /// **本地整档压在外来的前面**，同档按取号顺序。这条就是"本地优先"
    /// 的全部含义——注意它只决定排队顺序，不动已经在跑的那一件。
    static bool earlier(const Entry& a, const Entry& b) {
        if (a.origin != b.origin) return a.origin == Origin::Local;
        return a.seq < b.seq;
    }

    const Entry* front() const {
        const Entry* best = nullptr;
        for (const Entry& e : waiting_) {
            if (best == nullptr || earlier(e, *best)) best = &e;
        }
        return best;
    }

    const Entry* find(Ticket t) const {
        for (const Entry& e : waiting_) {
            if (e.ticket == t) return &e;
        }
        return nullptr;
    }

    std::vector<Entry> waiting_;
    bool running_ = false;
    Origin running_origin_ = Origin::Local;
    Ticket next_ = 1;
    std::uint64_t seq_ = 0;
};

}  // namespace changji::infer
