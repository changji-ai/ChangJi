#pragma once

// 出角色圣经和分镜表的两个接口。
//
//   /api/bible —— 只出角色与场景设定，不碰分镜表
//   /api/plan  —— 从剧本一路出到分镜表
//
// 拆成两个入口是刻意的：引导式界面把角色、场景、分镜分成三步，
// 每一步都要能单独重来。只有 /api/plan 的话，想重出一次角色设定
// 就会把人工改过的整张分镜表一起冲掉。

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "models/character.hpp"
#include "models/project.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// POST /api/bible —— 出角色与场景设定。
///
/// 结果是**合并**进资产库，不是替换。角色和场景是全片共用的库，
/// 第五章的场景要出的时候前四章的还在里面；整个换掉的话那些场景
/// 连同它们的空景图一起没了，而分镜表里还留着指向它们的 id，
/// 跑起来直接报「场景未注册」。
///
/// 同名的默认保留旧的——手改过的设定、传过的参考图都挂在旧的那一份上。
/// POST /api/assets/dedupe —— 把同名的场景/角色收成一条，镜头引用跟着改。
/// 不叫模型，只收不出。定妆那条路顺手也做，这是给存量项目的快路。
ApiResult post_assets_dedupe(const nlohmann::json& body);

/// 把一份新出的人物 / 场景设定并进项目的资产库（同名的按 overwrite 决定
/// 顶不顶掉，顺手去重、把镜头的指向改过来）。定妆和理解故事共用。
ApiResult merge_assets(const models::ProjectStore& store,
                       const models::AssetLibrary& fresh, bool overwrite,
                       const char* source);

ApiResult post_bible(const nlohmann::json& body, llm::Client& client,
                     pipeline::CancelToken& tok);

/// POST /api/plan —— 从剧本出角色设定和分镜表。
ApiResult post_plan(const nlohmann::json& body, llm::Client& client,
                    pipeline::CancelToken& tok);

}  // namespace changji::http
