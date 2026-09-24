// 连接设置和运行参数两组接口的测试。
//
// 这三个是阶段 1 漏掉的，路由表核对时找出来的。
//
// 没走对拍语料，理由和两个长任务一样：Python 那边这几个接口改的是
// **进程内的全局状态**（settings 对象、_TIER_OVERRIDES 字典），
// TestClient 录到的只是响应体，录不到"改完之后进程里是什么样"。
// 而那恰恰是这几个接口的全部意义。所以直接对着行为写断言，
// 注释里标了每条对应 Python 的哪一段。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "http/config_api.hpp"
#include "infer/scheduler.hpp"
#include "models/hardware.hpp"
#include "pipeline/jobs.hpp"
#include "stages/limits.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 假体检。真体检会去连推理服务和大模型，连不上时每一项要等一个超时。
http::DoctorFn fake_doctor(bool can_run = true) {
    return [can_run](const config::Settings&) {
        doctor::Report r;
        r.checks.push_back(doctor::Check{
            "假的", can_run ? doctor::Level::OK : doctor::Level::FAIL, "细节", ""});
        return r;
    };
}

config::Settings baseline() {
    config::Settings s;
    s.llm.base_url = "http://127.0.0.1:11434/v1";
    s.llm.model = "qwen3:14b";
    s.llm.api_key = "sk-abcdefgh";
    s.llm.temperature = 0.7;
    s.tts.backend = "local";
    s.assembly.fps = 24;
    s.assembly.crf = 18;
    s.gates.enabled = true;
    return s;
}

/// 每条用例前把全局 runtime 摆回基线。它是单例，用例之间会串。
void reset_runtime() {
    config::runtime().replace(baseline());
    config::runtime().clear_tier_overrides();
}

}  // namespace

TEST_CASE("读连接设置时不回传密钥明文") {
    // 回明文的话它会进浏览器的网络面板、进前端的状态、进任何一次截图。
    reset_runtime();
    const auto r = http::guard([] { return http::get_connections(); });
    REQUIRE(r.status == 200);

    CHECK(r.body.at("llm_api_key_set") == true);
    CHECK(r.body.at("llm_api_key_hint") == "sk***gh");
    // 整个响应体里不能出现完整的密钥
    CHECK(r.body.dump().find("sk-abcdefgh") == std::string::npos);

    SUBCASE("短密钥整个遮掉") {
        // 掐头去尾之后剩不下几个字符，等于把密钥泄了一半
        config::Settings s = baseline();
        s.llm.api_key = "abc";
        config::runtime().replace(s);
        const auto r2 = http::guard([] { return http::get_connections(); });
        CHECK(r2.body.at("llm_api_key_hint") == "***");
        CHECK(r2.body.dump().find("abc") == std::string::npos);
    }

    SUBCASE("没设密钥") {
        config::Settings s = baseline();
        s.llm.api_key = "";
        config::runtime().replace(s);
        const auto r2 = http::guard([] { return http::get_connections(); });
        CHECK(r2.body.at("llm_api_key_set") == false);
    }

    SUBCASE("密钥两端带全角字符时 hint 按字符掐，不按字节") {
        // 粘贴时带进一个全角空格并不稀奇。按字节掐头去尾会留下半个字符，
        // 这个 hint 序列化成 JSON 时 nlohmann 抛 type_error.316，整个连接
        // 设置页回 500——和 json_extract 那条错误消息 2026-09-11 炸掉的是
        // 同一个坑。
        const std::string fw = "\xE3\x80\x80";  // U+3000 全角空格
        const std::string key = fw + "sk-abcdefgh" + fw;
        // 原来按字节那种写法留下的就是半个字符，装进 json 一 dump 就抛
        CHECK_THROWS(
            json(key.substr(0, 2) + "***" + key.substr(key.size() - 2)).dump());

        config::Settings s = baseline();
        s.llm.api_key = key;
        config::runtime().replace(s);
        const auto r2 = http::guard([] { return http::get_connections(); });
        REQUIRE(r2.status == 200);
        CHECK(r2.body.at("llm_api_key_hint") == fw + "s***h" + fw);
        CHECK_NOTHROW(r2.body.dump());
        CHECK(r2.body.dump().find("sk-abcdefgh") == std::string::npos);
    }
}

TEST_CASE("读连接设置的字段形状") {
    reset_runtime();
    const auto r = http::guard([] { return http::get_connections(); });
    REQUIRE(r.status == 200);
    for (const char* k : {
                          "llm_base_url", "llm_model",
                          "llm_api_key_set", "llm_api_key_hint",
                          "llm_temperature", "tts_backend", "tts_base_url",
                          "vram_gb_override", "config_file",
                          "env_locked"}) {
        CAPTURE(k);
        CHECK(r.body.contains(k));
    }
    // 没设时是 null 不是 0——0 是个合法的显存值，混起来分不清
    CHECK(r.body.at("vram_gb_override").is_null());
    // 空的 tts_base_url 回空串不是 null，Python 那边是 `or ""`
    CHECK(r.body.at("tts_base_url") == "");
}

TEST_CASE("改连接设置") {
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_model", "新模型"},
                            {"llm_base_url", "http://别处/v1"}}},
                 {"persist", false}},
            fake_doctor());
    });
    REQUIRE(r.status == 200);

    // changed 里只有真的变了的
    const auto changed = r.body.at("changed").get<std::vector<std::string>>();
    CHECK(changed.size() == 2);
    // 标签是中文。回执用英文字段名的话，用户得自己对着界面猜是哪一项。
    const auto labels = r.body.at("labels").get<std::vector<std::string>>();
    CHECK(labels.size() == 2);
    for (const auto& l : labels) CHECK(l.find('_') == std::string::npos);

    // 立刻生效
    const config::Settings s = config::runtime().snapshot();
    CHECK(s.llm.model == "新模型");
    CHECK(s.llm.base_url == "http://别处/v1");

    // 没勾写回就不写文件
    CHECK(r.body.at("saved_to").is_null());
    // 体检结果带回来了
    CHECK(r.body.at("can_run") == true);
    CHECK(r.body.at("checks").is_array());
}

TEST_CASE("没变的字段不算 changed") {
    // 算进去的话界面会显示"已应用 模型名"，而用户什么都没改——
    // 他会以为自己误触了什么。
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_model", "qwen3:14b"}}}, {"persist", false}},
            fake_doctor());
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("changed").empty());
    CHECK(r.body.at("saved_to").is_null());
}

TEST_CASE("一项都没给要报错") {
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(json{{"patch", json::object()}},
                                      fake_doctor());
    });
    CHECK(r.status == 400);
    CHECK(r.body.at("detail") == "没有要改的项");

    SUBCASE("全填 null 也算没给") {
        // 对应 pydantic 的 exclude_none=True
        const auto r2 = http::guard([] {
            return http::post_connections(
                json{{"patch", {{"llm_model", nullptr}}}}, fake_doctor());
        });
        CHECK(r2.status == 400);
    }
}

TEST_CASE("配音后端的两条约束") {
    reset_runtime();
    const auto try_tts = [](const json& patch) {
        return http::guard([&] {
            return http::post_connections(
                json{{"patch", patch}, {"persist", false}}, fake_doctor());
        });
    };

    CHECK(try_tts(json{{"tts_backend", "别的"}}).status == 400);

    // 选 http 就必须填地址。不填的话跑到配音那一步才报错，
    // 而那时候前面几十分钟的渲染已经跑完了。
    const auto r = try_tts(json{{"tts_backend", "http"}});
    CHECK(r.status == 400);
    CHECK(r.body.at("detail") == "配音后端选 http 就必须填地址");

    CHECK(try_tts(json{{"tts_backend", "http"},
                       {"tts_base_url", "http://tts:9000"}}).status == 200);
}

TEST_CASE("改成外接配音要把显存里那份放掉") {
    // **这条是需求那句话的落点。** 显存不够时，体检和 out_of_vram_message
    // 都在劝用户"把配音改成外接 HTTP 服务，本机就不用装配音模型、也不占
    // 显存"。他照做了，结果只改了配置、权重还占着——照着提示做了、显存
    // 没少，只会以为这一项没生效。
    reset_runtime();
    auto& sched = infer::scheduler();
    sched.evict_all();

    int unloads = 0;
    infer::SlotSpec tts;
    tts.slot = infer::Slot::TTS;
    tts.vram_estimate = 1;
    tts.load = [] {};
    tts.unload = [&unloads] { ++unloads; };
    sched.register_slot(tts);
    { auto lease = sched.acquire(infer::Slot::TTS); }
    REQUIRE(sched.loaded(infer::Slot::TTS));

    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"tts_backend", "http"},
                            {"tts_base_url", "http://别处:9000"}}},
                 {"persist", false}},
            fake_doctor());
    });
    REQUIRE(r.status == 200);
    CHECK(unloads == 1);
    CHECK_FALSE(sched.loaded(infer::Slot::TTS));

    sched.evict_all();   // 别把状态留给后面的用例
}

TEST_CASE("tts_base_url 填空串当作没配") {
    // 留一个空串的话，backend 选 http 时会拿它去连，
    // 报的错是"连不上 "——后面什么都没有。
    reset_runtime();
    {
        config::Settings s = baseline();
        s.tts.base_url = "http://old:9000";
        config::runtime().replace(s);
    }
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"tts_base_url", ""}}}, {"persist", false}},
            fake_doctor());
    });
    REQUIRE(r.status == 200);
    CHECK_FALSE(config::runtime().snapshot().tts.base_url.has_value());
}

TEST_CASE("写回配置文件") {
    reset_runtime();
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_连接写回");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // user_config_path() 指向真实的用户目录，测试里不该动它。
    // 所以这里只验"写回开关关掉时不写"，写回本身由 test_writeback 覆盖。
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_model", "不写回"}}}, {"persist", false}},
            fake_doctor());
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("saved_to").is_null());
    CHECK(config::runtime().snapshot().llm.model == "不写回");

    fs::remove_all(dir, ec);
}

TEST_CASE("正在跑的时候不让换机器") {
    // 前半章是一台机器出的，后半章是另一台，画风对不上。
    reset_runtime();
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();

    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress& p) {
                               while (!release.load() && !p.cancelled()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(1));
                               }
                           });

    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_model", "x"}}}}, fake_doctor());
    });
    CHECK(r.status == 409);
    CHECK(r.body.at("detail") == "正在跑，这时候换机器会把这一章跑坏");

    SUBCASE("改参数也不让") {
        const auto r2 = http::guard([] {
            return http::post_settings(json{{"fps", 30}});
        });
        CHECK(r2.status == 409);
        CHECK(r2.body.at("detail") == "正在跑，改参数会让这一章前后不一致");
    }

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("改运行参数：两种请求写法都收") {
    // 老写法是把字段直接摊在请求体里，新写法包在 patch 里。
    // 只收一种的话，刷新慢一步的页面点一下就报 422。
    reset_runtime();
    {
        const auto r = http::guard([] {
            return http::post_settings(json{{"fps", 30}});
        });
        REQUIRE(r.status == 200);
        CHECK(config::runtime().snapshot().assembly.fps == 30);
    }
    reset_runtime();
    {
        const auto r = http::guard([] {
            return http::post_settings(
                json{{"patch", {{"fps", 25}}}, {"persist", false}});
        });
        REQUIRE(r.status == 200);
        CHECK(config::runtime().snapshot().assembly.fps == 25);
    }
}

TEST_CASE("改运行参数：三组字段都能改") {
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_settings(json{
            {"fps", 30}, {"crf", 20}, {"subtitle_font", "思源黑体"},
            {"max_attempts_per_shot", 5}, {"gates_enabled", false},
            {"target_lufs", -15.0},
        });
    });
    REQUIRE(r.status == 200);
    const config::Settings s = config::runtime().snapshot();
    CHECK(s.assembly.fps == 30);
    CHECK(s.assembly.crf == 20);
    CHECK(s.assembly.subtitle_font == "思源黑体");
    CHECK(s.gates.max_attempts_per_shot == 5);
    CHECK(s.gates.enabled == false);
    // tts_tolerance_s 2026-09-13 删了：它有校验有持久化，但引擎里没有
    // 任何一个阶段读过它。换成一个真的会生效的字段。
    CHECK(s.gates.target_lufs == doctest::Approx(-15.0));
    CHECK(r.body.at("changed").size() == 6);
}

TEST_CASE("分辨率必须是 32 的倍数") {
    // 否则 Wan 的潜空间对不齐，出来的图是错位的。
    reset_runtime();
    const auto try_wh = [](const json& body) {
        return http::guard([&] { return http::post_settings(body); }).status;
    };
    CHECK(try_wh(json{{"draft_width", 500}}) == 400);
    CHECK(try_wh(json{{"draft_height", 500}}) == 400);
    CHECK(try_wh(json{{"draft_width", 512}}) == 200);
}

TEST_CASE("画质档位真的生效，这是有意和 Python 不一样") {
    // Python 那边有个 _TIER_OVERRIDES 字典，**只写不读**——
    // 用户改了草稿分辨率，接口回「已应用 草稿宽度」，
    // 而那个值存进一个没有任何地方读的字典里，实际什么都没发生。
    //
    // 这里让它真的生效。不生效的话，这个开关是在骗用户。
    reset_runtime();
    const models::HardwareProfile before = config::runtime().profile();
    const auto bit = before.tiers.find(models::Tier::DRAFT);
    REQUIRE(bit != before.tiers.end());

    const auto r = http::guard([] {
        return http::post_settings(
            json{{"draft_width", 512}, {"draft_height", 768},
                 {"draft_steps", 8}});
    });
    REQUIRE(r.status == 200);

    const models::HardwareProfile after = config::runtime().profile();
    const auto ait = after.tiers.find(models::Tier::DRAFT);
    REQUIRE(ait != after.tiers.end());
    CHECK(ait->second.width == 512);
    CHECK(ait->second.height == 768);
    CHECK(ait->second.steps == 8);

    // 成片档没被顺手改掉
    const auto fit = after.tiers.find(models::Tier::FINAL);
    const auto fbefore = before.tiers.find(models::Tier::FINAL);
    REQUIRE(fit != after.tiers.end());
    REQUIRE(fbefore != before.tiers.end());
    CHECK(fit->second.width == fbefore->second.width);

    SUBCASE("清掉进程内覆盖之后退回探测值") {
        // **前提是 [tiers] 里没填。** 填了的话清掉进程内那份只会退到
        // 文件里那份，不会退到探测值——两层覆盖，文件在下、进程内在上。
        config::runtime().clear_tier_overrides();
        auto s = config::runtime().snapshot();
        s.tiers = config::TiersConfig{};
        config::runtime().replace(s);
        const auto back = config::runtime().profile();
        CHECK(back.tiers.at(models::Tier::DRAFT).width == bit->second.width);
    }
}

TEST_CASE("档位要写回配置文件——设完重启不能丢") {
    // **这条用例 2026-09-10 反过来了。** 原来断言的是"档位不写回"，
    // 理由是"档位按显存推，写死等于把这台机器的显存刻进配置"。
    // 那个理由站不住：这个文件里 [models] 全是这台机器的模型路径。
    //
    // 真实后果是用户把成片档调成 1280×704 跑了一章，重启回到 960×544，
    // 而界面上没有任何提示。现在写进 [tiers]，没填的项还是 0（按显存推）。
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_settings(
            json{{"patch", {{"draft_width", 512}}}, {"persist", true}});
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("changed") == json::array({"draft_width"}));
    CHECK_FALSE(r.body.at("saved_to").is_null());
}

TEST_CASE("多余字段要拒") {
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_settings(json{{"patch", {{"typo", 1}}}});
    });
    CHECK(r.status == 422);
    CHECK(r.body.at("detail")[0].at("type") == "extra_forbidden");

    const auto r2 = http::guard([] {
        return http::post_connections(json{{"patch", {{"typo", 1}}}},
                                      fake_doctor());
    });
    CHECK(r2.status == 422);
}

TEST_CASE("类型不对要拒，而且不能改坏一半") {
    // 半套改动比不改更糟：用户看到"已应用"但配置是残的。
    reset_runtime();
    const int fps_before = config::runtime().snapshot().assembly.fps;
    const auto r = http::guard([] {
        return http::post_settings(json{{"fps", 30}, {"crf", "不是数字"}});
    });
    CHECK(r.status == 422);
    // fps 没被改掉
    CHECK(config::runtime().snapshot().assembly.fps == fps_before);
}

TEST_CASE("校验不过要整体回滚") {
    reset_runtime();
    const int fps_before = config::runtime().snapshot().assembly.fps;
    // crf 的合法范围是 0..51
    const auto r = http::guard([] {
        return http::post_settings(json{{"fps", 30}, {"crf", 999}});
    });
    CHECK(r.status == 400);
    CHECK(config::runtime().snapshot().assembly.fps == fps_before);
    CHECK(config::runtime().snapshot().assembly.crf == 18);
}

TEST_CASE("设置接口认的配音后端，和配置文件认的是同一批") {
    // **这条用例以前钉的是一个反直觉的不对称**：接口只放 comfy 和 http 过，
    // 配置文件另外认 local。那个不对称是为了和 Python 的
    // `if new_tts.backend not in ("comfy", "http")` 一字不差。
    //
    // ComfyUI 2026-09-10 拆掉之后不对称没有了：comfy 不再是合法取值，
    // local 成了默认。两边认的都是 local 和 http。
    reset_runtime();
    for (const char* b : {"local", "http"}) {
        CAPTURE(b);
        config::TTSConfig c;
        c.backend = b;
        if (std::string(b) == "http") c.base_url = "http://x";
        CHECK(c.validate().empty());
    }
    // 认不出的值两边都要拒
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"tts_backend", "comfy"}}}, {"persist", false}},
            fake_doctor());
    });
    CHECK_MESSAGE(r.status == 400, r.body.dump());
}

// ---------------------------------------------------------------------------
// 命令行后端（claude / codex）
// ---------------------------------------------------------------------------

TEST_CASE("改成跑本机命令行：收得下，也落到配置对象上") {
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_backend", "command"},
                            {"llm_command", "claude"},
                            {"llm_command_args",
                             json::array({"-p", "--output-format", "text"})},
                            {"llm_command_timeout_s", 1200}}},
                 {"persist", false}},
            fake_doctor());
    });
    REQUIRE(r.status == 200);
    const auto& s = config::runtime().snapshot();
    CHECK(s.llm.backend == "command");
    CHECK(s.llm.command == "claude");
    REQUIRE(s.llm.command_args.size() == 3);
    CHECK(s.llm.command_args[0] == "-p");
    CHECK(s.llm.command_timeout_s == doctest::Approx(1200));
}

TEST_CASE("**白名单放行的每一个键，persist 那段都得写得回去**") {
    // 2026-09-18 踩的就是这个：命令行那四个键进了白名单、改也落到内存上了
    //（/api/connections 读出来是对的），而 persist 那个循环没有对应分支，
    // `value` 是个 null——写进 config.toml 变成
    //     backend = ""
    //     command = ""
    // 于是**这次改好了，重启就没了**，而且 backend = "" 连 validate 都过不去。
    //
    // 最坏的是它静默：界面上照样弹「换好了」。现在那段遇到 null 会当场 500，
    // 这一条盯着那道闸——两份表（kAllowed 和 persist）对不上就红在这里，
    // 而不是等到用户重启之后。
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_backend", "command"},
                            {"llm_command", "claude"},
                            {"llm_command_args", json::array({"-p"})},
                            {"llm_command_timeout_s", 900}}},
                 {"persist", true}},
            fake_doctor());
    });
    // 500 = 有键漏在 persist 那段外面（消息里写着是哪一个）
    CHECK_MESSAGE(r.status == 200,
                  "persist 那段漏了键：" << r.body.dump().substr(0, 300));
}

TEST_CASE("进程内那条的思考开关：收得下、落到配置上、persist 写得回去") {
    // 2026-09-19 本地那条路接回来时加的键。白名单、内存、persist 三处
    // 要一起有——上面那条用例盯的就是三处对不上的那种漏法。
    reset_runtime();
    REQUIRE(config::runtime().snapshot().llm.thinking);   // 默认开
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_backend", "local"}, {"llm_thinking", false}}},
                 {"persist", true}},
            fake_doctor());
    });
    CHECK_MESSAGE(r.status == 200, r.body.dump().substr(0, 300));
    const auto& s = config::runtime().snapshot();
    CHECK(s.llm.backend == "local");
    CHECK_FALSE(s.llm.thinking);
    // 读回来也带着它，模型窗那个勾靠它回填
    const auto g = http::guard([] { return http::get_connections(); });
    REQUIRE(g.status == 200);
    CHECK(g.body.at("llm_thinking") == false);
    CHECK(g.body.contains("llm_local_model"));
}

TEST_CASE("选了命令行却没说跑哪个，要挡下来") {
    // 引擎那边 validate 管这条（LLMConfig::validate）。放过去的话，
    // 每一次生成才失败，而这儿本可以一句话说清。
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_backend", "command"}}}, {"persist", false}},
            fake_doctor());
    });
    CHECK(r.status == 400);
}

TEST_CASE("command_args 要是字符串数组，别的形状要报 422") {
    reset_runtime();
    const auto r = http::guard([] {
        return http::post_connections(
            json{{"patch", {{"llm_backend", "command"},
                            {"llm_command", "claude"},
                            {"llm_command_args", "-p --output-format text"}}},
                 {"persist", false}},
            fake_doctor());
    });
    CHECK(r.status == 422);
}


TEST_CASE("正在出片时存设置：出片那一份单镜上限不许被机器那份换掉") {
    // 出片进门按这部片子算过一遍上限（`[video].max_shot_s`），后面拆超长镜、算
    // 帧数、「前 n 分钟」重挑都读它。改机器表、下模型这几条存设置的路不挡出片，
    // 原来一存就按机器那份把它换掉，半截出片之后的镜头按另一套上限走。
    reset_runtime();
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();

    config::Settings film = baseline();
    film.video.max_shot_s = 3.0;   // 默认那个模型一镜最长 5 秒，3 秒才夹得下来
    const int film_frames = config::video_limits_for(film).max_frames;
    const int machine_frames = config::video_limits_for(baseline()).max_frames;
    REQUIRE(film_frames < machine_frames);   // 两份得真不一样，下面才测得出东西

    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release, &film](pipeline::JobProgress& p) {
                               config::apply_video_limits(film);   // 出片进门那一下
                               while (!release.load() && !p.cancelled()) {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(1));
                               }
                           });
    while (stages::video_limits().max_frames != film_frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    config::runtime().replace(baseline());               // 半截里存了一下设置
    CHECK(stages::video_limits().max_frames == film_frames);

    release = true;
    pipeline::jobs().wait_idle();
    config::runtime().replace(baseline());               // 出片完了再存：照常换
    CHECK(stages::video_limits().max_frames == machine_frames);
}
