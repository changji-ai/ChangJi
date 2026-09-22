// 稿纸刷新那一下留谁。三个字符串，而它决定人打的字还在不在。
//
// 判据和三档的理由写在 `util/paper_merge.hpp` 头上。

#include <doctest/doctest.h>

#include "util/paper_merge.hpp"

using changji::util::OnRefresh;
using changji::util::what_to_keep;

TEST_CASE("稿纸刷新：没动过就照常换") {
    // 不换的话场记写完了人还得关掉再打开——第一条就是为这个加的。
    CHECK(what_to_keep("旧的", "旧的", "场记刚写的") == OnRefresh::TakeDisk);
    CHECK(what_to_keep("", "", "刚写出来的一章") == OnRefresh::TakeDisk);
    // 盘上和存下的一样：换不换都一样，走常规那条。
    CHECK(what_to_keep("一样", "一样", "一样") == OnRefresh::TakeDisk);
}

TEST_CASE("稿纸刷新：人正在打字，盘上没变——留住，别说话") {
    // **这一档最冤**：盘上那份根本没变，换过去等于拿旧的盖掉正在打的，
    // 而且一声不响。条件写成"只有盘上也变了才留"就会掉进这儿。
    CHECK(what_to_keep("我打了半句", "旧的", "旧的") == OnRefresh::KeepTyping);
}

TEST_CASE("稿纸刷新：人在打字，场记也改了——留住，还要说一声") {
    // 这一章现在有两份。留人手上那份（手在键盘上那个人赢），但得说出来
    // ——不说的话他不知道自己一存就把场记那一版顶掉了。
    CHECK(what_to_keep("我打了半句", "旧的", "场记刚写的") ==
          OnRefresh::KeepTypingAndWarn);
}

TEST_CASE("稿纸刷新：空字符串不是特例") {
    // 人把整章删光了（还没存），场记这会儿写完了一版。**删光也是他的动作**，
    // 照样算"动过"。
    CHECK(what_to_keep("", "旧的", "场记刚写的") == OnRefresh::KeepTypingAndWarn);
    CHECK(what_to_keep("", "旧的", "旧的") == OnRefresh::KeepTyping);
}
