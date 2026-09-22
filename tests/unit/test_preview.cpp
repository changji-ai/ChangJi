// 「只做前 n 分钟」。
//
// 这里钉的是那几件跑完才会发现的事：
//
//   * 挑前缀用的是**真实时长**（帧数对齐到格子之后的），不是分镜表上那个
//     名义值——差的那几百毫秒逐镜累积，二十镜之后能差出一整镜；
//   * 配音会把镜头**压短**，所以前缀必须在配音之后重挑——挑一次就冻死的话，
//     做出来的片子短过用户要的那个数，而且没有任何一处会说；
//   * 前缀之外的镜头**一格都不许碰**（那是这条路存在的全部理由：二十分钟
//     而不是一个钟头）；
//   * 已经出过片的镜头照样计入长度——不算的话，第二次点会一路往后延，
//     每点一次多做两分钟。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "models/character.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "config/settings.hpp"
#include "doctor/doctor.hpp"
#include "media/assemble.hpp"
#include "media/ffmpeg.hpp"
#include "util/say.hpp"
#include "stages/music.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/preview.hpp"
#include "stages/limits.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 这一套用例都在这份格子下算：Wan 那一族的 4n+1，上限 121 帧。
/// 名义 4 秒（24fps = 96 帧）对齐上去是 97 帧 = 4.0417 秒。
struct WanGrid {
    stages::VideoLimits saved = stages::video_limits();
    WanGrid() {
        stages::VideoLimits v;
        v.max_frames = 121;
        v.frame_step = 4;
        v.frame_base = 1;
        stages::set_video_limits(v);
    }
    ~WanGrid() { stages::set_video_limits(saved); }
};

models::Shot make_shot(const std::string& id, int order, double seconds) {
    models::Shot s;
    s.shot_id = id;
    s.order = order;
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台";
    s.motion_prompt = "镜头缓慢推近";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.duration_s = seconds;
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);
    return s;
}

models::Episode make_episode(int n, double seconds = 4.0) {
    models::Episode ep;
    ep.episode_id = "ep01";
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "ep01_sh%03d", i + 1);
        ep.shots.push_back(make_shot(buf, i, seconds));
    }
    return ep;
}

}  // namespace

TEST_CASE("前 n 分钟按真实时长挑，不按分镜表的名义值") {
    // 名义 4 秒在 4n+1 的格子上是 97 帧 = 4.0417 秒。
    // 目标 12 秒：按名义值算 3 镜正好 12.0「够了」，按真实时长算
    // 3 镜是 12.125——也够。差别要在**边界**上才看得出来，所以取 12.1：
    // 名义值会说 4 镜（3 镜只有 12.0 < 12.1），真实时长说 3 镜。
    const WanGrid grid;
    const models::Episode ep = make_episode(8);

    const auto pick = pipeline::pick_preview_prefix(ep, 12.1,
                                                    stages::video_limits(), 24);
    CHECK(pick.shot_ids.size() == 3);
    CHECK(pick.enough);
    CHECK(pick.planned_s == doctest::Approx(12.125).epsilon(0.01));
    CHECK(pick.shot_ids.front() == "ep01_sh001");
    CHECK(pick.shot_ids.back() == "ep01_sh003");
    CHECK_FALSE(pick.whole_episode);
}

TEST_CASE("跨过那条线的那一镜也要：前 n 分钟是不少于 n") {
    const WanGrid grid;
    const models::Episode ep = make_episode(8);

    // 两镜 8.08 秒，还差一点；第三镜跨过去，总长 12.125 > 10。
    const auto pick = pipeline::pick_preview_prefix(ep, 10.0,
                                                    stages::video_limits(), 24);
    CHECK(pick.shot_ids.size() == 3);
    CHECK(pick.planned_s >= 10.0);
    CHECK(pick.enough);
}

TEST_CASE("整章都不够 n 分钟就整章都做，而且说得出来") {
    const WanGrid grid;
    const models::Episode ep = make_episode(3);

    const auto pick = pipeline::pick_preview_prefix(ep, 600.0,
                                                    stages::video_limits(), 24);
    CHECK(pick.shot_ids.size() == 3);
    CHECK(pick.whole_episode);
    // **这两个不能混。** whole_episode 是"取到底了"，enough 是"够不够"。
    // 只有一个旗子的话，页面没法区分「按你要的做了 2 分钟」和「整章就这么
    // 短，只有 40 秒」——而后者人按下去之前就该知道。
    CHECK_FALSE(pick.enough);
}

TEST_CASE("已经出过片的镜头照样计入长度，不然第二次点会一路往后延") {
    const WanGrid grid;
    models::Episode ep = make_episode(8);
    // 前三镜上一轮已经做完了。
    for (int i = 0; i < 3; ++i) {
        ep.shots[i].status = models::ShotStatus::FINAL_DONE;
        ep.shots[i].video_path = "shots/x.mp4";
    }

    const auto pick = pipeline::pick_preview_prefix(ep, 12.1,
                                                    stages::video_limits(), 24);
    // 还是那三镜——它们已经在片子里占着那段时间了。
    CHECK(pick.shot_ids.size() == 3);
    CHECK(pick.shot_ids.back() == "ep01_sh003");
}

TEST_CASE("进不了成片的镜头不算时间，但位置照旧占着") {
    // 一轮跑完 sh002 被闸门退回：它在片子里是个空档。前缀要往后再取一镜
    // 把时间补上，**而 sh002 自己还留在名单里**——下一轮还要再试它一次，
    // 从名单里摘掉的话它永远没有第二次机会，而片子中间就那么缺着。
    const WanGrid grid;
    const models::Episode ep = make_episode(8);
    const std::set<std::string> dead = {"ep01_sh002"};

    const auto pick = pipeline::pick_preview_prefix(
        ep, 12.1, stages::video_limits(), 24,
        [&dead](const models::Shot& s) { return dead.count(s.shot_id) == 0; });

    CHECK(pick.shot_ids.size() == 4);
    CHECK(std::find(pick.shot_ids.begin(), pick.shot_ids.end(), "ep01_sh002") !=
          pick.shot_ids.end());
    CHECK(pick.planned_s == doctest::Approx(12.125).epsilon(0.01));
}

TEST_CASE("n 给 0 或者负数就是没挑，不是挑了整章") {
    const WanGrid grid;
    const models::Episode ep = make_episode(4);
    CHECK(pipeline::pick_preview_prefix(ep, 0.0, stages::video_limits(), 24)
              .shot_ids.empty());
    CHECK(pipeline::pick_preview_prefix(ep, -5.0, stages::video_limits(), 24)
              .shot_ids.empty());
}

TEST_CASE("前缀按 order 排，不按在数组里的位置") {
    // 分镜表被手工调过顺序之后，数组顺序和 order 对不上，而"前 n 分钟"
    // 说的是**片子开头**那几分钟——按数组顺序挑的话，挑出来的是一堆
    // 散落在全章各处的镜头，接起来根本不是开头。
    const WanGrid grid;
    models::Episode ep;
    ep.episode_id = "ep01";
    ep.shots.push_back(make_shot("ep01_sh003", 2, 4.0));
    ep.shots.push_back(make_shot("ep01_sh001", 0, 4.0));
    ep.shots.push_back(make_shot("ep01_sh002", 1, 4.0));

    const auto pick = pipeline::pick_preview_prefix(ep, 8.0,
                                                    stages::video_limits(), 24);
    REQUIRE(pick.shot_ids.size() == 2);
    CHECK(pick.shot_ids[0] == "ep01_sh001");
    CHECK(pick.shot_ids[1] == "ep01_sh002");
}

TEST_CASE("预告落在 output/preview/ 下，不占正片的位置") {
    // **这条钉的是"为什么要有那个子目录"。**
    //
    // 预告的文件名 stem 就是章号，而 `media::episode_of_output` 正是按 stem
    // 认领的——下面第二条 CHECK 说的就是"摆在 output/ 顶层的话它会被认成
    // 这一章的正片"：那样 film_join 会把半章接进整部电影，项目库那句
    // 「N 章已出片」也跟着错。躲开的办法是位置，不是名字：
    // `get_outputs` 和那两处认领都只扫顶层。
    CHECK(pipeline::preview_output_name("ep01") == "preview/ep01.mp4");
    const auto owner = media::episode_of_output("ep01");
    REQUIRE(owner.has_value());
    CHECK(*owner == "ep01");
    CHECK(fs::path(pipeline::preview_output_name("ep01")).parent_path() ==
          fs::path("preview"));
}

// ---------------------------------------------------------------------------
// 整条编排：配音之后重挑、只跑前缀、够了就停
// ---------------------------------------------------------------------------

namespace {

/// 一次后端调用的记录。测"碰了哪几镜"靠它。
/// 拿 ffmpeg 现做一段能放的 mp4（纯色 + 静音）。定义在底下那个匿名命名
/// 空间里——同一个翻译单元里那两段是同一个命名空间，这儿先报个名。
void make_clip(const fs::path& dest, double seconds, const char* size = "320x180");

struct Recorder {
    std::vector<std::string> frames;
    std::vector<std::string> videos;
    /// 真做能放的片子，不是占位文件。**要走到装配之后那句话就得开它**
    /// ——「假的 MP4」四个字喂给 ffmpeg 是装不出东西来的。
    bool real_clips = false;

    static void write_stub(const fs::path& dest, const char* what) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary);
        out << what;
    }

    stages::FrameRenderer frame() {
        return [this](const models::Shot& shot, const stages::PromptBundle&,
                      const models::TierSpec&, const fs::path& dest,
                      pipeline::CancelToken&, const infer::StepCallback&) {
            frames.push_back(shot.shot_id);
            write_stub(dest, "假的 PNG");
        };
    }

    stages::VideoRenderer video() {
        return [this](const models::Shot& shot, const stages::RenderPlan&,
                      const std::optional<fs::path>&, const fs::path& dest,
                      pipeline::CancelToken&, const infer::StepCallback&) {
            videos.push_back(shot.shot_id);
            // **尺寸要对得上这一档。** 320x180 的片子闸门当场退回
            // 「分辨率 320x180，期望 768x448」，于是一镜都装不成，
            // 而这两条用例要看的正是装完之后说的那句话。
            if (real_clips) make_clip(dest, shot.duration_s, "768x448");
            else write_stub(dest, "假的 MP4");
        };
    }
};

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_预告_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// 一章 n 镜，每镜名义 `seconds` 秒。`line` 非空时每镜都带这么一句台词
/// ——配音会照台词长度把镜头**锁短**，这正是"挑一次不算数"的理由。
models::ProjectStore make_store(const std::string& tag, int n, double seconds,
                                const std::string& line = "") {
    const fs::path root = temp_root(tag);
    auto store = models::ProjectStore::create(root, "yu_ye", "雨夜天台");
    models::Project project = store.load_project();
    models::Episode ep = make_episode(n, seconds);
    if (!line.empty()) {
        for (auto& s : ep.shots) {
            models::DialogueLine d;
            d.char_id = "c_lin_wan";
            d.text = line;
            s.dialogue.push_back(d);
        }
    }
    project.episodes.push_back(ep);
    store.save_project(project);

    models::AssetLibrary assets;
    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性";
    assets.characters["c_lin_wan"] = lin;
    assets.style.global_style = "电影感";
    assets.style.aspect_ratio = "9:16";
    store.save_assets(assets);
    return store;
}

models::HardwareProfile make_profile() {
    models::HardwareProfile p;
    p.vram_gb = 6.0;
    p.tiers = models::tiers_for_vram(6.0);
    p.detected = true;
    return p;
}

pipeline::RunReport run_preview_it(const models::ProjectStore& store,
                                   double preview_s, Recorder& rec,
                                   std::vector<std::string>* said = nullptr,
                                   bool with_ffmpeg = false) {
    pipeline::JobTable table;
    pipeline::CancelToken tok;
    pipeline::Backends backends;
    backends.frame = rec.frame();
    backends.video = rec.video();
    // **不给 ffmpeg 就走不到"做好了"那一句**：装配那一步会说一句
    // 「没有 ffmpeg，装不出预告」就收手，后面的 done 根本不发。查那句话的
    // 用例得把它接上。
    if (with_ffmpeg) {
        backends.ffmpeg = media::FFmpeg("ffmpeg", "ffprobe", media::default_runner());
    }
    // ffmpeg 不给：装配那一步会说一句"装不出预告"就收手，而这几条用例要
    // 钉的是**碰了哪几镜**，不是装出来的文件。
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.preview_s = preview_s;
    // 和 `POST /api/run` 的默认一致（挂 Turbo 之后草稿档就是白跑一遍）。
    // **这一层的默认是"都跑"**，见 RunOptions::skip_draft 上那段。
    opts.skip_draft = true;

    config::Settings settings;
    settings.sound.music = false;   // 这台没有配乐命令，省一句 warn

    pipeline::RunReport report;
    std::string thrown;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        try {
            report = pipeline::run_preview(store, make_profile(),
                                           settings, opts, backends,
                                           p, tok);
        } catch (const std::exception& e) {
            thrown = e.what();
        }
    });
    table.wait_idle();
    // 说了哪几句。**这一族用例原来只看"碰了哪几镜"**，说出来的话一个字
    // 都没查过——而进度条上那一行正是人唯一看得见的东西。
    if (said != nullptr) {
        // ⚠️ **快照要先接住。** 写成
        //     for (const auto& e : table.snapshot(...)["events"])
        // 的话，`snapshot()` 回的那个临时 json 在循环开始之前就析构了
        //（range-for 只给**最终那个表达式**续命，不给中间的那个），
        // 于是循环体一次都不进，而 `.size()` 在同一条语句里量又是对的——
        // 量出 34 条、一条都没收到，我为此追了三轮。
        const nlohmann::json snap = table.snapshot(pipeline::JobKind::Run);
        for (const auto& e : snap["events"]) {
            said->push_back(e.value("message", std::string()));
        }
    }
    if (!thrown.empty()) throw std::runtime_error(thrown);
    return report;
}

}  // namespace

TEST_CASE("只跑前缀那几镜，后面的一格都不碰") {
    const WanGrid grid;
    const auto store = make_store("只跑前缀", 10, 4.0);
    Recorder rec;

    run_preview_it(store, 12.1, rec);

    // 12.1 秒 = 前三镜（4.0417 × 3 = 12.125）。
    CHECK(rec.frames == std::vector<std::string>{"ep01_sh001", "ep01_sh002",
                                                 "ep01_sh003"});
    CHECK(rec.videos == std::vector<std::string>{"ep01_sh001", "ep01_sh002",
                                                 "ep01_sh003"});
    // 剩下七镜原封不动：状态还是未开工，一个文件都没有。
    const models::Project p = store.load_project();
    const models::Episode* ep = p.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    for (std::size_t i = 3; i < ep->shots.size(); ++i) {
        CHECK(ep->shots[i].status == models::ShotStatus::PLANNED);
        CHECK_FALSE(ep->shots[i].video_path.has_value());
    }
}

TEST_CASE("配音把镜头压短之后，前缀要跟着变长") {
    // **这一条是整条编排存在的理由。**
    //
    // 分镜表上每镜 6 秒，按它挑「前 12 秒」是两镜。可这几镜都只有一句两个
    // 字的台词，配音跑完把它们锁到两秒出头——两镜加起来只有四秒多，做出来
    // 的片子**连一半都不到**，而不论哪一层都不会报错：镜头都跑成功了，
    // 装配也成功了，只是短。
    //
    // 挑一次就冻死的写法（开跑前算好 shot_ids）正是这么坏的。
    const WanGrid grid;
    const auto store = make_store("配音压短", 12, 6.0, "走吧");
    Recorder rec;

    run_preview_it(store, 12.0, rec);

    // 压短之后要更多镜头才够 12 秒。**判据是"比两镜多"**，不写死具体几镜
    // ——那个数跟着语速常数走（stages::kCharsPerSecond），钉死它等于把一个
    // 会调的常数焊进用例。
    CHECK(rec.frames.size() > 2);
    // 而且真的够了：按锁定后的时长重算一遍。
    const models::Project p = store.load_project();
    const models::Episode* ep = p.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    double got = 0.0;
    for (const auto& id : rec.frames) {
        for (const auto& s : ep->shots) {
            if (s.shot_id == id) {
                got += stages::video_limits().real_duration_s(s.duration_s, 24);
            }
        }
    }
    CHECK(got >= 12.0);
}

TEST_CASE("第二次点同一个长度，已经做好的不重跑") {
    const WanGrid grid;
    const auto store = make_store("不重跑", 10, 4.0);
    Recorder first;
    run_preview_it(store, 12.1, first);
    REQUIRE(first.videos.size() == 3);

    Recorder again;
    run_preview_it(store, 12.1, again);
    // **一镜都不该再跑**：前三镜已经是终态，而它们的时长照样算数，
    // 所以前缀还是那三镜，不会往后延。
    CHECK(again.frames.empty());
    CHECK(again.videos.empty());
}

// ---------------------------------------------------------------------------
// 装配：预告落在哪、字幕落在哪、配乐用谁
// ---------------------------------------------------------------------------
//
// **这三样错了都不报错。** 装配成功、片子能放，只是：正片被当成半章接进
// 整部电影、播放器挂上的是另一条片子的字幕、整章的后半截静悄悄没有配乐。
//
// 这一条**要真跑 ffmpeg**（这套里唯一一条）：判的是"文件最后落在哪儿"，
// 而那是 Assembler 真写盘那一下才决定的。机器上没有 ffmpeg 就跳过——
// 那时候整个装配阶段本来就不跑（run_episode 里那句"没有 ffmpeg，跳过装配"）。

namespace {

bool have_ffmpeg() {
    const media::FFmpeg ff("ffmpeg", "ffprobe", media::default_runner());
    return ff.available();
}

/// 拿 ffmpeg 现做一段能放的 mp4（纯色 + 静音）。
void make_clip(const fs::path& dest, double seconds, const char* size) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const media::Runner run = media::default_runner();
    char dur[32];
    std::snprintf(dur, sizeof(dur), "%.3f", seconds);
    run("ffmpeg",
        {"-y", "-f", "lavfi", "-i",
         std::string("color=c=navy:s=") + size + ":d=" + dur,
         "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-shortest",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-r", "24",
         paths::to_utf8(dest)},
        {});
}

}  // namespace

TEST_CASE("预告装到 output/preview/ 下，字幕不盖正片那份") {
    if (!have_ffmpeg()) return;   // 没有 ffmpeg 的机器上装配整个不跑
    const WanGrid grid;
    const auto store = make_store("装配", 4, 4.0);

    // 四镜都出好了片。
    models::Project project = store.load_project();
    models::Episode* ep = project.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    for (auto& s : ep->shots) {
        const std::string rel = "shots/" + s.shot_id + ".mp4";
        make_clip(store.paths().abs(rel), 4.0);
        s.video_path = rel;
        s.status = models::ShotStatus::FINAL_DONE;
        models::DialogueLine line;
        line.char_id = "c_lin_wan";
        line.text = "走吧";
        line.actual_duration_s = 1.2;
        s.dialogue.push_back(line);
    }
    store.save_project(project);

    pipeline::JobTable table;
    config::Settings settings;
    settings.sound.music = false;   // 这台没有配乐命令，省一句 warn
    const media::FFmpeg ff("ffmpeg", "ffprobe", media::default_runner());

    // 只装前两镜，走预告那条。
    pipeline::AssembleOptions aopts;
    aopts.only_shots = {"ep01_sh001", "ep01_sh002"};
    aopts.out_name = pipeline::preview_output_name("ep01");
    aopts.music_suffix = "_preview";
    aopts.sweep_orphans = false;
    double total = 0.0;
    aopts.total_s = &total;

    std::string out;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        models::Project pj = store.load_project();
        out = pipeline::assemble_episode(store, settings, *pj.episode_by_id("ep01"),
                                         ff, p, aopts);
    });
    table.wait_idle();

    // 片子在子目录里，**不在 output/ 顶层**——顶层那个名字是正片的，
    // media::episode_of_output 认它。
    CHECK(fs::is_regular_file(store.paths().output() / "preview" / "ep01.mp4"));
    CHECK_FALSE(fs::exists(store.paths().output() / "ep01.mp4"));
    CHECK(out.find("preview") != std::string::npos);
    // 只装了两镜：长度是两镜，不是四镜。
    CHECK(total == doctest::Approx(8.08).epsilon(0.05));

    // 字幕跟着落到 subtitles/preview/ 下，**没有盖掉正片那份**。
    CHECK(fs::is_regular_file(store.paths().subtitles() / "preview" / "ep01.ass"));
    CHECK_FALSE(fs::exists(store.paths().subtitles() / "ep01.ass"));

    std::error_code ec;
    fs::remove_all(store.root(), ec);
}

TEST_CASE("预告的配乐用自己的文件名，不会把整章那条占了") {
    // 配乐是"文件在就沿用"，而 ffmpeg 那头短了不循环：预告先出一条两分钟
    // 的，整章再跑就沿用它——**后面几分钟静悄悄没有配乐**。
    const models::ProjectPaths paths(fs::temp_directory_path() / "changji_配乐名");
    CHECK(stages::music_path_for(paths, "ep01").filename() ==
          fs::path("ep01_music.wav"));
    CHECK(stages::music_path_for(paths, "ep01", "_preview").filename() ==
          fs::path("ep01_preview_music.wav"));
    CHECK(stages::music_path_for(paths, "ep01") !=
          stages::music_path_for(paths, "ep01", "_preview"));
}

// ---------------------------------------------------------------------------
// 本机出不了图，但机器表里有能干的
// ---------------------------------------------------------------------------

TEST_CASE("本机没编 sd.cpp：两样都派得出去才算能开工") {
    // 2026-09-20 实测撞到的：一台 Mac（CHANGJI_SD=OFF）加了一台五项全绿的
    // L20，机器表上 frame / video 都亮着，而镜头页三颗按钮全灰，写着
    // 「出图后端 · 没编进来」——**那台卡正是为这件事租的**。
    //
    // 降级的判据是「两样都派得出去」，不是「随便哪样」：只有一样的话，
    // 另一样仍然没人能干，而出片这条链少哪一段都走不完。点亮按钮让人跑
    // 二十分钟撞在后半截上，比一开始就灰着更糟。
    CHECK(doctor::sd_level_without_local(true, true) == doctor::Level::WARN);
    CHECK(doctor::sd_level_without_local(true, false) == doctor::Level::FAIL);
    CHECK(doctor::sd_level_without_local(false, true) == doctor::Level::FAIL);
    CHECK(doctor::sd_level_without_local(false, false) == doctor::Level::FAIL);
}

TEST_CASE("整章都在这一段预览里的时候，不许再说「还有 0 镜没做」") {
    // **0 那个数比没有更糟。** 原来那句是无条件加的，于是整章都被预览盖住
    // 时，屏幕上写着「整章还有 0 镜没做，接着按「出片」就是把这一章补完」
    // ——先要人自己反应一下"0 等于没有"，然后还劝他再按一次「出片」，
    // 而根本没有可补的。
    //
    // ⚠️ 这一族用例原来只钉"碰了哪几镜"，**说出来的话一个字都没查过**，
    // 而进度条上那一行正是人唯一看得见的东西。查它得把 ffmpeg 接上：
    // 不接的话装配那一步先收手，"做好了"那句根本不发，用例会**绿得
    // 莫名其妙**——它什么都没查到。
    if (!have_ffmpeg()) return;
    const WanGrid grid;
    const auto store = make_store("整章都在里面", 3, 4.0);
    Recorder rec;
    rec.real_clips = true;
    std::vector<std::string> said;

    run_preview_it(store, 600.0, rec, &said, /*with_ffmpeg=*/true);

    bool told = false;
    for (const std::string& one : said) {
        CAPTURE(one);
        CHECK(one.find("整章还有") == std::string::npos);
        if (one.find("的片子做好了") != std::string::npos) told = true;
    }
    CHECK_MESSAGE(told, "连「做好了」那句都没说——这条用例没在查东西");
}

TEST_CASE("还剩几镜没做的时候，那句话要说，而且数得对") {
    if (!have_ffmpeg()) return;
    const WanGrid grid;
    const auto store = make_store("剩几镜", 10, 4.0);
    Recorder rec;
    rec.real_clips = true;
    std::vector<std::string> said;

    // 12.1 秒 = 前三镜（4.0417 × 3），还剩七镜。
    run_preview_it(store, 12.1, rec, &said, /*with_ffmpeg=*/true);

    bool told = false;
    for (const std::string& one : said) {
        if (one.find("整章还有") == std::string::npos) continue;
        told = true;
        CAPTURE(one);
        CHECK(one.find("整章还有 7 镜没做") != std::string::npos);
    }
    CHECK_MESSAGE(told, "该说的没说");
}

TEST_CASE("剩一镜的时候，英语那句话得是单数——真跑一遍流水线") {
    // **这条才是那一族毛病本身。** 从前这句在英语下是
    // 「The chapter still has 1 shots to go」——满屏都对，就这一个字怪。
    //
    // ⚠️ 和 `test_say.cpp` 里那几条不一样：那几条是**直接问表**，这一条走
    // 的是**真流水线**——出首帧、出片、闸门、装配，最后那句话是
    // `preview.cpp` 现拼的。表对了而调用处把数传错（`%n` 和 `%1` 混了）
    // 的话，只有这一条会红。
    if (!have_ffmpeg()) return;
    const WanGrid grid;
    // 两镜的章，只做前一镜：**挑出来的是 1 镜、剩下的也是 1 镜**——
    // 一趟把两句话的单数都验了。
    const auto store = make_store("剩一镜", 2, 4.0);
    Recorder rec;
    rec.real_clips = true;
    std::vector<std::string> said;

    // 跑完把语言放回去：这是个**进程级**的开关，别的用例也在看它。
    struct Back {
        std::string was = changji::i18n::spoken();
        ~Back() { changji::i18n::speak(was); }
    } back;
    changji::i18n::speak("en");

    // ⚠️ **3 秒，不是 4.1 秒。** 一镜实测 4.0417 秒，而「前 n 秒」的规矩是
    // **不少于 n**——4.1 秒要两镜才够，整章就都在里面了，那句「还剩」反而
    // 一个字都不该说（上面那条用例钉的正是这个）。3 秒只要一镜。
    run_preview_it(store, 3.0, rec, &said, /*with_ffmpeg=*/true);

    bool told = false, picked = false;
    for (const std::string& one : said) {
        if (one.find("chapter still has") != std::string::npos) {
            told = true;
            CAPTURE(one);
            CHECK(one.find("still has 1 shot to go") != std::string::npos);
            CHECK(one.find("1 shots") == std::string::npos);
        }
        // 开工那一句同一族：「going by the first 4 s: 1 shot from the top」。
        if (one.find("shot from the top") != std::string::npos
            || one.find("shots from the top") != std::string::npos) {
            picked = true;
            CAPTURE(one);
            CHECK(one.find("1 shot from the top") != std::string::npos);
            CHECK(one.find("1 shots from the top") == std::string::npos);
        }
    }
    CHECK_MESSAGE(told, "英语下连这句都没说");
    CHECK_MESSAGE(picked, "开工那一句也没说");
}
