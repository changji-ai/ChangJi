#pragma once

// 一件长活干完了，怎么把结果送回界面。
//
// **接口当场回一句"开始了"，结果走这条 WebSocket。** 为什么非这么办：
// Crow 一条 I/O 线程管着一批连接，请求在它上面占多久，落在同一条线程上的
// 连接就干等多久——写一章一两分钟，出一张参考图几十秒。实测那期间别的
// 请求会卡满二十多秒，而顶栏那块表更惨：它是长连接，认准了一条线程，
// 一旦落在被占住那条上，会冻到这件活干完。
//
// ⚠️ 试过另一条路——活挪到后台线程、干完在那条线程上 `res.end()`
// （Crow README 里的异步写法）——**不行，别再试**。详见 server.hpp 里
// concurrency 那段。响应必须由 Crow 自己那条线程发出，所以它只能当场
// 回"开始了"。
//
// 三种消息，够了：
//
//     job_progress   干到哪儿了（有进度才发，比如出图的采样步数）
//     job_done       干完了，带整份结果——界面拿它换掉手上那份
//     job_error      砸了，带一句人话
//
// **done 里带整份结果，而不是让界面自己再拉一次。** 多一个来回本身不算
// 什么，但那一步正好是用户等得最久的一步，"写完了"和"看到"之间再插一段
// 空白很难受。

#include <cstddef>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "pipeline/jobs.hpp"

namespace changji::http {

/// 干完了。`result` 就是这个接口同步跑时会回的那份 body。
void job_done(const std::string& stream_id, nlohmann::json result);

/// 砸了。`message` 直接显示给用户看。
void job_error(const std::string& stream_id, const std::string& message);

/// 干到哪儿了。`total` 为 0 表示"不知道一共几步"，界面就不画进度条。
void job_progress(const std::string& stream_id, int current, int total,
                  const std::string& message = "");

/// 模型"先想再写"的那一段，边想边推。
///
/// **每一步都该有这个**——写大纲、写正文、写剧本、拆分镜。现在的模型都要
/// 思考，而思考少则十几秒、多则十几分钟：不推的话用户面对的是一个一动不动
/// 的进度条，分不清是在想还是卡死了。
///
/// 只推增量，不推累计：一段思考能有几千字，每次重发全文的话这条通道就
/// 被它占满了。攒成整段是界面的事。
void job_thinking(const std::string& stream_id, const std::string& piece);

/// 这条后台线程正在给哪条 stream 干活。没有就是空串。
///
/// **为什么要有它。** 思考流要接到每一步上（写大纲、写正文、写剧本、
/// 拆分镜），而那些处理函数散在五个文件里、十五个调用点。让每一个都自己
/// 从请求体里把 `stream` 捞出来的话，还得挨个去改它们的 `forbid_extra`
/// 白名单——十五处里漏一处的表现是"那一步没有思考显示"，而且不报错。
///
/// 后台那条活本来就是一个线程干一件事（见 start_async），所以"当前这条
/// 线程在给谁干活"是个准确的说法，不是投机取巧。
std::string current_stream();

/// 进后台线程时挂上，出去时摘掉。**一定要用 RAII**：中途抛异常的路径
/// 有好几条，手动清的话总有一条会漏，而漏掉的后果是下一件活把思考推到
/// 上一件活的频道上。
///
/// 它顺带持有**这件活的取消令牌**，并按 stream_id 登记进一张表——
/// 这样界面上那个「停下」才有东西可以按。见 cancel_job。
class JobScope {
public:
    /// 自己开一个令牌。**同步那条路和 `start_async` 用这个**：那几条活
    /// 读的就是 `current_cancel()`，登记自带的那个正好对上。
    explicit JobScope(std::string stream_id);

    /// **借外面那个令牌。批量那几条必须用这个。**
    ///
    /// 批量（写全片、展开正文、批量补分镜）的形状不一样：活跑在 JobTable
    /// 起的那条 job 上，循环查的是 `JobProgress::cancelled()`、给大模型的
    /// 是 `JobProgress::token()`——**那才是真正管事的令牌**。而 JobScope
    /// 挂上去只是为了让思考流和顶栏那个「停下」找得到它。
    ///
    /// 用上面那个构造函数的话，两者是两个不相干的令牌：`cancel_job` 点亮
    /// JobScope 自带的那个，循环和大模型读的却是 job 的那个——接口照回
    /// `{"stopped": true}`，而活一秒没停。而顶栏那块「AI 作业中」是从设定页
    /// 点完「批量补分镜」之后**唯一**看得见的出口（那一页自己的提示就写着
    /// 去那儿看进度），按下去没反应就是"点了运行之后取消不掉"。
    ///
    /// `token` 要活得比这个作用域长——批量那几处传的是 `p.token()`，
    /// 它属于这条 job，生命周期本来就包着整件活。
    JobScope(std::string stream_id, pipeline::CancelToken& token);

    ~JobScope();
    JobScope(const JobScope&) = delete;
    JobScope& operator=(const JobScope&) = delete;

private:
    void enter();   ///< 两个构造函数共用的那一段

    std::string id_;
    std::string prev_;                    ///< 上一层的 stream id
    pipeline::CancelToken* prev_token_;   ///< 同一个 id 上一层登记的那个
    pipeline::CancelToken* prev_cancel_;  ///< 上一层这条线程的令牌
    pipeline::CancelToken token_;         ///< 自带的那个；借外面的时候不用它
    pipeline::CancelToken* use_;          ///< 真正登记、真正给 current_cancel 的
};

/// 这条后台线程这件活的取消令牌。
///
/// **没有 JobScope 时返回一个哑元**（同步那条路、单测直接调）：那儿没人
/// 能按停，给个永远不会被触发的令牌比让调用方各自 new 一个干净。
pipeline::CancelToken& current_cancel();

// ---------------------------------------------------------------------------
// 没有 WebSocket 的时候：同一批消息，改成拿 HTTP 来取
// ---------------------------------------------------------------------------
//
// 上面整套是按"界面连得上那条 socket"写的。连不上的场合是真实存在的——
// 公司代理把 Upgrade 掐了、页面刚打开还没握完手——原来那条退路是**不带
// async 发过去，让 HTTP 一直等到干完**。拿得到结果，但那几分钟里：
//
//     进度   没有（job_progress 播出去没人听）
//     思考   没有（同上，而这恰恰是最需要看见的那一步）
//     停下   没有（顶栏那块徽标是 thinking.start 才出现的，没有它就没有
//            按钮；而且同步那条路上 JobScope 压根没挂，cancel_job 找不到）
//
// 三样一起没有，所以单补取消也没用：没有按钮可按。
//
// 现在改成：**socket 连不上就开个信箱，照样异步跑，消息从 HTTP 取走。**
// 界面先 `mail_open`，再发请求（带 stream + async），然后一秒拉一次
// `GET /api/job/events`。进度、思考、结果全都回来了，而后台那条活是
// `start_async` 起的，JobScope 挂着，顶栏那个「停下」照样按得动。
//
// **只给开过信箱的那条 stream 存。** 不然每一件走 socket 的活都白攒一份，
// 而思考流一段能有上万字。
//
// **不存 job_preview。** 一张几十 KB、一步一张，攒起来这个信箱就成了
// 主要流量——理由和 job_preview 头上那句"只广播，不留底"是同一条。
// 退到轮询时没有半成品小图，进度条照旧走。

/// 播一条自定义消息出去，**并且存一份进信箱**。
///
/// 上面那四个（progress/thinking/done/error）是所有活共用的；有几步还有
/// 自己的消息，形状不一样：
///
///     outline_progress   到此为止解出来的那份大纲（**整份**，不是增量）
///     story_token        正文/改稿逐字推出来的那一段
///     story_error        流到一半砸了，编辑器据此把半截字撤掉
///
/// 它们原来是直接 `ws::hub().broadcast` 的——也就是说没有 socket 的时候
/// 一个字都到不了。走这条就跟着进信箱了。
///
/// `msg` 要自带 `type` 和 `job_id`，和 broadcast 的规矩一样。
///
/// ⚠️ **增量那种（story_token）攒爆了会从前面丢**，界面上表现为那段字
/// 缺了个开头。不要紧：`job_done` 带的才是权威的那一份，界面拿它换掉手上
/// 那份。整份那种（outline_progress）更省事——丢掉旧帧本来就无所谓。
void job_relay(const std::string& stream_id, nlohmann::json msg);

/// 开一个信箱。已经有了就只是把它的过期时间往后推。
///
/// **要在发请求之前开。** 反过来的话，开之前那几条（第一段思考往往就在
/// 那几百毫秒里）没地方存，界面上表现为"前面一截思考不见了"。
void mail_open(const std::string& stream_id);

/// 取走 `since` 之后的消息。
///
/// 回 `{"events": [...], "next": n, "done": bool, "dropped": n, "exists": bool}`。
/// `exists` 为假表示这个信箱没了——没开过、或者太久没人来取被扫掉了；
/// 界面该把它当成"连接断了"，而不是"干完了"。
///
/// 取到 `job_done` / `job_error` 之后这个信箱就销号：结果只送一次，
/// 再来取就是 `exists=false`。
nlohmann::json mail_take(const std::string& stream_id, std::size_t since);

/// 信箱里攒多少条就开始从前面丢。思考是一段一条推的，几千条很正常。
inline constexpr std::size_t kMailMaxEvents = 4000;
/// 信箱里攒多少字节就开始从前面丢。
inline constexpr std::size_t kMailMaxBytes = 512 * 1024;
/// 多久没人来取就当人已经走了，把信箱扫掉。
inline constexpr int kMailIdleSeconds = 180;

/// 按 stream_id 把那件活停掉。找不到（早干完了、id 写错了）返回 false。
///
/// **找不到不是错。** 界面上那个按钮是无条件可点的：用户按下去的那一刻
/// 活可能刚好干完，重复点也不该弹错误框。
bool cancel_job(const std::string& stream_id);

/// 给 `llm::Request::on_thinking` 用的那个回调，已经接好这条通道。
///
/// 不给 `stream_id` 就用 `current_stream()`——绝大多数调用点该用这个。
/// 空 stream 时返回一个空的 function，同步那条路什么都不发生。
std::function<void(const std::string&)> thinking_sink(std::string stream_id = {});

/// 采样到一半的那张小图（`data:image/png;base64,…`）。
///
/// **只广播，不留底。** 一张几十 KB，而这条通道两秒还要推一次系统表；
/// 存起来或者补发都会把它变成主要流量。错过就错过——下一步马上又有一张。
void job_preview(const std::string& stream_id, int step, std::string data_url);

// ---------------------------------------------------------------------------
// 出参考图：一条名字固定的频道
// ---------------------------------------------------------------------------
//
// 用户 2026-09-12：「我刷新了这个页面，正在生成的图就不会实时更新」。
//
// **stream id 是点一下的时候浏览器随机生成的**（`ref-a3f9b1c2`），刷新之后
// 那串字就没了——页面回来时既不知道有活在跑，也没法订上它。而这一族活是
// 几十秒一张、一排十几张，刷新期间正好在跑是常态，不是边角情况。
//
// 所以除了各自那条 stream，再往一条**名字固定**的频道上播一份，
// 每条带上 `target`（`char_id_slot` 或者 `location_id_empty`）说这是哪一格。
// 页面一进来就订它，不用知道任何 id。
inline constexpr const char* kRefChannel = "refs";

void ref_progress(const std::string& target, int current, int total);
void ref_preview(const std::string& target, int step, std::string data_url);
/// 画完了。界面据此重新拉一遍那张图。
void ref_done(const std::string& target);
void ref_error(const std::string& target, const std::string& message);

/// 整批出图排到哪儿了：`{project, total, done, failed, queued, running:[…]}`。
///
/// **和上面那几条不一样，这条不带 target**——它说的是整批，不是某一格。
/// 页面上那一行「正在画 X、Y，还排着 N 张」就是照它画的。
void ref_queue(nlohmann::json state);

}  // namespace changji::http
