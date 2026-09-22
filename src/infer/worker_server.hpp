#pragma once

#include <crow.h>

// 工作进程：一张卡，一次一个任务。
//
// 起法：`changji --worker --gpu 0 --port 9001`
//
// **它就是现在这个单卡程序，只是不发接口、不碰项目文件。**
// 自己的 `Scheduler`、自己的显存预算、自己的 `ActiveGeneration`——
// 进程边界把"每卡一份预算"白送了，所以 `Scheduler` 一行都不用改。
//
// 四条路由，够用就行：
//
//     POST /task            派一个任务   → 202 {id}，忙着回 409
//     GET  /task/{id}       查进度       → {state, step, steps, loading, result}
//     POST /task/{id}/cancel 取消
//     GET  /health          活着没       → {gpu, busy}
//
// **忙着的时候回 409，不排队。** 排队会让协调者那边的并发上限失效——
// 它以为派出去的都在跑，实际上有几个在对面排着。

#include <string>

#include "config/settings.hpp"
#include "infer/worker_pool.hpp"

namespace changji::infer {

struct WorkerOptions {
    int port = 9001;
    /// 用哪张卡。**通过 CUDA_VISIBLE_DEVICES 生效**——
    /// 这一条在只有一张卡的机器上验不了，见方案里那三件要先验的事。
    int gpu = 0;
    std::string host = "127.0.0.1";
};

/// 起一个工作进程。阻塞到进程结束。
///
/// **回 false 表示压根没起**：对外监听而 `[peer].token` 没设时会当场
/// 拒绝（见 `infer/peer_auth.hpp`），拒绝的理由已经打到 stderr 了。
/// 调用方要把它变成一个非零退出码——起没起来必须能从退出码看出来，
/// 不然拿脚本拉起一堆工作进程时，"没起来"和"起来了"长得一模一样。
bool run_worker(const config::Settings& settings, const WorkerOptions& opts);

/// 把这套接口挂到**别的** app 上，让主程序也能当节点用。回是否挂上了。
///
/// **一台机器一个进程、一条连接。** 用户 2026-09-17：「我不说了只起一个，
/// 互联一个机器只连一次」。在这之前做不到：这几条路由只长在 `--worker`
/// 那个独立 app 上，主程序的同名路径会被单页应用的兜底路由接走、回一段
/// HTML，派活那头判成"不在线"（当天实撞：curl 回 200，body 是网页）。
/// 于是远程要当节点用，只能手动起 worker，还得一张卡一个。
///
/// 挂上之后，`[[peer.nodes]]` 里填那台的**主程序地址**就行。
///
/// 对外监听而 `[peer].token` 是空的时候**不挂**并回 false——那种情况下谁都
/// 能派活过来烧卡。判据和 run_worker 同一个（peer_auth.hpp）。
/// 外来任务怎么跑。空 = 在本进程里就地跑（`--worker` 那条就是这样）。
///
/// **主程序要传一个"交给自己那几张卡"的。** 一个进程只能用一张卡
/// （CUDA_VISIBLE_DEVICES 在后端初始化时就读走了，跑起来改不了），所以
/// 主程序就地跑外来任务时，双卡机上永远只有一张卡在动——用户 2026-09-17：
/// 「只用了一张卡」。把它交给本机那个按卡拉起子进程的池，两张卡才都吃得到。
/// 形状和池那边的 LocalRunner 一样，直接复用，别造第二个同形状的类型。
using TaskRunner = LocalRunner;

/// 这台此刻能**同时**接几件。空 = 1。
///
/// **是回调不是数字**：它跟着执行器背后真正的能力走。主程序那边是"本机
/// 那几个按卡拉起的子进程里活着的有几个"——那个数在起服务后才知道、
/// 中途还会变（有一个挂了就少一）。2026-09-17 做砸的那次就是把它写死成
/// 卡数 2，而 farm 实际只拉起一个，接了两件却只有一个干得动，槽锁死、
/// 后面 51 次全 409。
using Capacity = std::function<std::size_t()>;

bool mount_worker_api(crow::SimpleApp& app, const config::Settings& settings,
                      const WorkerOptions& opts, TaskRunner runner = {},
                      Capacity capacity = {});

}  // namespace changji::infer
