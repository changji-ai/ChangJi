#pragma once

// 把「挑了哪一档模型」落到一份 Settings 上。
//
// **为什么单独一个文件、而且在 config 层。** 这两个函数原来长在
// `http/setup_api.cpp` 里——那时候只有设置页那条路会用。而从"模型配置跟着
// 项目走"这件事起，**派活那一头也要用**：协调者把这部电影挑的那几档带过去，
// 工作进程照着它把自己的 `[models]` 覆一层，再去自己的模型目录里找文件。
// `infer/` 不该反过来依赖 `http/`，所以它们落到这儿——它们本来也只碰
// `config::Settings`，一点 HTTP 的事都没有。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"

namespace changji::config {

/// 角色名（`image` / `video_vae` / `tts_decoder` …）对应 ModelsConfig 里
/// 的哪个字段。认不出来回 nullptr。
///
/// 一张平表而不是一串 if：漏一个角色的表现是"下完了但配置里没写上"，
/// 而那要到出片时才报"模型没配"，离这里隔着十万八千里。
/// 加字段时这里也要加——test_setup.cpp 拿模型清单里出现过的角色查这张表。
std::string* models_field(ModelsConfig& m, const std::string& role);

/// 把一份补丁（`setup::config_patch` 出来的那个形状）落到内存里这份设置上。
///
/// **只改内存，不碰盘**。落盘是调用方的事——派活那条路上根本不该落盘：
/// 工作进程按这一趟的活覆一层，下一趟可能是另一部电影。
void apply_setup_patch(Settings& s, const nlohmann::json& patch);

}  // namespace changji::config
