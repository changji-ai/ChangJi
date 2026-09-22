// util/human_time 和 util/fs_time 的测试。
//
// 这两个文件加起来 57 行，原来一条测试都没有——**而它们错了都不报错**，
// 只是显示出来的数字不对。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/fs_time.hpp"
#include "util/human_time.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden() {
    const fs::path p = fs::path(CHANGJI_GOLDEN_DIR) / "human_time.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到 " << paths::to_utf8(p));
    return json::parse(in);
}

}  // namespace

TEST_CASE("human_time 逐条对上 Python 的输出") {
    // **期望值是真的调了一遍 Python 录下来的**（tools/gen_human_time_golden.py），
    // 不是手写的。手写的话最容易写成小学四舍五入，而 Python 的 f-string
    // `:.0f` 用的是 IEEE 754 的就近取偶：
    //   0.5 → "0 秒"、1.5 → "2 秒"、2.5 → "2 秒"
    //
    // C++ 这边靠 snprintf("%.0f") 拿到同样的行为。这件事**只有在恰好落在
    // .5 上才看得出来**，随便挑几个数是测不到的——所以语料里专门挑了那几个点。
    for (const auto& c : load_golden().at("cases")) {
        const double seconds = c.at("seconds").get<double>();
        const std::string want = c.at("text").get<std::string>();
        CAPTURE(seconds);
        CHECK(util::human_time(seconds) == want);
    }
}

TEST_CASE("human_time 负数当零，不出现「-3 秒」") {
    CHECK(util::human_time(-1e9) == "0 秒");
}

TEST_CASE("file_mtime_unix 返回的是 Unix 纪元的秒，不是 1601") {
    // **这一条盯的是一个差 369 年的 bug。**
    //
    // MSVC 上 `file_time_type` 的纪元是 1601-01-01，直接拿
    // `time_since_epoch()` 会比 Unix 时间大 11644473600 秒。
    // 那个数不会让任何东西崩，只会让"这个文件是什么时候改的"全错，
    // 而且所有文件一起错，**排序看起来还是对的**——最难发现的那种。
    const fs::path dir = fs::temp_directory_path() / "changji_fs_time_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path f = dir / "a.txt";
    { std::ofstream(f) << "x"; }

    const double now = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const double got = util::file_mtime_unix(f);

    // 刚写完的文件，时间应该就在现在附近。给 5 分钟的余量：
    // 够宽松到不受时钟精度和文件系统粒度影响，又远小于 369 年。
    CHECK(got > now - 300);
    CHECK(got < now + 300);

    fs::remove_all(dir, ec);
}

TEST_CASE("file_mtime_unix 对不存在的文件返回 0，不抛") {
    // 调用方拿它排序和显示，为一个缺文件抛异常会把整个列表打断。
    CHECK(util::file_mtime_unix("这个文件不存在_98765.txt") == 0.0);
}

// ── 文本小工具 ─────────────────────────────────────────────────────
//
// `strip_ws` / `collapse_ws` / `rstrip_punct` / `utf8_chars` /
// `utf8_codepoint` 原来一条测试都没有（tools/coverage_audit.py 查出来的）。
//
// 它们错了**都不报错**，只是产出的字不对：提示词里多半个字、字幕断在
// 一个汉字中间、按字节切出非法 UTF-8 然后一路往下走，最后表现成
// "字幕整轨不显示"或者"序列化时抛异常"——离出错的地方很远。
//
// 期望值取自 tests/golden/text_helpers.json，那份是**真的跑一遍 Python
// 的对应写法**生成的（rstrip("。.；;，,、 ")、str.strip()、
// re.sub(r"\s+", " ", s)、list(s)、ord(c)），不是手写的。

namespace {

json text_golden() {
    const fs::path p = fs::path(CHANGJI_GOLDEN_DIR) / "text_helpers.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到 " << paths::to_utf8(p));
    return json::parse(in);
}

}  // namespace

TEST_CASE("strip_ws 和 Python 的 str.strip() 一致") {
    for (const auto& c : text_golden().at("strip_ws")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(text::strip_ws(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("rstrip_punct 按字符剥，不按字节") {
    // 中文标点每个 3 字节，按字节剥会把前一个汉字劈成半个，产出乱码——
    // 而这个串会出现在**每一个镜头的提示词**里，坏掉是全片性的。
    for (const auto& c : text_golden().at("rstrip_punct")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(text::rstrip_punct(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("utf8_chars 切出来的是字符不是字节") {
    for (const auto& c : text_golden().at("utf8_chars")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        const auto got = text::utf8_chars(in);
        const auto want = c.at("out").get<std::vector<std::string>>();
        CHECK(got == want);
    }
    // 单独点一下四字节的：emoji 是最容易被切坏的那一档。
    const auto got = text::utf8_chars("a🎬b");
    REQUIRE(got.size() == 3);
    CHECK(got[1].size() == 4);
}

TEST_CASE("utf8_codepoint 和 Python 的 ord() 一致") {
    for (const auto& c : text_golden().at("utf8_codepoint")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(static_cast<std::uint32_t>(text::utf8_codepoint(in)) ==
              c.at("out").get<std::uint32_t>());
    }
    // 不是合法字符时返回 0，而不是抛或者返回垃圾。
    CHECK(text::utf8_codepoint("") == 0);
}

TEST_CASE("collapse_ws 只压 ASCII 空白——和 Python 有意不一样") {
    // **这一条不能照抄 Python。** Python 的 re.sub(r"\s+", " ", s) 在 str 上
    // 是 Unicode 感知的，全角空格 U+3000 也会被压成半角；C++ 这边只认
    // ASCII 空白，全角空格原样留着。text.cpp 里写明了这是有意的：
    // 为这个引一整套 Unicode 表不划算，而两边都不产生乱码，
    // 差别只是提示词里多一个全角空格。
    //
    // 语料里那几条是 Python 的输出，所以**只对不含全角空格的那些逐条比**；
    // 全角那条单独验 C++ 的行为，并把这处偏差钉住——
    // 哪天有人"顺手对齐"了，这里会红，提醒他先看 text.cpp 那段说明。
    for (const auto& c : text_golden().at("collapse_ws")) {
        const std::string in = c.at("in").get<std::string>();
        if (in.find("\u3000") != std::string::npos) continue;  // 占位，见下
        CAPTURE(in);
        CHECK(text::collapse_ws(in) == c.at("out").get<std::string>());
    }
    // 全角空格：C++ 原样留着，Python 会压成半角。
    const std::string full = "\xe4\xb8\xad\xe3\x80\x80\xe6\x96\x87";  // 中　文
    CHECK(text::collapse_ws(full) == full);
}

TEST_CASE("indent_rest：第一行不动，后面每行缩进") {
    // 体检报告的排版。原来只有 fix 那一段做了这件事，detail 是原样打的——
    // 而"出图后端"那一项的 detail 是多行的（sd.cpp 的 System Info），
    // 于是它顶格贴在报告中间，把整张表的对齐冲掉了。那是用户看到的第一屏。
    CHECK(text::indent_rest("单行", "    ") == "单行");
    CHECK(text::indent_rest("", "    ").empty());
    CHECK(text::indent_rest("a\nb", "  ") == "a\n  b");
    CHECK(text::indent_rest("a\nb\nc", "  ") == "a\n  b\n  c");
    // 结尾的换行也要照顾到：多缩一个空行比少缩一行好看得多。
    CHECK(text::indent_rest("a\n", "  ") == "a\n  ");
}

TEST_CASE("标题自己带的编号削掉，界面上才不会编两次号") {
    // **2026-09-13 用全新项目走前三步时撞到的。** 模型出的大纲编号自己就
    // 不一致（章/部混用），而界面上本来就有编号前缀，于是显示成
    //     故事页   第 2 章 · 第二部：被折叠的时间
    //     章节表   2   第1章：不该存在的呼吸
    CHECK(text::strip_leading_ordinal("第二部：被折叠的时间") == "被折叠的时间");
    CHECK(text::strip_leading_ordinal("第1章：不该存在的呼吸") ==
          "不该存在的呼吸");
    CHECK(text::strip_leading_ordinal("第三章 雨中告别") == "雨中告别");
    CHECK(text::strip_leading_ordinal("第十回、初雪") == "初雪");

    SUBCASE("没有分隔符就不削——「部」也是量词") {
        // 「第二部手机」削了会变成「手机」。
        CHECK(text::strip_leading_ordinal("第二部手机") == "第二部手机");
        CHECK(text::strip_leading_ordinal("第三章雨中告别") == "第三章雨中告别");
    }

    SUBCASE("不是章节单位的不动") {
        CHECK(text::strip_leading_ordinal("第一滴血") == "第一滴血");
        CHECK(text::strip_leading_ordinal("第三次告别") == "第三次告别");
        CHECK(text::strip_leading_ordinal("第几个人") == "第几个人");
    }

    SUBCASE("削完为空就不削") {
        // 「第三章」本身就是这一章的标题时，留着比变成空串强。
        CHECK(text::strip_leading_ordinal("第三章") == "第三章");
        CHECK(text::strip_leading_ordinal("第三章：") == "第三章：");
        CHECK(text::strip_leading_ordinal("") == "");
    }

    SUBCASE("正常标题一个字不动") {
        CHECK(text::strip_leading_ordinal("雨夜送达") == "雨夜送达");
        CHECK(text::strip_leading_ordinal("记忆回响") == "记忆回响");
    }
}

TEST_CASE("human_time_precise：对比两个时长时零头不能丢") {
    // **踩过的那一条**：配音重排之后报
    //     [info] 按配音重排了镜头时长：1 分钟 → 1 分钟（目标 1 分钟）
    // 三个数分别是 80.0 / 64.0 / 60.0 秒，human_time 在一分钟以上只留整
    // 分钟（80/60 = 1.33，%.0f 也是「1」），全压成了「1 分钟」——这句话
    // 等于什么也没说，而且同一屏上的警告还说「比目标 1 分钟长」，
    // 看上去自相矛盾。
    CHECK(util::human_time(80.0) == util::human_time(64.0));      // 病根
    CHECK(util::human_time(64.0) == util::human_time(60.0));
    CHECK(util::human_time_precise(80.0) != util::human_time_precise(64.0));
    CHECK(util::human_time_precise(64.0) != util::human_time_precise(60.0));

    CHECK(util::human_time_precise(57.0) == "57.0 秒");
    CHECK(util::human_time_precise(80.0) == "1 分 20.0 秒");
    CHECK(util::human_time_precise(64.0) == "1 分 4.0 秒");
    CHECK(util::human_time_precise(60.0) == "1 分");     // 整分钟不拖尾巴
    CHECK(util::human_time_precise(0.0) == "0.0 秒");
    CHECK(util::human_time_precise(-5.0) == "0.0 秒");   // 负数按 0 算
    CHECK(util::human_time_precise(3600.0) == "1 小时");
    CHECK(util::human_time_precise(3780.0) == "1 小时 3 分");

    // 进度条那边照旧用 human_time：那里要的是"还得等多久"，
    // 一分钟以内的零头对"该不该去睡"没有意义。
    CHECK(util::human_time(1847.0) == "31 分钟");
}

TEST_CASE("human_time_precise_as：并排比较的几个数用同一个量纲") {
    // 各自决定量纲的话，57.58 和 60.0 正好跨在一分钟两边，一句话里两种单位：
    //     51.9 秒 → 57.6 秒（目标 1 分）
    CHECK(util::human_time_precise(57.58) == "57.6 秒");
    CHECK(util::human_time_precise(60.0) == "1 分");   // 病根：单位跳了

    // 按组里**最小**的那个定：需要最细单位的是它，按它定所有人都写得下。
    const double scale = 51.9;   // = min(51.9, 57.58, 60.0)
    CHECK(util::human_time_precise_as(51.9, scale) == "51.9 秒");
    CHECK(util::human_time_precise_as(57.58, scale) == "57.6 秒");
    CHECK(util::human_time_precise_as(60.0, scale) == "60.0 秒");

    // 按最大的定会得到「0 分 51.9 秒」，更糟——这条钉住方向别反。
    CHECK(util::human_time_precise_as(51.9, 60.0) == "0 分 51.9 秒");

    // 整章本来就上分钟的，三个数照样一致。
    const double big = 175.0;    // min(175, 181.2, 180)
    CHECK(util::human_time_precise_as(175.0, big) == "2 分 55.0 秒");
    CHECK(util::human_time_precise_as(180.0, big) == "3 分");

    // 不传 scale_ref 就是拿自己当参照，和 human_time_precise 等价。
    CHECK(util::human_time_precise_as(64.0, 64.0) == util::human_time_precise(64.0));
}
