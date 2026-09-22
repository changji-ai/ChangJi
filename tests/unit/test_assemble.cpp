// 装配的测试。
//
// **ffmpeg 的参数当契约来测。** 参数错了不会当场报错，只会让成片在
// 某些播放器上打不开、音画错位、或者中间丢掉一整个镜头——那要等到
// 成片出来、有人从头看一遍才发现，而那是一整部电影。
//
// 时间线那部分测的是**字幕时间戳来自配音的真实时长**。用估算时长的话，
// 一章下来字幕会越飘越远，而每一条单看都"差不多对"。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <cctype>
#include <vector>

#include <nlohmann/json.hpp>

#include "media/assemble.hpp"
// 时间轴按真正会生成的帧数排，这两个头提供上限和 frames_for
#include "stages/limits.hpp"
#include "stages/render.hpp"
#include "util/paths.hpp"

using json = nlohmann::json;

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 找一个参数的值。找不到返回空串。
std::string arg_value(const std::vector<std::string>& args,
                      const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || it + 1 == args.end()) return {};
    return *(it + 1);
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_装配_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// 造一个带视频文件的项目。
models::ProjectPaths make_paths(const std::string& tag) {
    const fs::path root = temp_root(tag);
    models::ProjectPaths p(root);
    p.ensure();
    return p;
}

void touch(const fs::path& p, std::size_t bytes = 2048) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << std::string(bytes, 'x');
}

models::Shot make_shot(const std::string& id, double dur,
                       const std::string& video_rel) {
    models::Shot s;
    s.shot_id = id;
    s.duration_s = dur;
    s.video_path = video_rel;
    return s;
}

models::DialogueLine line(const std::string& text, double dur,
                          std::optional<std::string> char_id,
                          const std::string& audio_rel = "") {
    models::DialogueLine l;
    l.text = text;
    l.actual_duration_s = dur;
    l.char_id = std::move(char_id);
    if (!audio_rel.empty()) l.audio_path = audio_rel;
    return l;
}

/// 让「名义时长 == 真实时长」，好让下面几条用例专心测时间轴本身。
///
/// 时间轴是按这一镜**真正会生成多少帧**排的，而帧数要落在模型的格子上
/// （默认 MiniMax-H3 的 17k+5），所以 5 秒的镜头实际是 124 帧 = 5.167 秒。
/// 那件事由「时间轴按真实帧数排」那一条单独钉；这几条测的是字幕时间戳和
/// 溶解重叠，掺进帧数对齐只会让期望值看不懂。
struct ExactFrames {
    stages::VideoLimits saved = stages::video_limits();
    ExactFrames() {
        stages::VideoLimits v;
        v.max_frames = 100000;
        v.frame_step = 1;
        v.frame_base = 0;
        stages::set_video_limits(v);
    }
    ~ExactFrames() { stages::set_video_limits(saved); }
};

}  // namespace

TEST_CASE("时间轴按这一镜真正会生成的帧数排，不按分镜表的名义时长") {
    // 帧数要落在模型的格子上：名义 4 秒的镜头按 17k+5 对齐之后是 107 帧
    // = 4.458 秒。按名义值排的话，每镜差的那几百毫秒会**逐镜累积**——
    // 十几镜之后字幕和画面能错开好几秒，而每一段单独看都是对的。
    const auto paths = make_paths("真实帧数");
    touch(paths.shots("draft") / "a.mp4");
    touch(paths.shots("draft") / "b.mp4");

    const auto s1 = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    const auto s2 = make_shot("sh002", 4.0, "shots/draft/b.mp4");
    const auto tl = media::build_timeline({s1, s2}, paths,
                                          config::AssemblyConfig{});
    REQUIRE(tl.entries.size() == 2);

    const double d1 = stages::frames_for(5.0, 24) / 24.0;   // 124 帧
    const double d2 = stages::frames_for(4.0, 24) / 24.0;   // 107 帧
    CHECK(d1 > 5.0);
    CHECK(d2 > 4.0);
    CHECK(tl.entries[0].duration_s == doctest::Approx(d1));
    CHECK(tl.entries[1].start_s == doctest::Approx(d1));
    CHECK(tl.total_duration_s() == doctest::Approx(d1 + d2));
}

TEST_CASE("时间线：字幕时间戳来自配音的真实时长") {
    const ExactFrames exact;
    // 用估算时长的话，一章下来字幕会越飘越远，而每一条单看都"差不多对"。
    const auto paths = make_paths("时间线");
    touch(paths.shots("draft") / "a.mp4");
    touch(paths.shots("draft") / "b.mp4");

    auto s1 = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    s1.dialogue.push_back(line("我等了你三年。", 2.5, "c_lin"));
    s1.dialogue.push_back(line("你却什么都没说。", 1.75, "c_lin"));
    auto s2 = make_shot("sh002", 4.0, "shots/draft/b.mp4");
    s2.dialogue.push_back(line("那一夜之后，她再没回来过。", 3.0, std::nullopt));

    const auto tl = media::build_timeline({s1, s2}, paths,
                                          config::AssemblyConfig{});
    REQUIRE(tl.entries.size() == 2);
    CHECK(tl.entries[0].start_s == doctest::Approx(0.0));
    CHECK(tl.entries[1].start_s == doctest::Approx(5.0));
    CHECK(tl.total_duration_s() == doctest::Approx(9.0));

    const auto cues = tl.cues();
    REQUIRE(cues.size() == 3);
    // 第一句从 0 到 2.5，第二句紧接着——**不是按镜头时长均分**
    CHECK(cues[0].start_s == doctest::Approx(0.0));
    CHECK(cues[0].end_s == doctest::Approx(2.5));
    CHECK(cues[1].start_s == doctest::Approx(2.5));
    CHECK(cues[1].end_s == doctest::Approx(4.25));
    // 第二镜的字幕从这一镜的起点算，不是接着上一句
    CHECK(cues[2].start_s == doctest::Approx(5.0));

    SUBCASE("没有 char_id 的是旁白，用另一套样式") {
        CHECK(cues[0].style == "dialogue");
        CHECK(cues[2].style == "narration");
    }

    SUBCASE("没有配音时长的台词不出字幕") {
        // 时长是 0 说明配音没跑或者跑失败了。硬出一条 0 秒的字幕，
        // 播放器要么闪一下要么直接不显示，两种都比不出更糟——
        // 因为看起来"有字幕"。
        auto s = make_shot("sh003", 3.0, "shots/draft/a.mp4");
        s.dialogue.push_back(line("没配音的一句", 0.0, "c_lin"));
        const auto t = media::build_timeline({s}, paths, config::AssemblyConfig{});
        CHECK(t.cues().empty());
    }
}

TEST_CASE("转场不影响时间线：装配是纯硬切，没有重叠") {
    // **这条用例原来钉的是反的**：「溶解让两镜重叠，起点往回挪」，
    // 期望 4.6。可装配走的是 `-f concat -c copy`，全树一处 xfade 都没有
    // ——转场从来没被渲染过，两镜之间根本没有重叠。
    //
    // 按重叠排时间线的后果是**字幕比画面早，而且逐个 dissolve 累积**。
    // 2026-09-13 在 walk_c ep01 上量到的（两镜 dissolve，各 0.4 秒）：
    //
    //     ep01_sh001    画面 0.00   字幕 0.00   差 +0.00
    //     ep01_sh004    画面 13.58  字幕 13.18  差 -0.40
    //     ep01_sh009    画面 31.13  字幕 30.33  差 -0.80
    //     ep01_sh014_b  画面 59.50  字幕 58.70  差 -0.80
    //
    // 每一条单看都"差不多对"，连起来到片尾差了将近一秒。
    const ExactFrames exact;
    const auto paths = make_paths("溶解");
    touch(paths.shots("draft") / "a.mp4");

    auto s1 = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    auto s2 = make_shot("sh002", 4.0, "shots/draft/a.mp4");
    s2.transition_in = models::Transition::DISSOLVE;
    s2.transition_dur_s = 0.4;

    const auto tl = media::build_timeline({s1, s2}, paths,
                                          config::AssemblyConfig{});
    // 第二镜就接在第一镜后面，一秒不差。
    CHECK(tl.entries[1].start_s == doctest::Approx(5.0));
    // 字段本身照旧记着——它是意图，将来真做转场时要用。
    CHECK(tl.entries[1].transition_in == models::Transition::DISSOLVE);
    CHECK(tl.entries[1].transition_dur_s == doctest::Approx(0.4));

    SUBCASE("硬切同样不重叠") {
        auto s3 = make_shot("sh003", 4.0, "shots/draft/a.mp4");
        s3.transition_in = models::Transition::CUT;
        s3.transition_dur_s = 0.4;
        const auto t = media::build_timeline({s1, s3}, paths,
                                             config::AssemblyConfig{});
        CHECK(t.entries[1].start_s == doctest::Approx(5.0));
    }
}

TEST_CASE("缺视频时说清是哪一镜") {
    const auto paths = make_paths("缺视频");

    SUBCASE("根本没记路径") {
        models::Shot s;
        s.shot_id = "sh001";
        s.duration_s = 5.0;
        try {
            media::build_timeline({s}, paths, config::AssemblyConfig{});
            FAIL("该抛");
        } catch (const media::AssemblyError& e) {
            CHECK(std::string(e.what()).find("sh001") != std::string::npos);
        }
    }

    SUBCASE("记了路径但文件不在") {
        // 这两种要分开说：前者是流水线没跑到，后者是文件被删了或者
        // 项目挪过位置。下一步不一样。
        const auto s = make_shot("sh002", 5.0, "shots/draft/没有这个.mp4");
        try {
            media::build_timeline({s}, paths, config::AssemblyConfig{});
            FAIL("该抛");
        } catch (const media::AssemblyError& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("不见了") != std::string::npos);
        }
    }
}

TEST_CASE("统一规格：先按比例缩到框内再补边") {
    // 直接 scale 到目标尺寸会拉伸，而竖屏的电影里混进一个横屏镜头时，
    // 拉伸出来的人脸一眼就不对。
    config::AssemblyConfig cfg;
    cfg.fps = 24;
    const auto args = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4");
    const std::string vf = arg_value(args, "-vf");
    CAPTURE(vf);

    CHECK(vf.find("force_original_aspect_ratio=decrease") != std::string::npos);
    CHECK(vf.find("pad=1080:1920") != std::string::npos);
    // 采样宽高比也要统一，否则拼接处画面会横向拉伸
    CHECK(vf.find("setsar=1") != std::string::npos);
    CHECK(vf.find("fps=24") != std::string::npos);

    SUBCASE("时基统一到 90000") {
        // 不统一的话 concat 会在拼接处丢帧，表现是每一镜的开头卡一下。
        CHECK(arg_value(args, "-video_track_timescale") == "90000");
    }

    SUBCASE("这一步不带音频") {
        // 音频统一在后面处理。带上的话每一镜的音频会各自重编码一次。
        CHECK(has_flag(args, "-an"));
    }

    SUBCASE("编码参数按配置来") {
        config::AssemblyConfig c2;
        c2.video_codec = "libx265";
        c2.crf = 28;
        c2.pix_fmt = "yuv444p";
        const auto a = media::normalize_args("in.mp4", 640, 352, c2, "out.mp4");
        CHECK(arg_value(a, "-c:v") == "libx265");
        CHECK(arg_value(a, "-crf") == "28");
        CHECK(arg_value(a, "-pix_fmt") == "yuv444p");
    }
}

TEST_CASE("项目名里带撇号也要拼得出来") {
    // 清单写的是 file '<路径>'，而 concat 解析器在单引号内再遇到
    // 单引号就结束引用。项目叫 "don't" 的话，这一行断在半截，后面那截
    // 被当成别的记号。
    //
    // **代价不对称**：表现是最后装配那一步报一句 ffmpeg 的解析错，指的位置
    // 离真正的原因（项目叫什么名）十万八千里，而且是在整章都跑完之后才炸。
    const fs::path work = paths::from_utf8("C:\\库\\don't\\output");
    const std::vector<fs::path> clips = {work / "norm_0000.mp4"};
    const std::string listing = media::concat_listing(clips);
    CAPTURE(listing);

    // 规矩是 ' -> '\\''：收引用、转义的单引号、再开引用。
    CHECK(listing.find("don'\\''t") != std::string::npos);
    // 整行仍然是 file '...' 的形状，末尾带换行
    CHECK(listing.rfind("file '", 0) == 0);
    CHECK(listing.back() == '\n');

    // **引用必须闭合。** 只数**没被反斜杠转义**的那些单引号——
    // 转义掉的那个不参与开合。断在半截的话这个数是奇数。
    //
    // 第一版我直接数了所有单引号、要求偶数，结果被这条用例当场证伪：
    // '\'' 里有三个引号，其中一个是转义的，总数是奇数而引用是闭合的。
    int open_close = 0;
    for (std::size_t i = 0; i < listing.size(); ++i) {
        if (listing[i] != '\'') continue;
        if (i > 0 && listing[i - 1] == '\\') continue;   // 被转义的，不算
        ++open_close;
    }
    CHECK(open_close % 2 == 0);
}

TEST_CASE("拼接清单用正斜杠，拼接本身零重编码") {
    // **路径要用反斜杠拼出来。** 原来这里给的是正斜杠字面量，于是
    // "清单里没有反斜杠"那条断言**永远为真**——输入里本来就没有，
    // fwd() 那个转换一次都没被走到。
    //
    // 是这么发现的：把 concat_listing 里的 fwd(p) 换成 to_utf8(p)，
    // 全套 509 条**依然全绿**。
    //
    // 而真实调用方给的正是反斜杠——Windows 上 work / "norm_0000.mp4"
    // 拼出来就是反斜杠分隔的。
    const fs::path work =
        paths::from_utf8("C:\\项目 库\\雨夜天台\\output\\.work");
    const std::vector<fs::path> clips = {work / "norm_0000.mp4",
                                         work / "norm_0001.mp4"};
    // 前提先立住：这台机器上拼出来确实带反斜杠，否则下面那条又是空的
    REQUIRE(paths::to_utf8(clips[0]).find('\\') != std::string::npos);

    const std::string listing = media::concat_listing(clips);
    CAPTURE(listing);
    CHECK(listing.find('\\') == std::string::npos);
    CHECK(listing.find("file 'C:/项目 库/") != std::string::npos);
    // 每行一个，末尾要有换行——最后一行没换行的话 ffmpeg 会忽略它
    CHECK(listing.back() == '\n');

    const auto args = media::concat_args("list.txt", "joined.mp4");
    CHECK(arg_value(args, "-c") == "copy");   // 零重编码
    // -safe 0 是必需的：清单里是绝对路径，默认的安全检查会拒绝
    CHECK(arg_value(args, "-safe") == "0");
}

TEST_CASE("混音绝不能用 -shortest") {
    // **实测一次四镜头的片子丢了整整一个镜头。**
    // 配音总长几乎总是短于画面（无对白的镜头没有音频），
    // -shortest 会把成片截到配音那么长，后面的画面直接丢掉。
    config::AssemblyConfig cfg;
    const std::vector<media::AudioSegment> segs = {
        {"a1.wav", 0.0}, {"a2.wav", 2.5}, {"a3.wav", 5.0}};
    const auto args =
        media::mix_args("joined.mp4", segs, cfg, -16.0, -1.5, 9.0, "out.mp4");

    CHECK_FALSE(has_flag(args, "-shortest"));
    // 以视频为准截断补出来的静音尾巴
    CHECK(arg_value(args, "-t") == "9.000");

    const std::string chain = arg_value(args, "-filter_complex");
    CAPTURE(chain);

    SUBCASE("音轨补静音到视频长度") {
        CHECK(chain.find("apad") != std::string::npos);
    }

    SUBCASE("每条配音延迟到自己的位置，两个声道各给一个值") {
        // 只给一个值的话右声道不延迟，听起来是回声。
        CHECK(chain.find("adelay=0|0") != std::string::npos);
        CHECK(chain.find("adelay=2500|2500") != std::string::npos);
        CHECK(chain.find("adelay=5000|5000") != std::string::npos);
    }

    SUBCASE("loudnorm 之后要收回采样率") {
        // 这个滤镜内部按 192k 工作，不收回来的话编码器会挑个 96k
        // 之类的采样率，文件白白变大。
        const std::size_t ln = chain.find("loudnorm");
        REQUIRE(ln != std::string::npos);
        CHECK(chain.find("aresample=48000", ln) != std::string::npos);
    }

    SUBCASE("响度目标从参数来，不是写死的") {
        // 以前这里写死 -16，界面上改了响度目标其实一点用没有。
        const auto a = media::mix_args("j.mp4", segs, cfg, -14.0, -2.0, 9.0,
                                       "o.mp4");
        const std::string c = arg_value(a, "-filter_complex");
        CHECK(c.find("loudnorm=I=-14:TP=-2") != std::string::npos);
    }

    SUBCASE("响度目标不许被格式化截掉位数") {
        // **这里原来是 %g，会截到 6 位有效数字。** target_lufs 是配置里
        // 一个没有小数位限制的浮点数，填 -16.123456 的话 C++ 写出的是
        // -16.1235——**响度目标真的变了**，而 Python 那边写的是
        // f"{self.target_lufs}"（str(float)，不截位）。
        //
        // 不是显示问题：ffmpeg 照着 -16.1235 归一，成片响度和 Python 的
        // 不一样，而两边的命令行都"看着对"。
        const auto a = media::mix_args("j.mp4", segs, cfg, -16.123456, -1.234567,
                                       9.0, "o.mp4");
        const std::string c = arg_value(a, "-filter_complex");
        CAPTURE(c);
        CHECK(c.find("loudnorm=I=-16.123456:TP=-1.234567") != std::string::npos);
    }

    SUBCASE("整数值这里写 -16，Python 写 -16.0——有意留着的") {
        // ffmpeg 两个都当 -16 解析，成片一模一样。要逐字节一样得照搬
        // Python 的 repr 规则（整数浮点补 ".0"），为一个解析结果相同的
        // 字符串背那套算法不值。
        //
        // 钉住是因为**下一个人看见这处不一致，得知道它是被想过的**，
        // 而不是抄漏了——尤其是删掉 Python 之后再也没法比对的时候。
        const auto a = media::mix_args("j.mp4", segs, cfg, -16.0, -1.5, 9.0,
                                       "o.mp4");
        const std::string c = arg_value(a, "-filter_complex");
        CHECK(c.find("loudnorm=I=-16:TP=-1.5") != std::string::npos);
        CHECK(c.find("I=-16.0") == std::string::npos);
    }

    SUBCASE("视频原样拷贝") {
        // 混音不碰画面，重编码一次是白白多一次有损压缩。
        CHECK(arg_value(args, "-c:v") == "copy");
    }
}

TEST_CASE("没有配音时也要有音轨") {
    // 没音轨的话有些平台会认为文件损坏。
    config::AssemblyConfig cfg;
    const auto args = media::silent_audio_args("joined.mp4", cfg, "out.mp4");
    const std::string lavfi = arg_value(args, "-f");
    CHECK(lavfi == "lavfi");
    bool found = false;
    for (const auto& a : args) {
        if (a.find("anullsrc") != std::string::npos) found = true;
    }
    CHECK(found);
    // **这条路用 -shortest 是安全的**：静音源是无限长的，
    // 截到视频长度正是想要的结果。
    CHECK(has_flag(args, "-shortest"));
    CHECK(arg_value(args, "-c:v") == "copy");
}

TEST_CASE("烧字幕时路径里的冒号要转义") {
    // Windows 上 C:\x 里的冒号是滤镜的参数分隔符，反斜杠是转义符，
    // 直接传进去滤镜解析失败——而报错是滤镜语法错误，看不出根因是路径。
    const std::string e = media::escape_filter_path(
        paths::from_utf8("C:/项目 库/雨夜天台/subtitles/第一集.ass"));
    CAPTURE(e);
    CHECK(e.find("C\\:/") == 0);
    CHECK(e.find('\\') != std::string::npos);
    // 反斜杠已经换成正斜杠了，不该再有别的反斜杠
    CHECK(e.find("\\\\") == std::string::npos);

    const auto args = media::burn_args("in.mp4", paths::from_utf8("C:/a/b.ass"),
                                       config::AssemblyConfig{}, "out.mp4");
    const std::string vf = arg_value(args, "-vf");
    CAPTURE(vf);
    CHECK(vf.rfind("subtitles='", 0) == 0);
    CHECK(vf.back() == '\'');

    SUBCASE("烧字幕时音频原样拷贝") {
        CHECK(arg_value(args, "-c:a") == "copy");
    }
}

TEST_CASE("每条命令都带 -y") {
    // 不带的话 ffmpeg 遇到已存在的输出文件会问"覆盖吗"，
    // 而这个进程的 stdin 是关着的，它会一直等到超时。
    config::AssemblyConfig cfg;
    CHECK(has_flag(media::normalize_args("i.mp4", 640, 352, cfg, "o.mp4"), "-y"));
    CHECK(has_flag(media::concat_args("l.txt", "o.mp4"), "-y"));
    CHECK(has_flag(media::silent_audio_args("i.mp4", cfg, "o.mp4"), "-y"));
    CHECK(has_flag(media::mix_args("i.mp4", {{"a.wav", 0.0}}, cfg, -16.0, -1.5,
                                   5.0, "o.mp4"), "-y"));
    CHECK(has_flag(media::burn_args("i.mp4", "s.ass", cfg, "o.mp4"), "-y"));
}

TEST_CASE("装配前的字幕自检") {
    const auto paths = make_paths("自检");
    touch(paths.shots("draft") / "a.mp4");
    auto s = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    s.dialogue.push_back(line("正常的一句", 2.0, "c_lin"));
    s.dialogue.push_back(line("太快了", 0.2, "c_lin"));

    const auto tl = media::build_timeline({s}, paths, config::AssemblyConfig{});
    const auto problems =
        media::subtitle_problems(tl, config::AssemblyConfig{});
    REQUIRE_FALSE(problems.empty());
    bool said = false;
    for (const auto& p : problems) {
        if (p.find("看不清") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("路径里有单引号：以前会破掉滤镜，2026-09-11 补上了") {
    // `-vf subtitles='<路径>'` 是用单引号括起来的，路径里再出现一个
    // 单引号就会提前收尾，后面的字变成滤镜的其它参数，ffmpeg 报一句语法错。
    //
    // **以前这条用例钉的是"现状有个洞"**，理由是 Python 侧那个
    // _escape_filter_path 也只做了 \\ -> / 和 : -> \\:，单方面改会让对拍
    // 多一条不同。当时写的是"真要修就两边一起修（阶段 8 删掉 Python 之后
    // 就只剩一侧）"——**Python 已经删了，那个前提到了**，所以修了。
    //
    // 金语料里那条 subtitles 路径不带撇号，所以对拍一个字没变（确认过）。
    const std::string e =
        media::escape_filter_path(paths::from_utf8("C:/Bob's drama/a.ass"));
    CAPTURE(e);
    CHECK(e.find("C\\:/") == 0);              // 冒号照旧转义
    CHECK(e.find("Bob'\\''s") != std::string::npos);  // 撇号按 '\\'' 转

    // 拼进整条 -vf 之后，引用必须是闭合的（只数没被反斜杠转义的单引号）。
    const auto args = media::burn_args(
        "in.mp4", paths::from_utf8("C:/Bob's drama/a.ass"),
        config::AssemblyConfig{}, "out.mp4");
    const std::string vf = arg_value(args, "-vf");
    CAPTURE(vf);
    int open_close = 0;
    for (std::size_t i = 0; i < vf.size(); ++i) {
        if (vf[i] != '\'') continue;
        if (i > 0 && vf[i - 1] == '\\') continue;
        ++open_close;
    }
    CHECK(open_close % 2 == 0);
}

// ===========================================================================
// 和 Python 比 ffmpeg 的命令行。
//
// **上面那些用例钉的是我们自己的意图，不是"和 Python 一样"。** 两者的
// 差别不是学究：2026-09-08 抓到 loudnorm 的响度目标被 %g 截到 6 位有效
// 数字，而上面那条用例**把截过的值当成正确答案钉住了**
// （`loudnorm=I=-14:TP=-2`）。自己钉自己，钉错了照样绿。
//
// 语料由 cpp/tests/export_assemble_golden.py 生成，期望值全部来自
// **调真的 Python 函数**（假一个 FFmpeg，把 run_ffmpeg 收到的 argv 记下来）。
// ===========================================================================

namespace {

json commands_golden() {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/ffmpeg/commands.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

/// 和导出脚本里的 norm() 对应：反斜杠换正斜杠。
///
/// 语料里项目根已经换成了字面量 <ROOT>，所以这边直接拿 <ROOT> 当根用，
/// 路径就天然对得上。**放宽的只有路径的拼写，不是命令的语义。**
std::string fwd_slash(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/// Python 的 str(float) 和 C++ 的 %g 对整数值拼法不同：
/// Python 写 -16.0，我们写 -16。ffmpeg 两个都当 -16 解析，成片一模一样。
///
/// **只抹这一种差别**，别的照旧逐字节比。要逐字节一样得照搬 Python 的
/// repr 规则（整数浮点补 ".0"），为一个解析结果相同的字符串背那套算法
/// 不值——这个决定在 media/assemble.cpp 的 mix_args 里写着。
std::string drop_trailing_dot_zero(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        // 只在"数字 . 0"且后面不再跟数字时抹掉 ".0"
        if (s[i] == '.' && i + 1 < s.size() && s[i + 1] == '0' &&
            i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1])) &&
            (i + 2 >= s.size() ||
             !std::isdigit(static_cast<unsigned char>(s[i + 2])))) {
            ++i;   // 跳过 '0'
            continue;
        }
        out += s[i];
    }
    return out;
}

void check_argv(const std::vector<std::string>& got, const json& want,
                const std::string& what) {
    const auto expect = want.get<std::vector<std::string>>();
    CAPTURE(what);
    REQUIRE_MESSAGE(got.size() == expect.size(),
                    what << "：参数个数对不上，我们 " << got.size()
                         << " 条，Python " << expect.size() << " 条");
    for (std::size_t i = 0; i < expect.size(); ++i) {
        CAPTURE(i);
        const std::string mine = fwd_slash(got[i]);
        if (mine == expect[i]) continue;
        // 只有浮点拼法这一种差别可以放过
        CHECK_MESSAGE(drop_trailing_dot_zero(expect[i]) == mine,
                      what << " 第 " << i << " 个参数不一样：\n"
                           << "  我们  " << mine << "\n"
                           << "  Python " << expect[i]);
    }
}

/// 语料里的 argv 不含 -y（Python 在 run_ffmpeg 里统一加），
/// 而我们的 *_args() 自己带 -y。比之前去掉。
std::vector<std::string> without_y(std::vector<std::string> args) {
    const auto it = std::find(args.begin(), args.end(), "-y");
    REQUIRE_MESSAGE(it != args.end(), "我们这边每条命令都该带 -y");
    args.erase(it);
    return args;
}

config::AssemblyConfig golden_config() {
    return config::AssemblyConfig{};   // 语料是用两边的默认值导的
}

}  // namespace

TEST_CASE("统一规格的命令和 Python 一样") {
    const json g = commands_golden().at("normalize");
    const auto cfg = golden_config();
    const auto srcs = g.at("srcs").get<std::vector<std::string>>();
    const auto dests = g.at("dests").get<std::vector<std::string>>();
    REQUIRE(srcs.size() == g.at("calls").size());

    for (std::size_t i = 0; i < srcs.size(); ++i) {
        const auto args = media::normalize_args(
            paths::from_utf8(srcs[i]), g.at("target_w").get<int>(),
            g.at("target_h").get<int>(), cfg, paths::from_utf8(dests[i]));
        check_argv(without_y(args), g.at("calls")[i], "统一规格");
    }
}

TEST_CASE("拼接的清单和命令都和 Python 一样") {
    const json g = commands_golden().at("concat");

    std::vector<fs::path> clips;
    for (const auto& c : g.at("clips")) {
        clips.push_back(paths::from_utf8(c.get<std::string>()));
    }
    // 清单里的路径必须是正斜杠——ffmpeg 的 concat 分离器只认这个
    CHECK(media::concat_listing(clips) == g.at("listing").get<std::string>());

    const auto args = media::concat_args(
        paths::from_utf8(g.at("listing_path").get<std::string>()),
        paths::from_utf8(g.at("dest").get<std::string>()));
    check_argv(without_y(args), g.at("calls")[0], "拼接");
}

TEST_CASE("响度目标带小数时也和 Python 一样") {
    // **这条是整份语料存在的直接理由。** 上面那条混音用的是默认的
    // -16.0 / -1.5，而出问题的 %g 把这两个值渲染成 -16 / -1.5，
    // 抹掉 ".0" 之后和 Python 一样——**默认值根本试不出那个 bug**。
    // 响度目标得有 6 位以上有效数字才暴露得出来。
    const json g = commands_golden().at("mix_precise");

    std::vector<media::AudioSegment> segs;
    for (const auto& s : g.at("segments")) {
        segs.push_back({paths::from_utf8(s.at("path").get<std::string>()),
                        s.at("at_s").get<double>()});
    }
    const auto args = media::mix_args(
        paths::from_utf8(g.at("video").get<std::string>()), segs,
        golden_config(), g.at("target_lufs").get<double>(),
        g.at("max_true_peak_db").get<double>(),
        g.at("video_duration_s").get<double>(),
        paths::from_utf8(g.at("dest").get<std::string>()));
    check_argv(without_y(args), g.at("calls")[0], "混音（高精度响度）");
}

TEST_CASE("混音的滤镜链和 Python 一样") {
    // 这一条是全套里最容易飘的：延迟、混音、响度归一、补静音，
    // 四段拼成一个字符串，任何一处差一个字符 ffmpeg 都会整条拒绝，
    // 或者更糟——照着跑出一个音画错位的成片。
    const json g = commands_golden().at("mix");

    std::vector<media::AudioSegment> segs;
    for (const auto& s : g.at("segments")) {
        segs.push_back({paths::from_utf8(s.at("path").get<std::string>()),
                        s.at("at_s").get<double>()});
    }
    const auto args = media::mix_args(
        paths::from_utf8(g.at("video").get<std::string>()), segs,
        golden_config(), g.at("target_lufs").get<double>(),
        g.at("max_true_peak_db").get<double>(),
        g.at("video_duration_s").get<double>(),
        paths::from_utf8(g.at("dest").get<std::string>()));
    check_argv(without_y(args), g.at("calls")[0], "混音");
}

TEST_CASE("没有配音时的静音轨命令和 Python 一样") {
    // 没有音轨的话有些平台会认为文件损坏。
    const json g = commands_golden().at("silent_audio");
    const auto args = media::silent_audio_args(
        paths::from_utf8(g.at("video").get<std::string>()), golden_config(),
        paths::from_utf8(g.at("dest").get<std::string>()));
    check_argv(without_y(args), g.at("calls")[0], "静音轨");
}

TEST_CASE("烧字幕的命令和 Python 一样") {
    const json g = commands_golden().at("burn");
    const auto args = media::burn_args(
        paths::from_utf8(g.at("video").get<std::string>()),
        paths::from_utf8(g.at("ass_path").get<std::string>()), golden_config(),
        paths::from_utf8(g.at("dest").get<std::string>()));
    check_argv(without_y(args), g.at("calls")[0], "烧字幕");
}

TEST_CASE("滤镜里的路径转义和 Python 逐字节一样") {
    // 上一条里的路径在语料里已经换成了 <ROOT>，冒号就没了，
    // 所以转义那一步在那条命令里其实没被走到。这里单独拿真路径过。
    for (const auto& c : commands_golden().at("escape_filter_path")) {
        const std::string in = c.at("input").get<std::string>();
        CAPTURE(in);
        CHECK(media::escape_filter_path(paths::from_utf8(in)) ==
              c.at("expected").get<std::string>());
    }
}

TEST_CASE("时间线的起点和字幕时间戳和 Python 一样") {
    const ExactFrames exact;
    // 混音里每一段的偏移量全从这里来。时间线错了，adelay 也就错了，
    // 而上面那条用例是拿语料里的 segments 直接喂的，绕过了这一步。
    const json g = commands_golden().at("timeline");

    const auto root = temp_root("语料时间线");
    models::ProjectPaths paths(root);
    paths.ensure();
    touch(paths.abs("shots/draft/a.mp4"));
    touch(paths.abs("shots/draft/b.mp4"));
    touch(paths.abs("audio/a_1.wav"));
    touch(paths.abs("audio/a_2.wav"));

    models::Shot a = make_shot("ep01_sh001", 6.0, "shots/draft/a.mp4");
    a.transition_in = models::Transition::CUT;
    a.transition_dur_s = 0.0;
    a.dialogue = {line("你终于来了。", 2.5, std::string("c_lin_wan"),
                       "audio/a_1.wav"),
                  line("雨下了一整夜。", 2.0, std::nullopt, "audio/a_2.wav")};
    models::Shot b = make_shot("ep01_sh002", 5.0, "shots/draft/b.mp4");
    b.transition_in = models::Transition::DISSOLVE;
    b.transition_dur_s = 0.4;

    std::vector<models::Shot> shots = {a, b};
    config::AssemblyConfig cfg;
    const auto tl = media::build_timeline(shots, paths, cfg);

    REQUIRE(tl.entries.size() == g.size());
    for (std::size_t i = 0; i < tl.entries.size(); ++i) {
        CAPTURE(i);
        const auto& e = tl.entries[i];
        CHECK(e.shot_id == g[i].at("shot_id").get<std::string>());
        CHECK(e.start_s == doctest::Approx(g[i].at("start_s").get<double>()));
        CHECK(e.duration_s ==
              doctest::Approx(g[i].at("duration_s").get<double>()));
        REQUIRE(e.cues.size() == g[i].at("cues").size());
        for (std::size_t k = 0; k < e.cues.size(); ++k) {
            CAPTURE(k);
            const auto& want = g[i].at("cues")[k];
            CHECK(e.cues[k].start_s ==
                  doctest::Approx(want.at("start_s").get<double>()));
            CHECK(e.cues[k].end_s ==
                  doctest::Approx(want.at("end_s").get<double>()));
            CHECK(e.cues[k].text == want.at("text").get<std::string>());
            CHECK(e.cues[k].style == want.at("style").get<std::string>());
        }
    }
}


// ---------------------------------------------------------------------------
// build_timeline 排出来的时间线，和 Python 逐条比。
//
// 上面那些用例比的是**由时间线拼出来的 ffmpeg 命令**，命令里只看得见
// 一部分（adelay 的偏移）。时间线里最要紧的那部分——**字幕的时间戳**——
// 走的是另一条路（烧进 .ass），命令里一个字都看不到。
// `coverage_audit.py` 一直把 Timeline / TimelineEntry / build_timeline
// 列在没碰过的符号里。
//
// **这是音画对齐的最后一环。** 每条字幕的起止时间来自
// `line.actual_duration_s`——配音阶段量出来写回台词的那个数
// （那一步由 audio_stage.json 钉住），这里再把它累成时间轴。
// 算错了不报错：字幕生成成功、成片渲染成功、总时长也对，
// 只是字幕比人声早半秒或晚半秒，而且一章下来越飘越远，
// 每一条单看都"差不多对"。
// ---------------------------------------------------------------------------

TEST_CASE("时间线的排期和字幕时间戳和 Python 一样") {
    const ExactFrames exact;
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/timeline.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json g;
    in >> g;

    const auto cases = g.at("cases");
    REQUIRE(cases.size() == 13);

    const fs::path root = fs::temp_directory_path() /
                          paths::from_utf8("changji_时间线语料");
    std::error_code ec;
    fs::remove_all(root, ec);
    const models::ProjectPaths pp(root);
    touch(root / "audio" / "x.wav");

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const auto want = c.at("timeline");

        // 语料记的是**排完之后**的样子，输入要从里面还原：
        // 每个 entry 的 shot_id / duration_s / 转场是输入，
        // start_s 和 cues 是被测的产物。台词从 cues 还原不出来
        // （不出字幕的那几种正是要覆盖的），所以按用例名单独给。
        std::vector<models::Shot> shots;
        for (const auto& e : want.at("entries")) {
            const std::string vid = e.at("video_path").get<std::string>();
            touch(root / paths::from_utf8(vid));
            auto s = make_shot(e.at("shot_id").get<std::string>(),
                               e.at("duration_s").get<double>(), vid);
            const std::string tr = e.at("transition_in").get<std::string>();
            s.transition_in = tr == "dissolve" ? models::Transition::DISSOLVE
                            : tr == "fade_in"  ? models::Transition::FADE_IN
                            : tr == "fade_out" ? models::Transition::FADE_OUT
                            : tr == "whip"     ? models::Transition::WHIP
                                               : models::Transition::CUT;
            s.transition_dur_s = e.at("transition_dur_s").get<double>();
            shots.push_back(s);
        }
        // 台词由语料另给（见 dialogue_in 字段）
        const auto& din = c.at("dialogue_in");
        REQUIRE(din.size() == shots.size());
        for (std::size_t i = 0; i < shots.size(); ++i) {
            for (const auto& l : din[i]) {
                auto dl = line(l.at("text").get<std::string>(),
                               0.0,
                               l.at("char_id").is_null()
                                   ? std::optional<std::string>{}
                                   : l.at("char_id").get<std::string>(),
                               l.at("audio_path").is_null()
                                   ? std::string()
                                   : l.at("audio_path").get<std::string>());
                if (l.at("actual_duration_s").is_null()) {
                    dl.actual_duration_s.reset();
                } else {
                    dl.actual_duration_s =
                        l.at("actual_duration_s").get<double>();
                }
                shots[i].dialogue.push_back(dl);
            }
        }

        const auto tl = media::build_timeline(shots, pp, config::AssemblyConfig{});

        CHECK(tl.total_duration_s() ==
              doctest::Approx(want.at("total_duration_s").get<double>()));

        const auto want_entries = want.at("entries");
        REQUIRE(tl.entries.size() == want_entries.size());
        for (std::size_t i = 0; i < tl.entries.size(); ++i) {
            CAPTURE(i);
            const auto& got = tl.entries[i];
            const auto& w = want_entries[i];
            CHECK(got.shot_id == w.at("shot_id").get<std::string>());
            // **起点**：溶解要把它往回挪，漏了这一步整章字幕全偏
            CHECK(got.start_s == doctest::Approx(w.at("start_s").get<double>()));
            CHECK(got.duration_s ==
                  doctest::Approx(w.at("duration_s").get<double>()));
            CHECK(got.audio_paths.size() ==
                  w.at("audio_paths").size());

            const auto want_cues = w.at("cues");
            REQUIRE(got.cues.size() == want_cues.size());
            for (std::size_t j = 0; j < got.cues.size(); ++j) {
                CAPTURE(j);
                CHECK(got.cues[j].start_s ==
                      doctest::Approx(want_cues[j].at("start_s").get<double>()));
                CHECK(got.cues[j].end_s ==
                      doctest::Approx(want_cues[j].at("end_s").get<double>()));
                CHECK(got.cues[j].text ==
                      want_cues[j].at("text").get<std::string>());
                CHECK(got.cues[j].style ==
                      want_cues[j].at("style").get<std::string>());
            }
        }

        // 摊平之后的那一份也比一遍——烧字幕读的是它
        const auto all = tl.cues();
        const auto want_all = want.at("all_cues");
        REQUIRE(all.size() == want_all.size());
        for (std::size_t i = 0; i < all.size(); ++i) {
            CAPTURE(i);
            CHECK(all[i].start_s ==
                  doctest::Approx(want_all[i].at("start_s").get<double>()));
            CHECK(all[i].text == want_all[i].at("text").get<std::string>());
        }
    }
}

TEST_CASE("成片文件名属于哪一章") {
    // **`_NN` 是老项目留下的名字**：2026-09-18 之前一章会在章内切成几段，
    // 切出来的叫 ep01_01 / ep01_02，不切的那次叫 ep01。今天一章只出一个
    // ep01.mp4，可老项目盘上还躺着那种名字，于是"这个文件属于哪一章"
    // 有两处要判：装配时清上一版留下的孤儿，列表时数"几章出片了"。
    // 2026-09-17 撞过：两处各写一遍，清理那边认两种、计数那边只认一种，
    // 切过的章在「N/M 章已出片」里全被漏掉。判法收在这一个函数上。
    using media::episode_of_output;
    CHECK(episode_of_output("ep01") == "ep01");
    CHECK(episode_of_output("ep01_01") == "ep01");
    CHECK(episode_of_output("ep01_12") == "ep01");
    // **不用子串**：短 id 拿子串会命中所有 epNN
    CHECK(episode_of_output("ep") == "ep");
    CHECK(episode_of_output("ep0112") == "ep0112");   // 没有下划线，不是老项目切出来的
    CHECK(episode_of_output("ep01_ab") == "ep01_ab"); // 后两位不是数字
    CHECK(episode_of_output("ep01_1") == "ep01_1");   // 只有一位数字
    CHECK_FALSE(episode_of_output("").has_value());
}
