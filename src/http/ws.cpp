#include "http/ws.hpp"

#include <iterator>
#include <set>

#include <crow/websocket.h>

namespace changji::ws {

using json = nlohmann::json;

Hub& hub() {
    static Hub instance;
    return instance;
}

void Hub::add(crow::websocket::connection* conn) {
    std::lock_guard<std::mutex> lk(mu_);
    // 关张之后来的连接不收。收了就会往它身上发，而那时候 io 上下文已经没了。
    if (closed_) return;
    conns_.insert(conn);
}

void Hub::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    conns_.clear();
    subs_.clear();
    last_sent_.clear();
}

void Hub::reopen() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = false;
}

void Hub::remove(crow::websocket::connection* conn) {
    std::lock_guard<std::mutex> lk(mu_);
    conns_.erase(conn);
    // 订阅表里也要清干净。漏掉的话下一次 broadcast 会向已析构的连接
    // 发送，这是最典型的一类崩溃，而且只在压测或者用户频繁刷新时才复现。
    for (auto it = subs_.begin(); it != subs_.end();) {
        it->second.erase(conn);
        it = it->second.empty() ? subs_.erase(it) : std::next(it);
    }
}

void Hub::subscribe(crow::websocket::connection* conn, const std::string& job_id) {
    // 空 job_id 不订阅：客户端发一条 {"type":"subscribe"} 不带 job_id 时会
    // 走到这儿，而订阅一个空 id 永远等不到任何消息（没有 job 叫这个），
    // 只是往 subs_ 里种一个永远不会被清掉的键。
    if (job_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    // 只认已经登记过的连接。断开之后再来的订阅要丢掉，
    // 否则 subs_ 里会留着一个已经没了的指针。
    if (conns_.count(conn)) subs_[job_id].insert(conn);
}

void Hub::unsubscribe(crow::websocket::connection* conn, const std::string& job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subs_.find(job_id);
    if (it == subs_.end()) return;
    it->second.erase(conn);
    if (it->second.empty()) subs_.erase(it);
}

void Hub::handle_client_message(crow::websocket::connection* conn,
                                const std::string& raw) {
    // 客户端发来的东西一律当不可信输入：解析失败就静默丢弃，
    // 不回错误也不断开连接。畸形消息不该影响一个正在跑的任务。
    json msg = json::parse(raw, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) return;

    // **`value()` 只兜住"键不存在"，兜不住"键在但类型不对"。**
    // `msg.value("type", std::string{})` 碰上 {"type":123} 会抛
    // type_error.302，而这里是客户端能直接喂进来的地方——上面那句
    // "一律当不可信输入"就不成立了。所以先问类型再取。
    //
    // 这是新加的 Hub 用例抓出来的：那条用例里塞了一串畸形消息，
    // 其中 {"type":123,"job_id":456} 让整个 handler 抛了出去。
    const auto str_field = [&msg](const char* key) -> std::string {
        const auto it = msg.find(key);
        if (it == msg.end() || !it->is_string()) return {};
        return it->get<std::string>();
    };
    const std::string type = str_field("type");
    const std::string job_id = str_field("job_id");
    if (type == "subscribe") {
        subscribe(conn, job_id);
    } else if (type == "unsubscribe") {
        unsubscribe(conn, job_id);
    }
}

size_t Hub::subscriber_count(const std::string& job_id) {
    std::lock_guard lg(mu_);
    auto it = subs_.find(job_id);
    return it == subs_.end() ? 0 : it->second.size();
}

bool Hub::should_throttle(const std::string& job_id, const std::string& type,
                          const std::string& kind) {
    // 完成和失败永远不节流。这两条消息漏发一次，前端就会一直显示
    // 「生成中」直到用户手动刷新——比进度条不流畅严重得多。
    if (type != "progress") return false;

    // **每一镜的"落定"也是完成消息，同上。**
    //
    // 上面那条只挡住了**整个任务**的完成（type = done / error）。而一镜跑完
    // 报的是 `{type: "progress", kind: "shot_done"}`——type 仍是 progress
    // （jobs.cpp 里 `ev.kind == "error" ? "error" : "progress"`，只分两种），
    // 于是它落进下面的节流里，和**别的镜头的 shot_done** 抢同一个 200ms
    // 的槽（key 是 `job_id|kind`，同 kind 共桶）。
    //
    // 而镜头是并发跑的（render.cpp 那句「并发时序号先到的未必先跑完」，
    // 上面 kind 那段也写着「多卡之后六镜同时在跑」）。两镜前后脚跑完，
    // 后一条 shot_done 就被丢了——而界面上删掉那张牌的**唯一**依据就是它
    // （run store 的 trackInflight：`kind === 'progress'` 更新，否则
    // `next.delete(msg.shot_id)`），于是那一镜明明跑完了，牌子还写着
    // 「生成中」，一直挂到换阶段（trackInflight 里那道兜底才清）。
    // 正是上面那句话描述的毛病，只是发生在镜头这一级。
    //
    // 放行不会带来量：这几种 kind 都是**一镜一条**（shot_done / gate /
    // warn / eta）或者一段一条（start / done / info），不像 progress 和
    // preview 是一步一条。真正要挡的从来只有那两条流。
    if (!kind.empty() && kind != "progress" && kind != "preview") return false;

    // **按"流"分桶，不只按 job_id。** 采样进度（kind=progress）和预览图
    // （kind=preview）都用 type=progress 广播，但它们是两条独立的流：
    // 一条几十字节、要跟得上步数，一条几十 KB、够看个大概就行。
    // 共用一个桶的话，sd.cpp 每步先调预览回调、后调进度回调，预览抢先占了
    // 这个 200ms 的槽，紧接着的真进度就被丢掉——表现是预览在动、步数
    // 却冻在开跑那一下（2026-09-10 实测：6 秒里 6 条预览、0 条进度）。
    const std::string key = job_id + "|" + (kind.empty() ? type : kind);
    auto now = std::chrono::steady_clock::now();
    auto it = last_sent_.find(key);
    if (it != last_sent_.end() && now - it->second < kThrottleInterval) {
        return true;
    }
    last_sent_[key] = now;
    return false;
}

// 发送为什么必须在锁内：
//
// 直觉上应该先在锁里拷一份连接列表，出锁再发，避免持锁做 IO。
// 但那样有个 use-after-free 窗口——出锁到发送之间，onclose 可能已经
// 在事件循环线程上跑完并析构了连接，我们手里的裸指针就悬空了。
//
// 而 Crow 的 send_text 并不真的写 socket，它把消息塞进连接的写队列
// 交给 asio 异步发出，是个 O(1) 的入队操作。持锁做这个的代价可以忽略，
// 换来的是和 remove() 严格互斥，指针在发送期间保证有效。
//
// 前提是 send_text 确实不阻塞。哪天换掉 Crow 或者它改了实现，
// 这个假设不成立时必须改成让连接由 shared_ptr 管理。

void Hub::broadcast(const std::string& job_id, const json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    // 关张了就一条都别发。见 ws.hpp 上 `shutdown()` 那段。
    if (closed_) return;
    // `value()` 兜不住"键在但类型不对"，会抛 type_error.302——
    // 和 handle_client_message 里那处是同一个坑。这里的 msg 是我们自己
    // 造的，理论上 type 一定是字符串；但这段跑在流水线的工作线程上，
    // 一个异常穿出去比多写三行难查得多。
    const auto type_it = msg.find("type");
    const std::string type =
        (type_it != msg.end() && type_it->is_string())
            ? type_it->get<std::string>()
            : std::string{};
    const auto kind_it = msg.find("kind");
    const std::string kind =
        (kind_it != msg.end() && kind_it->is_string())
            ? kind_it->get<std::string>()
            : std::string{};
    if (should_throttle(job_id, type, kind)) return;

    // 收件人有两种：订了这个**具体 job_id** 的，和订了这一**类**任务的。
    //
    // **按类订阅是必须有的，不是方便。** job_id 由 `new_job_id` 随机生成
    // （mt19937_64 + random_device），而且**从来不从任何接口暴露出去**——
    // `POST /api/run` 回的是 {started, queue}，`GET /api/run` 那十几个字段里
    // 也没有它，Event 里更没有。于是客户端根本拿不到这个 id，
    // 也就永远订不上任何任务：**这条路以前一条消息都送不出去**。
    //
    // 补 job_id 到 REST 响应里是另一种修法，但那样前端得先打一次 GET 才能
    // 订阅，而 WebSocket 本来就是来替掉那次轮询的；而且比 Python 多一个
    // 字段就是一处破契约。按类订阅两样都不占：连上就能订，接口一个字没改。
    //
    // 类名是 job_id 里第一个 '-' 之前的部分（"run-a3f..." → "run"），
    // 和 `to_string(JobKind)` 对得上。
    std::set<crow::websocket::connection*> targets;
    if (auto it = subs_.find(job_id); it != subs_.end()) {
        targets.insert(it->second.begin(), it->second.end());
    }
    const std::size_t dash = job_id.find('-');
    if (dash != std::string::npos) {
        if (auto it = subs_.find(job_id.substr(0, dash)); it != subs_.end()) {
            targets.insert(it->second.begin(), it->second.end());
        }
    }
    if (targets.empty()) return;

    // 用 set 去重：同一个连接既订了具体 id 又订了类别时只该收一条。
    //
    // **dump 用 replace**，理由同 server.cpp 的 json_response：默认的
    // strict 碰上非法 UTF-8 会抛，而这儿是在**干活那条线程**上、还持着
    // 这把锁——抛出去就是那件活整个断掉，而进度消息本来只是给人看的。
    std::string payload = msg.dump(-1, ' ', false, json::error_handler_t::replace);
    for (auto* c : targets) c->send_text(payload);
}

void Hub::broadcast_all(const json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) return;
    // replace 的理由同上。
    std::string payload = msg.dump(-1, ' ', false, json::error_handler_t::replace);
    for (auto* c : conns_) c->send_text(payload);
}

size_t Hub::connection_count() {
    std::lock_guard<std::mutex> lk(mu_);
    return conns_.size();
}

}  // namespace changji::ws
