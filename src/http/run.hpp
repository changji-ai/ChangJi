#pragma once

// POST /api/run —— 开跑。
//
// 走 job 表：立刻返回 {"started": true, "queue": [...]}，进度靠
// GET /api/run 轮询或者 WebSocket 推。一章跑几十分钟，同步返回没有意义。
//
// 队列是为量产准备的：all_episodes 一次把整个项目有分镜的章都排上。
// **一章出错不拖垮后面几章**——跑一晚上，早上发现第二章挂了导致后面十章
// 都没动，那这一晚上就白熬了。

#include <functional>

#include <nlohmann/json.hpp>

#include "infer/worker_server.hpp"
#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/episode.hpp"

namespace changji::http {

/// 开跑要用的外部东西。
///
/// 全都是回调而不是值，两个原因：一是**每次开跑现取配置**，用户改完
/// 模型文件不用重启（这条是有代价学来的，见 sd_image.hpp 里那段注释）；
/// 二是测试能塞假后端进来——真跑一章要几十分钟，而这一层要测的是
/// 队列、错误汇总和状态码，那些几毫秒就能测完。
struct RunDeps {
    std::function<config::Settings()> settings;
    std::function<models::HardwareProfile()> profile;
    /// 按当前配置造出图和出片的后端。
    ///
    /// 要 store 是因为 ComfyUI 那条路的工作流在**项目里**
    /// （workflows/video.json 覆盖内置的那份）。只给 settings 的话，
    /// "不同的电影用不同的模型"就没了。
    std::function<pipeline::Backends(const config::Settings&,
                                     const models::ProjectStore&)> backends;
    /// 还不能开工的话，为什么；空串 = 能。post_run 开跑前问一次。
    ///
    /// 默认那套问的是体检（doctor::run_checks，**认远程机器**：本机没
    /// 模型但机器表里有能干的，算能开工）。是回调不是直接调 doctor，
    /// 因为体检那一层链 httplib，进不了测试目标；测试里留空 = 不拦。
    std::function<std::string()> blocked;
};

/// 默认的那套：配置从 runtime 取，后端是 sd.cpp。
RunDeps default_run_deps();

/// 外来任务怎么在这台机器上跑（主程序挂节点协议时用），以及能同时接几件。
///
/// **一个进程只能用一张卡**——CUDA_VISIBLE_DEVICES 在后端初始化时就读走
/// 了，跑起来改不了。所以主程序就地跑外来任务时，双卡机上永远只有一张卡
/// 在动（用户 2026-09-17：「只用了一张卡」）。这一个把任务交给本机那个
/// 按卡拉起子进程的池，两张卡才都吃得到。
///
/// 2026-09-17 第一版做砸了两处，这一版照着改：
///   · **farm 在起服务时就后台预热**，不是第一件任务到了才拉。第一版让
///     任务线程堵在拉起和探活上（每个子进程最多两分钟），期间新来的全 409。
///   · **capacity 跟活着的子进程数走**，不是卡数。第一版写死成卡数 2，
///     而 farm 只拉起了一个，接了两件只有一个干得动，槽锁死。
///
/// farm 还没热好、或者单卡机上根本没有子进程时：就地跑、一次一件——
/// 和以前一模一样。**只用本机那几个子进程，不含别的机器**：外来的活再
/// 派出去会绕回来。
struct FarmRunner {
    infer::TaskRunner run;
    infer::Capacity capacity;
};
FarmRunner local_farm_runner(const config::Settings& settings);

ApiResult post_run(const nlohmann::json& body, const RunDeps& deps);

/// GET /api/run/preview —— 开跑之前先说清楚这一次会做什么、大概多久。
///
/// 以前只能按下开始再看，一按就是几十分钟。哪些镜头会重做、总共要等多久，
/// 这两件事应该在按下去之前就知道。
///
/// **一个镜头从它现在的状态开始，会一路走完后面所有阶段。** 只按当前状态
/// 归到一个阶段的话，会告诉人"配音 2 镜，粗估 16 秒"，而实际上那两镜还要
/// 出首帧、跑草稿档、跑成片档，得等十几分钟。报小了的预演比没有预演更糟。
/// `skip_draft` 默认真，和 `POST /api/run` 一致。**预览和实际必须是同一套
/// 默认**——不一致的话它报的是另一件事（"草稿 22 镜加成片 22 镜、1.8 小时"
/// 而实际只跑成片、Turbo 6 步），而用户按它安排时间。
/// `preview_s > 0`：只算**前 n 秒那几镜**的活。回包里多一块 `preview`，
/// 说清挑中的是哪几镜、算下来多长、够不够。
///
/// **这一条不加的话按钮上写的是整章的时间**——人按「只做前 2 分钟」，
/// 下面那行预估却报着「22 镜 · 约 1 小时 38 分」，而真跑的是 7 镜 18 分钟。
/// 预览和真按下去那一下必须说同一件事。
ApiResult get_run_preview(const std::string& path,
                          const std::string& episode_id, bool all_episodes,
                          bool skip_final, bool skip_draft, bool force,
                          const models::HardwareProfile& profile,
                          double preview_s = 0.0);

/// GET /api/run 那一份快照，外加**这一次问的人关心的两件事**：
///
/// · `mine` —— 正在跑的那一轮是不是 `project` 这部电影的。
///   **不判这一条的话，一部电影在出片，切到另一部电影的镜头页上照样显示
///   「停下」和进度**——2026-09-20 实测撞到：按下去停的是别人那一轮，而
///   屏幕上从头到尾说的是这一部。判在引擎这头是因为路径要规范化才比得对
///   （CLAUDE.md 第十一条，macOS 上 `/var` 是 `/private/var` 的软链）。
/// · `queue` —— 排着的那几件（`items` / `total` / 可能有 `error`），
///   每一件也带自己的 `mine`。
///
/// `project` 留空就不判，`mine` 一律为真（老客户端、命令行）。
ApiResult get_run_status(const std::string& project);

/// POST /api/run/queue/clear —— 把排着的全清了。回 {cleared: N}。
///
/// **点错了要能撤。** 没有这条出口的话，手滑排上去的那几件只能等它们一件件
/// 跑完——而那是几个钟头。
ApiResult post_run_queue_clear();

/// GET /api/outputs —— 列出已经出好的成片。审片时直接在界面里播。
ApiResult get_outputs(const std::string& path);

/// 把阶段名列表翻成枚举。认不出的抛 ApiError。
///
/// 单独暴露是因为**它抛的错不该变成 400**：Python 那边这个校验在
/// run_stages 里，而那是在任务线程上跑的，错误落进任务状态的 error 字段。
/// 变成 400 的话前端的表现完全不同——一个是弹错误框，一个是任务列表里
/// 显示一条失败记录。
std::vector<pipeline::Stage> parse_stages(
    const std::vector<std::string>& names);

}  // namespace changji::http
