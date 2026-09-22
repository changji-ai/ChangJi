#pragma once

// 本机的执行位：sd.cpp 那一路**一次只跑一件**，按 `ExecQueue` 的规矩排队。
//
// 策略在 `exec_queue.hpp`（纯逻辑、能测），这里只有锁、条件变量和等待。
// 为什么要这一层、为什么只管 sd.cpp、为什么不抢占，全写在那个文件头里。
//
// **和显存调度器的先后顺序是有讲究的：先拿执行位，再向 `Scheduler` 借槽。**
// 反过来的话会撞上这么一幕：外来的出片任务先借到 Video 槽（借的过程中会
// 去驱逐 Image），而本机自己的出图正攥着 Image 槽在跑、驱逐不掉，于是那个
// 出片任务在显存那儿干等五分钟然后超时——而它本该做的事只是"等本机这一镜
// 出完"。执行位是更粗的那道闸，粗的先过。
//
// 用法就是一行：
//
//     auto hold = infer::local_exec().enter(origin, pipeline::note_queued, &tok);
//     ...向 Scheduler 借槽、跑生成...
//                                      // hold 析构时自动让位

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

#include "infer/exec_queue.hpp"
#include "pipeline/jobs.hpp"

namespace changji::infer {

class LocalExec {
public:
    /// 排队情况变了叫一声，`ahead` 是前面还有几件，`blocker` 是占着位子的
    /// 那件活是谁派的。**`ahead < 0` 表示不排了**（轮到自己了），
    /// 界面上那句「排队中」要清掉。
    ///
    /// 签名和 `Scheduler::AcquireOptions::on_queued` **一模一样**，
    /// 为的就是两处都能直接接 `pipeline::note_queued`，不用各写一个转接。
    using OnQueued = std::function<void(int ahead, const std::string& blocker)>;

    /// 攥着执行位。析构时让位。
    class Hold {
    public:
        Hold() = default;
        ~Hold();

        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        Hold(Hold&& other) noexcept;
        Hold& operator=(Hold&& other) noexcept;

        bool valid() const { return owner_ != nullptr; }

        /// 提前让位。析构时会自动做。
        void release();

    private:
        friend class LocalExec;
        explicit Hold(LocalExec* owner) : owner_(owner) {}
        LocalExec* owner_ = nullptr;
    };

    /// 排队，拿到执行位才返回。
    ///
    /// `tok` 给了的话，等的过程中被点了停止就**抛 std::runtime_error**，
    /// 消息是"取消了"——和工作进程池那边中途取消时的说法一致。
    /// 不给就是一直等（没有超时：挡在前面的最长就是一镜，而"等久了报错"
    /// 对用户来说只是同一件事换个说法，还得他自己再点一次）。
    Hold enter(Origin o, const OnQueued& on_queued = {},
               pipeline::CancelToken* tok = nullptr);

    /// 此刻有活在跑吗、还有几件在等。给诊断和测试用。
    bool busy() const;
    std::size_t waiting() const;

private:
    friend class Hold;
    void give_back();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    ExecQueue q_;
};

/// 进程内那一个。
///
/// **必须是全局的**，因为它挡的那件事是全局的：sd.cpp 的进度回调
/// （`sd_set_progress_callback`）不带 ctx，整个进程只有一份。
LocalExec& local_exec();

}  // namespace changji::infer
