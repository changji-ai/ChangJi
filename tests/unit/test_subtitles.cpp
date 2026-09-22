// 字幕断行与 ASS 输出的对拍。
//
// 语料由 tools/gen_subtitles_golden.py 生成，期望值由 Python 的真函数算出来。
// **那个脚本随 Python 引擎一起删了**，所以这份语料是冻住的：跑挂了的正确
// 反应是改 C++，不是重导语料（重导不了）。
//
// ASS 的**整份文件内容**都比对：它要喂给 libass，多一个空格少一个逗号
// 都可能让整条字幕轨不渲染，而那要到成片出来才看得见。
//
// 断行错了更隐蔽——不会不渲染，只是断在词中间，观感差一点。
// 一章几十条字幕，人不会逐条去看。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "media/subtitles.hpp"
#include "util/text.hpp"
#include "util/paths.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json load_corpus() {
    const fs::path p =
        fs::path(CHANGJI_GOLDEN_DIR) / "subtitles" / "subtitles.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(),
                    "语料不在：" << paths::to_utf8(p)
                                 << "。这份语料生不出来了——导它的 Python 引擎已经删了；"
                                    "它在版本库里，从 git 取回来。");
    std::ostringstream buf;
    buf << in.rdbuf();
    json doc = json::parse(buf.str(), nullptr, false);
    REQUIRE_FALSE(doc.is_discarded());
    return doc;
}

std::vector<media::SubtitleCue> cues_of(const json& arr) {
    std::vector<media::SubtitleCue> out;
    for (const auto& c : arr) {
        media::SubtitleCue cue;
        cue.start_s = c.at("start_s");
        cue.end_s = c.at("end_s");
        cue.text = c.at("text");
        cue.style = c.at("style");
        out.push_back(cue);
    }
    return out;
}

/// 两份文本第一处不一样在哪儿。直接比的话报错只说"两个大串不等"。
std::string first_diff(const std::string& got, const std::string& want) {
    const std::size_t n = std::min(got.size(), want.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (got[i] != want[i]) {
            const std::size_t from = i > 40 ? i - 40 : 0;
            return "第 " + std::to_string(i) + " 字节起不同：\n  得到 ..." +
                   got.substr(from, 90) + "\n  期望 ..." + want.substr(from, 90);
        }
    }
    if (got.size() != want.size()) {
        return "长度不同：" + std::to_string(got.size()) + " vs " +
               std::to_string(want.size()) + "，多出来的是：" +
               (got.size() > want.size() ? got.substr(n, 80) : want.substr(n, 80));
    }
    return {};
}

}  // namespace

TEST_CASE("台词里的花括号会被 ASS 当成特效码吞掉") {
    // ASS 里 `{` 开始一个特效覆盖块、到 `}` 为止整段被吞掉。台词里出现
    // 一个 `{`，那几个字在成片里就没了——**而且不报错**，要盯着片子看
    // 才发现。剧本是大模型写的，它偶尔会吐出 ASCII 花括号。
    std::vector<media::SubtitleCue> cues;
    media::SubtitleCue c;
    c.start_s = 0.0;
    c.end_s = 2.0;
    c.text = "他说{这里有鬼}然后跑了";
    c.style = "dialogue";
    cues.push_back(c);

    const std::string ass = media::build_ass(cues);
    CAPTURE(ass);
    // 转义成字面的大括号，libass 认这种写法。
    // **不逐字比整句**：文本可能被自动折行插进 \\N，那样比法太脆。
    CHECK(ass.find("\\{") != std::string::npos);
    CHECK(ass.find("\\}") != std::string::npos);
    // **字一个都不能少**
    CHECK(ass.find("这里有鬼") != std::string::npos);
    CHECK(ass.find("然后跑了") != std::string::npos);
    // Dialogue 那几行里不许再有没转义的花括号——有就会被当成特效码。
    const std::size_t d = ass.find("Dialogue:");
    REQUIRE(d != std::string::npos);
    for (std::size_t i = d; i < ass.size(); ++i) {
        if (ass[i] != '{' && ass[i] != '}') continue;
        CHECK(i > 0);
        CHECK(ass[i - 1] == '\\');
    }
}

TEST_CASE("台词里混进裸换行不能把 Dialogue 行拆断") {
    // Dialogue 是一行一条记录。文本里混进 CR/LF 之后半条记录变成下一行，
    // 渲染器多半直接忽略——又是一处静默丢字。
    std::vector<media::SubtitleCue> cues;
    media::SubtitleCue c;
    c.start_s = 0.0;
    c.end_s = 2.0;
    c.text = "上半句\n下半句";
    c.style = "dialogue";
    cues.push_back(c);

    const std::string ass = media::build_ass(cues);
    CAPTURE(ass);
    // 只能有一条 Dialogue，而且两半都还在
    std::size_t n = 0, pos = 0;
    while ((pos = ass.find("Dialogue:", pos)) != std::string::npos) {
        ++n;
        pos += 9;
    }
    CHECK(n == 1);
    CHECK(ass.find("上半句") != std::string::npos);
    CHECK(ass.find("下半句") != std::string::npos);
}

TEST_CASE("显示宽度：全角算一，半角算半") {
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("width")) {
        const std::string t = c.at("text");
        CAPTURE(t);
        CHECK(media::display_width(t) ==
              doctest::Approx(c.at("expected").get<double>()));
    }

    SUBCASE("区间表是从 Python 的 unicodedata 导出的，不是手写的") {
        // 手写必然有出入，而出入的表现不是报错，是某几句字幕断行位置
        // 和 Python 不一样——那要逐帧比对成片才看得出来。
        CHECK(media::is_wide(U'你'));
        CHECK(media::is_wide(U'，'));
        CHECK(media::is_wide(U'Ａ'));      // 全角拉丁字母
        CHECK(media::is_wide(U'🎉'));      // 基本平面之外
        CHECK_FALSE(media::is_wide(U'a'));
        CHECK_FALSE(media::is_wide(U'，' - 0xFEE0));   // 对应的半角逗号
        CHECK_FALSE(media::is_wide(U'①'));            // 圈号不是 W/F
    }
}

TEST_CASE("中文断行：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("wrap").size() >= 13);

    for (const auto& c : corpus.at("wrap")) {
        const std::string t = c.at("text");
        const double w = c.at("max_width");
        const int lines = c.at("max_lines");
        CAPTURE(t);
        CAPTURE(w);
        CAPTURE(lines);

        const auto got = media::wrap_chinese(t, w, lines);
        const auto want = c.at("expected").get<std::vector<std::string>>();
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CHECK(got[i] == want[i]);
        }
    }
}

TEST_CASE("断行不丢字") {
    // 超过行数上限时，塞不下的部分并进最后一行。丢字是最糟的：
    // 观众看到的是一句没说完的话，而且不会有任何报错。
    const std::string long_line =
        "第一句话说完了，第二句话也说完了，第三句话还在继续说个不停。";
    const auto lines = media::wrap_chinese(long_line, 10.0, 2);
    std::string joined;
    for (const auto& l : lines) joined += l;
    // 断行只会去掉行首行尾的空白，中文没有空格，所以应该一个字不少
    CHECK(joined == long_line);
}

TEST_CASE("断行不产出非法 UTF-8") {
    // 按字节算下标的话，一个中文字会被劈成三段——那个串会进 ASS 文件，
    // 播放器直接不显示整条字幕轨。
    const auto lines = media::wrap_chinese(
        "这是一句没有任何标点符号的很长很长的中文台词需要按宽度断开", 7.0, 2);
    for (const auto& l : lines) {
        CAPTURE(l);
        // 每个字符的首字节都不能是续接字节（10xxxxxx）
        CHECK((static_cast<unsigned char>(l.front()) & 0xC0) != 0x80);
        // 长度必须是完整字符之和
        std::size_t i = 0;
        while (i < l.size()) {
            const std::size_t n =
                text::utf8_char_len(static_cast<unsigned char>(l[i]));
            REQUIRE(i + n <= l.size());
            i += n;
        }
        CHECK(i == l.size());
    }
}

TEST_CASE("ASS 时间格式") {
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("ass_time")) {
        const double t = c.at("input");
        CAPTURE(t);
        CHECK(media::ass_time(t) == c.at("expected").get<std::string>());
    }

    SUBCASE("负数按 0 算") {
        // 时间轴上出现负数说明上游算错了，但字幕文件本身不能因此不合法——
        // 一个 -1:59:59 会让 libass 整份文件都不解析。
        CHECK(media::ass_time(-5.0) == "0:00:00.00");
    }
}

TEST_CASE("ASS 整份文件逐字节和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("ass").size() >= 5);

    for (const auto& c : corpus.at("ass")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        const std::string got = media::build_ass(cues_of(c.at("cues")));
        const std::string want = c.at("expected");
        const std::string diff = first_diff(got, want);
        CHECK_MESSAGE(diff.empty(), name << "：" << diff);
    }
}

TEST_CASE("字幕样式跟着真实画布缩，不然每行都从两头出画") {
    // **实机撞出来的。** 成片画布 2026-09-10 从 1080×1920 改成 544×928
    // 之后，样式里那几个数原封不动：字号 54、左右边距各 60、单行 15 字。
    // 15 × 54 = 810 像素，而 544 减掉两边 60 只剩 424——每一行都超出
    // 将近一倍。更糟的是 WrapStyle: 2 不自动折行（断点是我们自己插的），
    // 所以超出的部分是**直接切掉**：从 walk_c ep01 抽的那一帧上，
    // 「该死，这单要是超时了我得赔光这」左边的"该"和右边的"这"各缺一半。
    SUBCASE("参考画布上一个字都不动") {
        media::AssOptions opt;   // 默认就是 1080×1920
        const auto m = media::ass_metrics(opt);
        CHECK(m.font_size == 54);
        CHECK(m.margin_h == 60);
        CHECK(m.margin_v == 180);
        CHECK(m.max_chars_per_line == 15);
    }

    SUBCASE("544×928：横向按宽比缩，纵向按高比缩") {
        media::AssOptions opt;
        opt.width = 544;
        opt.height = 928;
        const auto m = media::ass_metrics(opt);
        CHECK(m.font_size == 27);    // 54 × 544/1080
        CHECK(m.margin_h == 30);
        CHECK(m.margin_v == 87);     // 180 × 928/1920
        // 装得下：15 × 27 = 405 ≤ 544 − 60 = 484
        CHECK(m.max_chars_per_line * m.font_size <= opt.width - 2 * m.margin_h);
    }

    SUBCASE("每一档都装得下") {
        for (const auto& wh : std::vector<std::pair<int, int>>{
                 {544, 928}, {704, 1280}, {1440, 2560},
                 {928, 544}, {1280, 704}, {2560, 1440}}) {
            media::AssOptions opt;
            opt.width = wh.first;
            opt.height = wh.second;
            const auto m = media::ass_metrics(opt);
            CAPTURE(opt.width);
            CAPTURE(opt.height);
            CAPTURE(m.font_size);
            CAPTURE(m.max_chars_per_line);
            CHECK(m.max_chars_per_line * m.font_size <=
                  opt.width - 2 * m.margin_h);
            CHECK(m.max_chars_per_line >= 6);
        }
    }

    SUBCASE("字号被人单独调大时，单行字数跟着让步") {
        // 兜底那一层：不信配置里那个 15，按算出来的宽度取小的。
        media::AssOptions opt;
        opt.width = 544;
        opt.height = 928;
        opt.font_size = 108;   // 参考画布上的两倍
        const auto m = media::ass_metrics(opt);
        CHECK(m.font_size == 54);
        CHECK(m.max_chars_per_line < 15);
        CHECK(m.max_chars_per_line * m.font_size <= opt.width - 2 * m.margin_h);
    }

    SUBCASE("真出一份 ASS，样式行里是缩过的数") {
        media::AssOptions opt;
        opt.width = 544;
        opt.height = 928;
        const std::string ass = media::build_ass(
            {media::SubtitleCue{0.0, 2.0, "该死，这单要是超时了我得赔光这个月的房租",
                                "dialogue"}},
            opt);
        CHECK(ass.find("PlayResX: 544") != std::string::npos);
        CHECK(ass.find("Style: dialogue,Source Han Sans SC,27,") !=
              std::string::npos);
        CHECK(ass.find(",30,30,87,1") != std::string::npos);
        CHECK(ass.find(",60,60,180,1") == std::string::npos);
    }
}

TEST_CASE("写出来的字幕文件带 UTF-8 BOM") {
    // 一些播放器靠它才认出中文，没有的话按本地代码页解，出来的是一屏乱码。
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_字幕");
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path dest = dir / paths::from_utf8("第一集.ass");

    media::write_ass(dest, {media::SubtitleCue{0.0, 2.0, "你好", "dialogue"}});

    std::ifstream in(dest, std::ios::binary);
    REQUIRE(in.good());
    std::string head(3, '\0');
    in.read(head.data(), 3);
    CHECK(head == "\xEF\xBB\xBF");

    SUBCASE("父目录会自动建出来") {
        CHECK(fs::exists(dest));
    }
}

TEST_CASE("字幕自检：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("validate")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        const auto got = media::validate_cues(cues_of(c.at("cues")));
        const auto want = c.at("expected").get<std::vector<std::string>>();
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CHECK(got[i] == want[i]);
        }
    }
}
