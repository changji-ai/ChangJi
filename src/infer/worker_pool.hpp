#pragma once

// 工作进程池：把出图和出片派给别的进程去算。
//
// **为什么。** 见方案「多卡和多机怎么用起来」。一句话：sd.cpp 的进度回调是
// 全局的，同进程两个生成会互相串——同种模型的多实例只能靠多进程。
//
// **换实现的接缝早就在了。** `FrameRenderer` / `VideoRenderer` 是
// `std::function`，ComfyUI 那条路就是从这里换的实现，而那本质上就是
// "把渲染交给另一个进程"。这里是同一个接缝上的第三种实现，
// 只是另一头跑的是我们自己的 sd.cpp。
//
// **协调者这一侧不改状态。** 池只负责"把活派出去、把结果拿回来"，
// `Shot` 仍然由阶段那一层在协调者的线程上顺序改、顺序存盘。
// 并行的是推理，不是状态改动——这条守住了，单一写者就不破。

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <functional>

#include "infer/worker_proto.hpp"
#include "stages/audio.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"

namespace changji::infer {

/// 池子里代表**本机进程内**的那个槽位的地址。
///
/// **本机是池里正经的一个槽，不是特例。** 不这样的话，只要配了一台远程
/// 机器，本机那张卡就整个闲着——而那正是"本地还是远程"被做成两条代码
/// 路径的后果。这个约定串让它变成同一条路上的一个取值。
inline constexpr const char* kLocalEndpoint = "local";

/// 在本机进程内把任务跑完。由调用方注入。
///
/// **做成回调而不是直接调 `run_task_locally`**：那个函数要 `Settings`，
/// 而这个文件是给"把活发出去"用的，不该知道配置长什么样。理由和
/// `WorkerFarm::HealthProbe` 那处一样。
using LocalRunner = std::function<TaskResult(
    const Task&, const StepCallback&, pipeline::CancelToken&)>;

/// 一个工作进程的地址。
struct WorkerEndpoint {
    /// `http://127.0.0.1:9001` 这样。
    std::string url;
    /// 派活时带的口令，对应那台的 `[peer].token`。
    ///
    /// **本机自己拉起的那些不用填**：它们听回环，那边根本不查
    /// （见 `infer/peer_auth.hpp`）。跨机才要。
    std::string token;
};

/// 池。**线程安全**：阶段那一层会从多个线程同时借。
///
/// **池里一台工作机都连不上时，不抛、不停整轮：那一镜在队列里等它们回来**，
/// 只有取消能打断（run_task）。这和"这一镜渲染失败"是两回事：后者换台机器
/// 可能就好了、值得重试几次再降级；前者下一镜必然撞同一堵墙，重试和降级
/// 都是白费——2026-09-17 实撞过 17 镜各自撞墙、各自降级、人回来看到
/// 「16 个镜头已降级」。曾经的做法是停整轮（PoolUnreachable），用户不要：
/// 机器掉线多半是几分钟的事，该等着，不该让人回来再点一次。
class WorkerPool {
public:
    /// `pick` 是这部电影挑的档位（项目的 `[models.pick]`，{组: 选项 id}）。
    /// 池给每个派出去的任务都盖上它——**跨机时这是唯一一条能让对面用对
    /// 模型的路**：那台的模型目录在别处，路径带过去没有意义，只能带 id
    /// 让它自己去解析。空 = 这部电影没挑过，每台按自己 `[models]` 里的
    /// 文件名跑（老行为）。
    explicit WorkerPool(std::vector<WorkerEndpoint> endpoints,
                        LocalRunner local_runner = {},
                        std::map<std::string, std::string> pick = {});
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// 有几个工作进程。
    std::size_t size() const;

    /// 阶段那一层该开几路并发：每个工作进程一路，**跨机的再多一路**——
    /// 那一路用来在取上一镜产物的同时把下一镜派出去。算法在
    /// worker_roster.hpp 的 pool_lanes，那儿能测。
    std::size_t lanes() const;

    /// 挨个 ping 一遍，回活着的个数。起跑前查一次，
    /// **别等跑到一半才发现有一个是死的**。
    std::size_t alive() const;

    /// 把一件任务派给池里空着的那个工作进程，跑完回结果。
    ///
    /// **给"这台机器替别人干活"那条路用的。** 主程序挂着节点协议之后
    /// （一台机器一个进程、一条连接），外来任务原来是在主进程里就地跑的
    /// ——而一个进程只能用一张卡（CUDA_VISIBLE_DEVICES 在后端初始化时就
    /// 读走了，跑起来改不了）。用户 2026-09-17：「只用了一张卡」。
    ///
    /// 交给池之后，多卡机上那几个按卡拉起的子进程才吃得到外来的活。
    /// 三个 renderer 走的也是同一条路（它们各自拼好 Task 再进这儿），
    /// 所以档位、重试、隔离那几条规矩一视同仁。
    TaskResult run(const Task& task, const StepCallback& on_step,
                   pipeline::CancelToken& tok);

    /// 出图。签名和 `stages::sd_renderer()` 回的那个一样，可以直接顶替。
    stages::FrameRenderer frame_renderer();

    /// 出片。同上。
    stages::VideoRenderer video_renderer();

    /// 配音。签名和 `stages::TTSBackend::synthesize` 那个一样，直接顶替。
    ///
    /// **只在本机自己配不了的时候才该用它**：配一句才十几秒，为它跨机
    /// 搬一趟音频不划算。装配的那一层不知道这些，所以由 run_deps 决定。
    stages::Synthesizer tts_synthesizer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 从配置里那几个地址造一个池。地址为空回 nullptr——
/// **调用方看到 nullptr 就走进程内那条路**，行为和以前一模一样。
///
/// `token` 会带给每一个地址。**一个口令走遍自己这几台**是刻意的：
/// 都是自己的机器，每台配一个不同的口令只是给自己添麻烦。
/// `local_runner` 给了的话，`endpoints` 里那个 `kLocalEndpoint` 就不发
/// HTTP，直接在本进程里跑。没给而地址里又有 `local` 的话，那个槽会被
/// 当成一台连不上的机器——所以两者要一起给。
std::shared_ptr<WorkerPool> make_worker_pool(
    const std::vector<std::string>& endpoints, const std::string& token = {},
    LocalRunner local_runner = {},
    std::map<std::string, std::string> pick = {});

/// 同上，但**每台带自己的口令**。
///
/// 上面那个重载把同一个口令发给所有地址，注释里写的理由是"都是自己的机器"。
/// 但配置格式里每台可以单独写（`[[peer.nodes]].token`），探活那条也一直是
/// **先用这台自己的、没有才退回全局**（node_registry.cpp），而界面上
/// 「加一台机器」收的就是这台自己的口令。三处里只有派活用的是全局那个。
///
/// 后果实测到了（2026-09-15）：给一台机器单独设口令之后，它在表上是
/// 在线的、五项能力全绿，一派活就
///
///     配音失败：工作进程拒了这个任务（401）：口令不对或者没带
///
/// ——**看得见、永远派不动**。而公网绑定又强制要求设口令
/// （`refuse_to_listen`），推荐的安全配置恰好就是坏掉的那一种。
std::shared_ptr<WorkerPool> make_worker_pool(
    std::vector<WorkerEndpoint> endpoints, LocalRunner local_runner = {},
    std::map<std::string, std::string> pick = {});

}  // namespace changji::infer
