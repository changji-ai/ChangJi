#pragma once

// 故事大纲生成。故事分三层里的中间那层。
//
//     梗概（一两句）
//       ↓  这里
//     故事大纲（分章 + 人物关系 + 场景表）
//       ↓
//     章节正文（逐章展开）
//
// **中间这层是压缩的全局记忆。** 原来一章接一章往下写会失忆，不是因为
// 「一章一章写」，是因为没有一份压缩的全局状态可以每次都带上——只能带前
// 三章原文，带不下
// 就截断。有了大纲，写第二十章时第二章埋的伏笔照样在上下文里。
//
// 和 bible/storyboard 一样，这里**不碰网络也不碰 llama.cpp**，只有拼提示词、
// 给 schema、解析返回三个纯函数。怎么把提示词送给模型是调用方的事。

#include <stdexcept>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 大模型没按预期返回时抛这个。
class StoryError : public std::runtime_error {
public:
    /// `code` 是**闸门代号**：一个稳定的、不带具体数字的短名（"no_dialogue"、
    /// "repetition"）。`what()` 那句是给人看的，里面带着这一次的数和这一次的
    /// 词（「65 段里 0 段有人说话」），**每一次都不一样，分不了组**——而
    /// 「哪条闸门最费钱」正是一次 group by。两样各记各的，见
    /// `llm::GateVerdict`。
    ///
    /// 默认空串：不是闸门的那些（"没有这一章"、配置不对）留空，账上一眼能
    /// 和真闸门分开。
    explicit StoryError(const std::string& what, std::string code = {})
        : std::runtime_error(what), code_(std::move(code)) {}

    const std::string& code() const { return code_; }

private:
    std::string code_;
};

/// 请求里带的 JSON Schema，约束模型的输出结构。
///
/// **schema 里没有任何外观字段**，和分镜表是同一个手法：给了模型它就会忍不住
/// 写一遍长相，而那份长相和后面美术那一步出的必然有偏差。外观只存在于资产库。
const nlohmann::ordered_json& outline_schema();

/// 拼大纲提示词。
///
/// 章数由体量推（models::suggested_chapters），**不让用户自己填**——
/// 用户手里有的是「这个故事有多大」，不是「要写几章」。
///
/// `variation` 非零时往提示词里拼两段随机的底子（见 build_outline_names /
/// build_outline_spark）。**0 是不拼**，提示词回到老样子——语料和单测拿的
/// 就是这一档，否则每跑一次对不上一次。生产里由 HTTP 层传
/// `stages::random_shape()`。
/// `chapters` 非 0 时**就写这么多章**，不再按体量推：项目页那颗「一键成片」
/// 要的是一章。给 1 时提示词里会多一段把「多一两章少一两章都行」那条收回来
/// ——不收的话模型照着那条给出两三章，而那正是它被要求做的事。
std::string build_outline_prompt(const std::string& premise,
                                 models::StoryScale scale,
                                 models::StyleLine style_line,
                                 const std::string& keywords = "",
                                 std::uint32_t variation = 0,
                                 int chapters = 0);

/// 「这个故事里的人怎么取名」那一段：几个姓 + 一种名字的形状。
///
/// **这一段每条路都拼**，有没有梗概都拼。治的是用户那句「角色的名字也都
/// 差不多」——实测反复出现的是陈默、林晚、苏婉这一批，而名字和用户给的
/// 方向不冲突：他写的是故事，不是花名册。
///
/// 不列黑名单。写「不要用林晚」等于把林晚送进上下文，那是同一个病
/// （见 prompts.toml 开头「别在提示词里举具体的例子」）。给一池姓让它挑，
/// 是正向的约束。
///
/// variation = 0 时返回空串。
std::string build_outline_names(std::uint32_t variation);

/// 「这一次从下面这几样起手」那一段：场域 + 关系 + 压力 + 调子，各抽一个。
///
/// **只在既没梗概也没关键词时才该拼**（调用方负责判断）。用户给了方向的话
/// 再塞一组随机的场域是跟他对着干；而"什么都没有，你来一个"那条路是雷同
/// 最严重的一条——提示词里一个变量都没有，同一个按钮按十次，模型拿到的是
/// 同一串字节十次。
///
/// variation = 0 时返回空串。
std::string build_outline_spark(std::uint32_t variation);

/// 解析模型返回，产出 Story。
///
/// premise 和 scale 是调用方给的，模型不回这两项——它们是**输入**，
/// 让模型复述一遍只会让它改写用户写的那句话。
///
/// 章节 id 由这里生成（ch01、ch02…），不让模型取：模型给的 id 会重、会跳号、
/// 会带中文，而章节计划是按 id 指回章节的。
models::Story parse_outline(const std::string& raw, const std::string& premise,
                            models::StoryScale scale);

}  // namespace changji::stages
