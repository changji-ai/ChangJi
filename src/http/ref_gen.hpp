#pragma once

// 生成参考图：/api/character/reference/generate、/api/location/reference/generate。
//
// 和 upload.hpp 那两个是**同一个槽位的两条来路**——一条是用户自己找图传上来，
// 一条是照着设定里那段外观描述现画一张。落点、旧图清理、以及"换了参考图就
// 全片重跑"这条语义，两条路完全一样，所以共用 claim_ref_path 和
// reset_all_shots。
//
// 这是**「设定」那一页上唯一该用 AI 的地方**（用户 2026-09-11 的判断）：
// 内容都在故事里创作完，设定这一步不创作新的人和地方，只把故事里的名单
// 翻成可画的描述、再照着那份描述生成参考图。
//
// **同步的，一次一张。** 一张图几十秒（头一张还要先把出图模型读进显存，
// 十几秒到一分钟），没有走任务表：任务表一种只有一个槽，参考图要是占了
// Run 那个槽，用户就没法一边出参考图一边跑别的——而这两件事在调度器那层
// 本来就会排队，再加一层槽只会让"为什么点不动"更难说清。

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"  // ApiResult / ApiError / guard
#include "models/project.hpp"
#include "stages/frames.hpp"  // FrameRenderer

namespace changji::http {

/// 画参考图用哪条出图后端。
///
/// **和出首帧同一条 FrameRenderer**——进程内的 sd.cpp，或者派给别的机器的
/// 工作进程池（run_deps.cpp 里搭的那套）。2026-09-16 之前参考图只会在
/// 本机进程内画，本机没有出图模型（比如 Mac 上配了一台 L20 干活）时
/// 「一键出图」整个不可用，而首帧却能派出去。server.cpp 启动时把
/// default_run_deps().backends(...).frame 装进来；测试塞假的；没装时
/// 退回进程内的 sd_renderer。
/// 一条出图后端，外加**它同时能接几件**。
///
/// 位置数和后端得一起给：只给后端的话，「一键出图」不知道该同时开几张，
/// 而那正是 2026-09-17 那条「一键出图也没用多 gpu」的根子。
struct RefBackend {
    stages::FrameRenderer render;
    /// 池里有几个位置。1 = 就地一张一张跑（单卡机、没搭池的测试）。
    int lanes = 1;
};

using RefRendererProvider = std::function<RefBackend(
    const config::Settings&, const models::ProjectStore&)>;
void set_ref_renderer(RefRendererProvider provider);

/// 这一张用哪个种子。
///
/// body 里给了 seed 就用它（折回 [0, 2^31) ），没给就按 stem 算一个**固定**
/// 值：同一个角色重出得到的还是那张图。随机的话，用户每点一次刷新就换一
/// 张脸——而他点刷新往往只是想确认刚才那张存下来了。真要换一张，界面自己
/// 送一个 seed 进来。
///
/// 导出只为了能测这条"固定"的规矩。
std::int64_t ref_seed(const nlohmann::json& body, const std::string& stem);

/// POST /api/character/reference/generate
///
/// body: {project, char_id, slot?（默认 front）, seed?}
///
/// seed 不给就按 char_id + slot 算一个固定值——同一个角色重出还是那张图。
/// 界面上的「换一张」要自己送一个随机 seed 进来，响应里回的就是这次用的。
ApiResult post_character_reference_generate(const nlohmann::json& body);

/// POST /api/location/reference/generate
///
/// body: {project, location_id, seed?}。场景只有一张空景图，没有 slot。
ApiResult post_location_reference_generate(const nlohmann::json& body);

// ---------------------------------------------------------------------------
// 「一键出图」：**队列在引擎这头**
// ---------------------------------------------------------------------------
//
// 用户 2026-09-17：「应该将所有图片放到队列里，然后一个一个分配才对」。
//
// 原来这一圈在浏览器里：页面问一遍池里有几个位置，照着开几条道，每条道
// 自己往下取下一张、各发各的 POST。三处不对：
//
//   1. **排队这件事没人记。** 页面手里只有"正在画的那几张"，说不出还排着
//      几张；两条道恰好都在同一个人身上时，那一行显示成「唐海、唐海」，
//      看着像卡住了（用户：「光作业中还显示同一个名字，排队被你吃了？」）。
//   2. **关掉页面就散了。** 队列活在那个标签页的闭包里。
//   3. **位置数是按下去那一刻的快照。** 中途多连一台机器不会多开一条道，
//      掉一台也不会缩。
//
// 收到引擎这头之后：一次请求交一整批，引擎按池里的位置数**一件一件派**，
// 派完一件补一件；页面只管订那条 `refs` 频道上的 `ref_queue`，上面写着
// 「正在画哪几张、还排着几张、已经好了几张」。
//
// POST /api/assets/references —— body {project, force?}
//   force 为真时连已有的一起重画。回 202 {total, started}；
//   一张都不缺时回 200 {total: 0}；同一个项目已经在跑时回 409。
ApiResult post_references_generate_all(const nlohmann::json& body);

/// POST /api/assets/references/stop —— body {project}。整批停下。
ApiResult post_references_generate_all_stop(const nlohmann::json& body);

/// GET /api/assets/references —— 这一批现在跑到哪儿了。
///
/// **刷新过页面的人靠它把进度接回来**：`ref_queue` 是广播，错过就错过。
ApiResult get_references_queue(const std::string& project);

}  // namespace changji::http
