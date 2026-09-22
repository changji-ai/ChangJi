#pragma once

// 正在干的那些**短活**。
//
// 任务表（jobs.hpp）管的是长跑任务：出片、写全片。它们有取消令牌、有事件
// 环、有快照接口，一种只能有一个。而引擎上还有另外一半 AI 活儿是**同步
// 请求**——生成一张参考图、写一章、改一段稿、朗读一段。它们几十秒就回，
// 不需要那一整套。
//
// 但顶栏那块「AI 作业中」要回答的是"引擎现在在忙什么"，而不是"长跑任务表
// 里有什么"。2026-09-11 实地撞上过一次：用户在生成参考图（占着图像槽），
// 另一头的批量写作四章全部失败在「显存不够加载 LLM：「图像」正用着」，
// 而那一刻界面上 `jobs: []`、顶栏一片安静、GPU 占用 0%——挡路的那件事
// 整个不可见，只能登服务器翻日志才查得到。
//
// 所以这里补一本**很轻的账**：谁在干活、干的哪个项目、干到哪儿了。
// 一个 RAII 对象，构造登记、析构划掉——出异常也不会留下幽灵记录。
//
// 刻意**不**做的事：没有取消（这些活本来就短）、没有事件环、没有排队。
// 它只回答一个问题，就是顶栏那一行字。

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace changji::pipeline {

class Task;
// 令牌本体在 pipeline/jobs.hpp。这儿只存引用，前置声明就够——
// 这个头被 http 那边一大片文件包着，不想把 jobs.hpp 也拖进去。
class CancelToken;

/// 一件正在干的短活。构造即登记，析构即划掉。
///
/// 用法就是往栈上一放：
/// ```
/// pipeline::Activity act{"image", project_path, "", "正在画参考图"};
/// ```
/// 中途想改那句话就 `act.set_message(...)`，有进度就 `act.set_progress(...)`。
class Activity {
public:
    /// `kind` 是界面上那个小标签认得的类型，见前端 JobBadge 里的 KIND 表。
    /// `project` 是项目目录的绝对路径（顶栏靠它点过去），留空就只显示名字。
    /// `slot` 是界面上哪一格；留空由 `kind` 算（只有 `image` 要自己说，
    /// 它一词两用——见 `util/task_slot.hpp`）。
    Activity(std::string kind, std::string project, std::string episode_id,
             std::string message, std::string slot = {});

    /// 接管一件**已经登记过**的活。
    ///
    /// 批量那条是"排进队的那一刻就登记，轮到了才开工"（见
    /// `http/ref_gen.cpp` 的 `enroll`）。到了真干活那一层，那儿本来会自己
    /// `Activity act{...}` 再登记一行——**同一件活在页面上就出现两次**，
    /// 一行叫「画参考图 · 董平 正面」、一行叫「正在画参考图」。
    ///
    /// 接管这一条只把它推上本线程那个栈（`note_queued` 要找的就是它），
    /// 不新开一行，也不负责结账——账是排队那头开的，也由那头放手。
    explicit Activity(Task& existing);

    ~Activity();

    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;
    Activity(Activity&&) = delete;
    Activity& operator=(Activity&&) = delete;

    void set_message(std::string m);
    void set_progress(int current, int total);

    /// 这件活画的是哪一格参考图（`char_id_slot` / `location_id_empty`）。
    ///
    /// **给没有 WebSocket 的那条路用的。** 每一格的进度、半成品小图、
    /// 「画完了去重拉这张图」全走 `refs` 那条固定频道，而那是纯 socket 的
    /// ——代理把 Upgrade 掐了就一条消息都不来，设定页上那几格从头到尾一动
    /// 不动。顶栏那份系统表有 REST 的那一份（`/api/system`），只差"这一行
    /// 画的是哪一格"，填上它前端就认得出来了。
    ///
    /// 只有出参考图那一族填。别的活留空，前端照旧不看这一栏。
    void set_target(std::string t);

    /// 盖在那句话上的一层：排队时写「排队中，前面还有 2 件」，轮到了就清掉。
    ///
    /// **盖一层而不是改那句话**，是因为排完了还得说回原来那句（"正在画参考
    /// 图"），而那句话是谁写的、写的什么，排队这一层不知道也不该知道。
    void set_note(std::string n);

    /// 大模型想到哪儿了。页面上那个「思考」点开看的就是它。
    void set_thinking(std::string all);

    /// 底下那件 `Task`。取消令牌、id 都在它身上。
    Task& task();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 把账本那一行的叉，接到真正在干活的那个令牌上。
///
/// **不接的话，任务页面上那个叉是个摆设。** 2026-09-17 实测：点了"结束这
/// 一件"，行上写着"正在停…"，而那条大纲又跑了一分半，思考字数从三万三一路
/// 涨到三万五——它压根没停。
///
/// 断在哪儿：`Activity` 自带一个令牌（账本那一行的叉点的是它），而这一族
/// 接口给大模型的是**外面传进来的那一个**（http 那边 script_route 里的）。
/// 两个不同的对象，点灭一个，另一个照跑。流式那条其实每收一块都查
/// `tok.cancelled()`（见 llm/client.cpp），查的是没人点的那一个。
///
/// 用法：紧跟在 Activity 后面摆一个，让它先于 Activity 析构。
///
///     pipeline::Activity act{"outline", project, "", "正在出大纲"};
///     const pipeline::CancelLink stop_here{tok, act};
///
/// ⚠️ **走的时候一定要摘，这不是可选项。** `CancelToken::link` 那句注释写着
/// "上一级必须活得比自己久"，而这里正好反过来：同步那一支的 `tok` 是
/// script_route 里的 `static thread_local`，活得比 `act` 久得多。不摘的话
/// 它攥着一个已经析构的令牌，下一个请求落到同一条线程上就是野指针。
class CancelLink {
public:
    CancelLink(CancelToken& worker, Activity& act);
    ~CancelLink();
    CancelLink(const CancelLink&) = delete;
    CancelLink& operator=(const CancelLink&) = delete;
    CancelLink(CancelLink&&) = delete;
    CancelLink& operator=(CancelLink&&) = delete;

private:
    CancelToken& worker_;
};

/// 这个线程此刻在干的那件活。没有就是 nullptr。
///
/// **给深处的代码用的。** 排队发生在调度器里（infer/scheduler），而那句
/// "排队中"要写到顶栏的账本上；一层层往下传一个回调的话，llm、sd、tts
/// 三条路每一条都要改签名，而它们和"界面上显示什么"没有半点关系。
Activity* current_activity();

/// 接到 Scheduler::AcquireOptions::on_queued 上：把排队情况写到当前线程
/// 那件活的那句话上。`ahead < 0` 表示不排了（轮到了或者不等了）。
void note_queued(int ahead, const std::string& blocker);

/// 此刻在干的那些短活，一行一个。形状和 JobTable::running_jobs() 一样，
/// 顶栏把两边接成一个列表。**实现在 task_board.cpp**——那本账才是数据在
/// 的地方，这儿只是个老名字的出口。
nlohmann::json running_activities();

/// 引擎此刻在干的**全部** AI 活：长跑任务加短活，接成一个列表。
///
/// **只此一处拼。** 这份东西有两个出口（两秒一次推给浏览器的 system 消息，
/// 和 `/api/system` 那条给人 curl 的），两边各拼一次的话迟早只改一边——
/// 而症状是"界面上明明有任务，curl 出来却是空的"，查起来先怀疑的准是
/// 前端。已经栽过一次了。
nlohmann::json running_work();

}  // namespace changji::pipeline
