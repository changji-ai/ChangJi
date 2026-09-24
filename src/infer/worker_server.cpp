#include "infer/worker_server.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <optional>
#include <map>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <crow.h>

#include "config/runtime.hpp"
#include "http/setup_api.hpp"
#include "infer/blob.hpp"
#include "infer/node_status.hpp"
#include "infer/peer_auth.hpp"
#include "lan/sense.hpp"
#include "setup/downloader.hpp"
#include "infer/task_run.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "infer/worker_proto.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// 一个任务的活动状态。
///
/// **不存 std::thread。** 存了就得保证析构前 join 或 detach，而这里有好几条
/// 路走不到那一步：没人来查状态、进程收到 SIGTERM、任务还在跑时被顶掉。
/// 析构一个还 joinable 的 thread 会直接 `terminate called without an active
/// exception`——实机上就是这么崩的，日志里只有那一行，看不出和线程有关系。
///
/// 改成建完就 detach，靠 lambda 捕获的 shared_ptr<Live> 保证对象活到线程结束。
struct Live {
    TaskProgress progress;
    pipeline::CancelToken tok;
    /// 上一次派活方来问它的时间（建的时候也算一次）。
    ///
    /// **槽是靠"派活方回来收"才放的**（`/task/<id>` 见到 done/failed 才
    /// erase）。派活方要是没了——进程重启、任务被人取消、网络断了——那条
    /// 记录就**永远占着一个槽**。2026-09-17 实撞：本机引擎重启了几次，每次
    /// 丢下远端两件没人认领的活，之后一切派活都 409；而 409 当天刚改成
    /// 「等它空」（worker_pool.cpp），于是**永远等下去**，两张卡闲着，
    /// 界面上只显示「0/16」一动不动。
    std::chrono::steady_clock::time_point seen =
        std::chrono::steady_clock::now();
    /// 这件活是什么时候开跑的。**取消的理由只能是"跑了太久，多半卡住了"**，
    /// 不能是"派活的那头没影了"——见 infer::orphan_check。
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
};

struct State {
    std::mutex mu;
    /// 手上正在跑的那几件，按任务 id。
    ///
    /// **满了就 409，不排队**——排队会让协调者那边的并发上限失效：
    /// 它以为派出去的都在跑，实际有几个在这儿排着。
    ///
    /// ⚠️ **槽数不是 1。** 2026-09-17 之前这儿写死一件，那对
    /// `--worker` 是对的（一个进程绑一张卡）。可主程序也挂这套接口之后
    /// （一台机器一个进程、一条连接），**一台双卡机就成了单槽节点**——
    /// 派活那头的首帧池和出片池各拿着同一个 URL，第二个任务当场 409。
    /// 实撞：19 镜栽在「工作进程 … 正忙。**这不该发生**」上。
    /// 槽数跟着卡数走，见 slots 的赋值处。
    std::map<std::string, std::shared_ptr<Live>> running;
    /// **跑完了、还没人来取的**：按内容指纹（`task_key`）存着。
    ///
    /// 用户 2026-09-20：「任务做完了如果对方下线了应该放到一边保存接着做
    /// 下一个事情，对方上线后自己来取」。
    ///
    /// 在这之前跑完的那件**还占着位置**（`running` 里躺着，直到派活方回来
    /// 收或者五分钟后被收掉）——那五分钟里这张卡什么都不干，而它早就算完了。
    /// 现在一落定就挪到这儿：**位置当场放出来**（还会把等着的叫醒），
    /// 结果留着等人来取。
    ///
    /// 留着的只是一小块元数据（状态、产物的 blob 指纹）；产物本身在 blob
    /// 库里按内容存着，本来就不跟着这条记录走。
    std::map<std::string, std::shared_ptr<Live>> finished;
    /// 任务 id → 指纹，好让老路子（`GET /task/<id>`）也找得到已经挪走的那件。
    std::map<std::string, std::string> id_to_key;

    /// 空 = 一次一件（`--worker` 那条）。主程序传"活着的子进程数"。
    Capacity capacity;
    std::size_t slots() const { return capacity ? std::max<std::size_t>(1, capacity()) : 1; }
    std::atomic<std::uint64_t> next_id{1};

    /// **空位变动的序号**，每放出一个位置就 +1。
    ///
    /// 用户 2026-09-20：「任务结束了也要连接的全部机器广播，这样别的机器
    /// 也可以派任务，就看谁抢的快」。
    ///
    /// 在这之前，没抢到的那一头是**盲等三秒再问一遍**（worker_pool.cpp 里
    /// 那个循环）。三秒是瞎猜的：位置可能在第 0.1 秒就空出来（于是白等
    /// 2.9 秒，而这台卡闲着），也可能两分钟才空（于是白问四十次）。
    ///
    /// 有了这个序号，等的那头带着"我知道的那一版"来问，位置一空就**同时
    /// 叫醒所有人**——谁先 POST 上来谁拿到，这正是"谁抢得快"。
    std::uint64_t free_seq = 0;
    std::condition_variable freed;

    /// 放一个位置出来：序号 +1，把等着的全叫醒。**调用时必须已经持有 mu。**
    void release_locked() {
        ++free_seq;
        freed.notify_all();
    }

    /// 收掉没人认领的那几件，回收了几件。**调用时必须已经持有 mu。**
    ///
    /// 规矩在 `infer::orphan_check`（那儿能测）：跑完没人取的删记录，
    /// 还在跑而派活方早没影了的先取消再删。**收一件就放一个位置、叫醒
    /// 等着的**——不叫的话，那台机器下线之后这儿空出来的位置没人知道。
    /// 一件落定了：**位置当场放出来，结果挪到一边等人来取。**
    /// 调用时必须已经持有 mu。
    void settle_locked(const std::string& id) {
        const auto it = running.find(id);
        if (it == running.end()) return;
        const auto key = id_to_key.count(id) ? id_to_key[id] : id;
        finished[key] = it->second;
        running.erase(it);
        release_locked();
    }

    std::size_t reap_locked() {
        const auto now = std::chrono::steady_clock::now();
        std::size_t gone = 0;
        // ---- 放在一边的那些：按自己的保质期收 ----
        //
        // **它们不占位置了**，所以留久一点没有代价：留的是一小块元数据，
        // 而产物在 blob 库里按内容存着。留着的用处是"对方上线后自己来取"
        // ——重启之后它派过来的还是同一件活（同样的提示词、同样的种子），
        // 指纹对得上就直接把结果给它，省掉整整一次渲染。
        for (auto it = finished.begin(); it != finished.end();) {
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second->seen);
            if (orphan_check(/*settled=*/true, idle, std::chrono::seconds(0)) !=
                Orphan::DropSettled) {
                ++it;
                continue;
            }
            std::fprintf(stderr, SAY_NEVER("[节点] 放了一个钟头没人来取，扔掉结果 %s"
                                 "（产物还在 blob 库里）\n"),
                         it->first.c_str());
            it = finished.erase(it);
            ++gone;
        }
        for (auto it = running.begin(); it != running.end();) {
            const bool settled = it->second->progress.state == "done" ||
                                 it->second->progress.state == "failed";
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second->seen);
            const auto ran = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second->started);
            // 落定了的本该在 settle_locked 里就挪走了；万一漏了（老路径、
            // 异常路上），这儿兜一次——**挪走，不是删掉**。
            if (settled) {
                const auto id = it->first;
                ++it;
                settle_locked(id);
                continue;
            }
            if (orphan_check(settled, idle, ran) != Orphan::CancelStuck) {
                ++it;
                continue;
            }
            // **取消的理由只有"跑了太久，多半卡住了"。**
            // 派活方在不在，这儿不问——它不在，这件跑完了会放到一边等它
            // 回来取（见 settle_locked）。
            it->second->tok.request();
            std::fprintf(stderr,
                         SAY_NEVER("[节点] 跑了 %lld 秒还没完，多半卡住了，取消 %s\n"),
                         static_cast<long long>(ran.count()), it->first.c_str());
            it = running.erase(it);
            release_locked();
            ++gone;
        }
        return gone;
    }
};

crow::response json_res(const json& body, int code = 200) {
    // dump 用 replace 不用默认的 strict，理由同 http/server.cpp 的
    // `json_response`：这里回的 body 里带着 sd.cpp 抛上来的那句错误原文，
    // 而那是一个原生库拼出来的字符串——夹一个非法字节进去，strict 就在这
    // 一行抛，整条 /task 变成一个没有 body 的 500，主进程那头只看得到
    // 「这一镜失败了」，真正的原因反而丢了。合法输入逐字节不变。
    crow::response res(code, body.dump(-1, ' ', false,
                                       json::error_handler_t::replace));
    res.set_header("Content-Type", "application/json; charset=utf-8");
    return res;
}

}  // namespace

/// 起一条**自己会跑的**回收线程。
///
/// 用户 2026-09-20：「派活的这个机器派完后下线了，等任务完成都没上线，
/// 怎么处理呢」。
///
/// 在这之前回收只长在 `POST /task` 里——**得有人来派活才会收**。派活的那台
/// 一下线，这条记录就永远占着位置：`/status` 一直报 `free: 0`，`/slots/wait`
/// 一直挂着（没有位置放出来，自然也没人被叫醒），而这台机器从此对谁都回
/// 409。**唯一能解开的动作，恰恰是那台已经做不到的那一个。**
///
/// 所以让它自己转：每 10 秒扫一遍，该收的收掉、位置放出来、等着的叫醒。
/// 10 秒是相对于两档超时（2 分钟 / 5 分钟）足够细的粒度，而这一趟只是
/// 遍历一张几件的表，代价可以忽略。
///
/// **握 weak_ptr**：主程序退出时 state 一没，这条线程下一拍就自己收摊，
/// 不用另外接一套停止信号。
void start_reaper(const std::shared_ptr<State>& state) {
    std::weak_ptr<State> weak = state;
    std::thread([weak] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const auto st = weak.lock();
            if (!st) return;   // 进程在退了
            std::lock_guard lg(st->mu);
            st->reap_locked();
        }
    }).detach();
}

void mount_worker_api_impl(http::EngineApp& app,
                           const config::Settings& settings,
                           const WorkerOptions& opts,
                           const models::HardwareProfile& profile,
                           const std::shared_ptr<State>& state,
                           const TaskRunner& runner) {
    // **这台的自我介绍。** 别的机器靠它决定派不派活过来：能力齐不齐、
    // 卡多大、模型目录还剩多少。拼的地方只有一处（node_status.cpp），
    // 界面上那张表和 --doctor 末尾那句用的是同一份。
    // 对外监听时，除了 /health 都要口令。
    //
    // **/health 故意不要**：它只回 ok/gpu/busy，探活的那一头（可能是
    // 负载均衡、可能是脚本）不该为了 ping 一下就拿到口令。
    const auto gate = [settings, opts](const crow::request& req)
        -> std::optional<crow::response> {
        if (!is_public_bind(opts.host)) return std::nullopt;
        if (token_ok(req.get_header_value("Authorization"),
                     settings.peer.token)) {
            return std::nullopt;
        }
        // **局域网上配好的那几台走另一条门。**
        //
        // 那头在设置里点了「给它用这台」，这头就给它配了一张票
        //（`lan/peers.hpp` 的 `Grant::ticket`）。带着票来就放行。
        //
        // ⚠️ **认票不认 id。** id 写在 mDNS 的 TXT 里，同一个网段谁都读得到
        // ——拿 id 放行等于"谁报得出名字谁就能用"。
        //
        // ⚠️ **感知关着时这条门也不开**（`Sense::by_ticket` 里判的）：
        // 关掉的意思是"这台机器现在不参与"。
        const std::string auth = req.get_header_value("Authorization");
        if (lan::Sense::instance()
                .by_ticket(req.get_header_value("X-Changji-Lan-Ticket"))
                .use) {
            return std::nullopt;
        }
        // **票也认 `Authorization: Bearer <票>` 这种写法。**
        //
        // 派活那条路（`worker_pool`）只会发这一个头，它不知道"局域网"
        // 这回事。认了它，局域网上配好的那台就能**原样走现成的派活路**
        // ——一行调度代码都不用改。
        if (lan::Sense::instance().by_ticket(bearer_of(auth)).use) {
            return std::nullopt;
        }
        // ⚠️ **这一族 `detail` 是回给派活那台机器的**，而它会原样端到它
        // 自己的用户面前。所以它说的是**这台工作机**的语言，不是派活那台
        // 的——两台设成不同语言时，那句话就是这台的语言。比从前一律中文
        // 强，但不算完全对；真要对得上得让派活那头按码翻，那是另一件事。
        return json_res({{"detail",
                          SAY("口令不对或者没带。要 Authorization: Bearer "
                              "<对面 [peer].token 那个值>；"
                              "局域网上配过的那几台带 X-Changji-Lan-Ticket")}},
                        401);
    };

    // **读活的那一份，不是启动时捕获的拷贝。** 见上面 runtime().replace
    // 那段：这台自己下完模型之后，这份自我介绍必须跟着变，否则派活那头
    // 看到的永远是"干不了"。
    CROW_ROUTE(app, "/status")([profile, gate, state](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        auto js = node_status_json(config::runtime().snapshot(), profile);
        // **同时收得下几件，得报出去。** 派活那边按这个数开槽（node_pick.hpp
        // 的 remote_slots_for）；不报的话它按 1 开，一台双卡机就永远只用
        // 一张卡——2026-09-17「只用了一张卡」的最后一截就在这儿。
        // 数字和下面 /task 回 409 的门槛是同一个（state->slots()）：两边
        // 各算一次迟早对不上，对不上的后果是 409 = 那一镜失败。
        js["slots"] = state->slots();
        // **还剩几个位置，以及"空位变到第几版了"。**
        //
        // 光报总数不够：派活那头据此开了 N 条通道，可这台此刻可能正替
        // **别的机器**干着活（用户 2026-09-20：「应该先看这个机器有几个
        // 空闲卡然后派几个任务」）。报出占用之后，派活那头一眼就知道自己
        // 这一发多半会被拒，也知道该拿哪个 `seq` 去 `/slots/wait` 上等。
        {
            std::lock_guard lg(state->mu);
            const std::size_t slots = state->slots();
            const std::size_t used = state->running.size();
            js["running"] = used;
            js["free"] = used >= slots ? 0 : slots - used;
            js["seq"] = state->free_seq;
        }
        return json_res(js);
    });

    // 有位置了就叫我一声。**长轮询，不是让人定时来问。**
    //
    // 用户 2026-09-20：「任务结束了也要连接的全部机器广播，这样别的机器也
    // 可以派任务，就看谁抢的快」。
    //
    // `seq` 是调用方手上那一版（从 409 或 /status 里拿的）。这台的空位版本
    // 一变就立刻回；没变就挂着等，最多等 `wait_ms`（默认 25 秒，上限 60）。
    // **位置一空是同时叫醒所有人的**（`notify_all`）——谁先 POST /task 上来
    // 谁拿到，剩下的再拿新的 seq 回来等。这就是"谁抢得快"。
    //
    // 为什么是长轮询不是推送：派活那头连过来用的是 httplib，没有反向通道；
    // 而长轮询在代理和 NAT 后面最不容易坏。**超时回 200 不回 408**——
    // 等满了不是错，是"这段时间没空出位置"，调用方原样再来一趟即可。
    CROW_ROUTE(app, "/slots/wait")([gate, state](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        std::uint64_t since = 0;
        if (const char* v = req.url_params.get("seq")) {
            try {
                since = std::stoull(v);
            } catch (const std::exception&) {
                since = 0;
            }
        }
        // **上限 30 秒，不是随便定的。** 挂着的每一个都占着 Crow 的一条
        // 工作线程（这台一共 16 条），而那几条线程还要答探活、答进度、答
        // 取产物。挂久了省的是几次轮询，赔的是"人多的时候这台答不上话"。
        //
        // 派活那头自己切成 5 秒一段（worker_pool.cpp 的 wait_for_room），
        // 所以正常用法下一条线程最多被占 5 秒；这个上限管的是别的调用方。
        int wait_ms = 25000;
        if (const char* v = req.url_params.get("wait_ms")) {
            try {
                wait_ms = std::max(0, std::min(30000, std::stoi(v)));
            } catch (const std::exception&) {
            }
        }
        std::unique_lock lk(state->mu);
        const auto has_room = [&] {
            return state->free_seq != since ||
                   state->running.size() < state->slots();
        };
        if (!has_room()) {
            state->freed.wait_for(lk, std::chrono::milliseconds(wait_ms),
                                  has_room);
        }
        const std::size_t slots = state->slots();
        const std::size_t used = state->running.size();
        return json_res({{"slots", slots},
                         {"running", used},
                         {"free", used >= slots ? 0 : slots - used},
                         {"seq", state->free_seq}});
    });

    // **GET 和 POST 都要答。** 多卡那条探活走的是 llm::default_http_post()
    // （run_deps.cpp 里写着"为一次探活单独引一条 HTTP 路径不值得"），发的是
    // POST；而这儿原来只注册了 GET，POST 一律 405。后果是 auto_spawn 拉起的
    // 每一个子进程都"两分钟没应答"然后被杀掉——**多卡自动拉起从来没成功过**，
    // 只是之前没人在多卡机上跑过。2026-09-17 在两张 L20 的机器上实撞：
    // 手动起的子进程 GET /health 秒回，POST /health 405。
    CROW_ROUTE(app, "/health")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST)([opts, state] {
        std::lock_guard lg(state->mu);
        return json_res({{"ok", true},
                         {"gpu", opts.gpu},
                         {"busy", state->running.size() >= state->slots()}});
    });

    // ---- 装模型：让派活那头能指挥这台去补齐 ----
    //
    // **转调初始化页那套**（http/setup_api），不另写一份：清单、推荐档、
    // aria2/curl、断点续传、按真实字节数判完成，全在那儿了。这台机器
    // 自己打开界面点下载，和别的机器指挥它下载，走的必须是同一条路——
    // 两份的话，"下完了没有"的判据迟早只改一边。
    const auto api_res = [](const http::ApiResult& r) {
        return json_res(r.body, r.status);
    };

    CROW_ROUTE(app, "/setup/state")(
        [gate, settings, profile, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return api_res(http::get_setup_state(settings, profile));
    });

    CROW_ROUTE(app, "/setup/download").methods(crow::HTTPMethod::POST)(
        [gate, settings, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            return json_res({{"detail", SAY("请求体不是 JSON")}}, 400);
        }
        try {
            return api_res(http::post_setup_download(settings, body));
        } catch (const http::ApiError& e) {
            return json_res({{"detail", e.detail()}}, e.status());
        }
    });

    CROW_ROUTE(app, "/setup/progress")(
        [gate, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return api_res(http::get_setup_progress());
    });

    CROW_ROUTE(app, "/setup/cancel").methods(crow::HTTPMethod::POST)(
        [gate, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return api_res(http::post_setup_cancel());
    });

    // ---- blob：跨机时输入和产物都走这三条 ----
    //
    // **为什么不把文件塞进任务的 JSON 里。** 一张参考图几 MB，base64 之后
    // 还要涨三分之一，而一章里那几张图是同一批文件——塞进去就是同一张脸
    // 传二十二遍。分开之后，第二镜起 probe 一问就跳过了。
    const auto cache = infer::cache_root_of(settings.workspace_path());

    CROW_ROUTE(app, "/blob/<string>/probe")(
        [gate, cache](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        // 派活那头靠这一句决定传不传。**不合法的指纹回 have:false 就够**
        // ——它本来也不可能存在，而当成错误会让派活方以为链路坏了。
        return json_res({{"have", blob_present(cache, id)}});
    });

    CROW_ROUTE(app, "/blob/<string>").methods(crow::HTTPMethod::POST)(
        [gate, cache](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        // 先核指纹再落地，对不上不写——截断的那份是最阴的故障，
        // 见 blob.hpp。
        if (const auto why = blob_store(cache, id, req.body); !why.empty()) {
            return json_res({{"detail", why}}, 400);
        }
        return json_res({{"ok", true}});
    });

    CROW_ROUTE(app, "/blob/<string>")(
        [gate, cache](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        const auto p = blob_path(cache, id);
        std::error_code ec;
        if (p.empty() || !std::filesystem::is_regular_file(p, ec)) {
            return json_res({{"detail", SAYF("没有这个 blob：%1", id)}}, 404);
        }
        // **按段给，不一口气发整个文件。** 跨境公网实测 20～30 KB/s：
        //   · 一次发整个：Crow 对 1 MB 以下的 body 是异步写完就开连接
        //     计时器（默认 5 秒），字节没发完计时器先到，对面收到半截
        //     （curl 每次都在 33 万字节处断，2026-09-15 实撞）；
        //   · 走静态文件那条路是**同步**分块写，一条连接把整个
        //     io_service 堵 30 秒，落在同一个 io_service 上的 /task、
        //     轮询全等着——下一镜派不出去，显卡白闲 25 秒。
        // 派活方按 off/len 一段一段地取（见 worker_pool.cpp 的
        // pull_artifact），每段几秒钟就完，走异步写、不堵别人；
        // 不带 off/len 的老客户端照样拿整份。
        std::error_code ec2;
        const auto total = static_cast<std::uint64_t>(std::filesystem::file_size(p, ec2));
        std::uint64_t off = 0;
        std::uint64_t len = total;
        if (const char* o = req.url_params.get("off")) off = std::strtoull(o, nullptr, 10);
        if (const char* l = req.url_params.get("len")) len = std::strtoull(l, nullptr, 10);
        if (off > total) return json_res({{"detail", SAY("off 超出文件")}}, 416);
        len = std::min(len, total - off);
        std::ifstream in(p, std::ios::binary);
        if (!in) return json_res({{"detail", SAYF("读不了：%1", id)}}, 500);
        std::string body(static_cast<std::size_t>(len), '\0');
        in.seekg(static_cast<std::streamoff>(off));
        in.read(body.data(), static_cast<std::streamsize>(len));
        body.resize(static_cast<std::size_t>(in.gcount()));
        crow::response res(200, std::move(body));
        res.set_header("Content-Type", "application/octet-stream");
        res.set_header("X-Blob-Size", std::to_string(total));
        return res;
    });

    CROW_ROUTE(app, "/task").methods(crow::HTTPMethod::POST)(
        [state, settings, gate, runner](const crow::request& req) {
            if (auto deny = gate(req)) return std::move(*deny);
            // **先看这串字节是不是合法 UTF-8。** 不是的话下面每一条
            // 路都会炸在同一个地方：nlohmann 解析时照单全收，而把出错
            // 位置附近的原始字节拼进 {"detail": …} 再 dump，就在报错的
            // 路上又抛一次——第二次没人接，派活方拿到一个空白的 500。
            // 2026-09-12 实撞，日志里只有一行 invalid UTF-8 byte。
            if (!text::is_valid_utf8(req.body)) {
                return json_res(
                    {{"detail",
                      SAY("请求体不是合法的 UTF-8。派活那头多半没按 UTF-8 "
                          "编码（Windows 上直接发 GBK 的中文就会这样）")}},
                    400);
            }
            Task task;
            try {
                task = task_from_json(json::parse(req.body));
            } catch (const std::exception& e) {
                // e.what() 里可能带着原始字节，洗一遍再放进 JSON
                return json_res({{"detail", SAYF("任务读不懂：%1",
                                                 text::sanitize_utf8(e.what()))}},
                                400);
            }

            // **先自检再排队。** 干不成就当场说——这一条是烧过一次换来的。
            // 判据在 task_run.cpp，两条路（工作进程、对等互联）共用一份。
            if (const auto why = cannot_do(task, settings); !why.empty()) {
                return json_res({{"detail", why}, {"shot_id", task.shot_id}}, 400);
            }

            const std::string key = task_key(task);

            std::lock_guard lg(state->mu);
            // **先回收：** 放在一边过了保质期的扔掉，还在跑但派活方早没影
            // 了的先取消再删（那张卡不该替一个没人要的活干活）。见 reap_locked。
            state->reap_locked();

            // ---- 这件活我做过了：直接把结果给它 ----
            //
            // 用户 2026-09-20：「对方上线后自己来取」。**这就是"来取"那一
            // 下**：派活方重启之后，照着同样的镜头、同样的提示词和种子再派
            // 一次，指纹对得上——不用重算，把上一次的结果交给它。
            //
            // 不用另开一条"取结果"的接口，也不用派活方记住上次那个任务 id
            // （那个 id 只活在它那次调用的栈上，重启就没了）。它照原样再派
            // 一次，这儿认出来就行。
            //
            // **两台机器派同一件活也走这条**：第二台不会让这张卡再算一遍。
            if (const auto hit = state->finished.find(key);
                hit != state->finished.end()) {
                hit->second->seen = std::chrono::steady_clock::now();
                std::fprintf(stderr, SAY_NEVER("[节点] 这件做过了，直接给结果：%s key=%s\n"),
                             task.shot_id.c_str(), key.substr(0, 12).c_str());
                // 回的形状和新收下一件一模一样（`{"id": …}`），派活方照旧
                // 去轮询 `GET /task/<id>`，第一拍就拿到 done。
                std::string old_id = key;
                for (const auto& [id, k] : state->id_to_key) {
                    if (k == key) { old_id = id; break; }
                }
                return json_res({{"id", old_id}, {"cached", true}}, 202);
            }
            if (state->running.size() >= state->slots()) {
                // **不排队。** 见文件头。
                //
                // **拒也要把话说全**（用户 2026-09-20：「派成功失败都要
                // 反馈」）：几个位置、占了几个、等到哪一版就能再来问。
                // 只回一句"正忙"的话，派活那头除了盲等没有别的选择——
                // 而它现在能拿着 `seq` 去 `/slots/wait` 上等，位置一空
                // 就被叫醒。
                return json_res({{"detail", SAY("正忙")},
                                 {"busy", true},
                                 {"running", state->running.size()},
                                 {"slots", state->slots()},
                                 {"free", 0},
                                 {"seq", state->free_seq}},
                                409);
            }

            const std::string id =
                std::to_string(state->next_id.fetch_add(1));
            auto live = std::make_shared<Live>();
            live->progress.state = "running";
            // 记下这件活的内容指纹：跑完挪到「放在一边」那格时按它存，
            // 派活方重启之后再派同一件就认得出来。
            state->id_to_key[id] = key;
            // **收下的每一件都记一行。** 派活那台重启之后再派同一件活时，
            // 指纹对不上就说明任务内容变了（种子、提示词、输入图……）——
            // 那时候唯一能查的就是这两行的 key 对不对得上。
            std::fprintf(stderr, SAY_NEVER("[节点] 收下 %s key=%s\n"),
                         task.shot_id.c_str(), key.substr(0, 12).c_str());

            // **建完就 detach**，见 Live 的注释。live 是 shared_ptr，
            // 被 lambda 捕获一份，线程跑多久它就活多久。
            // id 也捕一份：跨机时沙箱按它起名（<cache>/tasks/<id>）。
            std::thread([state, live, task, settings, id, runner] {
                const auto on_step = [state, live](int step, int steps,
                                                  double, Phase phase) {
                    std::lock_guard lg(state->mu);
                    live->progress.step = step;
                    live->progress.steps = steps;
                    live->progress.phase = phase_name(phase);
                };
                // 采样中途的预览存进进度里，派活方轮询时按需带走
                // （见 TaskProgress::preview）。只认这件活自己的 tag。
                const PreviewSinkHandle preview_sink(
                    [state, live, tag = task.shot_id](const std::string& t, int step,
                                                      std::string url) {
                        if (t != tag) return;
                        std::lock_guard lg(state->mu);
                        live->progress.preview_step = step;
                        live->progress.preview = std::move(url);
                    });
                // 怎么跑在 task_run.cpp 里，那一层不碰网络。
                // **origin 是 Local**：这些工作进程是本机自己按显卡数
                // 拉起来的（见 worker_farm.hpp），它们干的就是本机的活。
                // 别的机器派来的活走对等互联那条路，那边传 Peer。
                //
                // （合并时这儿原来是一整段就地跑的代码，包括那句
                //  `sd_renderer_with_seed(settings, task.seed)`——采样旋钮
                //  要跟着这一章的 settings 走。搬进 run_task_locally 之后
                //  那个参数还在，见 task_run.cpp 里那一行。）
                // **给了执行器就交给它。** 主程序传的是交给本机那几张卡
                // （见头文件上 TaskRunner 那段）：一个进程只能用一张卡，
                // 就地跑的话双卡机上永远只有一张在动。
                // `--worker` 不传，照旧就地跑——它本来就绑着一张卡。
                //
                // ⚠️ **整段必须包在 try 里。** 这是个 detach 出去的线程，
                // 异常逃出线程函数就是 std::terminate——**整个进程死**，
                // 连同它按显卡数拉起的那几个子进程。
                //
                // 2026-09-17 实撞：本机按了「停下」，派活那头给远端发
                // `POST /task/<id>/cancel`，这儿的活抛出「取消了」，远端
                // 主进程当场 terminate。日志里只有
                //     terminate called after throwing an instance of
                //     'std::runtime_error'  what(): 取消了
                // 而界面上看到的是"那台机器掉线了"——**每按一次停下，
                // 对面就死一次**，而且看不出这两件事有关系。
                //
                // 取消不是崩溃的理由：把这件活记成失败，进程接着服务。
                TaskResult result;
                try {
                    result = runner ? runner(task, on_step, live->tok)
                                    : run_task_locally(task, settings,
                                                       Origin::Local, id,
                                                       on_step, live->tok);
                } catch (const std::exception& e) {
                    result.ok = false;
                    result.error = e.what();
                } catch (...) {
                    // 不是 std::exception 的也不能放它出去——出去就是死。
                    result.ok = false;
                    result.error = SAY("工作进程里抛了个不认识的异常");
                }
                std::lock_guard lg(state->mu);
                live->progress.state = result.ok ? "done" : "failed";
                live->progress.result = result;
                // **算完那一刻就把位置放出来，别等派活方回来收。**
                //
                // 用户 2026-09-20：「任务做完了如果对方下线了应该放到一边
                // 保存接着做下一个事情」。原来这条记录要在 `running` 里躺到
                // 派活方回来取（或者五分钟后被收掉）——**那几分钟里这张卡
                // 什么都不干，而它早就算完了**；派活方要是下线了，那就是
                // 白白空转五分钟。
                //
                // 现在挪到「放在一边」那格：位置当场放出来、等着的机器一起
                // 被叫醒（release_locked），结果留着等人来取。
                state->settle_locked(id);
            }).detach();

            state->running[id] = live;
            return json_res({{"id", id}}, 202);
        });

    CROW_ROUTE(app, "/task/<string>")(
        [state, gate](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        std::lock_guard lg(state->mu);
        std::shared_ptr<Live> live;
        if (const auto it = state->running.find(id); it != state->running.end()) {
            live = it->second;
        } else {
            // **在跑的里没有，就去「放在一边」那格找。**
            //
            // 跑完的那一刻就挪过去了（位置当场放出来，见 settle_locked），
            // 所以正常路上派活方最后那一拍问到的正是这儿这一份。
            const auto k = state->id_to_key.find(id);
            const auto key = k != state->id_to_key.end() ? k->second : id;
            if (const auto f = state->finished.find(key);
                f != state->finished.end()) {
                live = f->second;
            }
        }
        if (!live) {
            return json_res({{"detail", SAY("没有这个任务")}}, 404);
        }
        live->seen = std::chrono::steady_clock::now();
        auto p = live->progress;
        // **取走了也不删。** 位置在它落定那一刻就已经放出来了，留着这条
        // 记录只为"对方重启之后照样取得到"（见 State::finished）。
        // 过了保质期由回收那条收走。
        // 预览只在问了、而且比它手里那张新时才带（几十 KB 一张，见
        // TaskProgress::preview）。没问的轮询一个字节都不多。
        const char* after = req.url_params.get("preview_after");
        if (!after || p.preview_step <= std::atoi(after)) p.preview.clear();
        return json_res(to_json(p));
    });

    CROW_ROUTE(app, "/task/<string>/cancel")
        .methods(crow::HTTPMethod::POST)(
            [state, gate](const crow::request& req, const std::string& id) {
            if (auto deny = gate(req)) return std::move(*deny);
            std::lock_guard lg(state->mu);
            const auto it = state->running.find(id);
            if (it == state->running.end()) {
                return json_res({{"detail", SAY("没有这个任务")}}, 404);
            }
            it->second->tok.request();
            return json_res({{"ok", true}});
        });

}

bool mount_worker_api(http::EngineApp& app, const config::Settings& settings,
                      const WorkerOptions& opts, TaskRunner runner,
                      Capacity capacity) {
    // **对外监听而没设口令就不挂。** 挂了等于谁都能派活过来烧这张卡、
    // 读走这台有哪些模型。判据和 run_worker 用的是同一个。
    if (const auto why = refuse_to_listen(opts.host, settings.peer.token);
        !why.empty()) {
        // `CROW_LOG_*` 是日志不是界面，不包（同 `server.cpp` 那两条）。
        CROW_LOG_WARNING << SAY_NEVER("没有开放对等互联的接口：") << why;
        return false;
    }
    // 只探一次，理由同 run_worker：detect 会跑 nvidia-smi。
    static const auto profile =
        models::HardwareProfile::detect(settings.vram_gb_override);
    static auto state = std::make_shared<State>();
    // 能同时接几件由执行器那头说了算（见头文件 Capacity 那段）。
    state->capacity = std::move(capacity);
    start_reaper(state);
    mount_worker_api_impl(app, settings, opts, profile, state, runner);
    return true;
}

bool run_worker(const config::Settings& settings, const WorkerOptions& opts) {
    // **先看这个地址开不开得起。** 对外监听而没设口令的话当场拒绝——
    // 那种情况下谁都能派活过来烧这张卡、读走这台有哪些模型。
    // 理由和判据在 peer_auth.hpp。
    if (const auto why = refuse_to_listen(opts.host, settings.peer.token);
        !why.empty()) {
        std::cerr << why << std::endl;
        return false;
    }

    // **绑卡靠 CUDA_VISIBLE_DEVICES。** 在建任何 ggml 上下文之前设，
    // 之后再设没用——后端初始化的时候就把设备列表读走了。
    //
    // ⚠️ 这一条在只有一张卡的机器上验不了。要是它对 ggml 的 CUDA 后端
    // 不生效，表现是**八个进程全挤在卡 0 上，看着在并行实际在排队，
    // 而且一声不吭**。上多卡机器第一件事就是验它。
    paths::set_env("CUDA_VISIBLE_DEVICES", std::to_string(opts.gpu));

    // 和主进程一样注册两个槽。**每个工作进程一份**——
    // 进程边界把"每卡一份预算"白送了，Scheduler 一行没改。
    // **把 sd.cpp 的日志接到 stderr。** 不接的话一条都不会落地，而出图失败
    // 时抛的是"看一眼上面 sd.cpp 打的日志"——上面什么都没有。工作进程是
    // 独立进程，它的 stderr 就是排查出图问题唯一的地方。
    sd_log_to_stderr();

    // **只探一次。** detect 会跑 nvidia-smi，一百毫秒上下；
    // /status 每次重探的话，别的机器轮询一下就是白白拖慢这台。
    const auto profile =
        models::HardwareProfile::detect(settings.vram_gb_override);

    // **把启动时这一份装进 runtime，工作进程里原来没人做这件事。**
    //
    // `config::runtime()` 是个单例，完整服务在 server.cpp 起来时会
    // `replace(settings)`，工作进程这条路一处都没有——于是它一直是默认
    // 构造的空配置。后果有两层，都指向同一个症状：
    //
    // 一，从界面把模型下到这台机器上，下完那一下 `on_item_done` 会调
    //     setup_api 的 `persist()`。那个函数写文件之外还要
    //     `runtime().snapshot()` → 打补丁 → `replace()`，而它拿到的是空配置，
    //     于是内存里那份变成"默认值 + 这一组"，别的键全丢了。
    // 二，下面 `/status` 和出图槽读的都是**启动那一刻**的拷贝，配置文件
    //     后来写对了也看不见。
    //
    // 实测（2026-09-15）：Qwen-Image-Edit 20 GB 下完、config.toml 里
    // `image` 也写上了、文件就在盘上，这台仍然一直报「[models].image
    // 没配，或者文件不在」，派活那头永远不会把首帧派过来——除非重启它。
    // 而整个「在界面上给远程机器装模型」就是为了不用去碰那台机器。
    config::runtime().replace(settings);
    // 上一轮被杀时孤儿 curl 下全的 `.part` 收编进来，见 adopt_finished_parts。
    setup::adopt_finished_parts(settings.models.dir_path(settings.workspace_path()));

    // 槽也读活的那一份，理由同上：下完模型不重启就该能用。
    register_sd_slots([] { return config::runtime().snapshot(); }, profile);

    auto state = std::make_shared<State>();
    // `--worker` 这条也要自己收：它一样会被一台下了线的机器丢下没人认领的活。
    start_reaper(state);
    // 门在 app 里（`http/crow_guard.hpp`）：工作进程也监听端口——按卡拉起的那几个在
    // 回环上，浏览器里的网页一样摸得到；`/task` 能让它把产物写到任意路径。
    http::EngineApp app;
    app.get_middleware<http::SameSiteGuard>().policy = http::guard_policy_for(opts.host);
    app.loglevel(crow::LogLevel::Warning);
    // 连接计时器放宽到 60 秒：一段 blob 在 10 KB/s 的链路上也要二十多秒，
    // 默认 5 秒会把它切断（见上面 /blob 那条）。
    app.timeout(60);
    // 几个 io_service：一条慢连接（往外发 blob、收 blob）只占住一个，
    // 别的连接上的 /task、轮询照常有人接。
    app.concurrency(4);

    mount_worker_api_impl(app, settings, opts, profile, state, {});

    CROW_LOG_INFO << SAY_NEVER("工作进程 gpu=") << opts.gpu << SAY_NEVER(" 听 ") << opts.host << ":"
                  << opts.port;
    // **端口还被上一个进程占着就等它，别当场退出。** 重启工作进程时上一个
    // 可能正在出片、几秒才退干净；这时 bind 报 Address already in use，新的
    // 一退，机器上就没有工作进程了，派活方那头整台变灰（2026-09-16 实撞）。
    // 等最多一分钟，每两秒试一次；别的错照样抛。
    for (int attempt = 1;; ++attempt) {
        try {
            app.bindaddr(opts.host).port(static_cast<std::uint16_t>(opts.port)).run();
            break;
        } catch (const std::exception& e) {
            const std::string what = e.what();
            if (what.find("Address already in use") == std::string::npos || attempt >= 30) {
                throw;
            }
            CROW_LOG_WARNING << SAY_NEVER("端口 ") << opts.port << SAY_NEVER(" 还被占着（多半是上一个")
                             << SAY_NEVER("工作进程还没退干净），两秒后再试（") << attempt << "/30）";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    // ---- 收到信号，run() 返回了 ----
    //
    // **别让它走到静态析构。** 出片那个线程是 detach 的，可能还在 sd.cpp 里；
    // 调度器里的 SdContext（带 CUDA 上下文）在它脚下被析构，最后是
    // std::terminate——systemctl restart 时 journal 里那条
    // "code=dumped, status=6/ABRT" 就是它。
    //
    // 先取消当前任务，等它自己退出来（取消令牌在采样回调里查，一两步就停），
    // 最多等 30 秒，然后 _Exit：跳过所有析构。工作进程没有任何值得析构的
    // 东西——它的全部状态是内存里的模型缓存，进程一没就没了。
    {
        std::vector<std::shared_ptr<Live>> running;
        {
            std::lock_guard lg(state->mu);
            for (auto& [_, live] : state->running) running.push_back(live);
        }
        for (const auto& one : running) {
            one->tok.request();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;) {
                {
                    std::lock_guard lg(state->mu);
                    const auto& st = one->progress.state;
                    if (st == "done" || st == "failed") break;
                }
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    std::_Exit(0);
    return true;   // 到不了，但签名要它
}

}  // namespace changji::infer
