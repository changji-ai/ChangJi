#pragma once

// 这台机器的口令（对外监听时要，`request_guard.hpp` 第五条）。**只有一个**（2026-09-28 起）：
// 网页进门、别的机器派活过来、这台派活出去而那一台没单独配口令，认的都是 `[peer].token`。
//
// 原来是两个：网页一个（`CHANGJI_UI_TOKEN` / 数据目录里现生的 `ui_token`），机器之间一个
// （`[peer].token`，人手写，不写就空）。两个都开得了网页，只有后一个开得了派活那几条——人照着
// 网页的口令去「加一台」，那一行永远 401；而 `[peer].token` 空着时，对外监听的引擎连派活那几条
// 都不挂（`refuse_to_listen`），局域网上谁也借不走它的卡。
//
// 顺序：`[peer].token` → 环境变量 `CHANGJI_UI_TOKEN` → 数据目录里旧的 `ui_token` → 现生一个。
// **配置里那个排第一**：设置页改的就是它，改了就该算数；环境变量只在配置里空着时顶上。
// 后两种由调用方搬进 `[peer].token`（`settle_machine_token`），`ui_token` 那个文件搬完就删。

#include <filesystem>
#include <string>
#include <string_view>

namespace changji::http {

struct MachineToken {
    std::string value;
    /// 从哪儿来的：`config` / `env` / `file`（旧的 ui_token）/ `new`（新生成的）。
    std::string from;
};

/// 定下这台的口令。**`file` / `new` 两种没进配置**，调用方要搬进去；`new` 那个先落在
/// `ui_token` 里（0600），搬不进配置（目录只读）的话下次起来照样是它、不换。
MachineToken resolve_machine_token(const std::string& configured, const std::filesystem::path& data_dir);

/// 现生一个：32 个十六进制字（128 位）。
std::string fresh_token();

/// 这个口令能不能用：能用回空串，不能用回那句给人看的话。
///
/// 至少 16 个字，只用字母、数字和 `- _ . ~`：它要进 cookie、查询串、`Authorization` 头和
/// config.toml，这几样字符哪一处都不用转义。**只管设置页新填的**——配置里手写的、环境变量给的
/// 原样认（改规矩不能把已经连着的机器踢下线）。
std::string token_problem(std::string_view token);

}  // namespace changji::http
