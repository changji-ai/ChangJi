#include "pipeline/jobs.hpp"

#include "pipeline/activity.hpp"
#include "pipeline/task_board.hpp"
#include "util/paths.hpp"
#include "util/writer.hpp"

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

std::string chat_lane(const std::string& chat) { return "chat:" + chat; }

namespace {

/// Write 槽的键。片子走 `paths::dir_key`（第十一条：同一部片子两种写法
/// 要落到同一个槽上，不然该 409 的没 409，两件同时往一个文件里写）。
std::string lane_key(const std::string& project, const std::string& lane) {
    return paths::dir_key(project) + '\x1f' + lane;
}

/// 槽上那件是不是这部片子、这一道的。Run 只有一个槽，问"这一道"时靠它。
bool slot_is(const JobSlot& s, const std::string& project, const std::string& lane) {
    return s.state.lane == lane && paths::same_dir(s.state.project, project);
}

}  // namespace

JobTable::JobTable() { run_.kind = JobKind::Run; }

JobTable::~JobTable() {
    // 先请求取消再等。不取消的话析构会卡在一个可能跑几十分钟的任务上。
    {
        std::lock_guard lg(mu_);
        for (Slot* s : all_slots()) s->token.request();
    }
    wait_idle();
    // 锁外 join：工作线程收尾要拿这把锁。槽只增不减，拿一份指针表够了。
    std::vector<Slot*> slots;
    {
        std::lock_guard lg(mu_);
        slots = all_slots();
    }
    for (Slot* s : slots) {
        if (s->worker.joinable()) s->worker.join();
    }
}

JobTable::Slot* JobTable::find(JobKind k, const std::string& project,
                               const std::string& lane, bool make) {
    if (k == JobKind::Run) return &run_;
    const std::string key = lane_key(project, lane);
    auto it = write_.find(key);
    if (it != write_.end()) return it->second.get();
    if (!make) return nullptr;
    auto s = std::make_unique<Slot>();
    s->kind = JobKind::Write;
    Slot* raw = s.get();
    write_.emplace(key, std::move(s));
    return raw;
}

const JobTable::Slot* JobTable::find(JobKind k, const std::string& project,
                                     const std::string& lane) const {
    if (k == JobKind::Run) return &run_;
    auto it = write_.find(lane_key(project, lane));
    return it == write_.end() ? nullptr : it->second.get();
}

const JobTable::Slot* JobTable::latest(JobKind k) const {
    if (k == JobKind::Run) return &run_;
    const Slot* best = nullptr;
    for (const auto& [key, s] : write_) {
        // 在跑的优先：老客户端不带片子来问"写得怎么样了"，给它一件跑完的
        // 而另一件正在跑，它会以为全停了。
        if (best == nullptr) { best = s.get(); continue; }
        if (s->state.running != best->state.running) {
            if (s->state.running) best = s.get();
            continue;
        }
        if (s->seq > best->seq) best = s.get();
    }
    return best;
}

std::vector<JobTable::Slot*> JobTable::all_slots() {
    std::vector<Slot*> out{&run_};
    for (auto& [key, s] : write_) out.push_back(s.get());
    return out;
}

std::vector<const JobTable::Slot*> JobTable::all_slots() const {
    std::vector<const Slot*> out{&run_};
    for (const auto& [key, s] : write_) out.push_back(s.get());
    return out;
}

bool JobTable::start(JobKind kind, const std::string& episode_id, Body body,
                     const std::string& stop_message,
                     const std::string& project, const std::string& title,
                     const std::string& lane) {
    std::unique_lock lk(mu_);
    Slot& s = *find(kind, project, lane, /*make=*/true);
    if (s.state.running) return false;

    // 先占坑再干别的。下面 join 上一轮线程时要临时解锁，
    // 不先把 running 立起来的话，那个窗口里第二个 start() 会看到
    // running==false 一起挤进来，两条线程抢同一个槽。
    s.state.running = true;

    // 上一轮的线程可能还没退——正常跑完没来得及 join，或者被手动停止后
    // 还在收尾（那种情况 running 早就是 false 了）。这里一定要等到它真的结束：
    // 不等的话 std::thread 的赋值运算符会调 terminate，
    // 而且新旧两条线程会同时写同一个 state。
    //
    // **只等这一道自己的上一件。** 分道之前这儿等的是全机器那个槽——A 那边
    // 刚按了停、还在收尾（命令行后端一收尾就是一次完整的生成），B 派活的那个
    // 请求线程就陪着干等。
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
    s.state.lane = lane;
    s.state.started_at = std::chrono::steady_clock::now();
    s.state.stop_message = stop_message;
    s.active = true;
    s.seq = ++next_seq_;

    const std::string job_id = s.state.job_id;
    Slot* sp = &s;

    const std::string task_title =
        !title.empty() ? title
        : kind == JobKind::Run ? SAY("出片")
                               : SAY("批量写作");
    s.state.title = task_title;
    s.worker = std::thread([this, kind, sp, job_id, project, lane, task_title,
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
        // **这条线程上写盘的是谁**（`util/writer.hpp`）：写作记在派活的那一道
        // 名下，存盘时认得出"盖了别人"；出片只是回填状态、路径，不算作者。
        //
        // ⚠️ **要立在 `Activity` 前面**：账本上那一件在构造那一刻从线程上读
        // "哪一道派的"（`Task` 的 `lane`）。立在后面的话，这件长跑自己记成
        // "没人派的"，对话那头报「跑完了」时认不出这是自己的。
        const util::WriterScope writer{util::Writer{
            lane, /*derived=*/kind == JobKind::Run, task_title, {}}};
        pipeline::Activity act{to_string(kind), project, std::string{},
                               task_title};
        act.task().token().link(&sp->token);
        // 顶栏那块牌子把任务表和短活接成一个列表，这一件两边都在——
        // 打个记号，短活那一份跳过它，别数两遍。
        act.task().mark_long_job();
        {
            std::lock_guard lg(mu_);
            sp->task_id = act.task().id();
        }
        JobProgress progress(this, kind, sp, job_id, project, lane);
        try {
            body(progress);
        } catch (const std::exception& e) {
            std::lock_guard lg(mu_);
            sp->state.error = e.what();
        } catch (...) {
            std::lock_guard lg(mu_);
            sp->state.error = SAY("未知异常");
        }

        json final_msg;
        {
            std::lock_guard lg(mu_);
            Slot& sl = *sp;
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
            // **哪部片子、谁派的**。同一条 "write" 频道上同时有好几件在推，
            // 界面不认这两样就会把 A 的「写完了」当成自己这件写完了。
            final_msg["project"] = sl.state.project;
            final_msg["lane"] = sl.state.lane;
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

bool JobTable::cancel_locked(Slot& s) {
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
    s.state.error = SAY(s.state.stop_message.empty() ? stopped_message(s.kind)
                                                     : s.state.stop_message);
    return true;
}

bool JobTable::cancel(JobKind kind) {
    std::lock_guard lg(mu_);
    bool any = false;
    for (Slot* s : all_slots()) {
        if (s->kind == kind && cancel_locked(*s)) any = true;
    }
    return any;
}

bool JobTable::cancel(JobKind kind, const std::string& project,
                      const std::string& lane) {
    std::lock_guard lg(mu_);
    if (kind == JobKind::Run) {
        // Run 只有一个槽：**不是这一道的不停**。原来谁按停都停它，于是 B 那边
        // 按停，A 的片子出到一半没了。
        if (!slot_is(run_, project, lane)) return false;
        return cancel_locked(run_);
    }
    Slot* s = find(kind, project, lane, /*make=*/false);
    return s != nullptr && cancel_locked(*s);
}

int JobTable::cancel_project(JobKind kind, const std::string& project) {
    // 空的片子不认：空串匹配谁都行的话，一个没带片子的"停"会停掉全部——
    // 那正是这一整套要治的事。
    if (project.empty()) return 0;
    std::lock_guard lg(mu_);
    int n = 0;
    for (Slot* s : all_slots()) {
        if (s->kind != kind || !s->state.running) continue;
        if (!paths::same_dir(s->state.project, project)) continue;
        if (cancel_locked(*s)) ++n;
    }
    return n;
}

void JobTable::record(Slot& slot, Event ev) {
    std::string job_id;
    json msg;
    const JobKind kind = slot.kind;

    // 预览图**只广播**：不进事件环（几十 KB 一张，环放不下），不动进度
    // （它不是一步，是一步中间的样子），不进 /api/run（和 Python 对拍）。
    // 老客户端不认识 kind = preview，按 progress 处理也只是多刷一次状态。
    if (ev.kind == "preview") {
        std::string project, lane;
        {
            std::lock_guard lg(mu_);
            job_id = slot.state.job_id;
            project = slot.state.project;
            lane = slot.state.lane;
        }
        emit(job_id, json{{"type", "progress"},
                          {"kind", "preview"},
                          {"job_id", job_id},
                          {"project", project},
                          {"lane", lane},
                          {"stage", ev.stage},
                          {"shot_id", ev.shot_id.value_or("")},
                          {"step", ev.current},
                          {"preview", ev.preview}});
        return;
    }
    {
        std::lock_guard lg(mu_);
        JobState& st = slot.state;

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
            // 哪部片子、谁派的。见 JobState::lane。
            {"project", st.project},
            {"lane", st.lane},
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
    const Slot* s = latest(kind);
    return s != nullptr ? s->state.pending : std::vector<std::string>{};
}

bool JobTable::cancel_by_task(std::uint64_t task_id) {
    if (task_id == 0) return false;
    // **一把锁里查完、停完。** 原来查一个槽放一次锁、再去 cancel(kind)
    // ——分道之后 cancel(kind) 是"这一类全停"，照原样搬过来就是按一行的叉
    // 停掉所有片子的写作。
    std::lock_guard lg(mu_);
    for (Slot* s : all_slots()) {
        if (s->task_id == task_id) return cancel_locked(*s);
    }
    return false;
}

json JobTable::running_jobs() const {
    json out = json::array();
    {
    std::lock_guard lg(mu_);
    for (const Slot* sl : all_slots()) {
        const JobState& s = sl->state;
        const JobKind k = sl->kind;
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
            {"id", sl->task_id},
            {"kind", to_string(k)},
            {"project", s.project},
            // 谁派的（见 JobState::lane）。对话按它认"这件是我派的"。
            {"lane", s.lane},
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
            // 长跑任务没有"排队"这一说：一道一个槽，起得来就是在跑。
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
    }
    // **想了多少字、写的东西换过几次**：这两样记在账本那一行上（批量写正文的
    // 思考、边写边排的正文都挂在这一件的 Activity 上），界面拿它们判断要不要去
    // `/api/task/thinking` 取。同短活那份（`running_activities`）的字段名。
    //
    // ⚠️ **出了任务表的锁再问账本。** 两把锁套着拿就是一个固定顺序，早晚和
    // 反过来拿的那一处撞上（`cancel_task` 上那段同一个理由）。
    for (auto& j : out) {
        const auto id = j.value("id", std::uint64_t{0});
        if (id == 0) continue;
        const LiveCounts c = task_live_counts(id);
        j["thinking_chars"] = c.thinking_chars;
        j["output_ver"] = c.output_ver;
    }
    return out;
}

namespace {

json snapshot_of(JobKind kind, const JobState& s) {
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

}  // namespace

json JobTable::snapshot(JobKind kind) const {
    std::lock_guard lg(mu_);
    const Slot* s = latest(kind);
    return snapshot_of(kind, s != nullptr ? s->state : JobState{});
}

json JobTable::snapshot(JobKind kind, const std::string& project,
                        const std::string& lane) const {
    std::lock_guard lg(mu_);
    if (kind == JobKind::Run) {
        // Run 只有一个槽。**不是这一道的就当没在跑**——不然 B 问自己的进度，
        // 回来的是 A 那件跑到第几镜。
        return snapshot_of(kind, slot_is(run_, project, lane) ? run_.state
                                                               : JobState{});
    }
    const Slot* s = find(kind, project, lane);
    return snapshot_of(kind, s != nullptr ? s->state : JobState{});
}

bool JobTable::running(JobKind kind) const {
    std::lock_guard lg(mu_);
    for (const Slot* s : all_slots()) {
        if (s->kind == kind && s->state.running) return true;
    }
    return false;
}

bool JobTable::running(JobKind kind, const std::string& project,
                       const std::string& lane) const {
    std::lock_guard lg(mu_);
    if (kind == JobKind::Run) return run_.state.running && slot_is(run_, project, lane);
    const Slot* s = find(kind, project, lane);
    return s != nullptr && s->state.running;
}

bool JobTable::running_in(JobKind kind, const std::string& project) const {
    std::lock_guard lg(mu_);
    for (const Slot* s : all_slots()) {
        if (s->kind != kind || !s->state.running) continue;
        // 在跑、却没说是哪部片子的：**当它是**（fail-closed，见 running_project）。
        if (s->state.project.empty() || paths::same_dir(s->state.project, project)) {
            return true;
        }
    }
    return false;
}

std::string JobTable::running_project(JobKind kind) const {
    std::lock_guard lg(mu_);
    const Slot* s = latest(kind);
    return s != nullptr && s->state.running ? s->state.project : std::string();
}

std::vector<std::string> JobTable::running_projects(JobKind kind) const {
    std::lock_guard lg(mu_);
    std::vector<std::string> out;
    for (const Slot* s : all_slots()) {
        if (s->kind == kind && s->state.running) out.push_back(s->state.project);
    }
    return out;
}

std::string JobTable::job_id(JobKind kind) const {
    std::lock_guard lg(mu_);
    const Slot* s = latest(kind);
    return s != nullptr ? s->state.job_id : std::string();
}

std::string JobTable::job_id(JobKind kind, const std::string& project,
                             const std::string& lane) const {
    std::lock_guard lg(mu_);
    if (kind == JobKind::Run) {
        return slot_is(run_, project, lane) ? run_.state.job_id : std::string();
    }
    const Slot* s = find(kind, project, lane);
    return s != nullptr ? s->state.job_id : std::string();
}

void JobTable::wait_idle() {
    std::unique_lock lk(mu_);
    idle_cv_.wait(lk, [this] {
        for (const Slot* s : all_slots()) {
            if (s->active) return false;
        }
        return true;
    });
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
            // 哪部片子、谁派的。见 JobState::lane。
            {"project", s.project},
            {"lane", s.lane},
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
void JobTable::mutate(Slot& slot, F&& fn) {
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
        JobState& st = slot.state;
        fn(st);
        // 没在跑就不用推：起之前和收尾之后的那几次 mutate 跟界面无关，
        // 而收尾自己会发 done/error。
        if (!st.running) return;
        job_id = st.job_id;
        msg = progress_of(slot.kind, job_id, st);
        task_id = slot.task_id;
        // Run 用 current（第几镜），Write 用 done（第几章）——和
        // `running_jobs` 那儿抹平成一套是同一个道理。
        cur = slot.kind == JobKind::Run ? st.current : st.done;
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

void JobProgress::report(Event ev) { table_->record(*slot_, std::move(ev)); }

void JobProgress::set_message(std::string m) {
    table_->mutate(*slot_, [&](JobState& s) { s.message = std::move(m); });
}

void JobProgress::set_done(int done) {
    table_->mutate(*slot_, [&](JobState& s) { s.done = done; });
}

void JobProgress::set_total(int total) {
    table_->mutate(*slot_, [&](JobState& s) { s.total = total; });
}

void JobProgress::add_episode(nlohmann::json ep) {
    table_->mutate(*slot_,
                   [&](JobState& s) { s.episodes.push_back(std::move(ep)); });
}

void JobProgress::set_pending(std::vector<std::string> shot_ids,
                              std::string stage) {
    table_->mutate(*slot_, [&](JobState& s) {
        s.pending = std::move(shot_ids);
        s.pending_stage = std::move(stage);
    });
}

void JobProgress::set_output(std::string path) {
    table_->mutate(*slot_, [&](JobState& s) { s.output = std::move(path); });
}

void JobProgress::add_output(std::string path) {
    table_->mutate(*slot_,
                   [&](JobState& s) { s.outputs.push_back(std::move(path)); });
}

void JobProgress::set_episode_id(std::string id) {
    table_->mutate(*slot_, [&](JobState& s) { s.episode_id = std::move(id); });
}

void JobProgress::set_queue(int done, int total) {
    table_->mutate(*slot_, [&](JobState& s) {
        s.queue_done = done;
        s.queue_total = total;
    });
}

void JobProgress::set_error(std::string e) {
    table_->mutate(*slot_, [&](JobState& s) { s.error = std::move(e); });
}

bool JobProgress::cancelled() const {
    std::lock_guard lg(table_->mu_);
    return slot_->token.cancelled();
}

CancelToken& JobProgress::token() { return slot_->token; }

JobTable& jobs() {
    static JobTable table;
    return table;
}

}  // namespace changji::pipeline
