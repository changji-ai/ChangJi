#pragma once

// 读一遍现成的正文，把结构提出来。
//
// 粘贴导入（story_import）只切章节，切完的故事里人物、关系、地点全是空的，
// 走到「设定」那一步资产库还是空的，再往下分镜就指不到任何角色。这一步把
// 那个洞补上。
//
// 和 story_outline 的分工：outline 是**无中生有**（从一句梗概编出整个
// 故事），analyze 是**读已经有的**。两件事不能合成一个提示词——让「写故事」
// 那份去读现成的正文，它会忍不住改写，而后面每一章都是照着正文展开的。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 把章节渲染成节选：每章标题 + 开头一段 + 「……」+ 结尾一段。
///
/// 整本小说塞不进上下文。开头交代这一章从哪儿接上，结尾决定钩子在哪，
/// 中间的过程靠这两头能推个八九不离十。章多的时候每章分到的字数更少，
/// 但**每章都在**——漏掉一整章比每章少几百字糟糕得多。
std::string render_chapters_for_analysis(const models::Story& story);

/// 请求里带的 JSON Schema。
///
/// 和大纲那份的差别：这里的 chapters 带 chapter_id（要映射回去）和
/// hook_after（钩子前面那句原文，程序靠它在正文里定位），而且**没有 title**——
/// 章名是作者自己写的，不该让模型改。
const nlohmann::ordered_json& analyze_schema();

std::string build_analyze_prompt(const models::Story& story,
                                 models::StyleLine style_line);

/// 把模型读出来的东西并回故事里。
///
/// **正文、章名、章节 id 一个字不动。** 这一步只补 summary、hook 和全片的
/// 人物/关系/地点——改正文就等于把用户粘进来的东西换掉了。
///
/// hook 的位置靠 hook_after 在正文里查：查得到就在那句话之后挂上说法，
/// **查不到就把那一条丢掉**（老形状的单个 hook 例外，它兜底挂章尾）。
/// story_reverse 机械登记的那些段落边界候选**留着**：它们是只有位置、
/// 没有说法的一批，读出来的说法位置正好对上就填进去，对不上才新加一条。
///
/// `overwrite` 就是「理解故事」上那个勾，**和另外两步、和设定库那半是同
/// 一条规矩**：
///
///   不勾 = 只补不顶。已经填着的那一栏一个字不动，空的才填。
///   勾上 = 全部重出。
///
/// 2026-09-19 之前这一步只有"重出"一档：写了第二章再点一次，第一章的梗概、
/// 出场人、地点、钩子全被这一趟的结果盖掉——而人按下去只是想让它读一下
/// 新写的那章。理由和 9954f00 一样：**可预测比聪明重要**，按下去之前说得
/// 出会动哪几章。
models::Story apply_analysis(const models::Story& story, const std::string& raw,
                             bool overwrite);

}  // namespace changji::stages
