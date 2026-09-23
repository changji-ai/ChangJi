#include "util/say.hpp"
#include "http/server.hpp"

#include <crow.h>

#include <cstdlib>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <mutex>
#include <iterator>
#include <string>
#include <nlohmann/json.hpp>

#include "doctor/doctor.hpp"
#include "http/editing.hpp"
#include "http/media.hpp"
#include "http/ref_gen.hpp"
#include "http/tts_api.hpp"
#include "lan/sense.hpp"
#include "http/upload.hpp"
#include "http/readonly.hpp"
#include "config/runtime.hpp"
#include "config/writeback.hpp"
#include "util/text.hpp"
#include "http/batch.hpp"
#include "http/chat_api.hpp"
#include "http/memory_api.hpp"
#include "http/enums.hpp"
#include "http/rail.hpp"
#include "http/config_api.hpp"
#include "http/episodes.hpp"
#include "http/llm_info.hpp"
#include "llm/thinking.hpp"
#include "http/planning.hpp"
#include "http/projects.hpp"
#include "http/oneclick.hpp"
#include "http/run.hpp"
#include "http/voices.hpp"
#include "http/scripting.hpp"
#include "http/story_api.hpp"
#include "http/setup_api.hpp"
#include "setup/autostart.hpp"
#include "setup/update_check.hpp"
#include "setup/downloader.hpp"
#include "llm/client.hpp"
#include "llm/local_client.hpp"
#include "infer/llama_chat.hpp"
#include "http/film.hpp"
#include "http/flow.hpp"
#include "util/paths.hpp"
#include "util/sysstat.hpp"
#include "http/webapp.hpp"
#include "http/ws.hpp"
#include "infer/worker_server.hpp"
#include "infer/node_prefs.hpp"
#include "infer/worker_pool.hpp"
#include "infer/node_proxy.hpp"
#include "infer/node_registry.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "http/job_stream.hpp"
#include "http/offload.hpp"
#include "pipeline/activity.hpp"
#include "pipeline/task_board.hpp"
#include "pipeline/jobs.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace changji::http {

namespace {

/// 开几条 Crow 线程。0 = 自己定。
///
/// **要多少条，看的是"能同时卡住几条"**，不是这台机器有多少核。会卡住
/// 一条线程的是那几个同步 AI 接口（写一章、改稿、出参考图、朗读），一个
/// 人手快也就同时点出三五个，而它们现在还会排队等显存——排着的那几个
/// 同样占着线程。给三十二条，等于给了一个人的操作留足余量，而代价只是
/// 三十来个基本闲着的线程。
///
/// 小机器上不必开这么多：取机器核数和 32 的小的那个，但不少于 8——
/// 少于 8 的话一条长任务就能吃掉可观的一份。
unsigned resolve_concurrency(unsigned want) {
    if (want > 0) return want;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return std::max(8u, std::min(32u, cores));
}

}  // namespace

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 统一的 JSON 响应。
///
/// Crow 自带 crow::json，但整个项目其它地方用的是 nlohmann——
/// 混用两套 JSON 库是长期的麻烦源。这里统一序列化成字符串再交给 Crow。
crow::response json_response(const json& body, int status = 200) {
    // **dump 用 replace，不用默认的 strict。**
    //
    // nlohmann 默认在碰到非法 UTF-8 时**抛异常**（type_error.316），而这个
    // 函数是所有响应的最后一步——抛在这儿就是整条接口 500，消息是一句
    // 「invalid UTF-8 byte at index …」，指不到是哪个字段带进来的。
    //
    // 非法字节进得来的路不止一条：磁盘上的文件名（`/api/outputs` 列的是
    // 整个 output 目录，谁往里扔一个别处拷来的文件就够了）、外部服务回的
    // 字段、模型输出里被截断的一段。这一族 2026-09-11 咬过一次，症状写在
    // `extract_json` 末尾那段注释里：「字节截断落在半个汉字上时整个
    // /api/script/series 都回 500，进度就看不见了」——那次是去修了源头，
    // 而源头不止一处。
    //
    // `replace` 把非法字节换成 U+FFFD（）。**合法输入逐字节不变**，所以
    // 对拍不受影响；换来的是"一个字显示成方块"而不是"整页打不开"。
    // 其余三个参数就是默认值（不缩进、空格、不转义非 ASCII）。
    crow::response res(status, body.dump(-1, ' ', false,
                                         json::error_handler_t::replace));
    // **不带 charset。** FastAPI 发的就是这个，对拍比响应头时发现
    // 两边差一个 "; charset=utf-8"。JSON 按 RFC 8259 本来就必须是 UTF-8，
    // 这个参数在 application/json 上是没注册的，加了不算更对。
    // 差异出现在每一个接口上，而前面那几层对拍只比 body 不比头，一直没看见。
    res.set_header("Content-Type", "application/json");
    return res;
}

/// 请求体里要不要走异步。**取出来就删掉**：下面那些处理函数各有各的
/// `forbid_extra`，留着它等于每一个都要去加一条白名单。
bool take_async(json& body) {
    if (!body.is_object() || !body.contains("async")) return false;
    const bool want = body.at("async").is_boolean() && body.at("async").get<bool>();
    body.erase("async");
    return want;
}

std::string stream_of(const json& body) {
    if (!body.is_object() || !body.contains("stream") ||
        !body.at("stream").is_string()) {
        return {};
    }
    return body.at("stream").get<std::string>();
}

/// 把一件慢活挪到后台线程上干，接口当场回一句"开始了"。
///
/// **为什么非这么办**：Crow 一条 I/O 线程管着一批连接，请求在它上面占多久，
/// 落在同一条线程上的连接就干等多久——写一章一两分钟，出一张图几十秒。
/// 实测那期间别的请求会卡满二十多秒，顶栏那块表（长连接）更是会直接冻住。
///
/// ⚠️ **响应仍然由 Crow 自己那条线程发出**，只不过发的是"开始了"。
/// 试过在后台线程上 `res.end()`，不行——详见 server.hpp 里 concurrency 那段。
///
/// 结果和错误都走那条 WebSocket（见 http/job_stream.hpp）。所以**没有
/// stream 就不能异步**：那时结果没地方送回去，照旧同步跑到底。老客户端、
/// curl、对拍脚本走的都是那条，一个字没变。
template <typename Work>
ApiResult start_async(const std::string& stream_id, Work work) {
    Offload::instance().post([stream_id, work] {
        // 挂上"这条线程在给谁干活"，里面每一步的思考流就不用各自去捞
        // stream 了（见 job_stream.hpp 的 current_stream）。
        const JobScope scope{stream_id};
        // **异步的活砸了，日志里要有一行。**
        //
        // 这三条路原来只 job_error 给前端，服务端一个字不写。后果是活砸了
        // 之后**引擎日志里只剩一条 202，再没有别的**——2026-09-16 实测：
        // 「重写整章」跑了十分钟，正文流了一半又被撤掉，日志里从头到尾就是
        //     Request: ... POST /api/story/chapter
        //     Response: ... /api/story/chapter 202 0
        // 两行。页面上那句红字一闪而过，翻日志什么都查不到，只能再跑十分钟。
        //
        // 这儿是**所有异步活的唯一出口**（写正文、改稿、出图、配音都从这儿
        // 过），补一行就全覆盖了。
        const auto fail = [&stream_id](const std::string& why) {
            CROW_LOG_ERROR << SAY_NEVER("异步作业砸了 [") << stream_id << "]：" << why;
            job_error(stream_id, why);
        };
        try {
            const ApiResult r = work();
            // 处理函数自己回了个错状态码（不抛，直接回）也要算砸了，
            // 否则界面会把一句报错当成结果显示出来。
            if (r.status >= 400) {
                const std::string msg =
                    r.body.is_object() && r.body.contains("detail") &&
                            r.body.at("detail").is_string()
                        ? r.body.at("detail").get<std::string>()
                        : SAY("没干成");
                fail(msg);
            } else {
                job_done(stream_id, r.body);
            }
        } catch (const ApiError& e) {
            fail(e.what());
        } catch (const std::exception& e) {
            fail(e.what());
        }
    });
    return {202, {{"started", true}, {"stream", stream_id}}};
}



/// 跑一遍体检。给了项目路径就按**这部电影**的配置跑。
///
/// **画面规格是每部电影自己的**（项目目录的 changji.toml 里那个 [video]）。
/// 不读它的话「出片画布」那一项查的是全局默认、而出片用的是这部电影的那个
/// ——项目切到 2k 之后体检照样说 544×928 没问题，这条检查等于没有。
/// 实测撞到过。
///
/// ⚠️ **那一份读不了不能把整份报告带走。** `load_settings` 在项目的
/// changji.toml 语法坏了时会抛（read_toml_into 里那个 runtime_error，
/// 消息里带着文件名和行号）。原来这一句是裸着的：一抛就穿出处理函数，
/// Crow 回一个光秃秃的 500，前端拿到的是「请求失败（500）」——
/// **而"你的配置文件坏了"正是体检存在的理由**，偏偏这时候它整个不见了，
/// 连带「出片画布」「显卡」这些和那份配置无关的项也一起没了。
///
/// 现在退回全局那份接着跑，把原因当成一条 FAIL 摆在最前面：`can_run()`
/// 看见 FAIL 就是假，该拦的照样拦，而那句带行号的话终于送得到人眼前。
/// 这也和 doctor.cpp 里 `guarded()` 的规矩一致：单项炸了降级成一条，
/// 其余照跑。
doctor::Report doctor_for(const char* raw_path) {
    auto settings = config::runtime().snapshot();
    std::string broken;
    if (raw_path != nullptr && *raw_path != '\0') {
        try {
            settings = config::load_settings(changji::paths::from_utf8(raw_path));
        } catch (const std::exception& e) {
            broken = e.what();
        }
    }
    doctor::Report r = doctor::run_checks(settings);
    if (!broken.empty()) {
        r.checks.insert(
            r.checks.begin(),
            doctor::Check{SAY("这部电影的配置"), doctor::Level::FAIL, broken,
                          SAY("改掉那个文件里的语法错误再刷新。在修好之前，"
                              "下面这些项查的是全局配置（用户目录那份），"
                              "和这部电影真正会用的不是一回事。")});
    }
    return r;
}

/// 取布尔查询参数。
///
/// 认的取值抄 FastAPI：true/1/on/yes/y/t，大小写不论。别的一律 false——
/// FastAPI 那边不认识的值是 422，但前端只会发 URLSearchParams 序列化出来的
/// "true"/"false"，为一个到不了的分支加一条错误路径不划算。
bool query_bool(const crow::request& req, const char* key, bool def = false) {
    const char* v = req.url_params.get(key);
    if (v == nullptr) return def;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s == "true" || s == "1" || s == "on" || s == "yes" || s == "y" ||
           s == "t";
}

/// 取一个**必填**的查询参数。没这个键就抛 422。
///
/// **"没给"和"给了个空的"是两回事。** FastAPI 对 `path: str` 这种没有默认值
/// 的参数，缺了就在处理函数跑之前拦下来回 422；而 `?path=` 是一个合法的
/// 空字符串，会进处理函数然后回 400。少了这个区分的话，缺参数时 C++ 回的是
/// 404「没有章节 」——注意末尾那个空格，那是拿空参数去查的结果。
///
/// 这一条是实时对拍抓出来的：单元测试直接调处理函数，天然绕过了这一层。
std::string required_query(const crow::request& req, const char* key) {
    const char* v = req.url_params.get(key);
    if (v == nullptr) throw unprocessable_query(key);
    return v;
}

/// 取查询参数。Crow 拿不到时返回 nullptr，转成空串——
/// 空串该怎么处理由各个接口自己决定（比如 path 为空是 400 不是 500）。
std::string query(const crow::request& req, const char* key) {
    const char* v = req.url_params.get(key);
    return v ? std::string(v) : std::string();
}

/// 正在跑的那个 app。`request_stop()` 靠它停服。
///
/// **做成指针 + 一把锁，不是裸指针。** 关窗那一下 Qt 那条线程调
/// `request_stop()`，而引擎线程可能正好在 `run()` 的收尾里——两边同时动
/// 这个变量就是数据竞争，而竞争出来的表现是"偶尔关不掉窗口"，最难查。
std::mutex g_app_mu;
crow::SimpleApp* g_app = nullptr;

}  // namespace

int pick_free_port(const std::string& host) {
    try {
        asio::io_context io;
        asio::ip::tcp::acceptor acc(io);
        const asio::ip::tcp::endpoint ep(asio::ip::make_address(host), 0);
        acc.open(ep.protocol());
        // **不设 SO_REUSEADDR**：这里要的就是"真没人占"，
        // 设了的话拿回来的端口可能正被别人的 TIME_WAIT 连接占着。
        acc.bind(ep);
        const int port = static_cast<int>(acc.local_endpoint().port());
        acc.close();
        return port;
    } catch (const std::exception&) {
        return 0;
    }
}

void request_stop() {
    // ⚠️ **先关广播，再停服务。顺序不能反。**
    //
    // `app.stop()` 之后 Crow 那个 io 上下文很快就没了，而 `send_text` 是往它
    // 里头 post 一件事——连接对象还在、指针还有效，post 进去的却是一具尸体。
    // 模型正一段段吐字的时候退出程序就会撞上（每个 delta 都推一条），
    // 2026-09-21 实撞，SIGSEGV 在 `post_immediate_completion` 里。
    //
    // 这儿关在前面：手上正在发的那一条还发得完（它持着 Hub 那把锁，而
    // io 上下文这会儿还活着），之后的一条都进不来。
    ws::hub().shutdown();
    std::lock_guard<std::mutex> lk(g_app_mu);
    if (g_app) g_app->stop();
}

void run(const config::Settings& settings, const Options& opts) {
    crow::SimpleApp app;
    {
        std::lock_guard<std::mutex> lk(g_app_mu);
        g_app = &app;
    }
    // 出这个函数时把它摘掉——摘晚了 `request_stop()` 会去动一个已经析构的
    // 对象；而那种崩溃发生在退出路径上，崩了也只留一份让人跑偏的报告。
    struct ClearApp {
        ~ClearApp() {
            std::lock_guard<std::mutex> lk(g_app_mu);
            g_app = nullptr;
        }
    } clear_app;

    // 同一个进程里可能再起一次（桌面端换引擎地址、用例里连着跑几遍），
    // 上一轮 `shutdown()` 关掉的广播这儿重新开。
    ws::hub().reopen();

    // 起服务前先把配置放进 runtime。后面所有读配置的地方都从那里拿——
    // /api/connections 和 /api/settings 能在运行期改它，
    // 各处捕获一份的话，改完之后有的地方是新的有的是旧的。
    config::runtime().replace(settings);
    // 上一轮被杀时孤儿 curl 下全的 `.part` 收编进来，见 adopt_finished_parts。
    setup::adopt_finished_parts(settings.models.dir_path(settings.workspace_path()));
    // 画参考图和出首帧走同一条后端——本机没出图模型时能派给别的机器。
    // 见 RefRendererProvider。
    set_ref_renderer([](const config::Settings& s, const models::ProjectStore& store) {
        // **整份 Backends 要活到画完。** 池那条 FrameRenderer 捕的是裸指针，
        // 池本身由 Backends::keepalive 持有——只取 `.frame` 的话临时对象一析构
        // 池就没了，下一步就是段错误（2026-09-16 一键出图当场把引擎打崩）。
        auto b = std::make_shared<pipeline::Backends>(default_run_deps().backends(s, store));
        RefBackend out;
        out.render = stages::FrameRenderer(
            [b](const models::Shot& shot, const stages::PromptBundle& prompts,
                const models::TierSpec& spec, const std::filesystem::path& dest,
                pipeline::CancelToken& tok, const infer::StepCallback& on_step) {
                b->frame(shot, prompts, spec, dest, tok, on_step);
            });
        // **和出首帧同一个数**：参考图走的就是那个池的那批位置。
        out.lanes = std::max(1, b->render_lanes);
        return out;
    });

    // job 表往 WebSocket 推消息，但它不认识 WebSocket——中间靠这个回调接上。
    // 分层的好处很实在：jobs.cpp 因此不用链 Crow，单元测试才编得动。
    pipeline::jobs().set_sink(
        [](const std::string& job_id, const json& msg) {
            ws::hub().broadcast(job_id, msg);
        });

    // 出图和出片的两个模型槽注册到调度器上。**一个进程只注册一次**：
    // 槽已经加载着的时候重新注册会抛异常，所以不能放在开跑的路径上。
    //
    // 注册不等于加载。真正加载要等第一次 acquire——一个视频模型好几个 G，
    // 起服务时就加载的话，只想看看分镜表的人也要等上几十秒。
    // 同工作进程那边：不接的话 sd.cpp 一条日志都不会落地，
    // 而出图失败时抛的是"看一眼上面 sd.cpp 打的日志"。
    infer::sd_log_to_stderr();
    infer::register_sd_slots([] { return config::runtime().snapshot(); },
                             config::runtime().profile());
    // [llm].backend = "local" 时把大模型也挂上调度器。远端那条不注册——
    // 没有本地权重，注册一个装不上的槽只会在借它时抛没意义的错。
    // **不在这儿预装**：注册不等于加载，调度器是借出时才装的
    // ——用户 2026-09-11 重申："用的时候才加载是对的，不做启动预载"。
    llm::register_llm_slot([] { return config::runtime().snapshot(); },
                           config::runtime().profile());

    // **把工作进程那套接口也挂上：一台机器一个进程、一条连接。**
    //
    // 用户 2026-09-17：「我不说了只起一个，互联一个机器只连一次」。
    // 在这之前做不到——那几条路由只长在 `--worker` 那个独立 app 上，
    // 主程序的 /health、/task 会被下面单页应用的兜底路由接走、回一段 HTML，
    // 派活那头判成"不在线"（当天实撞：curl 回 200 而 body 是网页，
    // 我照着那个 200 得出过一个错结论）。于是远程要当节点用，只能手动起
    // worker，还得一张卡一个、各带 --host。
    //
    // 挂上之后，别的机器在 `[[peer.nodes]]` 里填这台的**主程序地址**即可。
    // 对外监听而 [peer].token 空着时它自己不挂（见 mount_worker_api）。
    {
        infer::WorkerOptions wo;
        wo.host = opts.host;
        wo.port = opts.port;
        // 主程序不绑卡（不设 CUDA_VISIBLE_DEVICES），这个数只进 /status
        // 那份自我介绍。-1 = 不声称自己是哪张卡。
        wo.gpu = -1;
        // **外来任务交给本机那几张卡**，别在主进程里就地跑——一个进程
        // 只用得上一张卡（用户 2026-09-17：「只用了一张卡」）。
        // 单卡机上这个回空，挂载那头就就地跑，和以前一样。
        const FarmRunner fr = local_farm_runner(settings);
        infer::mount_worker_api(app, settings, wo, fr.run, fr.capacity);
    }
    // **不在这儿预装大模型。** 注册不等于加载，调度器是**借出时**才装的
    // ——用户 2026-09-11 重申："用的时候才加载是对的，不做启动预载"。
    //
    // 这一行原来是 `warm_llm_in_background(...)`，起服务就把权重推进显存。
    // 代价是：一台什么都没干的机器，显存上来就少一大块（这台 5090 上是
    // 22 GB，见 config::LLMConfig::context_tokens 那段账），而用户看着
    // 顶栏"没有任何任务，显存也占着"，第一反应是表坏了。
    //
    // 换来的只是第一次写字省下一次加载。那一次等待是看得见、也说得清的
    // （界面上会说"正在装大模型"）；而显存被莫名占着是看不懂的。

    // 顶栏那三个小表：CPU、内存、每张卡。**推，不轮询**（用户 2026-09-11：
    // 「使用 ws 方式」）。
    //
    // **采样单独一条线程**，这里和 /api/system 都只读它采好的那份。
    // 原因是问卡会挂：显卡满负荷时 NVML 被驱动挂住好几秒（实测大模型
    // 生成时 13.4 秒，还有一次超过 20 秒）。以前两处各自现采，于是一开始
    // 写字，`/api/system` 就从 1 毫秒变成十几秒、推送也跟着停——而前端
    // 八秒没消息就当断线，把整块表清掉。**表恰恰在最该看的时候空掉。**
    // 采归采、推归推之后，推送永远准时，数据顶多旧几秒，而旧了多少
    // 界面上说得出来（age_s）。
    sysstat::start_sampler();

    // 机器表也在起服务时先问一遍。**关着的那台要走满探活超时**（实测
    // 3.2 秒），不预热的话第一个打开设置页的人就得等它——而那一页恰恰是
    // "出事了才打开"的那一页。和上面多卡子进程预热是同一条理由。
    infer::node_registry().warm(settings);

    // 停的方式：run() 回来后析构，析构里先立旗再 join。
    // 睡眠切成 100ms 一段，Ctrl+C 之后最多再等零点一秒。
    std::atomic<bool> stop_pump{false};
    struct PumpAtExit {
        std::atomic<bool>& stop;
        std::thread t;
        ~PumpAtExit() {
            stop = true;
            if (t.joinable()) t.join();
            // 采样线程也在这儿收：它可能正挂在 NVML 上，join 要等，
            // 但不等就是在它还在写缓存的时候把缓存析构掉。
            sysstat::stop_sampler();
        }
    } sys_pump{stop_pump, std::thread([&stop_pump] {
        while (!stop_pump) {
            if (ws::hub().subscriber_count("system") > 0) {
                json msg = sysstat::to_json(sysstat::latest());
                // **搭这趟车，不另开一条。** 顶栏那块"AI 作业中"要的就是
                // 两秒一次的心跳，而这条通道已经在跑了；另开一个轮询等于
                // 为同一个节奏做两遍功。
                msg["jobs"] = pipeline::running_work();
                msg["type"] = "system";
                msg["job_id"] = "system";
                ws::hub().broadcast("system", msg);
            }
            for (int i = 0; i < 20 && !stop_pump; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    })};

    // ---- REST ----

    CROW_ROUTE(app, "/api/health")([] {
        return json_response({{"ok", true}, {"service", "changji"}});
    });

    // 那张「机器 × 能力」的表。本机也是一行，不是特例——
    // 「本地 vs 远程」是同一张表上的两个格子，不是两条代码路径。
    //
    // **答得慢是正常的**：它要挨个问对面的 /status（每台最多 3 秒）。
    // 五秒缓存兜着，连着刷不会把对面问烦。
    CROW_ROUTE(app, "/api/nodes")([] {
        return json_response(infer::nodes_json(config::runtime().snapshot()));
    });

    // 加一台别的机器 / 去掉一台。**写进全局配置的 `[[peer.nodes]]`。**
    //
    // 在这之前这件事只能手改配置文件——而那张表就摆在设置页上，上面每台
    // 机器每个能力都能点，唯独"这张表从哪儿来"得去翻文档、找配置文件、
    // 记住 `[[peer.nodes]]` 这个写法（用户 2026-09-15 提的：远程服务器应该
    // 在设置里直接加）。
    //
    // 和上面那条 `/off` 不一样，这条**改的是机器的属性**，所以落 config.toml
    // 而不是项目库里那份 nodes.json：换个项目不该换一套机器。
    const auto peer_nodes_json = [](const config::Settings& s) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : s.peer.nodes) {
            nlohmann::json one{{"url", n.url}, {"token", n.token}};
            nlohmann::json off = nlohmann::json::array();
            for (const auto& c : n.off) off.push_back(c);
            one["off"] = off;
            arr.push_back(std::move(one));
        }
        return arr;
    };

    /// 地址长得像不像一台机器。**只挡明显不对的**：能不能连上由那张表说，
    /// 这儿挡的是"填了个明显不是地址的东西"——那种错误在表上显示成
    /// "连不上"，用户会去查网络，而问题在他自己刚敲的那一行。
    const auto bad_peer_url = [](const std::string& url) -> std::string {
        if (url.empty()) return SAY("要填地址");
        if (url == infer::kLocalEndpoint) return SAY("local 是本机，不用加");
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            return SAY("地址要以 http:// 或 https:// 开头，比如 "
                       "http://192.168.1.20:9101");
        }
        const auto rest = url.substr(url.find("//") + 2);
        if (rest.empty() || rest.front() == '/') return SAY("地址里没有主机名");
        if (rest.find(' ') != std::string::npos) return SAY("地址里不能有空格");
        return {};
    };

    CROW_ROUTE(app, "/api/nodes/add")
        .methods("POST"_method)([peer_nodes_json,
                                 bad_peer_url](const crow::request& req) {
            const auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                return json_response({{"detail", SAY("请求体不是一个 JSON 对象")}},
                                     400);
            }
            std::string url = changji::text::strip_ws(
                body.value("url", std::string()));
            // 末尾的斜杠去掉：`http://x:9101/` 和 `http://x:9101` 是同一台，
            // 留着的话查重查不出来，表上就出现两行一模一样的机器。
            while (url.size() > 8 && url.back() == '/') url.pop_back();
            if (const auto why = bad_peer_url(url); !why.empty()) {
                return json_response({{"detail", why}}, 422);
            }
            // **正在跑的时候不许改。** 同 /api/nodes/off：半章换机器会让
            // 前后画风对不上。
            if (pipeline::jobs().running(pipeline::JobKind::Run)) {
                return json_response(
                    {{"detail", SAY("正在跑，这时候改派活的机器会把这一章跑坏")}},
                    409);
            }

            auto s = config::runtime().snapshot();
            for (const auto& n : s.peer.nodes) {
                if (n.url == url) {
                    return json_response(
                        {{"detail", SAY("这台已经在表上了：") + url}}, 409);
                }
            }
            auto arr = peer_nodes_json(s);
            arr.push_back({{"url", url},
                           {"token", body.value("token", std::string())},
                           {"off", nlohmann::json::array()}});
            try {
                config::save_peer_nodes(arr);
            } catch (const std::exception& e) {
                return json_response({{"detail", e.what()}}, 500);
            }
            // 写完要让这个进程也跟着变：不重读的话，表上是新的、
            // 真派活时用的还是旧的那一份。
            s = config::load_settings();
            config::runtime().replace(s);
            infer::node_registry().refresh(s);
            return json_response(infer::nodes_json(s));
        });

    CROW_ROUTE(app, "/api/nodes/remove")
        .methods("POST"_method)([peer_nodes_json](const crow::request& req) {
            const auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                return json_response({{"detail", SAY("请求体不是一个 JSON 对象")}},
                                     400);
            }
            const std::string url = body.value("url", std::string());
            if (url.empty()) return json_response({{"detail", SAY("要 url")}}, 422);
            if (pipeline::jobs().running(pipeline::JobKind::Run)) {
                return json_response(
                    {{"detail", SAY("正在跑，这时候改派活的机器会把这一章跑坏")}},
                    409);
            }

            auto s = config::runtime().snapshot();
            auto arr = peer_nodes_json(s);
            nlohmann::json left = nlohmann::json::array();
            for (const auto& n : arr) {
                if (n.value("url", std::string()) != url) left.push_back(n);
            }
            if (left.size() == arr.size()) {
                return json_response({{"detail", SAY("表上没有这台：") + url}}, 404);
            }
            try {
                config::save_peer_nodes(left);
            } catch (const std::exception& e) {
                return json_response({{"detail", e.what()}}, 500);
            }
            s = config::load_settings();
            config::runtime().replace(s);
            infer::node_registry().refresh(s);
            return json_response(infer::nodes_json(s));
        });

    // 改一台：换地址、换口令。
    //
    // **不做成"先删再加"**：那是两次写配置，中间任何一步失败（地址不合法、
    // 新地址和别人撞了、写盘失败）都会把这台机器整个丢掉，而用户以为
    // 自己只是改了个端口。这条一次算完再落一次盘。
    CROW_ROUTE(app, "/api/nodes/update")
        .methods("POST"_method)([peer_nodes_json,
                                 bad_peer_url](const crow::request& req) {
            const auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                return json_response({{"detail", SAY("请求体不是一个 JSON 对象")}},
                                     400);
            }
            const std::string url = body.value("url", std::string());
            if (url.empty()) return json_response({{"detail", SAY("要 url")}}, 422);

            // 新地址不给就是不改地址，只改口令。
            std::string next = changji::text::strip_ws(
                body.value("new_url", url));
            while (next.size() > 8 && next.back() == '/') next.pop_back();
            if (const auto why = bad_peer_url(next); !why.empty()) {
                return json_response({{"detail", why}}, 422);
            }
            if (pipeline::jobs().running(pipeline::JobKind::Run)) {
                return json_response(
                    {{"detail", SAY("正在跑，这时候改派活的机器会把这一章跑坏")}},
                    409);
            }

            auto s = config::runtime().snapshot();
            auto arr = peer_nodes_json(s);
            bool found = false;
            for (auto& n : arr) {
                if (n.value("url", std::string()) != url) {
                    // 换到一个别人已经占着的地址上，和 /add 撞车一个意思。
                    if (n.value("url", std::string()) == next) {
                        return json_response(
                            {{"detail", SAY("这台已经在表上了：") + next}}, 409);
                    }
                    continue;
                }
                found = true;
                n["url"] = next;
                // **口令这个键不给就不动它。** 给空串才是"清掉"——
                // 界面上那一格留空时用户想的是"不改"还是"删掉"分不出来，
                // 所以由前端明确传，这边只照做。
                if (body.contains("token")) {
                    n["token"] = body.value("token", std::string());
                }
            }
            if (!found) {
                return json_response({{"detail", SAY("表上没有这台：") + url}}, 404);
            }
            try {
                config::save_peer_nodes(arr);
            } catch (const std::exception& e) {
                return json_response({{"detail", e.what()}}, 500);
            }
            s = config::load_settings();
            config::runtime().replace(s);
            infer::node_registry().refresh(s);
            return json_response(infer::nodes_json(s));
        });

    // 点那张表上的一个格子：关掉／打开某台的某个能力。
    //
    // 存在 <项目库>/nodes.json，不写 config.toml——理由见
    // infer/node_prefs.hpp（那边的 off 是数组表，逐行写回那套认不了）。
    CROW_ROUTE(app, "/api/nodes/off")
        .methods("POST"_method)([](const crow::request& req) {
            const auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                return json_response({{"detail", SAY("请求体不是一个 JSON 对象")}},
                                     400);
            }
            const std::string url = body.value("url", std::string());
            const auto cap =
                infer::capability_from(body.value("cap", std::string()));
            if (url.empty() || !cap) {
                return json_response(
                    {{"detail", SAY("要 url 和 cap（llm/tts/frame/video/assemble）")}},
                    422);
            }
            // **正在跑的时候不许改。** 半章换机器会让前后画风对不上——
            // 和 /api/connections 那边"正在跑时不许换机器"是同一条规矩。
            if (pipeline::jobs().running(pipeline::JobKind::Run)) {
                return json_response(
                    {{"detail", SAY("正在跑，这时候改派活的机器会把这一章跑坏")}},
                    409);
            }

            const auto s = config::runtime().snapshot();
            const auto ws = s.workspace_path();
            auto prefs = infer::load_node_prefs(ws);
            if (body.value("off", false)) {
                prefs[url].insert(*cap);
            } else if (auto it = prefs.find(url); it != prefs.end()) {
                it->second.erase(*cap);
            }
            try {
                infer::save_node_prefs(ws, prefs);
            } catch (const std::exception& e) {
                // 写不进去要当场说。默默回到原样的话，用户会以为点生效了。
                return json_response({{"detail", e.what()}}, 500);
            }
            return json_response(infer::nodes_json(s));
        });

    // 下面这些从 runtime 取而不是用 run() 收到的那份 settings：
    // /api/connections 和 /api/settings 能在运行期改配置，
    // 用捕获的那份的话，改完之后体检和硬件画像还是老的，
    // 用户会以为改动没生效。
    CROW_ROUTE(app, "/api/doctor")([](const crow::request& req) {
        // 体检里有三项要发网络请求，最坏情况阻塞二十多秒。
        // Crow 是线程池模型，这只占住一个工作线程，不影响其它请求。
        return json_response(
            doctor::to_json(doctor_for(req.url_params.get("path"))));
    });

    // ---- 阶段 2：只读接口 ----
    //
    // 处理逻辑放在 readonly.cpp 里的纯函数，这里只负责取查询参数和转响应。
    // 那些函数不碰 crow 类型，单元测试能不起服务就把它们跑一遍。

    CROW_ROUTE(app, "/api/hardware")([](const crow::request& req) {
        auto r = guard([&] {
            // **给了项目就按这部电影算。** 单镜最长是四道夹子连乘出来的，
            // 其中一道（[video].max_shot_s）是电影的属性——不按项目算的话，
            // 这一栏报的是全局默认，而排分镜用的是项目那一份，两个数对不上。
            const char* raw = req.url_params.get("path");
            const std::string path = raw ? raw : "";
            if (!path.empty()) {
                const config::Settings s =
                    config::load_settings(paths::from_utf8(path));
                config::apply_video_limits(s);
                return get_hardware(s, config::runtime().profile());
            }
            return get_hardware(config::runtime().snapshot(),
                                config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    // 此刻的负载，一次性的。顶栏走的是 WebSocket 那条（订 "system"），
    // 这个留给 curl 看一眼和排查用。
    CROW_ROUTE(app, "/api/system")([] {
        auto r = guard([]() -> ApiResult {
            auto body = sysstat::to_json(sysstat::latest());
            // **和推过去的那份一样**，而且是同一个函数拼的——见
            // pipeline::running_work()。两边各拼一次的话迟早只改一边。
            body["jobs"] = pipeline::running_work();
            return {200, std::move(body)};
        });
        return json_response(r.body, r.status);
    });

    // ---- 任务页面 ----
    //
    // 用户 2026-09-17：「增加任务页面显示正在做的（已经用时，结束图标按钮）、
    // 排队中的（预计什么时候开始，取消图标按钮）、已经做完的（耗时），如果是
    // 大模型有思考的还得显示思考点击展开思考内容」。
    //
    // 账在 pipeline/task_board.hpp，这儿只是三个出口。
    CROW_ROUTE(app, "/api/tasks")([](const crow::request& req) {
        auto r = guard([&]() -> ApiResult {
            const char* p = req.url_params.get("project");
            return {200, pipeline::task_board(p ? p : "")};
        });
        return json_response(r.body, r.status);
    });

    // **思考正文单独取。** 一次写作的思考几千字，塞进上面那份两秒推一次的
    // 账里的话，它一件就能把整条通道占满。页面点开那一下才来要。
    CROW_ROUTE(app, "/api/task/thinking")([](const crow::request& req) {
        auto r = guard([&]() -> ApiResult {
            const char* id = req.url_params.get("id");
            if (id == nullptr) throw ApiError(400, SAY("要给 id"));
            const char* from = req.url_params.get("from");
            const auto t = pipeline::task_thinking(
                std::strtoull(id, nullptr, 10),
                from != nullptr ? std::strtoull(from, nullptr, 10) : 0);
            // `start` 大于问的那个 `from` 就是中间断了一截（思考太长被从头
            // 截过）。页面据此把手上那份丢掉重接，而不是把两段错接起来。
            return {200,
                    {{"thinking", t.text},
                     {"start", t.start},
                     {"end", t.end}}};
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/task/cancel").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                const json body = parse_body(req.body);
                if (!body.is_object() || !body.contains("id")) {
                    throw ApiError(400, SAY("要给 id"));
                }
                const auto id = body.at("id").is_string()
                                    ? std::strtoull(
                                          body.at("id").get<std::string>().c_str(),
                                          nullptr, 10)
                                    : body.at("id").get<std::uint64_t>();
                // 找不到不报错：按下去那一刻可能刚好干完，重复点也不该弹框。
                return ApiResult{200, {{"stopped", pipeline::cancel_task(id)}}};
            });
            return json_response(r.body, r.status);
        });

    // ---- 更新检查 ----
    //
    // 用户 2026-09-17：「增加自动更新」。**这一条只答"是不是最新"，不换二进
    // 制**——理由见 setup/update_check.hpp（换掉正在跑的可执行文件三个平台
    // 三种做法，而换错了的后果是程序起不来，那时候界面也没了）。
    CROW_ROUTE(app, "/api/update")([](const crow::request& req) {
        static const auto fetch = default_http_get();
        // 缓存在这儿放一个，不在那个文件里放静态的（见 update_check.hpp）。
        static setup::UpdateCache cache;
        auto r = guard([&]() -> ApiResult {
            const auto cfg = config::runtime().snapshot().update;
            // `?force=1` = 人点了「现在查一次」，那一下必须真去问。
            const char* f = req.url_params.get("force");
            const auto info = setup::cached_update(
                cache,
                cfg, CHANGJI_VERSION, [](const std::string& url) -> std::string {
                    // 超时写短：这是个后台检查，卡住不该让设置页跟着转。
                    const auto r = fetch(url, {}, 8.0);
                    return r.status == 200 ? r.body : std::string();
                },
                f != nullptr && std::string(f) != "0");
            return {200,
                    {{"current", info.current},
                     {"latest", info.latest},
                     {"newer", info.newer},
                     {"url", info.url},
                     {"built_at", info.built_at},
                     {"channel", cfg.channel},
                     {"auto_check", cfg.auto_check},
                     {"error", info.error}}};
        });
        return json_response(r.body, r.status);
    });

    // ---- 随系统启动 ----
    //
    // 用户 2026-09-17：「增加在设置中随系统启动开关」。三个平台都只是往登录
    // 时系统会扫的那个目录写一个文件，见 setup/autostart.hpp。
    // ---- 局域网感知 ----
    //
    // **这台自己那张卡忙成什么样，在这儿拼进去。** `lan/` 那一层只管网络，
    // 不该去碰采样器；而界面上那颗灯的正中间画的就是这个数，一次请求拿全
    // 比两次拼合强（两次的话两个数来自不同时刻，而它们要画在同一颗图标上）。
    const auto lan_json = [] {
        auto j = lan::Sense::instance().to_json();
        const auto load = sysstat::latest();
        // **报不出就是 -1，不是 0。** 苹果的卡就报不出——0 会被画成"闲着"，
        // 而那是个假消息。
        j["me"]["gpu"] = load.gpus.empty() ? -1.0 : load.gpus[0].util_percent;
        j["me"]["vramUsed"] = load.gpus.empty() ? 0.0 : load.gpus[0].vram_used_gb;
        j["me"]["vramTotal"] = load.gpus.empty() ? 0.0 : load.gpus[0].vram_total_gb;

        // **这一台在机器表上会长什么地址。**
        //
        // 设置页把机器表和局域网上配对的那几台**接成一张表**（一台机器一行），
        // 而"这一行和那一台是同一台"靠的就是这条地址。
        //
        // ⚠️ **地址在这头拼，界面只拿两个字符串对一下。** 界面那头照着
        // 主机名和端口自己拼一条的话，就是同一件事两处各写一遍
        //（CLAUDE.md 第八条）——哪天末尾那个点或者端口的处理改了，同一台
        // 机器就会在那张表上分成两行，而两行谁也不说自己是另一行。
        for (auto& p : j["peers"]) {
            p["url"] = lan::Sense::instance().their_url(p.value("id", std::string()));
        }
        return j;
    };

    //
    // 往外报一声「这儿有一台」，同时听听还有谁（`lan/sense.hpp`）。
    // **默认关着**，而且**看见 ≠ 能用**：对方只是出现在表上，要用得这头
    // 的人逐台点开（`lan/peers.hpp` 上那两段）。
    CROW_ROUTE(app, "/api/lan")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST)(
            [lan_json](const crow::request& req) {
                auto r = guard([&]() -> ApiResult {
                    if (req.method == crow::HTTPMethod::POST) {
                        const json body = parse_body(req.body);
                        if (!body.is_object() || !body.contains("on") ||
                            !body.at("on").is_boolean()) {
                            throw ApiError(422, SAY("要一个布尔的 on"));
                        }
                        // **这个平台没接上就照实说**，别默默把开关吞了：
                        // 界面会一直显示"开着"，而什么都没发生。
                        if (body.at("on").get<bool>() && !lan::Sense::supported()) {
                            // ⚠️ **这句话里别再点名"只有某某接上了"。**
                            // 原来写的是「眼下只有 macOS 那一档接上了」，
                            // 而 2026-09-22 Linux 也接上了——这种句子每接
                            // 一个平台就过期一次，而过期了没人会想起来改。
                            throw ApiError(501,
                                           SAY("这台机器上还没接局域网感知"
                                               "（这一档要系统自带的 mDNS，"
                                               "这个系统上没有）"));
                        }
                        lan::Sense::instance().set_on(body.at("on").get<bool>());
                    }
                    return {200, lan_json()};
                });
                return json_response(r.body, r.status);
            });

    // 给一台权限 / 收回来。**两档分开**：能用是一档，能挑模型参数是另一档，
    // 理由写在 `lan/peers.hpp` 上。
    CROW_ROUTE(app, "/api/lan/allow")
        .methods("POST"_method)([lan_json](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                const json body = parse_body(req.body);
                const std::string id = body.value("id", std::string());
                if (id.empty()) throw ApiError(422, SAY("要 id"));
                lan::Grant g;
                g.use = body.value("use", false);
                g.params = body.value("params", false);
                lan::Sense::instance().allow(id, g);
                return {200, lan_json()};
            });
            return json_response(r.body, r.status);
        });

    // 对面来问：你给了我什么、我的票是哪一张。
    //
    // ⚠️ **这一条只认 id**，而 id 不是秘密（它写在 mDNS 的 TXT 里）。
    // 也就是说**同一个网段里冒充 id 的人也要得到票**——挡不住。它挡住的是
    // 网段外面：票不在 mDNS 上，出了这个网段就问不到。真要挡住同网段，
    // 得靠"两边对着念一串码"那种办法，不在这一轮里。
    //
    // 没给过权限的一律回 403，**而且不说"你没被允许"以外的任何东西**
    // ——回一句"这台机器上有哪些模型"之类的，等于把没授权的人也招待了。
    CROW_ROUTE(app, "/api/lan/ticket")([](const crow::request& req) {
        auto r = guard([&]() -> ApiResult {
            const std::string who = req.get_header_value("X-Changji-Lan");
            if (who.empty()) throw ApiError(422, SAY("要 X-Changji-Lan 那个头"));
            const auto g = lan::Sense::instance().grant_of(who);
            if (!g.use || !lan::Sense::instance().on()) {
                throw ApiError(403, SAY("这台机器还没给你用。让机主在设置里点一下。"));
            }
            return {200, {{"ticket", g.ticket},
                          {"use", g.use},
                          {"params", g.params}}};
        });
        return json_response(r.body, r.status);
    });

    // 「拿它来算」：把一台给过我权限的机器写进机器表。
    //
    // ⚠️ **票在引擎这头取，不让界面经手。** 界面只说"这一台"，
    // 票从来不发给它——那是个凭据，少露一处是一处。
    //
    // 写进去之后**就是一台普通的机器**：派活、开关某一项能力、去掉，
    // 全走现成那几条路。票当口令使（那头的门认 `Bearer <票>`）。
    CROW_ROUTE(app, "/api/lan/use")
        .methods("POST"_method)([peer_nodes_json](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                const json body = parse_body(req.body);
                const std::string id = body.value("id", std::string());
                if (id.empty()) throw ApiError(422, SAY("要 id"));
                const auto [url, ticket] = lan::Sense::instance().their_door(id);
                if (url.empty() || ticket.empty()) {
                    throw ApiError(409,
                                   SAY("那台还没给你用，或者这会儿不在线。"
                                       "让对面在设置里点一下「给它用这台」。"));
                }
                auto s = config::runtime().snapshot();
                for (const auto& n : s.peer.nodes) {
                    if (n.url == url) throw ApiError(409, SAY("这台已经在表上了：") + url);
                }
                auto arr = peer_nodes_json(s);
                arr.push_back({{"url", url},
                               {"token", ticket},
                               {"off", nlohmann::json::array()}});
                try {
                    config::save_peer_nodes(arr);
                } catch (const std::exception& e) {
                    throw ApiError(500, e.what());
                }
                // ⚠️ **写完要让这个进程也跟着变**（同 `/api/nodes/add`）。
                // 不重读的话配置文件里是新的、这个进程手里还是旧的那一份
                // ——表上看不见它，真派活时也用不上它。2026-09-21 实撞：
                // config.toml 里那一行明明在，`/api/nodes` 里没有。
                s = config::load_settings();
                config::runtime().replace(s);
                infer::node_registry().refresh(s);
                return {200, infer::nodes_json(s)};
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/autostart")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST)(
            [opts](const crow::request& req) {
                auto r = guard([&]() -> ApiResult {
                    if (req.method == crow::HTTPMethod::GET) {
                        const auto st = setup::autostart_status(opts.port);
                        return {200,
                                {{"supported", st.supported},
                                 {"enabled", st.enabled},
                                 {"path", st.path},
                                 {"command", st.command}}};
                    }
                    const json body = parse_body(req.body);
                    if (!body.is_object() || !body.contains("enabled") ||
                        !body.at("enabled").is_boolean()) {
                        throw ApiError(400, SAY("要给 enabled（true / false）"));
                    }
                    const auto res = setup::set_autostart(
                        body.at("enabled").get<bool>(), opts.port);
                    // **改不成要当场说，不要回一个"成了"再让人自己发现。**
                    if (!res.error.empty()) throw ApiError(500, res.error);
                    return {200,
                            {{"supported", res.state.supported},
                             {"enabled", res.state.enabled},
                             {"path", res.state.path},
                             {"command", res.state.command}}};
                });
                return json_response(r.body, r.status);
            });

    CROW_ROUTE(app, "/api/settings")([] {
        auto r = guard([] {
            return get_settings(config::runtime().snapshot(),
                                config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    // 挑一个放模型的文件夹。**只读、只回目录**，见 get_dirs。
    CROW_ROUTE(app, "/api/fs/dirs")([](const crow::request& req) {
        auto r = guard([&] {
            return get_dirs(query(req, "path"),
                            config::runtime().snapshot());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/projects")([] {
        auto r = guard([] {
            return get_projects(config::runtime().snapshot());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/project")([](const crow::request& req) {
        auto r = guard([&] { return get_project(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots")([](const crow::request& req) {
        auto r = guard([&] {
            return get_shots(required_query(req, "path"),
                             required_query(req, "episode_id"));
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/assets")([](const crow::request& req) {
        auto r = guard([&] { return get_assets(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    // ---- /api/media：带 Range 的文件服务 ----
    //
    // Range 是必须的，不是锦上添花：不实现的话前端 <video> 标签
    // 拖不动进度条，只能从头播。方案第三节点了名。

    CROW_ROUTE(app, "/api/media")([](const crow::request& req) {
        // 这条路不走 guard（它要回文件内容不是 JSON），所以必填参数
        // 的 422 要自己接住。
        const char* path_p = req.url_params.get("path");
        const char* rel_p = req.url_params.get("rel");
        if (path_p == nullptr || rel_p == nullptr) {
            const auto e =
                unprocessable_query(path_p == nullptr ? "path" : "rel");
            return json_response(e.detail(), e.status());
        }
        const auto t = resolve_media(path_p, rel_p);
        if (t.status != 200) {
            return json_response({{"detail", t.detail}}, t.status);
        }

        std::error_code ec;
        const auto size = static_cast<std::uint64_t>(fs::file_size(t.path, ec));
        if (ec) return json_response({{"detail", SAY("读不到文件大小")}}, 500);

        std::ifstream in(t.path, std::ios::binary);
        if (!in) return json_response({{"detail", SAY("打不开文件")}}, 500);

        const std::string ctype = content_type_for(t.path);
        const std::string range_header = req.get_header_value("Range");

        // 整个文件发出去。**不读进内存**，交给 crow 那条分块写的路
        // （`do_write_static`，16 KB 一块，占用是常数）。
        //
        // 原来这儿是 `istreambuf_iterator` 一口气读成一个 std::string：
        // 一章的成片就是一百多兆，而 2026-09-21 起桌面端会走这条路去放
        // **整部电影**——三十章接起来一两个 G，一次请求就是一两个 G 的
        // 内存，而且 crow 还要再拷一份进响应。
        //
        // ⚠️ 用 `_unsafe` 那个版本：带清洗的那个会把绝对路径搅坏，而越界
        // 检查我们自己在 `resolve_media` 里做过了（那才是真正该防的那一
        // 层）。万一 stat 失败它会把 path 清空、code 设 404——照实回一句
        // JSON，别让人收到一个空的 404。
        const auto send_whole = [&](void) {
            crow::response res;
            res.set_static_file_info_unsafe(paths::to_utf8(t.path));
            // `file_info` 是私有的，所以照它设的 `code` 判：stat 失败时它
            // 把 code 设成 404 并清空路径。我们前面已经拿到过 file_size 了，
            // 走到这儿还 404 说明文件刚刚没了。
            if (res.code != 200) {
                return json_response({{"detail", SAY("打不开文件")}}, 500);
            }
            // 我们自己那份 Content-Type 更准（crow 按扩展名猜的那张表没有
            // 我们要的几种），放在后面覆盖它。
            res.set_header("Content-Type", ctype);
            res.set_header("Accept-Ranges", "bytes");
            return res;
        };

        if (range_header.empty()) {
            // 没有 Accept-Ranges 这个头，浏览器不知道服务端支持分段，
            // 于是根本不会发 Range 请求，进度条照样拖不动（在 send_whole 里）。
            return send_whole();
        }

        const auto r = parse_range(range_header, size);

        // **语法不认识和起点越界不是一回事。** 原来两种都回 416，
        // 对拍比出来 Python 那边前者回 400。对浏览器来说含义不同：
        // 416 带着文件真实长度，是"照这个重来"；400 是"你的请求是坏的"。
        if (r.verdict == RangeVerdict::Malformed) {
            crow::response res(400, SAY("Range 头看不懂"));
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            return res;
        }
        if (r.verdict == RangeVerdict::NotSatisfiable) {
            // 416 必须带 Content-Range 告诉对方真实长度，而且 **body 是空的**
            // ——Python 那边 Content-Length 是 0，写点什么进去就对不上了。
            crow::response res(416, "");
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.set_header("Content-Range", "bytes */" + std::to_string(size));
            return res;
        }
        if (r.verdict == RangeVerdict::Ignore) {
            // 多区间。见 media.hpp 里那段：当没看见这个头，回整个文件。
            return send_whole();
        }

        const std::uint64_t len = r.range.last - r.range.first + 1;
        std::string chunk(static_cast<std::size_t>(len), '\0');
        in.seekg(static_cast<std::streamoff>(r.range.first));
        in.read(chunk.data(), static_cast<std::streamsize>(len));
        chunk.resize(static_cast<std::size_t>(in.gcount()));

        crow::response res(206, std::move(chunk));
        res.set_header("Content-Type", ctype);
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Content-Range",
                       "bytes " + std::to_string(r.range.first) + "-" +
                           std::to_string(r.range.last) + "/" +
                           std::to_string(size));
        return res;
    });

    // ---- 阶段 3：编辑接口 ----

    CROW_ROUTE(app, "/api/shot").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shot(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/character").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_character(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/location").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_location(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/style").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_style(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/batch").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_batch(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/reorder").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_reorder(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/link_locations").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_link_locations(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    // ---- 参考图上传（multipart）----
    //
    // multipart 的解析是 crow 的事，留在这一层；校验和落盘在 upload.cpp
    // 的纯函数里，那样不起服务也能测。

    CROW_ROUTE(app, "/api/character/reference").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                crow::multipart::message msg(req);
                const auto field = [&](const char* name) -> std::string {
                    auto it = msg.part_map.find(name);
                    return it == msg.part_map.end() ? std::string() : it->second.body;
                };
                auto fit = msg.part_map.find("file");
                if (fit == msg.part_map.end()) throw ApiError(400, SAY("没有上传文件"));
                // 文件那一部分的 Content-Type 在它自己的头里，不是请求头里
                const std::string ctype =
                    fit->second.get_header_object("Content-Type").value;
                return post_character_reference(field("project"), field("char_id"),
                                                field("slot"), ctype,
                                                fit->second.body);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/location/reference").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                crow::multipart::message msg(req);
                const auto field = [&](const char* name) -> std::string {
                    auto it = msg.part_map.find(name);
                    return it == msg.part_map.end() ? std::string() : it->second.body;
                };
                auto fit = msg.part_map.find("file");
                if (fit == msg.part_map.end()) throw ApiError(400, SAY("没有上传文件"));
                const std::string ctype =
                    fit->second.get_header_object("Content-Type").value;
                return post_location_reference(field("project"), field("location_id"),
                                               ctype, fit->second.body);
            });
            return json_response(r.body, r.status);
        });

    // 参考音色：一段人声片段，进程内配音照着它的音色念。
    // 形状照抄上面的参考图那条——multipart 解析留在路由层。
    CROW_ROUTE(app, "/api/character/voice").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                crow::multipart::message msg(req);
                const auto field = [&](const char* name) -> std::string {
                    auto it = msg.part_map.find(name);
                    return it == msg.part_map.end() ? std::string() : it->second.body;
                };
                auto fit = msg.part_map.find("file");
                if (fit == msg.part_map.end()) throw ApiError(400, SAY("没有上传文件"));
                const std::string ctype =
                    fit->second.get_header_object("Content-Type").value;
                return post_character_voice(field("project"), field("char_id"),
                                            ctype, fit->second.body);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/character/voice/clear").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_character_voice_clear(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/character/reference/clear").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_character_reference_clear(
                    parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/location/reference/clear").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_location_reference_clear(
                    parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // 念一段字出来。编辑器右下角那个「朗读」。
    CROW_ROUTE(app, "/api/tts/say").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                json body = parse_body(req.body);
                const bool want_async = take_async(body);
                const std::string stream_id = stream_of(body);
                if (want_async && !stream_id.empty()) {
                    return start_async(stream_id,
                                       [body] { return post_tts_say(body); });
                }
                return post_tts_say(body);
            });
            return json_response(r.body, r.status);
        });

    // 参考图的另一条来路：照着设定里那段外观描述现画一张。
    //
    // **同步，一次一张，几十秒。** 头一张还要先把出图模型读进显存。
    // 没走任务表的理由见 ref_gen.hpp。
    CROW_ROUTE(app, "/api/character/reference/generate").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_character_reference_generate(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/location/reference/generate").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_location_reference_generate(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // ---- 剧本 ----
    //
    // 三个都是同步的：调一次大模型，几十秒内返回。写全片不一样，
    // 那个要跑几分钟，走下面的 job 表。
    //
    // 客户端在这里造一次，三个路由共用。**存进 static 不会把配置冻住**：
    // 地址、模型、密钥走 ConfigProvider 每次现读，"走哪条后端"走
    // `llm::make_client` 里那层分派、也是每次现读。理由写在那儿，别在这儿
    // 再写一遍。
    static std::shared_ptr<llm::Client> script_client =
        llm::make_client(llm::default_http_post(),
                         llm::default_http_post_stream());

    // 这一族全是要大模型的：出梗概、写剧本、出大纲、读故事、写一章、
    // 改一段稿。**一件一两分钟**，所以带上 async + stream 时挪到后台干，
    // 接口当场回 202——见上面 start_async 那段。
    const auto script_route = [](auto handler) {
        return [handler](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                json body = parse_body(req.body);
                const bool want_async = take_async(body);
                const std::string stream_id = stream_of(body);
                if (want_async && !stream_id.empty()) {
                    return start_async(stream_id, [handler, body] {
                        // **令牌从 JobScope 拿**（start_async 里挂的那个）。
                        // 原来这儿是就地 new 一个，谁也够不着它——那句
                        // "取消令牌是个不会被触发的哑元，等接进 job 表之后
                        // 换成真的"就是说这件事。现在它按 stream_id 登记着，
                        // 界面上那个「停下」按的就是它。
                        return handler(body, *script_client, current_cancel());
                    });
                }
                // 这几个接口没有自己的 job，取消令牌是个不会被触发的哑元。
                // 等它们接进 job 表之后换成真的那个。
                static thread_local pipeline::CancelToken tok;
                tok.reset();
                return handler(body, *script_client, tok);
            });
            return json_response(r.body, r.status);
        };
    };

    // 「一键出图」：**一次交一整批，队列在引擎这头**（见 ref_gen.hpp）。
    // 同一条路两个动作：POST 交一批，GET 问这批跑到哪儿了。
    // **一条 rule 只能注册一次**，两条 CROW_ROUTE 写同一个路径会在起服务时
    // 撞车，所以合在一起按方法分。
    CROW_ROUTE(app, "/api/assets/references")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST)(
            [](const crow::request& req) {
                auto r = guard([&] {
                    if (req.method == crow::HTTPMethod::GET) {
                        const char* p = req.url_params.get("project");
                        return get_references_queue(p ? p : "");
                    }
                    return post_references_generate_all(parse_body(req.body));
                });
                return json_response(r.body, r.status);
            });
    CROW_ROUTE(app, "/api/assets/references/stop").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_references_generate_all_stop(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/script/premise").methods("POST"_method)(
        script_route(&post_script_premise));
    CROW_ROUTE(app, "/api/script/write").methods("POST"_method)(
        script_route(&post_script_write));
    CROW_ROUTE(app, "/api/script/trailer").methods("POST"_method)(
        script_route(&post_script_trailer));

    // 出角色圣经和分镜表。走同一套包装。
    CROW_ROUTE(app, "/api/assets/dedupe").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_assets_dedupe(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });
    CROW_ROUTE(app, "/api/bible").methods("POST"_method)(
        script_route(&post_bible));
    CROW_ROUTE(app, "/api/plan").methods("POST"_method)(
        script_route(&post_plan));

    // ---- 故事层 ----
    //
    // 整条流水线的新源头：先有完整故事，人物关系和场景从故事里提，
    // 再照大纲的章一章一条排出章节计划（每章时长只在这一章还没正文时
    // 当回落，见 stages/story_plan.cpp 的 plan_episodes）。
    // 见 docs/故事优先重构方案.md。
    //
    // 写和采用分成两个接口，照的是剧本那边已经立住的规矩：源头没人审过
    // 就往下跑，后面几十分钟的渲染全是白跑。/outline 只回草稿不落库。

    CROW_ROUTE(app, "/api/story")([](const crow::request& req) {
        auto r = guard([&] { return get_story(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/story").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_story(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/story/outline").methods("POST"_method)(
        script_route(&post_story_outline));

    CROW_ROUTE(app, "/api/story/adopt").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_story_adopt(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/story/chapter/delete").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard(
                [&] { return post_story_chapter_delete(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/story/analyze").methods("POST"_method)(
        script_route(&post_story_analyze));

    CROW_ROUTE(app, "/api/story/chapter").methods("POST"_method)(
        script_route(&post_story_chapter));

    CROW_ROUTE(app, "/api/story/import").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_story_import(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/story/episodes").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_story_episodes(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    // 改原稿的某一段。先出草稿（要大模型），人点了才落（不要大模型）。
    CROW_ROUTE(app, "/api/story/revise").methods("POST"_method)(
        script_route(&post_story_revise));

    CROW_ROUTE(app, "/api/story/revise/apply").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_story_revise_apply(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // 老项目接回新流程的那一步。不碰大模型，也不重新排章节计划。
    CROW_ROUTE(app, "/api/story/from_episodes").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_story_from_episodes(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/story/plan").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_story_plan(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    // ---- 连接设置与运行参数 ----
    //
    // 这三个是阶段 1 漏掉的，路由表核对时找出来的——前端那次探测只发
    // GET 请求，POST-only 的接口整片看不见。

    CROW_ROUTE(app, "/api/connections")([] {
        return json_response(get_connections().body);
    });

    CROW_ROUTE(app, "/api/connections").methods("POST"_method)(
        [](const crow::request& req) {
            static const auto check = default_doctor();
            auto r = guard([&] {
                return post_connections(parse_body(req.body),
                                        check);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/settings").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_settings(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // ---- 大模型接入信息 ----
    //
    // 方案的破契约白名单里原本有这两条，理由是"进程内推理之后语义重定义"。
    // 决策 4 之后那个理由不成立了：用远端大模型是长期形态之一，
    // 不是过渡状态。所以 providers 原样保留，models 是**扩展**不是替换——
    // 远端有哪些照旧列，另加一个字段列本机的 gguf。

    CROW_ROUTE(app, "/api/llm/providers")([] {
        return json_response(get_llm_providers().body);
    });

    // GET = 问配置里存着的那一家；POST = 问 body 里指定的那一家。
    // **换平台那一下要用 POST**：那会儿配置里还是旧地址（见 llm_info.hpp）。
    // 密钥只走请求体，不进查询串——查询串会落进访问日志和浏览器历史。
    CROW_ROUTE(app, "/api/llm/models")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::POST)(
            [](const crow::request& req) {
                static const auto fetch = default_http_get();
                auto r = guard([&] {
                    const auto s = config::runtime().snapshot();
                    if (req.method == crow::HTTPMethod::GET) {
                        return get_llm_models(s, fetch);
                    }
                    return post_llm_models(parse_body(req.body), s, fetch);
                });
                return json_response(r.body, r.status);
            });

    // GET /api/llm/thinking?model=&base_url= —— 这个模型「想多久」能挑哪几档。
    //
    // **界面上那颗牌子靠它变灰。** 查的是 `llm/thinking.hpp` 那一张表，
    // 而拼请求的那两处查的是同一张——所以界面上能选的一定发得出去，
    // 界面上灰的一定发出去也没用（理由写在那个头文件上）。
    //
    // 不填就问配置里存着的那一家。**不走网络**：表是死的，一次查表而已。
    CROW_ROUTE(app, "/api/llm/thinking")
        .methods(crow::HTTPMethod::GET)([](const crow::request& req) {
            auto r = guard([&] {
                const auto s = config::runtime().snapshot();
                const char* m = req.url_params.get("model");
                const char* b = req.url_params.get("base_url");
                const std::string model = m ? std::string(m) : s.llm.model;
                const std::string base = b ? std::string(b) : s.llm.base_url;
                const auto sup = llm::thinking_support(base, model);
                nlohmann::json tiers = nlohmann::json::array();
                for (const char* k : llm::kThinkTiers) {
                    tiers.push_back(
                        {{"k", k},
                         {"able", std::find(sup.tiers.begin(), sup.tiers.end(),
                                            std::string(k)) != sup.tiers.end()}});
                }
                return ApiResult{200, {{"model", model},
                                       {"tiers", tiers},
                                       {"why", sup.why}}};
            });
            return json_response(r.body, r.status);
        });

    // ---- 根路径 ----
    //
    // **这是白名单里唯一真正的破契约。** Python 那边 GET / 返回
    // page.py 生成的一整页 HTML（那是删 Python 之前的内置界面）。
    // 两层架构下界面由 Node 提供，浏览器根本不会访问到这里——
    // 会撞上它的只有直接开了后端端口的人。给他们一句指路的话，
    // 比返回 404 或者一个空页面有用。
    // ---- Node 那个 BFF 的几条，引擎自己也答一份 ----
    //
    // **为什么引擎要管这个。** 引擎自己发前端之后，走这条路的人拿不到
    // `/bff/*`——界面能打开、项目列表也在，但**章节下拉框是空的**、
    // 顶栏写着"引擎连不上"，「这一章」那一页点不动。那等于"打开端口就能看处理进度"
    // 没做完。
    //
    // 只搬**判定和状态**这几条。投递（`/bff/publish/*`）没搬：它要存投递
    // 记录、要平台配置，是另一套东西，不是顺手能带的。要投递就仍然起 Node。

    CROW_ROUTE(app, "/bff/health")([] {
        return json_response({{"ok", true}, {"service", "changji"}});
    });

    // 前端拿它点亮顶栏那个"引擎连不上/已连接"。
    // **由引擎自己答的时候它恒为在线**——答得出这个请求就说明活着，
    // 再去 ping 自己一次没有意义。
    CROW_ROUTE(app, "/bff/settings/status")([] {
        return json_response({{"online", true},
                              {"baseUrl", ""},
                              {"latencyMs", 0},
                              {"service", "changji"}});
    });

    CROW_ROUTE(app, "/bff/settings/config")([] {
        const auto s = config::runtime().snapshot();
        json locked = json::object();
        for (const auto& [k, v] : config::env_overridden()) locked[k] = v;
        // **embedded 告诉前端"引擎就是我自己"。**
        // 由引擎自己答这个请求时，"引擎地址"和"请求超时"两个输入框是空的、
        // 也没有意义（改了也没人读），显示出来只会让人以为哪里没配好。
        // 起 Node 那层转发时答的是另一份，带真地址，那时才该显示。
        return json_response(
            {{"embedded", true},
             {"engineBaseUrl", ""},
             {"engineTimeoutMs", 0},
             {"configFile", changji::paths::to_utf8(config::user_config_path())},
             {"envLocked", locked}});
    });

    // **设置页真正用的是这一条**，不是上面那两条。
    //
    // 漏了它的后果很难看懂：顶栏读 /bff/settings/status 显示"引擎已连接"，
    // 而设置页读这一条拿到 404，于是整页当引擎离线处理——地址空、
    // 配置文件空、右上角写"连不上"。**同一个页面上两个相反的结论。**
    //
    // 形状照抄 Node 那份（webapp/server/src/routes/settings.js）：它要去
    // 发四次 HTTP，我们在进程内直接取。errors 留空对象——那几项是它
    // 转发失败时填的，我们没有转发这一层。
    CROW_ROUTE(app, "/bff/settings/overview")([](const crow::request& req) {
        json locked = json::object();
        for (const auto& [k, v] : config::env_overridden()) locked[k] = v;
        const auto s = config::runtime().snapshot();

        json out;
        out["node"] = {
            {"embedded", true},
            {"engineBaseUrl", ""},
            {"engineTimeoutMs", 0},
            {"configFile", changji::paths::to_utf8(config::user_config_path())},
            // **大模型现在到底装着没有。** 进程内那条 2026-09-19 接回来了，
            // 这两项跟着回来：设置页那句"出片要显存时它会自动让开"是句
            // 空话，除非能看到让没让开。量到多少一并给出来：没量过是 null。
            // 走远端时恒为 false / null——本机显存上根本没有它。
            {"llmLoaded", infer::scheduler().loaded(infer::Slot::LLM)},
            {"llmMeasuredVramGb",
             [] {
                 const std::size_t b =
                     infer::scheduler().measured_vram(infer::Slot::LLM);
                 return b == 0 ? json(nullptr)
                               : json(static_cast<double>(b) /
                                      (1024.0 * 1024 * 1024));
             }()},
            // **后端名也不在这儿发。** 这儿原来还有 llmBackend / ttsBackend
            // 两条，注释写着"前端要拿它决定哪些输入框该显示——backend =
            // local 时 API 地址/模型名/密钥三项一个都不读"。那件事是真的
            // 该做，但**从来不是这两条在做**：前端一处都没读过它们。
            // 真正在管的是同一个回包里的 `connections.tts_backend`
            // （snake_case，来自 get_connections），设置页那句
            // `v-if="conn.tts_backend === 'http'"` 用的就是它。
            // llmBackend 则是从头就不可能有用——settings.cpp 只认
            // `llm.backend = remote`，别的值当场报错，没有第二种情况可分。
            {"envLocked", locked}};
        // 由引擎自己答就说明它活着，再 ping 自己一次没有意义。
        out["engine"] = {{"online", true},
                         {"baseUrl", ""},
                         {"latencyMs", 0},
                         {"service", "changji"}};
        out["connections"] = get_connections().body;
        out["settings"] = get_settings(s).body;
        out["hardware"] =
            get_hardware(s, config::runtime().profile()).body;
        // **这一轮真正会用的规格。C++ 独有，所以放在 /bff。**
        //
        // `hardware.tiers.final` 是档位表按显存推出来的，出片时会被两件事
        // 盖掉：画幅来自项目的 [video]，步数在挂了 Turbo 时压到 6。
        // 设置页照着档位表显示的话，写的是"成片步数 28"而实际跑 6 步——
        // 用户看了会问"怎么没用 turbo"。真发生过。
        //
        // 和 run.cpp 调的是同一个函数，两边不会分叉。
        {
            const auto& prof = config::runtime().profile();
            int table = 0;
            if (const auto it = prof.tiers.find(models::Tier::FINAL);
                it != prof.tiers.end()) {
                table = it->second.steps;
            }
            const auto eff = config::effective_spec(s, table);
            out["effective"] = {{"width", eff.width},
                                {"height", eff.height},
                                {"finalSteps", eff.final_steps},
                                {"frameSteps", eff.frame_steps},
                                {"turbo", eff.turbo},
                                {"stepsPinned", eff.steps_pinned},
                                {"tableSteps", table}};
            // **显卡真有多少显存，和配置里顶着的那个数分开给。**
            // /api/hardware 的 vram_gb 在有 vram_gb_override 时回的是 override
            // （Python 就这样，对拍不能动），于是设置页写着 "5090 · 12 GB"——
            // 用户说"硬件 GPU 显存有获取不准的 bug"。探到的数单独回，
            // 顶着的那个也回，界面把两件事说清楚。
            out["effective"]["physicalVramGb"] =
                prof.gpu.has_value() ? json(prof.gpu->vram_gb()) : json(nullptr);
            out["effective"]["vramOverride"] =
                s.vram_gb_override.has_value() ? json(*s.vram_gb_override) : json(nullptr);
            // **程序给两个模型算出来的权重放置，显示给用户看。**
            //
            // weights 已经不让人在界面上填了（用户："都应该让程序自己算"），
            // 但算完了不给看是另一个极端：出图慢到底是卡不行还是权重在内存里
            // 每步搬一趟，用户没有任何线索。2026-09-10 排查 GPU 利用率只有
            // 18% 那次，答案就是这一项——当时得连上机器看日志才知道。
            //
            // 和出图出片建上下文走的是同一个 expand_placement，不会分叉。
            {
                const double card_gb = prof.gpu.has_value() ? prof.gpu->vram_gb()
                                                            : prof.vram_gb;
                // unified 也要传：界面显示的必须和引擎真正用的是同一个
                // 展开结果，漏了这个参数就又分叉了（而这一处存在的理由
                // 正是"不分叉"）。
                const auto ex = config::expand_placement(
                    s, card_gb, prof.gpu.has_value() && prof.gpu->unified());
                const auto pack = [](const config::PlacementInfo& p) {
                    return json{{"weights", p.weights},
                                {"modelGb", p.model_gb},
                                {"liveVramGb", p.live_vram_gb},
                                {"resident", p.resident}};
                };
                // **实测占用也给出来。** 估算和真实差得离谱（video 那一路
                // 算 14.6 GB、实测 74 GB），界面上只显示估算会误导人。
                // 量到之前是 null——那时候调度器走的也正是保守估算。
                const auto all = infer::scheduler().all_measured();
                const auto measured = [&all](infer::Slot sl) {
                    const auto it = all.find(sl);
                    return (it == all.end() || it->second.bytes == 0)
                               ? json(nullptr)
                               : json(static_cast<double>(it->second.bytes) /
                                      (1024.0 * 1024 * 1024));
                };
                // **量的是多大的活也要给出来。** 光给一个"实测 74.6 GB"
                // 是没法判断的：那个数只在"活不比当时大"时才算数，
                // 而画幅档位差七倍多。见 Scheduler::record_measured_vram。
                const auto measured_work = [&all](infer::Slot sl) {
                    const auto it = all.find(sl);
                    return (it == all.end() || it->second.work == 0)
                               ? json(nullptr)
                               : json(static_cast<double>(it->second.work));
                };
                json vid = pack(config::video_placement(ex));
                json img = pack(config::image_placement(ex));
                vid["measuredVramGb"] = measured(infer::Slot::Video);
                img["measuredVramGb"] = measured(infer::Slot::Image);
                vid["measuredWork"] = measured_work(infer::Slot::Video);
                img["measuredWork"] = measured_work(infer::Slot::Image);
                // **最近一次"要不要腾地方"的判断，原样露出来。**
                // 这个判断错了的表现是"该留的时候卸了"（慢）或者"该卸的
                // 时候没卸"（CUDA OOM 把整个服务带走），而以前只能登上
                // 机器看 stderr。2026-09-11 服务器连不上那几个钟头，
                // 这条线索就彻底断了。
                const auto d = infer::scheduler().last_room_decision();
                json dec = json(nullptr);
                if (d.valid) {
                    const auto gb = [](std::size_t b) {
                        return static_cast<double>(b) / (1024.0 * 1024 * 1024);
                    };
                    dec = json{{"slot", infer::to_string(d.slot)},
                               {"needGb", gb(d.need)},
                               {"liveGb", gb(d.live)},
                               {"freeSeenGb", d.free_seen ? json(gb(d.free_seen))
                                                          : json(nullptr)},
                               {"probed", d.probed},
                               // **"够"是拿量到的数判的还是拿估的判的。**
                               // 估算在 video 这一路错得离谱（算 14.6 GB、
                               // 实测 74 GB），拿它判出来的"够，不卸"随时
                               // 可能是 CUDA OOM 的前一步。界面上要分开说。
                               {"liveMeasured", d.live_measured},
                               // 这一次压根没判（模型本来就装着、画幅也
                               // 没超）。不标出来的话，界面会把"当时空闲
                               // 是 0、不是问来的"显示成**问不到卡**，
                               // 而那是让用户盯着报警的那一项。
                               {"alreadyLoaded", d.already_loaded},
                               {"kept", d.kept},
                               {"evicted", d.evicted}};
                }
                out["effective"]["placement"] = {{"video", vid},
                                                 {"image", img},
                                                 {"cardGb", card_gb},
                                                 {"lastRoomDecision", dec}};
            }
        }
        // 进程内大模型能同时跑几路。
        //
        // **配的和实际开出来的要分开摆。** [llm].parallel 是上限，真开几个
        // 由显存说了算（第一个开不出来才算失败，后面的开不出来只是并发度低
        // 一档）。只显示配置的话，用户配了 4 会以为就是 4 路，而实际可能
        // 只有 1 路——那时候「同时编两个项目会互相等」就成了没法解释的怪事。
        {
            const auto st = llm::local_llm_status();
            out["effective"]["llm"] = {{"backend", s.llm.backend},
                                       {"parallelWanted", s.llm.parallel},
                                       {"loaded", st.loaded},
                                       {"slots", st.slots},
                                       {"contextTokens", st.context_tokens},
                                       {"thinking", s.llm.thinking},
                                       {"supportsThinking", st.supports_thinking}};
        }
        // **体检要发网络请求，最坏二十多秒。** Node 那份也是同步等的，
        // 形状要一致就只能照做；Crow 是线程池，占住一个工作线程不影响别的请求。
        //
        // ⚠️ **这里也要带项目路径。** 原来是 `run_checks(s)`——全局那份配置。
        // 于是同一条「出片画布」在两处给出两个答案：镜头页那个开跑前的体检
        // 走 `/api/doctor?path=…`（查的是这部电影的 [video]，能说出"2K 超了
        // 模型上限"），而设置页这一节查的是全局默认的 544×928，永远说没问题
        // ——**而设置页正是产品把体检摆在第一节的那一页**，上面还挂着一颗
        // 「可以开工 / 还不能跑」的牌子。一个照着全局配置发的"可以开工"，
        // 在当前这部电影上可能根本跑不出东西来。
        //
        // 前端把顶栏选中的那部电影的路径带上来（api.settingsOverview(project)）。
        // 没带就还是全局那份，和以前一样。
        out["doctor"] = doctor::to_json(doctor_for(req.url_params.get("path")));
        out["errors"] = json::object();
        return json_response(out);
    });

    // ---- 大模型跑在哪 ----
    //
    // 走 /bff 不走 /api/connections：那个接口在对拍覆盖范围内，
    // Python 没有 llm.backend 这个字段。
    //
    // **这儿只剩一条路了。** 原来这段写着「两条都要留着，默认内置」——那是
    // 进程内那条还在的时候；它 2026-09-14 删了（理由见生成的配置模板里
    // [llm] 那段），下面的校验也只放 remote 过。所以这条路线现在的全部用处
    // 是**把老机器上那份 backend = "local" 的配置改回来**：体检那条警告让人
    // 去改配置文件，改不动的（装在别人机器上、没有文件访问）还能 POST 一下。
    // 界面上没有对应的控件，也不该再加——只有一个值的开关是摆设。
    CROW_ROUTE(app, "/bff/settings/llm")
        .methods("POST"_method)([](const crow::request& req) {
            auto r = guard([&] {
                const auto body = parse_body(req.body);
                const auto it = body.find("backend");
                if (it == body.end() || !it->is_string()) {
                    throw ApiError(400, SAY("缺 backend"));
                }
                const auto backend = it->get<std::string>();
                // **进程内那条 2026-09-14 删了，只剩 remote。**
                // 报错要说清是"这条路没有了"，不是"你写错了"——
                // 只说"只能是 remote"的话，用过旧版的人会去翻配置找哪儿错了。
                if (backend != "local" && backend != "remote") {
                    throw ApiError(400, SAY("backend 只能是 local 或 remote"));
                }
                if (backend == "local" && !infer::llama_chat_available()) {
                    // **说清是构建选项，不是配置写错了。** 只说"不支持"
                    // 的话用户会去翻配置文件找哪里填错了。
                    throw ApiError(
                        400,
                        SAY("这个二进制没编进程内大模型（构建时 "
                            "CHANGJI_LLAMA=OFF）。用外接：backend = remote "
                            "并填 [llm].base_url"));
                }
                config::save_user_config(json{{"llm", {{"backend", backend}}}});
                auto s = config::runtime().snapshot();
                s.llm.backend = backend;
                config::runtime().replace(s);
                // 切到进程内：槽要当场挂上调度器，不能等重启——起服务时配的
                // 是远端的话那一步什么都没注册，这会儿借槽会撞"没注册"。
                // 注册过就不会再注册（register_llm_slot 自己记着）。
                llm::register_llm_slot([] { return config::runtime().snapshot(); },
                                       config::runtime().profile());

                // **改成外接就把显存里那份放掉。**
                //
                // 用户切这一项，多半就是为了腾显存——小卡上撞到"显存不够"
                // 时，体检和报错都在劝他走这条路。可光改配置的话，那十几
                // GB 还原封不动占着：要等到下一次出片腾地方才顺带被卸掉，
                // 而设置页此刻还写着"现在：装着（实测占 15.4 GB）"。
                // 他会以为这一项没生效。
                //
                // 正在写剧本时 evict 返回 false，不强卸——那会让正在跑的
                // 那次生成段错误。**这一次先留着**，反正它已经不会再被借，
                // 下一次腾地方时就走了。
                //
                // 先看它装着没有再动手：evict 的语义是"事后不装着就算成功"
                // ——槽压根没装也返回 true。直接拿它当回答的话，这个字段
                // 在"本来就没装"的时候会说成"卸掉了"。
                bool unloaded = false;
                if (backend == "remote" &&
                    infer::scheduler().loaded(infer::Slot::LLM)) {
                    unloaded = infer::scheduler().evict(infer::Slot::LLM);
                }
                return ApiResult{200,
                                 {{"backend", backend}, {"unloaded", unloaded}}};
            });
            return json_response(r.body, r.status);
        });

    // ---- 这部电影的画面规格 ----
    //
    // 竖屏还是横屏、720p 还是 2K。**一部电影一份**，写在项目目录的
    // changji.toml 里——一台机器上可以同时有一部横屏的正片和一批竖版的
    // 物料（竖屏预告、花絮）。
    //
    // 走 /bff 不走 /api：这两项是 C++ 独有的，而 /api/project 那份 JSON
    // 在对拍覆盖范围内，Python 没有它们。
    // ---- 这一轮还没落定的镜头 ----
    //
    // **C++ 独有，所以在 /bff 不在 /api。** 镜头墙上的「排队中」原来只存在
    // 浏览器内存里：刷新一下、换个标签页、换台设备，排着的全没了，正在跑的
    // 那一镜也要等到下一条进度才亮。引擎自己一直知道这一轮还有哪几镜没跑完
    // （每个阶段开工登记一批，每落定一镜划掉一个）——页面一进来问它就是了。
    // **带上 `path` 问。** 镜头号在各部电影之间是重的（每一部都有
    // `ep01_sh001`），这份名单不分项目的话，另一部电影排着的那几镜会在
    // 这一部同名的格子上点亮「排队中」。不带 path 时照旧全回（命令行、
    // 老界面）。
    CROW_ROUTE(app, "/bff/run/pending")([](const crow::request& req) {
        const std::string want = query(req, "path");
        const bool running = pipeline::jobs().running(pipeline::JobKind::Run);
        const bool mine =
            want.empty() || !running ||
            changji::paths::same_dir(
                pipeline::jobs().running_project(pipeline::JobKind::Run), want);
        nlohmann::json ids = nlohmann::json::array();
        if (mine) ids = pipeline::jobs().pending(pipeline::JobKind::Run);
        return json_response({{"running", running && mine}, {"shot_ids", ids}});
    });

    // ---- 首次运行：把模型下下来 ----
    //
    // **C++ 独有，所以在 /bff 不在 /api。** Python 那边模型是 ComfyUI 管的，
    // 它根本不知道文件在哪，更没有"下模型"这件事。
    //
    // 装好程序之后 `[models]` 是空的，界面能打开、项目能建，点到「出片」
    // 才发现什么都跑不了——那时候用户手上只有一句"本地模型一个都没配"
    // 和一个配置文件路径。这四条接口把那段路变成"看推荐、点下载、等"。

    // `path` 给了就**带上那部电影的 changji.toml**。
    //
    // 「挑了哪一档」记在项目里（`[models.pick]`），而这条接口正是那个模型
    // 窗口读的。不带项目的话它只看全局——于是人在项目页挑完、写进了项目，
    // 再打开那个窗口看到的还是全局那一档，**看着像没保存上**。
    //
    // 不给 path 照旧只看全局：设置页那一节不属于任何一部电影。
    CROW_ROUTE(app, "/bff/setup/state")([](const crow::request& req) {
        auto r = guard([&] {
            const std::string project = query(req, "path");
            const auto s = project.empty()
                               ? config::runtime().snapshot()
                               : config::load_settings(
                                     paths::expand_user(project));
            return get_setup_state(s, config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/setup/download")
        .methods("POST"_method)([](const crow::request& req) {
            auto r = guard([&] {
                return post_setup_download(config::runtime().snapshot(),
                                           parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // 前端一秒问一次。**故意不走 WebSocket**：进度是一个可以随时重新问出来
    // 的状态，不是一串必须收全的事件。刷新页面、换台设备、下到一半关掉浏览器
    // 第二天回来——轮询这三种都对，而事件流每一种都要另写一段补偿。
    CROW_ROUTE(app, "/bff/setup/progress")([] {
        auto r = guard([&] { return get_setup_progress(); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/setup/cancel")
        .methods("POST"_method)([](const crow::request&) {
            auto r = guard([&] { return post_setup_cancel(); });
            return json_response(r.body, r.status);
        });

    // ---- 任意一台机器的装模型：本机走本地那份，别的机器转发过去 ----
    //
    // **界面只跟本机的引擎说话。** 浏览器连不上那几台（地址可能只有引擎
    // 这边通，口令也在配置里、不该发到前端去），所以引擎替它跑一趟。
    // `local` 不转发，直接调本地实现——转一圈回到自己身上是白多一次
    // HTTP，而且要求本机对自己可达（它未必）。
    //
    // 这四条和 /bff/setup/* 用的是同一份实现。**这台自己点下载，和别的
    // 机器指挥它下载，必须走同一条路**——两份的话"下完了没有"的判据
    // 迟早只改一边，而那一边会把半截文件当成下好了。
    const auto node_target = [](const crow::request& req) {
        const char* v = req.url_params.get("url");
        return std::string(v == nullptr ? "" : v);
    };
    const auto proxy_status = [](int status) {
        // 0 = 压根没连上。**翻成 502**：那是"我这一头到那一头断了"，
        // 和对面自己回的错要分得开。
        return status == 0 ? 502 : status;
    };

    // `path` 给了就带上那部电影的 `[models.pick]`，理由同 /bff/setup/state。
    //
    // **这一条尤其要带。** 界面拿本机这一份当「标准那一套」发给别的机器
    // （NodeMatrix 的「装成和本机同一套」）。不带项目的话那个标准是**这台
    // 机器全局配着的那一档**，而派活时带过去的是**这部电影挑的那一档**——
    // 两者不同的时候，给对面装的和真要用的就不是一个东西，而表现要到
    // 那一镜被对面拒了才看得出来（「这台装的是别的档」，见 task_run.cpp）。
    CROW_ROUTE(app, "/api/nodes/setup")(
        [node_target, proxy_status](const crow::request& req) {
            const std::string url = node_target(req);
            if (url.empty()) {
                return json_response({{"detail", SAY("要 url（local 或节点地址）")}},
                                     422);
            }
            const std::string project = query(req, "path");
            const auto s = project.empty()
                               ? config::runtime().snapshot()
                               : config::load_settings(
                                     paths::expand_user(project));
            if (url == infer::kLocalEndpoint) {
                auto r = guard([&] {
                    return get_setup_state(s, config::runtime().profile());
                });
                return json_response(r.body, r.status);
            }
            // 扫一遍模型目录要点时间，给宽一些
            const auto r = infer::node_get(s, url, "/setup/state", 20);
            return json_response(r.body, proxy_status(r.status));
        });

    CROW_ROUTE(app, "/api/nodes/setup/download")
        .methods("POST"_method)([proxy_status](const crow::request& req) {
            const auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                return json_response({{"detail", SAY("请求体不是一个 JSON 对象")}},
                                     400);
            }
            const std::string url = body.value("url", std::string());
            if (url.empty()) {
                return json_response({{"detail", SAY("要 url")}}, 422);
            }
            // url 是给我们自己看的，别跟着发出去
            nlohmann::json payload = body;
            payload.erase("url");

            const auto s = config::runtime().snapshot();
            if (url == infer::kLocalEndpoint) {
                auto r = guard([&] { return post_setup_download(s, payload); });
                return json_response(r.body, r.status);
            }
            const auto r =
                infer::node_post(s, url, "/setup/download", payload, 20);
            return json_response(r.body, proxy_status(r.status));
        });

    CROW_ROUTE(app, "/api/nodes/setup/progress")(
        [node_target, proxy_status](const crow::request& req) {
            const std::string url = node_target(req);
            if (url.empty()) {
                return json_response({{"detail", SAY("要 url")}}, 422);
            }
            const auto s = config::runtime().snapshot();
            if (url == infer::kLocalEndpoint) {
                auto r = guard([&] { return get_setup_progress(); });
                return json_response(r.body, r.status);
            }
            const auto r = infer::node_get(s, url, "/setup/progress", 10);
            return json_response(r.body, proxy_status(r.status));
        });

    CROW_ROUTE(app, "/api/nodes/setup/cancel")
        .methods("POST"_method)([proxy_status](const crow::request& req) {
            const auto body = nlohmann::json::parse(req.body, nullptr, false);
            const std::string url =
                body.is_object() ? body.value("url", std::string()) : "";
            if (url.empty()) {
                return json_response({{"detail", SAY("要 url")}}, 422);
            }
            const auto s = config::runtime().snapshot();
            if (url == infer::kLocalEndpoint) {
                auto r = guard([&] { return post_setup_cancel(); });
                return json_response(r.body, r.status);
            }
            const auto r = infer::node_post(s, url, "/setup/cancel",
                                            nlohmann::json::object(), 10);
            return json_response(r.body, proxy_status(r.status));
        });

    CROW_ROUTE(app, "/bff/project/video")([](const crow::request& req) {
        auto r = guard([&] {
            const auto root =
                changji::paths::from_utf8(required_query(req, "path"));
            // 老项目没有 `[video]` 时，这儿报的是 load_settings 从它自己的
            // assets.json 推出来的那一档，不是内置默认——设置页显示的、
            // 和用户按保存时发回来的，于是都是这个项目自己的画幅。
            const auto s = config::load_settings(root);
            const auto [w, h] = s.video.size();
            return ApiResult{200,
                             {{"orientation", s.video.orientation},
                              {"quality", s.video.quality},
                              // 把算出来的尺寸也回去：界面上要显示
                              // "720p 竖屏 = 704×1280"，用户才知道自己选的
                              // 到底是多大。
                              {"width", w},
                              {"height", h}}};
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/project/video")
        .methods("POST"_method)([](const crow::request& req) {
            auto r = guard([&] {
                const auto body = parse_body(req.body);
                const auto pick = [&body](const char* k) {
                    const auto it = body.find(k);
                    if (it == body.end() || !it->is_string()) {
                        throw ApiError(400, SAY("缺 ") + k);
                    }
                    return it->get<std::string>();
                };
                const auto root = changji::paths::from_utf8(pick("path"));
                config::VideoConfig v;
                v.orientation = pick("orientation");
                v.quality = pick("quality");
                // **先校验再写。** 写进去再报错的话，文件已经坏了，
                // 而下一次加载会整个失败——那时候连界面都打不开。
                const auto errs = v.validate();
                if (!errs.empty()) {
                    std::string msg;
                    for (const auto& e : errs) {
                        if (!msg.empty()) msg += SAY("；");
                        msg += e;
                    }
                    throw ApiError(400, msg);
                }

                // 老项目（建在有标准模板之前）没有这份文件时先按项目模板
                // 起底。不然 save_user_config 拿**全局**模板起底，项目配置里
                // 会冒出 [llm]、[workers] 这些和电影无关的节。
                // 文件已经在的话这一步什么都不做，下面照常改那两行。
                config::write_project_config(root, v);
                config::save_user_config(
                    json{{"video",
                          {{"orientation", v.orientation},
                           {"quality", v.quality}}}},
                    root / "changji.toml");

                // **比例跟着画幅一起写。**
                //
                // 这两个字段管的是同一件事，但住在两个文件里：
                //     画幅  changji.toml [video].orientation      → 成片尺寸
                //     比例  assets.json StyleProfile.aspect_ratio → 参考图尺寸
                // 这个接口以前一个字都不碰后者，于是「横屏 + 9:16」配得出来
                // 也不报错，出来是参考图竖的、成片横的——而参考图是每一镜
                // 的底子。**存一次画面就把那份拷贝对齐**，顺带治好老项目。
                //
                // 资产库还没建起来的新项目跳过：这时候没有文件可改，
                // 而定妆那一步会按当时的画幅写对（见 post_bible）。
                try {
                    const models::ProjectStore store{root};
                    models::AssetLibrary assets = store.load_assets();
                    if (assets.style.aspect_ratio != v.aspect_ratio()) {
                        assets.style.aspect_ratio = v.aspect_ratio();
                        store.save_assets(assets);
                    }
                } catch (const std::exception&) {
                    // 画面设置本身已经存好了，不该因为资产库不在就整个失败。
                }

                const auto [w, h] = v.size();
                return ApiResult{200,
                                 {{"orientation", v.orientation},
                                  {"quality", v.quality},
                                  {"width", w},
                                  {"height", h}}};
            });
            return json_response(r.body, r.status);
        });

    // ---- 这部电影的成片工序：后期链和声音 ----
    //
    // 电影的属性，住在项目的 changji.toml 的 [look] / [sound]。放大和配乐
    // 命令是机器属性，不在这儿（全局 [upscale] / [sound].music_command）。
    // 同 /bff/project/video：C++ 独有，所以在 /bff。
    const auto finish_json = [](const config::Settings& s) {
        return json{{"look",
                     {{"preset", s.look.preset},
                      {"lut", s.look.lut},
                      {"lut_strength", s.look.lut_strength},
                      {"grain", s.look.grain},
                      {"soften", s.look.soften},
                      {"letterbox", s.look.letterbox}}},
                    {"sound",
                     {{"ambient", s.sound.ambient},
                      {"ambient_db", s.sound.ambient_db},
                      {"music", s.sound.music},
                      {"music_db", s.sound.music_db},
                      {"duck", s.sound.duck},
                      {"music_style", s.sound.music_style},
                      // 机器上配没配配乐命令：页面上「配乐」那个勾要靠它
                      // 说清「勾了也出不来」。
                      {"music_ready", !s.sound.music_command.empty()}}},
                    // 「只做前 n 分钟」那个 n。**和后期、声音同住一条接口**
                    // 是因为它们是同一类东西：这部电影自己的成片工序，一部
                    // 电影一份，跟着项目目录走。分开一条接口的话，弹窗里
                    // 三格要发三次请求，而它们本来就在同一个保存按钮下面。
                    {"preview_s", s.preview.seconds},
                    {"upscale", s.upscale.enabled()}};
    };

    CROW_ROUTE(app, "/bff/project/finish")([finish_json](const crow::request& req) {
        auto r = guard([&] {
            const auto root =
                changji::paths::from_utf8(required_query(req, "path"));
            return ApiResult{200, finish_json(config::load_settings(root))};
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/project/finish")
        .methods("POST"_method)([finish_json](const crow::request& req) {
            auto r = guard([&] {
                const auto body = parse_body(req.body);
                const auto it = body.find("path");
                if (it == body.end() || !it->is_string()) {
                    throw ApiError(400, SAY("缺 path"));
                }
                const auto root = changji::paths::from_utf8(it->get<std::string>());

                // 先按现在的读出来、盖上改动、校验，再写。写进去一个非法值
                // 的话下一次加载整个项目都打不开。
                config::Settings s = config::load_settings(root);
                json patch = json::object();
                const auto take_look = body.find("look");
                if (take_look != body.end() && take_look->is_object()) {
                    const json& l = *take_look;
                    if (l.contains("preset") && l["preset"].is_string()) {
                        s.look.preset = l["preset"].get<std::string>();
                    }
                    if (l.contains("lut") && l["lut"].is_string()) {
                        s.look.lut = l["lut"].get<std::string>();
                    }
                    if (l.contains("lut_strength") && l["lut_strength"].is_number()) {
                        s.look.lut_strength = l["lut_strength"].get<double>();
                    }
                    if (l.contains("grain") && l["grain"].is_number()) {
                        s.look.grain = l["grain"].get<double>();
                    }
                    if (l.contains("soften") && l["soften"].is_number()) {
                        s.look.soften = l["soften"].get<double>();
                    }
                    if (l.contains("letterbox") && l["letterbox"].is_number()) {
                        s.look.letterbox = l["letterbox"].get<double>();
                    }
                    patch["look"] = {{"preset", s.look.preset},
                                     {"lut", s.look.lut},
                                     {"lut_strength", s.look.lut_strength},
                                     {"grain", s.look.grain},
                                     {"soften", s.look.soften},
                                     {"letterbox", s.look.letterbox}};
                }
                const auto take_sound = body.find("sound");
                if (take_sound != body.end() && take_sound->is_object()) {
                    const json& d = *take_sound;
                    if (d.contains("ambient") && d["ambient"].is_boolean()) {
                        s.sound.ambient = d["ambient"].get<bool>();
                    }
                    if (d.contains("ambient_db") && d["ambient_db"].is_number()) {
                        s.sound.ambient_db = d["ambient_db"].get<double>();
                    }
                    if (d.contains("music") && d["music"].is_boolean()) {
                        s.sound.music = d["music"].get<bool>();
                    }
                    if (d.contains("music_db") && d["music_db"].is_number()) {
                        s.sound.music_db = d["music_db"].get<double>();
                    }
                    if (d.contains("duck") && d["duck"].is_boolean()) {
                        s.sound.duck = d["duck"].get<bool>();
                    }
                    if (d.contains("music_style") && d["music_style"].is_string()) {
                        s.sound.music_style = d["music_style"].get<std::string>();
                    }
                    patch["sound"] = {{"ambient", s.sound.ambient},
                                      {"ambient_db", s.sound.ambient_db},
                                      {"music", s.sound.music},
                                      {"music_db", s.sound.music_db},
                                      {"duck", s.sound.duck},
                                      {"music_style", s.sound.music_style}};
                }
                if (const auto pv = body.find("preview_s");
                    pv != body.end() && pv->is_number()) {
                    s.preview.seconds = pv->get<double>();
                    patch["preview"] = {{"seconds", s.preview.seconds}};
                }
                std::vector<std::string> errs = s.look.validate();
                for (const auto& e : s.sound.validate()) errs.push_back(e);
                for (const auto& e : s.preview.validate()) errs.push_back(e);
                if (!errs.empty()) {
                    std::string msg;
                    for (const auto& e : errs) {
                        if (!msg.empty()) msg += SAY("；");
                        msg += e;
                    }
                    throw ApiError(400, msg);
                }
                if (!patch.empty()) {
                    // 老项目没有这份文件时先按项目模板起底（同 /bff/project/video）。
                    //
                    // ⚠️ **起底那份写的是 `s.video`**，也就是
                    // `load_settings` 给出的「这个项目自己的画幅」——老项目
                    // 是从它的 assets.json 推回来的那一档。这一页调的是后期
                    // 和声音，用户根本没碰画幅；写内置默认的话，2026-09-18
                    // 默认翻成横屏之后，**在这一页点一下保存就把 landscape
                    // 永久写进一个竖屏项目**，不可逆也没有提示。
                    config::write_project_config(root, s.video);
                    config::save_user_config(patch, root / "changji.toml");
                }
                return ApiResult{200, finish_json(config::load_settings(root))};
            });
            return json_response(r.body, r.status);
        });

    // ---- 投递（第八步）：这一套没搬进来 ----
    //
    // 它要存投递记录、要平台配置和凭据，是另一套东西。**但不能就这么
    // 404**：前端 `Promise.all([api.platforms(), api.publishTargets()])`
    // 一挂，整页就是一个红框，用户不知道是"没做"还是"坏了"。
    //
    // 回一个合法的空形状加一句说明——页面画得出来，而且说得清为什么是空的。
    const auto publish_not_here = [](const char* field) {
        return [field] {
            json body{{field, json::array()},
                      {"error",
                       SAY("投递这一套 2026-09-12 随 Node 那层一起删了，现在"
                           "没有任何部署能跑它——别去找 webapp/server，那个"
                           "目录已经不在仓库里了。成片就在项目的 output 目录"
                           "下，自己拿去传；要把它做回来的话是在引擎里重做"
                           "一遍。")}};
            return json_response(body);
        };
    };
    CROW_ROUTE(app, "/bff/publish/platforms")(publish_not_here("platforms"));
    CROW_ROUTE(app, "/bff/publish/targets")(publish_not_here("targets"));
    CROW_ROUTE(app, "/bff/publish/records")(publish_not_here("records"));

    // 写那几条直接说清楚。回 501 而不是 404：404 像"地址写错了"，
    // 501 是"这条路存在但这个部署没实现"，而后者才是实情。
    const auto publish_write = [](const crow::request&) {
        return json_response(
            {{"detail",
              SAY("投递这一套 2026-09-12 随 Node 那层一起删了，没有任何部署"
                  "能跑它。成片在项目的 output 目录下，自己拿去传。")}},
            501);
    };
    CROW_ROUTE(app, "/bff/publish/targets").methods("POST"_method)(publish_write);
    CROW_ROUTE(app, "/bff/publish/deliver").methods("POST"_method)(publish_write);
    CROW_ROUTE(app, "/bff/publish/batch").methods("POST"_method)(publish_write);
    // 删投递目标：前端拼的是 /bff/publish/targets/<id>。
    CROW_ROUTE(app, "/bff/publish/targets/<string>")
        .methods("DELETE"_method)(
            [publish_write](const crow::request& req, const std::string&) {
                return publish_write(req);
            });

    // 八步走到哪一步了。
    CROW_ROUTE(app, "/bff/flow")([](const crow::request& req) {
        const char* raw_path = req.url_params.get("path");
        json body{{"steps", flow_steps()}};
        if (raw_path == nullptr || *raw_path == 0) {
            // 还没选项目：八步全未完成，前端照样画得出侧边栏
            json done = json::object();
            for (const auto& st : flow_steps()) done[st["key"]] = false;
            body["done"] = done;
            body["counters"] = json::object();
            body["project"] = nullptr;
            body["episode"] = nullptr;
            return json_response(body);
        }
        const std::string path = raw_path;

        auto proj = guard([&] { return get_project(path); });
        if (proj.status != 200) {
            return json_response(proj.body, proj.status);
        }
        const json& project = proj.body;

        // 没指定就跟第一章走，和 Node 那边一样。
        //
        // **认不出来的也算"没指定"。** 原来的判据只有"空串"，于是非空但
        // 不在这部电影里的章号（前端 localStorage 里躺着一个删掉的章号、
        // 另一个标签页刚把它删了、或者别处传错了）会被**原样回给前端**。
        // 而前端那句「引擎挑了哪一章就跟着它」比的是
        // `data.episodeId !== episodeId.value`——两边一样，它就不改；
        // `data.episodeId` 又非空，`if (!data.episodeId) selectEpisode('')`
        // 那条兜底也不走。结果章号永久卡在一个不存在的章上，还写在
        // localStorage 里，刷新也出不来：顶栏那个下拉 v-model 落空显示
        // 空白，「这一章」整页没有东西，而界面上一个字都不会说。
        //
        // 前端是**指望这儿判**的（session.js：「省得前端自己再判一遍第一
        // 章是谁」），那就真判到底：查不到就退回第一章；一章都没有就回
        // 空串，让前端把它清掉。
        std::string episode_id;
        if (const char* e = req.url_params.get("episode_id")) episode_id = e;
        const auto& eps = project.contains("episodes") && project["episodes"].is_array()
                              ? project["episodes"]
                              : json::array();
        const bool known =
            !episode_id.empty() &&
            std::any_of(eps.begin(), eps.end(), [&](const json& e) {
                return e.is_object() &&
                       e.value("episode_id", std::string()) == episode_id;
            });
        if (!known) {
            episode_id = !eps.empty() && eps[0].is_object()
                             ? eps[0].value("episode_id", std::string())
                             : std::string();
        }
        json episode = nullptr;
        for (const auto& e : eps) {
            if (e.is_object() && e.value("episode_id", std::string()) == episode_id) {
                episode = e;
                break;
            }
        }

        // 分镜和产物取不到就当空的——**别让整条判定挂掉**。
        // 一个还没出分镜的项目本来就该走到"分镜"那一步停下，
        // 而不是让侧边栏整个不显示。
        json shots = json::array();
        if (!episode_id.empty()) {
            auto r = guard([&] { return get_shots(path, episode_id); });
            if (r.status == 200 && r.body.contains("shots") &&
                r.body["shots"].is_array()) {
                shots = r.body["shots"];
            }
        }
        json outputs = json::array();
        {
            auto r = guard([&] { return get_outputs(path); });
            if (r.status == 200 && r.body.contains("files") &&
                r.body["files"].is_array()) {
                outputs = r.body["files"];
            }
        }

        // 故事读不到就当空的——老项目没有 story.json，不该因此让整条
        // 判定挂掉，跟上面分镜和产物是一个处理。
        json story = json::object();
        {
            auto r = guard([&] { return get_story(path); });
            if (r.status == 200 && r.body.contains("story") &&
                r.body["story"].is_object()) {
                story = r.body["story"];
            }
        }

        // 资产库也要：「理解过没有」看库里有没有人，「图画齐没有」看每个人
        // 三张脸、每个地方一张空景在不在——这些字段只有 /api/assets 带。
        json assets = json::object();
        {
            auto r = guard([&] { return get_assets(path); });
            if (r.status == 200 && r.body.is_object()) assets = r.body;
        }
        // 成片那一格：那一部电影在不在。
        json film = json::object();
        {
            auto r = guard([&] { return get_film(path); });
            if (r.status == 200 && r.body.is_object()) film = r.body;
        }

        const json assessed =
            flow_assess(project, shots, outputs, episode_id, story, assets, film);
        body["done"] = assessed["done"];
        body["counters"] = assessed["counters"];
        body["project"] = project;
        body["episodeId"] = episode_id;
        body["episode"] = episode;
        body["outputs"] = outputs;
        return json_response(body);
    });

    // ---- 前端 ----
    //
    // **这里以前回的是一句 text/plain 指路**（"界面在 Node 那一层，
    // 默认 5174"）。那是迁移期的临时状态，代价是用户得起两个进程，
    // 而且打开引擎的端口看不到任何东西。现在把打包好的前端嵌进二进制
    // 直接发（见 http/webapp.hpp）。
    //
    // `/bff/*` 现在也在上面答了（清单见 bff_routes.hpp），所以整个界面
    // 都能用，不用再起 Node 那一层。
    //
    // **逐字段填 res，不要整个赋值。**
    // catchall 拿到的 `crow::response&` 已经带着这次连接的内部状态，
    // `res = 另一个 response` 会把那些状态一起覆盖掉——浏览器收到的是
    // ERR_CONTENT_LENGTH_MISMATCH，而服务端日志里明明白白写着 200。
    // 日志说成功、浏览器说坏掉，这种最难查。
    const auto fill_webapp = [](const crow::request& req, crow::response& res) {
        const std::string rel = normalize_webapp_path(req.url);
        if (rel.empty()) {
            // 只有 `..` 这类会走到这儿
            res.code = 400;
            res.body = SAY("路径不合法");
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            return;
        }
        const std::string* body = find_webapp_file(rel);
        std::string name = rel;
        if (!body && wants_file(rel)) {
            // **要的是具体文件却没有，就老老实实 404。**
            //
            // 以前这里也回 index.html，代价很大：浏览器按 <script> 去取
            // assets/index-旧哈希.js，拿回来的是一整页 HTML，还是 200。
            // 解析当场失败、页面全白，而 F12 里每一个请求都是 200，
            // 日志里也全是 200 —— 没有任何东西提示出了错。
            // 换一版之后浏览器还拿着旧 index.html 的时候就是这个场面。
            res.code = 404;
            res.body = SAY("没有这个文件：") + rel;
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.set_header("Cache-Control", "no-store");
            return;
        }
        if (!body) {
            // 前端路由（/shots 这种，没有扩展名）回 index.html：
            // 单页应用的深链接靠这个。
            body = find_webapp_file("index.html");
            name = "index.html";
        }
        if (!body) {
            res.code = 500;
            // **照着这句话敲下去要能跑通。** 原来写的是
            // 「cd webapp/client && npm run build，然后 python
            // cpp/tools/gen_webapp.py」——两处都不成立：`cd` 之后人还站在
            // webapp/client 里，那儿没有 cpp/tools；而现在的 macOS 和多数
            // Linux 发行版里根本没有 `python` 这个命令（只有 python3）。
            // 脚本自己头上那段跑法是对的（带着 `&& cd ../..`），抄它。
            res.body =
                SAY("前端没打包进来。在 changji/ 目录下跑一遍："
                    "cd webapp/client && npm run build && cd ../.. ，然后 "
                    "python3 cpp/tools/gen_webapp.py（Windows 上是 python），"
                    "再重编。");
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            return;
        }
        res.code = 200;
        res.body = *body;
        res.set_header("Content-Type", webapp_content_type(name));
        // 带哈希的资源长期缓存，index.html 每次核一遍。见 webapp_cache_control。
        res.set_header("Cache-Control", webapp_cache_control(name));
    };

    const auto webapp_route = [fill_webapp](const crow::request& req) {
        crow::response res;
        fill_webapp(req, res);
        return res;
    };
    CROW_ROUTE(app, "/")(webapp_route);

    // **静态资源走普通路由，不走 catchall。**
    // 实测：同一个文件从 catchall 出去只有 65389 字节，从普通路由出去是
    // 完整的 131332——浏览器报 ERR_CONTENT_LENGTH_MISMATCH，页面一片空白，
    // 而服务端日志两次都写 200。原因没查到底（Crow 的 catchall 那条路上
    // 大 body 被截断），但普通路由是好的，就走普通路由。
    // catchall 只留给单页应用兜底，那里只发几百字节的 index.html。
    CROW_ROUTE(app, "/assets/<path>")(
        [webapp_route](const crow::request& req, const std::string&) {
            return webapp_route(req);
        });

    // **单段的前端路由也走普通路由。**
    //
    // 2026-09-11 实测：catchall **把 body 整个丢了**——`/project` 回
    // 200 但 0 字节，`/nope.js` 回 404 也是 0 字节，而同样的内容从普通
    // 路由出去是完整的。上面那条注释记的"从 catchall 出去只有 65389 字节"
    // 是同一个毛病，当时以为只有大 body 受影响，就把静态资源挪走了、
    // 把单页应用的兜底留下了。**留下的那半正是白屏**：
    // 从 `/` 进去能用，一刷新（或者直接打开 /shots）就是空白页，
    // 而且刷多少次都一样——body 压根没发出来。
    //
    // `<string>` 只吃一段，不含斜杠，所以 `/api/run` 这类两段的接口不会被
    // 它抢走（上面那条注释说的 `<path>` 会吃斜杠，才是不能用的那个）。
    // 现在所有前端路由都是单段：/project、/shots、/settings……
    CROW_ROUTE(app, "/<string>")(
        [fill_webapp](const crow::request& req, const std::string&) {
            crow::response res;
            if (!webapp_owns(req.url)) {
                // 单段的接口路径没匹配上，照常回 404 JSON，别拿 index.html 顶
                res.code = 404;
                res.body = R"({"detail":"Not Found"})";
                res.set_header("Content-Type", "application/json");
                return res;
            }
            fill_webapp(req, res);
            return res;
        });

    // **兜底用 CROW_CATCHALL_ROUTE，不能用 `/<path>`。**
    // Crow 的 `<path>` 连斜杠一起吃，写成路由的话它会把 `/api/outputs`、
    // `/api/run` 这些全匹配走——对拍当场报出来：Python 回 200，C++ 回 404。
    // catchall 只在别的路由都没匹配上时才触发，正好是单页应用兜底的语义。
    CROW_CATCHALL_ROUTE(app)
    ([fill_webapp](const crow::request& req, crow::response& res) {
        if (!webapp_owns(req.url)) {
            // 接口路径没匹配上就是真的没有这个接口，照常回 404 JSON
            res.code = 404;
            res.body = R"({"detail":"Not Found"})";
            res.set_header("Content-Type", "application/json");
            res.end();
            return;
        }
        fill_webapp(req, res);
        res.end();
    });

    // ---- 项目的新建、删除、改梗概 ----
    //
    // 同样是阶段 3 漏掉的。删项目那个不可逆，三道闸在 projects.cpp 里。

    CROW_ROUTE(app, "/api/new").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_new_project(parse_body(req.body),
                                        config::runtime().snapshot());
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/project/rename").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_project_rename(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/project/delete").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_delete_project(parse_body(req.body),
                                           config::runtime().snapshot());
            });
            return json_response(r.body, r.status);
        });

    // 「只改梗概」。**界面不走这条**，走的是 POST /api/story。
    //
    // 梗概在盘上有两份：story.json 里那份是故事页编辑的，project.json 里
    // 那份是老流程写剧本的提示词读的。这条只写后者，两份就此对不上——而
    // `post_story` 专门为这件事多写了一句同步（「两边各存一份的话，在故事
    // 页改完梗概、去写剧本用的还是旧的那句」）。
    //
    // 留着是因为它在和 Python 的对拍范围内，删了就是破契约；但别再给它接
    // 新的调用方，要改梗概就走 /api/story。
    CROW_ROUTE(app, "/api/project/premise").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_project_premise(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // ---- 剧本读写与章节增删改 ----
    //
    // 这几个属于阶段 3，当时按 test_web_editing.py 的覆盖面移植而漏了。
    // 阶段 2 判据的实机验证里前端调出 404 才发现。

    CROW_ROUTE(app, "/api/script")([](const crow::request& req) {
        auto r = guard([&] {
            return get_script(required_query(req, "path"),
                              required_query(req, "episode_id"));
        });
        return json_response(r.body, r.status);
    });

    // 这一章的原料：场、原文、钩子、四段按秒的排法。和 AI 改编时拿到的
    // 是同一份，只是给人看。
    CROW_ROUTE(app, "/api/script/context")([](const crow::request& req) {
        auto r = guard([&] {
            return get_script_context(required_query(req, "path"),
                                      required_query(req, "episode_id"));
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/script").methods("POST"_method)(
        script_route(&post_script));

    CROW_ROUTE(app, "/api/episode").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_episode(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/episode/action").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_episode_action(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // ---- 两个长任务 ----
    //
    // 立刻返回 {"started": true, ...}，活干在工作线程上。
    // 进度靠 GET /api/script/series 轮询或者 WebSocket 推。
    //
    // 客户端是 shared_ptr：**任务比起它的那次请求活得久**，交引用的话，
    // 将来换成按项目建的客户端时，引用会在任务还跑着的时候失效。
    // 存进 static 同样不冻配置，理由见 `llm::make_client`。
    static std::shared_ptr<llm::Client> batch_client =
        llm::make_client(llm::default_http_post(),
                         llm::default_http_post_stream());

    const auto batch_route = [](auto handler) {
        return [handler](const crow::request& req) {
            auto r = guard([&] {
                return handler(parse_body(req.body),
                               batch_client);
            });
            return json_response(r.body, r.status);
        };
    };

    // ---- 对话 ----
    //
    // 桌面端那条对话（`agent/`）。**全是新路由**，网页那 126 个接口一个都
    // 没动——它不认得这几条，也不会去要。
    //
    // POST 立刻回 202：一轮里模型可能连着调几次工具，占住 Crow 这条线程的
    // 后果见本文件开头 concurrency 那段（一写字界面就卡住）。
    CROW_ROUTE(app, "/api/chat").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_chat(parse_body(req.body), batch_client,
                             [] { return default_run_deps(); }); });
        return json_response(r.body, r.status);
    });

    // 图标条：这部片子现在都有些什么、各自动没动过。**只答事实**——
    // "看过了没有"是客户端自己的事，那是"这台机器上这个人"的属性。
    // 枚举值 → 中文。**桌面端读它，别自己抄一份**（见 http/enums.hpp）。
    CROW_ROUTE(app, "/api/enums")([] {
        auto r = guard([] { return get_enums(); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/rail")([](const crow::request& req) {
        const char* p = req.url_params.get("path");
        auto r = guard([&] { return get_rail(p ? p : ""); });
        return json_response(r.body, r.status);
    });

    // 那一格里有什么，一段话。走的**和模型是同一个工具**，但**不是同一份
    // 话**：模型那一遍要中文原话，这一遍要人自己的语言，所以 `get_peek`
    // 带着 `i18n::Audience::human()` 去调（见 util/say.hpp）。
    CROW_ROUTE(app, "/api/peek")([](const crow::request& req) {
        const char* p = req.url_params.get("path");
        const char* k = req.url_params.get("key");
        const char* e = req.url_params.get("episode_id");
        auto r = guard([&] { return get_peek(p ? p : "", k ? k : "", e ? e : ""); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/chat/history")([](const crow::request& req) {
        const char* p = req.url_params.get("project");
        const char* c = req.url_params.get("chat");
        auto r = guard([&] { return get_chat_history(p ? p : "", c ? c : ""); });
        return json_response(r.body, r.status);
    });

    // 这部片子有哪几条对话。**参数名是 `path`**，和别的"按项目问"的接口
    // 一致（`/api/shots?path=`、`/api/story?path=`）；`/api/chat/*` 那几条
    // 当初用的是 `project`，两边都留着，别为了整齐去动老的那几条。
    CROW_ROUTE(app, "/api/chats")([](const crow::request& req) {
        const char* p = req.url_params.get("path");
        auto r = guard([&] { return list_chats(p ? p : ""); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/chats").methods("DELETE"_method)([](const crow::request& req) {
        const char* p = req.url_params.get("path");
        const char* c = req.url_params.get("chat");
        auto r = guard([&] { return delete_chat(p ? p : "", c ? c : ""); });
        return json_response(r.body, r.status);
    });

    // ---- 记忆：设置里「记忆」那一类（http/memory_api.hpp） ----
    CROW_ROUTE(app, "/api/memory")([](const crow::request& req) {
        const char* p = req.url_params.get("path");
        auto r = guard([&] { return get_memory(p ? p : ""); });
        return json_response(r.body, r.status);
    });
    CROW_ROUTE(app, "/api/memory/forget").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_memory_forget(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });
    CROW_ROUTE(app, "/api/memory/move").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_memory_move(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/chat/stop").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_chat_stop(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    // 「每步问我」那一档：人点了允许 / 不（见 chat_api.hpp）。
    CROW_ROUTE(app, "/api/chat/permit").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_chat_permit(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    // 从某一条回话上分出一条新对话（见 chat_api.hpp）。
    CROW_ROUTE(app, "/api/chat/fork").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_chat_fork(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/script/series").methods("POST"_method)(
        batch_route(&post_script_series));
    CROW_ROUTE(app, "/api/script/all").methods("POST"_method)(
        batch_route(&post_script_all));
    CROW_ROUTE(app, "/api/plan/all").methods("POST"_method)(
        batch_route(&post_plan_all));
    // 和上面两个共用 JobKind::Write 那个槽，所以进度也走
    // GET /api/script/series，前端那个 writer store 直接能用。
    CROW_ROUTE(app, "/api/story/chapters").methods("POST"_method)(
        batch_route(&post_story_chapters));
    // 理解故事：一件活串起提结构、定长相、逐章写剧本。同一个槽，同一条进度。
    CROW_ROUTE(app, "/api/story/understand").methods("POST"_method)(
        batch_route(&post_story_understand));
    // 一键成片：从一个空项目到一条能看的片子，六步一路跑到底。
    //
    // **和别的批量活同一个槽（JobKind::Write）、同一条进度**
    // （GET /api/script/series），所以页面上那个 writer store 直接能用。
    // 最后一步把出片交给 Run 那个槽就收手，见 http/oneclick.hpp。
    CROW_ROUTE(app, "/api/oneclick").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_oneclick(parse_body(req.body), batch_client,
                                     default_run_deps());
            });
            return json_response(r.body, r.status);
        });
    // 从网上找热点写一个故事。上网那一层在这儿给真的（测试塞假的）。
    CROW_ROUTE(app, "/api/story/from_web").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_story_from_web(parse_body(req.body), batch_client,
                                           llm::default_http_get());
            });
            return json_response(r.body, r.status);
        });

    // ---- 成片：有一章出了片，就把出了片的章接成一部完整的电影 ----
    //
    // 2026-09-18 定成电影平台，POST /api/film/cut（body 里带 per_episode_s）
    // 换成了 /api/film/join。**旧路由不留兼容**：前端打包进这个二进制，
    // 没有外部调用方，留一条空壳只会让下一个人以为切段还在。
    CROW_ROUTE(app, "/api/film")([](const crow::request& req) {
        auto r = guard([&] { return get_film(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });
    CROW_ROUTE(app, "/api/film/join").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] { return post_film_join(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    // ---- 任务状态与开跑 ----

    // **带上 `path` 问**：回包里的 `mine` 说清正在跑的那一轮是不是这部电影
    // 的。不带也行（老客户端、命令行），那时 `mine` 一律真。见 run.hpp。
    CROW_ROUTE(app, "/api/run")([](const crow::request& req) {
        auto r = guard([&] { return get_run_status(query(req, "path")); });
        return json_response(r.body, r.status);
    });

    // 排着的那几件出片，全清了。点错了要能撤。
    CROW_ROUTE(app, "/api/run/queue/clear").methods("POST"_method)([] {
        auto r = guard([] { return post_run_queue_clear(); });
        return json_response(r.body, r.status);
    });

    // 开跑之前先看看这一次会做什么、大概多久。一按就是几十分钟，
    // 哪些镜头会重做应该在按下去之前就知道。
    //
    // 画像走 runtime 而不是自己 detect()：用户在设置页改过的画质档位
    // 要算进预估里，不然改完分辨率预演的时间不变，看着像是没生效。
    CROW_ROUTE(app, "/api/run/preview")([](const crow::request& req) {
        auto r = guard([&] {
            // skip_draft 默认真，和 POST /api/run 一致。
            // **预览和实际必须是同一套默认**，否则它说的是另一件事。
            const char* sd = req.url_params.get("skip_draft");
            const bool skip_draft = sd == nullptr || std::string(sd) != "false";
            // 「只做前 n 分钟」那颗按钮底下那行预估。不传 = 整章，老行为。
            double preview_s = 0.0;
            if (const char* p = req.url_params.get("preview_s")) {
                try {
                    preview_s = std::stod(p);
                } catch (const std::exception&) {
                    throw ApiError(400, SAY("preview_s 要是个数（秒）"));
                }
            }
            return get_run_preview(required_query(req, "path"),
                                   query(req, "episode_id"),
                                   query_bool(req, "all_episodes"),
                                   query_bool(req, "skip_final"), skip_draft,
                                   query_bool(req, "force"),
                                   config::runtime().profile(), preview_s);
        });
        return json_response(r.body, r.status);
    });

    // 服务端有哪些参考音色。设定页的「人物」那一格打开时顺带拉一次。
    //
    CROW_ROUTE(app, "/api/voices")([](const crow::request& req) {
        auto r = guard([&] {
            return get_voices(required_query(req, "path"),
                              config::runtime().snapshot().tts.backend);
        });
        return json_response(r.body, r.status);
    });

    // 「制作音色」：摇一段试听、满意了存成参考音频。
    //
    // **为什么是摇而不是描述。** 我们这条运行时（llama.cpp 的 mtmd）只
    // 实现了 Qwen3-TTS 的 Base 模式，也就是参考音频克隆；自带说话人的
    // CustomVoice 和用文字描述造音色的 VoiceDesign 都不在里面。而不给
    // 参考音频时说话人是被采样出来的，种子换一个就是换一个人。
    // 见 http/voices.hpp 里 preset_voice_seeds 上面那段。
    CROW_ROUTE(app, "/api/voice/take").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] { return post_voice_take(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/voice/save").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] { return post_voice_save(parse_body(req.body)); });
            return json_response(r.body, r.status);
        });

    // 预置音色就是一组固定的种子。**回的是种子不是音频**——音频要摇出来
    // 才有，而摇一次要几秒，九个一起摇会让页面卡一分多钟。
    CROW_ROUTE(app, "/api/voice/presets")([] {
        auto r = guard([]() -> ApiResult {
            nlohmann::ordered_json arr = nlohmann::ordered_json::array();
            int n = 1;
            for (const auto& p : stages::preset_voices()) {
                // **基频单独给一个字段**，界面挨着名字印（「预置 1 · 96Hz」）。
                // 九个「预置 N」并排，光看名字分不出哪个是男声。
                arr.push_back(
                    {{"id", "preset" + std::to_string(n)},
                     {"name", SAYF("预置 %1", std::to_string(n))},
                     {"seed", p.seed},
                     {"hz", p.hz}});
                ++n;
            }
            return {200, {{"presets", std::move(arr)}}};
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/outputs")([](const crow::request& req) {
        auto r = guard([&] { return get_outputs(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    // 开跑。立刻返回，进度靠上面那个轮询或者 WebSocket 推。
    // 后端每次开跑现取（配置可能刚被改过），所以 deps 不在这里存一份。
    CROW_ROUTE(app, "/api/run").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_run(parse_body(req.body),
                                default_run_deps());
            });
            return json_response(r.body, r.status);
        });

    // 把某一件正在后台跑的活停掉。**按 stream 停，不按种类停。**
    //
    // 上面 /api/stop 和 /api/script/series/stop 停的是"出片"和"写全片"那两个
    // 长跑任务，一种只有一个槽。而写大纲、写正文、拆分镜这一族是按请求起的，
    // 同时可以有好几件——只能按它自己那条 stream 认。
    CROW_ROUTE(app, "/api/job/cancel").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                const json body = parse_body(req.body);
                const std::string id = stream_of(body);
                if (id.empty()) throw ApiError(400, SAY("要给 stream"));
                // 找不到不报错：按下去那一刻可能刚好干完，重复点也不该弹框。
                return ApiResult{200, {{"stopped", cancel_job(id)}}};
            });
            return json_response(r.body, r.status);
        });

    // 连不上 WebSocket 的时候，同一批消息改从这儿取。见 job_stream.hpp 里
    // mail_open 上面那段：没有 socket 的那条退路原来是"同步跑到干完"，
    // 于是进度、思考、停下三样一起没有。
    //
    // **开信箱要在发那个请求之前。** 反过来的话开之前那几条没地方存，
    // 表现为"前面一截思考不见了"——而第一段思考往往就在那几百毫秒里。
    CROW_ROUTE(app, "/api/job/watch").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                const json body = parse_body(req.body);
                const std::string id = stream_of(body);
                if (id.empty()) throw ApiError(400, SAY("要给 stream"));
                mail_open(id);
                return ApiResult{200, {{"watching", true}, {"stream", id}}};
            });
            return json_response(r.body, r.status);
        });

    // 取走这条 stream 上 `since` 之后的消息。**只读**：信箱是上面那条
    // POST 开的，这儿开不出来——`exists` 为假就是"没开过或者早过期了"，
    // 界面该按"连接断了"处理，而不是傻等。
    CROW_ROUTE(app, "/api/job/events")([](const crow::request& req) {
        auto r = guard([&]() -> ApiResult {
            const std::string id = required_query(req, "stream");
            std::size_t since = 0;
            const std::string raw = query(req, "since");
            if (!raw.empty()) {
                try {
                    const long long n = std::stoll(raw);
                    if (n > 0) since = static_cast<std::size_t>(n);
                } catch (const std::exception&) {
                    throw ApiError(400, SAY("since 要是个数"));
                }
            }
            return ApiResult{200, mail_take(id, since)};
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/stop").methods("POST"_method)([](const crow::request&) {
        // 没在跑时回 {"stopped": false} 而不是报错。
        // 前端的停止按钮是无条件可点的，重复点不该弹错误框。
        const bool stopped = pipeline::jobs().cancel(pipeline::JobKind::Run);
        return json_response({{"stopped", stopped}});
    });

    CROW_ROUTE(app, "/api/script/series")([] {
        return json_response(pipeline::jobs().snapshot(pipeline::JobKind::Write));
    });

    CROW_ROUTE(app, "/api/script/series/stop").methods("POST"_method)
        ([](const crow::request&) {
            const bool stopped = pipeline::jobs().cancel(pipeline::JobKind::Write);
            return json_response({{"stopped", stopped}});
        });

    // ---- WebSocket ----
    //
    // 进度消息由 job 表通过上面那个 sink 推过来。
    // 前端可以只连 WebSocket，也可以继续轮询 /api/run——两条路并存，
    // WebSocket 断了退回轮询就行，任务本身不受影响。
    //
    // **两个地址是同一条路**，见 kWsRoutes 里为什么。
    const auto on_open = [](crow::websocket::connection& conn) {
        ws::hub().add(&conn);
        conn.send_text(json{{"type", "hello"},
                            {"service", "changji"}}.dump());
    };
    // 形参个数跟 Crow 版本走：1.2.0 是 (connection&, reason)，更新的版本
    // 多一个 uint16_t 关闭码。升级 Crow 时这里会编译报错，那是好事——
    // 静默的签名不匹配会让 onclose 根本不被调用，连接泄漏在注册表里，
    // 直到某次广播向已析构的对象发送才崩。
    const auto on_close = [](crow::websocket::connection& conn,
                             const std::string& /*reason*/) {
        ws::hub().remove(&conn);
    };
    const auto on_message = [](crow::websocket::connection& conn,
                               const std::string& data, bool is_binary) {
        if (is_binary) return;  // 上行没有二进制消息，忽略
        ws::hub().handle_client_message(&conn, data);
    };

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen(on_open)
        .onclose(on_close)
        .onmessage(on_message);

    CROW_WEBSOCKET_ROUTE(app, "/api/ws")
        .onopen(on_open)
        .onclose(on_close)
        .onmessage(on_message);

    CROW_LOG_INFO << SAY_NEVER("changji 监听 ") << opts.host << ":" << opts.port;

    // 局域网感知：把上次的状态读回来，**开着的话这会儿真转起来**。
    // 摆在 `run()` 前面一行——端口到这一刻才定下来，而往外报的就是它。
    lan::Sense::instance().boot(paths::user_data_dir("changji"), opts.port);

    app.bindaddr(opts.host)
        .port(static_cast<std::uint16_t>(opts.port))
        .concurrency(resolve_concurrency(opts.concurrency))
        .run();

    // 兜一道：不是走 `request_stop()` 停的时候（Crow 自己的信号处理），
    // 这儿也得在 `app` 析构之前把广播关掉。
    ws::hub().shutdown();
    // 那条 select 线程也收掉。**不收的话进程退不干净**——它挂在一个
    // 永远不会自己醒的 select 上。
    lan::Sense::instance().set_on(false);


    // 预热线程由上面那个 JoinAtExit 在这儿收掉。**一定要等它**：
    // 它碰的是调度器那个函数内静态量，不 join 就退出的话，它可能在静态量
    // 析构之后还在往里写。Ctrl+C 时如果正好在装模型，这里会多等它装完
    // ——装到一半没法打断，等它是对的。
}

}  // namespace changji::http
