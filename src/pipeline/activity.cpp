#include "pipeline/activity.hpp"

#include "pipeline/jobs.hpp"
#include "pipeline/task_board.hpp"
#include "util/say.hpp"

#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace changji::pipeline {
namespace {

/// 这个线程的活，最里层的在最后。
///
/// 是个栈不是一个指针：以后真出现"一件活里面套一件"的时候（比如写全片
/// 里面单独登记每一章），深处的代码该改的是最里层那件，不是最外层。
thread_local std::vector<Activity*> t_stack;

}  // namespace

// ⚠️ **这一层现在只是 `Task` 的壳。**
//
// 2026-09-17 之前它自己管一本账（一个 map、一个递增 id）。那本账答得了
// "此刻在忙什么"，答不了另外三件事：排着还没开始的、已经用了多久、干完的
// 那件花了多少时间——而用户要的任务页面三样都要
// （「正在做的（已经用时…）、排队中的（预计什么时候开始…）、已经做完的
// （耗时）」）。
//
// 两本账并排放着是最糟的一种：同一件活在两处各登记一遍，迟早只改一边。
// 所以这儿整个搬到 `task_board.hpp` 上，**构造即 begin**（短活本来就是
// 拿到就干），老调用点一个字没改就有了用时和完成记录。
struct Activity::Impl {
    /// 自己开的那一件。接管别人的时候是空的。
    std::optional<Task> owned;
    /// 真正操作的那一件。指向 `owned`，或者别人那件。
    Task* task = nullptr;

    Impl(std::string kind, std::string project, std::string episode_id,
         std::string message, std::string slot) {
        owned.emplace(std::move(kind), std::move(message), std::move(project),
                      std::move(episode_id), std::move(slot));
        task = &*owned;
        task->begin();
    }
    explicit Impl(Task& existing) : task(&existing) {}
};

Activity::Activity(std::string kind, std::string project,
                   std::string episode_id, std::string message,
                   std::string slot)
    : impl_(std::make_unique<Impl>(std::move(kind), std::move(project),
                                   std::move(episode_id), std::move(message),
                                   std::move(slot))) {
    t_stack.push_back(this);
}

Activity::Activity(Task& existing) : impl_(std::make_unique<Impl>(existing)) {
    t_stack.push_back(this);
}

Activity::~Activity() {
    // **按值找，不是直接 pop_back。** 正常用法下这就是最后一个，但万一
    // 有人把 Activity 放在成员里、析构顺序不是倒着来，pop_back 会把别人
    // 的那件活从栈上抹掉，而症状是"排队中"写到了另一件活头上。
    for (auto it = t_stack.rbegin(); it != t_stack.rend(); ++it) {
        if (*it == this) {
            t_stack.erase(std::next(it).base());
            break;
        }
    }
}

void Activity::set_message(std::string m) { impl_->task->set_title(std::move(m)); }
void Activity::set_progress(int c, int t) { impl_->task->set_progress(c, t); }
void Activity::set_target(std::string t) { impl_->task->set_target(std::move(t)); }
void Activity::set_note(std::string n) { impl_->task->set_note(std::move(n)); }
void Activity::set_thinking(std::string a) { impl_->task->set_thinking(std::move(a)); }

Task& Activity::task() { return *impl_->task; }

Activity* current_activity() {
    return t_stack.empty() ? nullptr : t_stack.back();
}

void note_queued(int ahead, const std::string& blocker) {
    Activity* a = current_activity();
    if (a == nullptr) return;
    if (ahead < 0) {
        a->set_note("");
        return;
    }
    if (ahead > 0) {
        a->set_note(SAYF("排队中，前面还有 %1 件", std::to_string(ahead)));
        return;
    }
    // 轮到自己了，但显存还腾不动——挡路的那个槽正被用着。
    a->set_note(blocker.empty() ? SAY("排队中")
                                : SAYF("排队中，等「%1」用完", blocker));
}

nlohmann::json running_work() {
    // 长跑的排前面：出片、写全片这种一跑十几分钟的，才是人最想点进去看的。
    nlohmann::json out = jobs().running_jobs();
    for (auto& row : running_activities()) out.push_back(std::move(row));
    return out;
}

CancelLink::CancelLink(CancelToken& worker, Activity& act) : worker_(worker) {
    worker_.link(&act.task().token());
}

CancelLink::~CancelLink() { worker_.link(nullptr); }

}  // namespace changji::pipeline
