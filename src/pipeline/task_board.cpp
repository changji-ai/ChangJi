#include "pipeline/task_board.hpp"

#include "util/task_slot.hpp"
#include "util/say.hpp"

#include "util/text.hpp"   // utf8_len：思考那个数报字，不报字节

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace changji::pipeline {
namespace {

using Clock = std::chrono::steady_clock;

/// 做完的留多少条。
///
/// **一条几百字节，两百条就是几十 KB**，而这份东西会整份推给浏览器。
/// 再多的价值也不大：人回头看的是"刚才那几件花了多久"，不是上周的流水。
constexpr std::size_t kDoneKeep = 200;

/// 一件活的思考最多留多少字节。**两百条 × 这个数**是这本账的上限。
/// 20 万字节大约六七万汉字，比任何一次写作想的都多。
constexpr std::size_t kThinkingKeep = 200000;

struct Row {
    std::uint64_t id = 0;
    std::string kind;
    std::string title;
    std::string note;
    std::string project;
    std::string episode_id;
    /// 界面上哪一格（`story` / `shots` / …）。见 `Task` 的构造。
    std::string slot;
    std::string target;
    std::string thinking;
    /// `thinking` 里第一个字的**绝对位置**（从这件活开工算起）。
    /// 被从头截掉一段时它往前走，取的那头据此知道自己断了一截。
    std::size_t thinking_from = 0;
    std::string error;
    TaskState state = TaskState::Queued;
    /// 长跑任务表里也有一份，见 Task::mark_long_job。
    bool long_job = false;
    /// 真在干了，不只是"领到了"。见 Task::begin 上那段。
    bool working = true;
    int current = 0;
    int total = 0;
    Clock::time_point queued_at{};
    Clock::time_point started_at{};
    Clock::time_point ended_at{};
    CancelToken tok;
};

struct Board {
    std::mutex mu;
    // **有序表**：id 递增，遍历出来就是登记顺序。页面每两秒重画一次，
    // 行的顺序要是每次都跳，看着像有活在闪。
    std::map<std::uint64_t, std::shared_ptr<Row>> live;
    std::deque<std::shared_ptr<Row>> done;
    std::uint64_t next = 1;

    /// 每一种活最近几次真花了多久。排队那几件的"预计什么时候开始"靠它。
    std::map<std::string, std::deque<double>> recent;
};

Board& board() {
    static Board b;
    return b;
}

double secs(Clock::time_point a, Clock::time_point b) {
    if (a.time_since_epoch().count() == 0) return 0.0;
    return std::chrono::duration<double>(b - a).count();
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

/// 同一族活按什么归类算耗时。
///
/// **不能只按 kind。** `image` 这一族里既有「画参考图 · 董平 正面」也有
/// 「出首帧 · ep08_sh001」，两者的耗时不是一个量级（参考图一分钟上下，
/// 首帧带着参考图去编辑要更久）。混在一个桶里算出来的中位数，报给谁都不对。
///
/// 标题里 `· ` 前面那一截正是"这是哪一族"（「画参考图」「出首帧」
/// 「出片成片档」「配音」），拿它当桶名。没有那个分隔就退回 kind。
std::string bucket_of(const Row& r) {
    const auto pos = r.title.find(" · ");
    return pos == std::string::npos ? r.kind : r.kind + "/" + r.title.substr(0, pos);
}

/// 这一族活一件大概多久。没跑过就回 0（页面上不报预计）。
/// **取中位数不是平均**：一件卡住的（等显存、等对面机器）能把平均拖成两倍。
double typical_locked(Board& b, const std::string& kind) {
    auto it = b.recent.find(kind);
    if (it == b.recent.end() || it->second.empty()) return 0.0;
    std::vector<double> v(it->second.begin(), it->second.end());
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

nlohmann::json to_json_locked(Board& b, const Row& r, Clock::time_point now) {
    const double elapsed =
        r.state == TaskState::Running
            ? secs(r.started_at, now)
            : secs(r.started_at, r.ended_at);
    nlohmann::json j = {
        {"id", r.id},
        {"kind", r.kind},
        {"title", r.title},
        {"note", r.note},
        {"project", r.project},
        {"episode_id", r.episode_id},
        {"slot", r.slot},
        {"target", r.target},
        {"state", to_string(r.state)},
        // 真在干，还是只是被领走了在等位置。见 Task::begin。
        {"working", r.working},
        // 这一行是"一整件长跑"（出片、写全片），它底下那些镜头是另外几行。
        // 页面数「几件在跑」时**不能把它和它自己的孩子一起数**。
        {"long_job", r.long_job},
        {"current", r.current},
        {"total", r.total},
        {"seconds", round1(elapsed)},
        {"error", r.error},
        // **思考只报有没有，不报正文**：一次写作的思考几千字，而这份账两秒
        // 推一次。页面点开那一下再单独去取（见 /api/task/thinking）。
        {"thinking", !r.thinking.empty()},
        // **报字，不是报字节。** `std::string::size()` 数的是字节，而
        // 页面上那句写的是"想了 N 字"——中文一个字三个字节，这个数当场
        // 虚报三倍。2026-09-17 实测一条大纲：这儿报 101817，真实是
        // 66277 个字（中英混着想，所以是一点五倍不是三倍）。
        //
        // 只有这一处用它，而且只用在那句提示气泡上（TasksView）。增量
        // 拉思考用的偏移量是另一个数（/api/task/thinking 的 end），
        // **那个必须还是字节**，别顺手一起改。
        {"thinking_chars", static_cast<int>(text::utf8_len(r.thinking))},
        // **按了叉之后它还在名单上待一会儿。**
        //
        // 排着的那几件是"有空位了才被领走"的，取消只是把令牌立起来——真正
        // 划掉要等领它的那一路回头看一眼。中间这段（最长就是一件活的时间）
        // 名单上还有它，不说一声的话看着像那个叉没按上。
        // ⚠️ **先判状态，再问令牌——顺序是这一行的全部要害。**
        //
        // 反过来写（`tok.cancelled() && 还活着`）的话，**退休的行也要问一次
        // 令牌**。而一件活的令牌可以挂在整批那个令牌上（`CancelToken::link`，
        // "停一件"和"停一整批"要同时成立），这一行却要在「做完的」里留两百
        // 条——比整批那个令牌（多半在栈上）活得久。于是拼这份 JSON 时顺着
        // 一个野指针往下问，**整个引擎段错误**。2026-09-20 写用例时撞见
        // （EXC_BAD_ACCESS in to_json_locked）。
        //
        // 退休的行早就有答案了（`state` 在退休那一刻记好），根本不用问。
        // 退休时还会把那条链解开（见下面 retire 那段），两道都留着：这一条
        // 是"不去问"，那一条是"就算问了也没有野指针"。
        {"cancelling", (r.state == TaskState::Queued ||
                        r.state == TaskState::Running) &&
                           r.tok.cancelled()},
    };
    if (r.state == TaskState::Queued) {
        // 排了多久了。**光有"预计还要等多久"不够**：等得久的那几件，人想
        // 知道的是"它是不是被忘了"。
        j["waited"] = round1(secs(r.queued_at, now));
        j["eta"] = round1(typical_locked(b, bucket_of(r)));
    }
    return j;
}

}  // namespace

const char* to_string(TaskState s) {
    switch (s) {
        case TaskState::Queued: return "queued";
        case TaskState::Running: return "running";
        case TaskState::Done: return "done";
        case TaskState::Failed: return "failed";
        case TaskState::Cancelled: return "cancelled";
    }
    return "queued";
}

Task::Task(std::string kind, std::string title, std::string project,
           std::string episode_id, std::string slot) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    id_ = b.next++;
    auto row = std::make_shared<Row>();
    row->id = id_;
    row->kind = std::move(kind);
    row->title = std::move(title);
    row->project = std::move(project);
    row->episode_id = std::move(episode_id);
    // 没自己说就按 kind 算。**只有 `image` 需要自己说**（一词两用）。
    row->slot = slot.empty() ? util::slot_of_task_kind(row->kind) : std::move(slot);
    row->queued_at = Clock::now();
    b.live.emplace(id_, std::move(row));
}

Task::~Task() {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    if (it == b.live.end()) return;
    auto row = it->second;
    b.live.erase(it);
    row->ended_at = Clock::now();
    if (row->state == TaskState::Running) {
        // **令牌立着就是被取消的，不是失败的。** 「人按的停不是失败」这条
        // 规矩在引擎里到处都写着（ref_gen.cpp、util/cancel_words.hpp），
        // 账本这头也得分清——不然页面上一排红的，而什么都没出错。
        row->state = row->tok.cancelled() ? TaskState::Cancelled
                     : row->error.empty() ? TaskState::Done
                                          : TaskState::Failed;
        if (row->state == TaskState::Done) {
            auto& q = b.recent[bucket_of(*row)];
            q.push_back(secs(row->started_at, row->ended_at));
            if (q.size() > 20) q.pop_front();
        }
    } else {
        // 没 begin 过就没了 = 排着的时候被取消了。
        row->state = TaskState::Cancelled;
    }
    // **退休之前先和上一级脱钩。**
    //
    // 一件活的令牌可以挂在整批那个令牌上（`CancelToken::link`，"停一件"和
    // "停一整批"要同时成立）。而**这一行还要在「做完的」里留两百条**，
    // 比整批那个令牌活得久——整批跑完、那个令牌（多半在栈上）一没，
    // `tok.cancelled()` 就顺着一个野指针往下问。
    //
    // 踩到的地方是 `/api/tasks`：`to_json_locked` 每一行都要问一次
    // `cancelling`，于是**整个引擎在拼那份 JSON 的时候段错误**。
    // 2026-09-20 写用例时撞见（EXC_BAD_ACCESS in to_json_locked）。
    //
    // 退休之后这一行只剩"当时是什么下场"，`state` 上一句已经记好了，
    // 挂着也没有用处。
    row->tok.link(nullptr);
    b.done.push_back(std::move(row));
    while (b.done.size() > kDoneKeep) b.done.pop_front();
}

void Task::begin(bool working) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    if (it == b.live.end()) return;
    it->second->state = TaskState::Running;
    it->second->started_at = Clock::now();
    it->second->note.clear();
    it->second->working = working;
    // 领到了但还没轮到算力的，那一行要说清是在等什么——不说的话它和
    // "真在画"长得一模一样，只是进度条永远不动。
    if (!working) it->second->note = SAY("派出去了，还没轮到");
}

#define CHANGJI_TASK_MUTATE(body)                     \
    Board& b = board();                               \
    std::lock_guard lg(b.mu);                         \
    auto it = b.live.find(id_);                       \
    if (it == b.live.end()) return;                   \
    Row& row = *it->second;                           \
    body

void Task::set_title(std::string t) { CHANGJI_TASK_MUTATE(row.title = std::move(t);) }
void Task::set_note(std::string n) { CHANGJI_TASK_MUTATE(row.note = std::move(n);) }
void Task::set_target(std::string t) { CHANGJI_TASK_MUTATE(row.target = std::move(t);) }
void Task::set_thinking(std::string a) { CHANGJI_TASK_MUTATE(row.thinking = std::move(a);) }
void Task::append_thinking(const std::string& piece) {
    CHANGJI_TASK_MUTATE(
        row.thinking += piece;
        if (row.thinking.size() > kThinkingKeep) {
            const std::size_t cut = row.thinking.size() - kThinkingKeep;
            row.thinking.erase(0, cut);
            row.thinking_from += cut;
        })
}
void Task::fail(std::string why) { CHANGJI_TASK_MUTATE(row.error = std::move(why);) }
void Task::mark_long_job() { CHANGJI_TASK_MUTATE(row.long_job = true;) }
void Task::set_progress(int current, int total) {
    // **自己报进度就是"真在干"了**（CLAUDE.md 第九条那条判据）。
    // 等位置那句话跟着清掉。
    CHANGJI_TASK_MUTATE(row.current = current; row.total = total;
                        if (!row.working) { row.working = true; row.note.clear(); })
}

void Task::working_now() {
    CHANGJI_TASK_MUTATE(if (!row.working) { row.working = true; row.note.clear(); })
}

#undef CHANGJI_TASK_MUTATE

bool Task::cancelled() const {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    return it != b.live.end() && it->second->tok.cancelled();
}

CancelToken& Task::token() {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    // 已经结完账的：给一个哑元，调用方照样查得动。和 job_stream.hpp 里
    // `current_cancel()` 没有 JobScope 时那条是同一个做法。
    static CancelToken dummy;
    return it == b.live.end() ? dummy : it->second->tok;
}

Thinking task_thinking(std::uint64_t id, std::size_t from) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    const Row* row = nullptr;
    auto it = b.live.find(id);
    if (it != b.live.end()) {
        row = it->second.get();
    } else {
        for (const auto& r : b.done) {
            if (r->id == id) { row = r.get(); break; }
        }
    }
    if (row == nullptr) return {};

    Thinking out;
    // 手上那份比留着的还靠前 = 中间被截掉了一段，从留着的头上给起。
    out.start = std::max(from, row->thinking_from);
    out.end = row->thinking_from + row->thinking.size();
    if (out.start >= out.end) {
        out.start = out.end;   // 没有新的
        return out;
    }
    out.text = row->thinking.substr(out.start - row->thinking_from);
    return out;
}

namespace {
template <class F>
void mutate_by_id(std::uint64_t id, F&& fn) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id);
    if (it != b.live.end()) fn(*it->second);
}
}  // namespace

void set_task_progress(std::uint64_t id, int current, int total) {
    mutate_by_id(id, [&](Row& r) { r.current = current; r.total = total; });
}

void set_task_note(std::uint64_t id, std::string note) {
    mutate_by_id(id, [&](Row& r) { r.note = std::move(note); });
}

std::optional<TaskFacts> task_facts(std::uint64_t id) {
    // `mutate_by_id` 的只读版。**拷一份出来再出锁**：调用方（记提示词日志那头）
    // 拿到之后还要去开文件，攥着 board 那把锁做 I/O 会把整本账卡住。
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id);
    if (it == b.live.end()) return std::nullopt;
    const Row& r = *it->second;
    return TaskFacts{r.kind, r.title, r.project, r.episode_id, r.slot};
}

bool cancel_task(std::uint64_t id) {
    // **长跑那一族要多走一步。** 这儿点亮的是行自己的令牌，而长跑的 worker
    // 查的是**槽**上那个（`JobProgress::cancelled()`）——只点行的话，叉按下去
    // 行上写着"正在停…"，活照跑。2026-09-17 实测：批量补分镜按了叉，三十多
    // 分钟一直跑到自己结束，而长跑正是最需要能停的那一种。
    //
    // 短活那一族不用：它们的令牌由 `CancelLink` 挂在这一行下面（见
    // activity.hpp），点亮这一个就够。
    bool long_job = false;
    {
        Board& b = board();
        std::lock_guard lg(b.mu);
        auto it = b.live.find(id);
        if (it == b.live.end()) return false;
        it->second->tok.request();
        long_job = it->second->long_job;
    }
    // **出了锁再喊。** 任务表那头自己有一把锁，套着 board 那把进去就是两把
    // 锁的固定顺序，早晚和别处撞上。
    if (long_job) jobs().cancel_by_task(id);
    return true;
}

nlohmann::json running_activities() {
    // 顶栏那一行要的形状和长跑任务那边一样（见 JobTable::running_jobs）。
    // **只报短活**：长跑任务由 jobs() 那头报，两边在 running_work() 里接起来。
    Board& b = board();
    std::lock_guard lg(b.mu);
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [id, row] : b.live) {
        if (row->state != TaskState::Running) continue;
        // **长跑那几条跳过**：`running_work` 把这份和任务表那份接成一个
        // 列表，不跳的话同一件活在顶栏上数两遍。见 Task::mark_long_job。
        if (row->long_job) continue;
        out.push_back({
            {"kind", row->kind},
            {"project", row->project},
            {"episode_id", row->episode_id},
            {"slot", row->slot},
            // stage 这一格短活没有，但形状要和长跑任务那边一样——
            // 前端一套代码画两边，少一个键就得在模板里到处判空。
            {"stage", ""},
            // 画的是哪一格参考图。**没有 WebSocket 的时候设定页就靠它**
            // 认出那一格在画（见前端 useRefStream）。别的活是空串。
            {"target", row->target},
            {"current", row->current},
            {"total", row->total},
            // 排队那句盖在上面。**顶栏只有一行**，两句都塞进去会挤掉
            // 后面的项目名，而正在排队的时候"在排队"比"要干什么"更要紧。
            {"message", row->note.empty()
                            ? row->title
                            : SAYF("%1：%2", row->note, row->title)},
            // **在跑还是在排，给个字段，别让前端去猜那句话。**
            // 领到了还没轮到算力的也算排着——顶栏那个数要和"真在动的"对上。
            {"queued", !row->note.empty() || !row->working},
            // 想了多少**字**（不是字节，理由见 to_json_locked 里那段）。
            //
            // 一件纯思考的活没有 current/total，那一行于是既没有进度也没有
            // 数字——除了一条来回走的条子，看不出它是在想还是卡住了。这个数
            // 一直在涨，是那种时候**唯一**动着的东西。0 就是没在想。
            //
            // 正文不在这儿（两秒一推，一件就能把通道占满），
            // 要看走 `/api/task/thinking`。
            {"thinking_chars", static_cast<int>(text::utf8_len(row->thinking))},
        });
    }
    return out;
}

nlohmann::json task_board(const std::string& project) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    const auto now = Clock::now();
    nlohmann::json running = nlohmann::json::array();
    nlohmann::json queued = nlohmann::json::array();
    nlohmann::json done = nlohmann::json::array();
    const auto mine = [&project](const Row& r) {
        return project.empty() || r.project.empty() || r.project == project;
    };
    for (const auto& [id, row] : b.live) {
        if (!mine(*row)) continue;
        (row->state == TaskState::Running ? running : queued)
            .push_back(to_json_locked(b, *row, now));
    }
    // 做完的**倒着报**：刚干完那件在最上面。
    for (auto it = b.done.rbegin(); it != b.done.rend(); ++it) {
        if (!mine(**it)) continue;
        done.push_back(to_json_locked(b, **it, now));
    }
    return {{"running", running}, {"queued", queued}, {"done", done}};
}

}  // namespace changji::pipeline
