#pragma once

// 角色圣经生成。
//
// 两阶段生成的第一阶段。先只产角色和场景，登记进资产库拿到 id，
// 第二阶段才产分镜，那时 schema 里已经没有外观字段可写了。
//
// 顺序不能反。先有分镜再补角色，模型已经在分镜里写过一遍外观，
// 之后再想统一就晚了。
//
// ---
//
// 这里刻意**不碰网络也不碰 llama.cpp**，只有两个纯函数：
// 拼提示词、解析返回。怎么把提示词送给模型是调用方的事。
//
// 这么切是因为这两件事的验证方式完全不同：提示词拼接要求和 Python
// 逐字节一致，可以用对拍测死；而"调模型"是有副作用、慢、且不确定的。
// 混在一起的话，前者就没法测了。

#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 大模型没按预期返回时抛这个。
class BibleError : public std::runtime_error {
public:
    explicit BibleError(const std::string& what) : std::runtime_error(what) {}
};

/// 请求里带的 JSON Schema。远端不支持统一的 response_format 方言，
/// 所以客户端会把它附在提示词后，并在收到结果后执行本地校验。
const nlohmann::ordered_json& bible_schema();

/// 从故事定妆时使用的动态 schema：名字只能来自故事名单，数组数量也必须一致。
nlohmann::ordered_json bible_schema_for_story(const models::Story& story);

/// 拼提示词。**输出必须和 Python 的 build_prompt 逐字节一致。**
///
/// 这是**从一章剧本**出圣经的老路径。故事层立起来之后它只留给没有
/// story.json 的老项目——见下面 build_bible_prompt_from_story 的说明。
std::string build_bible_prompt(const std::string& script,
                               models::StyleLine style_line);

/// 把故事渲染成给美术看的那一段：调子、人物表、关系、地点表、分章。
///
/// 单独拆出来是为了能测：拼提示词那一步只是在它前后各接一段常量，
/// 真正会出错的是这里——漏掉一个人、把欲望写进外观段、分章截断截在
/// 半个字上。
std::string render_story_for_bible(const models::Story& story);

/// 拼提示词。**从故事出**，这是故事层立起来之后的默认路径。
///
/// 和上面那个的区别不是措辞：老的那条是"读一章剧本，找出里面有哪些
/// 角色和场景"，于是全片共用的资产库其实是从第一章推出来的，后面几章
/// 新冒出来的人只能一个个补登记。名单从故事来之后，它一次就是全的。
std::string build_bible_prompt_from_story(const models::Story& story,
                                          models::StyleLine style_line);

/// 解析模型返回，产出资产库。
///
/// raw 是模型吐的原始文本，可能裹着 ```json 之类的外壳，
/// 解析前会先抽出 JSON 部分。
/// `aspect_ratio` **没有默认值，故意的**。它以前默认 "9:16"，于是横屏
/// 项目每定一次妆，assets.json 里的比例就被悄悄改回竖屏——不报错，
/// 下一次出参考图才看得出来。传 `settings.video.aspect_ratio()`。
models::AssetLibrary parse_bible(const std::string& raw,
                                 models::StyleLine style_line,
                                 const std::string& aspect_ratio);

/// 解析故事定妆结果，并核对角色、地点集合与故事名单完全一致。
models::AssetLibrary parse_bible_for_story(const std::string& raw,
                                           models::StyleLine style_line,
                                           const std::string& aspect_ratio,
                                           const models::Story& story);

/// 默认负向提示词。
///
/// 动漫线要额外压写实倾向，因为 Wan 有很强的写实偏置，
/// 不压的话动漫输入会被往真人方向拽。
std::string default_negative(models::StyleLine style_line);

}  // namespace changji::stages
