#include "pipeline/jobs.hpp"

#include "pipeline/activity.hpp"
#include "pipeline/task_board.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace changji::pipeline {

namespace {

/// Unix 秒，三位小数。对齐 Python 的 round(time.time(), 3)。
double now_unix() {
    using namespace std::chrono;
    const auto d = system_clock::now().time_since_epoch();
    const double s = duration<double>(d).count();
    return std::nearbyint(s * 1000.0) / 1000.0;
}

/// 一位小数。对齐 Python 的 round(elapsed, 1)。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

std::string new_job_id(JobKind kind) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream os;
    os << to_string(kind) << "-" << std::hex << rng();
    return os.str();
}

json opt_str(const std::optional<std::string>& v) {
    return v.has_value() ? json(*v) : json(nullptr);
}

}  // namespace

const char* stopped_message(JobKind k) {
    return k == JobKind::Run ? kRunStoppedMessage : kWriteStoppedMessage;
}

const char* to_string(JobKind k) {
    switch (k) {
        case JobKind::Run:   return "run";
        case JobKind::Write: return "write";
    }
    return "?";
}

json Event::to_json() const {
    return {
        {"at", at},
        {"stage", stage},
        {"kind", kind},
        {"message", message},
        {"shot_id", opt_str(shot_id)},
        {"current", current},
        {"total", total},
    };
}

JobTable::JobTable() = default;

JobTable::~JobTable() {
    // 先请求取消再等。不取消的话析构会卡在一个可能跑几十分钟的任务上。
    run_.token.request();
    write_.token.request();
    wait_idle();
    if (run_.worker.joinable()) run_.worker.join();
    if (write_.worker.joinable()) write_.worker.join();
}

JobTable::Slot& JobTable::slot(JobKind k) {
    return k == JobKind::Run ? run_ : write_;
}
const JobTable::Slot& JobTable::slot(JobKind k) const {
    return k == JobKind::Run ? run_ : write_;
}

bool JobTable::start(JobKind kind, const std::string& episode_id, Body body,
                     const std::string& stop_message,
                     const std::string& project, const std::string& title) {
    std::unique_lock lk(mu_);
    Slot& s = slot(kind);
    if (s.state.running) return false;

    // 先占坑再干别的。下面 join 上一轮线程时要临时解锁，
    // 不先把 running 立起来的话，那个窗口里第二个 start() 会看到
    // running==false 一起挤进来，两条线程抢同一个槽。
    s.state.running = true;

    // 上一轮的线程可能还没退——正常跑完没来得及 join，或者被手动停止后
    // 还在收尾（那种情况 running 早就是 false 了）。这里一定要等到它真的结束：
    // 不等的话 std::thread 的赋值运算符会调 terminate，
    // 而且新旧两条线程会同时写同一个 state。
    if (s.worker.joinable()) {
        lk.unlock();
        s.worker.join();
        lk.lock();
    }

    s.token.reset();
    s.state = JobState{};
    s.state.running = true;
    s.state.job_id = new_job_id(kind);
    if (!episode_id.empty()) s.state.episode_id = episode_id;
    s.state.project = project;
    s.state.started_at = std::chrono::steady_clock::now();
    s.state.stop_message = stop_message;
    s.active = true;

    const std::string job_id = s.state.job_id;

    const std::string task_title =
        !title.empty() ? title
        : kind == JobKind::Run ? SAY("出片")
                               : SAY("批量写作");
    s.state.title = task_title;
    s.worker = std::thread([this, kind, job_id, project, task_title,
                            body = std::move(body)]() {
        // **长跑任务也要进那本任务账。**
        //
        // 它原来只在自己这张表里（`running_jobs`），于是任务页面上一行都
        // 没有——而「写全片」「出片」正是最该在那儿看的两件：一跑十几分钟、
        // 有思考、要能停。
        //
        // 这儿开一个 `Activity`（它是 `Task` 的壳，构造即开工）：
        //   · 页面上有名字、有已经用时、有那个「结束」；
        //   · **思考自动接上**——`thinking_sink` 认的是"当前线程在干的那件
        //     活"，而底下每一次大模型调用都跑在这条线程上。
        //   · 令牌挂到这个槽的令牌上，页面上按「结束」等于按顶栏那个停。
        pipeline::Activity act{to_string(kind), project, std::string{},
                               task_title};
        act.task().token().link(&slot(kind).token);
        // 顶栏那块牌子把任务表和短活接成一个列表，这一件两边都在——
        // 打个记号，短活那一份跳过它，别数两遍。
        act.task().mark_long_job();
        {
            std::lock_guard lg(mu_);
            slot(kind).task_id = act.task().id();
        }
        JobProgress progress(this, kind);
        try {
            body(progress);
        } catch (const std::exception& e) {
            std::lock_guard lg(mu_);
            slot(kind).state.error = e.what();
        } catch (...) {
            std::lock_guard lg(mu_);
            slot(kind).state.error = SAY("未知异常");
        }

        json final_msg;
        {
            std::lock_guard lg(mu_);
            Slot& sl = slot(kind);
            sl.state.running = false;
            sl.active = false;
            // 跑完了就没有"还没落定"的了。留着的话下一次页面一进来，
            // 会把上一轮剩下的当成还在排队。
            sl.state.pending.clear();
            // done / error 是终止消息，**不受节流影响**——
            // 被节流掉的话前端会永远停在"跑着"的状态。
            //
            // 注意判断顺序：error 先于 cancelled。手动停止时 cancel() 已经
            // 把 error 写成了那句"已完成的镜头会保留"，这里不能再覆盖成"已取消"。
            if (sl.state.error.has_value()) {
                final_msg = {{"type", "error"}, {"job_id", job_id},
                             {"message", *sl.state.error}};
            } else if (sl.token.cancelled()) {
                final_msg = {{"type", "error"}, {"job_id", job_id},
                             {"message", SAY("已取消")}};
            } else {
                final_msg = {{"type", "done"}, {"job_id", job_id},
                             {"outputs", sl.state.outputs}};
            }
        }
        emit(job_id, final_msg);
        idle_cv_.notify_all();
        // **槽空出来之后**才叫，而且是在这条线程上叫——回调里不能直接
        // start（会 join 自己），见 set_idle_hook 上那段。
        IdleHook hook;
        {
            std::lock_guard lg(mu_);
            hook = idle_hook_;
        }
        if (hook) hook(kind);
    });

    return true;
}

void JobTable::set_sink(Sink s) {
    std::lock_guard lg(mu_);
    sink_ = std::move(s);
}

void JobTable::set_idle_hook(IdleHook h) {
    std::lock_guard lg(mu_);
    idle_hook_ = std::move(h);
}

void JobTable::emit(const std::string& job_id, const json& msg) const {
    // 取一份拷贝再调用，别拿着锁进 sink——sink 里是 Hub，Hub 自己有锁。
    Sink s;
    {
        std::lock_guard lg(mu_);
        s = sink_;
    }
    if (s) s(job_id, msg);
}

bool JobTable::cancel(JobKind kind) {
    std::lock_guard lg(mu_);
    Slot& s = slot(kind);
    if (!s.state.running) return false;
    s.token.request();

    // running 立刻置 false，不等工作线程真的退出。
    //
    // 这是抄 Python 的：那边 stop_run() 里是 task.cancel() 之后紧跟着
    // state.running = False。差别是可观测的——前端点完停止马上会拉一次
    // /api/run，如果这里还报 running=true，界面就会卡在"正在跑"上好几秒
    // （工作线程要跑到下一个取消检查点才退）。
    //
    // 代价是 running 不再等价于"线程还活着"。所以 start() 里那句 join
    // 不能删：槽看着空了，上一条线程可能还在收尾。
    s.state.running = false;
    s.state.pending.clear();
    // 任务自己指定的那句优先。**一个槽上跑着好几件活，各说各的**：展开正文
    // 停了是"已经写好的几章留着"，批量写剧本停了是"已经写好的几章剧本留着"，
    // 批量出分镜停了是"已经出好的分镜留着"。下面这句兜底只在任务没自报时用，
    // 一件活一条常量见 jobs.hpp 里 kWriteStoppedMessage 那一族。
    // 翻在这一行：常量里存的是中文原话（见 jobs.hpp 那段）。
    s.state.error = SAY(s.state.stop_message.empty() ? stopped_message(kind)
                                                     : s.state.stop_message);
    return true;
}

void JobTable::record(JobKind kind, Event ev) {
    std::string job_id;
    json msg;

    // 预览图**只广播**：不进事件环（几十 KB 一张，环放不下），不动进度
    // （它不是一步，是一步中间的样子），不进 /api/run（和 Python 对拍）。
    // 老客户端不认识 kind = preview，按 progress 处理也只是多刷一次状态。
    if (ev.kind == "preview") {
        {
            std::lock_guard lg(mu_);
            job_id = slot(kind).state.job_id;
        }
        emit(job_id, json{{"type", "progress"},
                          {"kind", "preview"},
                          {"job_id", job_id},
                          {"stage", ev.stage},
                          {"shot_id", ev.shot_id.value_or("")},
                          {"step", ev.current},
                          {"preview", ev.preview}});
        return;
    }
    {
        std::lock_guard lg(mu_);
        Slot& s = slot(kind);
        JobState& st = s.state;

        // 事件时间戳由这里统一打，调用方不用管。Python 那边是 append 时
        // 取 round(time.time(),3)，同一个位置。
        if (ev.at == 0.0) ev.at = now_unix();

        // 只有带总数的事件才更新进度。不加这个判断的话，
        // 一条 total=0 的日志事件会把进度条清零。
        if (ev.total) {
            // **同一阶段里 current 只进不退。** 多卡时几镜同时在跑，
            // 各自的进度事件带的是自己的序号：3、11、7、12……原样写进去
            // 进度条就来回蹦。Python 那边是串行的，序号天然单调，
            // 所以它直接赋值也对；这里加一道 max，串行时结果一个字不差。
            // 换了阶段就从头来——新阶段的 1/12 当然要比上一阶段的 12/12 小。
            const bool same_stage = st.stage == ev.stage;
            st.current = same_stage ? std::max(st.current, ev.current) : ev.current;
            st.total = ev.total;
        }
        st.stage = ev.stage;
        st.message = ev.message;

        st.events.push_back(ev);
        while (st.events.size() > kMaxEvents) st.events.pop_front();

        // 一镜落定（shot_done / warn / gate……任何带 shot_id 的非 progress）
        // 就从"还没落定"里划掉。和界面 trackInflight 的判据一字不差——
        // 两边判据不一样的话，刷新前后同一镜的「排队中」会不一样。
        //
        // **但只认登记那个阶段报的。** 理由见 JobState::pending_stage：
        // 首帧和出片同时跑时，首帧报的完成不该把出片那份名单划掉。
        if (ev.shot_id.has_value() && ev.kind != "progress" &&
            (st.pending_stage.empty() || ev.stage == st.pending_stage)) {
            auto& pend = st.pending;
            pend.erase(std::remove(pend.begin(), pend.end(), *ev.shot_id),
                       pend.end());
        }

        job_id = st.job_id;
        msg = {
            {"type", ev.kind == "error" ? "error" : "progress"},
            // 原样带上 kind。type 只分 progress / error 两种，warn、gate、
            // shot_done 到了界面全成了 "progress"——界面因此分不出
            // 一个镜头是"还在跑"还是"跑完了"。多卡之后六镜同时在跑，
            // 不带这个字段界面上就是六条进度轮流刷同一个位置。
            // **加字段不改旧字段**，老客户端照旧。
            {"kind", ev.kind},
            {"job_id", job_id},
            {"stage", ev.stage},
            // **推 st.* 而不是 ev.*。** 上面那道「只有带总数的事件才更新
            // 进度」只守住了服务端这份快照，推出去的消息原来带的还是事件
            // 自己的值——于是一条 total=0 的日志事件推出去就是
            // {step:0, total:0}，而两个 store 都是「是数字就收下」，
            // 刚轮询回来的正确值当场被打回零。
            //
            // 推快照里那两个值还顺带解决了多卡时数字来回蹦：st.current
            // 是同阶段内取过 max 的，ev.current 是各镜自己的序号。
            // 串行时两者一个字不差。
            //
            // ⚠️ **而且要按任务种类挑字段**（和 progress_of / snapshot /
            // overview 同一套）：Run 记第几镜在 current，Write 记第几章
            // 在 done。只推 current 的话，写章节这条路推出去恒等于 0——
            // 我 2026-09-12 第一次改这里就漏了这一半，界面照旧是 0/4。
            {"step", kind == JobKind::Run ? st.current : st.done},
            {"total", st.total},
            {"message", ev.message},
        };
        if (ev.shot_id.has_value()) msg["shot_id"] = *ev.shot_id;
        // 这一镜自己的进度。**只在这条路上给**，`/api/run` 的事件数组
        // 要和 Python 一字不差。见 Event::shot_steps 的注释。
        // 没有就不加：镜头墙靠"有没有这两个字段"决定画不画那条进度条，
        // 补个 0 会让每张牌上都挂一条永远空着的槽。
        if (ev.shot_steps > 0) {
            msg["shot_step"] = ev.shot_step;
            msg["shot_steps"] = ev.shot_steps;
            msg["shot_phase"] = ev.shot_phase.empty() ? "sample" : ev.shot_phase;
        } else if (ev.shot_phase == "prep") {
            // **还没进采样：没有步数，但"正在准备"这件事要说。**
            // 借槽那一下是阻塞的——可能先卸大模型腾地方，再从磁盘读
            // 十几二十 GB 进来，几十秒起。这一整段 sd.cpp 还没跑，
            // 它的进度回调一次都不触发，牌子上就是一动不动。
            // 只给阶段、不给步数：上面那条"补个 0 会挂一条空进度槽"
            // 的规矩还在，进度条照样不画。
            msg["shot_phase"] = "prep";
        } else if (ev.shot_phase == "wait") {
            // **工作机都连不上、排着队等它们回来。** 同样没有步数；
            // shot_step 借来装已等的分钟数，**必须带上**——不带的话界面
            // 沿用上一条的步数（掉线前正跑到 30/535），牌子上就成了
            // 「已等 30 分钟」。
            msg["shot_phase"] = "wait";
            msg["shot_step"] = ev.shot_step;
        }
    }
    // 广播放在锁外：Hub 自己有锁，嵌套两把锁是死锁的常见来源。
    emit(job_id, msg);
}

std::vector<std::string> JobTable::pending(JobKind kind) const {
    std::lock_guard lg(mu_);
    return slot(kind).state.pending;
}

bool JobTable::cancel_by_task(std::uint64_t task_id) {
    if (task_id == 0) return false;
    for (const JobKind k : {JobKind::Run, JobKind::Write}) {
        bool hit = false;
        {
            // **查完就放锁。** 下面 cancel() 自己要拿同一把锁，
            // 攥着进去就是自锁。
            std::lock_guard lg(mu_);
            hit = slot(k).task_id == task_id;
        }
        if (hit) return cancel(k);
    }
    return false;
}

json JobTable::running_jobs() const {
    std::lock_guard lg(mu_);
    json out = json::array();
    for (const JobKind k : {JobKind::Run, JobKind::Write}) {
        const JobState& s = slot(k).state;
        if (!s.running) continue;
        const double elapsed =
            s.started_at.time_since_epoch().count() == 0
                ? 0.0
                : std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - s.started_at).count();
        // **只带顶栏画得下的那几样。** 这条消息每两秒推给每个连着的浏览器，
        // 塞事件流进来的话，一个跑着的任务就能把这条通道变成主要流量。
        out.push_back({
            // 账本上那一行的 id。**思考正文按它取**（`/api/task/thinking`），
            // 和短活那份（`running_activities`）同一个字段名。0 = 还没登记上。
            {"id", slot(k).task_id},
            {"kind", to_string(k)},
            {"project", s.project},
            {"episode_id", s.episode_id.value_or("")},
            {"stage", s.stage},
            // 长跑任务不画某一格参考图。字段还是要有，理由同下面那个
            // `queued`：前端一套代码画两边，少一个键就得到处判空。
            {"target", ""},
            // Run 用 current/total（第几镜），Write 用 done/total（第几章）。
            // 两套字段在这儿抹平成一套，界面不用分情况画进度。
            {"current", k == JobKind::Run ? s.current : s.done},
            {"total", s.total},
            {"message", s.message},
            // 长跑任务没有"排队"这一说：一种一个槽，起得来就是在跑。
            // 字段还是要有，前端一套代码画两边。
            {"queued", false},
            // **这是一整件长跑，它底下那些镜头是另外几行。** 数「几件在跑」
            // 时不能把它和它自己的孩子一起数——2026-09-20 用户报的「明明只
            // 有一个，显示 3 个」，一半是这个。同 task_board 那份的字段名。
            {"long_job", true},
            {"working", true},
            // 已经跑了多久。和短活那份同一个字段名，界面一套代码画两边。
            {"seconds", round1(elapsed)},
        });
    }
    return out;
}

json JobTable::snapshot(JobKind kind) const {
    std::lock_guard lg(mu_);
    const JobState& s = slot(kind).state;

    json events = json::array();
    const std::size_t skip =
        s.events.size() > kSnapshotEvents ? s.events.size() - kSnapshotEvents : 0;
    for (std::size_t i = skip; i < s.events.size(); ++i) {
        events.push_back(s.events[i].to_json());
    }

    const double elapsed =
        s.started_at.time_since_epoch().count() == 0
            ? 0.0
            : round1(std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - s.started_at).count());

    if (kind == JobKind::Write) {
        // WriteState.snapshot() 的字段少得多，别把 Run 的那些混进来
        return {
            {"running", s.running},
            {"done", s.done},
            {"total", s.total},
            {"message", s.message},
            // 这一件是六件活里的哪一件。见 JobState::title。
            {"title", s.title},
            {"episodes", s.episodes},
            {"error", opt_str(s.error)},
        };
    }

    return {
        {"running", s.running},
        {"episode_id", opt_str(s.episode_id)},
        {"stage", s.stage},
        {"current", s.current},
        {"total", s.total},
        {"message", s.message},
        {"elapsed_s", elapsed},
        {"output", opt_str(s.output)},
        {"outputs", s.outputs},
        {"queue_done", s.queue_done},
        {"queue_total", s.queue_total},
        {"error", opt_str(s.error)},
        {"events", events},
    };
}

bool JobTable::running(JobKind kind) const {
    std::lock_guard lg(mu_);
    return slot(kind).state.running;
}

std::string JobTable::running_project(JobKind kind) const {
    std::lock_guard lg(mu_);
    const Slot& s = slot(kind);
    return s.state.running ? s.state.project : std::string();
}

std::string JobTable::job_id(JobKind kind) const {
    std::lock_guard lg(mu_);
    return slot(kind).state.job_id;
}

void JobTable::wait_idle() {
    std::unique_lock lk(mu_);
    idle_cv_.wait(lk, [this] { return !run_.active && !write_.active; });
}


// ---- JobProgress ----

/// 把一个槽的当前状态打成一条推给界面的进度。
///
/// **`step` 要按任务种类取不同的字段。** Run 记的是第几镜（`current`，由
/// record() 维护），Write 记的是第几章（`done`，由 set_done() 维护）——
/// 两个**不同的字段**。snapshot 和 overview 里早就各写了一遍这个映射
/// （`k == JobKind::Run ? s.current : s.done`），推送这条以前没有，
/// 于是推出去的 step 对写章节来说恒等于 0。
///
/// 三处必须是同一套映射，否则轮询拿到的和推上来的会互相打架：界面收下
/// 推上来的 0，把刚轮询到的正确值覆盖掉，进度条就永远停在 0/4。
static nlohmann::json progress_of(JobKind kind, const std::string& job_id,
                                  const JobState& s) {
    return {{"type", "progress"},
            {"kind", "progress"},
            {"job_id", job_id},
            {"stage", s.stage},
            {"step", kind == JobKind::Run ? s.current : s.done},
            {"total", s.total},
            {"message", s.message},
            // **这条是状态回声，不是新发生的事。**
            //
            // mutate 推的是当前快照，而 `message` 是上一条真事件留下的——
            // 界面把每条推上来的消息都往事件表里追加一行，于是每次
            // set_queue / set_pending / set_episode_id 都会把上一句重印一遍。
            // 2026-09-13 实机看到：装配跑完，日志里「成片已生成」连着两行
            // （第二行是 run.cpp 里 `p.set_queue(++done, total)` 的回声）。
            //
            // 带总数的进度还是要推（它是这条路存在的理由，见 mutate 的注释），
            // 所以不是不发，而是标出来：界面照收状态，但不再当成新的一行。
            {"echo", true}};
}

template <typename F>
void JobTable::mutate(JobKind kind, F&& fn) {
    // **改完要推出去。** 原来这儿只改状态不广播，而 JobProgress 的
    // set_done / set_total / set_message 全走它——也就是说写章节这条路
    // 从头到尾一条进度都没推过，界面只能靠 1.5 秒一次的轮询。轮询一旦
    // 断了（或者被推上来的 0 覆盖），显示就冻在那儿：用户看到的是
    // 「AI 展开中 0/4 · 正在写 ch01」，而同一刻接口回的是 done=1、
    // 正在写 ch02。
    std::string job_id;
    nlohmann::json msg;
    // 账本那一行也要跟着走，见下面那段。
    std::uint64_t task_id = 0;
    int cur = 0, tot = 0;
    std::string note;
    {
        std::lock_guard lg(mu_);
        JobState& st = slot(kind).state;
        fn(st);
        // 没在跑就不用推：起之前和收尾之后的那几次 mutate 跟界面无关，
        // 而收尾自己会发 done/error。
        if (!st.running) return;
        job_id = st.job_id;
        msg = progress_of(kind, job_id, st);
        task_id = slot(kind).task_id;
        // Run 用 current（第几镜），Write 用 done（第几章）——和
        // `running_jobs` 那儿抹平成一套是同一个道理。
        cur = kind == JobKind::Run ? st.current : st.done;
        tot = st.total;
        note = st.message;
    }
    // **进度同步到账本，出了锁再做。**
    //
    // 任务页面要画的是"这一条跑到哪儿了"，而那个数只在这张表里。两处各记
    // 一份必然只改一边，所以由这儿每次改完顺手推过去——它本来就是所有
    // set_done / set_total / set_message 的必经之路。
    if (task_id != 0) {
        set_task_progress(task_id, cur, tot);
        set_task_note(task_id, note);
    }
    // **出了锁再推。** emit 自己要取这把锁拷 sink，在锁里调就是自锁。
    emit(job_id, msg);
}

void JobProgress::report(Event ev) { table_->record(kind_, std::move(ev)); }

void JobProgress::set_message(std::string m) {
    table_->mutate(kind_, [&](JobState& s) { s.message = std::move(m); });
}

void JobProgress::set_done(int done) {
    table_->mutate(kind_, [&](JobState& s) { s.done = done; });
}

void JobProgress::set_total(int total) {
    table_->mutate(kind_, [&](JobState& s) { s.total = total; });
}

void JobProgress::add_episode(nlohmann::json ep) {
    table_->mutate(kind_,
                   [&](JobState& s) { s.episodes.push_back(std::move(ep)); });
}

void JobProgress::set_pending(std::vector<std::string> shot_ids,
                              std::string stage) {
    table_->mutate(kind_, [&](JobState& s) {
        s.pending = std::move(shot_ids);
        s.pending_stage = std::move(stage);
    });
}

void JobProgress::set_output(std::string path) {
    table_->mutate(kind_, [&](JobState& s) { s.output = std::move(path); });
}

void JobProgress::add_output(std::string path) {
    table_->mutate(kind_,
                   [&](JobState& s) { s.outputs.push_back(std::move(path)); });
}

void JobProgress::set_episode_id(std::string id) {
    table_->mutate(kind_, [&](JobState& s) { s.episode_id = std::move(id); });
}

void JobProgress::set_queue(int done, int total) {
    table_->mutate(kind_, [&](JobState& s) {
        s.queue_done = done;
        s.queue_total = total;
    });
}

void JobProgress::set_error(std::string e) {
    table_->mutate(kind_, [&](JobState& s) { s.error = std::move(e); });
}

bool JobProgress::cancelled() const {
    std::lock_guard lg(table_->mu_);
    return table_->slot(kind_).token.cancelled();
}

CancelToken& JobProgress::token() { return table_->slot(kind_).token; }

JobTable& jobs() {
    static JobTable table;
    return table;
}

}  // namespace changji::pipeline
