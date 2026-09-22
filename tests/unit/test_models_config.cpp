// [models] 配置的测试。
//
// 这一节 Python 侧没有对应，所以不是对拍——是纯粹的新逻辑，
// 唯一要测的是路径怎么解析。
//
// 路径这块值得测是因为它有三条分支（绝对、相对、~ 开头）而且错了不会立刻
// 报错：解析出一个不存在的路径，程序照常启动，直到真去加载模型时才炸，
// 那时候错误信息只会说"文件打不开"，看不出是解析错了还是文件没下。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "scoped_env.hpp"
#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "models/hardware.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 一条这个平台上真正算绝对的路径，带中文。
///
/// **不能写死 `D:/模型`。** 那在 Windows 上是绝对路径，在 Linux 上是
/// 相对路径——`resolve` 会把它接到工作区后面，断言全错。而这几条用例
/// 钉的是"绝对路径原样用、相对路径接 dir 后面"，和盘符没关系。
/// 中文目录名保留：那是这几条的另一半意图，中文一路走到底不能乱码。
std::string abs_utf8(const char* tail) {
#ifdef _WIN32
    return std::string("D:/") + tail;
#else
    return std::string("/mnt/") + tail;
#endif
}

}  // namespace

TEST_CASE("模型路径解析") {
    config::ModelsConfig m;
    const fs::path ws = paths::from_utf8(abs_utf8("工作区"));

    SUBCASE("空的返回空") {
        // 没配就是没配，不能返回一个"目录/"这样的半截路径——
        // 那种路径 is_regular_file 会返回 false，看起来像"文件不存在"，
        // 掩盖了"根本没配"这件事。
        CHECK(m.resolve("", ws).empty());
    }

    SUBCASE("相对路径接在 dir 后面") {
        m.dir = abs_utf8("模型");
        CHECK(m.resolve("a.gguf", ws) == paths::from_utf8(abs_utf8("模型/a.gguf")));
        // 带子目录的相对路径
        CHECK(m.resolve("wan/b.gguf", ws) ==
              paths::from_utf8(abs_utf8("模型/wan/b.gguf")));
    }

    SUBCASE("绝对路径原样用，不拼 dir") {
        // 多台机器共享网络盘时会这么填
        m.dir = abs_utf8("模型");
        const fs::path abs = paths::from_utf8(abs_utf8("共享/c.gguf"));
        CHECK(m.resolve(paths::to_utf8(abs), ws) == abs);
    }

    SUBCASE("dir 留空时回落到项目库下的 models") {
        CHECK(m.dir_path(ws) == ws / "models");
        CHECK(m.resolve("a.gguf", ws) == ws / "models" / "a.gguf");
    }

    SUBCASE("dir 是空串等同于没填") {
        m.dir = "";
        CHECK(m.dir_path(ws) == ws / "models");
    }

    SUBCASE("~ 会展开") {
        m.dir = "~/模型";
        const fs::path got = m.dir_path(ws);
        CHECK(got.is_absolute());
        // 展开之后不该还留着波浪号
        CHECK(paths::to_utf8(got).find('~') == std::string::npos);

        // 条目本身以 ~ 开头也要展开，而且不能被当成相对路径拼到 dir 后面
        const fs::path entry = m.resolve("~/单独放的.gguf", ws);
        CHECK(entry.is_absolute());
        CHECK(paths::to_utf8(entry).find("模型") == std::string::npos);
    }
}

TEST_CASE("模型配置不做存在性校验") {
    // 存在性检查在 doctor 里，不在这里。
    //
    // 理由：模型动辄好几个 G，装好程序还没下模型是常态。
    // 如果配置加载阶段就因为文件不在而失败，用户连界面都进不去，
    // 也就没法在界面里看到到底缺哪个文件——只能盯着一行启动错误猜。
    config::ModelsConfig m;
    m.llm = "根本不存在的文件.gguf";
    m.dir = "Z:/这个盘也不存在";
    CHECK(m.validate().empty());

    config::Settings s;
    s.models = m;
    CHECK(s.validate().empty());
}

TEST_CASE("模型配置能从 toml 读出来") {
    const fs::path tmp = fs::temp_directory_path() /
                         paths::from_utf8("changji_模型配置");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const fs::path cfg = tmp / "changji.toml";
    {
        std::ofstream out(cfg, std::ios::binary);
        REQUIRE(out.good());
        out << "[models]\n"
            << "dir = \"" << abs_utf8("模型库") << "\"\n"
            << "llm = \"Qwen3-14B-Q4_K_M.gguf\"\n"
            << "video = \"Wan2.2-TI2V-5B-Q4_K_M.gguf\"\n"
            << "video_vae = \"Wan2.2_VAE.safetensors\"\n"
            << "video_text_encoder = \"umt5-xxl-encoder-Q5_K_M.gguf\"\n"
            << "image = \"Qwen-Image-Edit-Q4_K_M.gguf\"\n";
    }

    const config::Settings s = config::load_settings(tmp);
    REQUIRE(s.models.dir.has_value());
    CHECK(*s.models.dir == abs_utf8("模型库"));
    CHECK(s.models.llm == "Qwen3-14B-Q4_K_M.gguf");
    CHECK(s.models.video == "Wan2.2-TI2V-5B-Q4_K_M.gguf");
    CHECK(s.models.video_vae == "Wan2.2_VAE.safetensors");
    CHECK(s.models.video_text_encoder == "umt5-xxl-encoder-Q5_K_M.gguf");
    CHECK(s.models.image == "Qwen-Image-Edit-Q4_K_M.gguf");

    // 中文目录名要能一路走到底而不乱码。MSVC 上 fs::path 和 std::string
    // 之间用错转换函数的话，这里会变成问号或者直接抛异常。
    CHECK(s.models.resolve(s.models.llm, tmp) ==
          paths::from_utf8(abs_utf8("模型库/Qwen3-14B-Q4_K_M.gguf")));

    SUBCASE("没写的项保持空，不会被填上猜的默认值") {
        // **把用户级配置隔离掉。** load_settings 先读 user_config_path()
        // 再读项目里的 changji.toml，两份是叠加的。这台机器上要是真配过
        // 模型（服务器上就是），那几项永远不会是空的——这条用例于是在
        // 干净机器上过、在真在用的机器上挂。2026-09-10 在服务器上撞到：
        // 整套用例常年红一个，红着红着就没人看了，真回归也照样漏过去。
        //
        // 具体怎么隔离（三个平台各换哪个变量、为什么目录名得是纯 ASCII）
        // 挪进 scoped_env.hpp 了——2026-09-13 写回那边也栽在同一件事上，
        // 两处各写一份迟早分家，而分了家最难发现。
        const changji::test::ScopedUserConfigDir iso("models_config");
        const fs::path cfg2 = tmp / "changji.toml";
        {
            std::ofstream out(cfg2, std::ios::binary);
            out << "[models]\nllm = \"only-this.gguf\"\n";
        }
        const config::Settings s2 = config::load_settings(tmp);
        CHECK(s2.models.llm == "only-this.gguf");
        // 猜默认文件名只会让人以为配好了，然后在加载模型时才炸
        CHECK(s2.models.video.empty());
        CHECK(s2.models.image.empty());
        CHECK_FALSE(s2.models.dir.has_value());
    }

    fs::remove_all(tmp, ec);
}

// ── 环境变量覆盖 ─────────────────────────────────────────────────────
//
// **这一组防的是"两处不同步"。** 一个环境变量要在两个地方各写一遍：
// `env_mapping()` 那张表（决定 /api/connections 的 env_locked 里报不报它，
// 界面靠这个把输入框置灰）和 `apply_env()`（决定它到底生不生效）。
//
// 只加了表：界面说"被环境变量锁住了"，而值其实没被覆盖。
// 只加了 apply_env：值覆盖了，界面还让人编辑，改完悄悄丢掉。
// **两种都不会报错，都只能靠人发现。**
//
// CHANGJI_MODELS_ENGINE 就是补出来的——别的 [models] 键都有，
// 偏偏这个"走进程内还是走 ComfyUI"的开关漏了，
// 而它正是容器里和对拍时最需要临时翻的一个。

// ScopedEnv 挪去 scoped_env.hpp 了：which 那边也要用，各写一份迟早分家。

TEST_CASE("环境变量：改了值就要生效，而且要在 env_locked 里报出来") {
    struct Case {
        const char* var;      ///< 不带 CHANGJI_ 前缀
        const char* key;      ///< env_locked 里的键名
        const char* value;
    };
    // 挑的是这次新加的三个。老的那些同理，出问题的方式一模一样。
    const Case cases[] = {
        {"MODELS_ENGINE", "models_engine", "sd"},
        {"MODELS_TTS", "models_tts", "some-talker.gguf"},
        {"MODELS_TTS_DECODER", "models_tts_decoder", "some-decoder.gguf"},
    };

    for (const auto& c : cases) {
        CAPTURE(c.var);
        const test::ScopedEnv guard(std::string("CHANGJI_") + c.var, c.value);

        // 一、表里有它，界面才知道该把输入框置灰。
        const auto locked = config::env_overridden();
        CHECK_MESSAGE(locked.count(c.key) == 1,
                      "env_mapping() 里少了这一项，界面不会显示它被锁住");

        // 二、值真的被覆盖了。
        const config::Settings s = config::load_settings();
        const std::string got =
            std::string(c.key) == "models_engine"      ? s.models.engine
            : std::string(c.key) == "models_tts"       ? s.models.tts
                                                       : s.models.tts_decoder;
        CHECK_MESSAGE(got == c.value,
                      "apply_env() 里少了这一项，值没被覆盖");
    }
}

TEST_CASE("环境变量把 engine 写错了要被拦住，不能悄悄接受") {
    // 悄悄接受的话整条出片的路会走岔，而表现是"连不上 ComfyUI"或者
    // "没编进出图后端"——两句话都指不到真正的原因（环境变量拼错了）。
    const test::ScopedEnv guard("CHANGJI_MODELS_ENGINE", "sdcpp");
    const config::Settings s = config::load_settings();
    CHECK(s.models.engine == "sdcpp");
    const auto errs = s.models.validate();
    REQUIRE_FALSE(errs.empty());
    // ComfyUI 拆掉之后 engine 只剩 sd，而**老配置填 comfy 要给迁移说明**，
    // 不能只说"只能是 sd"——用户不知道自己那套工作流该怎么办。
    CHECK(errs[0].find("sd") != std::string::npos);
}

TEST_CASE("flash attention 默认开，配置里能关") {
    // sd.cpp 的 sd_ctx_params_init 把它设成 false，而方案第二节选 sd.cpp 的
    // 理由里就列着 --diffusion-fa。6 GB 卡上这一项直接影响塞不塞得下，
    // 不该靠用户自己想起来加。
    CHECK(config::ModelsConfig{}.diffusion_flash_attn);

    const fs::path tmp = fs::temp_directory_path() / "changji_fa_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\ndiffusion_flash_attn = false\n";
    }
    CHECK_FALSE(config::load_settings(tmp).models.diffusion_flash_attn);
    fs::remove_all(tmp, ec);
}

TEST_CASE("默认配置模板里 [models] 必须是注释掉的") {
    // **不是风格问题，是 Python 后端起不起得来的问题。**
    //
    // Python 引擎的 Settings 是 extra="forbid"，只要用户配置里出现 [models]，
    // 它整份加载失败（"Extra inputs are not permitted"），后端根本起不来。
    // 而迁移期间两个后端共用这一份文件。
    //
    // 这条用例是踩过之后加的：我拿 --init-config 当"看一眼配置在哪"的探针，
    // 一敲就把模板写进了用户配置，然后对拍里 152 条全变成
    // "Python 侧：连不上"——而那看起来像是网络或端口的问题。
    const fs::path tmp = fs::temp_directory_path() / "changji_tpl_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const fs::path f = tmp / "config.toml";
    config::write_default_config(f);

    std::ifstream in(f, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // 行首的 [models] 就是生效的节头；"# [models]" 是注释，不算。
    CHECK(text.find("\n[models]") == std::string::npos);
    CHECK(text.find("\nengine = \"sd\"") == std::string::npos);
    // 但内容要还在，只是注释掉——不然用户不知道有这一节可以填。
    CHECK(text.find("# [models]") != std::string::npos);
    fs::remove_all(tmp, ec);
}


// ---------------------------------------------------------------------------
// 配置模板必须提到代码真的认的每一个值。
//
// **这一条是被一个真 bug 逼出来的。** 模板里 `[tts] backend` 的注释写着
// "填 comfy……填 http……"，只列了两个值，而代码认三个——漏掉的正是
// `local`：阶段 9 做的进程内配音，`--say` 验过真能出声。
// `[models]` 那一节也没有 `tts` / `tts_decoder` 这两个键，
// 而它们是 local 那条路必须填的。
//
// 后果不是"少个功能"，是**把人推去装一个根本不需要的东西**：
// 用户跑 `--init-config`（这是他做的第一件事），照着模板只能选 comfy 或
// http，于是去装 34 GB 的 ComfyUI 或者另起一个配音服务，
// 而这台机器上的这个二进制自己就能出声。
//
// 单元测试看不见这种错——模板是注释，改错了每个值仍然解析得动、
// 每条断言仍然是绿的。所以这里改成**拿代码认的值去查模板**：
// 以后再加一个后端而不写进模板，这条就会红。
// ---------------------------------------------------------------------------

namespace {

/// 从模板里切出一节：从 `[名字]` 那行到下一个顶格的 `[` 为止。
///
/// **整份模板做子串搜索是不行的**，第一版就栽在这上面：查 `"tts"`
/// 撞上 `[tts]` 这个节名本身，查 `image` 撞上 `workflows/image.json`，
/// 两条用例从头到尾没验过任何东西——把模板改回漏掉 local 的样子，
/// 它们照样全绿。
std::string toml_section(const std::string& tpl, const std::string& name) {
    // [models] 那一节在模板里**整个是注释掉的**（默认走 Python 引擎，
    // 取消注释才切到进程内推理），所以两种开头都要认。
    const std::string head = "[" + name + "]";
    std::string opener = head;
    std::size_t i = tpl.find("\n" + opener);
    if (i == std::string::npos) {
        opener = "# " + head;
        i = tpl.find("\n" + opener);
        if (i == std::string::npos) return {};
    }
    i += 1;
    std::size_t j = i + opener.size();
    while (true) {
        const std::size_t nl = tpl.find('\n', j);
        if (nl == std::string::npos) return tpl.substr(i);
        // 下一节的开头：顶格的 '[' 或者顶格的 "# ["
        const bool bare = nl + 1 < tpl.size() && tpl[nl + 1] == '[';
        const bool commented = tpl.compare(nl + 1, 3, "# [") == 0;
        if (bare || commented) return tpl.substr(i, nl - i);
        j = nl + 1;
    }
}

}  // namespace

TEST_CASE("模板的 [tts] 那一节提到了代码认的每一个后端") {
    // 代码里真的按这个值分派的三处：stages/tts_backends.cpp 的
    // pick_tts_backend（出片那条路）、http/run_deps.cpp（装依赖）、
    // doctor/doctor.cpp（体检，认不出就 WARN）。
    // （第三个 comfy 2026-09-10 拆了，settings.cpp 的校验现在直接拒它）
    const std::string sec =
        toml_section(config::default_config_template(), "tts");
    REQUIRE_FALSE(sec.empty());

    for (const char* backend : {"local", "http"}) {
        CAPTURE(backend);
        CHECK_MESSAGE(sec.find(backend) != std::string::npos,
                      "[tts] 那一节里没提 " << backend
                      << " —— 用户照着模板配就不会知道有这条路");
    }
}

TEST_CASE("模板的 [models] 那一节列出了每一个会被读的键") {
    // settings.cpp 里 take(t, "...", ...) 那一串。
    // **按 `键 = ` 的形状找**，不是找到这个词就算——散文里提一嘴不等于
    // 给了用户一行可以取消注释就用的东西。
    const std::string sec =
        toml_section(config::default_config_template(), "models");
    REQUIRE_FALSE(sec.empty());

    for (const char* key : {"dir", "engine", "llm", "video", "video_vae",
                            "video_text_encoder", "image", "image_vae",
                            "image_text_encoder", "image_text_encoder_vision",
                            "tts", "tts_decoder", "weights",
                            "video_cfg", "video_flow_shift",
                            "image_cfg", "image_flow_shift", "frame_tier",
                            "frame_steps",
                            "vram_reserve_gb", "video_high_noise",
                            "video_moe_boundary", "video_llm",
                            "video_llm_vision", "video_audio_vae",
                            "video_rng", "video_lora",
                            "video_lora_strength", "video_vae_tile",
                            "vae_vram_min_gb", "video_lora_tiers",
                            "image_weights"}) {
        CAPTURE(key);
        // 模板里这一节整个是注释掉的，所以形状是 `# 键 = `
        const std::string want = std::string("# ") + key + " = ";
        CHECK_MESSAGE(sec.find(want) != std::string::npos,
                      "[models] 里没有 `" << want << "` 这一行");
    }
}

TEST_CASE("模板自己解析得动，而且解析出来就是默认值") {
    // 模板是用户拿到的第一个文件。它要是解析不动，或者解析出来和默认值
    // 不一样，那"生成一份模板"这件事本身就是在骗人。
    const fs::path dir = fs::temp_directory_path() /
                         paths::from_utf8("changji_模板往返");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "changji.toml", std::ios::binary);
        f << config::default_config_template();
    }
    const auto s2 = config::load_settings(dir);
    const config::Settings def;
    CHECK(s2.tts.backend == def.tts.backend);
    CHECK(s2.llm.model == def.llm.model);
    CHECK(s2.models.engine == def.models.engine);
}

TEST_CASE("[models].weights：默认 smart，认 auto，别的拒") {
    // **默认 smart（2026-09-10 起）**：按模型多大、卡多大自己算，换卡不用改。
    // 原来默认 cpu，是"6 GB 卡上能跑"的保守选择，但代价是大卡上一直走慢路
    // 且没提示——用户："都应该让程序自己算。"smart 在小卡上自己会退回 cpu。
    CHECK(config::ModelsConfig{}.weights == "smart");

    const fs::path tmp = fs::temp_directory_path() / "changji_weights_cfg";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\nweights = \"auto\"\n";
    }
    CHECK(config::load_settings(tmp).models.weights == "auto");

    // 第三种取值：sd.cpp 的组件规格，原样传下去。
    // **给"差一点就装得下"的卡用的**：5090 上 fp8 图像模型用 auto 会连
    // 编码器一起塞进显存，挤不下 VAE 解码那 6.6 GB；只把编码器和 VAE
    // 放内存就够了，而编码器只在采样前跑一次，放内存几乎不影响速度。
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\nweights = \"te=cpu,vae=cpu\"\n";
    }
    CHECK(config::load_settings(tmp).models.weights == "te=cpu,vae=cpu");

    config::Settings s;
    s.models.weights = "";      // 空的才拒——别的都可能是合法规格
    const auto errs = s.validate();
    bool said = false;
    for (const auto& e : errs) {
        if (e.find("weights") != std::string::npos) said = true;
    }
    CHECK(said);
    fs::remove_all(tmp, ec);
}

TEST_CASE("video_lora_tiers：Turbo 只挂草稿档") {
    // Turbo 那类蒸馏 LoRA 拿画质换速度（实测采样 164 秒 → 41 秒），
    // 所以适合只挂草稿档：草稿看叙事和构图，成片跑满步数要最好的画面。
    // 上下文是两档共用的，所以这个决定落在**每次请求**上。
    CHECK(config::ModelsConfig{}.video_lora_tiers == "both");

    for (const char* v : {"draft", "final", "both"}) {
        CAPTURE(v);
        config::Settings ok;
        ok.models.video_lora_tiers = v;
        for (const auto& e : ok.validate()) {
            CHECK_MESSAGE(e.find("video_lora_tiers") == std::string::npos, e);
        }
    }

    config::Settings bad;
    bad.models.video_lora_tiers = "草稿";   // 只认那三个英文值
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("video_lora_tiers") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("weights = smart：按视频模型多大和卡多大算，不用人填") {
    // 用户的原话："都应该让程序自己算。"以前 smart 只看卡，5090 上要人手填
    // cpu；换了大卡还得记得改回来，不改就一直走慢路，而且没有任何提示。
    //
    // 账是 5090 上量的：扩散权重常驻还要给 1280×704 的计算缓冲 19.7 GB
    // 和余量 4 GB；VAE 也常驻再加 5.5 GB；卡按九成算。
    config::ModelsConfig m;
    m.weights = "smart";
    const double h3 = 18.8;   // MiniMax-H3 Q4_K_M

    // **实测锚点**：5090（32.6 GB）上 H3 18.8 GB 常驻差 788 MB 装不下 → cpu，
    // 和用户之前手填的一样，只是现在不用填了
    CHECK(m.weights_for(32.6, h3) == "cpu");   // 18.8 + 14.6 = 33.4 > 32.6
    // 小模型（Wan 5B 约 10 GB）在同一张卡上就装得下：10 + 14.6 = 24.6 ≤ 32.6
    CHECK(m.weights_for(32.6, 10.0) == "te=cpu,vae=cpu");
    // 大卡全装得下 → 只有文本编码器留内存（18.8 + 14.6 + 5.5 = 38.9 ≤ 80，且 ≥ 40）
    CHECK(m.weights_for(80.0, h3) == "te=cpu");
    // 拿不到模型大小：按装不下处理——猜错是整章出片失败，放内存只是慢
    CHECK(m.weights_for(80.0, 0.0) == "cpu");

    // **文本编码器永远放内存**：它每镜只跑一次（H3 实测 8 到 9 秒），
    // 却是最大的一块（18.9 GB）。80 GB 的卡上也不该占着它。
    CHECK(m.weights_for(80.0, h3).find("te=cpu") != std::string::npos);

    // VAE 那道门槛仍然可配：抬高它，80 GB 也不让 VAE 进显存
    m.vae_vram_min_gb = 100.0;
    CHECK(m.weights_for(80.0, h3) == "te=cpu,vae=cpu");

    // 别的取值原样传下去，不碰
    for (const char* w : {"cpu", "auto", "te=cpu,vae=cpu"}) {
        config::ModelsConfig other;
        other.weights = w;
        CAPTURE(w);
        CHECK(other.weights_for(8.0, h3) == w);
        CHECK(other.weights_for(80.0, h3) == w);
    }

    SUBCASE("画布大了计算缓冲跟着大，不能还按 1280×704 判") {
        // 上面那个 14.6 GB 是 1280×704 量的，而这里以前根本收不到画布——
        // **2K 被当成 1280×704**：大卡上判成"装得下、VAE 也常驻"，
        // 跑到一半 OOM。
        config::ModelsConfig mm;
        mm.weights = "smart";
        const double p2k = 1440.0 * 2560.0;        // 368.6 万像素
        const double p720 = 544.0 * 928.0;         // 50.5 万，当前标准档
        const double anchor = 1280.0 * 704.0;      // 90.1 万，实测锚点

        // 2K 是锚点的 4.09 倍，缓冲 14.6 → 59.7 GB
        // 80 GB：18.8 + 59.7 = 78.5 装得下，但再加 VAE 5.5 就超了
        CHECK(mm.weights_for(80.0, h3, false, p2k) == "te=cpu,vae=cpu");
        // 不传画布还是老答案——正是这个差别说明以前判错了
        CHECK(mm.weights_for(80.0, h3) == "te=cpu");

        // 60 GB 上 2K 连扩散权重都常驻不下：18.8 + 59.7 = 78.5 > 60
        CHECK(mm.weights_for(60.0, h3, false, p2k) == "cpu");
        CHECK(mm.weights_for(60.0, h3) != "cpu");   // 以前会说装得下

        // **只往上放大，不往下缩小。** 缓冲里有一部分不随画布变（CUDA
        // 上下文、驱动余量），而我们只有一个锚点，分不出固定和可变。
        // 往下缩会在小画布上低估，低估的后果是 OOM。
        CHECK(mm.weights_for(32.6, h3, false, p720) ==
              mm.weights_for(32.6, h3, false, anchor));
        CHECK(mm.video_live_vram_gb("cpu", h3, p720) ==
              doctest::Approx(mm.video_live_vram_gb("cpu", h3, anchor)));

        // 两个函数必须用同一个画布，否则会自相矛盾
        CHECK(mm.video_live_vram_gb("cpu", h3, p2k) >
              mm.video_live_vram_gb("cpu", h3, anchor));
        CHECK(mm.video_live_vram_gb("te=cpu", h3, p2k) > 80.0);
    }

    // 负门槛要拒
    config::Settings bad;
    bad.models.vae_vram_min_gb = -1.0;
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("vae_vram_min_gb") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("统一内存：装得下就一个组件都不往内存放") {
    // **苹果芯片上"权重放内存"是笔不成立的交易。**
    //
    // 独显上它换的是显存：权重待在系统内存里，用到才走一趟 PCIe。
    // 统一内存上两头都不成立——CPU 和 GPU 指的是同一片物理内存，挪过去
    // 不会让 GPU 多出一个字节；也没有那趟搬运可省。剩下的只有"把计算
    // 赶去 CPU 跑"（UMT5-XXL 在 CPU 上 8 到 9 秒，在 GPU 上一两秒）。
    //
    // 所以 smart 在这种机器上顺序反过来：默认全常驻，超过 Metal 那条线
    // 才开始退让。数字用这台 M3 Max：128 GB 统一内存，Metal 肯给 107.5 GB。
    config::ModelsConfig m;
    m.weights = "smart";
    const double h3 = 18.8;   // MiniMax-H3 Q4_K_M

    // 一样的卡，两套算法给出两个答案——这正是这次要改的东西
    CHECK(m.weights_for(107.5, h3, /*unified=*/true) == "gpu");
    CHECK(m.weights_for(107.5, h3, /*unified=*/false) == "te=cpu");

    // 退让的阶梯还在：ggml 的 CPU 缓冲不算进 Metal 那条线，超了系统开始
    // 压缩换页，那比把编码器放 CPU 慢得多。
    //   18.8 + 14.6 + 5.5 = 38.9 —— 38 装不下，40 装得下
    CHECK(m.weights_for(38.0, h3, true) == "te=cpu");
    CHECK(m.weights_for(40.0, h3, true) == "gpu");
    // 连权重带缓冲都常驻不下时和独显一样，全放内存（18.8 + 14.6 = 33.4）
    CHECK(m.weights_for(32.6, h3, true) == "cpu");
    // 拿不到模型大小：照旧按装不下处理
    CHECK(m.weights_for(107.5, 0.0, true) == "cpu");

    // 图像那一路同理。Qwen-Image fp8 20 GB + 解码缓冲 6.6 + 余量 4 = 30.6
    config::ModelsConfig im;
    im.image_weights = "smart";
    CHECK(im.image_weights_for(107.5, 20.0, true) == "gpu");
    // 独显装得下：只把编码器放内存，**VAE 留在显存**。VAE 才 0.24 GB，
    // 放内存的代价是每镜采样完的分块解码全在 CPU 上跑：L20 上实测一镜
    // 42 秒里 GPU 只忙 9 秒，「解码 N/78」那一段卡满 20 多秒
    // （2026-09-15）；留显存 2.7 秒。
    CHECK(im.image_weights_for(107.5, 20.0, false) == "te=cpu");
    // 装不下：独显退到"全放内存"，统一内存退到"编码器和 VAE 放内存"——
    // 那一档在统一内存上仍然有用（少占 Metal 的额度），而全放内存不是。
    CHECK(im.image_weights_for(32.6, 20.0, true) == "te=cpu,vae=cpu");
    CHECK(im.image_weights_for(32.6, 20.0, false) == "cpu");

    // 人写死了值就别动它，unified 与否都一样
    for (const char* w : {"cpu", "auto", "te=cpu,vae=cpu", "gpu"}) {
        config::ModelsConfig other;
        other.weights = w;
        CAPTURE(w);
        CHECK(other.weights_for(107.5, h3, true) == w);
        CHECK(other.weights_for(8.0, h3, true) == w);
    }

    // **"gpu" 要按全常驻估显存。** 调度器拿这个数判要不要卸模型，
    // 漏了这一支就会把"什么都在显存里"估成"只有缓冲"，然后不卸——
    // 而估低的下场是 OOM。
    CHECK(m.video_live_vram_gb("gpu", h3) == doctest::Approx(18.8 + 14.6 + 5.5));
    CHECK(m.image_live_vram_gb("gpu", 20.0) == doctest::Approx(20.0 + 6.6 + 4.0));
}

TEST_CASE("[models]：双专家视频模型的两项") {
    // Wan 2.2 的 A14B 是混合专家：高噪声专家跑前几步定构图和运动，
    // 低噪声专家跑后几步出细节。换它的理由是 TI2V-5B 的动作质量不够
    // （用户看了草稿档的原话：图生视频还是太差了）。
    const fs::path tmp = fs::temp_directory_path() / "changji_moe_cfg";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\nvideo = \"low.gguf\"\n"
             "video_high_noise = \"high.gguf\"\n"
             "video_moe_boundary = 0.9\n";
    }
    const auto s = config::load_settings(tmp);
    CHECK(s.models.video_high_noise == "high.gguf");
    CHECK(s.models.video_moe_boundary == doctest::Approx(0.9));

    // 默认：空 + sd.cpp 的 0.875
    CHECK(config::ModelsConfig{}.video_high_noise.empty());
    CHECK(config::ModelsConfig{}.video_moe_boundary == doctest::Approx(0.875));

    SUBCASE("只填高噪声那份要拒") {
        // 症状会是"出的片和以前一样"——高噪声那份被静默忽略，看不出来，
        // 所以这里必须拦住。
        config::Settings bad;
        bad.models.video_high_noise = "high.gguf";
        bad.models.video.clear();
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_high_noise") != std::string::npos) said = true;
        }
        CHECK(said);
    }
    SUBCASE("出片的 LoRA：默认就指着 Turbo 那份") {
        // 用途是 Turbo 那类蒸馏适配器：H3 的 Turbo LoRA 把 28 步压到 6 步。
        // **挂上之后步数要跟着改**，不改的话白挂，28 步跑 Turbo 只会更糊。
        // **默认非空**（2026-09-10 改的）：用户要"都使用 turbo 加速"，
        // 那就不该是个要手填的旋钮。文件不在时按"没配"处理并在日志里
        // 说一声——没下过 LoRA 的机器照样出片，只是慢。
        CHECK(config::ModelsConfig{}.video_lora.find("turbo") !=
              std::string::npos);
        CHECK(config::ModelsConfig{}.video_lora_strength ==
              doctest::Approx(1.0));
        {
            std::ofstream f(tmp / "changji.toml", std::ios::binary);
            f << "[models]\nvideo_lora = \"loras/turbo.safetensors\"\n"
                 "video_lora_strength = 0.8\n";
        }
        const auto got = config::load_settings(tmp);
        CHECK(got.models.video_lora == "loras/turbo.safetensors");
        CHECK(got.models.video_lora_strength == doctest::Approx(0.8));

        // VAE 分块：0 = 用内置的 16×11，负数拒。调小它换显存——
        // VAE 放内存解码 71 秒，放显存 8 秒，而按内置块大小放显存差 112 MB。
        CHECK(config::ModelsConfig{}.video_vae_tile == 0);
        config::Settings bad;
        bad.models.video_vae_tile = -1;
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_vae_tile") != std::string::npos) said = true;
        }
        CHECK(said);
    }
    SUBCASE("随机数发生器：默认 auto 按家族，认 cpu/std，别的拒") {
        // sd.cpp 的默认是 cuda，Wan 那一路就用它；上游给 MiniMax-H3 的
        // 命令行是 --rng cpu。发生器不同则同一个种子出的画面不同，
        // **而且不报错**，所以这一项必须能配、且拼错要拦住。
        // 默认 auto：没配视频模型时落成 cuda（老默认），配了 H3 落成 cpu。
        CHECK(config::ModelsConfig{}.video_rng == "auto");
        CHECK(config::ModelsConfig{}.effective_video_rng() == "cuda");
        {
            std::ofstream f(tmp / "changji.toml", std::ios::binary);
            f << "[models]\nvideo_rng = \"cpu\"\n";
        }
        CHECK(config::load_settings(tmp).models.video_rng == "cpu");

        config::Settings bad;
        bad.models.video_rng = "gpu";   // 没有这个取值，是 cuda
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_rng") != std::string::npos) said = true;
        }
        CHECK(said);
    }
    SUBCASE("t5xxl 和 llm 两个编码器参数位只能填一个") {
        // MiniMax-H3 用裁过的 Qwen3-VL-32B 当编码器，在 sd.cpp 里是
        // llm_path；Wan 用 UMT5-XXL，是 t5xxl_path。**填错了不报错**——
        // 照常加载，然后出一段和提示词没关系的片，没有任何日志指到这儿。
        config::Settings bad;
        bad.models.video = "h3.gguf";
        bad.models.video_llm = "qwen3vl.gguf";
        bad.models.video_text_encoder = "umt5.safetensors";
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_llm") != std::string::npos) said = true;
        }
        CHECK(said);

        // 各填一个都行
        for (const bool use_llm : {true, false}) {
            CAPTURE(use_llm);
            config::Settings ok;
            ok.models.video = "v.gguf";
            ok.models.video_text_encoder.clear();
            ok.models.video_llm.clear();
            (use_llm ? ok.models.video_llm : ok.models.video_text_encoder) =
                "enc.gguf";
            for (const auto& e : ok.validate()) {
                CHECK_MESSAGE(e.find("video_llm") == std::string::npos, e);
            }
        }
    }
    SUBCASE("交班点要在 0 和 1 之间") {
        for (const double v : {0.0, 1.0, 1.5, -0.1}) {
            CAPTURE(v);
            config::Settings bad;
            bad.models.video_moe_boundary = v;
            bool said = false;
            for (const auto& e : bad.validate()) {
                if (e.find("video_moe_boundary") != std::string::npos) said = true;
            }
            CHECK(said);
        }
    }

}

TEST_CASE("[models].frame_tier：默认 final，认 draft，别的拒") {
    // **默认是 final，别改回 draft。** 曾经跟 Python 一样默认草稿档，
    // 但画幅搬到项目上（`[video]`）之后，那个默认只盖成片档的宽高，
    // 草稿档还停在档位表里的 512×288。默认草稿档 = 新用户什么都不配
    // 就拿 512×288 的首帧去喂 704×1280 的视频，锚点放大两倍再用。
    //
    // 这条**不会报错**，出来的片子只是"看着不太行"。所以钉在这儿。
    CHECK(config::ModelsConfig{}.frame_tier == "final");

    // 变量别叫 small：Windows 的 rpcndr.h 里 `#define small char`，
    // 报错是"Settings 后面接 char 是非法的"，指着的是下一行。
    config::Settings tiny;
    tiny.models.frame_tier = "draft";   // 小卡上显存不够，有意降档
    for (const auto& e : tiny.validate()) {
        CHECK_MESSAGE(e.find("frame_tier") == std::string::npos, e);
    }

    config::Settings ok;
    ok.models.frame_tier = "final";
    for (const auto& e : ok.validate()) {
        CHECK_MESSAGE(e.find("frame_tier") == std::string::npos, e);
    }

    config::Settings bad;
    bad.models.frame_tier = "preview";   // 有这个档位，但首帧只认两个
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("frame_tier") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("[models].vram_reserve_gb：默认 6，负数拒") {
    // 默认 6 GB 是量出来的：5090 上 1280×704 的 VAE 解码要 6576 MB。
    // 留少了的症状是出图全失败，而且在 sd.cpp 的日志接上之前，
    // 上层只看得到一句"出图失败，看一眼上面 sd.cpp 打的日志"。
    CHECK(config::ModelsConfig{}.vram_reserve_gb == doctest::Approx(6.0));

    config::Settings bad;
    bad.models.vram_reserve_gb = -1.0;
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("vram_reserve_gb") != std::string::npos) said = true;
    }
    CHECK(said);

    // 0 是合法的：大卡上不想留就不留
    config::Settings zero;
    zero.models.vram_reserve_gb = 0.0;
    for (const auto& e : zero.validate()) {
        CHECK_MESSAGE(e.find("vram_reserve_gb") == std::string::npos, e);
    }
}

TEST_CASE("[tiers]：填了以填的为准，没填按显存推") {
    // **这一节是被"设完重启就丢"逼出来的。** 档位以前只在进程内生效，
    // 用户把成片档调成 1280×704 跑了一章，重启回到 960×544，界面没提示。
    config::TiersConfig t;
    CHECK(t.draft_width == 0);          // 0 = 没填
    CHECK(t.validate().empty());

    SUBCASE("分辨率要是 32 的倍数") {
        // 不是的话 Wan 那一族潜空间对不齐，出图直接失败而日志指不到这儿。
        // **这条以前只在接口层查**，从配置文件进来是绕过的。
        config::TiersConfig bad;
        bad.final_width = 1000;
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("final_width") != std::string::npos) said = true;
        }
        CHECK(said);

        config::TiersConfig ok;
        ok.final_width = 1280;
        ok.final_height = 704;
        CHECK(ok.validate().empty());
    }
    SUBCASE("负数拒，步数不要求 32 的倍数") {
        config::TiersConfig bad;
        bad.final_steps = -1;
        CHECK_FALSE(bad.validate().empty());
        config::TiersConfig ok;
        ok.final_steps = 6;             // 6 不是 32 的倍数，但步数不受这条管
        CHECK(ok.validate().empty());
    }
}

TEST_CASE("[video]：横竖屏加清晰度，宽高算出来") {
    // **用户要决定的是"竖屏还是横屏、720p 还是 2K"**，不是
    // "928 还是 1440、544 还是 920"。中间那层换算不该甩给用户——
    // 填错一个不是 32 倍数的数，报错要到出图那一步才出现。
    //
    // 这一节放在**项目目录的 changji.toml** 里，一部电影一份：一台机器上可以
    // 同时有横屏的正片和竖版的物料，画幅是这部电影的属性不是这台机器的属性。
    // 不进 project.json——那份在对拍覆盖范围内，Python 没有这些字段。
    SUBCASE("默认横屏 720p") {
        // 2026-09-18 默认从 portrait 翻成 landscape：用户把产品定位从短剧
        // 改成电影制作平台，**电影是横的**。竖屏仍然是一个选项，只是不再是
        // 「不说话就有」的那一档。
        //
        // ⚠️ 这一条钉的只是「**没有项目**的时候内置默认是横的」。老项目
        // 不是靠"把画幅写进了自己的 changji.toml"躲开它的——很多老项目
        // 目录里根本没有 [video]（ProjectStore::create 不写那份文件）。
        // 它们靠的是 load_settings 从 assets.json 推，见下面那条
        // 「老项目没有 [video]」。
        const config::VideoConfig v;
        CHECK(v.orientation == "landscape");
        CHECK(v.quality == "720p");
        // **标准档的短边 544、长边 928**（2026-09-10 用户定的）。920 ÷ 32 =
        // 28.75 除不尽，取最近的 928 = 32 × 29；544 = 32 × 17。
        // 32 对齐是硬约束，不对齐 sd.cpp 直接出图失败。横屏是宽 928 高 544。
        CHECK(v.size() == std::pair<int, int>{928, 544});
    }
    SUBCASE("竖屏把长短边调过来") {
        config::VideoConfig v;
        v.orientation = "portrait";
        CHECK(v.size() == std::pair<int, int>{544, 928});
        v.quality = "2k";
        CHECK(v.size() == std::pair<int, int>{1440, 2560});
    }
    SUBCASE("**四种组合的宽高都得是 32 的倍数**") {
        // Wan 那一族的潜空间要求。不对齐出图直接失败，日志里指不到这儿，
        // 所以这条要钉死，不能靠人每次心算。
        for (const char* o : {"portrait", "landscape"}) {
            for (const char* q : {"720p", "2k"}) {
                config::VideoConfig v;
                v.orientation = o;
                v.quality = q;
                const auto [w, h] = v.size();
                // CAPTURE 一个 const char* 打的是指针，看不出是哪一组
                CAPTURE(std::string(o));
                CAPTURE(std::string(q));
                CHECK(w % 32 == 0);
                CHECK(h % 32 == 0);
            }
        }
    }
    SUBCASE("单镜上限：0 = 自己定，填了至少 2 秒") {
        config::VideoConfig v;
        CHECK(v.max_shot_s == 0.0);
        CHECK(v.validate().empty());
        v.max_shot_s = 5.0;
        CHECK(v.validate().empty());
        v.max_shot_s = -1.0;
        CHECK_FALSE(v.validate().empty());
        v.max_shot_s = 1.0;   // 最短的档位是 2 秒，1 秒排不出任何镜头
        CHECK_FALSE(v.validate().empty());
    }
    SUBCASE("认不出的取值要拒，别悄悄当默认") {
        for (const auto& [field, bad] :
             std::vector<std::pair<std::string, std::string>>{
                 {"orientation", "竖"}, {"quality", "1080p"}}) {
            config::VideoConfig v;
            (field == "orientation" ? v.orientation : v.quality) = bad;
            CAPTURE(field);
            CAPTURE(bad);
            CHECK_FALSE(v.validate().empty());
        }
    }
}

TEST_CASE("老项目没有 [video]：画幅从它自己的 assets.json 推") {
    // **判据是「这个项目自己说了算」。**
    //
    // 2026-09-18 内置默认从 portrait 翻成 landscape，理由是「老项目把画幅
    // 写进了自己的 changji.toml」。那个前提不成立：写 changji.toml 的是
    // http::post_new_project，ProjectStore::create 不写——手工建的、早于
    // 项目模板的老项目目录里根本没有 [video]（仓库自己的夹具
    // tests/golden/项目_雨夜天台/ 就是这一类，只有 assets.json 和
    // project.json）。翻默认于是把它们全改成横屏：已经出了两百镜 544×928
    // 的项目再补一镜出的是 928×544，同一章两种画幅而全程不报错。
    //
    // 画幅在盘上是有记录的——assets.json 里那份比例，是这个项目所有参考图
    // 和已出镜头的实际比例。这条用例钉的就是「读它、而不是读内置默认」。
    const changji::test::ScopedUserConfigDir iso("老项目画幅");
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_老项目画幅");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const auto write_assets = [&](const std::string& style_body) {
        std::ofstream out(root / "assets.json", std::ios::binary);
        REQUIRE(out.good());
        out << R"({"characters":{},"locations":{},"style":{)" << style_body
            << "}}";
    };

    SUBCASE("assets.json 说 9:16，读出来就是竖屏") {
        write_assets(R"("style_line":"realistic","aspect_ratio":"9:16")");
        const auto s = config::load_settings(root);
        CHECK(s.video.orientation == "portrait");
        // 出图那两层拿的是比例不是画幅，两头得对得上——对不上的表现是
        // 首帧竖的、成片横的，而参考图正是每一镜的底子。
        CHECK(s.video.aspect_ratio() == "9:16");
        CHECK(s.video.size() == std::pair<int, int>{544, 928});
    }

    SUBCASE("assets.json 说 16:9，读出来就是横屏") {
        write_assets(R"("style_line":"realistic","aspect_ratio":"16:9")");
        CHECK(config::load_settings(root).video.orientation == "landscape");
    }

    SUBCASE("比这个字段还老的 assets.json：没有那一栏也算数据") {
        // 缺这一栏说明这份文件比 aspect_ratio 这个字段还老，而那个年代
        // 建的项目全是竖屏的。按 StyleProfile 自己的初值算，正好答对。
        write_assets(R"("style_line":"realistic","global_style":"")");
        CHECK(config::load_settings(root).video.orientation == "portrait");
    }

    SUBCASE("读不出那一栏的一律按竖屏算") {
        // **成因是同一个，答案就得是同一个。** 下面四种文件各有各的坏法，
        // 说的却是同一件事：这份 assets.json 比 aspect_ratio 这个字段还老，
        // 或者被手改歪了。它们和上面「缺那一栏」是一家的。
        //
        // 答横屏的后果正是这条推导本来要防的那件事：一份连 style 都没有的
        // 极老竖屏项目补一镜走 scaled_to("16:9") 出 928×544，同一章两种
        // 画幅而全程不报错。
        //
        // 对账的另一头是 ProjectStore::load_assets：它读同一份文件给的是
        // 9:16（style 缺那一支回 def.style）或者直接抛，**没有一种会答
        // 横屏**。两个读法对同一份盘给不同答案时谁也看不出来——一边是设置页
        // 上显示的画幅，一边是出图那几层实际用的比例。
        const auto write_raw = [&](const std::string& body) {
            std::ofstream out(root / "assets.json", std::ios::binary);
            REQUIRE(out.good());
            out << body;
        };
        for (const std::string& body : {
                 // 整个 style 对象都没有：比"缺 aspect_ratio 一栏"还老一辈
                 std::string(R"({"characters":{},"locations":{}})"),
                 // style 被手改成了别的形状
                 std::string(R"({"characters":{},"style":"写实"})"),
                 // 那一栏类型不对（手改时写成了数字）
                 std::string(R"({"style":{"aspect_ratio":916}})"),
                 // 文件根本不是合法 JSON
                 std::string("{ 这不是 json"),
             }) {
            write_raw(body);
            CAPTURE(body);
            // 坏文件不能让配置加载整个失败——那会连设置页都打不开，
            // 而用户看到的错和"画幅"八竿子打不着。
            CHECK_NOTHROW(config::load_settings(root));
            const auto s = config::load_settings(root);
            CHECK(s.video.orientation == "portrait");
            CHECK(s.video.size() == std::pair<int, int>{544, 928});
        }
    }

    SUBCASE("比例认不出来才退回内置默认") {
        // **唯一一种"真认不出"**：那一栏在、是字符串、却既不是 16:9 也不是
        // 9:16（手改过，或者哪天加了档而 orientation_of_aspect 那张表没跟）。
        // 这时候盘上说的是一件这个函数答不上来的事，不硬猜。
        write_assets(R"("style_line":"realistic","aspect_ratio":"1:1")");
        CHECK(config::load_settings(root).video.orientation == "landscape");
    }

    SUBCASE("项目自己写了 [video] 就以它为准，不看 assets.json") {
        write_assets(R"("style_line":"realistic","aspect_ratio":"9:16")");
        {
            std::ofstream toml(root / "changji.toml", std::ios::binary);
            toml << "[video]\norientation = \"landscape\"\n";
        }
        CHECK(config::load_settings(root).video.orientation == "landscape");
    }

    SUBCASE("目录里什么都没有：内置默认，横屏") {
        // **"连 assets.json 都没有"和"assets.json 读不出来"不是一回事。**
        // ProjectStore::create 建目录那一下就把 assets.json 写下来了（还顺手
        // 把当时的画幅钉进那一栏），所以一个连它都没有的目录不是一份等着被
        // 认出来的老项目，是盘上一个字都没说——那种交给内置默认答。
        CHECK(config::load_settings(root).video.orientation == "landscape");
    }

    fs::remove_all(root, ec);
}

TEST_CASE("比例反推画幅：和 aspect_ratio() 是同一张表的两头") {
    // 两头必须闭合。分家的表现是"这个项目读出来的画幅和它自己的参考图
    // 对不上"，全程不报错。
    for (const char* o : {"portrait", "landscape"}) {
        config::VideoConfig v;
        v.orientation = o;
        const auto back = config::orientation_of_aspect(v.aspect_ratio());
        CAPTURE(std::string(o));
        REQUIRE(back.has_value());
        CHECK(*back == o);
    }
    // 认不出来要说"认不出来"，不许挑一个当默认——挑了的话，哪天加一档
    // 1:1，所有 1:1 的项目会被静悄悄当成横屏。
    CHECK_FALSE(config::orientation_of_aspect("1:1").has_value());
    CHECK_FALSE(config::orientation_of_aspect("").has_value());
}

TEST_CASE("老配置里的 comfy 自己换掉，不是让程序起不来") {
    // **拆掉一条路之后，老配置不能让程序起不来。**
    //
    // 拆 ComfyUI 时只在 validate() 里加了迁移说明，于是升级上来的用户
    // 遇到的是：程序直接退出，往 stderr 打一句"已经不支持了"。
    // 双击启动的人连那句都看不到，窗口一闪就没了。而那句话让他去改的
    // toml，正是他多半不知道在哪的那个文件——他本来会去设置页改，
    // 可设置页就是这个进程发的，起不来就打不开。
    //
    // 这一条是在本机跑二进制时真栽的：自己的 ~/.config 里还留着
    // tts.backend = "comfy"，程序起不来。
    config::Settings s;
    s.tts.backend = "comfy";
    s.models.engine = "comfy";

    const auto notes = config::migrate_legacy(s);

    CHECK(s.tts.backend == "local");
    CHECK(s.models.engine == "sd");
    // 换完必须是合法的，否则只是把"起不来"往后挪了一步
    for (const auto& e : s.validate()) {
        CHECK_MESSAGE(e.find("tts.backend") == std::string::npos, e);
        CHECK_MESSAGE(e.find("models.engine") == std::string::npos, e);
    }

    // **两处都要说一声。** 悄悄换掉比报错更糟：用户以为还在走 ComfyUI。
    REQUIRE(notes.size() == 2);
    bool said_tts = false, said_engine = false;
    for (const auto& n : notes) {
        if (n.find("[tts].backend") != std::string::npos) said_tts = true;
        if (n.find("[models].engine") != std::string::npos) said_engine = true;
        CHECK(n.find("comfy") != std::string::npos);   // 说清是从什么换过来的
    }
    CHECK(said_tts);
    CHECK(said_engine);
}

TEST_CASE("没有老取值时迁移一句话都不说") {
    // 每次启动都刷两行"已经帮你改了"，用户会当噪音略过——
    // 而真需要看的那一次也就跟着略过了。
    config::Settings s;
    CHECK(config::migrate_legacy(s).empty());
    CHECK(s.tts.backend == "local");
    CHECK(s.models.engine == "sd");

    // 认得的取值也不该被动
    config::Settings http;
    http.tts.backend = "http";
    CHECK(config::migrate_legacy(http).empty());
    CHECK(http.tts.backend == "http");
}

TEST_CASE("帧率跟着出片模型纠回去，并且说一声") {
    // **这一条堵的是第二条入口。** 出片那条路每跑一章都从项目的
    // changji.toml 重读一遍设置（http/run.cpp 的 load_settings(store.root())），
    // 根本不经过 Runtime::replace——只在 Runtime 里纠正的话，整章会带着
    // 错的帧率跑完，而表现不是报错是**整片变速**。
    config::Settings s;
    s.models.video = "minimax_h3_fl2va-Q4_K_M.gguf";
    s.assembly.fps = 30;

    const auto notes = config::migrate_legacy(s);

    CHECK(s.assembly.fps == 24);
    REQUIRE(notes.size() == 1);
    // 说清楚从几改到几，否则用户只看到片子不对、不知道是谁改的。
    CHECK(notes[0].find("30") != std::string::npos);
    CHECK(notes[0].find("24") != std::string::npos);
    CHECK(notes[0].find("[assembly].fps") != std::string::npos);

    SUBCASE("本来就对就一句话都不说") {
        config::Settings ok;
        ok.models.video = "minimax_h3_fl2va-Q4_K_M.gguf";
        ok.assembly.fps = 24;
        CHECK(config::migrate_legacy(ok).empty());
        CHECK(ok.assembly.fps == 24);
    }

    SUBCASE("模型不挑帧率就别替人做主") {
        // sd.cpp 对 Wan 不做覆盖，传什么用什么。
        config::Settings wan;
        wan.models.video = "wan2.2_ti2v_5B.gguf";
        wan.assembly.fps = 30;
        CHECK(config::migrate_legacy(wan).empty());
        CHECK(wan.assembly.fps == 30);
    }

    SUBCASE("名字认不出时看编码器走哪条路") {
        // H3 挂 video_llm，Wan 挂 video_text_encoder。
        config::Settings guess;
        guess.models.video = "my_video_model.gguf";
        guess.models.video_llm = "qwen3vl_32b_minimax_h3-Q4_K_M.gguf";
        guess.assembly.fps = 16;
        CHECK(config::migrate_legacy(guess).size() == 1);
        CHECK(guess.assembly.fps == 24);
    }
}

TEST_CASE("这一轮真正会用的规格：出片跟 Turbo，首帧不跟") {
    // **界面和真跑的必须是同一个数。** 2026-09-10 用户问"怎么没用 turbo"，
    // 因为设置页照着档位表显示"成片步数 28"，而每一镜实际跑的是 6 步。
    // 那时候这段判断只写在 run.cpp 里，设置页那边自己算——必然分叉。
    // 现在两边都调 effective_spec，这条用例钉住它的行为。
    config::Settings s;
    s.video.orientation = "portrait";
    s.video.quality = "720p";

    SUBCASE("没挂 Turbo：出片和首帧都用档位表的数") {
        // video_lora 留空 = 没挂
        s.models.video_lora = "";
        const auto e = config::effective_spec(s, 28);
        CHECK_FALSE(e.turbo);
        CHECK(e.final_steps == 28);
        CHECK(e.frame_steps == 28);
        CHECK(e.width == 544);      // 标准档 2026-09-10 从 704×1280 改成
        CHECK(e.height == 928);     // 544×928，见 VideoConfig::size()
    }

    SUBCASE("挂了 Turbo：出片 6 步，首帧还是档位表那个数") {
        // 文件真的要在——配了个不存在的路径不算挂上，那时候压到 6 步
        // 就是拿一个没有 LoRA 的模型跑 6 步，画面直接废掉。
        const auto dir = std::filesystem::temp_directory_path() /
                         "changji_eff_spec";
        std::filesystem::create_directories(dir / "loras");
        { std::ofstream f(dir / "loras" / "turbo.safetensors"); f << "x"; }
        s.models.dir = paths::to_utf8(dir);
        s.models.video_lora = "loras/turbo.safetensors";

        const auto e = config::effective_spec(s, 28);
        CHECK(e.turbo);
        CHECK(e.final_steps == 6);
        // **这一条是关键**：Turbo 只挂在视频模型上，出图那一步没有它。
        CHECK(e.frame_steps == 28);

        // 配了但文件不在 = 没挂上
        config::Settings missing = s;
        missing.models.video_lora = "loras/不存在.safetensors";
        const auto m = config::effective_spec(missing, 28);
        CHECK_FALSE(m.turbo);
        CHECK(m.final_steps == 28);

        std::filesystem::remove_all(dir);
    }

    SUBCASE("用户在 [tiers] 里写死了步数，Turbo 不许改它") {
        s.tiers.final_steps = 12;
        const auto e = config::effective_spec(s, 28);
        CHECK(e.steps_pinned);
        CHECK(e.final_steps == 12);
    }

    SUBCASE("[models].frame_steps 填了就以它为准") {
        s.models.frame_steps = 20;
        CHECK(config::effective_spec(s, 28).frame_steps == 20);
    }

    SUBCASE("画幅跟项目走") {
        s.video.orientation = "landscape";
        s.video.quality = "2k";
        const auto e = config::effective_spec(s, 28);
        CHECK(e.width == 2560);
        CHECK(e.height == 1440);
    }
}


TEST_CASE("[models].image_weights：图像模型的权重放哪，不跟 weights 走") {
    // **量出来的。** 5090 上 weights = "cpu"（给 18 GB 视频模型的）让 7 GB
    // 图像模型也每一步从内存搬权重：采样时 GPU 利用率 18%、一步 6.8 秒；
    // 扩散权重常驻时 82%、一步 1.25 秒。慢五倍，而且看起来像"显卡没吃满"。
    config::ModelsConfig m;
    CHECK(m.image_weights == "smart");
    m.weights = "cpu";
    // **按模型大小算，不是按卡算。** 32.6 GB 的 5090：
    //   fp8 20 GB → 20 + 6.6 + 4 = 30.6 > 29.3，装不下→cpu（实测第 34/62 段 OOM）
    //   Q6_K 16 GB → 26.6 ≤ 29.3，装得下→常驻
    CHECK(m.image_weights_for(32.6, 20.0) == "cpu");
    // 装得下：编码器放内存，VAE 留显存（理由见上面统一内存那一条）
    CHECK(m.image_weights_for(32.6, 16.0) == "te=cpu");
    CHECK(m.image_weights_for(32.6, 12.0) == "te=cpu");
    // 小卡：全放内存，不然加载就 OOM
    CHECK(m.image_weights_for(6.0, 12.0) == "cpu");
    // 拿不到模型大小：按装不下处理——猜错是六镜全废，放内存只是慢
    CHECK(m.image_weights_for(32.6, 0.0) == "cpu");
    // 显式填了就照填的来
    m.image_weights = "auto";
    CHECK(m.image_weights_for(32.6, 20.0) == "auto");
    // 视频那一项一个字不动
    CHECK(m.weights_for(32.0, 18.8) == "cpu");
}

// ---- 老实的显存需求（调度器问过卡之后拿它比） ----

TEST_CASE("老实数 = 常驻权重 + 计算缓冲，和决定放哪用同一组常数") {
    config::ModelsConfig m;

    SUBCASE("权重全放内存：只剩缓冲") {
        // 视频 14.6；图像 6.6 + 4.0。
        CHECK(m.video_live_vram_gb("cpu", 18.8) == doctest::Approx(14.6));
        CHECK(m.image_live_vram_gb("cpu", 20.0) == doctest::Approx(10.6));
    }
    SUBCASE("只有扩散常驻") {
        CHECK(m.video_live_vram_gb("te=cpu,vae=cpu", 18.8) ==
              doctest::Approx(18.8 + 14.6));
        CHECK(m.image_live_vram_gb("te=cpu,vae=cpu", 16.0) ==
              doctest::Approx(16.0 + 10.6));
    }
    SUBCASE("VAE 也常驻：再加 5.5") {
        CHECK(m.video_live_vram_gb("te=cpu", 18.8) ==
              doctest::Approx(18.8 + 14.6 + 5.5));
    }
    SUBCASE("不认得的规格按全常驻算——估高只是多卸一次，估低是 OOM") {
        CHECK(m.video_live_vram_gb("auto", 18.8) ==
              doctest::Approx(18.8 + 14.6 + 5.5));
    }
}

TEST_CASE("老实数要和 *_for 的判断对得上") {
    // 两边共用常数，所以"算得下"和"要占多少"必须自洽：
    // weights_for 说装得下的时候，老实数就该不超过整卡。
    config::ModelsConfig m;
    const double card = 32.6;
    const double model = 16.0;             // Q6_K
    const std::string w = m.weights_for(card, model);
    CHECK(m.video_live_vram_gb(w, model) <= card);

    // 而 H3 那种 18.8 GB 的，同一张卡上 weights_for 会判 "cpu"，
    // 老实数也就只剩缓冲，仍然装得下。
    const std::string w2 = m.weights_for(card, 18.8);
    CHECK(w2 == "cpu");
    CHECK(m.video_live_vram_gb(w2, 18.8) <= card);
}

TEST_CASE("大模型的老实数：权重 + KV 缓存和上下文") {
    config::ModelsConfig m;
    // 实测那一组：9 GB 的文件载进去 15.4 GB，式子给 15.25，对得上。
    CHECK(m.llm_live_vram_gb(9.0) == doctest::Approx(15.25));
    // **多出来的那部分不是常数**：KV 缓存跟着模型大小走。
    // 写成"加一个固定值"的话只在锚点上对，模型一换就偏。
    const double d1 = m.llm_live_vram_gb(4.0) - 4.0;
    const double d2 = m.llm_live_vram_gb(20.0) - 20.0;
    CHECK(d2 > d1);
    // 拿不到文件大小就说"没有"，让调用方退回保守估算。
    // **不许猜**：猜小了是 OOM，退回保守只是多卸一次。
    CHECK(m.llm_live_vram_gb(0.0) == doctest::Approx(0.0));
    CHECK(m.llm_live_vram_gb(-1.0) == doctest::Approx(0.0));
}

// ---- 三档画幅 ----
//
// 2026-09-10 按用户要求把标准档从 704×1280 改成 544×928；9-11 他说
// "糊掉、变形"。同一章里 sh001 是 704×1280、sh002 是 544×928，像素
// 90 万对 50 万，差 44%——就是这个。所以把 704×1280 作为「高清」加回来，
// 让他自己挑，而不是我来回翻。

TEST_CASE("画幅三档，每一档都必须是 32 的倍数") {
    // 不对齐的话 Wan 那一族的潜空间对不上，**出图直接失败而且日志里
    // 指不到这儿**。第一版把 920 写进去时就是这条用例抓住的。
    config::VideoConfig v;
    for (const char* q : {"720p", "hd", "2k"}) {
        v.quality = q;
        for (const char* o : {"portrait", "landscape"}) {
            v.orientation = o;
            const auto [w, h] = v.size();
            CAPTURE(q);
            CAPTURE(o);
            CHECK(w % 32 == 0);
            CHECK(h % 32 == 0);
            CHECK(w > 0);
            CHECK(h > 0);
        }
    }
}

TEST_CASE("三档的具体尺寸和界面上写的一致") {
    config::VideoConfig v;
    v.orientation = "portrait";
    v.quality = "720p";
    CHECK(v.size() == std::pair<int, int>{544, 928});
    v.quality = "hd";
    CHECK(v.size() == std::pair<int, int>{704, 1280});
    v.quality = "2k";
    CHECK(v.size() == std::pair<int, int>{1440, 2560});

    // 横屏是长短边对调，不是另一套数
    v.orientation = "landscape";
    v.quality = "hd";
    CHECK(v.size() == std::pair<int, int>{1280, 704});
}

TEST_CASE("清晰度只认这三个值") {
    config::VideoConfig v;
    v.orientation = "portrait";
    for (const char* q : {"720p", "hd", "2k"}) {
        v.quality = q;
        CAPTURE(q);
        CHECK(v.validate().empty());
    }
    // 写错了要拦下来并且**把认得的值列出来**——只说"不合法"的话
    // 用户不知道该填什么
    v.quality = "1080p";
    const auto errs = v.validate();
    REQUIRE_FALSE(errs.empty());
    CHECK(errs[0].find("hd") != std::string::npos);
    CHECK(errs[0].find("720p") != std::string::npos);
    CHECK(errs[0].find("2k") != std::string::npos);
}

TEST_CASE("profile 报的步数是真正会跑的那个，不是档位表里的") {
    // 实跑撞上的：/api/hardware 说成片档 30 步、908.7 秒，而日志里是
    // 「第 3/6 步」、整镜 210 秒。档位表的步数假设不挂蒸馏 LoRA，挂了 Turbo
    // 之后 effective_spec 会改成 6——但那一步在出片的路上，不在 profile 里。
    // profile 又是 /api/hardware 和 /api/run/preview 的唯一来源，于是界面上
    // 的规格和「要等多久」两个数都是错的，而人按它安排时间。
    // 中文目录名要过 paths::from_utf8，直接用窄字符串在中文 locale 的
    // Windows 上会抛「No mapping for the Unicode character」。
    const auto dir = std::filesystem::temp_directory_path() /
                     paths::from_utf8("changji_步数一致");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "loras", ec);
    // 挂一个真实存在的 LoRA 文件，effective_spec 才认它
    { std::ofstream f(dir / "loras" / "turbo.safetensors"); f << "x"; }

    config::Settings s;
    s.models.dir = paths::to_utf8(dir);
    s.models.video_lora = "loras/turbo.safetensors";
    s.tiers.final_steps = 0;          // 没人钉死步数，让 Turbo 说了算

    config::runtime().replace(s);
    const auto p = config::runtime().profile();
    const auto fin = p.tiers.find(models::Tier::FINAL);
    REQUIRE(fin != p.tiers.end());
    CHECK(fin->second.steps == 6);    // 不是档位表里那个 28/30

    SUBCASE("耗时跟着步数一起缩，不能还报按 30 步标定的那个数") {
        if (fin->second.measured_seconds.has_value()) {
            CHECK(*fin->second.measured_seconds > 0.0);
            // 6 步的活不可能比 28 步还久
            config::Settings bare = s;
            bare.models.video_lora = "";
            config::runtime().replace(bare);
            const auto p2 = config::runtime().profile();
            const auto fin2 = p2.tiers.find(models::Tier::FINAL);
            REQUIRE(fin2 != p2.tiers.end());
            CHECK(fin2->second.steps > 6);
            if (fin2->second.measured_seconds.has_value()) {
                CHECK(*fin->second.measured_seconds <
                      *fin2->second.measured_seconds);
            }
        }
    }

    SUBCASE("人显式钉了步数就别替他改") {
        config::Settings pinned = s;
        pinned.tiers.final_steps = 28;
        config::runtime().replace(pinned);
        const auto p3 = config::runtime().profile();
        CHECK(p3.tiers.at(models::Tier::FINAL).steps == 28);
    }

    SUBCASE("画幅也要换成真正会跑的那个，不能只换步数") {
        // **上一版就只换了步数。** 于是 /api/hardware 报 1920×1088，而磁盘上
        // 的成片是 544×928（ffprobe 量出来的）——同一个函数里同一个毛病，
        // 修了一半，而画幅那一项差 4.13 倍像素，比步数还大。
        //
        // **这一条必须显式选竖屏**。2026-09-18 默认翻成横屏之后，项目画幅和
        // 档位表推出来的那个（1920×1088，横的）形状一样了——「没换」和
        // 「换了」长得一模一样，下面那条朝向断言就白写了。竖屏是唯一能让
        // 两者分得开的那一档。
        config::Settings portrait = s;
        portrait.video.orientation = "portrait";
        config::runtime().replace(portrait);
        const auto pp = config::runtime().profile();
        const auto pfin = pp.tiers.find(models::Tier::FINAL);
        REQUIRE(pfin != pp.tiers.end());

        const auto [want_w, want_h] = portrait.video.size();
        CHECK(pfin->second.width == want_w);
        CHECK(pfin->second.height == want_h);
        // 竖屏：高一定大于宽。档位表里推出来的是横的（1920×1088），
        // 没换的话这一条就挂了。
        CHECK(pfin->second.height > pfin->second.width);
    }

    config::runtime().replace(config::Settings{});
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("workload_scale：档位表的耗时换算到真跑那一档") {
    // 一镜的时间分两段：采样跟步数走，解码不跟（VAE 跑的是最后那一次潜空间）。
    // 全按步数比缩会明显偏小——实测报 9 分钟、实际 14 分钟。
    config::EffectiveSpec eff;
    eff.width = 544;
    eff.height = 928;
    eff.final_steps = 6;

    // 表里 1920×1088 / 20 步 → 真跑 544×928 / 6 步
    const double got = config::workload_scale(1920, 1088, 20, eff);
    const double px = 544.0 * 928.0 / (1920.0 * 1088.0);          // 0.242
    CHECK(got == doctest::Approx(px * (0.2 + 0.8 * 6.0 / 20.0)));

    // **一定比纯步数比大**：解码那段没跟着降，报小了比没有预演更糟。
    CHECK(got > px * (6.0 / 20.0));
    // 也一定比"只缩像素不缩步数"小，否则等于没算步数。
    CHECK(got < px);

    SUBCASE("规格没变就是 1.0，别把标定值改坏") {
        config::EffectiveSpec same;
        same.width = 1920;
        same.height = 1088;
        same.final_steps = 20;
        CHECK(config::workload_scale(1920, 1088, 20, same) ==
              doctest::Approx(1.0));
    }

    SUBCASE("表里的数不合法时返回 1.0，不拿 0 去除") {
        CHECK(config::workload_scale(0, 1088, 20, eff) == doctest::Approx(1.0));
        CHECK(config::workload_scale(1920, 1088, 0, eff) == doctest::Approx(1.0));
        config::EffectiveSpec empty;
        CHECK(config::workload_scale(1920, 1088, 20, empty) ==
              doctest::Approx(1.0));
    }
}

TEST_CASE("单镜上限是这部电影的属性：[video].max_shot_s 往下夹，不往上抬") {
    // 2026-09-13 实测：H3 权重全放内存时峰值显存不随帧数涨，「按显存推
    // 单镜上限」推的是个不存在的量；而放开到 8 秒的镜头会中途硬切成另一
    // 场戏。五秒上下本来就是一个镜头的常见长度。所以由这部电影自己定。
    config::Settings s;
    s.models.video = "minimax_h3_fl2va-Q4_K_M.gguf";   // 17k+5，模型上限 360

    // 0 = 自己定：走模型 ∩ 显卡 ∩ 内核那条链，结果至少保住五秒那一档
    s.video.max_shot_s = 0.0;
    const auto auto_limits = config::video_limits_for(s);
    CHECK(auto_limits.frame_step == 17);
    CHECK(auto_limits.max_frames >= 124);

    // 这部电影要 3 秒：3×24 = 72 → 17k+5 对齐到 73。任何卡上都比五秒那道地板低，
    // 所以这个数不受测试机有没有显卡影响
    s.video.max_shot_s = 3.0;
    CHECK(config::video_limits_for(s).max_frames == 73);
    CHECK(config::video_limits_for(s).duration_slots() ==
          std::vector<double>{2.0, 3.0});

    // 这部电影要 15 秒：只能往下夹，不会把机器的上限抬上去
    s.video.max_shot_s = 15.0;
    CHECK(config::video_limits_for(s).max_frames == auto_limits.max_frames);

    // 手填的 video_max_frames 是机器的上限，这部电影的要求仍然只往下夹
    s.models.video_max_frames = 90;
    s.video.max_shot_s = 3.0;
    CHECK(config::video_limits_for(s).max_frames == 73);
    s.video.max_shot_s = 5.0;
    CHECK(config::video_limits_for(s).max_frames == 90);
}

TEST_CASE("只有编辑模型才收参考图：按文件名认 edit") {
    // sd.cpp 看到 ref_images 就走 EDIT mode；基础版 Qwen-Image 出来的是
    // 参考图的翻版（2026-09-13 项目 321 ep01_sh002 实见）。见
    // ModelsConfig::accepts_reference_images 头上那段。
    using config::ModelsConfig;
    CHECK_FALSE(ModelsConfig::accepts_reference_images("qwen-image-Q6_K.gguf"));
    CHECK_FALSE(ModelsConfig::accepts_reference_images(
        "/root/models/qwen_image_fp8_e4m3fn.safetensors"));
    CHECK(ModelsConfig::accepts_reference_images("Qwen_Image_Edit-Q8_0.gguf"));
    CHECK(ModelsConfig::accepts_reference_images("Qwen-Image-Edit-2509-Q4_K_S.gguf"));
    CHECK(ModelsConfig::accepts_reference_images("qwen-image-edit-2511-Q4_K_M.gguf"));
    // 目录名里的 edit 不算：模型是哪个看文件
    CHECK_FALSE(ModelsConfig::accepts_reference_images("D:/edit_models/qwen-image-Q4.gguf"));
    CHECK_FALSE(ModelsConfig::accepts_reference_images(""));
}

TEST_CASE("Qwen-Image-Edit 2509/2511 要一起填视觉塔，初版 Edit 和基础模型不用") {
    // 不填的话 sd.cpp 只说一句 vision disabled 就照常出图，出来的和参考图
    // 对不上——那种错人会先去怀疑参考图本身。见 validate 里那段。
    const auto said = [](const config::ModelsConfig& m) {
        for (const auto& e : m.validate()) {
            if (e.find("image_text_encoder_vision") != std::string::npos) return true;
        }
        return false;
    };
    config::ModelsConfig m;
    m.image = "Qwen-Image-Edit-2509-Q4_K_S.gguf";
    CHECK(said(m));
    m.image = "qwen-image-edit-2511-Q4_K_M.gguf";
    CHECK(said(m));
    m.image_text_encoder_vision = "Qwen2.5-VL-7B-Instruct.mmproj-Q8_0.gguf";
    CHECK_FALSE(said(m));
    m.image_text_encoder_vision.clear();
    m.image = "Qwen_Image_Edit-Q8_0.gguf";   // 初版 Edit 不要视觉塔
    CHECK_FALSE(said(m));
    m.image = "qwen-image-Q6_K.gguf";        // 基础模型根本不收参考图
    CHECK_FALSE(said(m));
}

TEST_CASE("cfg / rng 的默认按出片模型家族给，别再默认 Wan 的 6.0") {
    // video_lora / video_max_frames 的默认早就按 H3 了，cfg 还默认 6.0 是
    // 自相矛盾：手改 [models].video 换成 H3 的人拿 6.0 跑，多跑一遍 uncond
    // 还不报错。上游 docs：H3 --cfg-scale 1.0 --rng cpu；Wan A14B 3.5；5B 6.0。
    config::ModelsConfig m;
    CHECK(m.video_cfg == 0.0);
    CHECK(m.video_family() == config::ModelsConfig::VideoFamily::Unknown);
    CHECK(m.effective_video_cfg() == doctest::Approx(6.0));

    m.video = "minimax_h3_fl2va-Q4_K_M.gguf";
    CHECK(m.video_family() == config::ModelsConfig::VideoFamily::MiniMaxH3);
    CHECK(m.effective_video_cfg() == doctest::Approx(1.0));
    CHECK(m.effective_video_rng() == "cpu");

    m.video = "wan2.2_ti2v_5B_fp16.safetensors";
    CHECK(m.video_family() == config::ModelsConfig::VideoFamily::Wan5B);
    CHECK(m.effective_video_cfg() == doctest::Approx(6.0));
    CHECK(m.effective_video_rng() == "cuda");
    m.video_high_noise = "Wan2.2-I2V-A14B-HighNoise-Q8_0.gguf";
    CHECK(m.video_family() == config::ModelsConfig::VideoFamily::WanA14B);
    CHECK(m.effective_video_cfg() == doctest::Approx(3.5));

    // 名字认不出就看编码器：挂了 video_llm 就是 H3 那一路
    m = config::ModelsConfig{};
    m.video = "mystery.gguf";
    m.video_llm = "qwen3vl_32b_minimax_h3-Q4_K_M.gguf";
    CHECK(m.effective_video_cfg() == doctest::Approx(1.0));

    // 填了就是填的，家族不管
    m.video = "minimax_h3_fl2va-Q4_K_M.gguf";
    m.video_cfg = 2.0;
    m.video_rng = "std";
    CHECK(m.effective_video_cfg() == doctest::Approx(2.0));
    CHECK(m.effective_video_rng() == "std");

    SUBCASE("load_settings 读出来的已经是具体值，下游不用再问家族") {
        const fs::path tmp = fs::temp_directory_path() / paths::from_utf8("changji_cfg_family");
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        {
            std::ofstream f(tmp / "changji.toml", std::ios::binary);
            f << "[models]\nvideo = \"minimax_h3_fl2va-Q4_K_M.gguf\"\n";
        }
        const config::Settings s = config::load_settings(tmp);
        CHECK(s.models.video_cfg == doctest::Approx(1.0));
        CHECK(s.models.video_rng == "cpu");
        fs::remove_all(tmp, ec);
    }
}

TEST_CASE("干活那台按自己有没有 Turbo 定步数，人钉死的不动") {
    // 2026-09-15 实测：Mac（没 LoRA）按 20 步派给 L20（有 LoRA），
    // 那边挂着 Turbo 跑 20 步，一段 462 秒还过锐。
    const auto dir = std::filesystem::temp_directory_path() / "changji_node_steps";
    std::filesystem::create_directories(dir / "loras");
    { std::ofstream f(dir / "loras" / "turbo.safetensors"); f << "x"; }
    config::Settings node;
    node.models.dir = paths::to_utf8(dir);
    node.models.video_lora = "loras/turbo.safetensors";

    // 这台有 Turbo：派来 20 压成 6；派来 6 还是 6（幂等）
    CHECK(config::steps_on_node(node, 20, false) == 6);
    CHECK(config::steps_on_node(node, 6, false) == 6);
    // 人钉死的一律不动
    CHECK(config::steps_on_node(node, 28, true) == 28);
    // 这台自己的 final_steps 不算数——派来的活听派活那部电影的
    node.tiers.final_steps = 12;
    CHECK(config::steps_on_node(node, 20, false) == 6);
    // 这台没 Turbo：照派来的跑
    config::Settings bare;
    bare.models.video_lora = "";
    CHECK(config::steps_on_node(bare, 20, false) == 20);
    std::filesystem::remove_all(dir);
}

TEST_CASE("[llm].call_log_max_mb：两头都夹住，填离谱的数不许过") {
    // **上下界漏哪一头，症状都一模一样**：日志目录永远是空的，而人看到的是
    // 开关明明开着、index.jsonl 还在一行一行地长。因为这个数乘 1024×1024
    // 变成字节上限（llm/call_log.cpp 的 call_log_options），而「超了就剪」
    // 紧跟在写完正文之后跑——上限算成 0 就是把刚写下的那一份当场删掉。
    //   · 下界那头：0 和负数直接就是 0 字节。
    //   · 上界那头：1e30 转 uintmax_t 是未定义行为，实测常得 0，
    //     于是**填得越大反而一份都留不住**。设置页那条路上
    //     /api/connections 只判是不是数，手滑多按几个 0 就到这儿。
    // 所以两头各钉一条。
    CHECK(config::LLMConfig{}.call_log_max_mb == doctest::Approx(1024.0));
    CHECK(config::Settings{}.validate().empty());

    auto says = [](double mb) {
        config::Settings s;
        s.llm.call_log_max_mb = mb;
        for (const auto& e : s.validate()) {
            if (e.find("call_log_max_mb") != std::string::npos) return true;
        }
        return false;
    };

    SUBCASE("下界") {
        CHECK(says(0.0));
        CHECK(says(-1.0));
        // 比一次调用的正文还小的上限等于刚写下就被剪光，那和关掉没区别，
        // 而关掉有它自己的开关（call_log = false）。
        CHECK(says(0.5));
    }
    SUBCASE("上界") {
        CHECK(says(1e30));
        CHECK(says(1e9));
        CHECK(says(102400.0 * 2));
    }
    SUBCASE("正常填法一句都不该说") {
        CHECK_FALSE(says(1.0));
        CHECK_FALSE(says(1024.0));
        CHECK_FALSE(says(102400.0));
    }
    SUBCASE("nan 和 inf 也得挡住") {
        // **NaN 和任何数比都是假**，所以 `v < lo || v > hi` 这种写法对它
        // 一句话都不说。而 `nan` 是 toml 认的字面量（toml++ 直接给回
        // quiet_NaN），于是 `call_log_max_mb = nan` 能静默过关，一路传到
        // 记录器那边算出上限 0——**刚写下的正文当场被删光**，而开关明明开着、
        // index.jsonl 还在长。`inf` 被 `> hi` 挡得住，`nan` 挡不住，
        // 所以这两个要分开钉。判据收在 settings.cpp 的 check_range 里，
        // 那儿一改，所有走它的字段一起受益。
        CHECK(says(std::numeric_limits<double>::quiet_NaN()));
        CHECK(says(std::numeric_limits<double>::infinity()));
        CHECK(says(-std::numeric_limits<double>::infinity()));
    }
}

TEST_CASE("模板的 [llm] 那一节给了提示词日志这两行") {
    // 一个配置项要在**五处**同时在场才算真的有：settings.hpp 的字段、
    // settings.cpp 的 apply_table 和 validate、这份模板、
    // http/config_api.cpp 的可写字段表、field_names.inc.hpp 的中文名。
    // 模板是最容易漏的那一处——漏了不会红、解析照样过，只是用户拿到的第一个
    // 文件里根本没有这一项，于是"默认开着、会把整章正文写到磁盘上"这件事
    // 他永远不会知道。所以在这儿钉一条会响的。
    const std::string sec =
        toml_section(config::default_config_template(), "llm");
    REQUIRE_FALSE(sec.empty());

    for (const char* key : {"call_log", "call_log_max_mb"}) {
        CAPTURE(key);
        // **按 `键 = ` 的形状找**：散文里提一嘴不等于给了用户一行能改的东西。
        const std::string want = std::string("\n") + key + " = ";
        CHECK_MESSAGE(sec.find(want) != std::string::npos,
                      "[llm] 里没有 `" << key << " = ` 这一行");
    }
    // 这一节还得把「密钥不落盘」这句话说出来：提示词是原样落盘的，
    // 人看到「把每一次给大模型的提示词都留下来」第一反应就是问密钥。
    // **查的是那半句原话，不是「密钥」两个字**——这一节上面讲 api_key
    // 的地方就有「同一把密钥」，只查两个字的话这条用例永远绿。
    CHECK(sec.find("密钥不落盘") != std::string::npos);
}
