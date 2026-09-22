#include "util/say.hpp"
#include "infer/local_exec.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace changji::infer {

namespace {

/// 等着的时候多久回头看一眼取消令牌。
///
/// `CancelToken` 是个 atomic<bool>，没有"变了就叫一声"的路子，所以只能
/// 定期醒过来看一眼。200 毫秒：人点了停止之后感觉不到延迟，
/// 而一件活最长也就等一两分钟，这点唤醒开销可以忽略。
constexpr std::chrono::milliseconds kPollCancel{200};

/// 占着位子的那件活，界面上叫它什么。
///
/// 「排队中，等「别的机器的活」用完」——这句话里那半截就是它。
/// 分本机和外来两种是有用的：外来的那种用户可以去关掉（能力表上把勾
/// 去掉），本机自己的只能等。
std::string blocker_of(const std::optional<Origin>& o) {
    if (!o) return {};
    return *o == Origin::Local ? SAY("本机的活") : SAY("别的机器的活");
}

}  // namespace

LocalExec::Hold::~Hold() { release(); }

LocalExec::Hold::Hold(Hold&& other) noexcept : owner_(other.owner_) {
    other.owner_ = nullptr;
}

LocalExec::Hold& LocalExec::Hold::operator=(Hold&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        other.owner_ = nullptr;
    }
    return *this;
}

void LocalExec::Hold::release() {
    if (owner_ != nullptr) {
        owner_->give_back();
        owner_ = nullptr;
    }
}

LocalExec::Hold LocalExec::enter(Origin o, const OnQueued& on_queued,
                                 pipeline::CancelToken* tok) {
    std::unique_lock lk(mu_);
    const ExecQueue::Ticket t = q_.join(o);

    // **-2 是"还没说过话"**，好让第一次排上队时一定会叫一声（哪怕前面是 0 件，
    // 那种情况说明轮到自己了，走的是下面那条清提示的路）。
    int last_said = -2;
    while (!q_.ready(t)) {
        if (tok != nullptr && tok->cancelled()) {
            // **退票一定要退。** 不退的话后面的人永远排不到头，
            // 表现是整条流水线停在那儿不动，日志里一行都没有。
            q_.leave(t);
            throw std::runtime_error(SAY("取消了"));
        }
        const int ahead = q_.ahead_of(t);
        if (on_queued && ahead != last_said) {
            last_said = ahead;
            const std::string blocker = blocker_of(q_.running_origin());
            // **叫的时候把锁放开。** 那个回调要去写顶栏那本活动账
            // （`pipeline::note_queued` → `Activity::set_note`），
            // 那边有它自己的锁；攥着这把锁进去就是两把锁定不下顺序。
            lk.unlock();
            on_queued(ahead, blocker);
            lk.lock();
        }
        cv_.wait_for(lk, kPollCancel);
    }
    q_.begin(t);

    if (on_queued && last_said >= 0) {
        // 排过队才要清。没排过的话这一句会把别人写的那条提示擦掉——
        // 顶栏那句话是共享的，谁写的谁清。
        lk.unlock();
        on_queued(-1, {});
        lk.lock();
    }
    return Hold(this);
}

void LocalExec::give_back() {
    {
        std::lock_guard lg(mu_);
        q_.end();
    }
    // **notify_all 不是 notify_one。** 等着的那几个里只有队头那一个的
    // `ready()` 会变成真，叫醒一个的话很可能叫醒的不是它——然后所有人
    // 接着睡到下一次轮询超时。
    cv_.notify_all();
}

bool LocalExec::busy() const {
    std::lock_guard lg(mu_);
    return q_.running();
}

std::size_t LocalExec::waiting() const {
    std::lock_guard lg(mu_);
    return q_.waiting();
}

LocalExec& local_exec() {
    static LocalExec one;
    return one;
}

}  // namespace changji::infer
