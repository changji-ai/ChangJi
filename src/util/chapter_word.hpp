#pragma once

// 磁盘上那个键，换成说给人听的说法：`ep01` → 「第 1 章」，
// `ep01_sh007` → `sh007`。
//
// `ep01` / `ch01` 是 project.json 里的键，**改不动**（改名会砸掉已有的项目
// 文件），所以只改说法——界面和文案上一律说「章」，见 CLAUDE.md 开头。
//
// ⚠️ **这一份给"说给人听"的地方用，不给章节表用。** 模型的状态摘要里那张
// 「各章：」表得留着 `ep01`：那是它的索引，它要从那儿学会该往 `episode_id`
// 里填什么。该换说法的是**回给调用方的那几句**——模型自己写着 `ep01` 问过
// 来，回话里再念一遍，对模型是废话，而 `/api/peek` 会把那段话原样摆到面板
// 上，对人就是一个英文标识符。
//
// 收成一处是因为这件事已经在两层上各要一遍了（引擎的工具回话、桌面端的
// 镜头墙和播放器）。两处各写一份的话，改了一处另一处静悄悄不跟
// ——CLAUDE.md 第八条。

#include <cctype>
#include <string>

#include "util/say.hpp"

namespace changji::util {

/// `ep01` → 「第 1 章」，`ch12` → 「第 12 章」。
///
/// **认不出来就原样回。** 猜不准的时候把原文摆出去，比编一个数出来强。
///
/// `to` 是说给谁听（见 `i18n::Audience`）。**默认给模型**，也就是中文原话
/// ——这一份最早只有模型在用，后来才被「就地看一眼」那五格摆到界面上。
/// 摆给人看的那一头要自己写上 `i18n::Audience::human()`。
inline std::string chapter_word(const std::string& id,
                                i18n::Audience to = i18n::Audience::model()) {
    std::size_t i = 0;
    while (i < id.size() && !std::isdigit(static_cast<unsigned char>(id[i]))) i++;
    if (i == 0 || i == id.size()) return id;   // 一上来就是数字、或者一个数字都没有
    std::size_t j = i;
    while (j < id.size() && std::isdigit(static_cast<unsigned char>(id[j]))) j++;
    if (j != id.size()) return id;             // 数字后面还挂着别的，认不准
    return SAYF_TO(to, "第 %1 章", std::to_string(std::stoi(id.substr(i))));
}

/// 这串字是不是一个章的键（`ep01` / `ch12`）。
///
/// **给"从外面进来的那一串"用。** 桌面端说「人这会儿在说第 3 章」时带的就是
/// 这串字，而它会**原样贴进给模型的提示词里**——不挡的话，一串别的什么东西
/// 就那么进了系统提示。这条和 `util/chat_id.hpp` 是一个道理：**拒掉，不洗**
/// （洗出来的那一个未必是他要的那一章，而他看不出来）。
///
/// 认的形状就是 `chapter_word` 读得懂的那一种：**几个字母跟几个数字，别的
/// 都没有**。空串是"没说"，由调用方自己判，这儿回 false。
inline bool chapter_key_ok(const std::string& id) {
    if (id.empty() || id.size() > 16) return false;
    std::size_t i = 0;
    while (i < id.size() && std::isalpha(static_cast<unsigned char>(id[i]))) i++;
    if (i == 0 || i == id.size()) return false;   // 没字母，或者全是字母
    for (std::size_t j = i; j < id.size(); ++j) {
        if (!std::isdigit(static_cast<unsigned char>(id[j]))) return false;
    }
    return true;
}

/// `ep01_sh007` → `sh007`；认不出前缀就原样回。
///
/// ⚠️ **去掉的是前缀，不是"取最后一段"。** 配音装不下台词时会把一镜拆成
/// 两镜（`sh003_b`），按最后一段取就只剩一个 `b`，看不出是第几镜。
///
/// **这一份是唯一的一份。** 原来它以两条一模一样的正则活在 QML 里
/// （镜头墙那一行、塞进输入框那个 chip），而工具回话里压根没有——于是同一
/// 个镜号在界面上三处三个样：`sh007`、`sh007`、`ep03_sh007`。
/// 桌面端走 `ShotModel::shortShotId` 调到这儿来（CLAUDE.md 第八条）。
inline std::string short_shot_id(const std::string& id) {
    if (id.rfind("ep", 0) != 0) return id;
    std::size_t i = 2;
    while (i < id.size() && std::isdigit(static_cast<unsigned char>(id[i]))) i++;
    if (i == 2 || i >= id.size() || id[i] != '_') return id;
    return id.substr(i + 1);
}

}  // namespace changji::util
