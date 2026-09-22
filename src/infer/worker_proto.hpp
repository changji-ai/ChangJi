#pragma once

// 工作进程的协议：一次任务发什么、回什么。
//
// **为什么要有工作进程。** 见方案「多卡和多机怎么用起来」那一节。一句话：
// sd.cpp 的进度回调是全局的（`sd_set_progress_callback` 不带 ctx），
// 同进程里两个生成会互相串取消和进度——**同种模型的多实例只能靠多进程**。
// 上游接口形状改不了。
//
// 顺带白送一条：**崩溃隔离**。今天 `sd_image` 那个 0xc0000094 是整个服务
// 进程没了，跑了一半的一章、WebSocket 连接、排队的任务全丢。切成工作进程
// 之后，协调者只看到"这一镜的连接断了"，记一次失败接着跑。
// 这一条单卡起一个工作进程就有，不需要多卡。
//
// ---
//
// 序列化单独放一层是为了**能测**：接上真模型之后一个用例要跑几分钟，
// 协议层的 bug 就没法反复撞了。这和 `scheduler.hpp` 把加载卸载做成
// 注入回调是同一个理由。

#include <map>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"
#include "models/shot.hpp"
#include "stages/prompt_compose.hpp"
#include "stages/render.hpp"

namespace changji::infer {

/// 一次任务要干的活。
///
/// **配音也在里头**，虽然它不吃显卡那么狠：那张「机器 × 能力」的表上
/// 配音是一列，能勾就得能派，否则界面在说一件做不到的事。
enum class TaskKind { Frame, Video, Tts };

const char* to_string(TaskKind k);
std::optional<TaskKind> task_kind_from(const std::string& s);

/// 派给工作进程的一次任务。
///
/// **输入是协调者算好的**：提示词已经拼完、档位已经算完、产物该落在哪
/// 也定了。工作进程不碰项目文件、不做判断、只算——
/// 这是"单一写者"那条线在进程边界上的体现。
struct Task {
    TaskKind kind = TaskKind::Frame;
    std::string shot_id;
    /// 出图用。出片时忽略。
    stages::PromptBundle prompts;
    models::TierSpec spec;
    /// 出片用。出图时忽略。
    int frames = 0;
    std::string motion;
    models::StyleLine style_line = models::StyleLine::REALISTIC;
    models::Tier tier = models::Tier::DRAFT;
    /// 首帧图。出片时可能有（图生视频），出图时没有。
    std::optional<std::string> start_image;
    /// 尾帧图（首尾帧那条路，FL2VA）。2026-09-16 之前这一项不在协议里：
    /// 填了 last_frame_prompt、尾帧也出来了，走工作进程池就退回单帧图生
    /// 视频，一声不吭——同一章换条执行路径结果不同。
    std::optional<std::string> end_image;
    /// 留不留模型自己出的声音。**跟着任务走**，不读工作进程那台的设置：
    /// 那样同一章里不同节点出的镜头一半有环境声一半没有。没带就按本机。
    std::optional<bool> keep_ambient;
    /// 产物落在哪。**绝对路径**——工作进程可能在别的工作目录里跑。
    ///
    /// `return_artifact` 为真时这一项**没有意义**：那时候对面写的是自己的
    /// 沙箱，产物由派活方拉回来落到自己这边。
    std::string dest;

    /// 产物别写 dest，写你自己的沙箱，然后把内容指纹回给我。
    ///
    /// **跨机时必须为真**：dest 是派活方的路径，对面根本没有那个目录。
    /// 同机（本机多卡）留假——同一个文件系统，让它直接写省一次搬运。
    bool return_artifact = false;
    /// 随机种子。协调者算好传下来，**不能让工作进程自己算**：
    /// 它不知道 attempts，算出来的图和串行跑的会不一样。
    std::int64_t seed = 0;

    /// 这部电影挑的档位：{组: 选项 id}，来自项目的 `[models.pick]`。
    ///
    /// **带 id，不带路径。** 别的机器的模型目录在别处、盘符都可能不一样，
    /// 而"这部电影要 h3-full-q4_k_m"这件事是跟着电影走的、跟机器无关。
    /// 那台拿到之后照这个 id 去**自己的**模型目录里找文件
    /// （`setup::with_selections`）。
    ///
    /// 空 = 这部电影没挑过（老项目），那时候每台按自己 `[models]` 里写的
    /// 文件名跑，和以前一模一样。
    std::map<std::string, std::string> pick;

    // ---- 配音专用。别的 kind 忽略 ----

    /// 要念的那句话。
    std::string text;
    /// 用哪个音色。空 = 让那台自己挑。
    std::string voice_id;
    /// 情绪标签和强度，照 `Synthesizer` 的签名传下去。
    std::string emotion;
    double intensity = 0.0;
};

nlohmann::json to_json(const Task& t);
/// 解析失败时抛 std::runtime_error，消息里说清缺什么。
Task task_from_json(const nlohmann::json& j);

/// 一次任务的结果。
struct TaskResult {
    bool ok = false;
    /// 失败时的原因。**要能直接给用户看**——它会变成事件流里那条 warn。
    std::string error;
    /// 产物的绝对路径。成功时才有意义。
    ///
    /// **跨机时这是对面沙箱里的路径**，对派活方没用，看 `artifact_id`。
    std::string dest;

    /// 配音出来多长（秒）。**只有配音任务有意义。**
    ///
    /// 为什么要回这个数而不是让派活方自己去量：配音先行那条线靠它反推
    /// 镜头时长，而量一次要么读 wav 头要么起 ffprobe——活是在那台跑的，
    /// 它顺手就知道，没道理让派活方再算一遍。
    double duration_s = 0.0;

    /// 产物的内容指纹。`Task::return_artifact` 为真时才有。
    ///
    /// 派活方拿它去 `GET /blob/<id>` 把产物取回来。**产物和输入走的是
    /// 同一个 blob 仓库**——重跑一镜出来字节相同的话，第二次连传都不用传。
    std::string artifact_id;
};

nlohmann::json to_json(const TaskResult& r);
TaskResult task_result_from_json(const nlohmann::json& j);

/// 工作进程报的进度。
struct TaskProgress {
    /// queued / running / done / failed
    std::string state = "queued";
    int step = 0;
    int steps = 0;
    /// 这一下报的是哪个阶段：prep / sample / decode，见 infer::Phase。
    std::string phase = "sample";
    /// 采样中途最新的那张预览（`data:image/png;base64,…`）和它是第几步的。
    /// **只在派活方问的时候带**（`GET /task/<id>?preview_after=N`，且比 N
    /// 新）：一张几十 KB，跨境公网 25 KB/s，每次轮询都带会把链路吃光。
    /// 没有就是 -1 / 空。
    int preview_step = -1;
    std::string preview;
    std::optional<TaskResult> result;
};

nlohmann::json to_json(const TaskProgress& p);
TaskProgress task_progress_from_json(const nlohmann::json& j);

/// 工作进程拒了任务（回的不是 200/202）时给用户看的那句话。
///
/// body 只带前 200 个**字符**，而且按字符截、不按字节：这句话会变成
/// 事件流里那条 warn，进任务快照，再序列化成 JSON。按字节截落在半个汉字上，
/// nlohmann 就在序列化那一步抛 type_error.316，整个快照接口回 500，
/// 进度全看不见——json_extract 那一处 2026-09-11 实跑就是这么炸的。
///
/// 放在这一层而不是 worker_pool.cpp 里，是因为那个文件链 httplib、
/// 进不了测试目标；这两句话的形状在这儿能测。
std::string worker_rejected_message(int status, const std::string& body);

/// 工作进程回了 2xx 但 body 不是 `{"id":...}` 时的那句话。截法同上。
std::string worker_bad_accept_message(const std::string& body);

// ---------------------------------------------------------------------------
// 抢位置：那台满了之后，派活这头怎么等
// ---------------------------------------------------------------------------
//
// 用户 2026-09-20：「任务结束了也要连接的全部机器广播，这样别的机器也可以
// 派任务，就看谁抢的快」。
//
// 那台放出一个位置时把**所有**挂在 `/slots/wait` 上的一起叫醒，谁先
// `POST /task` 上来谁拿到；没抢到的拿新的版本号再等一轮。下面两个是这条
// 协议里**纯判断**的那一半——`worker_pool.cpp` 链 httplib、进不了测试目标，
// 所以放这儿。

/// 那台回 409 时带的空位版本（`seq`）。
///
/// **取不到回 0**：0 的意思是"我不知道你现在是第几版"，而那头见到对不上的
/// 版本会立刻回——退化成一次短轮询，不会把这一镜卡住。老版本的工作进程
/// 回的 409 里没有这个字段，走的正是这一支。
std::uint64_t room_seq_of(const std::string& reject_body);

/// `/slots/wait` 回来之后：该去抢了吗。
///
/// 两种情况都算"该去抢"：
///   · `free > 0` —— 明摆着有位置；
///   · **版本变了** —— 这中间有人放过位置（哪怕这一刻又被别人占上了）。
///     不认这一条的话，两台机器互相抢的时候，慢的那台会一直等在一个
///     永远为 0 的 `free` 上。
///
/// `now_seq` 非空时写回那台报的最新版本，给下一轮用。答不出来（body 不是
/// JSON、缺字段）当成"没变"，由调用方按超时退回原来的节奏。
bool room_freed(const std::string& wait_body, std::uint64_t asked_seq,
                std::uint64_t* now_seq = nullptr);

// ---------------------------------------------------------------------------
// 派活的那台没影了，手上这件怎么办
// ---------------------------------------------------------------------------
//
// 用户 2026-09-20：「派活的这个机器派完后下线了，等任务完成都没上线，怎么
// 处理呢」。
//
// **槽是靠派活方回来收才放的**（`GET /task/<id>` 见到 done/failed 才删记录）。
// 它要是没了——进程重启、被人关掉、网线拔了——这条记录就永远占着一个位置，
// 而这台从此对谁都回 409。2026-09-17 实撞过：本机引擎重启几次，丢下几件
// 没人认领的活，之后一切派活都被拒。
//
// **派活方不会回来认领同一件。** 它手上那个任务 id 只活在那次调用的栈上，
// 进程一重启就没了；重启之后它是照着镜头状态重新派一件新的。所以"等它回来"
// 没有意义，该收就收。
/// 这件活的**内容指纹**：同样的输入必然回同样的东西。
///
/// 用户 2026-09-20：「任务做完了如果对方下线了应该放到一边保存接着做下一个
/// 事情，对方上线后自己来取」。
///
/// "自己来取"需要一把**对方重启之后还拿得出来的钥匙**。任务 id 是那台现编
/// 的、只活在派活方那次调用的栈上，进程一重启就没了——所以钥匙只能从**任务
/// 本身**算：同一镜、同样的提示词、同样的种子和档位，算出来就是同一把。
///
/// **`dest` 不算进去**：那是派活方本机的落盘路径，和产出的内容没关系；
/// 算进去的话，重启后换个临时目录就成了另一件活，白跑一遍。
/// `return_artifact` 同理——那是"怎么取"，不是"是什么"。
///
/// 种子、attempt 那些**要算**（它们本来就在任务里）：换了种子就是另一张图，
/// 拿上一次的顶替是错的。
std::string task_key(const Task& t);

enum class Orphan {
    Keep,        ///< 别动
    DropSettled, ///< 放在一边太久没人来取：扔掉那条记录（产物还在 blob 库里）
    CancelStuck, ///< 跑了太久，多半是卡住了：取消它，那张卡该还回来
};

/// 手上这件该怎么办。
///
/// **「派活的那头没影了」不是取消的理由。** 2026-09-20 实测撞到：派活那台
/// 被杀掉之后，那台 L20 上那一镜**已经采样了 125 秒**，眼看就出来了，却因为
/// 「两分钟没人来问」被取消——那 125 秒的卡时间白烧，而结果本来是留得住的。
///
/// 对的做法（用户当天的原话）：**做完了放到一边保存，接着做下一个事情，
/// 对方上线后自己来取。** 所以：
///
/// · `since_seen`（派活方多久没来问）**只管"放在一边"那一格**——跑完的
///   结果留一个钟头，没人来取才扔。它不占位置（算完那一刻位置就放出来了），
///   留着的代价只有一小块元数据。
/// · `running_for`（这件活跑了多久）管还在跑的那些。取消的理由只有一个：
///   **跑了太久，多半是卡住了**——那张卡再等下去也等不出东西。半小时是
///   照最慢那一档定的（成片档一镜几分钟，2K 长镜头最坏十几分钟）。
///
/// 两个数分开，是因为它们回答的是两个问题：一个是"这份结果还有人要吗"，
/// 一个是"这张卡还在干正经事吗"。混成一个的后果就是上面那 125 秒。
Orphan orphan_check(bool settled, std::chrono::seconds since_seen,
                    std::chrono::seconds running_for);

}  // namespace changji::infer
