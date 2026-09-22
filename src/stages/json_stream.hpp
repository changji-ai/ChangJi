#pragma once

// 从正在生成的 JSON 里，边生边把某一个字段的字符串值抠出来。
//
// **为什么要这么个东西**：用户要"AI 生成在编辑器里流式插入"。改一小段话
// 可以让模型直接吐大白话（见 story_revise 的 plain 那条），但**写一整章
// 不行**——那一步除了正文还要模型标出"这一章里哪几个地方悬着"
// （hooks），而那些钩子是一章停在真悬念上的全部依据：实跑里它把
// "停在真悬念上"的比例从 25% 抬到 56%。为了能流式就把 hooks 砍掉，等于拿
// 章尾的质量换一个动画。
//
// 所以正文那一步照旧约束成 JSON，只是在 token 流上**顺手把 text 那个字段
// 解出来**推给编辑器。JSON 外壳（`{"text":"`、结尾那串 hooks）不推。
//
// 纯状态机，不碰网络也不碰模型。**单独成文件是为了能测**：它要处理转义、
// \uXXXX、以及"一个转义序列被切在两个 token 中间"——最后这条是流式独有的，
// 攒齐了再解析的代码永远碰不到，而它错了的表现是正文里凭空多出几个
// 反斜杠。

#include <cstdint>
#include <string>

namespace changji::stages {

/// 从 JSON token 流里抠一个顶层字段：**一个字符串，或者一串字符串**。
///
/// 用法：每来一段 token 就 feed 一次，返回**这一次新解出来的字符**（可能
/// 是空串）。字段收完了 done() 变真，之后 feed 一律返回空。
///
/// 只认**顶层对象的第一层**键。嵌套对象里的同名键不认——这一层不需要，
/// 而认了就得维护一个真正的 JSON 栈。
///
/// ⚠️ **数组那条的分隔符必须和落库那边拼得一模一样。** 章节正文的 schema
/// 是 `{"paragraphs": [...]}`，`parse_chapter` 按 `\n` 把它们拼回去；
/// 这里要是拼成别的，边看边写的和最后落库的就对不上——而人是照着流出来
/// 那一版判断"写得行不行"的。见 `kArraySeparator`。
class JsonFieldStreamer {
public:
    /// 数组那条里，两项之间补什么。
    ///
    /// **和 `stages::parse_chapter` 拼 paragraphs 用的那个字符一致。**
    /// 两处各写各的话，编辑器里看到的段距和存下来的段距不一样——而那种
    /// 不一致没人会想到去查流式这一层。
    static constexpr const char* kArraySeparator = "\n";

    /// `repeating` = 这个键在一份 JSON 里**出现好几次**，每一次都要收。
    ///
    /// 章节正文 2026-09-12 改成了 `scenes[].paragraphs`——一场一个数组。
    /// 不开这个开关的话，收完第一场那个 `]` 就 done() 了，编辑器里只看得到
    /// 三分之一，而**后端不报任何错**（正文照样解析、落库、重算章节计划）。
    /// 这和 c41821d 那次「流式一个字都抠不出来」是同一类故障，那次查了很久。
    ///
    /// 开着的时候 done() 永远是假：谁也不知道后面还有没有下一场，
    /// 收到哪儿算完由调用方（生成结束）说了算。
    explicit JsonFieldStreamer(std::string field, bool repeating = false);

    /// 喂一段原始文本，拿回新解出来的字符。
    ///
    /// **返回的永远是完整的 UTF-8**：一个汉字可能被切在两个 token 中间，
    /// 那半个字符会被扣在这里，等下一段来了再一起吐。不扣的话广播那一层
    /// `json{{"text", fresh}}.dump()` 当场抛 `incomplete UTF-8 string`，
    /// 而那个异常会把整章写作打断——2026-09-12 实跑就这么掉了一章。
    std::string feed(const std::string& piece);

    /// 这个字段的值收完了没有。
    bool done() const { return state_ == State::Done; }

    /// 到目前为止解出来的全部。收尾时可以拿它和权威那份对一下。
    const std::string& text() const { return out_; }

private:
    enum class State {
        SeekKey,    ///< 在找下一个 `"`（可能是键的开头）
        InKey,      ///< 正在读一个键名
        AfterKey,   ///< 键名读完了，等冒号
        SeekValue,  ///< 等值那个 `"` 或者 `[`
        SeekItem,   ///< 数组里，等下一项的 `"` 或者收尾的 `]`
        InValue,    ///< 正在读值，这一段才往外吐
        Escape,     ///< 值里遇到了 `\`
        Unicode,    ///< 正在收 \uXXXX 的四位
        Done,
    };

    std::string field_;
    State state_ = State::SeekKey;
    std::string key_;      ///< 正在读的键名
    std::string out_;      ///< 已经解出来的全部
    std::string hex_;      ///< \uXXXX 收到一半的那几位
    std::uint16_t high_ = 0;  ///< 代理对的高位，等低位来配
    bool key_escape_ = false;  ///< 键名里的转义。键名一般没有，但不能崩
    bool array_ = false;       ///< 这个字段是一串字符串，不是一个字符串
    bool repeating_ = false;   ///< 这个键会出现好几次，每一次都收
    std::string tail_;         ///< 结尾那半个 UTF-8 字符，扣着等下一段
    bool wrote_any_ = false;   ///< 已经吐过至少一项（决定要不要先补分隔符）
};

}  // namespace changji::stages
