#pragma once

// 把一个任务在本机跑完。
//
// **为什么要单独一层。** 这段原来埋在 `worker_server.cpp` 那条 `/task`
// 路由的 lambda 里，只有一个调用方所以无所谓。对等互联那条路上别的机器
// 派来的活是第二个调用方，抄一遍的话两边迟早走散——而走散的表现是
// **"同一个任务在两条路上出来的图不一样"**，没有哪一层会去比这个。
// `worker_pool.cpp` 里那条"出片要用 render_seed 不是 frame_seed"的注释
// 记的就是同一类事故：不报错、不变慢，只是产物悄悄不同。
//
// 这一层**不碰网络、不碰项目文件**：输入是派活方算好的（提示词拼完了、
// 档位算完了、种子给了、产物落哪儿定了），输出是一个结果结构。
// 谁把它接到 HTTP 上、谁把产物传回去，是上面那层的事。

#include <optional>
#include <string>

#include "config/settings.hpp"
#include "media/ffmpeg.hpp"
#include "stages/audio.hpp"
#include "infer/exec_queue.hpp"
#include "infer/sd_image.hpp"  // StepCallback
#include "infer/worker_proto.hpp"
#include "pipeline/jobs.hpp"

namespace changji::infer {

/// 按配置搭本机的配音后端。搭不起来回 nullopt。
///
/// **两个调用方**：跑一章时装进 `Backends`，以及别的机器派配音任务过来时。
/// 一份实现——两份的话，"这台配音到底走哪条路"迟早在两边不一样，
/// 而那种不一样的表现是同一章里前半段有声、后半段静音。
///
/// 三条路按 `[tts].backend` 选：`http`（外接服务）／`local`（进程内
/// llama.cpp）／别的都退回估算后端。**任何一条搭不起来都退回估算，不抛**：
/// 配音只是五个阶段之一，为它整条流水线跑不起来不值当，而估算后端会写出
/// 等长静音，画面那几步照样能验。
std::optional<stages::TTSBackend> make_tts_backend(
    const config::Settings& s, const std::optional<media::FFmpeg>& ff);

/// 接任务之前先看这活干不干得成。**干得成回空串。**
///
/// **这条是实机烧出来的**：第一次跑出片，扩散 8 步全跑完，到最后编码
/// 那一步才报"找不到 ffmpeg"。本机上 doctor 会在起跑前拦，但直接派任务
/// 进来绕过了那道检查。八张卡上，"跑几十秒再失败"乘以八就是几分钟白烧。
///
/// `base` 是**这台自己**的配置。`Task::pick` 里那几档会先盖上去再判——
/// 和 `run_task_locally` 盖的是同一份，两处不能各覆各的。
std::string cannot_do(const Task& t, const config::Settings& base);

/// 在本机把一个任务跑完。**同步**，跑完才返回。
///
/// **不抛异常**：失败写进 `TaskResult::error`。那句话会一路变成派活方
/// 事件流里的一条 warn，所以要能直接给用户看。
///
/// `origin` 决定它在本机的执行位上排在哪一档（见 `exec_queue.hpp`）：
/// 本机自己拉起的工作进程传 `Local`，别的机器派来的传 `Peer`。
/// `task_id` 只用来给沙箱起名（`<cache>/tasks/<id>`），
/// `Task::return_artifact` 为假时用不上。
///
/// `base` 是**这台自己**的配置：模型目录、ffmpeg 在哪、显卡几张。
/// 这部电影挑的档位由 `Task::pick` 带过来，在这儿盖上去（只盖模型那几个
/// 文件名，见 `setup::with_selections`）。**不落盘**——下一趟可能是另一部电影。
TaskResult run_task_locally(const Task& t, const config::Settings& base,
                            Origin origin, const std::string& task_id,
                            const StepCallback& on_step,
                            pipeline::CancelToken& tok);

}  // namespace changji::infer
