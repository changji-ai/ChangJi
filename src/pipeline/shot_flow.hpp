#pragma once

// 首帧和出片同时跑时，两层之间的那根线。
//
// 以前是「首帧全出完 → 再出片」两段串着走：出片那层等着最后一张首帧，
// 而首帧只要几十秒、出片几分钟——一台只装了出片模型的机器在首帧那一段
// 整段闲着；同一台机器上，最后一两张首帧在跑时别的卡也闲着。用户
// 2026-09-17：「首帧图全部都处理完才能到成片，这样会让大量的 GPU 空闲」。
//
// 现在两层同时开，但**不是对半抢卡**——是首帧优先、出片捡空位：
//
//   1. 还有首帧没开跑时，出片那层一个位置都不去抢（`wait_slack`）。
//      两边从头就抢的话，第一镜的视频会和第二张首帧争同一张卡，
//      首帧整体反而更晚出完——而后面每一镜的视频都等着首帧。
//   2. 最后几张首帧都已经在跑、没有下一张可派了（`unstarted_` 归零），
//      出片那层才醒过来去排队。两张卡时就是用户说的那一幕：最后一张首帧
//      在卡 1 上跑，卡 0 空出来，立刻拿去出片，不等它跑完。
//   3. 轮到某一镜时再看**它自己**的首帧写回了没（`wait_ready`），
//      没写回就等（等的是这儿的条件变量，**不占池里的位置**）。
//
// **首帧失败也划**——出片那层本来就会拿"没有首帧"往下走（纯文生视频 /
// 留着上一张），划不划的区别只是让它现在走还是等到最后一起走。
//
// 两层各自写回 Shot、各自存盘，撞在一起就是一层在改镜头 i、另一层在把
// 整章序列化——所以写回 + 存盘那把锁也放在这儿，两层共用。
//
// **只在池里不止一个位置时用**（pipeline/episode.cpp）：单卡单进程时按
// 阶段分批只切 2 次模型、逐镜交错切 2N 次，而且一个位置也没有第二张卡
// 可填——流水在那儿只有代价没有收益。
//
// **纯逻辑，不含网络、不含文件**，所以能测（tests/unit/test_shot_flow.cpp）。

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "pipeline/jobs.hpp"

namespace changji::pipeline {

class ShotFlow {
public:
    /// `pending` 是还要等首帧的那几镜；不在里面的一问就放行
    /// （它们手里已经有首帧了，或者根本不出首帧）。
    explicit ShotFlow(std::set<std::string> pending)
        : unstarted_(pending.size()), pending_(std::move(pending)) {}

    /// 写回 Shot + 存盘用的锁，两层共用。
    std::mutex& commit_mutex() { return commit_mu_; }

    /// 首帧那层：又领走一镜开跑了。领完最后一镜（没有下一张可派）时，
    /// 出片那层就该醒过来捡空位了——**不等它跑完**。
    void frame_started() {
        bool drained = false;
        {
            std::lock_guard<std::mutex> lg(mu_);
            if (unstarted_ > 0) drained = (--unstarted_ == 0);
        }
        if (drained) cv_.notify_all();
    }

    /// 出片那层：等到"首帧那层没有下一张可派了"。回 false = 取消了。
    ///
    /// **这就是"不抢卡"的全部机制**：在这儿等的线程一个池位置都不占，
    /// 首帧那几路因此能吃满所有的卡。
    bool wait_slack(const CancelToken& tok) {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            if (closed_ || unstarted_ == 0) return true;
            if (tok.cancelled()) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(200));
        }
    }

    /// 首帧那层：这一镜写回了（成没成都算），可以出片了。
    void mark_ready(const std::string& shot_id) {
        {
            std::lock_guard<std::mutex> lg(mu_);
            pending_.erase(shot_id);
        }
        cv_.notify_all();
    }

    /// 首帧那层跑完了（正常、取消、抛异常都算）：剩下的全放行，
    /// 别让出片那层的线程吊在这儿。
    void close() {
        {
            std::lock_guard<std::mutex> lg(mu_);
            closed_ = true;
            unstarted_ = 0;
            pending_.clear();
        }
        cv_.notify_all();
    }

    bool ready(const std::string& shot_id) const {
        std::lock_guard<std::mutex> lg(mu_);
        return closed_ || pending_.count(shot_id) == 0;
    }

    /// 出片那层：等这一镜可以出片。回 false = 取消了——等的过程里每
    /// 两百毫秒看一眼取消标志，「停下」不会被这儿吊住。
    bool wait_ready(const std::string& shot_id, const CancelToken& tok) {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            if (closed_ || pending_.count(shot_id) == 0) return true;
            if (tok.cancelled()) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(200));
        }
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    /// 首帧那层还没领走的镜头数。归零 = 没有下一张可派了。
    std::size_t unstarted_ = 0;
    std::set<std::string> pending_;
    bool closed_ = false;
    std::mutex commit_mu_;
};

}  // namespace changji::pipeline
