#pragma once

// 生成参考图：角色三视图和场景空景图。
//
// **这是「设定」那一页上唯一该用 AI 的地方**（用户 2026-09-11 的判断）：
// 内容都在故事里创作完，设定这一步不创作新的人和地方，只做两件事——
// 把故事里的名单翻成可画的描述（文字，见 stages/bible），
// 再照着那份描述生成参考图（图片，这里）。
//
// 在这之前参考图只能手动上传，而这台机器上出图能力是现成的
// （逐镜首帧天天在用），三视图和空景图完全可以照着外观提示词生成。
//
// 纯函数：只拼提示词，不碰出图后端。怎么把提示词送给 sd.cpp 是调用方的事
// ——和 bible/script/storyboard 一个分法，理由也一样：提示词拼接能测死，
// 而真出一张图要几十秒。

#include <string>
#include <vector>

#include "models/character.hpp"

namespace changji::stages {

/// 三视图的一个朝向。
struct RefPose {
    /// 和 Character::ref_front / ref_three_quarter / ref_back 对得上的键。
    std::string key;
    std::string label;   ///< 界面上显示的
    std::string phrase;  ///< 拼进提示词的那一句
};

/// 要出哪几个朝向。**顺序就是界面上的顺序。**
const std::vector<RefPose>& ref_poses();

/// 角色参考图的提示词。
///
/// **外观块逐字用 Character::render_prompt 拼出来的那一份**，不另写一套——
/// 参考图和后面每一镜必须是同一个人，而"同一个人"靠的就是那段字逐字节
/// 相同。这里只在它前后加朝向和背景的要求。
///
/// pose_key 不认识时按正面出。
std::string build_character_ref_prompt(const models::Character& c,
                                       const models::StyleProfile& style,
                                       const std::string& pose_key);

/// 场景空景图的提示词。
///
/// **写死「里面不能有人」**：空景图是场景一致性的锚点，后面每一镜都拿它
/// 当底子。里面站个人的话，那个人会被当成这个地方的一部分，一路复制到
/// 每一镜里去。
std::string build_location_ref_prompt(const models::Location& l,
                                      const models::StyleProfile& style);

/// 参考图的负向提示词。
///
/// 比出片那份多压两样：**多人**和**文字水印**。三视图里多出一个人，
/// 后面分镜就分不清哪个是主角；而参考图上的字会被当成服装花纹复制下去。
std::string ref_negative(const models::StyleProfile& style);

}  // namespace changji::stages
