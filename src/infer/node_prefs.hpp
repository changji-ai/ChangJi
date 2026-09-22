#pragma once

// 界面上那张表的开关：**谁不许干什么**。
//
// **为什么不写进 config.toml。** 那里已经有一处 `[[peer.nodes]].off`，
// 但它是**数组表**——写回那套（`config/writeback.cpp`）是逐行编辑，
// 为的是保住满文件的注释，而它认的是"节 + 键"。数组表里有好几段同名的
// `[[peer.nodes]]`，逐行编辑分不出该改哪一段；为了一个开关把写回改成
// 能处理数组表，是拿一处低频功能去动一处所有配置都要过的代码。
//
// 所以分成两处，各管各的：
//
//   `[[peer.nodes]].off`   部署时定的。改它要动配置文件，界面上显示成
//                          锁着的——那是写配置的人的决定
//   这个文件               随手调的。点一下就生效，不碰 config.toml
//
// 两处取**并集**：任一处关了就是关了。界面上分得出是哪一处关的，
// 因为"点一下就能开"和"得去改配置"对用户是两件事。

#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "infer/capability.hpp"

namespace changji::infer {

/// 界面上关掉的那些。键是节点地址（`local` 或 `http://…`）。
using NodePrefs = std::map<std::string, std::set<Capability>>;

/// 存在哪：`<项目库>/nodes.json`。
std::filesystem::path node_prefs_path(const std::filesystem::path& workspace);

/// 读。文件不在、读坏了、认不出的能力名——**一律当成"没关任何东西"**。
///
/// 这一处的默认值要挑"不改变行为"的那边：读坏了就把所有能力都关掉的话，
/// 表现是整条流水线突然没机器可派，而用户完全不知道是一个 JSON 坏了。
NodePrefs load_node_prefs(const std::filesystem::path& workspace);

/// 写。写不进去抛 `std::runtime_error`——**界面上点了开关要么生效、
/// 要么当场说没生效**，不能默默回到原样。
void save_node_prefs(const std::filesystem::path& workspace,
                     const NodePrefs& prefs);

}  // namespace changji::infer
