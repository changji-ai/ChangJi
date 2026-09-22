#pragma once

// 把**还没写完**的一段 JSON 补齐成能解析的一份。
//
// 干什么用：写大纲是一次约束成 JSON 的生成（premise、人物、地点、章节表
// 全在一个对象里），一次要三四十秒。攒齐了再蹦出来的话，那几十秒界面上
// 一个字都没有——而那正是用户要看的"它在想什么"。
//
// **为什么不能照搬 JsonFieldStreamer。** 那个只抠顶层的一个字段（一个
// 字符串，或者一串字符串），章节正文那一步够用；大纲要的是"整份草稿边长
// 边看"——章节是一串**对象**，还嵌着人物名和地点名的数组。
//
// 办法是"补齐再解析"：模型吐出来的前缀本身是合法 JSON 的一个前缀，那么
//
//   1. 掐掉结尾那截没写完的东西（半个键、一个孤零零的逗号、冒号后面还没
//      开始的值）
//   2. 把还开着的引号、括号按相反顺序补上
//   3. 交给 nlohmann 解
//
// 就得到"到此为止它写了什么"。**值里的半截字符串要留着**——正文正在一个
// 字一个字长出来，掐掉它等于每次都只显示写完的句子，动画就没了；
// 而半截的**键**必须掐掉，`{"tit` 补成 `{"tit"` 还是解不了。
//
// ⚠️ **这个类不保证任何时刻都解得出来。** 解不出来就回 null，调用方跳过
// 这一帧即可——下一段 token 到了多半就好了。宁可少推一帧，也不要为了
// "每帧都有"去猜模型的意图。

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::stages {

/// 边收边补的 JSON。喂 token，问快照。
class PartialJson {
public:
    /// 收一段 token。原样攒着，不解析——解析在 snapshot() 里。
    void feed(const std::string& piece) { raw_ += piece; }

    /// 到此为止能解出来的那一份。解不出来返回 null（`is_null()` 为真）。
    nlohmann::json snapshot() const;

    /// 攒到现在有多少字节。给节流用（没长就别重解一遍）。
    std::size_t size() const { return raw_.size(); }
    /// 到此为止收到的原文。进度帧拿它数字数，见 write_outline。
    const std::string& raw() const { return raw_; }

private:
    std::string raw_;
};

/// 把一段没写完的 JSON 补成完整的一份**文本**（不解析）。
///
/// 单独暴露是为了能测：补齐这一步的边界情况全在这儿——结尾是转义符、
/// 结尾是键写了一半、结尾是冒号、结尾是逗号、字符串里有括号。
std::string close_partial_json(const std::string& raw);

}  // namespace changji::stages
