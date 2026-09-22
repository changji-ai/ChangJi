#pragma once

// 一件长跑干完了，弹给人看的那一句。
//
// **「活干完了」对等了一个钟头的人等于没说。** 一章出片一个钟头起，而这条
// 通知是他在别的程序里干别的时候唯一会看见的东西——他要知道的是"能去看片
// 了吗"，不是"有件事结束了"（CLAUDE.md 第十条：名字要说清干什么）。
//
// 收成一个纯函数是为了**能验**：真弹一条系统通知，判据在系统的通知中心里，
// 自检那几个口子够不着。分支本身（哪一种、有没有章号）在这儿钉住，弹不弹、
// 什么时候弹在 `desktop/tray.cpp`。
//
// ⚠️ **认不出来就说笼统的那一句，别编。** 将来多一种长跑（`kind` 是引擎
// 那头 `pipeline::to_string(JobKind)` 给的），这儿不认得它，回「活干完了」
// 是对的——编一句「第 3 章怎么怎么了」而它根本不是那回事更糟。

#include <string>

#include "util/chapter_word.hpp"

namespace changji::util {

/// 认不出哪一件时说的那句。
inline constexpr const char* kSomethingDone = "活干完了";

/// `kind` 是 `run`（出片）或 `write`（写全片），别的一律回 `kSomethingDone`。
/// `episode` 是章的键，空或者认不出形状就不提章号。
inline std::string done_words(const std::string& kind, const std::string& episode) {
    const bool named = chapter_key_ok(episode);
    const std::string chapter = named ? chapter_word(episode) : std::string();
    if (kind == "run") return named ? chapter + "出片了" : "出片了";
    if (kind == "write") return named ? chapter + "写完了" : "写完了";
    return kSomethingDone;
}

}  // namespace changji::util
