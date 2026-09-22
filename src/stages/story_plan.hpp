#pragma once

// 章节计划：一章一条，就这一条。
//
// **不切章。** 用户 2026-09-16 定的：一章就是一条，多长由这一章自己的内容
// 定，整部电影是最后把各章接起来，不切。这儿原来是一套按容量找钩子把一章
// 切成上/中/下的算法；章节表那头（sync_episodes_to_chapters）当天就改成
// 一章一条了，章节计划这头却留着——同一个 ep01，章节表说是整章，章节计划说
// 是前三分之一，而剧本页的「原文」读的是章节计划：一章 1800 字只显示前一半
//（2026-09-18 用户撞到）。两份真相留一份。
//
// 和 bible/script/storyboard 一样，这里只有纯函数，不碰网络也不碰 llama.cpp。

#include <cstddef>
#include <string>
#include <vector>

#include "models/story.hpp"

namespace changji::stages {

/// 一秒成片大概消化多少字原文。
///
/// **这是个估算值。** 原文是小说体，里面的心理活动、环境铺陈在改成剧本时
/// 大半会被丢掉，所以一秒能消化的原文字数远多于能说出口的字数
/// （对白那个预算是 script.hpp 的 budget_chars，约 2.9 字/秒）。
///
/// 真实时长以配音为准——这是 README 里「配音先行」那条规矩。这个数只用来
/// 估一章值多长（章节表和章节计划记的都是它）和写正文时定一章的篇幅。
inline constexpr double kProseCharsPerSecond = 15.0;

/// 一章还没有正文时，估它值多长（秒）。
///
/// **只有这一个用处**：章还只有梗概、正文没展开时，章节表
///（sync_episodes_to_chapters）和章节计划（plan_episodes）要有个数可记。
/// 有正文就按字数估（kProseCharsPerSecond），轮不到它。
/// 它**不卡长度**——一章写多长、拍多长由这一章的内容定。
///
/// 2026-09-18 之前这个回落值是 `[assembly].episode_s`（一个用户配置项）。
/// 那一项的正业是「成片按每集 N 秒切」，切段随电影平台一起拔掉之后，
/// 剩下的只有这个回落，不值得再让用户配。
inline constexpr double kDefaultChapterS = 60.0;

/// 这个时长大概能消化多少字原文。写正文时定一章篇幅用它。
int prose_budget_chars(double duration_s);

/// `ch07` → `ep07`。认不出编号就按它在章节表里的位置排（从 1 起）。
///
/// 章节计划（plan_episodes）和章节表（sync_episodes_to_chapters）都用这一条，
/// 两边的 id 才对得上——以前章节计划的 id 是按切片发的，一章切两段就有两个，
/// 按 episode_id 查到的是隔壁章的半截。
std::string episode_id_for_chapter(const std::string& chapter_id,
                                   std::size_t index);

/// 一章一条。
///
/// 第 i 章一条：整章 [0, 正文长度)，标题就是章名，钩子取最后一场的 turn
///（没有场就取最后一条有说法的钩子），id 按 episode_id_for_chapter。
/// 时长：有正文按字数估（kProseCharsPerSecond），和章节表记的是同一个数；
/// 没正文按 `fallback_duration_s`——它 ≤ 0 时退回 story.episode_duration_s，
/// 再不行退回 kDefaultChapterS。
std::vector<models::EpisodePlan> plan_episodes(const models::Story& story,
                                               double fallback_duration_s);

}  // namespace changji::stages
