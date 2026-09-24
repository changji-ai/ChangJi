#pragma once

// 引擎现在在干什么、待会儿要干什么、刚才干完了什么——**一本账，三个状态**。
//
// 用户 2026-09-17：「再增加任务页面显示正在做的（已经用时，结束图标按钮）、
// 排队中的（预计什么时候开始，取消图标按钮）、已经做完的（耗时），如果是
// 大模型有思考的还得显示思考点击展开思考内容」。
//
// 在这之前引擎里有两本半账，各答一半问题：
//
//   · `jobs.hpp` 的任务表：长跑任务（出片、写全片），一种一个槽，有取消
//     令牌、有事件流。答的是"那一条大任务跑到哪儿了"。
//   · `activity.hpp` 的短活：出一张参考图、写一章、朗读一段，构造即登记、
//     析构即划掉。答的是"引擎此刻在忙什么"。
//   · 半本是各处自己排的队（出图那一批、一章里的几十镜），**只在自己那一
//     页上看得见**，而且一件干完就没了。
//
// 三样都缺同一件事：**排着还没开始的看不见，干完了的也留不下**。于是页面
// 上只能报"正在画的那几张"，说不出还排着几件、刚才那件花了多久。
//
// 这本账把三个状态收在一处：
//
//   排队中 → 正在做 → 做完了 / 失败 / 取消了
//
// **一件活一个 `Task` 对象**，往栈上一放：构造登记成"排队中"，`begin()`
// 变"正在做"（开始计时），析构按结果记进"做完的"。中途抛异常也不会留下
// 幽灵行——这一条和 `Activity` 是同一个理由，那边已经验过两年。
//
// `Activity` 现在是这儿的一层壳（构造即 begin），所有老调用点一个字没改就
// 有了用时和完成记录。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pipeline/jobs.hpp"   // CancelToken

namespace changji::pipeline {

/// 一件活现在是什么状态。
enum class TaskState { Queued, Running, Done, Failed, Cancelled };

const char* to_string(TaskState s);

/// 一件活。构造即排队，`begin()` 即开工，析构即结账。
///
/// **不可拷贝、不可移动**：账本里存的是它的 id，而析构是结账的唯一出口。
/// 要放进容器就用 `std::unique_ptr<Task>`（出图那一批就是这么排的）。
class Task {
public:
    /// `kind` 是界面上那个小标签认得的类型（image / video / tts / llm /
    /// run / write），和顶栏 JobBadge 里的 KIND 表是同一套。
    ///
    /// `title` 是**这件活干什么**，要能一眼看懂：「画参考图 · 董平 正面」、
    /// 「出首帧 · 第 3 章 sh017」、「配音 · 唐海「你说过会来的」」。
    /// 用户 2026-09-17：「任务名要显示清楚干什么的」。
    ///
    /// `project` 是项目目录的绝对路径（页面靠它点过去、也靠它过滤）。
    /// `slot` 是**这件活动的是界面上哪一格**（`story` / `shots` / …）。
    /// 空着的话由 `kind` 算（`util/slot_of_task_kind`）——**只有 `image`
    /// 需要自己说**，它一词两用（参考图在设定那一格，出首帧在镜头那一格）。
    Task(std::string kind, std::string title, std::string project = {},
         std::string episode_id = {}, std::string slot = {});
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;

    /// 轮到它了。**从这一刻开始计时**，页面上那个"已经用时"就是它。
    ///
    /// `working = false` 是「**领到活了，但还没轮到算力**」：出图出片那几路
    /// 比真正的位置多开一路（`infer::pool_lanes`：跨机的每台再加一路，为的
    /// 是拉产物那几十秒里把下一件先派出去），那一路会先把下一镜领走，然后
    /// 在池子上等。
    ///
    /// **「派出去了」不等于「正在画」**（CLAUDE.md 第九条）。不分的话，
    /// 一张卡在画，任务页上写着「2 件在跑」，两行都在走秒——2026-09-20
    /// 实测：sh002 的进度 0/6→1/6 在动，sh003 五分钟里一直是 0/0。
    ///
    /// 什么时候变成"真在画"：**它自己报进度那一刻**（`set_progress`）。
    /// 这条判据和参考图队列那儿用的是同一条。
    void begin(bool working = true);

    /// **对面开始为这一件干活了**（拿到位置、开始读权重、开始采样都算）。
    ///
    /// `begin(false)` 之后靠它转正。判据是"它自己报了一声"——和参考图队列
    /// 那条一样（CLAUDE.md 第九条：真在画的自己会报进度）。
    ///
    /// **不能只认采样那一声**：冷启动时读权重要一分钟（实测远端 59 秒），
    /// 那一分钟里它确实在为这一镜干活，报成「在等一个位置」是另一种假话。
    void working_now();

    void set_title(std::string t);
    void set_progress(int current, int total);
    /// 盖在标题上的一层：「排队中，前面还有 2 件」。轮到了就清掉。
    void set_note(std::string n);
    /// 这件活画的是哪一格参考图。见 Activity::set_target。
    void set_target(std::string t);

    /// 大模型想到哪儿了。**覆盖式，只留最后一份**：思考流是一个字一个字
    /// 来的，每来一段存一条的话，一次写作就是几千条。页面点开看的是"到现
    /// 在为止想了什么"，那正是最后这一份。
    void set_thinking(std::string all);

    /// 又想了一段。**思考流是一段一段来的**，接起来存。
    ///
    /// 有上限（几十万字节）：一次长写作能想上万字，而这份东西还要留在
    /// "做完的"里两百条。超了就从头上截，留最近这一段——人点开看的是
    /// "它最后在想什么"。
    void append_thinking(const std::string& piece);

    /// 到现在为止写出来的东西（给人读的那一份，不是模型吐的原文）。
    ///
    /// **覆盖式，整份换**，不像思考那样接着攒：写的东西是从一段还没写完的
    /// JSON 里抠出来排好的，后到的一个字段可能改掉前面那一行的样子（先有台词、
    /// 后有说话人），只能每次整份重排。每换一次版本号加一，取的那头拿版本号
    /// 判断要不要重取（`task_output`）。
    ///
    /// 有上限：太长就只留最后那一截——人看的是"正写到哪儿"。
    void set_output(std::string all);

    /// 砸了。记一句原话，析构时按"失败"结账。
    void fail(std::string why);

    /// **这一件在长跑任务表里也有一份。**
    ///
    /// `JobTable::start` 起的那条（出片、批量写作）两边都登记：任务表那份
    /// 带着阶段和引擎现说的那句话，账本这份带着用时、思考和取消。顶栏那块
    /// 牌子把两边接成一个列表（`running_work`），**不打这个记号的话同一件
    /// 活在那儿会数两遍**——用户 2026-09-17：「后面的数字和正在做的也对
    /// 不上」，页面上「正在做 6」而顶栏写着 8，多出来的正是这两条。
    void mark_long_job();

    /// 页面上按了那个叉。
    bool cancelled() const;
    CancelToken& token();

    std::uint64_t id() const { return id_; }

private:
    std::uint64_t id_ = 0;
};

/// 按 id 改进度 / 改那句现说的话。
///
/// **给长跑任务那条用的。** 它的进度在 `JobTable` 那张表里（第几镜、第几
/// 章），而账本这份要拿来画进度条；两处各记一份必然只改一边，所以由任务表
/// 每次改完顺手同步过来。找不到（已经结完账）就什么都不做。
void set_task_progress(std::uint64_t id, int current, int total);
void set_task_note(std::uint64_t id, std::string note);

/// 一件活的身份。**给深处的代码认"我这一下是在给谁干活"用。**
///
/// `llm/call_log.cpp` 记提示词日志时要把这四样写进索引——研究那份日志时，
/// 「这次调用是哪一步、为哪一部电影的哪一章发的」正是最先要回答的问题。
///
/// 为什么不从 `Activity` 上直接读：`Activity` 只露了 `task()`，`Task` 只露了
/// `id()` / `cancelled()` / `token()`，这四样都躺在 task_board.cpp 里那个匿名
/// 的 `Row` 上。加一个只读的出口比把 `Row` 搬出来省事得多，也不用动
/// activity.hpp（它被 http 那边一大片文件包着）。
struct TaskFacts {
    std::string kind;
    std::string title;
    std::string project;     ///< 项目目录的绝对路径
    std::string episode_id;
    std::string slot;        ///< 界面上哪一格（见 Task 的构造）
};

/// 按 id 取。**已经结完账（不在 live 里）就回空**——和 `set_task_progress`
/// 找不到就什么都不做是同一个形状。
std::optional<TaskFacts> task_facts(std::uint64_t id);

/// 页面上按了「取消」/「结束」。找不到（已经结完账了）就回 false。
///
/// **排队中的直接从队里划掉，正在做的把令牌立起来**——两种都叫"取消"，
/// 但前者当场就没了，后者要等那一层自己查令牌。
bool cancel_task(std::uint64_t id);

/// 这一刻的账：`{running:[…], queued:[…], done:[…]}`。
///
/// `project` 非空时只报那一部电影的（页面上那一页是跟着项目走的）。
/// 每一行带 `id / kind / title / note / project / episode_id / current /
/// total / seconds / thinking / error / state`。
nlohmann::json task_board(const std::string& project = {});

/// 那件活到现在为止想了什么。
///
/// **单独一条路**：思考动辄十几万字，塞进上面那份每秒多次推的账里的话，
/// 它一件就能把整条通道占满。页面点开才来取。
///
/// **而且是按段取的。** `from` 是调用方手上已经有的字节数（绝对位置，从这
/// 件活开工算起）；回的是从那儿往后的新增。整份重取的话，一件十五万字的活
/// 每一拍就要搬十五万字过去——实测页面上那一版就是这么卡的，而且人要的是
/// **最新那一段**，不是从头。
///
/// 回的 `start` 是这一段的绝对起点：正常等于 `from`；**思考太长被从头截过**
/// 时会大于 `from`（见 Task::append_thinking 那个上限），调用方据此知道自己
/// 中间断了一截。`end` 是取完之后的绝对位置，下次拿它当 `from`。
///
/// 还没结账的、和留在"做完的"里的都找得到；再找不到就回 `{0, 0, ""}`。
struct Thinking {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};
Thinking task_thinking(std::uint64_t id, std::size_t from = 0);

/// 那件活到现在为止写出来的东西（`Task::set_output`）。
///
/// `ver` 是调用方手上那份的版本号：**没变就不带正文**（`text` 空、`changed`
/// 为 false），变了才整份给。版本号 0 = 还一个字都没写。
struct Output {
    std::uint64_t ver = 0;
    bool changed = false;
    std::string text;
};
Output task_output(std::uint64_t id, std::uint64_t ver = 0);

/// 那一行想了多少字、写出来的东西换过几次。**给长跑任务那份名单用**
///（`JobTable::running_jobs`）：长跑那一行的思考和写的东西都记在这本账上，
/// 而那份名单是任务表自己拼的，不问一声就报不出来——批量写正文想了一个钟头，
/// 界面上一个字都看不到（2026-09-24 用户：「正文写作也要实时显示思考和写作
/// 内容」）。找不到（已经结完账）就是两个 0。
struct LiveCounts {
    int thinking_chars = 0;
    std::uint64_t output_ver = 0;
};
LiveCounts task_live_counts(std::uint64_t id);

}  // namespace changji::pipeline
