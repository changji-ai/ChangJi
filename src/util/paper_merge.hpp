#pragma once

// 稿纸刷新的那一下：屏幕上这份、上次存下的那份、盘上现在这份，留谁。
//
// 这件事只有三个字符串，**但它决定人打的字还在不在**，所以单拎出来写，
// 让用例够得着（同 `half_written` / `window_place`）。
//
// 什么时候会撞上：场记说完一句话，开着的那一格会重拉一遍（Main.qml 那个
// `onBusyChanged`）。而人正好在稿纸上打字——自动存是停笔 1.5 秒才写，
// 那一秒半里屏幕上这份**和谁都不一样**。
//
// 原来那一句是无条件的 `text_ = 盘上; saved_ = 盘上;`：他打的半句当场没了，
// **连"还没存"这件事都一起没了**（`saved_` 也被换掉，自动存那头再也不会把
// 他那份写回去）。屏幕上什么都没发生。

#include <string>

namespace changji::util {

/// 刷新那一下该怎么办。
enum class OnRefresh {
    /// 照常换成盘上那份。**没动过的时候就该这样**——不换的话场记写完了人
    /// 还得关掉再打开。
    TakeDisk,
    /// 留住人手上这份，什么都不说。盘上那份其实没变（多半是场记动的是
    /// 别的地方），换过去只会把他打的字抹掉，而且抹得一声不响。
    KeepTyping,
    /// 留住人手上这份，**并且说一声**：这一章现在有两份。
    KeepTypingAndWarn,
};

/// `shown` 屏幕上这份，`saved` 上次存下去的那份，`on_disk` 刚拉回来的那份。
///
/// **判据是"人动过没有"**（`shown != saved`），不是"盘上变没变"。两个条件
/// 写反过一次：只在盘上也变了的时候才留，于是"人在打字 + 盘上没变"那一档
/// 照样把他的字换成了旧的那份——没变的那份盖掉正在打的那份，是这三档里
/// 最冤的一种。
inline OnRefresh what_to_keep(const std::string& shown, const std::string& saved,
                              const std::string& on_disk) {
    if (shown == saved) return OnRefresh::TakeDisk;
    return on_disk == saved ? OnRefresh::KeepTyping : OnRefresh::KeepTypingAndWarn;
}

}  // namespace changji::util
