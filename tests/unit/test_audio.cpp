// 配音编排的测试。
//
// TTS 后端是注入的，所以整条编排能在毫秒里跑完。这里盯的是三件事：
//
//   **重切循环**——估算的语速和引擎实测差得很远，估 4.8 秒出来 5.8 秒是
//   真实发生过的，那 1 秒会盖到下一镜上去；
//   **音色挑选的优先级**——人工指定的不该被自动挑选覆盖，
//   但服务端不认的老 id 又必须退回自动挑选，否则整条流水线断在这里；
//   **锁定时长**——有台词的镜头向上吸附，没台词的不动。

#include <doctest/doctest.h>
#include <mutex>
#include <thread>
#include <atomic>

#include <nlohmann/json.hpp>
#include <memory>

#include <filesystem>
#include <fstream>
#include <string>

#include "stages/audio.hpp"
#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

models::ProjectPaths make_paths(const std::string& tag) {
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_配音_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    models::ProjectPaths p(root);
    p.ensure();
    return p;
}

models::DialogueLine line(const std::string& text,
                          std::optional<std::string> char_id = "c_lin") {
    models::DialogueLine l;
    l.text = text;
    l.char_id = std::move(char_id);
    return l;
}

models::Shot make_shot(const std::string& id,
                       std::vector<models::DialogueLine> lines,
                       double duration = 5.0) {
    models::Shot s;
    s.shot_id = id;
    s.duration_s = duration;
    s.dialogue = std::move(lines);
    return s;
}

models::AssetLibrary make_assets(const std::string& voice_id = "",
                                 const std::string& gender = "female") {
    models::AssetLibrary a;
    models::Character c;
    c.char_id = "c_lin";
    c.name = "林晚";
    c.voice_gender = gender;
    c.voice_order = 0;
    if (!voice_id.empty()) c.voice_id = voice_id;
    a.characters["c_lin"] = c;
    return a;
}

/// 一个可以按文本长度返回任意时长的假 TTS。
struct FakeTTS {
    /// 每个字多少秒。默认比估算的 4.6 字/秒慢一点。
    double seconds_per_char = 1.0 / 4.6;
    /// 这些音色是"服务端有的"。空表示问不到列表。
    std::vector<std::string> voices;
    bool has_lister = false;

    mutable std::vector<std::string> synthesized;
    mutable std::vector<std::optional<std::string>> used_voices;

    stages::TTSBackend backend() const {
        stages::TTSBackend b;
        b.name = "fake";
        b.synthesize = [this](const std::string& text, const fs::path& out,
                              const std::optional<std::string>& voice,
                              const std::string&, double) {
            synthesized.push_back(text);
            used_voices.push_back(voice);
            stages::SynthesisResult r;
            r.duration_s =
                static_cast<double>(text::utf8_len(text)) * seconds_per_char;
            stages::write_silence(out, r.duration_s, 24000);
            r.audio_path = out;
            return r;
        };
        if (has_lister) {
            b.list_voices = [this] { return voices; };
        }
        return b;
    }
};

}  // namespace

TEST_CASE("静音 wav 写出来能读回时长") {
    const auto paths = make_paths("静音");
    const fs::path p = paths.audio() / paths::from_utf8("测试.wav");

    stages::write_silence(p, 2.5, 24000);
    CHECK(fs::exists(p));
    CHECK(stages::probe_wav_duration(p) == doctest::Approx(2.5).epsilon(0.01));

    SUBCASE("零秒也写出至少一帧") {
        // 零帧的 wav 有些播放器和 ffmpeg 的 concat 会当成损坏文件。
        const fs::path z = paths.audio() / paths::from_utf8("零.wav");
        stages::write_silence(z, 0.0, 24000);
        CHECK(fs::file_size(z) > 44);
        CHECK(stages::probe_wav_duration(z) > 0.0);
    }

    SUBCASE("不是 wav 时说清楚") {
        const fs::path bad = paths.audio() / paths::from_utf8("假的.wav");
        { std::ofstream f(bad, std::ios::binary); f << "这不是 wav"; }
        CHECK_THROWS_AS(stages::probe_wav_duration(bad), stages::AudioError);
    }
}

TEST_CASE("wav 的块要逐个走，不能假定固定偏移") {
    // 很多 TTS 引擎会插一个 LIST 块写元数据。按固定偏移读的话拿到的是
    // 垃圾数，而垃圾数会变成一个荒唐的时长，然后镜头按它锁定。
    const auto paths = make_paths("带元数据");
    const fs::path p = paths.audio() / paths::from_utf8("有LIST.wav");

    // 手工拼一个 fmt 之后插了 LIST 的 wav
    const std::uint32_t rate = 24000;
    const std::uint32_t data_bytes = rate * 2;   // 1 秒，16 位单声道
    // 长度取偶数：RIFF 规定奇数长度的块后面要补一个填充字节，
    // 而这里手写的话很容易漏。第一版就漏了，而且字面量里的
    // 十六进制转义还把串在 NUL 处截断了——两个错凑在一起，
    // 报出来的现象是"时长不对"，看不出是测试自己拼错了 wav。
    const std::string meta = "INFOISFTchangji test";   // 20 字节
    std::ofstream f(p, std::ios::binary);
    const auto u32 = [&f](std::uint32_t v) {
        const char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF),
                           char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
        f.write(b, 4);
    };
    const auto u16 = [&f](std::uint16_t v) {
        const char b[2] = {char(v & 0xFF), char((v >> 8) & 0xFF)};
        f.write(b, 2);
    };
    f.write("RIFF", 4);
    u32(static_cast<std::uint32_t>(36 + meta.size() + 8 + data_bytes));
    f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate);
    u32(rate * 2); u16(2); u16(16);
    f.write("LIST", 4); u32(static_cast<std::uint32_t>(meta.size()));
    f.write(meta.data(), static_cast<std::streamsize>(meta.size()));
    f.write("data", 4); u32(data_bytes);
    f.write(std::string(data_bytes, '\0').data(), data_bytes);
    f.close();

    CHECK(stages::probe_wav_duration(p) == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("合成出来装不下时按实测语速重切") {
    // **这条是这个文件里最要紧的一条。**
    // 估的是每秒 4.6 个字，不同引擎不同音色差得很远。
    // 实测有过估 4.8 秒、出来 5.8 秒的，那 1 秒就盖到下一镜上了。
    const auto paths = make_paths("重切");
    FakeTTS tts;
    // 引擎比估算慢一倍：估算过关的句子，合成出来一定超
    tts.seconds_per_char = 2.0 / 4.6;

    stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
    auto shot = make_shot("sh001", {line("这是一句刚好卡在估算上限附近的台词内容")});
    pipeline::CancelToken tok;

    const auto plan = stage.process_shot(shot, make_assets(), tok);

    // 重切之后每一段都装得进一个镜头
    const double limit = stages::max_line_seconds();
    for (const auto& l : shot.dialogue) {
        CAPTURE(l.text);
        CHECK(l.actual_duration_s.value_or(0.0) <= limit + 0.01);
    }
    CHECK(shot.dialogue.size() > 1);   // 真的切开了
    CHECK(plan.lines == static_cast<int>(shot.dialogue.size()));

    SUBCASE("重切不丢字") {
        std::string joined;
        for (const auto& l : shot.dialogue) joined += l.text;
        CHECK(joined == "这是一句刚好卡在估算上限附近的台词内容");
    }
}

TEST_CASE("重切有次数上限，不会把话剁碎") {
    // 切三次还装不下多半是引擎那边出了别的问题，再切下去只是把话剁碎。
    const auto paths = make_paths("剁碎");
    FakeTTS tts;
    tts.seconds_per_char = 100.0;   // 无论怎么切都超

    stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
    auto shot = make_shot("sh001", {line("这是一句会被反复重切的台词")});
    pipeline::CancelToken tok;
    stage.process_shot(shot, make_assets(), tok);

    // 有上限就一定会停下来，而且不会切成一个字一句
    CHECK(shot.dialogue.size() <= 16);
    for (const auto& l : shot.dialogue) {
        CHECK_FALSE(l.text.empty());
    }
}

TEST_CASE("音色：人工指定的优先，服务端不认的退回自动挑") {
    const auto paths = make_paths("音色");
    FakeTTS tts;
    tts.has_lister = true;
    tts.voices = {"zh_female_01.wav", "zh_male_01.wav", "en_female_02.wav"};

    SUBCASE("角色上填了而且服务端有，就用它") {
        stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
        auto shot = make_shot("sh001", {line("你好")});
        pipeline::CancelToken tok;
        stage.process_shot(shot, make_assets("zh_male_01.wav"), tok);
        CHECK(shot.dialogue[0].voice_id == "zh_male_01.wav");
        CHECK(stage.unknown_voices().empty());
    }

    SUBCASE("填的服务端不认，退回自动挑并记下来") {
        // 老项目里存的可能是 v_角色名 这种早年自造的 id。原样提交上去
        // 节点会拒绝，整条流水线断在配音这一步。
        stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
        auto shot = make_shot("sh001", {line("你好")});
        pipeline::CancelToken tok;
        stage.process_shot(shot, make_assets("v_林晚"), tok);

        REQUIRE(shot.dialogue[0].voice_id.has_value());
        CHECK(*shot.dialogue[0].voice_id != "v_林晚");
        // 换了声音要说一声，不然用户只会觉得"这个角色听着不像上次那个"
        CHECK(stage.unknown_voices().count("v_林晚") == 1);
    }

    SUBCASE("没填就按性别自动挑") {
        stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
        auto shot = make_shot("sh001", {line("你好")});
        pipeline::CancelToken tok;
        stage.process_shot(shot, make_assets("", "male"), tok);
        REQUIRE(shot.dialogue[0].voice_id.has_value());
        CHECK(shot.dialogue[0].voice_id->find("male") != std::string::npos);
    }

    SUBCASE("问不到列表时照填的来，不自作主张") {
        // 自动挑的话会从一个空列表里挑，结果是没有音色——
        // 而用户明明填了。
        FakeTTS blind;   // has_lister = false
        stages::AudioStage stage(blind.backend(), config::TTSConfig{}, paths);
        auto shot = make_shot("sh001", {line("你好")});
        pipeline::CancelToken tok;
        stage.process_shot(shot, make_assets("我自己的音色"), tok);
        CHECK(shot.dialogue[0].voice_id == "我自己的音色");
    }
}

TEST_CASE("锁定时长：有台词向上吸附，没台词不动") {
    const auto paths = make_paths("锁时长");
    FakeTTS tts;
    stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
    pipeline::CancelToken tok;

    SUBCASE("有台词的锁到可生成的档位上") {
        auto shot = make_shot("sh001", {line("我等了你三年。")}, 5.0);
        const auto plan = stage.process_shot(shot, make_assets(), tok);
        CHECK(shot.duration_locked);
        // 宁长勿短：锁定时长要装得下配音加尾巴
        CHECK(shot.duration_s >= plan.speech_duration_s + stages::kTailS - 0.001);
        CHECK(plan.slack_s >= 0.0);
        // 而且是一个能生成的档位，不是任意小数
        CHECK(shot.duration_s == doctest::Approx(
            stages::ceil_duration(plan.speech_duration_s + stages::kTailS)));
    }

    SUBCASE("没台词的保持分镜给的时长") {
        // 它们是节奏调节的余量，动了就等于把分镜的节奏设计抹掉。
        auto shot = make_shot("sh002", {}, 3.5);
        const auto plan = stage.process_shot(shot, make_assets(), tok);
        CHECK(shot.duration_s == doctest::Approx(3.5));
        CHECK_FALSE(shot.duration_locked);
        CHECK(plan.lines == 0);
    }
}

TEST_CASE("切开的台词不带着原来那份音频") {
    // 不清的话新的一段会指向整句的音频，混音时那一句会被念两遍。
    const auto paths = make_paths("清音频");
    FakeTTS tts;
    stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
    pipeline::CancelToken tok;

    auto l = line("这是一句非常非常长的台词长到必须切成好几段才装得进一个镜头里面去呢");
    l.audio_path = "audio/旧的.wav";
    l.actual_duration_s = 99.0;
    auto shot = make_shot("sh001", {l});

    stage.process_shot(shot, make_assets(), tok);
    REQUIRE(shot.dialogue.size() > 1);
    for (const auto& out : shot.dialogue) {
        CAPTURE(out.text);
        REQUIRE(out.audio_path.has_value());
        CHECK(out.audio_path->find("旧的") == std::string::npos);
        CHECK(out.actual_duration_s.value_or(99.0) < 99.0);
    }
}

TEST_CASE("一镜配音失败不拖垮后面几镜") {
    const auto paths = make_paths("失败");
    stages::TTSBackend b;
    b.name = "会炸的";
    b.synthesize = [](const std::string& text, const fs::path& out,
                      const std::optional<std::string>&, const std::string&,
                      double) -> stages::SynthesisResult {
        if (text.find("炸") != std::string::npos) {
            throw stages::AudioError("假装引擎挂了");
        }
        stages::SynthesisResult r;
        r.duration_s = 1.0;
        stages::write_silence(out, 1.0, 24000);
        r.audio_path = out;
        return r;
    };

    stages::AudioStage stage(b, config::TTSConfig{}, paths);
    auto s1 = make_shot("sh001", {line("正常")});
    auto s2 = make_shot("sh002", {line("会炸的一句")});
    auto s3 = make_shot("sh003", {line("也正常")});
    std::vector<models::Shot*> shots = {&s1, &s2, &s3};

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::ShotAudioPlan> plans;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        plans = stage.run(shots, make_assets(), p, tok);
    });
    table.wait_idle();

    CHECK(plans.size() == 2);   // 炸的那一镜没有计划
    CHECK(s1.status == models::ShotStatus::AUDIO_DONE);
    CHECK(s3.status == models::ShotStatus::AUDIO_DONE);
    // 失败那一镜状态不推进，后面的闸门会看出"有台词但没有配音时长"
    CHECK(s2.status == models::ShotStatus::PLANNED);
}

TEST_CASE("换了音色要在事件里说一声") {
    // 不说的话用户只会觉得"这个角色听着不像上次那个"。
    const auto paths = make_paths("提示");
    FakeTTS tts;
    tts.has_lister = true;
    tts.voices = {"zh_female_01.wav"};

    stages::AudioStage stage(tts.backend(), config::TTSConfig{}, paths);
    auto s = make_shot("sh001", {line("你好")});
    std::vector<models::Shot*> shots = {&s};

    pipeline::JobTable table;
    std::vector<nlohmann::json> msgs;
    table.set_sink([&msgs](const std::string&, const nlohmann::json& m) {
        msgs.push_back(m);
    });
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stage.run(shots, make_assets("v_不存在的音色"), p, tok);
    });
    table.wait_idle();

    bool said = false;
    for (const auto& m : msgs) {
        const std::string t = m.dump();
        if (t.find("已自动换成可用的") != std::string::npos) said = true;
    }
    CHECK(said);
}

// ---------------------------------------------------------------------------
// 配音阶段跑完之后镜头变成什么样，和 Python 逐条比。
//
// `audio_plan.json` 覆盖的是**算**那部分（时长估算、拆镜头）。
// 这一条补的是**编排**：一句一句配、把量到的时长写回台词、
// 反推锁定镜头时长、推状态。
//
// **时长回写最要紧**：`actual_duration_s` 是后面所有环节的输入——
// 字幕的时间戳、装配的 adelay 偏移、闸门的音画同步判定全从它来。
// 写错了不报错，成片里表现为字幕和人声对不上，而那时候已经隔了三个阶段。
//
// ⚠️ 语料是用 `concurrency=1` 导的。Python 的 run 默认并发 2，
// C++ 是顺序跑的（头文件里写了理由）。不统一的话比的就不是同一件事。
// ---------------------------------------------------------------------------

TEST_CASE("配音阶段的时长回写和状态变化和 Python 一样") {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/audio_stage.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    nlohmann::json g;
    in >> g;

    const auto cases = g.at("cases");
    REQUIRE(cases.size() == 6);

    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const auto& c = cases[ci];
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        const auto durations = c.at("synth_durations").get<std::vector<double>>();
        const auto shots_want = c.at("shots_after");

        // 按剧本回时长，和 Python 那边的 ScriptedTTS 一样：
        // 按调用顺序取，用光了回 1.0
        auto idx = std::make_shared<std::size_t>(0);
        stages::TTSBackend b;
        b.name = "scripted";
        b.synthesize = [durations, idx](const std::string&, const fs::path& out,
                                        const std::optional<std::string>&,
                                        const std::string&, double) {
            const double d = *idx < durations.size() ? durations[*idx] : 1.0;
            ++*idx;
            stages::SynthesisResult r;
            r.duration_s = d;
            stages::write_silence(out, d, 24000);
            r.audio_path = out;
            return r;
        };

        const auto paths = make_paths("语料" + std::to_string(ci));
        stages::AudioStage stage(b, config::TTSConfig{}, paths);

        // **输入用 shots_before**。shots_after 里的台词是拆过之后的，
        // 拿它当输入就不是同一个场景了——拆句本身正是被测的行为。
        std::vector<models::Shot> owned;
        for (const auto& sb : c.at("shots_before")) {
            std::vector<models::DialogueLine> lines;
            for (const auto& t : sb.at("dialogue")) {
                lines.push_back(line(t.get<std::string>()));
            }
            auto sh = make_shot(sb.at("shot_id").get<std::string>(), lines);
            sh.duration_s = sb.at("duration_s").get<double>();
            owned.push_back(sh);
        }

        std::vector<models::Shot*> shots;
        for (auto& s : owned) shots.push_back(&s);

        pipeline::JobTable table;
        pipeline::CancelToken tok;
        std::vector<stages::ShotAudioPlan> plans;
        table.start(pipeline::JobKind::Run, "ep01",
                    [&](pipeline::JobProgress& p) {
                        plans = stage.run(shots, make_assets(), p, tok);
                    });
        table.wait_idle();

        const auto want_plans = c.at("plans");
        REQUIRE(plans.size() == want_plans.size());
        for (std::size_t i = 0; i < plans.size(); ++i) {
            CAPTURE(i);
            CHECK(plans[i].shot_id == want_plans[i].at("shot_id").get<std::string>());
            CHECK(plans[i].speech_duration_s ==
                  doctest::Approx(want_plans[i].at("speech_duration_s").get<double>()));
            CHECK(plans[i].locked_duration_s ==
                  doctest::Approx(want_plans[i].at("locked_duration_s").get<double>()));
            CHECK(plans[i].slack_s ==
                  doctest::Approx(want_plans[i].at("slack_s").get<double>()));
            CHECK(plans[i].is_tight() == want_plans[i].at("is_tight").get<bool>());
        }

        // 跑完之后镜头本身变成什么样——**这才是下游真正读的东西**
        REQUIRE(owned.size() == shots_want.size());
        for (std::size_t i = 0; i < owned.size(); ++i) {
            CAPTURE(i);
            const auto& got = owned[i];
            const auto& want = shots_want[i];
            CHECK(std::string(models::to_string(got.status)) ==
                  want.at("status").get<std::string>());
            CHECK(got.duration_s ==
                  doctest::Approx(want.at("duration_s").get<double>()));
            CHECK(got.duration_locked == want.at("duration_locked").get<bool>());

            // 拆句：条数对不上，后面全错位
            const auto want_lines = want.at("dialogue");
            REQUIRE(got.dialogue.size() == want_lines.size());
            for (std::size_t j = 0; j < got.dialogue.size(); ++j) {
                CAPTURE(j);
                const auto& gl = got.dialogue[j];
                const auto& wl = want_lines[j];
                CHECK(gl.text == wl.at("text").get<std::string>());
                CHECK(gl.audio_path.has_value() == wl.at("has_audio").get<bool>());
                // **回写的时长**：字幕时间戳、装配 adelay、闸门同步判定全从它来
                if (wl.at("actual_duration_s").is_null()) {
                    CHECK_FALSE(gl.actual_duration_s.has_value());
                } else {
                    REQUIRE(gl.actual_duration_s.has_value());
                    CHECK(*gl.actual_duration_s == doctest::Approx(
                              wl.at("actual_duration_s").get<double>()));
                }
            }
        }
    }
}

TEST_CASE("配音并发：几路一起跑，结果和串行一模一样") {
    // 用户 2026-09-17：「配音是不是没有走多 gpu」。配音早就走池
    // （run_deps.cpp 的 tts_pool），可这一层写死串行，一台双卡机上第二个
    // 位置从头闲到尾。
    //
    // 并发的风险在**写回**：process_shot 就地改台词（重切）和时长。
    // 所以并行那一段只动副本，写回等收完在调用线程上按原顺序做。
    // 这一条钉的就是"并发跑出来的和串行跑出来的一样"。
    const auto paths = make_paths("并发");

    std::atomic<int> live{0};
    std::atomic<int> peak{0};
    stages::TTSBackend b;
    b.synthesize = [&](const std::string&, const fs::path& out,
                       const std::optional<std::string>&, const std::string&,
                       double) -> stages::SynthesisResult {
        const int now = ++live;
        int want = peak.load();
        while (now > want && !peak.compare_exchange_weak(want, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        --live;
        stages::SynthesisResult r;
        r.duration_s = 1.0;
        stages::write_silence(out, 1.0, 24000);
        r.audio_path = out;
        return r;
    };

    const auto go = [&](int lanes, std::vector<models::Shot>& owned) {
        std::vector<models::Shot*> shots;
        for (auto& s : owned) shots.push_back(&s);
        stages::AudioStage stage(b, config::TTSConfig{}, paths);
        pipeline::JobTable table;
        pipeline::CancelToken tok;
        std::vector<stages::ShotAudioPlan> plans;
        table.start(pipeline::JobKind::Run, "ep01",
                    [&](pipeline::JobProgress& p) {
                        plans = stage.run(shots, make_assets(), p, tok, lanes);
                    });
        table.wait_idle();
        return plans;
    };

    std::vector<models::Shot> a = {make_shot("sh001", {line("第一句")}),
                                   make_shot("sh002", {line("第二句")}),
                                   make_shot("sh003", {line("第三句")}),
                                   make_shot("sh004", {line("第四句")})};
    std::vector<models::Shot> b2 = a;   // 同一批，串行再跑一遍

    const auto par = go(3, a);
    CHECK(peak.load() > 1);             // 真的几路一起在跑
    const auto seq = go(1, b2);

    REQUIRE(par.size() == seq.size());
    REQUIRE(par.size() == 4);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        // 写回按原顺序，一镜不错位
        CHECK(a[i].shot_id == b2[i].shot_id);
        CHECK(a[i].status == b2[i].status);
        CHECK(a[i].duration_s == doctest::Approx(b2[i].duration_s));
        CHECK(par[i].shot_id == seq[i].shot_id);
    }
}

TEST_CASE("配音并发：开跑就取消，镜头一个字节都不许改") {
    // 同 render / frames 那两层的教训（2026-09-17 丢过一整章数据）：
    // 没跑过的一格装的是默认值，照着写回去就是把镜头抹平。
    const auto paths = make_paths("并发取消");
    stages::TTSBackend b;
    b.synthesize = [](const std::string&, const fs::path& out,
                      const std::optional<std::string>&, const std::string&,
                      double) -> stages::SynthesisResult {
        stages::SynthesisResult r;
        r.duration_s = 1.0;
        stages::write_silence(out, 1.0, 24000);
        r.audio_path = out;
        return r;
    };
    auto s1 = make_shot("sh001", {line("一")});
    auto s2 = make_shot("sh002", {line("二")});
    const auto before1 = s1;
    const auto before2 = s2;
    std::vector<models::Shot*> shots = {&s1, &s2};

    stages::AudioStage stage(b, config::TTSConfig{}, paths);
    pipeline::JobTable table;
    pipeline::CancelToken tok;
    tok.request();
    std::vector<stages::ShotAudioPlan> plans;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        plans = stage.run(shots, make_assets(), p, tok, 2);
    });
    table.wait_idle();

    CHECK(plans.empty());
    CHECK(s1.shot_id == before1.shot_id);
    CHECK_FALSE(s1.shot_id.empty());
    CHECK(s1.status == before1.status);
    CHECK(s2.shot_id == before2.shot_id);
    CHECK(s2.status == before2.status);
}
