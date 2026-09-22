#pragma once

// 把「这台机器能产什么」查出来，再拼成一份能传给别人看的东西。
//
// 推导规则在 `capability.hpp`（纯逻辑、能测），这一层做的是**摸磁盘**：
// 模型文件在不在、ffmpeg 跑不跑得起来、编译时链没链 sd.cpp 和 llama.cpp。
// 两层分开的理由写在那个头文件里。
//
// 这份东西有三个出口，而且**只此一处拼**：
//   - `--doctor` 末尾那句「这台能产：…」
//   - 节点的 `GET /status`，对等互联时别的机器靠它决定派不派活过来
//   - 设置页那张「机器 × 能力」的表
//
// 各拼一次的话迟早只改一边，而症状是"界面上说能出片，派过去却被拒"
// ——`pipeline::running_work()` 那儿已经栽过一次同样的事。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "infer/capability.hpp"
#include "models/hardware.hpp"

namespace changji::infer {

/// 摸一遍这台机器：模型文件在不在、ffmpeg 行不行、链了哪些推理库。
///
/// **会碰磁盘**（每个模型键 stat 一次）和**跑一次 ffmpeg -version**，
/// 所以别放在每个请求的热路径上。
NodeFacts probe_facts(const config::Settings& s);

/// 这台机器的自我介绍。对等互联时别的机器拿它决定派不派活过来。
///
/// 里头有什么：名字、版本、链了哪些库、几张卡多大、每个能力能不能干
/// （干不了附一句原因）、模型目录在哪还剩多少、忙不忙。
///
/// **磁盘剩余问的是模型目录所在的那个卷**，不是根卷——AutoDL 那种机器
/// 系统盘只有 30 GB 而模型在 `/root/autodl-tmp`，问错卷的话"够不够装
/// 这套模型"整个判反。
nlohmann::json node_status_json(const config::Settings& s,
                                const models::HardwareProfile& profile);

/// 能产哪几样，用顿号连起来（"写文、配音、装配"）。一样都不能就回
/// "什么都产不了"。`--doctor` 末尾那句用它。
std::string can_produce_line(const NodeFacts& f);

}  // namespace changji::infer
