#pragma once

// 逐镜首帧生成。
//
// **这是角色一致性真正落地的一环。** 视频模型只负责三到五秒的运动，
// 跨镜头的长相、服装、场景全靠首帧锁死。
//
// 这一层不知道图是谁出的：后端是一个函数对象。sd.cpp 进程内出图是一种，
// 阶段 6 接回 ComfyUI 是另一种，测试里那个只写个假文件的也是一种。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "infer/exec_queue.hpp"
#include "infer/sd_image.hpp"
#include "models/character.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/shot_flow.hpp"
#include "stages/prompt_compose.hpp"

namespace changji::stages {

/// 首帧的种子。
///
/// **和 Python 不一样，这是有意的。** 那边是
/// `abs(hash("frame:" + shot_id)) % 2**31`，而 Python 的 str hash
/// 按进程随机化（PYTHONHASHSEED）——同一个镜头每次重启得到的种子都不同，
/// 也就是**每次重跑都是另一张图**。而用派生种子而不是随机种子，
/// 全部的意义就是可复现：重跑要么修好要么没修好，不能是"换了一张"。
///
/// 这里用 SHA-1 取前 4 字节，跨进程、跨机器、跨版本都一样。
/// attempts 的偏移照抄（每次重试换一个种子，否则重试等于白重试）。
std::int64_t frame_seed(const std::string& shot_id, int attempts);

/// 一个镜头的出图结果。
struct FrameOutcome {
    std::string shot_id;
    bool ok = false;
    std::string path;      ///< 相对项目根，失败时是空的
    std::string error;
    double elapsed_s = 0.0;
};

/// 出一张图。返回写到了哪儿。
///
/// 抽成回调是为了让这一层能不依赖任何推理后端跑起来——
/// 测试里塞一个写假文件的进去，整个循环的状态流转就能测死，
/// 而那部分恰恰是最容易错的（改了状态没存盘、失败了没记次数）。
using FrameRenderer = std::function<void(
    const models::Shot&, const PromptBundle&, const models::TierSpec&,
    const std::filesystem::path& dest, pipeline::CancelToken&,
    const infer::StepCallback&)>;

/// 用 sd.cpp 出图的那个 renderer。
///
/// 它自己不管模型加载——那是调度器的事。每次调用时向调度器借图像槽。
/// 出首帧的实现。**要 settings**：采样旋钮（image_cfg /
/// image_flow_shift）跟着请求走，而这一份是「这一章」的设置——建 SD
/// 上下文用的是全局那份，两者在出片那条路上不是同一个东西。
/// 见 infer::SamplingKnobs。
FrameRenderer sd_renderer(const config::Settings& settings);

/// 同上，但**种子由外面给**，顺带说明这活是谁派的。
///
/// 工作进程用这个：任务里带着协调者算好的种子。它自己算不了——
/// `frame_seed` 要 `attempts`，而工作进程拿不到那个数。
/// 用错种子出来的图和串行跑的不一样，**而且不会有任何报错**。
/// **两个参数是两边各加的，都要。** `settings` 那一半是采样旋钮跟着
/// 「这一章」走（见 sd_renderer 上面那段），`origin` 那一半是"这活谁派的"
/// ——工作进程接外来的活时标 Peer，本机自己那一章就排在它前面。
FrameRenderer sd_renderer_with_seed(const config::Settings& settings,
                                    std::int64_t seed,
                                    infer::Origin origin = infer::Origin::Local);

/// 给一批镜头出首帧。
///
/// **顺序生成，不并发。** 显卡只有一张，并发只会更慢，而且两个上下文
/// 同时占显存正是 6GB 卡上跑不动的原因。
///
/// shots 会被就地改：成功的置 FRAME_DONE 并记下 frame_path，
/// 失败的 attempts 加一。调用方负责存盘——这一层不碰 ProjectStore，
/// 因为"什么时候存"是流水线的决定（一镜一存还是整章一存）。
/// 出一批首帧。
///
/// `concurrency` 是同时在跑的镜头数。**1 就是原来的行为**（逐镜串行）。
/// 大于 1 时只并行**渲染**那一步——渲染器收的是 `const Shot&`，只读；
/// 写回 `frame_path` / `status` / `attempts` 一律等全部收完之后
/// 在调用线程上顺序做。这条守住，"单一写者"就不破。
///
/// 多卡时取工作进程数：出首帧那一层不知道有几张卡，
/// 但它知道池里有几个。
std::vector<FrameOutcome> run_frames(
    std::vector<models::Shot*>& shots,
    const models::AssetLibrary& assets,
    const models::TierSpec& spec,
    const models::ProjectPaths& paths,
    const FrameRenderer& render,
    pipeline::JobProgress& progress,
    pipeline::CancelToken& tok,
    int concurrency,
    /// 每出完一镜调一次（写回之后）。见 pipeline::ShotCommit。
    /// **不给就是老行为**：整批跑完再统一写回。
    const pipeline::ShotCommit& commit = {},
    /// 首帧和出片同时跑时两层之间的那根线（pipeline/shot_flow.hpp）。
    /// 给了就每写回一镜划一下；**给了就一定逐镜写回**（不然出片那层
    /// 拿到的是没有 frame_path 的旧样子），写回和存盘用它那把锁。
    /// 跑完由调用方 close()。
    pipeline::ShotFlow* flow = nullptr);

}  // namespace changji::stages
