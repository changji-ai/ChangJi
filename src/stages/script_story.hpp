#pragma once

// 从故事写一章的剧本。
//
// 和 script.hpp 里那条「正片」路线的分水岭：
//
//   老的  给一句梗概 + 前三章原文，让模型自己想这一章该发生什么
//   新的  这一章要发生什么已经定好了（章节计划指着故事的一段），
//         模型只负责把那一段变成能拍的拍子
//
// **失忆是在这里治好的。** 老路线带的上下文是前三章的原文，取三章、截
// 4000 字符——写第五章时第一章已经不在上下文里了，伏笔全靠运气。这里带的
// 是压缩的全局记忆：大纲一句、人物表、关系、以及这一章之前每一章一句的
// 前情提要。压缩之后二十章也塞得下。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <cstdint>
#include <string>
#include <vector>

#include "models/character.hpp"
#include "models/story.hpp"
#include "stages/script.hpp"   // ScenePlan

namespace changji::stages {

/// 取章节计划里这一条覆盖的那段正文。
///
/// 按它的 [from, to) 区间在章节之间切。**按 UTF-8 字符切，不按字节**——
/// 按字节切会把汉字劈成三段，截出来的是非法 UTF-8，一路流到提示词和字幕。
///
/// 章节还没展开正文时返回空串，调用方退回用 summary。
std::string episode_text(const models::Story& story,
                         const models::EpisodePlan& plan);

/// 这一条覆盖了哪几章（按顺序，去重）。
std::vector<std::string> episode_chapters(const models::Story& story,
                                          const models::EpisodePlan& plan);

/// 这一条压着哪几场戏。
///
/// 章节计划一章一条、盖的是整章，所以正常情况下它压着这一章的全部场；人
/// 手改过区间时会只压住其中几场。拿它给写剧本那一步交代**这一章在哪、
/// 跟着谁走、他要什么、谁拦着**——正文里这些是化在叙述里的，单独列出来
/// 模型才不会把地点写丢（每一镜都要照着地点画）。
std::vector<models::Scene> episode_scenes(const models::Story& story,
                                          const models::EpisodePlan& plan);

/// 渲染给模型看的那一段：底子、人物、关系、前情提要、这一章、停在哪。
///
/// 单独拆出来是为了能测——拼提示词只是在它前后接常量，真正会错的是这里：
/// 把还没发生的事写进前情（模型会当成已经发生的写）、把这一章的正文
/// 漏掉、截断截在半个字上。
///
/// previous_tail 是上一章剧本的结尾几行，用来接语气；空着也行。
///
/// `chapter_scenes` 是这一章的场次清单（`chapter_scene_plan` 的产物）：一场
/// 一行，每行带这一场的拍数地板，发过去的顺序就是 JSON 里 scenes 该有的顺序。
/// 必填——没有场的章由 `chapter_scene_plan` 兜成「整章一场」，交不出清单的
/// 情况不存在。
std::string render_script_context(const models::Story& story,
                                  const models::EpisodePlan& plan,
                                  const std::string& previous_tail,
                                  const std::vector<ScenePlan>& chapter_scenes);

/// 给这一章配一条章节计划——整章，从第一个字到最后一个字。
///
/// **不从 story.plan 里按 episode_id 查。** 那张表的 id 以前是按切片发的
/// （一章切两段就有两条），而这里的 id 是从章号推的（ch07 → ep07），
/// 两套编号一错位，写出来的就是隔壁章的半截正文（2026-09-16 实撞）。
/// 钩子取最后一场的 turn，没有场就取最后一条有说法的钩子；都没有留空，
/// 提示词那头会写「停在最后一场的落点」。
models::EpisodePlan chapter_plan(const models::Story& story,
                                 const std::string& chapter_id,
                                 double target_duration_s);

/// 各场篇幅 → 每场的拍数地板。正文没有场（粘贴导入、大纲阶段）
/// 时整章当一场。where 那一行是给提示词看的：在哪、跟着谁、要什么、
/// 谁拦着、收在哪。
std::vector<ScenePlan> chapter_scene_plan(const models::Story& story,
                                          const models::EpisodePlan& plan);

/// 正文超过 limit 个字时掐中间：留头六成、尾四成，中间标「……（中间略）……」。
/// 结尾是钩子所在，截头的话模型就不知道这一章该停在哪。
std::string truncate_middle(const std::string& body, std::size_t limit);

/// 照着一章写剧本的提示词：**没有秒数、没有字数、
/// 没有四段**，这一章写多长由它的内容定，剧本照着正文的场走。`scenes`
/// 要和出 schema（script_schema_for_chapter）、解析（parse_chapter_script）
/// 用同一份——三处的场数和地板才对得上。
std::string build_chapter_script_prompt(
    const models::Story& story, const models::EpisodePlan& plan,
    models::StyleLine style_line, const std::vector<std::string>& characters,
    const std::string& previous_tail, const std::vector<ScenePlan>& scenes);

/// 取一段剧本的结尾，给下一章接语气用。按字符截，不按字节。
std::string script_tail(const std::string& script);

}  // namespace changji::stages
