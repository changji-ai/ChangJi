#pragma once

// 念一段字出来：/api/tts/say。
//
// 用户 2026-09-11 要的"编辑器右下角一排按钮：自动生成、对话修改、
// **语音阅读内容**"。前两件的口子都有了，这一件引擎里一直没有——
// `llama_tts` 那一层只在出片的配音那一步用，没有"把这段字念出来"的接口。
//
// **念的是选中的那一段**，不是整章。整章念完要好几分钟，而人在编辑器里想
// 听的通常是刚改过的那几句顺不顺口。所以这里有个字数上限，超了就念前面
// 那一段并在响应里说清楚——**不静默截断**：用户以为听完了整段，而实际
// 后半截根本没念，那比明说更糟。
//
// 后端按 `[tts].backend` 走，和出片那一步同一套（进程内 / 独立服务 /
// 估算）。退回估算后端时出来的是一段等长静音——那是配音阶段刻意的设计，
// 但在"念给我听"这件事上是没有意义的，所以这里会明说后端名字。

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

/// 一次最多念多少字。
///
/// Qwen3-TTS 一次合成的上限是 512 帧、12.5 Hz，约 41 秒；中文按每秒五个
/// 字算就是两百字出头。取 200 是让它落在上限里面一点——顶到上限时模型会
/// 把最后一句掐掉，而掐掉的那半句听起来像"念错了"。
inline constexpr int kSayMaxChars = 200;

/// POST /api/tts/say —— 把一段字念成音频。
///
/// body: {project, text, voice?}
/// 回: {rel, seconds, chars, truncated, backend}
///
/// `rel` 是项目目录里的相对路径，前端拿 /api/media 播它。
ApiResult post_tts_say(const nlohmann::json& body);

}  // namespace changji::http
