// 把 RunDeps 接到真实世界上。**单独一个文件，不进单元测试目标。**
//
// 理由和 llm/client_http.cpp 一样：这里要 include 那些带网络和大模型的
// 实现，而 run.cpp 里的队列、错误汇总、状态码全是纯逻辑，
// 得能在不链那些库的情况下测。
//
// 换句话说，这个文件里没有任何值得测的判断——它只是把
// "配置说走哪条路" 翻译成 "装哪两个函数对象"。
#include <mutex>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <memory>

#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "stages/tts_backends.hpp"
#include "config/runtime.hpp"
#include "doctor/doctor.hpp"
#include "http/run.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "infer/node_pick.hpp"
#include "infer/node_registry.hpp"
#include "infer/task_run.hpp"
#include "infer/worker_farm.hpp"
#include "infer/worker_pool.hpp"
#include "setup/catalog.hpp"
#include "stages/frames.hpp"
#include "util/say.hpp"

namespace changji::http {

using models::ProjectStore;

namespace {

/// 本机按显卡数拉起的那几个子进程。**全进程只拉一次。**
///
/// 原来这是 `backends()` 里的一个 static。**提出来是因为第二个用户来了**：
/// 主程序挂上节点协议之后（一台机器一个进程、一条连接），外来任务也该交给
/// 这几个子进程跑——不然一个进程只用得上一张卡（用户 2026-09-17：
/// 「只用了一张卡」）。两处共用同一个 static，不能各拉一份：各拉一份就是
/// 两套子进程抢同几张卡，端口还撞上。
std::shared_ptr<infer::WorkerFarm> shared_farm(const config::Settings& s) {
    static std::shared_ptr<infer::WorkerFarm> farm =
        infer::WorkerFarm::start(
            s, config::runtime().profile(),
            [](const std::string& base, int seconds) {
                // 工作进程的 /health 对 GET 和 POST 都答，
                // 为一次探活单独引一条 HTTP 路径不值得。
                auto post = llm::default_http_post();
                const auto deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(seconds);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (post(base + "/health", "", {}, 2.0).status == 200) {
                        return true;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(400));
                }
                return false;
            });;
    return farm;
}

}  // namespace

FarmRunner local_farm_runner(const config::Settings& settings) {
    // 预热好之后的池和它的规模。**ready 是发布点**：写 pool、n 都在它之前，
    // 读的一头看见 ready 为真才碰 pool。
    struct Warm {
        std::atomic<bool> ready{false};
        std::shared_ptr<infer::WorkerPool> pool;
        std::size_t n = 0;
        /// 本机拉起来的那几个子进程（配置里写死地址的那种没有）。槽数问它。
        std::shared_ptr<infer::WorkerFarm> farm;
    };
    static auto warm = std::make_shared<Warm>();
    static std::once_flag once;

    // 这台机器**会不会**有池子。和 `ready`（热好没有）是两回事：起服务后
    // 那几秒里 `ready` 还是假，而答案已经定了。
    //
    // ⚠️ **这两个混用过一次，代价是一张卡闲着、另一张卡上挤两个。**
    // 2026-09-17：双卡机上按下「一键出图」，第一件任务在 `ready` 变真前的
    // 那一秒进来，走下面那条"还没热好就就地跑"，于是**主进程自己在卡 0 上
    // 跑了一张**；一秒后 farm 就绪，第二件交给池子，池子挑了空着的
    // 9001——那也是卡 0。结果卡 0 上两个 21 GB 挤在一起（`nvidia-smi` 报
    // 43 GB），卡 1 上 3 MiB，用户看到的还是「没用多 GPU」。
    //
    // 就地跑那条路是给**单卡机**留的，不是给"还没热好"留的。会有池子就等，
    // 等的那几秒有进度可看（capacity 这段时间报 1，最多压着一件）。
    const bool pooled = !settings.workers.endpoints.empty() ||
                        (settings.workers.auto_spawn &&
                         config::runtime().profile().gpu.has_value() &&
                         config::runtime().profile().gpu->count > 1);
    // **起服务时就在后台拉 farm**，不让任何一件任务去等它（见 run.hpp）。
    std::call_once(once, [settings] {
        std::thread([settings] {
            const auto farm = shared_farm(settings);
            const std::vector<std::string> eps =
                farm ? farm->endpoints() : settings.workers.endpoints;
            if (!eps.empty()) {
                std::vector<infer::WorkerEndpoint> weps;
                weps.reserve(eps.size());
                // 本机那几个听回环、不查口令，token 留空。
                for (const auto& u : eps) weps.push_back(infer::WorkerEndpoint{u, ""});
                warm->pool = std::make_shared<infer::WorkerPool>(std::move(weps));
                warm->n = eps.size();
                warm->farm = farm;
            }
            warm->ready.store(true, std::memory_order_release);
        }).detach();
    });

    FarmRunner r;
    r.run = [settings, pooled](const infer::Task& task,
                               const infer::StepCallback& on_step,
                               pipeline::CancelToken& tok) {
        // 会有池子就等它热好。子进程起来加探活实测十秒上下，farm 起不来时
        // `ready` 照样会变真（`pool` 是空的），所以这儿不会永远等。
        while (pooled && !warm->ready.load(std::memory_order_acquire)) {
            if (tok.cancelled()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (warm->ready.load(std::memory_order_acquire) && warm->pool) {
            return warm->pool->run(task, on_step, tok);
        }
        // 单卡机（以及双卡机上 farm 一张都没拉起来）：就地跑，和以前一样。
        return infer::run_task_locally(task, settings, infer::Origin::Local,
                                       task.shot_id, on_step, tok);
    };
    r.capacity = [] {
        if (warm->ready.load(std::memory_order_acquire) && warm->pool) {
            // **活着的才算**：死了一个的话照旧报 n，别的机器以为这台还有位置，
            // 派过来的活挤在这台排队（见 WorkerFarm 里 `watch` 那段）。
            const std::size_t live = warm->farm ? warm->farm->alive() : warm->n;
            return std::max<std::size_t>(1, live);
        }
        return std::size_t{1};
    };
    return r;
}

RunDeps default_run_deps() {
    RunDeps d;
    d.settings = [] { return config::runtime().snapshot(); };
    // 场记上网那几个工具用的（RunDeps::web_get）。
    d.web_get = llm::default_http_get();
    // 远程 MCP 扩展用的（RunDeps::http_post）。
    d.http_post = llm::default_http_post();
    d.profile = [] { return config::runtime().profile(); };
    // 开工前那道闸，见 RunDeps::blocked。
    d.blocked = [] {
        const auto report = doctor::run_checks(config::runtime().snapshot());
        if (report.can_run()) return std::string{};
        std::string why;
        for (const auto& c : report.checks) {
            if (c.level != doctor::Level::FAIL) continue;
            // ⚠️ **连接的那两个标点也要跟着语言换。** `c.name` / `c.detail`
            // 早就翻了，可中间那个全角冒号没翻——德语下看到的是
            // 「Stimme im Prozess：Es fehlt…」，一句德文里嵌着一个中文标点。
            // 2026-09-22 起一台德语引擎点「出片」才看见。
            if (!why.empty()) why += SAY("；");
            why += SAYF("%1：%2", c.name, c.detail);
        }
        return why.empty() ? SAY("去设置页看体检那一节") : why;
    };
    // 第二个形参（项目目录）**故意不接名字**：RunDeps 的签名要求它在，而
    // 这一套后端一个字都没用到——`s` 已经是这一章自己的设置了（出片那条路
    // 每跑一章都 load_settings(项目目录) 重读）。接了名字不用，-Wall 每次
    // 编译报一条 unused-parameter。
    d.backends = [](const config::Settings& machine, const ProjectStore&) {
        // **先把这部电影挑的那几档盖上去。** `machine` 是这台自己的配置
        // （模型目录、ffmpeg 在哪），`[models.pick]` 是这部电影要哪一档——
        // 后者跟着项目目录走，机器换了也还是它。
        //
        // 盖在这一句上、而不是每条后端各盖一遍：进程内、本机工作进程、
        // 跨机三条路都从这个 `s` 长出去，漏掉哪一条的表现都是"同一章里
        // 有的镜头用了这一档、有的用了上一档"——出来都是能看的画面，
        // 没有哪一层会去比。
        const config::Settings s = setup::with_selections(machine,
                                                          machine.models.pick);
        pipeline::Backends b;
        // 默认这一套：进程内 sd.cpp。
        // 两边都拿 `s`：这是**这一章**的设置（出片那条路每跑一章
        // 都 load_settings(项目目录) 重读），采样旋钮要跟着它走。
        b.frame = stages::sd_renderer(s);
        b.video = infer::sd_video_renderer(s);
        b.frame_backend_name = "sd.cpp";

        // **本机多卡：自己把每张卡的工作进程拉起来。**
        // 用户启动的仍然是一个命令，多卡编排由它自己做。只在 endpoints
        // 为空、探到多于一张卡时才动手（见 WorkerFarm::start）。
        //
        // farm 是静态的：拉起来的子进程要活到进程结束，每次建 Backends
        // 都拉一遍的话，跑第二章时会再起 N 个、端口还撞上。
        const auto farm = shared_farm(s);
        const std::vector<std::string> farm_eps =
            farm ? farm->endpoints() : s.workers.endpoints;

        // 别的机器：按能力挑。**出图和出片要分开挑**——一台只装了出图
        // 模型的机器该参与首帧、不该参与出片，而以前那个池是"所有
        // 工作进程都能干所有活"。
        const auto nodes = infer::node_registry().snapshot(s);

        // 本机进程内跑一个任务。**做成回调注入**，池那一层不该知道
        // 配置长什么样（同 WorkerFarm::HealthProbe）。
        const infer::LocalRunner local_runner =
            [s](const infer::Task& t, const infer::StepCallback& on_step,
                pipeline::CancelToken& tok) {
                return infer::run_task_locally(t, s, infer::Origin::Local,
                                               infer::kLocalEndpoint, on_step,
                                               tok);
            };

        // **每台带自己的口令。** 配置格式里每台可以单独写
        // （`[[peer.nodes]].token`），探活那条一直是"先用这台自己的、没有才
        // 退回全局"（node_registry.cpp），界面上「加一台机器」收的也是这台
        // 自己的。派活这条原来只发全局那个，于是单独设了口令的机器在表上
        // 在线、一派活就 401——看得见、永远派不动。
        const auto token_of = [&](const std::string& url) {
            for (const auto& n : s.peer.nodes) {
                if (n.url == url) return n.token.empty() ? s.peer.token : n.token;
            }
            return s.peer.token;   // 本机自己拉起的那几个走这儿；它们不查
        };
        const auto eps_for = [&](infer::Capability cap) {
            std::vector<std::string> out;
            // 本机这一档：多卡时是自己拉起的那几个子进程，单卡时是
            // 进程内那个槽。**两者不叠加**——多卡时进程内不该再跑，
            // 那几个子进程已经把卡占满了。
            if (!farm_eps.empty()) {
                out = farm_eps;
            } else {
                for (const auto* n : infer::candidates_for(nodes, cap)) {
                    if (n->url == infer::kLocalEndpoint) {
                        out.push_back(n->url);
                        break;
                    }
                }
            }
            // 别的机器。**一台报几个槽就占几个位置**（同一个 url 出现几次）
            // ——池按下标记忙闲，两条就是两条独立通道，一台双卡机两张卡
            // 一起干靠的就是这一句。
            for (auto& u : infer::remote_slots_for(nodes, cap)) {
                out.push_back(std::move(u));
            }
            return out;
        };

        // **只有本机一个槽就别绕池了。** 那种情况下走池是纯粹多一层
        // 间接：一样的种子、一样的进程内 sd.cpp，只是中间过一遍任务的
        // 序列化和路径往返。单卡单机是最常见的用法，那条路上的行为
        // 应该和以前逐字节一样，不给自己留一个"绕了一圈才发现哪儿不同"
        // 的机会。
        const auto worth_pooling = [](const std::vector<std::string>& eps) {
            if (eps.empty()) return false;
            if (eps.size() == 1 && eps.front() == infer::kLocalEndpoint) {
                return false;
            }
            return true;
        };

        // **一个池只管一个能力。** 口令带上：本机自己拉起的那些听回环、
        // 不查，跨机那头要。
        const auto frame_eps = eps_for(infer::Capability::Frame);
        const auto video_eps = eps_for(infer::Capability::Video);
        // **档位也要带上。** 上面盖的那一层只改了本机这份配置里的文件名，
        // 而别的机器的模型目录在别处、盘符都可能不一样，路径带过去没有
        // 意义。带 id 过去，那台自己去解析（见 infer/task_run.cpp）。
        const auto with_tokens = [&](const std::vector<std::string>& urls) {
            std::vector<infer::WorkerEndpoint> out;
            out.reserve(urls.size());
            for (const auto& u : urls) out.push_back({u, token_of(u)});
            return out;
        };
        auto frame_pool = worth_pooling(frame_eps)
                              ? infer::make_worker_pool(with_tokens(frame_eps),
                                                        local_runner,
                                                        s.models.pick)
                              : nullptr;
        // **两边是同一批机器就共用一个池。** 首帧和出片现在同时跑
        //（pipeline/shot_flow.hpp），各建一个池的话，同一台机器在两个池里
        // 各占两个位置、加起来四个，而它只有两张卡——多出来的两件到了
        // 那台就是 409，本地把 409 当渲染失败，一镜三次之后降级
        //（2026-09-17 实撞：ep07 前两镜就是这么没的）。一个池，位置数
        // 才是那台真有的卡数，首帧和出片在同一批位置上自然交错。
        // 两边机器不一样（有台只装了出图模型）才各建各的。
        auto video_pool = (frame_pool && video_eps == frame_eps)
                              ? frame_pool
                          : worth_pooling(video_eps)
                              ? infer::make_worker_pool(with_tokens(video_eps),
                                                        local_runner,
                                                        s.models.pick)
                              : nullptr;

        if (frame_pool) {
            b.frame = frame_pool->frame_renderer();
            b.keepalive.push_back(frame_pool);
        }
        if (video_pool) {
            b.video = video_pool->video_renderer();
            // 共用时上面已经留过一次，别留两份
            if (video_pool != frame_pool) b.keepalive.push_back(video_pool);
        }
        if (frame_pool || video_pool) {
            const std::size_t n =
                std::max(frame_pool ? frame_pool->size() : 0,
                         video_pool ? video_pool->size() : 0);
            // 这个名字画在「给 N 个镜头出首帧（…）」那一句里，是界面。
            b.frame_backend_name = SAYF("sd.cpp（%1 处算力）",
                                        std::to_string(n));
            // **并发上限取大的那个。** 两个池不一样大时，小的那一阶段
            // 靠池自己挡住（借不到就等），而把上限压到小的那个会让
            // 大的那一阶段白白少跑几路。路数不等于机器数：跨机的多一路
            // 拉产物，见 WorkerPool::lanes。
            b.render_lanes = static_cast<int>(
                std::max(frame_pool ? frame_pool->lanes() : 0,
                         video_pool ? video_pool->lanes() : 0));
            if (farm) b.keepalive.push_back(farm);
        }

        // 装配和闸门要用。路径从配置来——用户可能把 ffmpeg 装在
        // 非 PATH 的地方，那时候 assembly.ffmpeg_path 是唯一的出路。
        b.ffmpeg = media::FFmpeg(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                 media::default_runner());

        // 配音后端。**搭法只有一份**（infer::make_tts_backend），因为别的
        // 机器派配音任务过来时走的也是它——两份的话，"这台配音到底走哪条
        // 路"迟早在两边不一样，表现是同一章里前半段有声、后半段静音。
        //
        // 任何一条路搭不起来都退回估算后端（留空即是），不抛：配音只是
        // 五个阶段之一，为它整条流水线跑不起来不值当，而估算后端会写出
        // 等长静音，画面那几步照样能验。真出不了声这件事在配音阶段的
        // start 事件里说清楚了，不会跑完一整章才发现。
        b.tts = infer::make_tts_backend(s, b.ffmpeg);

        // 配音也能派给别的机器：那张表上它是一列，能勾就得能派。
        //
        // **本机自己配得了就不派**：配一句才十几秒，为它跨机搬一趟音频
        // 不划算。本机配不了（没模型、或者这份二进制没编 llama.cpp）时
        // 才去找别人——而那正是以前只能改配置指到一个固定地址的情况。
        if (!b.tts.has_value()) {
            std::vector<std::string> remote_tts;
            for (const auto& u : eps_for(infer::Capability::Tts)) {
                if (u != infer::kLocalEndpoint) remote_tts.push_back(u);
            }
            // 口令同上：每台带自己的。实测 401 就是从这条路上报出来的
            // （「给 1 个镜头配音，peer」→「工作进程拒了这个任务（401）」）。
            if (auto tts_pool = infer::make_worker_pool(
                    with_tokens(remote_tts), local_runner, s.models.pick)) {
                b.tts = stages::TTSBackend{"peer", tts_pool->tts_synthesizer(),
                                           {}};
                // **池有几个位置就同时跑几镜。** 不带这一句的话配音那一层
                // 逐镜串行，池里第二个位置从头闲到尾——一台双卡机只有一张
                // 卡在配音。本机那条 TTS 不走这儿，保持 1。
                b.audio_lanes = static_cast<int>(tts_pool->lanes());
                b.keepalive.push_back(tts_pool);
            }
        }
        return b;
    };
    return d;
}

}  // namespace changji::http
