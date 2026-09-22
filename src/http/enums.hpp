#pragma once

// 枚举值 → 中文。**给界面用的那一份，收在引擎里。**
//
// 在这之前这张表有两份：引擎里只有状态那一族（`models::status_zh`），景别、
// 运镜、机位三族的中文只在网页那边（`webapp/.../api/labels.js`）。桌面端一
// 来就会抄成第三份——而这个仓库已经为"同一件事在两处各写一遍"付过账：
// CLAUDE.md 第八条那张表上就有「阶段文案（引擎 kLabels / 前端 STAGE_LABELS）
// ——改了前端，预估行还是旧词」。
//
// 所以这儿摆一份，桌面端读它。**网页那份这一轮不动**（那套已经定稿），
// 它和这一份之间靠既有的跨语言用例钉着键；哪天网页也来读这一条，两份就能
// 合成一份。
//
// ⚠️ **值不是手打的。** 每一条的 `value` 都走 `models::to_string()`——
// 手打一个 "pan_lef" 出来，界面上那一栏就永远显示英文原文，而且不报错。

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

/// `{shot_size: [{value,label}], camera_angle: […], camera_move: […],
///   shot_status: […]}`
nlohmann::json enums_json();

/// GET /api/enums
ApiResult get_enums();

}  // namespace changji::http
