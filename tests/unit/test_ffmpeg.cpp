// ffmpeg / ffprobe 输出解析的对拍。
//
// 语料由 tools/gen_ffmpeg_golden.py 生成，期望值由 Python 侧的真函数算出来。
// **那个脚本随 Python 引擎一起删了**，所以这份语料是冻住的：跑挂了的正确
// 反应是改 C++，不是重导语料（重导不了）。
//
// 这几个解析器错了的表现都很隐蔽：
//   帧率算成 0 会让时长校验永远通过；
//   展布取错字段会让所有画面都被判成纯色，于是整章重跑还是全被拒；
//   loudnorm 的 JSON 捞错一段会让响度归一化用一组错的参数。
// 全是"跑完一整章才发现"的那类。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "media/ffmpeg.hpp"
#include "util/paths.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json load_corpus() {
    const fs::path p = fs::path(CHANGJI_GOLDEN_DIR) / "ffmpeg" / "parsers.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "语料不在：" << paths::to_utf8(p)
                                            << "。这份语料生不出来了——导它的 Python 引擎"
                                               "已经删了；它在版本库里，从 git 取回来。");
    std::ostringstream buf;
    buf << in.rdbuf();
    json doc = json::parse(buf.str(), nullptr, false);
    REQUIRE_FALSE(doc.is_discarded());
    return doc;
}

/// 假的 ffmpeg：按脚本回内容，不真起进程。
media::Runner scripted(const std::string& out, int code = 0,
                       bool launched = true,
                       std::vector<std::vector<std::string>>* seen = nullptr) {
    return [out, code, launched, seen](const std::string&,
                                       const std::vector<std::string>& args,
                                       double) {
        if (seen) seen->push_back(args);
        media::ProcResult r;
        r.out = out;
        r.exit_code = code;
        r.launched = launched;
        return r;
    };
}

}  // namespace

TEST_CASE("ffprobe 输出解析：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("probe").size() >= 6);

    for (const auto& c : corpus.at("probe")) {
        const std::string name = c.at("name");
        CAPTURE(name);

        if (c.contains("python_raises")) {
            // **这一条是有意不复刻的 Python bug。**
            //
            // nb_frames 是 "N/A" 时 Python 的 int() 抛 ValueError，而 ffprobe
            // 对 mkv 和没有索引的流就是这么给的——也就是说那边 probe 一个
            // 正常的 mkv 会直接崩。C++ 侧把它当成 0，照常往下走。
            const std::string raised = c.at("python_raises");
            CAPTURE(raised);
            media::MediaInfo info;
            REQUIRE_NOTHROW(info = media::parse_probe(c.at("input"), "x.mkv"));
            CHECK(info.frames == 0);
            CHECK(info.width == 448);     // 别的字段照常解出来
            continue;
        }

        const auto& want = c.at("expected");
        const media::MediaInfo got = media::parse_probe(c.at("input"), "x.mp4");
        CHECK(got.duration_s == doctest::Approx(want.at("duration_s").get<double>()));
        CHECK(got.width == want.at("width").get<int>());
        CHECK(got.height == want.at("height").get<int>());
        CHECK(got.fps == doctest::Approx(want.at("fps").get<double>()));
        CHECK(got.frames == want.at("frames").get<int>());
        CHECK(got.has_video == want.at("has_video").get<bool>());
        CHECK(got.has_audio == want.at("has_audio").get<bool>());
        CHECK(got.pix_fmt == want.at("pix_fmt").get<std::string>());
        CHECK(got.sar == want.at("sar").get<std::string>());
    }
}

TEST_CASE("帧率是分数，不能直接当浮点解") {
    // "24000/1001" 直接 stod 会变成 24000，也就是把 NTSC 的 23.976
    // 当成 24000 fps。之后所有按帧率算的时长全错。
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("misc").at("fps")) {
        const std::string in = c.at("input");
        CAPTURE(in);
        CHECK(media::parse_fps(in) ==
              doctest::Approx(c.at("expected").get<double>()));
    }

    SUBCASE("分母是 0 时回 0，不是 inf") {
        // ffprobe 对没有视频轨的文件给 "0/0"，而 inf 会一路传进时长计算，
        // 最后表现成"时长是 nan"——那时候已经看不出源头在哪儿了。
        const double v = media::parse_fps("0/0");
        CHECK(v == 0.0);
    }
}

TEST_CASE("亮度统计解析：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("signalstats").size() >= 6);

    for (const auto& c : corpus.at("signalstats")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        const auto& want = c.at("expected");
        const media::PixelStats got = media::parse_signalstats(c.at("input"));

        CHECK(got.mean == doctest::Approx(want.at("mean").get<double>()));
        CHECK(got.spread == doctest::Approx(want.at("spread").get<double>()));
        CHECK(got.minimum == doctest::Approx(want.at("minimum").get<double>()));
        CHECK(got.maximum == doctest::Approx(want.at("maximum").get<double>()));
        CHECK(got.low == doctest::Approx(want.at("low").get<double>()));
        CHECK(got.high == doctest::Approx(want.at("high").get<double>()));
        CHECK(got.looks_blank() == want.at("looks_blank").get<bool>());
        CHECK(got.looks_clipped() == want.at("looks_clipped").get<bool>());
    }
}

TEST_CASE("展布用百分位而不是极差") {
    // 语料里 blank_with_hotspot 那一条就是为这个存在的：一张几乎全黑
    // 但有个高光点的废图，极差能到 234，看起来很正常；
    // 而 YHIGH-YLOW 是 4，一眼就是纯色。拿极差判的话这张图会被放过去。
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("signalstats")) {
        if (c.at("name") != "blank_with_hotspot") continue;
        const media::PixelStats s = media::parse_signalstats(c.at("input"));
        CHECK(s.maximum - s.minimum > 200.0);   // 极差很大
        CHECK(s.spread < 8.0);                  // 百分位展布很小
        CHECK(s.looks_blank());                 // 判对了
        return;
    }
    FAIL("语料里没有 blank_with_hotspot");
}

TEST_CASE("没有亮度统计时要抛，不是当成全黑") {
    // 当成全黑的话，一个解码失败的文件会被判成"画面是纯色"，
    // 于是流水线去重跑它——而重跑多少次都还是解码失败。
    CHECK_THROWS_AS(media::parse_signalstats("ffmpeg version 6.1\n"),
                    media::FFmpegError);
    CHECK_THROWS_AS(media::parse_signalstats(""), media::FFmpegError);
}

TEST_CASE("响度测量解析") {
    const json corpus = load_corpus();
    const auto& want = corpus.at("loudnorm").at("expected");
    const auto got = media::parse_loudnorm(corpus.at("loudnorm").at("input"));

    CHECK(got.size() == want.size());
    for (const auto& kv : want.items()) {
        CAPTURE(kv.key());
        REQUIRE(got.count(kv.key()) == 1);
        CHECK(got.at(kv.key()) == doctest::Approx(kv.value().get<double>()));
    }

    SUBCASE("只留 input_/output_/target_ 开头的键") {
        // normalization_type 是个字符串，混进来会变成 0，
        // 而调用方拿它当数字用。
        CHECK(got.count("normalization_type") == 0);
    }

    SUBCASE("从最后一对大括号里取") {
        // ffmpeg 前面还会打一堆日志，其中可能带大括号（滤镜图的描述就有）。
        // 从第一个开始找会捞到错的那一段。
        const std::string noisy =
            "[graph 0 input from stream 0:0 @ 0x1] { fake }\n" +
            corpus.at("loudnorm").at("input").get<std::string>();
        const auto again = media::parse_loudnorm(noisy);
        CHECK(again.size() == got.size());
    }

    SUBCASE("没有音轨时说清楚") {
        try {
            media::parse_loudnorm("ffmpeg version 6.1\nno audio here\n");
            FAIL("该抛");
        } catch (const media::FFmpegError& e) {
            CHECK(std::string(e.what()).find("音轨") != std::string::npos);
        }
    }
}

TEST_CASE("取样点避开首尾") {
    // 首尾各 10% 常有编码伪影，按那里判画面会误伤正常镜头。
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("misc").at("sample_points")) {
        const int n = c.at("input");
        CAPTURE(n);
        const auto got = media::sample_points(n);
        const auto want = c.at("expected").get<std::vector<double>>();
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CHECK(got[i] == doctest::Approx(want[i]));
        }
    }
}

TEST_CASE("找不到 ffmpeg 时说清怎么装") {
    // 只说"找不到"的话，用户下一步不知道该干什么。
    media::FFmpeg ff("根本没有这个程序", "也没有这个", scripted("", 1, false));
    CHECK_FALSE(ff.available());
    try {
        ff.check();
        FAIL("该抛");
    } catch (const media::FFmpegMissing& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("winget") != std::string::npos);
        CHECK(msg.find("assembly.ffmpeg_path") != std::string::npos);
    }
}

TEST_CASE("ffmpeg 报错时把它自己的话贴出来") {
    // 只说"退出码 1"的话，排查要重新手跑一遍命令。
    media::FFmpeg ff("ffmpeg", "ffprobe",
                     scripted("Error: Invalid data found when processing input",
                              1, true));
    try {
        ff.run({"-i", "x.mp4"});
        FAIL("该抛");
    } catch (const media::FFmpegError& e) {
        CHECK(std::string(e.what()).find("Invalid data") != std::string::npos);
    }
}

TEST_CASE("每次调用都带上 -nostdin") {
    // 不带的话，ffmpeg 遇到"文件已存在，覆盖吗"会去读 stdin，
    // 而这个进程的 stdin 是关着的，它会一直等到超时。
    std::vector<std::vector<std::string>> seen;
    media::FFmpeg ff("ffmpeg", "ffprobe", scripted("", 0, true, &seen));
    ff.run({"-i", "x.mp4"});

    REQUIRE(seen.size() == 1);
    const auto& args = seen[0];
    CHECK(std::find(args.begin(), args.end(), "-nostdin") != args.end());
    CHECK(std::find(args.begin(), args.end(), "-hide_banner") != args.end());
}

TEST_CASE("抽帧的 -ss 放在 -i 前面") {
    // 放后面是精确定位，但要从头解码到那一点——一段几分钟的片子会慢几十倍。
    // 放前面是关键帧定位，对"抽一张首帧"这个用途完全够。
    std::vector<std::vector<std::string>> seen;
    media::FFmpeg ff("ffmpeg", "ffprobe", scripted("", 0, true, &seen));
    ff.extract_frame("in.mp4", fs::temp_directory_path() / "out.png", 1.5);

    REQUIRE(seen.size() == 1);
    const auto& args = seen[0];
    const auto ss = std::find(args.begin(), args.end(), "-ss");
    const auto i = std::find(args.begin(), args.end(), "-i");
    REQUIRE(ss != args.end());
    REQUIRE(i != args.end());
    CHECK(ss < i);
    CHECK(*(ss + 1) == "1.500");
}

TEST_CASE("ffmpeg 超时要单独报，不能混进「退出码 N」") {
    // **这一条是跟着 proc::run 真的能超时之后才有意义的。**
    // 在那之前超时参数是摆设（popen 没有超时接口），所以从来不会走到。
    //
    // 混在"退出码 N"里的后果：一次跑了半小时被杀掉的编码，看起来就像
    // 编码参数写错了，人会去翻滤镜串——而真正的原因是这一段太长或者卡住了。
    media::FFmpeg ff("ffmpeg", "ffprobe",
                     [](const std::string&, const std::vector<std::string>&,
                        double) {
                         media::ProcResult r;
                         r.launched = true;
                         r.timed_out = true;
                         r.exit_code = 1;
                         r.out = "frame= 120 fps=24";
                         return r;
                     });
    try {
        ff.run({"-i", "a.mp4", "b.mp4"}, 1800.0);
        FAIL("该抛的");
    } catch (const media::FFmpegError& e) {
        const std::string msg = e.what();
        CHECK(msg.find("超时") != std::string::npos);
        CHECK(msg.find("1800") != std::string::npos);
        // 最后那段输出也要带上：卡在哪一帧是有用的线索。
        CHECK(msg.find("frame= 120") != std::string::npos);
    }
}

TEST_CASE("缺哪个就指哪个配置项，别永远指 ffmpeg_path") {
    // 2026-09-08 跑装配判据时撞上的：ffmpeg 配好了、ffprobe 没配，
    // 报的却是"在配置里填 assembly.ffmpeg_path"。**照着做没有任何用**，
    // 因为 ffprobe 有自己的 assembly.ffprobe_path。
    //
    // 这类错误比没提示更糟：它让人照做、发现没用，然后开始怀疑别的地方。
    const auto missing_only = [](const std::string& absent) {
        media::FFmpeg ff(
            "ffmpeg", "ffprobe",
            [absent](const std::string& exe, const std::vector<std::string>&,
                     double) {
                media::ProcResult r;
                r.launched = exe.find(absent) == std::string::npos;
                r.out = "version";
                return r;
            });
        try {
            ff.check();
            return std::string{"没抛"};
        } catch (const media::FFmpegMissing& e) {
            return std::string(e.what());
        }
    };

    SUBCASE("只缺 ffprobe：指 ffprobe_path") {
        const std::string msg = missing_only("ffprobe");
        CAPTURE(msg);
        CHECK(msg.find("assembly.ffprobe_path") != std::string::npos);
        // 不能再提 ffmpeg_path——那正是把人带偏的那句
        CHECK(msg.find("assembly.ffmpeg_path") == std::string::npos);
    }

    SUBCASE("只缺 ffmpeg：指 ffmpeg_path") {
        const std::string msg = missing_only("ffmpeg");
        CAPTURE(msg);
        CHECK(msg.find("assembly.ffmpeg_path") != std::string::npos);
        CHECK(msg.find("assembly.ffprobe_path") == std::string::npos);
    }

    SUBCASE("两个都缺：两个都要提") {
        media::FFmpeg ff("ffmpeg", "ffprobe",
                         [](const std::string&, const std::vector<std::string>&,
                            double) {
                             media::ProcResult r;
                             r.launched = false;
                             return r;
                         });
        try {
            ff.check();
            FAIL("该抛");
        } catch (const media::FFmpegMissing& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("assembly.ffmpeg_path") != std::string::npos);
            CHECK(msg.find("assembly.ffprobe_path") != std::string::npos);
        }
    }
}
