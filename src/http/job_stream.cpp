#include "http/job_stream.hpp"

#include "pipeline/activity.hpp"
#include "pipeline/task_board.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

#include "http/ws.hpp"

namespace changji::http {

namespace {

using Clock = std::chrono::steady_clock;

/// 一条 stream 的信箱。见 job_stream.hpp 里 mail_open 上面那段。
struct Mailbox {
    std::deque<nlohmann::json> events;
    std::size_t base = 0;       ///< events[0] 的序号
    std::size_t dropped = 0;    ///< 从前面丢掉了几条
    std::size_t bytes = 0;      ///< 估的，只用来决定该不该丢
    bool done = false;          ///< 收到过 job_done / job_error
    Clock::time_point touched = Clock::now();
};

std::mutex g_mail_mu;
std::map<std::string, Mailbox> g_mail;

/// 太久没人来取的那些，扫掉。**调用方要持锁。**
///
/// 挂在 open/post 上，不另起线程：这两下本来就频繁，而"没人来取"这件事
/// 不急着在那一秒发现。
void sweep_locked() {
    const auto now = Clock::now();
    for (auto it = g_mail.begin(); it != g_mail.end();) {
        const auto idle =
            std::chrono::duration_cast<std::chrono::seconds>(now - it->second.touched);
        if (idle.count() > kMailIdleSeconds) {
            it = g_mail.erase(it);
        } else {
            ++it;
        }
    }
}

/// 存一份进信箱——**只给开过信箱的那条 stream 存**。
void mail_post(const std::string& stream_id, const nlohmann::json& msg) {
    std::lock_guard<std::mutex> g(g_mail_mu);
    const auto it = g_mail.find(stream_id);
    if (it == g_mail.end()) return;
    Mailbox& box = it->second;
    const std::string& type = msg.at("type").get_ref<const std::string&>();
    // 预览图不存，理由见头文件。
    if (type == "job_preview") return;
    box.bytes += msg.dump().size();
    box.events.push_back(msg);
    if (type == "job_done" || type == "job_error") box.done = true;
    // 攒太多就从前面丢。**丢头不丢尾**：尾巴上那条才是结果。
    while ((box.events.size() > kMailMaxEvents || box.bytes > kMailMaxBytes) &&
           box.events.size() > 1) {
        box.bytes -= box.events.front().dump().size();
        box.events.pop_front();
        ++box.base;
        ++box.dropped;
    }
}

/// 播出去，再存一份。
void job_send(const std::string& stream_id, nlohmann::json msg) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, msg);
    mail_post(stream_id, msg);
}

}  // namespace

void job_relay(const std::string& stream_id, nlohmann::json msg) {
    job_send(stream_id, std::move(msg));
}

void mail_open(const std::string& stream_id) {
    if (stream_id.empty()) return;
    std::lock_guard<std::mutex> g(g_mail_mu);
    sweep_locked();
    g_mail[stream_id].touched = Clock::now();
}

nlohmann::json mail_take(const std::string& stream_id, std::size_t since) {
    nlohmann::json out{{"events", nlohmann::json::array()},
                       {"next", since},
                       {"done", false},
                       {"dropped", 0},
                       {"exists", false}};
    if (stream_id.empty()) return out;
    std::lock_guard<std::mutex> g(g_mail_mu);
    const auto it = g_mail.find(stream_id);
    if (it == g_mail.end()) return out;
    Mailbox& box = it->second;
    box.touched = Clock::now();
    out["exists"] = true;
    out["dropped"] = box.dropped;
    // 从前面丢过的话，`since` 可能落在已经没了的那一段里——从现有的头上接着给。
    const std::size_t from = since < box.base ? box.base : since;
    for (std::size_t i = from - box.base; i < box.events.size(); ++i) {
        out["events"].push_back(box.events[i]);
    }
    out["next"] = box.base + box.events.size();
    out["done"] = box.done;
    // 结果只送一次：送完就销号，免得一屋子信箱等着过期。
    if (box.done) g_mail.erase(it);
    return out;
}

void job_done(const std::string& stream_id, nlohmann::json result) {
    job_send(stream_id, {{"type", "job_done"},
                         {"job_id", stream_id},
                         {"result", std::move(result)}});
}

void job_error(const std::string& stream_id, const std::string& message) {
    job_send(stream_id, {{"type", "job_error"},
                         {"job_id", stream_id},
                         {"message", message}});
}

void job_progress(const std::string& stream_id, int current, int total,
                  const std::string& message) {
    job_send(stream_id, {{"type", "job_progress"},
                         {"job_id", stream_id},
                         {"current", current},
                         {"total", total},
                         {"message", message}});
}

void job_thinking(const std::string& stream_id, const std::string& piece) {
    if (piece.empty()) return;
    job_send(stream_id, {{"type", "job_thinking"},
                         {"job_id", stream_id},
                         {"text", piece}});
}

namespace {
/// 当前线程在给哪条 stream 干活。见 JobScope。
thread_local std::string g_stream;
/// 当前线程这件活的取消令牌。指向那个 JobScope 里的。
thread_local pipeline::CancelToken* g_cancel = nullptr;

/// 在跑的那几件活：stream_id → 它的令牌。
///
/// **不存 JobScope 本身，只存令牌的地址。** 令牌活在那条后台线程的栈上
/// （JobScope 的成员），生命周期严格包住这件活；表里那一条也是 JobScope
/// 的构造/析构成对加减的，所以不会出现"活干完了还能按停"的悬空指针。
std::mutex g_mu;
std::map<std::string, pipeline::CancelToken*> g_live;

/// 同步那条路上没人能按停，给个不会被触发的。
pipeline::CancelToken& dummy_token() {
    static pipeline::CancelToken t;
    return t;
}
}  // namespace

std::string current_stream() { return g_stream; }

pipeline::CancelToken& current_cancel() {
    return g_cancel != nullptr ? *g_cancel : dummy_token();
}

bool cancel_job(const std::string& stream_id) {
    if (stream_id.empty()) return false;
    std::lock_guard<std::mutex> g(g_mu);
    const auto it = g_live.find(stream_id);
    if (it == g_live.end()) return false;
    it->second->request();
    return true;
}

JobScope::JobScope(std::string stream_id)
    : id_(std::move(stream_id)),
      prev_(g_stream),
      prev_token_(nullptr),
      prev_cancel_(g_cancel),
      use_(&token_) {
    enter();
}

JobScope::JobScope(std::string stream_id, pipeline::CancelToken& token)
    : id_(std::move(stream_id)),
      prev_(g_stream),
      prev_token_(nullptr),
      prev_cancel_(g_cancel),
      use_(&token) {
    enter();
}

void JobScope::enter() {
    g_stream = id_;
    g_cancel = use_;
    if (!id_.empty()) {
        std::lock_guard<std::mutex> g(g_mu);
        // **记下同一个 id 上一层登记的那个，出去时还原。** 同一个 stream id
        // 被重用时（用户连点两下、前一件还没退干净）直接 erase 的话，
        // 外层那件活就再也停不了了——而它还在跑。
        const auto it = g_live.find(id_);
        if (it != g_live.end()) prev_token_ = it->second;
        g_live[id_] = use_;
    }
}

JobScope::~JobScope() {
    if (!id_.empty()) {
        std::lock_guard<std::mutex> g(g_mu);
        // 按地址比一次再动：不是我登记的那条就别碰（同名的下一层还在跑）。
        const auto it = g_live.find(id_);
        if (it != g_live.end() && it->second == use_) {
            if (prev_token_ != nullptr) {
                it->second = prev_token_;
            } else {
                g_live.erase(it);
            }
        }
    }
    // **还原上一层，不是置空。** 嵌套时置空的话，外层那件活的 current_cancel()
    // 会变成哑元——它还在跑，而按停就没反应了。和 g_stream 一个道理。
    g_cancel = prev_cancel_;
    g_stream = std::move(prev_);
}

std::function<void(const std::string&)> thinking_sink(std::string stream_id) {
    if (stream_id.empty()) stream_id = current_stream();
    // ⚠️ **没有 stream 也要回一个函数。**
    //
    // 这儿原来是 `if (stream_id.empty()) return {}`——没人点过的那条路
    // （批量写作、后台补分镜）于是一个字的思考都不留。而任务页面上那个
    // 「思考」正是给这种活看的：它一跑几分钟，屏幕上只有一句"正在写"。
    //
    // 账本那一份按**当前线程在干的那件活**挂（`current_activity`），
    // 和 stream 是两条独立的路：有 stream 就两边都喂。
    return [stream_id](const std::string& piece) {
        if (piece.empty()) return;
        if (!stream_id.empty()) job_thinking(stream_id, piece);
        if (auto* a = pipeline::current_activity()) {
            a->task().append_thinking(piece);
        }
    };
}

void job_preview(const std::string& stream_id, int step,
                 std::string data_url) {
    if (stream_id.empty() || data_url.empty()) return;
    // **不走 job_send**：信箱不存它（见头文件），这儿也就没必要把那串
    // 几十 KB 的 base64 多传一层。
    ws::hub().broadcast(stream_id, {{"type", "job_preview"},
                                    {"job_id", stream_id},
                                    {"current", step},
                                    {"image", std::move(data_url)}});
}

namespace {

void ref_send(const std::string& target, nlohmann::json msg) {
    if (target.empty()) return;
    msg["job_id"] = kRefChannel;   // Hub 按订阅的那串字分发
    msg["target"] = target;
    ws::hub().broadcast(kRefChannel, msg);
}

}  // namespace

void ref_progress(const std::string& target, int current, int total) {
    ref_send(target, {{"type", "ref_progress"},
                      {"current", current},
                      {"total", total}});
}

void ref_preview(const std::string& target, int step, std::string data_url) {
    if (data_url.empty()) return;
    ref_send(target,
             {{"type", "ref_preview"}, {"current", step}, {"image", std::move(data_url)}});
}

void ref_done(const std::string& target) {
    ref_send(target, {{"type", "ref_done"}});
}

void ref_error(const std::string& target, const std::string& message) {
    ref_send(target, {{"type", "ref_error"}, {"message", message}});
}

void ref_queue(nlohmann::json state) {
    state["type"] = "ref_queue";
    state["job_id"] = kRefChannel;
    ws::hub().broadcast(kRefChannel, state);
}

}  // namespace changji::http
