#pragma once

// 任务表与工作线程池。
//
// 对应 Python 侧 web/server.py 里的 RunState 和 WriteState 两个任务槽。
// 那边是两个独立的 asyncio.Task 变量，WriteState 的注释明写着
// "跟跑流水线分开，两件事可以同时进行"——所以这里不能只有一条工作线程。
//
// 方案第三节定的是线程池加按类型限流：流水线最多 1 个、写作最多 1 个。
// 用池而不是两条固定线程，是为了以后想并发跑多章时不用改架构。
//
// 取消令牌**按 job 挂，不按线程挂**。/api/stop 和 /api/script/series/stop
// 各自取消对应的 job，互不影响。
//
// 这里**不引用 WebSocket**。消息往哪儿发是 main() 通过 set_sink() 注入的。
// 一是分层：流水线不该知道传输层存在；二是很实际的原因——ws.cpp 要链 Crow，
// 而单元测试目标没链，直接依赖的话这个文件就没法测。

#include "util/say.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::pipeline {

/// 任务种类。一种一个槽，同种不能并发。
enum class JobKind {
    Run,    ///< 跑流水线，对应 RunState
    Write,  ///< 写全片 / 批量排分镜，对应 WriteState
};

const char* to_string(JobKind k);

/// 流水线事件。界面和命令行都靠它显示进度。
struct Event {
    double at = 0.0;      ///< Unix 秒，保留三位小数（对齐 Python 的 round(time(),3)）
    std::string stage;
    std::string kind;     ///< start / progress / shot_done / gate / warn / done / error
    std::string message;
    std::optional<std::string> shot_id;
    int current = 0;
    int total = 0;

    /// **这一镜自己**跑到第几步、一共几步。**C++ 独有，只走 WebSocket。**
    ///
    /// `current` / `total` 是整章的位置（第 21 镜 / 共 22 镜）。镜头墙上
    /// 每张牌要画的是**这一镜**的进度条，拿 21/22 去画的话，正在跑的那一
    /// 镜刚开始就显示 95%，六步走完还是 95%——一个不动的、而且一直是错的
    /// 进度条，比没有更糟。
    ///
    /// **不进 `to_json()`。** 那个是 `/api/run` 用的，逐字节和 Python 对拍，
    /// 多一个字段就是一处破契约（Python 的事件只有 at/stage/kind/message/
    /// shot_id/current/total）。WebSocket 那条 **Python 侧根本没有**
    /// （`web/server.py` 开头写明了不用 WebSocket），所以只在那边加。
    int shot_step = 0;
    int shot_steps = 0;

    /// 这一对数说的是哪个阶段："prep"（搬权重、腾显存）、"sample"（采样）
    /// 还是 "decode"（VAE 分块解码）；空 = 没有阶段这回事（不是进度事件）。
    ///
    /// 三件事的量级完全不同：准备可能是 26/28 段、解码 78 块，而挂了
    /// Turbo 的采样只有 6 步。牌子上不分的话，"成片 26/28" 看着就是"跑了
    /// 28 步"——用户会以为 Turbo 没生效（2026-09-10 已经问过一次了）；
    /// 解码写成"准备"，用户会以为模型又在重载（2026-09-15）。
    /// 值和 infer::phase_name 一致；这一层不依赖 infer，所以是字符串。
    std::string shot_phase;

    /// 采样中途的预览图（`data:image/png;base64,…`），只在 kind == "preview"
    /// 的事件上有。**只广播，不进事件环、不进 to_json()。** 一张几十 KB，
    /// 五百条的环塞不下几张；而 /api/run 的事件数组和 Python 逐字节对拍。
    std::string preview;

    nlohmann::json to_json() const;
};

/// 取消令牌。
///
/// 协作式：工作线程要在耗时循环里主动查。Python 那边靠 asyncio 的
/// CancelledError 在 await 点抛出，效果一样但机制不同——
/// 这里没有 await 点，所以查询的位置要自己安排好。
///
/// 三个地方必须能被打断：sd.cpp 的采样回调、llama.cpp 的生成、ffmpeg 子进程。
class CancelToken {
public:
    void request() { cancelled_.store(true, std::memory_order_relaxed); }
    bool cancelled() const {
        if (cancelled_.load(std::memory_order_relaxed)) return true;
        const CancelToken* p = parent_.load(std::memory_order_relaxed);
        return p != nullptr && p->cancelled();
    }
    void reset() { cancelled_.store(false, std::memory_order_relaxed); }

    /// 挂到上一级那个令牌上。**自己被立起来、或者上一级被立起来，都算停。**
    ///
    /// 为的是"停一件"和"停一整批"能同时成立：任务页面上每一行有自己的叉
    /// （停这一镜），而整批那头还有一个停（停全部）。没有这条的话两者只能
    /// 选一个——把批令牌传给渲染器，单镜就停不了；把单镜令牌传过去，批停
    /// 又要等这一镜跑完，而成片档一镜是几分钟。
    ///
    /// ⚠️ **上一级必须活得比自己久。** 用法只有一种：批跑那一层的令牌在
    /// 栈上，每一镜的令牌挂上去，镜子跑完就没了。
    void link(const CancelToken* parent) {
        parent_.store(parent, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> cancelled_{false};
    std::atomic<const CancelToken*> parent_{nullptr};
};

/// 一个任务槽的状态。
///
/// 字段刻意和 Python 的 RunState / WriteState 并集对齐——两种任务的
/// 快照形状不同（见 snapshot()），但存在同一个结构里省得写两套。
struct JobState {
    bool running = false;
    std::string job_id;                    ///< 每次启动生成一个，WebSocket 按它订阅
    std::optional<std::string> episode_id;
    /// 这一轮在跑哪个项目（目录的绝对路径）。
    ///
    /// **加它是为了顶栏那块"AI 作业中"能点过去。** 任务表是进程一份的，
    /// 而一个进程可以轮流跑好几个项目——只报 episode_id 的话，界面上说
    /// "ep01 正在出片"，用户点过去可能是另一部电影的 ep01。
    std::string project;
    std::chrono::steady_clock::time_point started_at{};

    // 进度
    std::string stage;
    int current = 0;
    int total = 0;
    std::string message;

    // 产物与错误
    std::optional<std::string> output;
    std::vector<std::string> outputs;
    std::optional<std::string> error;

    // 一次跑多章时的队列进度。只跑一章时是 1/1。
    int queue_done = 0;
    int queue_total = 1;

    /// 这一件长跑任务干什么（`start` 的 `title`）。
    ///
    /// **快照里要有它。** 「写」这个槽里跑着六件不同的活（展开正文、写全片、
    /// 批量写剧本、理解故事、从热点写这一章、一键成片），而页面要按"现在
    /// 跑的是哪一件"决定显示什么——项目页那颗「一键成片」靠它认出"正在跑
    /// 的就是我起的那条链"，不然它只能去匹配进度那句话的措辞，而措辞是会
    /// 改的（CLAUDE.md 第八条：同一件事别在两处各写一遍）。
    std::string title;

    // Write 专用
    int done = 0;
    nlohmann::json episodes = nlohmann::json::array();

    /// 事件环。上限 500，对应 Python 的 deque(maxlen=500)。
    std::deque<Event> events;

    /// 手动停止时写进 error 的话。空表示用这一类的默认值。
    std::string stop_message;

    /// 这一轮**还没落定**的镜头。**C++ 独有，不进 snapshot()。**
    ///
    /// 镜头墙上的「排队中」原来只存在浏览器内存里：刷新一下、换个标签页、
    /// 换台设备，排着的全没了，正在跑的那一镜也要等到下一条进度才亮。
    /// 引擎自己一直知道这一轮还有哪几镜没跑完——每个阶段开工时登记一批，
    /// 每落定一镜（shot_done / warn / gate）划掉一个，`/bff/run/pending` 回它。
    std::vector<std::string> pending;

    /// 这份名单是哪个阶段登的。
    ///
    /// **划掉只认这个阶段报的完成**。2026-09-17 之前是"任何带 shot_id 的
    /// 非 progress 事件都划一个"，那会儿流水线严格分阶段，同一时刻只有
    /// 一个阶段在报，这么写没错。首帧和出片改成同时跑之后就错了：出片
    /// 登了 17 镜，首帧每出完一张就划掉一个——于是刚出完首帧的那几镜
    /// 不再显示「排队中」，而**它们和还没轮到的那几镜是同一个状态**
    /// （首帧好了、等出片）。用户 2026-09-17：「排队显示还有有问题」——
    /// 墙上 1~4 号有排队条、5~8 号没有，两拨其实一模一样。
    ///
    /// 空 = 谁报的都划（老行为，给没传阶段的调用方留着）。
    std::string pending_stage;
};

/// 消息汇。job 表产生的进度和终止消息往这里送。
///
/// 默认什么都不做（命令行模式、单元测试都不需要推送）。
/// main() 里接到 ws::hub().broadcast 上。
using Sink = std::function<void(const std::string& job_id, const nlohmann::json& msg)>;

/// **一镜落定就调一次**，让上层把结果落盘。
///
/// 各阶段原来都是"整批跑完再统一写回 Shot、再存一次盘"。那在一批只有几镜
/// 的时候没问题，一章二十二镜、一镜两分钟的时候就是一个小时——这一个小时里：
///
///   * 镜头墙每六秒问一次 `/api/shots`，问到的永远是开跑那一刻的样子。
///     镜头墙上没有缩略图、没有可以点开看的视频，而用户要的正是
///     "已经生产的可以点击播放看效果"。
///   * 进程要是被杀掉（不是优雅停止），磁盘上躺着十几个 mp4，
///     project.json 里一条都没记——下次跑会当成没跑过，全部重来。
///
/// 给了就每出完一镜写回一次并调它；没给就是老行为（整批跑完再写回）。
/// 并发跑时这个调用**在锁里**，写回和存盘都串成一个写者。
using ShotCommit = std::function<void()>;

class JobTable;

/// 任务体能改的那部分状态。
///
/// 不直接把 JobState 交出去，是因为里面有一半字段只该由 job 表自己动
/// （running、job_id、started_at）。任务体拿到整个结构就迟早会去改它们，
/// 而改错的表现是"任务明明跑完了界面还转着圈"。
///
/// 所有方法都是线程安全的，可以从工作线程随便调。
class JobProgress {
public:
    /// 记一条事件：进环形缓冲、更新进度、广播出去。
    void report(Event ev);

    /// 只改一句话，不动进度条。跑长任务时"正在写第 3 章"这种。
    void set_message(std::string m);

    /// 完成数 / 总数。对应 Python 的 writing.done / writing.total。
    void set_done(int done);
    void set_total(int total);

    /// 追加一章的结果。写全片和批量出分镜都靠它，
    /// 每写完一章就往里加一条，界面能边跑边看。
    void add_episode(nlohmann::json ep);

    /// 产物路径。
    void set_output(std::string path);
    void add_output(std::string path);

    /// 当前跑到哪一章，以及队列进度。
    void set_episode_id(std::string id);
    void set_queue(int done, int total);

    /// 记一条错误。**不终止任务**——写全片时一章写砸了不该让前面几章白写。
    void set_error(std::string e);

    /// 登记这一阶段要跑的镜头。见 JobState::pending。
    /// 登记这一轮还没落定的镜头。`stage` 是登记方的阶段名——**只有它报的
    /// 完成才划得掉**（见 JobState::pending_stage）。不传 = 谁报的都划。
    void set_pending(std::vector<std::string> shot_ids, std::string stage = {});

    /// 该停了吗。耗时循环里要主动查。
    bool cancelled() const;

    /// 这个任务的取消令牌。
    ///
    /// 光有 cancelled() 不够：sd.cpp 的采样、llama.cpp 的生成、ffmpeg 子进程
    /// 都要拿到令牌**本身**才能被打断，它们不会回来问 JobProgress。
    /// 只给 cancelled() 的话，点停止要等当前这一镜跑完才有反应——
    /// 成片档一镜就是几分钟。
    ///
    /// 引用一直有效：令牌挂在 job 表的槽上，槽是表的成员，地址不变。
    CancelToken& token();

private:
    friend class JobTable;
    JobProgress(JobTable* t, JobKind k) : table_(t), kind_(k) {}
    JobTable* table_;
    JobKind kind_;
};

/// 任务表。所有公开方法可从任意线程调用。
class JobTable {
public:
    JobTable();
    ~JobTable();

    JobTable(const JobTable&) = delete;
    JobTable& operator=(const JobTable&) = delete;

    /// 任务体。拿到一个能改进度的句柄。
    using Body = std::function<void(JobProgress&)>;

    /// 启动一个任务。同种已经在跑就返回 false，调用方回 409。
    ///
    /// episode_id 只是给快照显示用的，不影响调度。
    ///
    /// stop_message 是手动停止时写进 error 的那句话。留空用这一类的默认值。
    /// 要能按任务指定，是因为同一个槽上跑的两件事说法不一样：
    /// 写全片停了是"已经写好的几章留着"，批量出分镜停了是
    /// "已经出好的分镜留着"。用同一句必然有一半场合是错的。
    ///
    /// `project` 是这一轮跑的项目目录，留空表示不知道（顶栏那块就只显示
    /// 名字、不给跳转）。
    /// `title` 是**这一条长跑任务干什么**，给任务页面那一行用：
    /// 「写全片 · 3 章」「补分镜 · 还缺的 2 章」「出片 · 3 章」。
    ///
    /// **不给就按 kind 报个类别**（「批量写作」「出片」）。类别不够用是
    /// 因为 `JobKind::Write` 这一个槽里跑着**六件**活——展开正文、写全片、
    /// 批量写剧本、理解故事、从热点写这一章、批量补分镜（六处 `start` 都在
    /// `http/batch.cpp`）——一律写「批量」的话，页面上那一行说不出正在干哪一件。
    bool start(JobKind kind, const std::string& episode_id, Body body,
               const std::string& stop_message = "",
               const std::string& project = "",
               const std::string& title = "");

    /// 请求取消。没在跑返回 false，对应 Python 的 {"stopped": false}。
    bool cancel(JobKind kind);

    /// 按**账本上那一行的 id** 取消。找不到对应的槽返回 false。
    ///
    /// **给任务页面上那个叉用的。** 那个叉走 `pipeline::cancel_task(id)`，
    /// 而它只点亮那一行自己的令牌——长跑这一族的 worker 查的是
    /// `p.cancelled()`，也就是**槽**上那个令牌。两个不同的对象，于是叉按
    /// 下去行上写着"正在停…"，活照跑。2026-09-17 实测：批量补分镜按了叉，
    /// 三十多分钟一直跑到自己结束。
    ///
    /// 链接那条（jobs.cpp 里 `act.task().token().link(&slot.token)`）方向是
    /// 反的：它管的是"顶栏按停也能停这一行"，反过来管不着。而 link 不能对
    /// 着挂回去——两个令牌互指，`cancelled()` 会无限递归。
    bool cancel_by_task(std::uint64_t task_id);

    /// 快照。形状与 Python 侧对应的 State.snapshot() 一致。
    nlohmann::json snapshot(JobKind kind) const;

    /// 此刻在跑的那些，一行一个，给顶栏那块"AI 作业中"用。
    ///
    /// **和 snapshot 分开**：那个是一整屏的详情（事件流、产物、每镜状态），
    /// 而这个每两秒往所有连着的浏览器推一次，只能带最少的东西。
    /// 没在跑就是空数组——顶栏那块跟着整个不显示。
    nlohmann::json running_jobs() const;

    bool running(JobKind kind) const;

    /// 这个槽此刻在跑哪个项目（目录绝对路径）。没在跑返回空串。
    ///
    /// ⚠️ **空串有歧义**：既可能是「没在跑」，也可能是「在跑但发起方没告诉
    /// 我们是哪个项目」（start 的 project 是尾参，默认空串）。调用方拿它做
    /// 闸的时候必须 fail-closed——见 post_delete_project。
    std::string running_project(JobKind kind) const;
    /// 这一轮还没落定的镜头。见 JobState::pending。
    std::vector<std::string> pending(JobKind kind) const;
    std::string job_id(JobKind kind) const;

    /// 等所有在跑的任务结束。析构和优雅关停时用。
    void wait_idle();

    /// 装消息汇。要在起任务之前调用。
    void set_sink(Sink s);

    /// 一件长跑任务落定、**槽已经空出来之后**叫一次。
    ///
    /// 「点了就排队」那条靠它接着起下一件（见 http/run.cpp 的 RunQueue）。
    ///
    /// ⚠️ **回调跑在刚跑完那条工作线程上，别在里面直接 `start()`。**
    /// `start()` 会 join 上一轮的线程，而那条线程就是你自己——join 自己当场
    /// 抛 `resource_deadlock_would_occur`。要起下一件就扔给别的线程
    /// （`http::Offload`），那也正是队列那头的做法。
    using IdleHook = std::function<void(JobKind)>;
    void set_idle_hook(IdleHook h);

private:
    struct Slot {
        JobState state;
        CancelToken token;
        std::thread worker;
    /// 这一轮在账本上那一行的 id。**进度和那句现说的话要同步过去**，
    /// 页面上那条进度条画的就是它。0 = 还没起或者已经结完账。
    std::uint64_t task_id = 0;
        /// 线程是否还在跑。
        ///
        /// 跟 state.running 不是一回事：手动停止后 running 立刻变 false
        /// （对齐 Python 的可观测行为，见 cancel()），但线程要跑到下一个
        /// 取消检查点才退。析构和 wait_idle() 要等的是**这个**，
        /// 等 running 的话会在线程还活着的时候就返回，然后析构掉它正在用的成员。
        bool active = false;
    };

    Slot& slot(JobKind k);
    const Slot& slot(JobKind k) const;
    friend class JobProgress;
    void record(JobKind kind, Event ev);
    void emit(const std::string& job_id, const nlohmann::json& msg) const;
    /// 在锁里改一下这个槽的状态。JobProgress 的所有 setter 都走它。
    template <typename F>
    void mutate(JobKind kind, F&& fn);

    mutable std::mutex mu_;
    Sink sink_;
    std::condition_variable idle_cv_;
    IdleHook idle_hook_;
    Slot run_;
    Slot write_;
};

/// 全局单例。接口层各处都要查状态和起任务，逐层传引用不划算。
JobTable& jobs();

// 手动停止时写进 error 的文案。**一件活一句话**，别合并。
//
// 这几句是契约——不是给开发看的日志，是前端直接显示给用户的话
// （界面靠自己按停时立的那面旗子认出"这是人按的停"，不认这些字，
// 见 util/cancel_words.hpp；所以改字不会把取消显示成红报错）。
// 每一句说的都是"这一趟停了，留下的是什么"，而各件活留下的东西不一样：
// 跑流水线留的是镜头，展开正文留的是写好的章，批量写剧本留的是剧本。
// 合成一句必然有一半用户看着不对。
//
// ⚠️ **这几条存的是中文原话，翻在用它的那一行。** 它们是静态存储期的
// 常量，在这儿 `SAY()` 的话语言会被冻在 `speak()` 之前那一刻（见
// `util/say.hpp` 的 `SAY_NOOP`）。真翻的那一处是 `JobTable::cancel`
// （`pipeline/jobs.cpp`）和 `film_join.cpp` 里抛出来的那一处。
// 用例比的也是这些常量本身——中文下 `SAY()` 回的就是原话，比得上。
//
// **两个槽的兜底**（任务没自报文案时 `JobTable::cancel` 用它）：
inline constexpr const char* kRunStoppedMessage =
    SAY_NOOP("已手动停止。已完成的镜头会保留，下次从这里继续。");
inline constexpr const char* kWriteStoppedMessage =
    SAY_NOOP("已手动停止。已经写好的几章留着。");

// **`JobKind::Write` 一个槽里跑着三件写作活，三件活各有各的一条。**
//
// 2026-09-18 撞过一次：展开正文那条被收编成了 `kWriteStoppedMessage`，
// 理由是"字一样"。字一样是巧合，不是同一件事——以后谁觉得那个兜底该贴合
// 「写全片」而改成「已经写好的几章剧本留着。」，展开正文（展开的是章
// **正文**、不是剧本）就会一声不响地跟着改口：用户停掉「展开正文」之后
// 看到的是一句关于剧本的话，全程不报错，用例比的也正是那个常量的值。
// 所以**下面三条眼下有两条同字，那也是三条**，改一条不要顺手改另一条。
//
// 批量出分镜、理解剧情、从热点写这一章那几条不在这儿：它们各自只有一处
// 用，字面量就写在 `http/batch.cpp` 的 `start` 调用上。

/// 「展开正文」（`POST /api/story/chapters`）。留下的是写好的**章正文**。
inline constexpr const char* kStoryChaptersStoppedMessage =
    SAY_NOOP("已手动停止。已经写好的几章留着。");

/// 「写全片」（`POST /api/script/series`）。留下的是已经写完那几章。
inline constexpr const char* kScriptSeriesStoppedMessage =
    SAY_NOOP("已手动停止。已经写好的几章留着。");

/// 「批量写剧本」（`POST /api/script/all`）。留下的是**剧本**。
inline constexpr const char* kScriptAllStoppedMessage =
    SAY_NOOP("已手动停止。已经写好的几章剧本留着。");

/// 取对应种类的停止文案。
const char* stopped_message(JobKind k);

/// 事件环上限。Python 那边是 deque(maxlen=500)。
inline constexpr std::size_t kMaxEvents = 500;
/// 快照里回传的事件条数。Python 那边是 list(events)[-80:]。
inline constexpr std::size_t kSnapshotEvents = 80;

}  // namespace changji::pipeline
