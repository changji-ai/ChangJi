#pragma once

// 理解故事：一次调用，把拍片要的"读后感"全出来。
//
// 人物（谁、要什么、怎么变、**长什么样**）、关系、地点（哪儿、**什么样**）、
// 每一章的梗概和钩子、全片的调子——一份回答里全有。结构那半落进 story.json
// （解析同 story_analyze），长相那半并进 assets.json（解析同 bible）。
//
// **同一份回答喂两个解析器**，名单天然一致，不需要「一个不多一个不少」那道
// 闸；也没有 minItems / maxItems——故事里有两个人就是两个人。
//
// 提示词只有一句话（prompts.toml [story_understand]），理由写在那儿。

#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 结构 + 长相合在一起的 schema。从 analyze_schema 长出来：人物项多了
/// key / body / face / attire，地点项多了 key / space / lighting / palette，
/// 顶层多了 global_style；所有 minItems / maxItems 摘掉。
const nlohmann::ordered_json& understand_schema();

/// 一句话 + 章节 + 只输出 JSON。
std::string build_understand_prompt(const models::Story& story,
                                    models::StyleLine style_line);

}  // namespace changji::stages
