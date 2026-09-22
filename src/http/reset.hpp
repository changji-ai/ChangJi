#pragma once

// 把已渲染的镜头退回待跑。
//
// 好几个接口都要做这件事：改角色外观、改场景、改风格、传参考图、
// 重出角色圣经。它们的共同点是**改了会影响提示词的东西**——
// 提示词变了，已经出的画面就和新设定对不上了，留着比重跑更糟：
// 用户以为改生效了，成片里却混着两套设定。
//
// 原来这个函数在 upload.cpp、editing_assets.cpp、editing_batch.cpp 里
// 各有一份逐字节相同的拷贝。抽出来是因为它有两条容易改歪的规则
// （见下），三份拷贝迟早会分叉，而分叉的表现是"同一件事在不同入口
// 结果不一样"，这种 bug 很难联想到是拷贝没同步。

#include <set>
#include <string>

#include "models/project.hpp"

namespace changji::http {

/// 把项目里所有已渲染的镜头退回 PLANNED，返回退了几个。
///
/// 两条不显然的规则：
///
/// 一，**LOCKED 的不动**。锁定的意思就是"这一镜我满意了，别再动它"。
///     用户锁一镜正是为了在反复调设定的过程中保住它。
///
/// 二，**PLANNED 的不计数**。它本来就没渲染过，退回它是空操作。
///     算进去的话返回给前端的数字会虚高，用户看到"重置了 40 个镜头"
///     但实际只有 3 个真的要重跑。
///
/// 顺带把 attempts 归零、gate_notes 清空——那些是上一轮的产物，
/// 留着会让重试次数从旧值接着数，第一次重跑就可能直接判超限。
int reset_all_shots(const models::ProjectStore& store);

/// 同样的规则，但**只动指定的那几章**。
///
/// `/api/shots/link_locations` 要的是这个。它改的是「某一章的镜头接回哪个
/// 场景」——别的章的提示词一个字都没变，不符合上面那条"改了会影响提示词的
/// 东西"。拿 reset_all_shots 去收尾的话，接一章的场景会把**整个项目**已经
/// 渲染好的镜头全退回待跑，下一次按「开始」就是几小时的重跑，而用户按的
/// 那颗按钮上写的是这一章。
int reset_shots_in(const models::ProjectStore& store,
                   const std::set<std::string>& episode_ids);

}  // namespace changji::http
