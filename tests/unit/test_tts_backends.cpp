// 两个真配音后端的测试。
//
// **最要紧的一条不是怎么调，是怎么发现"调成功了但没出声"。**
//
// ComfyUI 的 TTS 节点内部捕获异常之后，会输出一个一秒的空音频然后正常返回，
// 服务端的执行状态报的是 success。只信状态码的客户端会被完全骗过，
// 拿到一堆静音文件还以为配音成功了——实测撞上过：节点缺 librosa 报错，
// 任务状态却是成功。

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "infer/llama_tts.hpp"
#include "stages/audio.hpp"
#include "stages/tts_backends.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_tts_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

TEST_CASE("疑似空音频要被拦下来") {
    // **这道检查存在的理由**：ComfyUI 的 TTS 节点内部捕获异常后，
    // 会输出一个一秒的空音频然后正常返回，状态报的是 success。
    const fs::path dir = temp_dir("空音频");
    const fs::path p = dir / paths::from_utf8("一.wav");
    stages::write_silence(p, 1.0, 24000);

    const std::string line = "那一夜的雨下得特别大，她站在天台边上很久很久。";

    try {
        stages::reject_silent_audio(p, 1.0, line);
        FAIL("该抛");
    } catch (const stages::AudioError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        // 话要说到根因上：状态是成功的，用户第一反应不会是去看日志
        CHECK(msg.find("状态是成功") != std::string::npos);
        // 不再一口咬定是 ComfyUI——本地后端也走这条检查。
        CHECK(msg.find("ComfyUI") != std::string::npos);
        // 要给出判断依据，不然用户没法确认这是不是误杀
        CHECK(msg.find("1.00 秒") != std::string::npos);
    }
}

namespace {
// 在一段静音 wav 的数据区里写一个正弦：24 kHz 单声道 16 位，
// 和 write_silence 的头一致，data 块从第 44 字节起。
void write_tone(const fs::path& path, double seconds, double amplitude) {
    stages::write_silence(path, seconds, 24000);
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(44);
    const int frames = static_cast<int>(seconds * 24000);
    for (int i = 0; i < frames; ++i) {
        const double t = i / 24000.0;
        const int v = static_cast<int>(amplitude * 32767 * std::sin(6.2831853 * 220.0 * t));
        const unsigned char lo = static_cast<unsigned char>(v & 0xff);
        const unsigned char hi = static_cast<unsigned char>((v >> 8) & 0xff);
        f.put(static_cast<char>(lo));
        f.put(static_cast<char>(hi));
    }
}
}  // namespace

TEST_CASE("短台词的真声音不该被绝对下限误杀") {
    // 5090 上整章跑通时被这条误杀过一句：「苏晚！」本地 TTS 出了 1.04 秒的
    // 真声音，差 0.01 秒够不到 1.05 的绝对下限，整个镜头的配音就丢了，
    // 装配时闸门报"有台词但没有配音时长"。
    const fs::path dir = temp_dir("短台词");

    SUBCASE("1.04 秒、有声音：放行") {
        const fs::path p = dir / paths::from_utf8("苏晚.wav");
        write_tone(p, 1.04, 0.12);   // 峰值满幅 12%，和实测的 Qwen3-TTS 一个量级
        CHECK(stages::wav_peak_ratio(p).value() > 0.1);
        CHECK_NOTHROW(stages::reject_silent_audio(p, 1.04, "苏晚！"));
    }
    SUBCASE("1.04 秒、全零：还是占位音频，照拦") {
        const fs::path p = dir / paths::from_utf8("占位.wav");
        stages::write_silence(p, 1.04, 24000);
        CHECK(stages::wav_peak_ratio(p).value() == 0.0);
        CHECK_THROWS_AS(stages::reject_silent_audio(p, 1.04, "苏晚！"),
                        stages::AudioError);
    }
    SUBCASE("有声音但只有估算的一成：相对下限照旧") {
        const fs::path p = dir / paths::from_utf8("太短.wav");
        write_tone(p, 0.3, 0.5);
        CHECK_THROWS_AS(
            stages::reject_silent_audio(
                p, 0.3, "这是一句正常长度的台词，配出来不该只有零点三秒"),
            stages::AudioError);
    }
    SUBCASE("不是 16 位 PCM 的读不出峰值，退回只看时长") {
        const fs::path p = dir / paths::from_utf8("不是wav.wav");
        std::ofstream(p, std::ios::binary) << "not a wav at all";
        CHECK_FALSE(stages::wav_peak_ratio(p).has_value());
        CHECK_THROWS_AS(stages::reject_silent_audio(p, 1.04, "苏晚！"),
                        stages::AudioError);
    }
}

TEST_CASE("两条下限都要过") {
    // 判据是**绝对下限（1.05 秒）和相对下限（估算的 35%）都满足才放过**。
    //
    // ⚠️ 副作用：一个字的台词（"嗯。"估约 0.94 秒）如果配出来不到 1.05 秒，
    // 会被当成空音频拦下来。这是**照抄 Python 的行为**，不是这里引入的。
    // 实际影响有限——TTS 引擎自己会加头尾静音，真实的"嗯。"通常在一秒以上。
    // 真撞上了要改的是那个常数，而不是悄悄放宽，因为放宽的代价是
    // 一整章静音文件被当成配音成功。
    const fs::path dir = temp_dir("下限");
    const fs::path p = dir / paths::from_utf8("嗯.wav");
    stages::write_silence(p, 1.2, 24000);

    SUBCASE("过了绝对下限的短台词放过") {
        CHECK_NOTHROW(stages::reject_silent_audio(p, 1.2, "嗯。"));
    }

    SUBCASE("没到绝对下限就拦，哪怕台词很短") {
        CHECK_THROWS_AS(stages::reject_silent_audio(p, 0.6, "嗯。"),
                        stages::AudioError);
    }

    SUBCASE("长台词配出一秒就是不对") {
        // 相对下限管的就是这种：绝对值过关，但和台词长度完全不匹配。
        CHECK_THROWS_AS(
            stages::reject_silent_audio(
                p, 1.1, "那一夜的雨下得特别大，她站在天台边上很久很久。"),
            stages::AudioError);
    }
}

TEST_CASE("wav 自己读，别的格式没有 ffmpeg 时说清缺什么") {
    const fs::path dir = temp_dir("时长");
    const fs::path w = dir / paths::from_utf8("一.wav");
    stages::write_silence(w, 2.0, 24000);
    CHECK(stages::probe_audio_duration(w, std::nullopt) ==
          doctest::Approx(2.0).epsilon(0.01));

    SUBCASE("非 wav 而且没有 ffmpeg") {
        // 只说"读不出时长"的话，用户会去查文件是不是坏了。
        const fs::path m = dir / paths::from_utf8("一.mp3");
        { std::ofstream f(m, std::ios::binary); f << "假的 mp3"; }
        try {
            stages::probe_audio_duration(m, std::nullopt);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("ffmpeg") != std::string::npos);
            CHECK(msg.find("assembly.ffmpeg_path") != std::string::npos);
        }
    }

    SUBCASE("扩展名是 wav 但内容不是，有 ffmpeg 就再试一次") {
        // 有些引擎给的其实是别的格式，只是文件名叫 wav。
        const fs::path fake = dir / paths::from_utf8("其实不是.wav");
        { std::ofstream f(fake, std::ios::binary); f << "OggS 什么的"; }

        media::FFmpeg ff("ffmpeg", "ffprobe",
                         [](const std::string&, const std::vector<std::string>&,
                            double) {
                             media::ProcResult r;
                             r.launched = true;
                             r.out = R"({"streams":[{"codec_type":"audio"}],)"
                                     R"("format":{"duration":"3.5"}})";
                             return r;
                         });
        CHECK(stages::probe_audio_duration(fake, ff) == doctest::Approx(3.5));
    }
}

TEST_CASE("HTTP 后端：把服务端的话带上") {
    const fs::path dir = temp_dir("http");
    const fs::path out = dir / paths::from_utf8("一.wav");

    SUBCASE("连不上时说清地址") {
        const auto b = stages::http_tts_backend(
            "http://127.0.0.1:9999/", 30.0,
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 0;
                r.transport_error = "connection refused";
                return r;
            },
            std::nullopt);
        try {
            b.synthesize("你好", out, std::nullopt, "neutral", 0.5);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            // 结尾的斜杠要去掉，不然拼出来是 //tts
            CHECK(msg.find("http://127.0.0.1:9999）") != std::string::npos);
        }
    }

    SUBCASE("回错误码时把 body 带上") {
        // TTS 服务出错时常常回一整页 traceback，那里面才有真正的原因。
        const auto b = stages::http_tts_backend(
            "http://x", 30.0,
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 500;
                r.body = "RuntimeError: 模型没加载";
                return r;
            },
            std::nullopt);
        try {
            b.synthesize("你好", out, std::nullopt, "neutral", 0.5);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            CHECK(std::string(e.what()).find("模型没加载") != std::string::npos);
        }
    }

    SUBCASE("正常时写盘并验产出") {
        // 造一段真的 wav 当响应体
        const fs::path src = dir / paths::from_utf8("源.wav");
        stages::write_silence(src, 3.0, 24000);
        std::ifstream in(src, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        std::string sent_url;
        std::string sent_body;
        const auto b = stages::http_tts_backend(
            "http://x/", 30.0,
            [&](const std::string& url, const std::string& body,
                const std::map<std::string, std::string>&, double) {
                sent_url = url;
                sent_body = body;
                llm::HttpResponse r;
                r.status = 200;
                r.body = bytes;
                return r;
            },
            std::nullopt);

        const auto res =
            b.synthesize("我等了你三年。", out, "林晚.wav", "sad", 0.7);
        CHECK(res.duration_s == doctest::Approx(3.0).epsilon(0.01));
        REQUIRE(res.audio_path.has_value());
        CHECK(fs::exists(out));
        CHECK(sent_url == "http://x/tts");

        const auto payload = nlohmann::json::parse(sent_body);
        CHECK(payload.at("text") == "我等了你三年。");
        CHECK(payload.at("voice_id") == "林晚.wav");
        CHECK(payload.at("emotion") == "sad");
        CHECK(payload.at("emotion_intensity") == doctest::Approx(0.7));
    }

    SUBCASE("HTTP 后端同样要验产出物") {
        // 独立服务一样会"成功但没出声"。
        const fs::path src = dir / paths::from_utf8("静音.wav");
        stages::write_silence(src, 1.0, 24000);
        std::ifstream in(src, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        const auto b = stages::http_tts_backend(
            "http://x", 30.0,
            [&](const std::string&, const std::string&,
                const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 200;
                r.body = bytes;
                return r;
            },
            std::nullopt);
        CHECK_THROWS_AS(
            b.synthesize("那一夜的雨下得特别大，她站在天台边上很久很久。", out,
                         std::nullopt, "neutral", 0.5),
            stages::AudioError);
    }

    SUBCASE("body 是一大段中文时按字符截，不按字节") {
        // 2026-09-11 实跑：同样的写法在 json_extract 里截在半个汉字上，
        // 错误消息进了任务快照之后 /api/script/series 序列化 JSON 直接 500。
        // 配音服务出错时回的 traceback 里一样会夹中文。
        std::string body = "Traceback:\n";
        for (int i = 0; i < 40; ++i) body += "模型没加载，先去设置页把配音模型下下来。";
        REQUIRE(text::utf8_len(body) > 300);
        // 按字节截 300 落在半个汉字上——这就是要防的那一下
        CHECK_THROWS(nlohmann::json(body.substr(0, 300)).dump());

        const auto b = stages::http_tts_backend(
            "http://x", 30.0,
            [&](const std::string&, const std::string&,
                const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 500;
                r.body = body;
                return r;
            },
            std::nullopt);
        try {
            b.synthesize("你好", out, std::nullopt, "neutral", 0.5);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            const std::string msg = e.what();
            CHECK(msg.find("配音服务回了 500") != std::string::npos);
            CHECK(msg.find("模型没加载") != std::string::npos);
            CHECK_NOTHROW(nlohmann::json(msg).dump());
            CHECK(text::utf8_len(msg) < 350);
        }
    }
}

TEST_CASE("HTTP 后端不给音色列表") {
    // 独立服务的音色接口没有统一约定。猜一个路径去问，问不到会被当成
    // "服务端一个音色都没有"，然后自动挑选挑出个空的。
    // 不给的话走"照填的来"，那是对的。
    const auto b = stages::http_tts_backend(
        "http://x", 30.0,
        [](const std::string&, const std::string&,
           const std::map<std::string, std::string>&, double) {
            return llm::HttpResponse{};
        },
        std::nullopt);
    CHECK_FALSE(static_cast<bool>(b.list_voices));
}

// ── 进程内配音（阶段 9）─────────────────────────────────────────────
//
// 这个测试二进制编的是没开 CHANGJI_LLAMA 的那份，所以这里能验的是
// **搭不起来时说的话对不对**。合成本身要权重，一行都验不了——
// 那件事记在 infer/llama_tts.hpp 开头。
//
// 能验的这几条不是凑数：配错模型路径是这条路上最常见的失败，
// 而"配音悄悄退回估算后端出静音"是最难发现的症状。

TEST_CASE("进程内配音：没编进来时说清楚是构建选项，不是配置") {
    std::string why;
    const auto b = stages::local_tts_backend("a.gguf", "b.gguf", false,
                                             std::nullopt, why);
    CHECK_FALSE(b.has_value());
    // 用户看到这句话要能直接知道去改构建，而不是去翻配置文件。
    CHECK(why.find("CHANGJI_LLAMA") != std::string::npos);
}

TEST_CASE("进程内配音：缺模型路径时分别点名") {
    // 只填一个是最常见的配错法。笼统说一句"模型没配"的话，
    // 填了一个的人会以为自己填对了。
    if (!infer::llama_tts_available()) {
        // 没编进来时先撞上构建那条，测不到这一条——如实跳过，
        // 不要为了让用例"通过"而放宽断言。
        return;
    }
    std::string why;
    CHECK_FALSE(stages::local_tts_backend("", "b.gguf", false, std::nullopt, why)
                    .has_value());
    CHECK(why.find("[models].tts 没填") != std::string::npos);

    why.clear();
    CHECK_FALSE(stages::local_tts_backend("a.gguf", "", false, std::nullopt, why)
                    .has_value());
    CHECK(why.find("[models].tts_decoder 没填") != std::string::npos);
}

TEST_CASE("tts.backend 认 local，另外两个取值一个字没变") {
    config::TTSConfig c;
    c.backend = "local";
    CHECK(c.validate().empty());
    // comfy 那一档随 ComfyUI 一起拆了；现在只剩 http 一个别的取值
    for (const char* ok : {"http"}) {
        c.backend = ok;
        CAPTURE(ok);
        CHECK(c.validate().empty());
    }
    c.backend = "explode";
    CHECK_FALSE(c.validate().empty());
}

TEST_CASE("HTTP 后端：两处和 Python 不一样的地方，是故意的") {
    // 这两条**不是对拍能发现的**——对拍比的是接口响应，
    // 而配音后端在流水线里面，跑起来要有个真的 TTS 服务。
    //
    // 记在这里是因为 Python 删掉之后就没有参照物了：
    // 后来的人看见 C++ 比 Python 严，会以为是自己看漏了 Python 的哪一行，
    // 或者反过来，"对齐一下"把它改回去。
    const fs::path dir = temp_dir("http偏差");

    auto backend_returning = [&](const std::string& body) {
        return stages::http_tts_backend(
            "http://x", 30.0,
            [body](const std::string&, const std::string&,
                   const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 200;
                r.body = body;
                return r;
            },
            std::nullopt);
    };

    SUBCASE("回了一段静音 wav 要被拦下来——Python 这里是放行的") {
        // Python 的 HttpTTSBackend 收尾只有 probe_wav_duration，
        // 没有 _reject_silent_audio（audio.py:161）。只有它的 Comfy 后端有。
        //
        // 而"成功但没出声"和后端是谁没关系：独立服务同样会在模型没载好时
        // 回一个合法的空 wav。放行的代价是一整章静音被当成配音成功。
        const fs::path src = dir / paths::from_utf8("静音.wav");
        stages::write_silence(src, 0.3, 24000);   // 短过 1.05 秒的绝对下限
        std::ifstream in(src, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        const auto b = backend_returning(bytes);
        CHECK_THROWS_AS(
            b.synthesize("这是一句正常长度的台词，配出来不该只有零点三秒",
                         dir / paths::from_utf8("出.wav"), std::nullopt,
                         "neutral", 0.5),
            stages::AudioError);
    }

    // **第二处偏差这里钉不住，说清楚为什么。**
    //
    // Python 的 HTTP 后端调 probe_wav_duration（只认 wav），这里调
    // probe_audio_duration（wav 读不动就退 ffprobe）。要证明这个差别，
    // 得让一个非 wav 的响应在这里**成功**——而那需要机器上真有 ffmpeg，
    // 本机没有。给 std::nullopt 的话两边都抛，只是话不一样，
    // 而那句话上面"wav 自己读，别的格式没有 ffmpeg 时说清缺什么"
    // 那条已经钉过了。
    //
    // 所以这条偏差目前只有代码注释和方案文档记着，没有用例。
    // 装上 ffmpeg 之后应该补：回一段 mp3，断言拿得到时长。
}

TEST_CASE("进程内配音的采样默认值抄的是官方 generation_config.json") {
    // Qwen/Qwen3-TTS-12Hz-1.7B-Base 随模型发布的 generation_config.json
    // （2026-09-13 核过）：temperature 0.9、top_k 50、top_p 1.0、
    // repetition_penalty 1.05。以前这里是 llama.cpp 的通用默认
    // （top_k 40 / top_p 0.9，没温度也没重复惩罚），不是这个模型的。
    // 这条用例钉的是"来源"：谁改了这几个数，得先去核官方那份。
    const infer::LlamaTtsRequest req;
    CHECK(req.temperature == doctest::Approx(0.9f));
    CHECK(req.top_k == 50);
    CHECK(req.top_p == doctest::Approx(1.0f));
    CHECK(req.repetition_penalty == doctest::Approx(1.05f));
}
