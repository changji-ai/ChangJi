// 质量闸门的测试。
//
// ffmpeg 是脚本化的假的：按参数分流，问 probe 回 ffprobe 的 JSON，
// 问 signalstats 回亮度统计。这样整条判定链能真跑一遍，
// 而不是把 MediaInfo 直接塞进去——那样就测不到"probe 失败怎么办"。
//
// 闸门是无人值守下唯一阻止废片流入成片的机制，所以这里盯的是两件事：
// **判错档次的后果**（RETRY 和 REGRESS 的下一步完全不同），
// 以及**失败时说的话有没有用**。

#include "util/say.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "gates/checks.hpp"
#include "util/paths.hpp"

// ⚠️ 路径一律走 paths::from_utf8。直接把带中文的窄字符串字面量交给
// fs::path，MSVC 会按 ANSI 代码页解释，抛
// "No mapping for the Unicode character exists in the target multi-byte
// code page"。这条在 verify/RESULTS.md 里记过，写测试时照样会忘。

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 造一个够大的假视频文件（闸门先看文件大小）。
fs::path fake_video(const std::string& tag, std::size_t bytes = 4096) {
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_闸门");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path p = dir / paths::from_utf8(tag + ".mp4");
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << std::string(bytes, 'x');
    return p;
}

std::string probe_json(double duration, int w, int h, bool audio = false) {
    std::string streams =
        R"({"codec_type":"video","width":)" + std::to_string(w) +
        R"(,"height":)" + std::to_string(h) +
        R"(,"r_frame_rate":"24/1","nb_frames":"121","pix_fmt":"yuv420p"})";
    if (audio) streams += R"(,{"codec_type":"audio","codec_name":"aac"})";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", duration);
    return R"({"streams":[)" + streams + R"(],"format":{"duration":")" + buf +
           R"("}})";
}

std::string stats(double ymin, double ylow, double yavg, double yhigh,
                  double ymax) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "frame:0    pts:0\n"
                  "lavfi.signalstats.YMIN=%g\n"
                  "lavfi.signalstats.YLOW=%g\n"
                  "lavfi.signalstats.YAVG=%g\n"
                  "lavfi.signalstats.YHIGH=%g\n"
                  "lavfi.signalstats.YMAX=%g\n",
                  ymin, ylow, yavg, yhigh, ymax);
    return buf;
}

const std::string kNormalFrame = stats(16, 48, 112.5, 201, 235);
const std::string kBlankFrame = stats(16, 126, 127.2, 130, 250);

/// 按参数分流的假 ffmpeg。
struct FakeFF {
    std::string probe_out = probe_json(5.0, 448, 768);
    /// 依次回这些亮度统计，用光了重复最后一条。
    std::vector<std::string> frames = {kNormalFrame};
    std::string loudnorm_out;
    /// tblend 那条运动量命令回这个。默认给一段"会动、没硬切"的：
    /// 三帧差 2.0 / 3.0 / 4.0。留空 = 老 ffmpeg 没这个滤镜，量不到。
    std::string motion_out =
        "frame:0 pts:512 pts_time:0.04\nlavfi.signalstats.YAVG=2.0\n"
        "frame:1 pts:1024 pts_time:0.08\nlavfi.signalstats.YAVG=4.0\n"
        "frame:2 pts:1536 pts_time:0.12\nlavfi.signalstats.YAVG=3.0\n";
    bool probe_fails = false;
    bool stats_fail = false;
    mutable std::size_t frame_i = 0;

    media::Runner runner() const {
        return [this](const std::string&, const std::vector<std::string>& args,
                      double) {
            media::ProcResult r;
            r.launched = true;
            const auto has = [&args](const std::string& s) {
                return std::find(args.begin(), args.end(), s) != args.end();
            };
            const auto contains = [&args](const std::string& needle) {
                for (const auto& a : args) {
                    if (a.find(needle) != std::string::npos) return true;
                }
                return false;
            };

            if (has("-show_streams")) {
                if (probe_fails) {
                    r.exit_code = 1;
                    r.out = "moov atom not found";
                    return r;
                }
                r.out = probe_out;
                return r;
            }
            // 运动量那条也带 signalstats，但它是整段的差分，先分出去，
            // 否则会吃掉 frames 序列里的一条、把后面取样点的顺序带偏。
            if (contains("tblend")) {
                r.out = motion_out;
                return r;
            }
            if (contains("signalstats")) {
                if (stats_fail) {
                    r.exit_code = 1;
                    r.out = "decode error";
                    return r;
                }
                r.out = frames[std::min(frame_i++, frames.size() - 1)];
                return r;
            }
            if (contains("loudnorm")) {
                r.out = loudnorm_out;
                return r;
            }
            return r;
        };
    }

    media::FFmpeg ff() const { return {"ffmpeg", "ffprobe", runner()}; }
};

models::Shot make_shot(const std::string& id = "ep01_sh001") {
    models::Shot s;
    s.shot_id = id;
    s.duration_s = 5.0;
    return s;
}

bool mentions(const gates::GateResult& r, const std::string& needle) {
    for (const auto& s : r.reasons) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("正常的一镜通过") {
    FakeFF fake;
    const auto r = gates::gate_video(make_shot(), fake_video("正常"), fake.ff(),
                                     config::GateConfig{}, 5.0,
                                     std::make_pair(448, 768));
    CAPTURE(r.describe());
    CHECK(r.ok());
    CHECK(r.metrics.at("width") == 448.0);
    CHECK(r.metrics.at("duration_s") == doctest::Approx(5.0));
}

TEST_CASE("时长和分辨率不符是 REGRESS，不是 RETRY") {
    // **这两档的下一步完全不同。** 帧数算错了、档位参数没生效，
    // 换个种子重跑还是一样的结果——判成 RETRY 会白跑三次然后降级，
    // 而真正该做的是退回上一阶段。
    SUBCASE("时长差太多") {
        FakeFF fake;
        fake.probe_out = probe_json(1.0, 448, 768);   // 期望 5 秒，实际 1 秒
        const auto r = gates::gate_video(make_shot(), fake_video("短了"),
                                         fake.ff(), config::GateConfig{}, 5.0);
        CHECK(r.verdict == gates::Verdict::Regress);
        CHECK(mentions(r, "帧数算错"));
        // 差多少要报出来，只说"不对"没法判断是差一点还是差一半
        CHECK(r.metrics.at("duration_drift_s") == doctest::Approx(4.0));
    }

    SUBCASE("分辨率不符") {
        FakeFF fake;
        fake.probe_out = probe_json(5.0, 640, 352);
        const auto r = gates::gate_video(make_shot(), fake_video("尺寸"),
                                         fake.ff(), config::GateConfig{}, 5.0,
                                         std::make_pair(448, 768));
        CHECK(r.verdict == gates::Verdict::Regress);
        CHECK(mentions(r, "档位参数没生效"));
    }

    SUBCASE("时长差在容差内就放过") {
        // 容差是 max(0.5, 期望*0.2)。5 秒的镜头容差是 1 秒。
        FakeFF fake;
        fake.probe_out = probe_json(5.9, 448, 768);
        const auto r = gates::gate_video(make_shot(), fake_video("略长"),
                                         fake.ff(), config::GateConfig{}, 5.0);
        CHECK(r.ok());
    }
}

TEST_CASE("纯色画面被拦下，而且说清是哪一段") {
    // 片头纯色多半是模型没起来，片尾纯色多半是帧数超了模型的上限，
    // 两者的下一步完全不同。只说"画面近乎纯色"等于什么都没说。
    FakeFF fake;
    fake.frames = {kNormalFrame, kNormalFrame, kBlankFrame};
    const auto r = gates::gate_video(make_shot(), fake_video("片尾纯色"),
                                     fake.ff(), config::GateConfig{});
    CHECK(r.verdict == gates::Verdict::Retry);
    CHECK(mentions(r, "片尾"));
    CHECK(mentions(r, "近乎纯色"));
    CHECK_FALSE(mentions(r, "片头"));

    SUBCASE("三段都纯色时三个位置都点名") {
        FakeFF all;
        all.frames = {kBlankFrame};
        const auto r2 = gates::gate_video(make_shot(), fake_video("全纯色"),
                                          all.ff(), config::GateConfig{});
        CHECK(mentions(r2, "片头"));
        CHECK(mentions(r2, "片中"));
        CHECK(mentions(r2, "片尾"));
    }
}

TEST_CASE("同一件事不说两遍") {
    // 展布低于阈值和"近乎纯色"是同一个现象。两句都报的话，
    // 用户要从里面判断这是一个问题还是两个。
    FakeFF fake;
    fake.frames = {kBlankFrame};
    const auto r = gates::gate_video(make_shot(), fake_video("重复"), fake.ff(),
                                     config::GateConfig{});
    CHECK(mentions(r, "近乎纯色"));
    CHECK_FALSE(mentions(r, "细节偏少"));
}

TEST_CASE("亮度剧烈跳变说明中途崩坏") {
    // 只看一帧会漏掉这种：片头片尾都正常，中间那段崩了。
    FakeFF fake;
    fake.frames = {stats(16, 48, 30.0, 201, 235),
                   stats(16, 48, 200.0, 201, 235),
                   stats(16, 48, 35.0, 201, 235)};
    const auto r = gates::gate_video(make_shot(), fake_video("跳变"), fake.ff(),
                                     config::GateConfig{});
    CHECK(r.verdict == gates::Verdict::Retry);
    CHECK(mentions(r, "中途崩坏"));
    CHECK(r.metrics.at("mean_swing") == doctest::Approx(170.0));
}

TEST_CASE("文件问题一律 RETRY") {
    // 这一类换个种子重跑真的可能就好了（磁盘满了、下载断了）。
    SUBCASE("文件不存在") {
        FakeFF fake;
        const auto r = gates::gate_video(make_shot(), paths::from_utf8("Z:/没有这个.mp4"),
                                         fake.ff(), config::GateConfig{});
        CHECK(r.verdict == gates::Verdict::Retry);
        CHECK(mentions(r, "不存在"));
    }

    SUBCASE("文件几乎是空的") {
        // 先看大小再 probe：省一次子进程，而且 ffprobe 对零字节文件的
        // 报错很难懂。
        FakeFF fake;
        const auto r = gates::gate_video(make_shot(), fake_video("空的", 10),
                                         fake.ff(), config::GateConfig{});
        CHECK(r.verdict == gates::Verdict::Retry);
        CHECK(mentions(r, "几乎是空的"));
    }

    SUBCASE("读不出来时把 ffmpeg 的话带上") {
        FakeFF fake;
        fake.probe_fails = true;
        const auto r = gates::gate_video(make_shot(), fake_video("坏文件"),
                                         fake.ff(), config::GateConfig{});
        CHECK(r.verdict == gates::Verdict::Retry);
        CHECK(mentions(r, "moov atom"));
    }

    SUBCASE("没有视频轨") {
        FakeFF fake;
        fake.probe_out = R"({"streams":[{"codec_type":"audio"}],)"
                         R"("format":{"duration":"5.0"}})";
        const auto r = gates::gate_video(make_shot(), fake_video("没画面"),
                                         fake.ff(), config::GateConfig{});
        CHECK(r.verdict == gates::Verdict::Retry);
        CHECK(mentions(r, "没有视频轨"));
    }
}

TEST_CASE("配音装不进镜头是 REGRESS") {
    // 这个检查放在装配之前。装完再发现装不下就得重做整章。
    models::Shot s = make_shot();
    models::DialogueLine line;
    line.char_id = "c_lin_wan";
    line.text = "我等了你三年。";
    line.actual_duration_s = 6.0;
    s.dialogue.push_back(line);

    FakeFF fake;
    fake.probe_out = probe_json(5.0, 448, 768);
    const auto r = gates::gate_audio_sync(s, fake_video("装不下"), fake.ff(),
                                          config::GateConfig{});
    CHECK(r.verdict == gates::Verdict::Regress);
    CHECK(mentions(r, "装不进"));
    // 要说清超出多少、下一步做什么
    CHECK(mentions(r, "重新锁定时长"));
    CHECK(r.metrics.at("slack_s") == doctest::Approx(-1.0));

    SUBCASE("装得下就通过") {
        FakeFF ok;
        ok.probe_out = probe_json(7.0, 448, 768);
        const auto r2 = gates::gate_audio_sync(s, fake_video("装得下"), ok.ff(),
                                               config::GateConfig{});
        CHECK(r2.ok());
    }

    SUBCASE("没有台词的镜头直接通过，不去 probe") {
        const auto r3 = gates::gate_audio_sync(make_shot(),
                                               paths::from_utf8("Z:/根本不存在.mp4"),
                                               fake.ff(), config::GateConfig{});
        CHECK(r3.ok());
    }
}

TEST_CASE("成片检查：时长容差比单镜宽") {
    // 几十镜拼起来，每镜零点几秒的误差累积是正常的。
    // 用单镜那套 20% 的容差，正常的成片会被判不合格。
    FakeFF fake;
    fake.probe_out = probe_json(181.0, 1080, 1920, true);
    fake.loudnorm_out =
        R"({"input_i":"-16.1","input_tp":"-1.6","output_i":"-16.0"})";

    const auto r = gates::gate_episode(fake_video("成片"), fake.ff(),
                                       config::GateConfig{}, 180.0);
    CHECK(r.ok());
    CHECK(r.metrics.at("lufs") == doctest::Approx(-16.1));

    SUBCASE("差太多还是要报") {
        FakeFF bad;
        bad.probe_out = probe_json(150.0, 1080, 1920, true);
        bad.loudnorm_out = R"({"input_i":"-16.0","input_tp":"-1.6"})";
        const auto r2 = gates::gate_episode(fake_video("短片"), bad.ff(),
                                            config::GateConfig{}, 180.0);
        CHECK_FALSE(r2.ok());
    }

    SUBCASE("没有音轨要报") {
        // 成片没声音是最容易漏掉的：画面一切正常，人不会去点开听。
        FakeFF mute;
        mute.probe_out = probe_json(180.0, 1080, 1920, false);
        const auto r3 = gates::gate_episode(fake_video("哑片"), mute.ff(),
                                            config::GateConfig{}, 180.0);
        CHECK_FALSE(r3.ok());
        bool said = false;
        for (const auto& s : r3.reasons) {
            if (s.find("没有音轨") != std::string::npos) said = true;
        }
        CHECK(said);
    }

    SUBCASE("响度偏离目标要报") {
        FakeFF loud;
        loud.probe_out = probe_json(180.0, 1080, 1920, true);
        loud.loudnorm_out = R"({"input_i":"-9.0","input_tp":"-1.6"})";
        const auto r4 = gates::gate_episode(fake_video("响"), loud.ff(),
                                            config::GateConfig{}, 180.0);
        CHECK_FALSE(r4.ok());
        CHECK(r4.metrics.at("lufs") == doctest::Approx(-9.0));
    }
}

TEST_CASE("重试超限时降级，不是停下来") {
    // 无人值守跑一晚上，为一镜停住等于整晚白熬。降级（留着最后那一版）
    // 至少整章能出片，问题记录下来事后查。
    config::GateConfig cfg;
    cfg.max_attempts_per_shot = 3;

    gates::GateResult failed;
    failed.shot_id = "ep01_sh001";
    failed.verdict = gates::Verdict::Retry;

    models::Shot s = make_shot();
    s.attempts = 0;
    CHECK(gates::decide_next(failed, s, cfg) == gates::Verdict::Retry);
    s.attempts = 1;
    CHECK(gates::decide_next(failed, s, cfg) == gates::Verdict::Retry);
    s.attempts = 2;   // 这次失败就是第三次
    CHECK(gates::decide_next(failed, s, cfg) == gates::Verdict::Fallback);

    SUBCASE("关掉降级就退回上一阶段") {
        cfg.fallback_on_exhausted = false;
        s.attempts = 2;
        CHECK(gates::decide_next(failed, s, cfg) == gates::Verdict::Regress);
    }

    SUBCASE("REGRESS 不看重试次数") {
        // 重跑也没用，问题在上一阶段。数着次数重试三遍是纯浪费——
        // 成片档一镜几分钟。
        gates::GateResult regress = failed;
        regress.verdict = gates::Verdict::Regress;
        s.attempts = 0;
        CHECK(gates::decide_next(regress, s, cfg) == gates::Verdict::Regress);
    }

    SUBCASE("过了就是过了") {
        gates::GateResult pass;
        pass.verdict = gates::Verdict::Pass;
        s.attempts = 99;
        CHECK(gates::decide_next(pass, s, cfg) == gates::Verdict::Pass);
    }
}

TEST_CASE("概览只列没过的") {
    // 全过时人不需要读四十行"通过"。
    std::vector<gates::GateResult> results;
    for (int i = 0; i < 3; ++i) {
        gates::GateResult r;
        r.shot_id = "ep01_sh00" + std::to_string(i + 1);
        r.verdict = gates::Verdict::Pass;
        r.gate = "画面闸门";
        results.push_back(r);
    }
    gates::GateResult bad;
    bad.shot_id = "ep01_sh004";
    bad.verdict = gates::Verdict::Retry;
    bad.gate = "画面闸门";
    bad.reasons = {"片尾的画面近乎纯色"};
    results.push_back(bad);

    const std::string s = gates::summarize(results);
    CAPTURE(s);
    CHECK(s.find("检查 4 个镜头，通过 3 个") != std::string::npos);
    CHECK(s.find("ep01_sh004") != std::string::npos);
    CHECK(s.find("ep01_sh001") == std::string::npos);   // 过了的不列

    SUBCASE("一个都没有时也有话说") {
        CHECK(gates::summarize({}) == "没有需要检查的镜头");
    }
}

// ---------------------------------------------------------------------------
// 画面闸门的判定，和 Python 逐条比。
//
// 上面那些用例钉的是我们自己的意图。contract_audit.py 把这个文件归在
// 「两边都有、又没有语料兜着」那一类，而闸门的判定决定一镜是重试、
// 退回上一阶段、还是降级——**两边判得不一样，就是同一段素材在两个后端上
// 得到不同的质量结论**。
//
// 零件本来就对得上（spread < 8、mean < 6 || > 249 两边一模一样）。
// 要比的是**把零件组装成一个判定**那一步——今天已经在别处栽过三次这个
// 形状：装配层的响度、视频后端的风格线、首帧的尺寸节点名单。
//
// 两边造假的层次不同：Python 假 sample_pixel_stats()（回对象），
// C++ 假 Runner（回 ffmpeg 的文本）。所以**输入也进语料**，
// 这边按同一组数把文本造出来——靠用例名去对应输入的话，
// 那边改个数字这边就悄悄不一样了。
// ---------------------------------------------------------------------------

namespace {

nlohmann::json gates_golden() {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/gates.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    nlohmann::json j;
    in >> j;
    return j;
}

/// 把 mean / spread 还原成一段 signalstats 输出。
/// spread = high - low，mean = YAVG，见 parse_signalstats。
std::string stats_from(double mean, double spread) {
    return stats(0.0, mean - spread / 2.0, mean, mean + spread / 2.0, 255.0);
}

}  // namespace

TEST_CASE("画面闸门的判定和 Python 一条一条对得上") {
    const nlohmann::json g = gates_golden();
    const auto cases = g.at("cases");
    // 语料读空了的话循环一次都不转，而用例照样绿。
    REQUIRE(cases.size() == 14);

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        FakeFF fake;
        fake.probe_fails = c.at("probe_raises").get<bool>();
        fake.stats_fail = c.at("stats_raises").get<bool>();

        if (!c.at("probe").is_null()) {
            const auto& pr = c.at("probe");
            const double dur = pr.at("duration_s").get<double>();
            const int w = pr.at("width").get<int>();
            const int h = pr.at("height").get<int>();
            if (pr.at("has_video").get<bool>()) {
                fake.probe_out = probe_json(dur, w, h);
            } else {
                // 只有音轨：探测得到，但没有视频流
                fake.probe_out =
                    R"({"streams":[{"codec_type":"audio","codec_name":"aac"}],)"
                    R"("format":{"duration":"4.000000"}})";
            }
        }

        fake.frames.clear();
        for (const auto& s : c.at("samples")) {
            fake.frames.push_back(stats_from(s.at("mean").get<double>(),
                                             s.at("spread").get<double>()));
        }
        // 语料里样本为空表示"取不到画面"。FakeFF 用光了会重复最后一条，
        // 所以空列表要单独处理成"回一段解析不出东西的文本"。
        const bool no_samples = fake.frames.empty();
        if (no_samples) fake.frames.push_back("");

        // file_bytes 为空表示"文件不存在"——给一个没建出来的路径。
        fs::path video;
        if (c.at("file_bytes").is_null()) {
            video = fs::temp_directory_path() /
                    paths::from_utf8("changji_闸门/没这个文件.mp4");
            std::error_code ec;
            fs::remove(video, ec);
        } else {
            video = fake_video("语料_" + name,
                               c.at("file_bytes").get<std::size_t>());
        }

        std::optional<double> want_dur;
        if (!c.at("expected_duration_s").is_null()) {
            want_dur = c.at("expected_duration_s").get<double>();
        }
        std::optional<std::pair<int, int>> want_size;
        if (!c.at("expected_size").is_null()) {
            const auto v = c.at("expected_size").get<std::vector<int>>();
            want_size = std::pair{v[0], v[1]};
        }

        config::GateConfig cfg;
        const auto r = gates::gate_video(make_shot(), video, fake.ff(), cfg,
                                         want_dur, want_size, "画面闸门");

        // **判定是契约。** 它决定这一镜是重试、退回还是降级。
        CHECK(std::string(gates::to_string(r.verdict)) ==
              c.at("verdict").get<std::string>());
    }
}

TEST_CASE("运动量只报数不判：进 metrics、挂在「通过闸门」后面") {
    // 2026-09-13：几乎不动的片子（均值 0.27～0.66）和中途硬切的片子
    // （最大 45 / 中位 2.2）都能过闸门。先把数露出来，攒够再定阈值。
    FakeFF f;
    models::Shot shot;
    shot.shot_id = "ep01_sh001";
    config::GateConfig cfg;

    SUBCASE("量到了：均值、中位、最大都进 metrics，判定不变") {
        const auto r = gates::gate_video(shot, fake_video("运动"), f.ff(), cfg);
        CHECK(r.ok());
        CHECK(r.metrics.at("motion_mean") == doctest::Approx(3.0));
        CHECK(r.metrics.at("motion_median") == doctest::Approx(3.0));
        CHECK(r.metrics.at("motion_max") == doctest::Approx(4.0));
        CHECK(gates::motion_note(r) == "（运动 3.0，最大 4）");
    }
    SUBCASE("中途硬切的那种：最大值远高于中位，数照记，判 REGRESS（见下一条）") {
        f.motion_out =
            "lavfi.signalstats.YAVG=2.0\nlavfi.signalstats.YAVG=45.0\n"
            "lavfi.signalstats.YAVG=2.2\n";
        const auto r = gates::gate_video(shot, fake_video("运动"), f.ff(), cfg);
        CHECK(r.verdict == gates::Verdict::Regress);
        CHECK(r.metrics.at("motion_median") == doctest::Approx(2.2));
        CHECK(r.metrics.at("motion_max") == doctest::Approx(45.0));
        CHECK(gates::motion_note(r) == "（运动 16.4，最大 45）");
    }
    SUBCASE("量不到（老 ffmpeg 没 tblend）：不进 metrics，也不算失败") {
        f.motion_out = "";
        const auto r = gates::gate_video(shot, fake_video("运动"), f.ff(), cfg);
        CHECK(r.ok());
        CHECK(r.metrics.count("motion_mean") == 0);
        CHECK(gates::motion_note(r).empty());
    }
    SUBCASE("运动量那条命令不吃亮度取样的序列") {
        // 三个取样点分别回三帧；要是 tblend 那条被当成第四次取样，
        // 片尾那一帧就轮不到——这里第三帧是纯色的，必须被拦下来。
        f.frames = {kNormalFrame, kNormalFrame, kBlankFrame};
        const auto r = gates::gate_video(shot, fake_video("运动"), f.ff(), cfg);
        CHECK_FALSE(r.ok());
    }
}

TEST_CASE("片中硬切判 REGRESS：最大帧差又高又孤，问题在首帧不在种子") {
    // 三次实见的数：89/3.3、45/2.2、77/约 2；正常镜头最大不过 27。
    FakeFF f;
    models::Shot shot;
    shot.shot_id = "ep01_sh004";
    config::GateConfig cfg;

    SUBCASE("孤零零一个大跳变：REGRESS，带 cut_inside 标记") {
        f.motion_out = "lavfi.signalstats.YAVG=2.0\nlavfi.signalstats.YAVG=77.0\n"
                       "lavfi.signalstats.YAVG=2.3\nlavfi.signalstats.YAVG=1.9\n";
        const auto r = gates::gate_video(shot, fake_video("硬切"), f.ff(), cfg);
        CHECK(r.verdict == gates::Verdict::Regress);
        CHECK(r.metrics.count("cut_inside") == 1);
        CHECK(mentions(r, "片中硬切"));
        CHECK(mentions(r, "首帧"));
        // decide_next 不看重试次数：REGRESS 就是 REGRESS
        CHECK(gates::decide_next(r, shot, cfg) == gates::Verdict::Regress);
    }
    SUBCASE("整段都在动（甩镜、快切前的运动）：中位也高，不算硬切") {
        f.motion_out = "lavfi.signalstats.YAVG=20.0\nlavfi.signalstats.YAVG=45.0\n"
                       "lavfi.signalstats.YAVG=30.0\nlavfi.signalstats.YAVG=25.0\n";
        const auto r = gates::gate_video(shot, fake_video("甩镜"), f.ff(), cfg);
        CHECK(r.ok());
        CHECK(r.metrics.count("cut_inside") == 0);
    }
    SUBCASE("27 那种真在动的镜头：放行") {
        // 地板 2026-09-16 降到 25 之后，这一条靠的是"孤不孤"：
        // 27 / 2.5 只差 10.8 倍，够不着 13。
        f.motion_out = "lavfi.signalstats.YAVG=2.0\nlavfi.signalstats.YAVG=27.0\n"
                       "lavfi.signalstats.YAVG=2.5\n";
        const auto r = gates::gate_video(shot, fake_video("真动"), f.ff(), cfg);
        CHECK(r.ok());
    }
    SUBCASE("几乎不动的空镜中途切黑：34 / 0.13，也要抓住") {
        // 2026-09-16 实测 ep04_sh010：一段几乎不动的空镜，第 114 帧整个
        // 切黑。最大 34、中位 0.13——孤得不能再孤，却因为原来的地板是 40
        // 判了通过，带着那一刀进了成片。
        f.motion_out = "lavfi.signalstats.YAVG=0.1\nlavfi.signalstats.YAVG=0.13\n"
                       "lavfi.signalstats.YAVG=34.0\nlavfi.signalstats.YAVG=0.12\n"
                       "lavfi.signalstats.YAVG=0.13\n";
        const auto r = gates::gate_video(shot, fake_video("切黑"), f.ff(), cfg);
        CHECK(r.verdict == gates::Verdict::Regress);
        CHECK(r.metrics.count("cut_inside") == 1);
    }
    SUBCASE("一只手带着纸快速划过：51 / 9.7，整段都在动，放行") {
        // ep01_sh006。最大值比上面那条切黑的还高，但中位跟着高，不孤。
        f.motion_out = "lavfi.signalstats.YAVG=9.0\nlavfi.signalstats.YAVG=51.3\n"
                       "lavfi.signalstats.YAVG=9.713\nlavfi.signalstats.YAVG=10.4\n";
        const auto r = gates::gate_video(shot, fake_video("挥手"), f.ff(), cfg);
        CHECK(r.ok());
    }
}

// ---------------------------------------------------------------------------
// **闸门说的话会跟着界面语言换。**
//
// ⚠️ 这一条非有不可，因为上面那些用例**证明不了它**：它们跑在源语言下，
// 而 `say()` 在源语言下原样返回——`cpp/tests/golden/gates.json` 里那些逐字
// 钉着的中文，在包了 `SAY()` 和没包的两种代码下**一模一样**。也就是说，
// 哪天有人顺手把一处 `SAY()` 拆掉，整套闸门用例照样全绿，而德语用户那头
// 的闸门理由静悄悄变回中文。
//
// 这一条换一种语言真跑一遍，判的是"这句话真的换了"。
TEST_CASE("闸门说的话跟着语言换") {
    struct ScopedLang {
        std::string old_ = changji::i18n::spoken();
        explicit ScopedLang(const char* l) { changji::i18n::speak(l); }
        ~ScopedLang() { changji::i18n::speak(old_); }
    };

    FakeFF fake;
    const auto zh = gates::gate_video(make_shot(), paths::from_utf8("这个文件不存在.mp4"), fake.ff(),
                                      config::GateConfig{}, std::nullopt,
                                      std::nullopt);
    REQUIRE(!zh.ok());
    REQUIRE(zh.reasons.size() == 1);

    {
        ScopedLang de("de");
        const auto r = gates::gate_video(make_shot(), paths::from_utf8("这个文件不存在.mp4"),
                                         fake.ff(), config::GateConfig{},
                                         std::nullopt, std::nullopt);
        REQUIRE(!r.ok());
        REQUIRE(r.reasons.size() == 1);
        CAPTURE(r.reasons.front());
        // 理由本身换了。
        CHECK(r.reasons.front() != zh.reasons.front());
        // `describe()` 那一整句也换了——它自己也拼着中文
        //（「未过」「：」），少包一处就露在这儿。
        CAPTURE(r.describe());
        CHECK(r.describe().find("未过") == std::string::npos);
        CHECK(r.describe().find("通过") == std::string::npos);
    }

    // 换回来之后照旧说中文。**进程级的开关，不还原会祸害后面的用例。**
    const auto back = gates::gate_video(make_shot(), paths::from_utf8("这个文件不存在.mp4"),
                                        fake.ff(), config::GateConfig{},
                                        std::nullopt, std::nullopt);
    CHECK(back.reasons.front() == zh.reasons.front());
}
