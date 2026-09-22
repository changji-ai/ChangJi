#pragma once

// 把改动写回用户全局配置。
//
// **保留文件里已有的注释和其它项。** 这一条是硬要求，不是锦上添花：
// 内置的配置模板整个是注释写的，每一节都在解释这一项是干什么的、
// 改了会怎样。重新序列化一遍整个 Settings 会把它们全抹掉，
// 用户下次打开配置文件看到的是一堆没有说明的键值对。
//
// Python 那边用 tomlkit，它天生保留格式。toml++ **不保留注释**——
// 解析成 table 再 << 出去，注释全没了。所以这里不走"解析再序列化"，
// 走**逐行编辑**：找到那一行改掉，别的一个字节都不动。
//
// 代价是要自己处理 TOML 的一小部分语法（节头、键值行、注释行）。
// 这个代价是值得的：写回配置是低频操作，而注释是用户理解配置的唯一途径。

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace changji::config {

/// 写回配置。patch 的形状是 {节名: {键: 值}}，顶层标量也收
/// （比如 vram_gb_override 就不在任何节里）。
///
/// 返回写到了哪个文件。写不进去抛 std::runtime_error。
///
/// 三条行为：
/// - 键已经在那一节里 —— 就地改那一行，前后的注释都不动
/// - 键不在但节在     —— 插在那一节的**末尾**，不是开头（开头会插到
///                       节的说明注释和第一个键之间，读起来断裂）
/// - 节不存在         —— 追加到文件末尾
std::filesystem::path save_user_config(
    const nlohmann::json& patch,
    const std::optional<std::filesystem::path>& path = std::nullopt);

/// 把「有哪些别的机器」整份写回 `[[peer.nodes]]`。
///
/// **单独一个函数，不走上面那条。** `save_user_config` 的形状是
/// {节: {键: 值}}，写不了数组表；而把这几台写成 `[peer]` 下的内联数组
/// （`nodes = [{url=…}]`）更糟：文件里要是已经有手写的 `[[peer.nodes]]`，
/// 那就是**同一个键定义了两遍**，toml++ 当场拒绝整份配置——一次「加一台
/// 机器」把配置写坏，程序下次连界面都起不来。数组表重复只是多几台，
/// 坏不到那个份上。
///
/// `nodes` 是个数组，每项 `{"url":…, "token":…, "off":[…]}`。**整份替换**：
/// 文件里所有**没被注释掉的** `[[peer.nodes]]` 块先删掉，再把这一份追加到
/// 末尾。模板里那段注释掉的示例不动（它以 `#` 开头，不算块）。
///
/// 追加在末尾是刻意的：`[[peer.nodes]]` 会隐式建出 `peer` 这张表，而显式
/// 的 `[peer]` 节头要是排在它后面，TOML 那条「不能重复定义」就可能踩上。
/// 摆在最后，`[peer]` 永远在前面。
///
/// 返回写到了哪个文件。写不进去抛 std::runtime_error。
std::filesystem::path save_peer_nodes(
    const nlohmann::json& nodes,
    const std::optional<std::filesystem::path>& path = std::nullopt);

/// 把一个 JSON 值转成 TOML 的字面量写法。
///
/// 单独暴露是为了能测：字符串要加引号并转义，浮点要保证带小数点
/// （写成 `1` 的话下次读回来是整数，而字段类型是 float，toml++ 会拒绝）。
std::string to_toml_literal(const nlohmann::json& v);

}  // namespace changji::config
