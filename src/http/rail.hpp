#pragma once

// 图标条：这部片子现在都有些什么，各自动没动过。
//
// 桌面端右边那条一档一个图标，**有东西了才出现**，新东西没看过带一个点
//（见 docs/桌面端方案.md 第四节）。这个文件答两件事：
//
//   一、**有没有东西**（`present`）——判据一律写成**正面条件**。
//       「有没有一章写了正文」，不是 `chapters.size() > 0`；
//       「有没有一镜能用」，不是 `shots` 数组非空。CLAUDE.md 第四条那三处
//       坑全是后者：一章 17 个 `shot_id` 是空串的空壳照样让数组非空，
//       于是三处判据一起说"已经有分镜了"。
//
//   二、**动没动过**（`stamp`）——这一档对应的那几个文件/目录里最新的那个
//       改动时间。**用时间不用计数**：「17 镜还是 17 镜，但内容改了」这种
//       情况计数一点反应都没有，而那恰恰是最常见的一种改动（重出一镜、
//       改一句台词）。
//
// **「看过了」不在这儿。** 那是"这台机器上这个人看没看过"，是客户端自己的
// 事，不该写进项目目录——两个人共用一个项目时，他看过不等于我看过。
// 客户端拿回 stamp，和自己存的那份比，不一样就打点。
//
// 这一层**最安静的坏法**是「点该出现没出现」：人永远不知道有东西可看，
// 而且不报任何错。所以 `rail_present` 是纯函数，用例直接钉它。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

/// 图标条上的一格。
struct RailSlot {
    std::string key;         ///< story / assets / script / shots / film
    std::string label;       ///< 界面上那个名字
    bool present = false;    ///< 有东西了吗
    std::int64_t stamp = 0;  ///< 这一档最新的改动时间（Unix 秒）。0 = 没有
};

/// 这几格有没有东西。**纯函数**，参数是三个接口的原样回包。
///
/// `project` 是 `/api/project`，`story` 是 `/api/story`，
/// `outputs` 是 `/api/outputs`。少哪一个就按"那一档还没有"算——
/// 老项目没有 story.json 是常事，不该因此整条算不出来。
std::vector<RailSlot> rail_present(const nlohmann::json& project,
                                   const nlohmann::json& story,
                                   const nlohmann::json& outputs);

/// 各档的指纹。碰盘。
std::map<std::string, std::int64_t> rail_stamps(const std::filesystem::path& root);

/// GET /api/rail?path=… —— 回 `{slots: [{key,label,present,stamp}]}`
///
/// 项目是空的（还没建）时回一份全是 `present=false` 的表，不是 404：
/// 界面上那条图标条本来就要在"什么都还没有"的时候画成空的。
ApiResult get_rail(const std::string& project);

/// GET /api/peek?path=…&key=story[&episode_id=ep01] —— 那一格里有什么，一段话。
///
/// **人看的和模型看的是同一份摘要**：这儿走的就是代理那几个「看」的工具
///（`agent::run_tool`）。两处各写一份的话，迟早出现"界面上说 17 镜、模型
/// 以为 16 镜"，而那种不一致谁也发现不了。摘要要是写得不好，人和模型会同时
/// 发现——这比各自将就强。
///
/// `episode_id` 不给时挑**第一章有东西的**：剧本和镜头是按章的，而人点图标
/// 那一下并没有说是哪一章。
ApiResult get_peek(const std::string& project, const std::string& key,
                   const std::string& episode_id);

}  // namespace changji::http
