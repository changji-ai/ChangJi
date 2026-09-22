// 一件长跑干完了弹的那一句：说清是哪一章、哪一件。
//
// 为什么要有这条用例：**真弹一条系统通知，判据在系统的通知中心里**，
// 桌面端那几个自检口子（截图、真点、真敲）一个都够不着。所以把措辞和分支
// 摘成纯函数，在这儿钉住；弹不弹、什么时候弹留在 `desktop/tray.cpp`。

#include <doctest/doctest.h>

#include "util/done_words.hpp"

using namespace changji;

TEST_CASE("干完了那句话：哪一章、哪一件都要说出来") {
    CHECK(util::done_words("run", "ep03") == "第 3 章出片了");
    CHECK(util::done_words("write", "ch12") == "第 12 章写完了");

    SUBCASE("没有章号：还是得说清是哪一件") {
        // 写全片那一件本来就不挂在某一章上。
        CHECK(util::done_words("write", "") == "写完了");
        CHECK(util::done_words("run", "") == "出片了");
    }

    SUBCASE("认不出来就说笼统的，别编") {
        // 将来多一种长跑（`kind` 来自 `pipeline::to_string(JobKind)`），
        // 这儿不认得它——回一句笼统的是对的，编一句「第 3 章怎么怎么了」
        // 而它根本不是那回事更糟。
        CHECK(util::done_words("polish", "ep03") == util::kSomethingDone);
        CHECK(util::done_words("", "ep03") == util::kSomethingDone);
        CHECK(util::done_words("", "") == util::kSomethingDone);
    }

    SUBCASE("章号形状不对就不提章号，但那一件照说") {
        // 这串字是从引擎那头一路带过来的，**别当它一定是个章号**。
        // 认不出形状时原样念出来（`chapter_word` 那条）会变成
        //「ep01_sh007出片了」——而那是一镜，不是一章。
        CHECK(util::done_words("run", "ep01_sh007") == "出片了");
        CHECK(util::done_words("run", "整部") == "出片了");
    }
}
