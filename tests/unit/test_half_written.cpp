// 流到一半的 Markdown 记号：屏幕上不许出现光秃秃的星号。
//
// 这条用例钉的是**中间那几档**——`**` 已经开了头、收尾还在路上。真模型
// 一次吐几十到几百毫秒，一段话里三处加粗就闪三次，而每一次闪的都是"它打错
// 字了"的样子。判据写在 `util/half_written.hpp` 的头上。

#include <doctest/doctest.h>

#include <string>

#include "util/half_written.hpp"

using changji::util::close_open_marks;

TEST_CASE("流到一半：正文停在记号上，就把记号摘掉") {
    // **不能补成 `****`**：CommonMark 里空的强调不成立，那是四个原样的星号
    // ——比原来还难看。这一条就是钉这个。
    CHECK(close_open_marks("读完了。**") == "读完了。");
    CHECK(close_open_marks("读完了。*") == "读完了。");
    CHECK(close_open_marks("报错：`") == "报错：");
    CHECK(close_open_marks("**") == "");
}

TEST_CASE("流到一半：后面已经有字了，就补个收尾——那几个字现在就是粗的") {
    CHECK(close_open_marks("读完了。**第 3 章") == "读完了。**第 3 章**");
    CHECK(close_open_marks("报错：`ENOENT") == "报错：`ENOENT`");
}

TEST_CASE("流到一半：收尾补在最后一个非空白后面，不补到换行后面") {
    // 补到换行后面的话屏幕上会凭空多一行——而那一行只活几百毫秒，
    // 表现成"底下抖了一下"。
    CHECK(close_open_marks("读完了。**第 3 章\n") == "读完了。**第 3 章**\n");
    CHECK(close_open_marks("读完了。**第 3 章  \n") == "读完了。**第 3 章**  \n");
}

TEST_CASE("流完了的那些，一个字都不动") {
    CHECK(close_open_marks("读完了。**第 3 章 · 19 镜**，总时长 1 分 47 秒。")
          == "读完了。**第 3 章 · 19 镜**，总时长 1 分 47 秒。");
    CHECK(close_open_marks("没有任何记号的一句话。") == "没有任何记号的一句话。");
    CHECK(close_open_marks("") == "");
    CHECK(close_open_marks("两处都齐的 **甲** 和 **乙**。") == "两处都齐的 **甲** 和 **乙**。");
    CHECK(close_open_marks("`a` 和 `b` 都齐。") == "`a` 和 `b` 都齐。");
}

TEST_CASE("围栏不归这儿管：三个反引号按单个数永远是奇数") {
    // 补一个上去就把围栏拆了。没收尾的围栏里那几行照旧当代码画——
    // 它本来就是代码。
    const std::string fenced = "试试这个：\n```\nchangji --doctor\n";
    CHECK(close_open_marks(fenced) == fenced);
    // 围栏在场时，同一段里的行内反引号也一起放过——分不清哪个属于谁。
    const std::string mixed = "```\ncode\n```\n再说一句 `半个";
    CHECK(close_open_marks(mixed) == mixed);
}

TEST_CASE("全是空白时不补，补了就是凭空一行") {
    CHECK(close_open_marks("  \n") == "  \n");
}
