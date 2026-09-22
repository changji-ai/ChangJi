// 「电影质感」那一轮（2026-09-14，docs/电影质感方案.md）加的东西：
// 后期链、环境声和配乐的混音、放大命令、焦段和光的字段、时间码运动描述、
// 关键镜头多条 take、尾帧串镜、配乐阶段。
//
// 每一段都是"错了不当场报错"的那种：滤镜串拼错只会让成片颜色不对，
// 混音链拼错只会让配乐盖过台词，字段没进 required 只会让模型整个略过。
// 所以全部按纯函数钉死。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "gates/checks.hpp"
#include "infer/sd_video.hpp"
#include "media/assemble.hpp"
#include "models/character.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "stages/music.hpp"
#include "stages/prompt_compose.hpp"
#include "stages/render.hpp"
#include "stages/prompts.inc.hpp"
#include "stages/script.hpp"
#include "stages/storyboard.hpp"
#include "util/cmdline.hpp"
#include "util/paths.hpp"
#include <regex>

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string arg_value(const std::vector<std::string>& args,
                      const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || it + 1 == args.end()) return {};
    return *(it + 1);
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

models::AssetLibrary make_assets() {
    models::AssetLibrary a;
    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性";
    lin.appearance.face = "黑色长直发";
    lin.appearance.attire = "白色衬衫";
    a.characters["c_lin_wan"] = lin;
    a.style.style_line = models::StyleLine::REALISTIC;
    a.style.global_style = "冷调";
    a.style.negative_prompt = "低质量";
    a.style.aspect_ratio = "9:16";
    return a;
}

models::Shot make_shot(const std::string& id, int order, double dur = 5.0) {
    models::Shot s;
    s.shot_id = id;
    s.scene_id = "sc01";
    s.order = order;
    s.first_frame_prompt = "雨夜天台";
    s.motion_prompt = "雨丝斜掠";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.camera_move = models::CameraMove::PUSH_IN;
    s.duration_s = dur;
    models::CharacterInShot c;
    c.char_id = "c_lin_wan";
    c.action = "转身";
    s.characters.push_back(c);
    return s;
}

models::TierSpec make_spec() {
    models::TierSpec spec;
    spec.tier = models::Tier::DRAFT;
    spec.width = 448;
    spec.height = 256;
    spec.steps = 8;
    return spec;
}

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_电影感_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void touch(const fs::path& p, const std::string& body = "x") {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << body;
}

}  // namespace

// ---------------------------------------------------------------------------
// 命令模板
// ---------------------------------------------------------------------------

TEST_CASE("命令模板：按空白切、引号包住的算一个、占位符整个替换") {
    const auto argv = util::expand_command(
        "\"C:/Program Files/py/python.exe\" gen.py --prompt {prompt} --out={out} {seconds}",
        {{"prompt", "cinematic score, no vocals"},
         {"out", "D:/剧/output/ep01_music.wav"},
         {"seconds", "62"}});
    REQUIRE(argv.size() == 6);
    CHECK(argv[0] == "C:/Program Files/py/python.exe");
    CHECK(argv[1] == "gen.py");
    // 描述带空格也还是一个参数——不经过 shell，所以不用再引一遍
    CHECK(argv[3] == "cinematic score, no vocals");
    CHECK(argv[4] == "--out=D:/剧/output/ep01_music.wav");
    CHECK(argv[5] == "62");

    SUBCASE("空模板 = 没配") {
        CHECK(util::expand_command("", {}).empty());
        CHECK(util::expand_command("   \n", {}).empty());
    }
    SUBCASE("不认识的占位符原样留着，那是用户写给自己脚本的") {
        const auto v = util::expand_command("x {mine}", {{"out", "o"}});
        REQUIRE(v.size() == 2);
        CHECK(v[1] == "{mine}");
    }
    SUBCASE("程序不存在：说「起不来」，不是「退出码」") {
        const auto r = util::run_command(
            {"changji_这个程序不存在_zzz", "--x"}, 5.0);
        CHECK_FALSE(r.ok);
        CHECK(has(r.error, "起不来"));
    }
}

// ---------------------------------------------------------------------------
// 后期链
// ---------------------------------------------------------------------------

TEST_CASE("后期链：off 一个滤镜都不加，film 是柔化 → 调色 → 颗粒") {
    config::LookConfig look;
    SUBCASE("off") {
        look.preset = "off";
        CHECK(media::look_filters(look, 1080, 1920, "/proj").empty());
    }
    SUBCASE("film 默认") {
        const std::string vf = media::look_filters(look, 1080, 1920, "/proj");
        CAPTURE(vf);
        // 顺序：柔化在前、颗粒最后（颗粒加在柔化前会被抹掉）
        const auto blur = vf.find("gblur=sigma=0.4");
        const auto split = vf.find("split[o][g];[g]curves=");
        const auto blend = vf.find("[g2];[g2][o]blend=all_mode=normal:all_opacity=0.6");
        const auto noise = vf.find("noise=c0s=10:c0f=t+u");
        REQUIRE(blur != std::string::npos);
        REQUIRE(split != std::string::npos);
        REQUIRE(blend != std::string::npos);
        REQUIRE(noise != std::string::npos);
        CHECK(blur < split);
        CHECK(split < blend);
        CHECK(blend < noise);
        // 竖屏不遮幅，哪怕填了
        look.letterbox = 2.39;
        CHECK_FALSE(has(media::look_filters(look, 1080, 1920, "/proj"), "crop="));
    }
    SUBCASE("clean：只柔化和颗粒") {
        look.preset = "clean";
        const std::string vf = media::look_filters(look, 1080, 1920, "/proj");
        CHECK(vf == "gblur=sigma=0.4,noise=c0s=10:c0f=t+u");
    }
    SUBCASE("横屏遮幅：裁到比例再补回原高，尺寸都是偶数") {
        look.letterbox = 2.39;
        look.soften = 0;
        look.grain = 0;
        look.preset = "clean";
        const std::string vf = media::look_filters(look, 1920, 1080, "/proj");
        CHECK(vf == "crop=1920:802:0:139,pad=1920:1080:0:139");
    }
    SUBCASE("有 LUT 就用 LUT，相对路径接项目根，冒号要转义") {
        look.lut = "luts/k.cube";
        look.soften = 0;
        look.grain = 0;
        const std::string vf = media::look_filters(look, 1080, 1920, "/proj");
        CHECK(vf == "split[o][g];[g]lut3d=file='/proj/luts/k.cube':interp=tetrahedral[g2];"
                    "[g2][o]blend=all_mode=normal:all_opacity=0.6");
    }
    SUBCASE("强度 1 不用分两路混") {
        look.lut_strength = 1.0;
        look.soften = 0;
        look.grain = 0;
        const std::string vf = media::look_filters(look, 1080, 1920, "/proj");
        CHECK_FALSE(has(vf, "blend"));
        CHECK(has(vf, "curves="));
    }
}

TEST_CASE("后期链的配置：只认三档，数值有范围") {
    config::LookConfig look;
    CHECK(look.validate().empty());
    look.preset = "vintage";
    CHECK_FALSE(look.validate().empty());
    look.preset = "film";
    look.soften = 3.0;
    CHECK_FALSE(look.validate().empty());
    look.soften = 0.4;
    look.letterbox = 1.0;
    CHECK_FALSE(look.validate().empty());
    look.letterbox = 2.39;
    CHECK(look.validate().empty());
}

TEST_CASE("统一规格：默认选项和以前逐字节一样，开了才变") {
    config::AssemblyConfig cfg;
    const auto plain = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4");
    const auto same = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4",
                                            media::NormalizeOptions{});
    CHECK(plain == same);

    SUBCASE("后期链接在缩放补边后面") {
        media::NormalizeOptions opt;
        opt.extra_vf = "gblur=sigma=0.4";
        opt.tune_grain = true;
        const auto a = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4", opt);
        const std::string vf = arg_value(a, "-vf");
        CHECK(has(vf, "setsar=1,fps=24,gblur=sigma=0.4"));
        CHECK(arg_value(a, "-tune") == "grain");
        CHECK(has_flag(a, "-an"));
    }
    SUBCASE("留声音：源里有就映射过来重编成统一规格") {
        media::NormalizeOptions opt;
        opt.keep_audio = true;
        opt.source_has_audio = true;
        const auto a = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4", opt);
        CHECK_FALSE(has_flag(a, "-an"));
        CHECK_FALSE(has_flag(a, "anullsrc=r=48000:cl=stereo"));
        CHECK(arg_value(a, "-c:a") == "aac");
        const auto it = std::find(a.begin(), a.end(), "-map");
        REQUIRE(it != a.end());
        CHECK(*(it + 1) == "0:v");
        CHECK(*(it + 3) == "0:a");
        CHECK_FALSE(has_flag(a, "-shortest"));
    }
    SUBCASE("留声音：源里没有就铺静音，每镜规格一致 concat 才不丢同步") {
        media::NormalizeOptions opt;
        opt.keep_audio = true;
        opt.source_has_audio = false;
        const auto a = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4", opt);
        CHECK(has_flag(a, "anullsrc=r=48000:cl=stereo"));
        CHECK(has_flag(a, "-shortest"));
        const auto it = std::find(a.begin(), a.end(), "-map");
        REQUIRE(it != a.end());
        CHECK(*(it + 3) == "1:a");
    }
}

// ---------------------------------------------------------------------------
// 混音：环境声、配乐、侧链
// ---------------------------------------------------------------------------

TEST_CASE("混音：不开环境声和配乐时滤镜链一个字不变") {
    config::AssemblyConfig cfg;
    const std::vector<media::AudioSegment> segs = {{"a.wav", 0.0}, {"b.wav", 3.0}};
    const auto old = media::mix_args("v.mp4", segs, cfg, -16.0, -1.5, 10.0, "o.mp4");
    const auto neu = media::mix_args("v.mp4", segs, cfg, -16.0, -1.5, 10.0, "o.mp4",
                                     media::MixOptions{});
    CHECK(old == neu);
    CHECK(has(arg_value(old, "-filter_complex"),
              "[a1][a2]amix=inputs=2:dropout_transition=0:normalize=0,loudnorm=I=-16:TP=-1.5:LRA=11,aresample=48000[amixed]"));
}

TEST_CASE("混音：环境声压在台词下面，台词处侧链再压一道") {
    config::AssemblyConfig cfg;
    const std::vector<media::AudioSegment> segs = {{"a.wav", 0.0}};
    media::MixOptions mo;
    mo.bed = true;
    mo.bed_db = -12.0;
    const auto a = media::mix_args("v.mp4", segs, cfg, -16.0, -1.5, 10.0, "o.mp4", mo);
    const std::string chain = arg_value(a, "-filter_complex");
    CAPTURE(chain);
    CHECK(has(chain, "[0:a]aresample=48000,volume=-12dB[bed]"));
    CHECK(has(chain, "[a1]amix=inputs=1:dropout_transition=0:normalize=0[dlg]"));
    // sidechaincompress 会吃掉侧链那一路，台词要分两份
    CHECK(has(chain, "[dlg]asplit[dlg1][dlg2]"));
    CHECK(has(chain, "[bed][dlg2]sidechaincompress="));
    CHECK(has(chain, "[bgd][dlg1]amix=inputs=2:dropout_transition=0:normalize=0,loudnorm=I=-16"));
    CHECK(has(chain, "[amixed]apad[aout]"));
    // 没有配乐就没有第二路输入
    CHECK(std::count(a.begin(), a.end(), "-i") == 2);

    SUBCASE("不压的话直接混") {
        mo.duck = false;
        const auto b = media::mix_args("v.mp4", segs, cfg, -16.0, -1.5, 10.0, "o.mp4", mo);
        const std::string c2 = arg_value(b, "-filter_complex");
        CHECK_FALSE(has(c2, "sidechaincompress"));
        CHECK(has(c2, "[bed][dlg]amix=inputs=2"));
    }
}

TEST_CASE("混音：配乐排在台词后面，没台词时底子直接归一") {
    config::AssemblyConfig cfg;
    media::MixOptions mo;
    mo.bed = true;
    mo.music = fs::path("m.wav");
    mo.music_db = -20.0;
    SUBCASE("两句台词 + 配乐：配乐是第 3 路输入") {
        const std::vector<media::AudioSegment> segs = {{"a.wav", 0.0}, {"b.wav", 2.0}};
        const auto a = media::mix_args("v.mp4", segs, cfg, -16.0, -1.5, 10.0, "o.mp4", mo);
        const std::string chain = arg_value(a, "-filter_complex");
        CAPTURE(chain);
        CHECK(a[a.size() - 1] == "o.mp4");
        // -i v.mp4 -i a.wav -i b.wav -i m.wav
        CHECK(std::count(a.begin(), a.end(), "-i") == 4);
        CHECK(has(chain, "[3:a]aresample=48000,volume=-20dB[mus]"));
        CHECK(has(chain, "[bed][mus]amix=inputs=2:dropout_transition=0:normalize=0[bg]"));
        CHECK(has(chain, "[bg][dlg2]sidechaincompress="));
    }
    SUBCASE("没台词：底子直接接 loudnorm") {
        const auto a = media::mix_args("v.mp4", {}, cfg, -16.0, -1.5, 10.0, "o.mp4", mo);
        const std::string chain = arg_value(a, "-filter_complex");
        CAPTURE(chain);
        CHECK(has(chain, "[bg]loudnorm=I=-16:TP=-1.5:LRA=11,aresample=48000[amixed]"));
        CHECK_FALSE(has(chain, "sidechaincompress"));
        CHECK_FALSE(has(chain, "amix=inputs=0"));
    }
}

// ---------------------------------------------------------------------------
// 出片那一步：原生音轨封进去
// ---------------------------------------------------------------------------

TEST_CASE("出片编码：有原生音轨就封进去、按画面截齐，没有就明说 -an") {
    config::AssemblyConfig a;
    SUBCASE("带音轨") {
        const auto args = infer::encode_args("x.rgb", 704, 1280, 24, a, "o.mp4",
                                             fs::path("x.wav"), 107.0 / 24.0);
        CHECK_FALSE(has_flag(args, "-an"));
        CHECK(std::count(args.begin(), args.end(), "-i") == 2);
        CHECK(arg_value(args, "-c:a") == "aac");
        CHECK(arg_value(args, "-ar") == "48000");
        CHECK(arg_value(args, "-t") == "4.458");
        CHECK_FALSE(has_flag(args, "-shortest"));
        CHECK(args.back() == "o.mp4");
    }
    SUBCASE("不带") {
        const auto args = infer::encode_args("x.rgb", 704, 1280, 24, a, "o.mp4");
        CHECK(has_flag(args, "-an"));
        CHECK(std::count(args.begin(), args.end(), "-i") == 1);
    }
    SUBCASE("wav 和裸帧同名不同后缀") {
        CHECK(infer::audio_path_for("d/.changji_raw_ab.rgb") ==
              fs::path("d/.changji_raw_ab.wav"));
    }
}

// ---------------------------------------------------------------------------
// 分镜表：焦段、光、串镜
// ---------------------------------------------------------------------------

TEST_CASE("镜头新字段：读写、默认值、长度") {
    json j = {{"shot_id", "ep01_sh001"}, {"scene_id", "s1"}, {"order", 0}};
    models::Shot s = j.get<models::Shot>();
    CHECK(s.lens == models::Lens::AUTO);
    CHECK(s.lighting.empty());
    CHECK_FALSE(s.continuous_with_prev);
    CHECK_FALSE(s.end_frame_path.has_value());

    j["lens"] = "portrait";
    j["lighting"] = "夜，路灯从左上打，硬光";
    j["continuous_with_prev"] = true;
    j["end_frame_path"] = "frames/ep01_sh001_end.png";
    s = j.get<models::Shot>();
    CHECK(s.lens == models::Lens::PORTRAIT);
    CHECK(s.lighting == "夜，路灯从左上打，硬光");
    CHECK(s.continuous_with_prev);
    REQUIRE(s.end_frame_path.has_value());
    const json back = s;
    CHECK(back.at("lens") == "portrait");
    CHECK(back.at("continuous_with_prev") == true);
    CHECK(back.at("end_frame_path") == "frames/ep01_sh001_end.png");

    SUBCASE("光那一句超过 80 字要拦") {
        s.lighting = std::string(81, 'a');
        const auto errs = s.validate();
        REQUIRE_FALSE(errs.empty());
        CHECK(has(errs.front(), "lighting"));
    }
}

TEST_CASE("给大模型的 schema：机位、焦段、光进了 required，焦段不给 auto 选") {
    const json s = json(stages::llm_shot_schema(make_assets()));
    const json& item = s.at("properties").at("shots").at("items");
    const json& req = item.at("required");
    const auto required_has = [&req](const char* key) {
        return std::any_of(req.begin(), req.end(),
                           [key](const json& v) { return v == key; });
    };
    CHECK(required_has("camera_angle"));
    CHECK(required_has("lens"));
    CHECK(required_has("lighting"));

    const json& angle = item.at("properties").at("camera_angle");
    CHECK_FALSE(angle.contains("default"));
    CHECK(angle.at("enum") == s.at("$defs").at("CameraAngle").at("enum"));

    const json& lens = item.at("properties").at("lens");
    CHECK_FALSE(lens.contains("default"));
    for (const auto& v : lens.at("enum")) CHECK(v != "auto");
    CHECK(lens.at("enum").size() == 4);

    const json& light = item.at("properties").at("lighting");
    CHECK(light.at("minLength") == 10);
    CHECK(light.at("maxLength") == 80);

    // 串镜和尾帧是可选的，但得在表里，不然模型永远填不了
    CHECK(item.at("properties").contains("continuous_with_prev"));
    CHECK(item.at("properties").contains("last_frame_prompt"));
    CHECK_FALSE(required_has("continuous_with_prev"));
}

// ---------------------------------------------------------------------------
// 提示词：镜头层的焦段和光、时间码运动、视频负向
// ---------------------------------------------------------------------------

TEST_CASE("镜头层：焦段和光拼进去，没填一个字不加") {
    const stages::PromptComposer composer(make_assets());
    models::Shot plain = make_shot("ep01_sh001", 0);
    const std::string base = composer.compose(plain).positive;

    models::Shot rich = plain;
    rich.lens = models::Lens::PORTRAIT;
    rich.lighting = "夜，路灯从画左上斜射，硬光";
    const std::string got = composer.compose(rich).positive;
    CAPTURE(got);
    CHECK(has(got, "85mm 人像镜头"));
    CHECK(has(got, "夜，路灯从画左上斜射，硬光"));
    // 顺序：景别机位焦段 → 光 → 画面描述
    CHECK(got.find("85mm") < got.find("路灯"));
    CHECK(got.find("路灯") < got.find("雨夜天台"));
    // 没填时和以前逐字节一样
    CHECK_FALSE(has(base, "mm"));

    SUBCASE("尾帧提示词只换画面描述那一层") {
        rich.last_frame_prompt = "推到桌上的信停住";
        const std::string end = composer.compose_end(rich).positive;
        CHECK(has(end, "推到桌上的信停住"));
        CHECK_FALSE(has(end, "雨夜天台"));
        CHECK(has(end, "85mm 人像镜头"));
        // 没填尾帧就和首帧一样
        CHECK(composer.compose_end(plain).positive == base);
    }
    SUBCASE("视频负向词是镜头自己的加全片的视频那份，不带图像那份") {
        rich.negative_prompt = "别有第二个人";
        const auto b = composer.compose(rich);
        CHECK(has(b.negative_video, "别有第二个人"));
        CHECK(has(b.negative_video, "溶解转场"));
        CHECK_FALSE(has(b.negative_video, "多余的手指"));
        CHECK(has(b.negative, "低质量"));
    }
}

TEST_CASE("运动描述按时间码分段：没分段的整镜当一段，角色动作并进第一段") {
    // ⚠️ **这一条 2026-09-17 跟着两处有意的改动翻新过**，两处的理由都写在
    // prompt_compose.cpp 里：
    //
    //   · **运镜词不再前插**。分镜模型已经把运镜写进段里了（提示词里要它写
    //     名字、幅度、速度），再前插枚举词就是「镜头缓慢推近, 缓慢推近」。
    //   · **角色动作并进第一段**，不是接在整串末尾。它是这一镜的起始状态；
    //     接在末尾的话，三段的镜头里所有动作都被读成最后一段的事。
    //
    // 用例原来钉的是这两处改动**之前**的样子，从那以后一直红着——红了不改
    // 会把人训练成"看见红的先跳过"。
    const stages::PromptComposer composer(make_assets());
    models::Shot s = make_shot("ep01_sh001", 0, 5.0);
    CHECK(composer.motion_prompt(s) == "[0-5秒] 雨丝斜掠，转身");

    s.duration_s = 4.5;
    CHECK(composer.motion_prompt(s) == "[0-4.5秒] 雨丝斜掠，转身");

    // **末段跟着这一镜的真时长走。** 这一镜是 4.5 秒而分镜写到 [2-5秒]，
    // 多出来那半秒没人描述，出片模型自由发挥——而它发挥的方式是把主体丢掉
    // （2026-09-16 实测）。夹在用之前，盘上的老分镜表也照样出对的片子。
    s.motion_prompt = "[0-2秒] 她抬头看雨。 [2-5秒] 雨越下越大";
    // 动作并进第一段：末段只剩它自己那句
    CHECK(composer.motion_prompt(s) ==
          "[0-2秒] 她抬头看雨，转身 [2-4.5秒] 雨越下越大");

    SUBCASE("空着的话只剩运镜词，也带时间码") {
        s.motion_prompt.clear();
        s.characters.clear();
        CHECK(composer.motion_prompt(s) == "[0-4.5秒] 镜头缓慢推近");
    }
}

// ---------------------------------------------------------------------------
// 出片：关键镜头多条 take、尾帧串镜、尾帧
// ---------------------------------------------------------------------------

TEST_CASE("关键镜头：第一镜、最后一镜、钩子反转的镜") {
    models::Shot s = make_shot("x", 3);
    CHECK(stages::is_hero_shot(s, true, false));
    CHECK(stages::is_hero_shot(s, false, true));
    CHECK_FALSE(stages::is_hero_shot(s, false, false));
    s.beat = "反转";
    CHECK(stages::is_hero_shot(s, false, false));
    s.beat = "章尾留扣";
    CHECK(stages::is_hero_shot(s, false, false));
    s.beat = "铺垫";
    CHECK_FALSE(stages::is_hero_shot(s, false, false));
}

TEST_CASE("挑 take：先要过闸门，再罚片中硬切，几乎不动的扣分") {
    const auto make = [](bool ok, double motion, bool cut) {
        gates::GateResult r;
        r.verdict = ok ? gates::Verdict::Pass : gates::Verdict::Retry;
        r.metrics["motion_mean"] = motion;
        if (cut) r.metrics["cut_inside"] = 1.0;
        return r;
    };
    // 过闸门的赢没过的，哪怕没过的动得多
    CHECK(stages::pick_take({make(false, 8.0, false), make(true, 3.0, false)}) == 1);
    // 都过：像回事的赢几乎不动的
    CHECK(stages::pick_take({make(true, 0.3, false), make(true, 4.0, false)}) == 1);
    // 都过：有硬切的输
    CHECK(stages::pick_take({make(true, 6.0, true), make(true, 3.0, false)}) == 1);
    // 平手取先出的
    CHECK(stages::pick_take({make(true, 4.0, false), make(true, 4.0, false)}) == 0);
    // 太猛的不如像回事的
    CHECK(stages::pick_take({make(true, 40.0, false), make(true, 5.0, false)}) == 1);
}

TEST_CASE("关键镜头出几条，按闸门的数留一条，别的删掉") {
    const fs::path root = temp_root("take");
    const models::ProjectPaths paths(root);
    paths.ensure();
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001", 0),
                                           make_shot("ep01_sh002", 1),
                                           make_shot("ep01_sh003", 2)};
    std::vector<models::Shot*> shots = {&owned[0], &owned[1], &owned[2]};

    std::vector<std::string> rendered;
    const stages::VideoRenderer render =
        [&rendered](const models::Shot&, const stages::RenderPlan&,
                    const std::optional<fs::path>&, const fs::path& dest,
                    pipeline::CancelToken&, const infer::StepCallback&) {
            rendered.push_back(paths::to_utf8(dest.filename()));
            touch(dest);
        };
    stages::GateHooks gate;
    gate.max_attempts = 3;
    gate.check = [](const models::Shot& s, const fs::path& video,
                    const stages::RenderPlan&) {
        gates::GateResult r;
        r.shot_id = s.shot_id;
        const std::string name = paths::to_utf8(video.filename());
        // 第一条几乎不动，第二条乱动，第三条才像回事——前两条都不干净，
        // 所以三条都要出（take_good_enough 只在干净的那一条上停）
        if (has(name, "_take1")) r.metrics["motion_mean"] = 0.3;
        else if (has(name, "_take2")) r.metrics["motion_mean"] = 40.0;
        else if (has(name, "_take3")) r.metrics["motion_mean"] = 6.0;
        else r.metrics["motion_mean"] = 2.0;
        return r;
    };
    gate.decide = [](const gates::GateResult&, const models::Shot&) {
        return gates::Verdict::Retry;
    };
    stages::RenderExtras extras;
    extras.hero_takes = 3;

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::RenderOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::render_batch(shots, make_assets(), make_spec(), paths,
                                    render, p, tok, 24, 1, gate, {}, extras);
    });
    table.wait_idle();

    REQUIRE(outs.size() == 3);
    // 第一镜和最后一镜各 3 条，中间那镜 1 条
    CHECK(std::count_if(rendered.begin(), rendered.end(),
                        [](const std::string& n) { return has(n, "sh001_take"); }) == 3);
    CHECK(std::count_if(rendered.begin(), rendered.end(),
                        [](const std::string& n) { return has(n, "sh002"); }) == 1);
    CHECK(std::count_if(rendered.begin(), rendered.end(),
                        [](const std::string& n) { return has(n, "sh003_take"); }) == 3);
    for (const auto& o : outs) CHECK(o.ok);
    CHECK(owned[0].status == models::ShotStatus::DRAFT_DONE);
    // 留下的那条在正式的路径上，take 文件全清掉了
    REQUIRE(owned[0].video_path.has_value());
    CHECK(fs::is_regular_file(paths.abs(*owned[0].video_path)));
    CHECK_FALSE(fs::exists(paths.shots("draft") / "ep01_sh001_take1.mp4"));
    CHECK_FALSE(fs::exists(paths.shots("draft") / "ep01_sh001_take2.mp4"));
    CHECK_FALSE(fs::exists(paths.shots("draft") / "ep01_sh001_take3.mp4"));

    SUBCASE("第一条就干净：不再多出，只有一条") {
        // 2026-09-16 用户问"为什么要进行两次"：无条件出两条是整章翻倍
        rendered.clear();
        gate.check = [](const models::Shot& s, const fs::path&,
                        const stages::RenderPlan&) {
            gates::GateResult r;
            r.shot_id = s.shot_id;
            r.metrics["motion_mean"] = 4.0;   // 像回事
            return r;
        };
        auto one = std::vector<models::Shot>{make_shot("ep01_sh011", 0)};
        std::vector<models::Shot*> ptrs = {&one[0]};
        pipeline::JobTable t3;
        pipeline::CancelToken tok3;
        t3.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
            stages::render_batch(ptrs, make_assets(), make_spec(), paths, render,
                                 p, tok3, 24, 1, gate, {}, extras);
        });
        t3.wait_idle();
        CHECK(rendered.size() == 1);
        REQUIRE(one[0].video_path.has_value());
        CHECK(fs::is_regular_file(paths.abs(*one[0].video_path)));
    }

    SUBCASE("没有闸门就没有依据挑，出一条") {
        rendered.clear();
        auto one = std::vector<models::Shot>{make_shot("ep01_sh009", 0)};
        std::vector<models::Shot*> ptrs = {&one[0]};
        pipeline::JobTable t2;
        pipeline::CancelToken tok2;
        t2.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
            stages::render_batch(ptrs, make_assets(), make_spec(), paths, render,
                                 p, tok2, 24, 1, {}, {}, extras);
        });
        t2.wait_idle();
        CHECK(rendered.size() == 1);
    }
}

TEST_CASE("尾帧串镜：标了紧接上一镜的，拿上一镜的最后一帧当首帧") {
    const fs::path root = temp_root("chain");
    const models::ProjectPaths paths(root);
    paths.ensure();
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001", 0),
                                           make_shot("ep01_sh002", 1)};
    owned[1].continuous_with_prev = true;
    // 两镜都有自己的首帧
    for (auto& s : owned) {
        const fs::path f = paths.frames() / paths::from_utf8(s.shot_id + ".png");
        touch(f);
        s.frame_path = paths.rel(f);
    }
    std::vector<models::Shot*> shots = {&owned[0], &owned[1]};

    std::vector<std::optional<fs::path>> starts;
    std::vector<std::optional<fs::path>> ends;
    const stages::VideoRenderer render =
        [&](const models::Shot&, const stages::RenderPlan& plan,
            const std::optional<fs::path>& start, const fs::path& dest,
            pipeline::CancelToken&, const infer::StepCallback&) {
            starts.push_back(start);
            ends.push_back(plan.end_image);
            touch(dest);
        };
    std::vector<std::string> extracted_from;
    stages::RenderExtras extras;
    extras.chain_frames = true;
    extras.last_frame = [&](const fs::path& video, const fs::path& dest) {
        extracted_from.push_back(paths::to_utf8(video.filename()));
        touch(dest);
        return true;
    };

    // 第一镜还有一张尾帧
    const fs::path end_png = paths.frames() / "ep01_sh001_end.png";
    touch(end_png);
    owned[0].end_frame_path = paths.rel(end_png);

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::render_batch(shots, make_assets(), make_spec(), paths, render, p,
                             tok, 24, 1, {}, {}, extras);
    });
    table.wait_idle();

    REQUIRE(starts.size() == 2);
    REQUIRE(ends.size() == 2);
    // 第一镜用自己的首帧，带尾帧
    REQUIRE(starts[0].has_value());
    CHECK(starts[0]->filename() == fs::path("ep01_sh001.png"));
    REQUIRE(ends[0].has_value());
    CHECK(ends[0]->filename() == fs::path("ep01_sh001_end.png"));
    // 第二镜接上一镜的最后一帧，抽的是上一镜刚出的视频
    REQUIRE(starts[1].has_value());
    CHECK(starts[1]->filename() == fs::path("ep01_sh002_chain.png"));
    REQUIRE(extracted_from.size() == 1);
    CHECK(extracted_from[0] == "ep01_sh001.mp4");
    CHECK_FALSE(ends[1].has_value());

    SUBCASE("关掉就用自己的首帧") {
        starts.clear();
        extracted_from.clear();
        extras.chain_frames = false;
        pipeline::JobTable t2;
        pipeline::CancelToken tok2;
        t2.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
            stages::render_batch(shots, make_assets(), make_spec(), paths, render,
                                 p, tok2, 24, 1, {}, {}, extras);
        });
        t2.wait_idle();
        REQUIRE(starts.size() == 2);
        CHECK(starts[1]->filename() == fs::path("ep01_sh002.png"));
        CHECK(extracted_from.empty());
    }
}

// ---------------------------------------------------------------------------
// 配乐
// ---------------------------------------------------------------------------

TEST_CASE("配乐：文件放 output 下，描述是器乐无人声，没配命令就说清楚") {
    const fs::path root = temp_root("music");
    const models::ProjectPaths paths(root);
    paths.ensure();
    CHECK(stages::music_path_for(paths, "ep01").filename() ==
          fs::path("ep01_music.wav"));

    models::Episode ep;
    ep.episode_id = "ep01";
    ep.title = "第一集";
    ep.synopsis = "林晚在雨夜天台发现了那封信。";
    config::SoundConfig sound;
    const std::string p = stages::music_prompt(ep, sound, 62.4);
    CAPTURE(p);
    CHECK(has(p, "no vocals"));
    CHECK(has(p, "about 62 seconds"));
    CHECK(has(p, "林晚在雨夜天台"));
    sound.music_style = "slow piano, rain";
    CHECK(has(stages::music_prompt(ep, sound, 60), "slow piano, rain"));

    config::Settings settings;
    SUBCASE("没配命令") {
        const auto r = stages::ensure_music(settings, paths, ep, 60);
        CHECK_FALSE(r.ok);
        CHECK(has(r.error, "music_command"));
    }
    SUBCASE("文件已经在就沿用，命令不跑") {
        touch(stages::music_path_for(paths, "ep01"), "RIFF");
        settings.sound.music_command = "changji_不存在的程序 {out}";
        const auto r = stages::ensure_music(settings, paths, ep, 60);
        CHECK(r.ok);
        CHECK(r.reused);
    }
    SUBCASE("命令起不来：说原因，不抛") {
        settings.sound.music_command = "changji_不存在的程序_zzz --out {out}";
        const auto r = stages::ensure_music(settings, paths, ep, 60);
        CHECK_FALSE(r.ok);
        CHECK(has(r.error, "起不来"));
    }
}

// ---------------------------------------------------------------------------
// 配置：三节都读得到、模板里默认是开的
// ---------------------------------------------------------------------------

TEST_CASE("[look] [sound] [upscale] 从 toml 读出来，非法值拦住") {
    const fs::path dir = temp_root("toml");
    {
        std::ofstream f(dir / "changji.toml", std::ios::binary);
        f << "[look]\npreset = \"clean\"\ngrain = 6\nsoften = 0.3\nletterbox = 2.39\n"
             "[sound]\nambient = false\nmusic_db = -24\nmusic_style = \"slow piano\"\n"
             "music_command = \"python gen.py {prompt} {seconds} {out}\"\n"
             "[upscale]\ncommand = \"up.sh {in} {out}\"\nscale = 2\n"
             "[video]\nhero_takes = 3\nchain_frames = false\n";
    }
    const auto s = config::load_settings(dir);
    CHECK(s.look.preset == "clean");
    CHECK(s.look.grain == doctest::Approx(6.0));
    CHECK(s.look.soften == doctest::Approx(0.3));
    CHECK(s.look.letterbox == doctest::Approx(2.39));
    CHECK_FALSE(s.sound.ambient);
    CHECK(s.sound.music_db == doctest::Approx(-24.0));
    CHECK(s.sound.music_style == "slow piano");
    CHECK(has(s.sound.music_command, "{out}"));
    CHECK(s.upscale.enabled());
    CHECK(s.upscale.scale == 2);
    CHECK(s.video.hero_takes == 3);
    CHECK_FALSE(s.video.chain_frames);

    SUBCASE("默认值：胶片开着、环境声和配乐开着、不放大、关键镜两条") {
        const config::Settings def;
        CHECK(def.look.preset == "film");
        CHECK(def.look.enabled());
        CHECK(def.sound.ambient);
        CHECK(def.sound.music);
        CHECK_FALSE(def.upscale.enabled());
        CHECK(def.video.hero_takes == 2);
        CHECK(def.video.chain_frames);
    }
    SUBCASE("项目模板里 [look] 和 [sound] 都在，而且解析出来就是默认值") {
        const fs::path d2 = temp_root("template");
        {
            std::ofstream f(d2 / "changji.toml", std::ios::binary);
            f << config::project_config_template();
        }
        const auto t = config::load_settings(d2);
        const config::Settings def;
        CHECK(t.look.preset == def.look.preset);
        CHECK(t.look.grain == doctest::Approx(def.look.grain));
        CHECK(t.sound.ambient == def.sound.ambient);
        CHECK(t.sound.music == def.sound.music);
        CHECK(t.video.hero_takes == def.video.hero_takes);
    }
    SUBCASE("正数的 dB 是把环境声顶到台词上面，拦") {
        config::SoundConfig sc;
        sc.ambient_db = 3.0;
        CHECK_FALSE(sc.validate().empty());
        config::UpscaleConfig up;
        up.scale = 9;
        CHECK_FALSE(up.validate().empty());
        config::VideoConfig v;
        v.hero_takes = 0;
        CHECK_FALSE(v.validate().empty());
    }
}

TEST_CASE("查这台的 ffmpeg 有没有某个滤镜") {
    // 2026-09-16 实撞：brew 出来的 ffmpeg 9.0.1 没编 libass，没有 subtitles
    // 滤镜。原来这条让整章装配失败，十七镜全渲完而输出目录是空的——
    // 为了一条可选的烧录把整章扔了。而 ffmpeg 报的还是一句误导的
    // 「No option name near '/Users/…'」，人会去查路径里的空格。
    //
    // **认不得的滤镜 ffmpeg 退出码是 0**，所以判据必须看输出。
    const auto ff_with = [](const std::string& out, int code = 0) {
        return media::FFmpeg("ffmpeg", "ffprobe",
                             [out, code](const std::string&,
                                         const std::vector<std::string>&, double) {
                                 media::ProcResult r;
                                 r.launched = true;
                                 r.exit_code = code;
                                 r.out = out;
                                 return r;
                             });
    };
    CHECK(ff_with("Filter subtitles\n  Render text subtitles onto input video "
                  "using the libass library.\n")
              .has_filter("subtitles"));
    // 退出码 0 但说不认得：没有
    CHECK_FALSE(ff_with("Unknown filter 'subtitles'.\n").has_filter("subtitles"));
    // 起不来也算没有——退回外挂字幕，片子照出，别把整章扔了
    CHECK_FALSE(media::FFmpeg("ffmpeg", "ffprobe",
                              [](const std::string&,
                                 const std::vector<std::string>&, double) {
                                  return media::ProcResult{};   // launched=false
                              })
                    .has_filter("subtitles"));
}

TEST_CASE("出片那条也要把参考图还原成绝对路径") {
    // 和 frames.cpp 里同名的那一段是同一件事：2026-09-13 在首帧那条修过，
    // 出片这条漏了。compose 交出来的是相对项目根的路径（refs/xxx.png），
    // 跨机派活时 ship_input 拿它去读文件，读的是工作目录——报
    // 「读不了输入文件：refs/c_zeng_laoban_front.png」。
    //
    // **一直没露出来，是因为上一版分镜全是 ECU**，而大特写整档不带参考图。
    // 景别修好、镜头重新带上参考图的当天（2026-09-16），这条就断了。
    const fs::path root = temp_root("refabs");
    const models::ProjectPaths paths(root);
    paths.ensure();
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001", 0)};
    // 让这一镜带上一个在场角色，compose 才会给参考图
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    owned[0].characters.push_back(in_shot);
    owned[0].shot_size = models::ShotSize::MS;   // ECU 整档不带参考图
    std::vector<models::Shot*> shots = {&owned[0]};

    const models::AssetLibrary assets = make_assets();
    // 资产库里那张参考图落到盘上，compose 才认
    for (const auto& kv : assets.characters) {
        const auto& c = kv.second;
        for (const std::optional<std::string>* rel :
             {&c.ref_front, &c.ref_three_quarter, &c.ref_back}) {
            if (!rel->has_value() || rel->value().empty()) continue;
            const fs::path p = paths.abs(rel->value());
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            touch(p);
        }
    }

    std::vector<std::string> seen;
    const stages::VideoRenderer render =
        [&seen](const models::Shot&, const stages::RenderPlan& plan,
                const std::optional<fs::path>&, const fs::path& dest,
                pipeline::CancelToken&, const infer::StepCallback&) {
            for (const auto& r : plan.prompts.reference_images) seen.push_back(r);
            touch(dest);
        };

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::render_batch(shots, assets, make_spec(), paths, render, p, tok,
                             24, 1, {}, {}, {});
    });
    table.wait_idle();

    for (const std::string& r : seen) {
        CAPTURE(r);
        // 绝对路径，而且文件真的在——相对路径跨机必然读不到
        CHECK(fs::path(paths::from_utf8(r)).is_absolute());
        CHECK(fs::is_regular_file(paths::from_utf8(r)));
    }
}

TEST_CASE("时段对不上就别喂那张空景图") {
    // 2026-09-16 实测 ep04_sh001：场景资产写着「白天，散射光从窗户来」、
    // 空景图是大白天的，而这一镜的光是「夜晚」——Edit 模型拿参考图压过文字，
    // 首帧出来是大白天；出片模型再拿这张白天首帧配「夜晚」的提示词，两秒里
    // 把画面从白天拉成了夜晚。
    using stages::lighting_clashes;
    CHECK(lighting_clashes("白天，散射光从窗户来，冷色调，软光",
                           "夜晚，光从窗户来，朝内打，硬光"));
    CHECK(lighting_clashes("夜晚，酒吧灯光昏暗", "下午，自然光"));
    // 同一个时段不算冲突
    CHECK_FALSE(lighting_clashes("白天，自然光", "上午，侧光，软"));
    CHECK_FALSE(lighting_clashes("夜晚，霓虹", "深夜，路灯，硬光"));
    // **说不准的一律照喂**：宁可喂一张可能不对的参考图，也不要因为一句
    // 没写时段的光把场景的样子整个丢掉
    CHECK_FALSE(lighting_clashes("", "夜晚，硬光"));
    CHECK_FALSE(lighting_clashes("白天，自然光", ""));
    CHECK_FALSE(lighting_clashes("侧逆光，硬", "顶光，软"));
}

TEST_CASE("每一场至少留一个大景：分布看着匀、却一个远景都没有") {
    // 2026-09-16 实测 ep07：MS 5 / CU 4 / MCU 8，十七镜没有一个 LS 或
    // MLS。八成塌成一种那道线根本够不着，而用户报的正是这个：
    // 「都是近景没有远景」。
    using models::Shot;
    using models::ShotSize;
    const auto make = [](const char* scene, ShotSize sz, bool people) {
        Shot s;
        s.scene_id = scene;
        s.shot_size = sz;
        if (people) {
            models::CharacterInShot who;
            who.char_id = "c_a";
            s.characters.push_back(who);
        }
        return s;
    };
    const auto widest = [](const std::vector<Shot>& v, const char* scene) {
        int n = 0;
        for (const Shot& s : v) {
            if (s.scene_id != scene) continue;
            if (s.shot_size == ShotSize::LS || s.shot_size == ShotSize::MLS ||
                s.shot_size == ShotSize::ELS) {
                ++n;
            }
        }
        return n;
    };

    SUBCASE("一个大景都没有：补一个，而且补在第一个有人的镜头上") {
        std::vector<Shot> v = {make("s1", ShotSize::CU, false),
                               make("s1", ShotSize::MCU, true),
                               make("s1", ShotSize::MS, true),
                               make("s1", ShotSize::MCU, true)};
        stages::ensure_establishing_shots(v);
        CHECK(widest(v, "s1") == 1);
        CHECK(v[1].shot_size == ShotSize::LS);   // 第一个有人的那一镜
        CHECK(v[0].shot_size == ShotSize::CU);   // 空镜不动
    }
    SUBCASE("整场没人：第一镜改 MLS，不是 LS") {
        std::vector<Shot> v = {make("s1", ShotSize::CU, false),
                               make("s1", ShotSize::MCU, false),
                               make("s1", ShotSize::CU, false),
                               make("s1", ShotSize::MCU, false)};
        stages::ensure_establishing_shots(v);
        CHECK(v[0].shot_size == ShotSize::MLS);
    }
    SUBCASE("一场一个是地板不是节奏：十七镜一场只补一个还是 5%，要按镜数再垫") {
        // 2026-09-16 实测 ep09：一场、十七镜，模型只排了一个 LS。前一道
        // 「一场一个」看它已经有了，一镜不动——5% 的大景，用户那句
        // 「都是近景没有远景」照旧成立。每六镜至少一个：17 镜要 3 个。
        std::vector<Shot> v;
        for (int i = 0; i < 17; ++i) v.push_back(make("s1", ShotSize::MCU, true));
        v[0].shot_size = ShotSize::LS;   // 模型自己排的那一个
        v[5].characters.clear();          // 一个空镜
        v[9].dialogue.clear();            // （默认就没台词，这里显式一下）
        // 给别的镜头都塞一句台词，好看出它挑的是没台词的
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i == 5 || i == 9) continue;
            models::DialogueLine line;
            line.text = "一句话";
            v[i].dialogue.push_back(line);
        }
        stages::ensure_establishing_shots(v);
        CHECK(widest(v, "s1") == 3);
        CHECK(v[0].shot_size == ShotSize::LS);    // 原来那个不动
        CHECK(v[5].shot_size == ShotSize::MLS);   // 第一档：空镜
        CHECK(v[9].shot_size == ShotSize::MLS);   // 第二档：没台词的
        // 有台词的一个都没被动
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i == 0 || i == 5 || i == 9) continue;
            CHECK(v[i].shot_size == ShotSize::MCU);
        }
    }
    SUBCASE("空镜和没台词的都不够时，才动每场的收尾镜") {
        std::vector<Shot> v;
        for (int i = 0; i < 12; ++i) {
            v.push_back(make("s1", ShotSize::MCU, true));
            models::DialogueLine line;
            line.text = "一句话";
            v.back().dialogue.push_back(line);
        }
        stages::ensure_establishing_shots(v);
        // 12 镜要 2 个：第一道给第一镜一个 LS，第二道找不到空镜和没台词的，
        // 只好动这一场的最后一镜
        CHECK(widest(v, "s1") == 2);
        CHECK(v[0].shot_size == ShotSize::LS);
        CHECK(v[11].shot_size == ShotSize::MLS);
        for (std::size_t i = 1; i < 11; ++i) CHECK(v[i].shot_size == ShotSize::MCU);
    }
    SUBCASE("已经够了的一镜不动：ep08 那种 17 镜 4 个大景") {
        std::vector<Shot> v;
        for (int i = 0; i < 17; ++i) v.push_back(make("s1", ShotSize::MCU, true));
        v[0].shot_size = ShotSize::LS;
        v[4].shot_size = ShotSize::MLS;
        v[8].shot_size = ShotSize::LS;
        v[12].shot_size = ShotSize::MLS;
        v[2].characters.clear();   // 有个空镜，但不该被动
        const std::vector<Shot> before = v;
        stages::ensure_establishing_shots(v);
        for (std::size_t i = 0; i < v.size(); ++i) {
            CHECK(v[i].shot_size == before[i].shot_size);
        }
    }
    SUBCASE("整表不足四镜：一个字不碰") {
        std::vector<Shot> v = {make("s1", ShotSize::CU, true),
                               make("s1", ShotSize::MCU, true),
                               make("s1", ShotSize::CU, true)};
        stages::ensure_establishing_shots(v);
        CHECK(v[0].shot_size == ShotSize::CU);
        CHECK(v[1].shot_size == ShotSize::MCU);
    }
    SUBCASE("已经有大景的场：一个字不碰") {
        std::vector<Shot> v = {make("s1", ShotSize::MLS, true),
                               make("s1", ShotSize::MCU, true),
                               make("s1", ShotSize::CU, true)};
        const std::vector<Shot> before = v;
        stages::ensure_establishing_shots(v);
        for (std::size_t i = 0; i < v.size(); ++i) {
            CHECK(v[i].shot_size == before[i].shot_size);
        }
    }
    SUBCASE("按场分别看：一场缺、一场不缺，只补缺的那一场") {
        std::vector<Shot> v = {make("s1", ShotSize::LS, true),
                               make("s1", ShotSize::MCU, true),
                               make("s2", ShotSize::MCU, true),
                               make("s2", ShotSize::CU, true),
                               make("s2", ShotSize::MS, true)};
        stages::ensure_establishing_shots(v);
        CHECK(v[0].shot_size == ShotSize::LS);
        CHECK(v[1].shot_size == ShotSize::MCU);   // s1 没动
        CHECK(widest(v, "s2") == 1);
        CHECK(v[2].shot_size == ShotSize::LS);
    }
    SUBCASE("一镜的场不折腾") {
        std::vector<Shot> v = {make("s1", ShotSize::MCU, true),
                               make("s2", ShotSize::CU, true),
                               make("s3", ShotSize::MCU, true)};
        stages::ensure_establishing_shots(v);
        CHECK(v[0].shot_size == ShotSize::MCU);
        CHECK(v[1].shot_size == ShotSize::CU);
    }
}

TEST_CASE("章模式：按场给拍数地板，没有秒数，也没有四段") {
    // 用户 2026-09-16 的判词：剧本从一章里直接拿，写多长由内容定。正文是按
    // 场写的，剧本照着场走；之前在场之上又套了一副按秒排的四段骨架，段头还
    // 印着「按秒排」。地板按每场的篇幅：六十字一拍起，二十字一拍封顶。
    SUBCASE("篇幅长的场拍数多，短的少，再短也有四拍") {
        const auto plan = stages::scene_plan_for_chapter({{"天台", 300}, {"咖啡馆", 3000}, {"", 0}});
        REQUIRE(plan.size() == 3);
        CHECK(plan[0].key == "s1");
        CHECK(plan[1].key == "s2");
        CHECK(plan[1].min_beats > plan[0].min_beats);
        CHECK(plan[1].min_beats >= 50);
        CHECK(plan[2].min_beats == 4);
        for (const auto& p : plan) CHECK(p.max_beats > p.min_beats);
    }
    SUBCASE("没有场：整章当一场") {
        const auto plan = stages::scene_plan_for_chapter({});
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].key == "s1");
    }
    SUBCASE("schema：scenes 里一场一个字段，拍子带四个新栏，都在 required 里") {
        const auto plan = stages::scene_plan_for_chapter({{"天台", 600}, {"咖啡馆", 900}});
        const auto sc = stages::script_schema_for_chapter(plan, {"林晚", "陈默"});
        const auto& scenes = sc.at("properties").at("scenes");
        CHECK(scenes.at("required") == nlohmann::ordered_json::array({"s1", "s2"}));
        for (const auto& p : plan) {
            const auto& beats = scenes.at("properties").at(p.key).at("properties").at("beats");
            CHECK(beats.at("minItems").get<int>() == p.min_beats);
            CHECK(beats.at("maxItems").get<int>() == p.max_beats);
            // **拍子的定义只有一份，各场 $ref 它**（2026-09-17）：原来每场
            // 各塞一份完整的，一章三场就把那一千三百字抄三遍贴给模型。
            CHECK(beats.at("items").at("$ref") == "#/$defs/Beat");
            const auto& item = sc.at("$defs").at("Beat");
            for (const char* f : {"kind", "speaker", "text", "characters", "delivery", "subtext", "fx"}) {
                bool found = false;
                for (const auto& r : item.at("required")) found = found || r == f;
                CAPTURE(f);
                CHECK(found);
            }
            CHECK(item.at("properties").at("characters").at("items").at("enum") ==
                  nlohmann::ordered_json::array({"林晚", "陈默"}));
        }
        // 没有四段的键
        CHECK_FALSE(sc.at("properties").contains("opening"));
        const std::string dumped = sc.dump();
        CHECK(dumped.find("秒") == std::string::npos);
    }
    SUBCASE("解析：各场顺次拼平，漏了场次头的补一个；四栏读进来；渲染带括号") {
        const auto plan = stages::scene_plan_for_chapter({{"天台", 60}, {"咖啡馆", 60}});
        const nlohmann::json raw = {
            {"title", "雨夜"}, {"logline", "x"},
            {"scenes", {
                {"s1", {{"beats", nlohmann::json::array({
                    {{"kind", "scene"}, {"speaker", ""}, {"text", "夜 · 外 · 天台"},
                     {"characters", nlohmann::json::array()}, {"delivery", ""}, {"subtext", ""}, {"fx", ""}},
                    {{"kind", "action"}, {"speaker", ""}, {"text", "林晚站在边缘。"},
                     {"characters", nlohmann::json::array({"林晚"})}, {"delivery", ""},
                     {"subtext", "她在等他先开口"}, {"fx", "雨"}},
                    {{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", "你来了。"},
                     {"characters", nlohmann::json::array({"林晚"})}, {"delivery", "压着嗓子"},
                     {"subtext", ""}, {"fx", ""}}})}}},
                {"s2", {{"beats", nlohmann::json::array({
                    {{"kind", "action"}, {"speaker", ""}, {"text", "陈默推开杯子。"},
                     {"characters", nlohmann::json::array({"陈默"})}, {"delivery", ""}, {"subtext", ""}, {"fx", ""}},
                    {{"kind", "dialogue"}, {"speaker", "陈默"}, {"text", "我不该来。"},
                     {"characters", nlohmann::json::array({"陈默"})}, {"delivery", ""}, {"subtext", ""}, {"fx", ""}}})}}}}}};
        const auto d = stages::parse_chapter_script(raw.dump(), plan);
        REQUIRE(d.beats.size() == 6);   // 第二场补了一个空场次头
        CHECK(d.acts.empty());
        CHECK(d.beats[0].kind == "scene");
        CHECK(d.beats[1].characters == std::vector<std::string>{"林晚"});
        CHECK(d.beats[1].subtext == "她在等他先开口");
        CHECK(d.beats[1].fx == "雨");
        CHECK(d.beats[2].delivery == "压着嗓子");
        CHECK(d.beats[3].kind == "scene");
        CHECK(d.beats[3].text.empty());
        const std::string r = d.render();
        CHECK(r.find("【第1场 · 夜 · 外 · 天台】") != std::string::npos);
        CHECK(r.find("林晚站在边缘。〔她在等他先开口〕〔特效：雨〕") != std::string::npos);
        CHECK(r.find("林晚（压着嗓子）：你来了。") != std::string::npos);
        CHECK(r.find("【第2场】") != std::string::npos);
        // 没有段头
        CHECK(r.find("秒】") == std::string::npos);
    }
    SUBCASE("老路（按秒）一个字没变：60 秒还是原来那组地板") {
        const auto old = stages::act_plan(60.0, 0);
        REQUIRE(old.size() == 4);
        CHECK(old[0].to_s > old[0].from_s);   // 段头带秒
    }
}

TEST_CASE("情绪标签词表：提示词那份和守卫那份逐项一致") {
    // 2026-09-16 之前两边各抄一份（11 个对 14 个），模型会因为没被告知的词
    // 被打回重写。现在守卫按 prompts.toml 的数组，提示词按顿号串，钉住一致。
    std::string joined;
    for (const char* w : stages::prompt::chapter_write::kEmotionLabels) {
        if (!joined.empty()) joined += "、";
        joined += w;
    }
    // [shared] 不生成命名空间，那串顿号文字展开在正文规则里：查它在不在。
    CHECK(std::string(stages::prompt::chapter_write::kRules).find(joined) !=
          std::string::npos);
}

TEST_CASE("开门关门：中间插一个副词也要认出来") {
    // 用户 2026-09-16 报的「开门关门都有 bug」。kRisky 里列着「开门」
    // 「门打开」「推开门」，而模型写的是「门缓慢打开」——中间插一个副词，
    // 三条全不匹配。那一版 ep06 的第 1 镜和第 7 镜都是「门缓慢打开」。
    const auto gone = [](const std::string& s) {
        const std::string out = stages::defuse_motion(s);
        for (const char* w : {"打开", "推开", "拉开", "关上", "关闭", "合上",
                              "敞开", "开启"}) {
            if (out.find(w) != std::string::npos) return false;
        }
        return true;
    };
    CHECK(gone("[0-2秒] 门缓慢打开，门铃叮当响，酒吧内部光线渐亮"));
    CHECK(gone("[0-2秒] 门缓缓打开，光线从门缝射入，纸张味弥漫"));
    CHECK(gone("[0-3秒] 他伸手，门被轻轻推开，灰尘扬起"));
    CHECK(gone("[0-3秒] 窗户忽然打开，风灌进来"));
    CHECK(gone("[0-3秒] 他把抽屉慢慢拉开，里面是一沓照片"));
    CHECK(gone("[0-3秒] 门在身后缓缓关上，屋里暗下来"));

    // 摘掉的是那一句，别的照旧留着，时间码也留着
    const std::string got =
        stages::defuse_motion("[0-2秒] 门缓慢打开，门铃叮当响，酒吧内部光线渐亮");
    CHECK(got.find("[0-2秒]") != std::string::npos);
    CHECK(got.find("门铃叮当响") != std::string::npos);
    CHECK(got.find("光线渐亮") != std::string::npos);

    // **别错杀。** 门铃、门口、门缝这些只是提了一句，没有人去开它。
    for (const char* ok : {"[0-2秒] 门铃叮当响后消失，光线渐暗",
                           "[0-2秒] 他站在门口，手里攥着照片",
                           "[0-2秒] 光线从门缝射入，灰尘浮动",
                           "[0-3秒] 他打开卷宗，纸张哗啦响"}) {
        CHECK(stages::defuse_motion(ok) == std::string(ok));
    }
}

TEST_CASE("镜头改短时，演不完的那几拍整段摘掉，不许把时间轴写反") {
    // 2026-09-16 实测：ep05_sh007 计划 15 秒、被锁成 3 秒，运动描述变成
    // `[0-5秒] … [5-10秒] … [10-3秒] …`——最后那一段的止点被往回拉到 3，
    // 时间轴倒着走，就这么进了出片提示词。
    const std::string three =
        "[0-5秒] 爬进灌木丛 [5-10秒] 手电扫过 [10-15秒] 跑向仓库";

    const std::string cut = stages::motion_covering(three, 3.0);
    CHECK(cut.find("[10-3秒]") == std::string::npos);
    CHECK(cut.find("10-") == std::string::npos);        // 那两拍整段没了
    CHECK(cut.find("手电扫过") == std::string::npos);
    CHECK(cut.find("爬进灌木丛") != std::string::npos);  // 第一拍留着
    CHECK(cut.find("[0-3秒]") != std::string::npos);     // 盖满新时长

    // 任何一段都不许起点晚于止点
    for (double d : {1.0, 2.5, 3.0, 4.0, 7.0, 12.0, 15.0, 20.0}) {
        const std::string got = stages::motion_covering(three, d);
        std::smatch m;
        std::string rest = got;
        const std::regex seg(
            R"(\[\s*([0-9]+(?:\.[0-9]+)?)\s*-\s*([0-9]+(?:\.[0-9]+)?)\s*秒\s*\])");
        while (std::regex_search(rest, m, seg)) {
            CHECK(std::stod(m[1].str()) <= std::stod(m[2].str()));
            rest = m.suffix().str();
        }
        // 最后一段的止点正好是新时长
        CHECK(stages::motion_covering(got, d) == got);
    }

    // 拉长照旧只改最后一段
    const std::string longer = stages::motion_covering(three, 20.0);
    CHECK(longer.find("[10-20秒]") != std::string::npos);
    CHECK(longer.find("[0-5秒]") != std::string::npos);
}

TEST_CASE("一镜拆成几镜时，运动描述跟着分段，不许每一镜挂同一条时间轴") {
    // 2026-09-16 实测：ep05_sh019 和 ep05_sh019_b 的 motion_prompt 一字
    // 不差——深拷贝把整条 15 秒时间轴原样带给了两镜，出来两条几乎一样的
    // 视频接在一起，而且每一镜的时间轴都比它自己的时长长一倍。
    const std::string three =
        "[0-5秒] 握紧拳头 [5-10秒] 一问一答 [10-15秒] 灰尘飘浮";

    const auto two = stages::split_motion(three, {6.0, 6.0});
    REQUIRE(two.size() == 2);
    CHECK(two[0] != two[1]);                              // 这就是那条 bug
    CHECK(two[0].rfind("[0-", 0) == 0);                   // 两份都从 0 起算
    CHECK(two[1].rfind("[0-", 0) == 0);
    CHECK(two[0].find("握紧拳头") != std::string::npos);
    CHECK(two[1].find("灰尘飘浮") != std::string::npos);
    CHECK(two[0].find("灰尘飘浮") == std::string::npos);  // 不重叠
    CHECK(two[1].find("握紧拳头") == std::string::npos);

    // 三份：一份一段，谁也不空
    const auto three_ways = stages::split_motion(three, {1.0, 1.0, 1.0});
    REQUIRE(three_ways.size() == 3);
    for (const auto& piece : three_ways) CHECK(!piece.empty());
    CHECK(three_ways[0] != three_ways[1]);
    CHECK(three_ways[1] != three_ways[2]);

    // 段数不够分就每份都给原文——同一个动作跨一刀接着演，说得过去；
    // 硬分会让后半段一点运动描述都没有。
    const auto short_one = stages::split_motion("[0-5秒] 翻动卷宗", {1.0, 1.0});
    REQUIRE(short_one.size() == 2);
    CHECK(short_one[0] == "[0-5秒] 翻动卷宗");
    CHECK(short_one[1] == "[0-5秒] 翻动卷宗");

    // 没写时间轴的照样不会崩
    const auto plain = stages::split_motion("他抬头", {1.0, 1.0});
    REQUIRE(plain.size() == 2);
    CHECK(plain[0] == "他抬头");
}

TEST_CASE("配音锁时长：分了多段动作的长镜头不许压短，一段的照旧收紧") {
    // 2026-09-16 实测：ep05_sh007 计划 15 秒、运动描述写了三段
    // （[0-5秒] / [5-10秒] / [10-15秒]），而它只有一句两秒的台词——
    // 配音把它锁成了 3 秒，那三段动作连同镜头本身一起没了。
    //
    // **判据是「分了几段」，不是一律不许压**：一段的镜头（绝大多数对话镜）
    // 照旧跟着台词收紧，节奏不变；分了两段以上的，说明分镜为这一镜安排了
    // 好几件事要演，那个时长是它的判断。
    using stages::count_motion_segments;
    CHECK(count_motion_segments("[0-5秒] 他爬进灌木丛 [5-10秒] 手电扫过 [10-15秒] 他跑") == 3);
    CHECK(count_motion_segments("[0-3秒] 他抬头") == 1);
    CHECK(count_motion_segments("他抬头") == 0);

    // 锁出来的时长：多段的取 max（保住长镜头），一段的跟着台词走
    const auto locked = [](const std::string& mp, double planned, double speech) {
        double v = stages::ceil_duration(speech + 0.3);
        if (count_motion_segments(mp) >= 2) v = std::max(v, planned);
        return v;
    };
    const std::string multi = "[0-5秒] a [5-10秒] b [10-15秒] c";
    const std::string one = "[0-5秒] a";
    CHECK(locked(multi, 15.0, 2.0) == doctest::Approx(15.0));   // 长镜头保住
    CHECK(locked(one, 5.0, 2.0) < 5.0);                          // 对话镜照旧收紧
    // 话比计划还长时照旧跟着台词涨（用例环境的档位上限是 5 秒，取 5 秒以内的值）
    CHECK(locked(multi, 2.0, 3.5) == doctest::Approx(stages::ceil_duration(3.8)));
    CHECK(locked(multi, 2.0, 3.5) > 2.0);
}
