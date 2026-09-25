#pragma once

// 阶段 3 的编辑接口。
//
// 移植自 src/changji/web/server.py 里对应的 POST 路由。
//
// 这一层和只读层的区别：它会写盘，而且写之前要校验。校验不过必须
// **原样不动**——Python 那边是先 model_validate 出一个新对象、验过了
// 才逐字段赋回原对象，中途失败原对象一个字段都没被改。这个性质要保住，
// 否则一次失败的编辑会留下半改的镜头。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"  // ApiResult / ApiError / guard

namespace changji::http {

/// POST /api/shot —— 改一个镜头。
///
/// body: {project, episode_id, shot_id, patch: {...}}
///
/// 改了画面相关的字段就把状态退回未开工，否则下次运行会跳过它，
/// 用户会以为改动没生效。
///
/// 台词两种改法：`dialogue_texts`（逐条改字，条数必须和盘上对上）和
/// `dialogue_lines`（`[{char_id, text}]`，整镜换成这几句，条数随便，
/// `char_id` 空是旁白）。两个只能给一个。
ApiResult post_shot(const nlohmann::json& body);

/// POST /api/character —— 改角色。
/// body: {project, char_id, patch: {...}, reset_shots: bool = true}
///
/// 外观五段是一致性的锚点，改了会影响所有引用它的镜头，所以要连带
/// 把未锁定的镜头退回未开工。改音色之类的不触发。
ApiResult post_character(const nlohmann::json& body);

/// POST /api/character/add —— 加一个角色。
/// body: {project, name, identity, body, face, attire}
///
/// **为什么要有**：人物是读正文时一次提出来的，那一步只收主要人物和重要配角。
/// 有台词的小角色（哨兵、店员）不在名单上，写剧本时 speaker 只能挑名单里的人，
/// 他的话就挂到了别人头上（2026-09-25 实测：哨兵六句台词全记成了女主说的）。
/// 那时候名单已经定了，「理解故事」见各章剧本都在就不重跑——人手、场记手上都
/// 没有补一个人的路。
///
/// 名字重了是 409（同一个人登记两遍，镜头引用哪个说不清）。回包里 `char_id`
/// 是新的那一个，按名字生成（`c_` + slug，中文名走 slug 的散列退路）。
ApiResult post_character_add(const nlohmann::json& body);

/// POST /api/location —— 改场景。
/// body: {project, location_id, patch: {...}, reset_shots: bool = true}
ApiResult post_location(const nlohmann::json& body);

/// POST /api/style —— 改全片风格层。
/// body: {project, patch: {...}, reset_shots: bool = true}
ApiResult post_style(const nlohmann::json& body);

/// POST /api/shots/batch —— 批量改状态。
/// body: {project, episode_id, action, shot_ids: [] = 整章}
/// action 可选：reset / lock / unlock / clear_notes
ApiResult post_shots_batch(const nlohmann::json& body);

/// POST /api/shots/reorder —— 按给定顺序重排镜头。
/// body: {project, episode_id, shot_ids: [完整列表]}
///
/// 必须给完整列表，不接受增量指令——界面和引擎对当前顺序的理解一旦对不上，
/// 结果是把成片剪乱，而且要播一遍才发现。
ApiResult post_shots_reorder(const nlohmann::json& body);

/// POST /api/shots/link_locations —— 把 location_id 空着的镜头接回场景。
/// body: {project, episode_id: "" = 整个项目}
ApiResult post_shots_link_locations(const nlohmann::json& body);

}  // namespace changji::http
