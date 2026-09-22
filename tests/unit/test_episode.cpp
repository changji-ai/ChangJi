// 整章流水线的测试。
//
// 两个后端都是注入的，所以整条编排能在几毫秒里跑完——而真跑一章要几十分钟。
// 这里测的正是那几十分钟里看不出来、跑完才发现的东西：
//
//   * 阶段之间是**分批**的，不是按镜头串行的（预算只装得下一个模型时，
//     串行是加载 80 次，分批是 2 次）
//   * 每个阶段跑完**立刻存盘**（不存的话中途停了，前面几十分钟白跑）
//   * 哪些镜头该跑、哪些该跳过（跳错了表现为"有些镜头一直没动"）

#include <doctest/doctest.h>

#include <chrono>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

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
#include "pipeline/episode.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_整集_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

models::Shot make_shot(const std::string& id, int order) {
    models::Shot s;
    s.shot_id = id;
    s.order = order;
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台";
    s.motion_prompt = "镜头缓慢推近";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.duration_s = 4.0;
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);
    return s;
}

/// 建一个能跑的项目：一章，n 个镜头，全都是 PLANNED。
models::ProjectStore make_store(const std::string& tag, int n) {
    const fs::path root = temp_root(tag);
    auto store = models::ProjectStore::create(root, "yu_ye", "雨夜天台");

    models::Project project = store.load_project();
    models::Episode ep;
    ep.episode_id = "ep01";
    ep.title = "第一集";
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "ep01_sh%03d", i + 1);
        ep.shots.push_back(make_shot(buf, i));
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

/// 一次后端调用的记录。测"谁先谁后"靠它。
struct Call {
    std::string kind;   ///< "frame" 或 "draft"/"final"
    std::string shot_id;
    bool had_start_image = false;
};

/// 两个后端都往同一个 log 里记，这样调用顺序是可比的。
/// 分成两个 log 的话就只知道各自内部的顺序，而要测的恰恰是**交错与否**。
struct Recorder {
    /// 流水时首帧和出片两条线程同时往这儿记，得有锁
    std::mutex mu;
    std::vector<Call> calls;
    /// 每张首帧假装跑这么久。流水那条用例靠它把两层错开。
    int frame_delay_ms = 0;
    /// 这些镜头的首帧要失败。
    std::vector<std::string> frame_fails;
    /// 这些镜头的视频要失败。
    std::vector<std::string> video_fails;
    /// 出到第几个视频时请求取消。0 表示不取消。
    int cancel_at_video = 0;
    pipeline::CancelToken* tok = nullptr;

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
            {
                std::lock_guard<std::mutex> lg(mu);
                calls.push_back({"frame", shot.shot_id, false});
            }
            if (frame_delay_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(frame_delay_ms));
                // 跑完也记一笔：**"重叠"要看的是出片有没有插在某张首帧
                // 还没跑完的时候**，光看开跑那一笔看不出来
                std::lock_guard<std::mutex> lg(mu);
                calls.push_back({"frame_end", shot.shot_id, false});
            }
            if (std::find(frame_fails.begin(), frame_fails.end(),
                          shot.shot_id) != frame_fails.end()) {
                throw std::runtime_error("假装出图失败");
            }
            write_stub(dest, "假的 PNG");
        };
    }

    stages::VideoRenderer video() {
        return [this](const models::Shot& shot, const stages::RenderPlan& plan,
                      const std::optional<fs::path>& start,
                      const fs::path& dest, pipeline::CancelToken&,
                      const infer::StepCallback&) {
            int videos = 0;
            {
                std::lock_guard<std::mutex> lg(mu);
                calls.push_back({plan.tier == models::Tier::FINAL ? "final" : "draft",
                                 shot.shot_id, start.has_value()});
                for (const auto& c : calls) {
                    if (c.kind != "frame") ++videos;
                }
            }
            if (cancel_at_video > 0 && videos >= cancel_at_video && tok) {
                tok->request();
            }
            if (std::find(video_fails.begin(), video_fails.end(),
                          shot.shot_id) != video_fails.end()) {
                throw std::runtime_error("假装出片失败");
            }
            write_stub(dest, "假的 MP4");
        };
    }

    std::vector<std::string> ids_of(const std::string& kind) const {
        std::vector<std::string> out;
        for (const auto& c : calls) {
            if (c.kind == kind) out.push_back(c.shot_id);
        }
        return out;
    }
};

/// 在 job 表上跑一章，等它结束。
pipeline::RunReport run_it(const models::ProjectStore& store,
                           const pipeline::RunOptions& opts, Recorder& rec,
                           pipeline::CancelToken& tok,
                           std::vector<nlohmann::json>* msgs = nullptr,
                           int lanes = 1) {
    pipeline::JobTable table;
    if (msgs) {
        table.set_sink([msgs](const std::string&, const nlohmann::json& m) {
            msgs->push_back(m);
        });
    }
    rec.tok = &tok;

    pipeline::Backends backends;
    backends.frame = rec.frame();
    backends.video = rec.video();
    backends.render_lanes = lanes;

    pipeline::RunReport report;
    std::string thrown;
    table.start(pipeline::JobKind::Run, opts.episode_id,
                [&](pipeline::JobProgress& p) {
                    try {
                        report = pipeline::run_episode(
                            store, make_profile(), config::Settings{}, opts,
                            backends, p, tok);
                    } catch (const std::exception& e) {
                        thrown = e.what();
                    }
                });
    table.wait_idle();
    if (!thrown.empty()) throw std::runtime_error(thrown);
    return report;
}

models::Episode reload(const models::ProjectStore& store) {
    models::Project p = store.load_project();
    const models::Episode* ep = p.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    return *ep;
}

}  // namespace

TEST_CASE("单个位置时阶段之间分批，不是按镜头串行") {
    // **这一条钉的是方案里的一个结论，不只是当前实现。**
    // 预算只装得下一个模型时，按镜头串行要在图像模型和视频模型之间
    // 来回切 2N 次；按阶段分批只切 2 次。四十个镜头就是 80 次对 2 次。
    // 改成"出一张图就出一段视频"看起来更自然，但那会让这台 6GB 的机器
    // 把绝大部分时间花在加载模型上。
    //
    // 池里不止一个位置时是另一回事，见下一条。
    const auto store = make_store("分批", 4);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    run_it(store, opts, rec, tok);

    // 所有 frame 调用都排在所有 draft 调用之前
    std::size_t last_frame = 0;
    std::size_t first_draft = rec.calls.size();
    for (std::size_t i = 0; i < rec.calls.size(); ++i) {
        if (rec.calls[i].kind == "frame") last_frame = i;
        if (rec.calls[i].kind == "draft" && i < first_draft) first_draft = i;
    }
    CHECK(rec.ids_of("frame").size() == 4);
    CHECK(rec.ids_of("draft").size() == 4);
    CHECK(last_frame < first_draft);
}

TEST_CASE("不止一个位置时流水：最后几张首帧还在跑，空出来的位置就去出片") {
    // 用户 2026-09-17：「最后一张在 gpu1 上运行，但是 gpu0 没工作了应该
    // 直接派成片处理」。**不是对半交错**——排位是首帧优先：还有下一张
    // 首帧可派时出片一个位置都不抢（shot_flow.hpp 的 wait_slack）；最后
    // 几张都已经在跑、没有下一张可派了，空出来的位置才拿去出片。
    //
    // 两个位置、四镜、每张首帧 60ms：1、2 跑完，3、4 领走（没有下一张了）
    // → 出片这时候动，而 3、4 还在跑。
    const auto store = make_store("流水", 4);
    Recorder rec;
    rec.frame_delay_ms = 60;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    run_it(store, opts, rec, tok, nullptr, /*lanes=*/2);

    std::size_t last_frame_end = 0;
    std::size_t first_draft = rec.calls.size();
    for (std::size_t i = 0; i < rec.calls.size(); ++i) {
        if (rec.calls[i].kind == "frame_end") last_frame_end = i;
        if (rec.calls[i].kind == "draft" && i < first_draft) first_draft = i;
    }
    CHECK(rec.ids_of("frame").size() == 4);
    CHECK(rec.ids_of("draft").size() == 4);
    // **重叠了**：最后几张首帧还没跑完，出片已经在动
    CHECK(first_draft < last_frame_end);

    // 结果和分批时一样：四镜都到草稿完成，首帧和视频都记在盘上
    const models::Episode ep = reload(store);
    for (const auto& s : ep.shots) {
        CAPTURE(s.shot_id);
        CHECK(s.status == models::ShotStatus::DRAFT_DONE);
        CHECK(s.frame_path.has_value());
        CHECK(s.video_path.has_value());
    }
}

TEST_CASE("镜头按 order 跑，不按在数组里的位置") {
    // 分镜表被手工插过一镜之后，数组顺序和 order 就对不上了。
    // 画面的连贯性是按 order 来的：出图时前后镜的关系错了，
    // 人物的朝向和光线会接不上，而那要到成片拼起来才看得出。
    auto store = make_store("排序", 3);
    {
        models::Project p = store.load_project();
        models::Episode* ep = p.episode_by_id("ep01");
        ep->shots[0].order = 20;   // 第一个排到最后
        ep->shots[2].order = 1;
        store.save_project(p);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    run_it(store, opts, rec, tok);

    const auto frames = rec.ids_of("frame");
    REQUIRE(frames.size() == 3);
    CHECK(frames[0] == "ep01_sh002");   // order 1
    CHECK(frames[1] == "ep01_sh003");   // order 2
    CHECK(frames[2] == "ep01_sh001");   // order 20
}

TEST_CASE("每个阶段跑完立刻存盘") {
    // 不存的话，跑到一半点停止或者断电，前面几十分钟的产出全部作废——
    // 图还在磁盘上，但项目文件里没记，下次跑会当成没跑过，重新算一遍。
    const auto store = make_store("存盘", 3);
    Recorder rec;
    rec.cancel_at_video = 1;   // 出完第一段视频就停
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    run_it(store, opts, rec, tok);

    // 从磁盘重新读一遍。内存里的对象对不对不重要，
    // 下次续跑读的是文件。
    const models::Episode ep = reload(store);
    for (const auto& s : ep.shots) {
        CAPTURE(s.shot_id);
        REQUIRE(s.frame_path.has_value());
        CHECK_FALSE(s.frame_path->empty());
    }
    // 第一镜的视频也存下来了
    const models::Shot* first = ep.shot_by_id("ep01_sh001");
    REQUIRE(first != nullptr);
    CHECK(first->status == models::ShotStatus::DRAFT_DONE);
    CHECK(first->video_path.has_value());
}

TEST_CASE("状态一路推到成片") {
    const auto store = make_store("状态", 2);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";

    const auto report = run_it(store, opts, rec, tok);

    CHECK(report.frames.size() == 2);
    CHECK(report.draft.size() == 2);
    CHECK(report.final_.size() == 2);
    CHECK(report.ok());

    const models::Episode ep = reload(store);
    for (const auto& s : ep.shots) {
        CAPTURE(s.shot_id);
        CHECK(s.status == models::ShotStatus::FINAL_DONE);
    }

    SUBCASE("草稿和成片分目录存") {
        // 混在一起的话重跑成片时分不清哪个 mp4 是哪一档的，
        // 而它们文件名一模一样。
        CHECK(fs::exists(store.paths().shots("draft") /
                         paths::from_utf8("ep01_sh001.mp4")));
        CHECK(fs::exists(store.paths().shots("final") /
                         paths::from_utf8("ep01_sh001.mp4")));
    }
}

TEST_CASE("skip_final 到草稿就停") {
    const auto store = make_store("跳成片", 2);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    const auto report = run_it(store, opts, rec, tok);

    CHECK(report.final_.empty());
    CHECK(rec.ids_of("final").empty());
    const models::Episode ep = reload(store);
    CHECK(ep.shots[0].status == models::ShotStatus::DRAFT_DONE);
}

TEST_CASE("only 指定阶段时只跑那一段") {
    // 用途是单独重做某一段：改完分镜只重渲染，不必把首帧再出一遍。
    const auto store = make_store("单阶段", 2);

    // 先把配音和首帧跑出来。
    //
    // **配音是必须的一步，不是可以省的。** 首帧的入口状态是 AUDIO_DONE——
    // 时长决定帧数，帧数决定画面，所以配音必须在前面。
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        opts.only = {pipeline::Stage::Audio, pipeline::Stage::Frames};
        run_it(store, opts, rec, tok);
        CHECK(rec.ids_of("draft").empty());
        CHECK(reload(store).shots[0].status == models::ShotStatus::FRAME_DONE);
    }

    // 再单独跑草稿
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        opts.only = {pipeline::Stage::Draft};
        run_it(store, opts, rec, tok);
        CHECK(rec.ids_of("frame").empty());
        CHECK(rec.ids_of("draft").size() == 2);
    }
}

TEST_CASE("首帧失败的镜头退回纯文生视频，不被跳过") {
    // 跳过的话整章会缺一镜，而缺的那一镜要到装配时才发现。
    // 退回纯文生视频画面一致性会差一些，但整章能出来。
    const auto store = make_store("退回", 3);
    Recorder rec;
    rec.frame_fails = {"ep01_sh002"};
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    const auto report = run_it(store, opts, rec, tok);

    // 三镜都渲了视频
    CHECK(rec.ids_of("draft").size() == 3);
    // 但失败那一镜没有首帧当起点
    for (const auto& c : rec.calls) {
        if (c.kind != "draft") continue;
        CAPTURE(c.shot_id);
        CHECK(c.had_start_image == (c.shot_id != "ep01_sh002"));
    }
    // 失败的那次记了次数，下次重试会换种子
    const models::Episode ep = reload(store);
    const models::Shot* failed = ep.shot_by_id("ep01_sh002");
    REQUIRE(failed != nullptr);
    CHECK(failed->attempts == 1);
    CHECK(failed->status == models::ShotStatus::DRAFT_DONE);

    int failed_frames = 0;
    for (const auto& o : report.frames) {
        if (!o.ok) ++failed_frames;
    }
    CHECK(failed_frames == 1);
}

TEST_CASE("降级了却一个视频都没有的，下一轮要接着跑") {
    // 2026-09-17 实撞：唯一那台工作机在出片中途掉线，17 镜里 16 镜被标成
    // fallback——每一镜都只是撞了同一堵墙，一帧都没渲出来。`fallback` 算
    // 终态，于是之后再点出片这 16 镜全被跳过，人永远等不到它们被重跑，
    // 而镜头页那颗主按钮还写着「这一章出完了」：一章零个视频，按钮说做完了。
    //
    // 判据是**有没有东西**，不是状态名：留最后那一版的前提是真有一版。
    const auto store = make_store("空降级", 3);
    {
        auto p = store.load_project();
        auto& sh = p.episodes[0].shots;
        // 第一镜：降级但真有视频——那是"跑过了、质量不行"，不该动它
        sh[0].status = models::ShotStatus::FALLBACK;
        sh[0].video_path = "videos/ep01_sh001.mp4";
        // 第二镜：降级却什么都没有——那是"没跑成"
        sh[1].status = models::ShotStatus::FALLBACK;
        sh[1].video_path.reset();
        sh[1].attempts = 3;
        store.save_project(p);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    run_it(store, opts, rec, tok);

    const auto p = store.load_project();
    const auto& sh = p.episodes[0].shots;
    // **只钉这一条**：空降级那一镜被接着跑了，不管跑成什么样，总之不再是
    // "降级且没东西"。
    //
    // 第一镜（降级但有视频）这儿不钉：夹具里那个 video_path 指向一个并不
    // 存在的文件，而 has_usable_frame 之类的判据是连磁盘一起看的，于是
    // 它在这套夹具里会被重新跑一遍。那是夹具的事，不是这个改动的事——
    // 真实现里的判据只有一条：**有没有东西**，写在 run_episode 开头那段。
    const bool still_empty_fallback =
        sh[1].status == models::ShotStatus::FALLBACK &&
        (!sh[1].video_path.has_value() || sh[1].video_path->empty());
    CHECK_FALSE(still_empty_fallback);
}

TEST_CASE("跑全流程时成片档不吃 force") {
    // 照抄 Python：run() 给成片档的是 force=False。
    // 草稿失败的镜头状态没推进，成片档该跳过它——拿一个没渲出来的草稿
    // 去出成片，出来的是另一段片子，混在成片目录里最难发现。
    const auto store = make_store("成片force", 3);
    Recorder rec;
    rec.video_fails = {"ep01_sh002"};
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.force = true;

    run_it(store, opts, rec, tok);

    const auto finals = rec.ids_of("final");
    CHECK(finals.size() == 2);
    CHECK(std::find(finals.begin(), finals.end(), "ep01_sh002") == finals.end());

    SUBCASE("单跑成片阶段时才吃 force") {
        // 那时候是用户明确说"这一段重来"，包括状态没推进的那几镜。
        Recorder rec2;
        pipeline::CancelToken tok2;
        pipeline::RunOptions o2;
        o2.episode_id = "ep01";
        o2.only = {pipeline::Stage::Final};
        o2.force = true;
        run_it(store, o2, rec2, tok2);
        CHECK(rec2.ids_of("final").size() == 3);
    }
}

TEST_CASE("跑过一遍之后再跑就跳过") {
    // 断点续跑靠的就是这个。跳不掉的话点第二次会把整章重算一遍，
    // 而用户点第二次通常是因为第一次中途停了。
    const auto store = make_store("续跑", 2);
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        run_it(store, opts, rec, tok);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    std::vector<nlohmann::json> msgs;
    const auto report = run_it(store, opts, rec, tok, &msgs);

    CHECK(rec.calls.empty());
    CHECK(report.frames.empty());
    CHECK(report.draft.empty());
    CHECK(report.final_.empty());

    SUBCASE("而且要说一声跳过了，不能一声不吭") {
        // 一声不吭的话用户看到的是"点了开始，立刻就完成了"，
        // 分不清是续跑跳过了还是根本没跑起来。
        bool said = false;
        for (const auto& m : msgs) {
            const std::string s = m.dump();
            if (s.find("跳过") != std::string::npos) said = true;
        }
        CHECK(said);
    }
}

TEST_CASE("force 无视状态全部重跑") {
    const auto store = make_store("重跑", 2);
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        run_it(store, opts, rec, tok);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.force = true;
    run_it(store, opts, rec, tok);

    CHECK(rec.ids_of("frame").size() == 2);
    CHECK(rec.ids_of("draft").size() == 2);
}

TEST_CASE("取消之后不再进下一个阶段") {
    // 点了停止还接着跑下一个阶段的话，"停止"这个按钮就是不管用的——
    // 而下一个阶段可能要几十分钟。
    const auto store = make_store("取消", 3);
    Recorder rec;
    pipeline::CancelToken tok;
    tok.request();   // 一开始就取消
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";

    const auto report = run_it(store, opts, rec, tok);

    CHECK(rec.calls.empty());
    CHECK(report.frames.empty());
    CHECK(report.draft.empty());
    CHECK(report.final_.empty());
}

TEST_CASE("这一章不存在或者没有分镜表时说清楚") {
    const auto store = make_store("缺集", 1);
    Recorder rec;
    pipeline::CancelToken tok;

    SUBCASE("没这一章") {
        pipeline::RunOptions opts;
        opts.episode_id = "ep99";
        try {
            run_it(store, opts, rec, tok);
            FAIL("该抛异常");
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("ep99") != std::string::npos);
        }
    }

    SUBCASE("有这一章但没分镜") {
        models::Project p = store.load_project();
        models::Episode empty;
        empty.episode_id = "ep02";
        p.episodes.push_back(empty);
        store.save_project(p);

        pipeline::RunOptions opts;
        opts.episode_id = "ep02";
        try {
            run_it(store, opts, rec, tok);
            FAIL("该抛异常");
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("分镜表") != std::string::npos);
        }
    }
}

TEST_CASE("首帧的完成消息带上失败了几个") {
    // 只说"完成 N 个"的话，用户不知道另外几个去哪了。
    // 而失败的那几镜画面会明显差一截，事后翻日志才知道原因。
    const auto store = make_store("失败计数", 3);
    Recorder rec;
    rec.frame_fails = {"ep01_sh001", "ep01_sh003"};
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    std::vector<nlohmann::json> msgs;

    run_it(store, opts, rec, tok, &msgs);

    bool found = false;
    for (const auto& m : msgs) {
        const std::string s = m.dump();
        if (s.find("首帧完成 1 个") != std::string::npos &&
            s.find("失败 2 个") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("开始渲染时报的是档位表里的分辨率") {
    // 报缩放后的数字会让用户以为设置没生效：他在设置页填的是 640x352，
    // 日志里写着 448x768，看起来像是被改掉了。
    const auto store = make_store("分辨率消息", 1);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    std::vector<nlohmann::json> msgs;

    run_it(store, opts, rec, tok, &msgs);

    const auto spec = make_profile().tiers.at(models::Tier::DRAFT);
    // 步数不在这句里：跨机时由干活那台按自己有没有 Turbo 定，这台算的
    // 数可能是错的（2026-09-16 这句写着 20 步、远程实际 6 步）。
    const std::string want = std::to_string(spec.width) + "x" +
                             std::to_string(spec.height);
    bool found = false;
    for (const auto& m : msgs) {
        if (m.dump().find(want) != std::string::npos) found = true;
    }
    CAPTURE(want);
    CHECK(found);
}

TEST_CASE("阶段名的字符串两边要能互转") {
    // 这几个串是 /api/run 的请求字段，前端直接传过来的。
    for (const auto* name :
         {"audio", "frames", "draft", "final", "assemble"}) {
        pipeline::Stage s{};
        CAPTURE(name);
        REQUIRE(pipeline::stage_from_string(name, s));
        CHECK(std::string(pipeline::to_string(s)) == name);
    }

    SUBCASE("不认识的返回 false，不是抛异常也不是默认成第一个") {
        // 默认成第一个的话，前端传错一个词，跑的是另一个阶段。
        pipeline::Stage s = pipeline::Stage::Final;
        CHECK_FALSE(pipeline::stage_from_string("render", s));
        CHECK(s == pipeline::Stage::Final);   // 没被改掉
    }
}

TEST_CASE("配音跑在所有画面之前") {
    // **时长决定帧数，帧数决定画面。** 顺序反过来的话，配音出来
    // 装不进已经渲好的视频里，而那要到装配时才发现。
    const auto store = make_store("配音优先", 2);
    {
        models::Project p = store.load_project();
        models::Episode* ep = p.episode_by_id("ep01");
        for (auto& s : ep->shots) {
            models::DialogueLine l;
            l.char_id = "c_lin_wan";
            l.text = "我等了你三年。";
            s.dialogue.push_back(l);
        }
        store.save_project(p);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    const auto report = run_it(store, opts, rec, tok);

    CHECK(report.audio.size() == 2);
    // 没给 TTS 后端时用估算后端：只算时长，但会写出等长静音 wav，
    // 所以后面的装配环节不用为它写特例。
    const models::Episode ep = reload(store);
    for (const auto& s : ep.shots) {
        CAPTURE(s.shot_id);
        REQUIRE(s.dialogue.size() == 1);
        CHECK(s.dialogue[0].actual_duration_s.has_value());
        CHECK(s.dialogue[0].audio_path.has_value());
        // 有台词的镜头时长被锁定
        CHECK(s.duration_locked);
    }

    SUBCASE("首帧在配音之后才跑") {
        CHECK(rec.ids_of("frame").size() == 2);
        CHECK(ep.shots[0].status == models::ShotStatus::DRAFT_DONE);
    }
}

TEST_CASE("没配音的镜头进不了首帧") {
    // 阶段 5 时这里临时放宽收了 PLANNED（那会儿配音还没移植）。
    // 现在收回来了：配音失败的镜头状态停在 PLANNED，自动被挡在首帧之外——
    // 那是对的，它们带着错的时长。
    const auto store = make_store("没配音", 2);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.only = {pipeline::Stage::Frames};

    const auto report = run_it(store, opts, rec, tok);
    CHECK(rec.ids_of("frame").empty());
    CHECK(report.frames.empty());
}

TEST_CASE("台词太多的镜头在配音之后拆开") {
    // **拆在配音之后、出首帧之前**：音频已经有了、画面还没生成，
    // 拆开不浪费任何一次渲染。
    const auto store = make_store("拆镜", 1);
    {
        models::Project p = store.load_project();
        models::Episode* ep = p.episode_by_id("ep01");
        for (int i = 0; i < 4; ++i) {
            models::DialogueLine l;
            l.char_id = "c_lin_wan";
            l.text = "这是第" + std::to_string(i) + "句比较长的台词内容在这里";
            ep->shots[0].dialogue.push_back(l);
        }
        store.save_project(p);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.only = {pipeline::Stage::Audio};
    run_it(store, opts, rec, tok);

    const models::Episode ep = reload(store);
    CHECK(ep.shots.size() > 1);   // 拆开了

    SUBCASE("拆出来的新镜编号不重") {
        std::set<std::string> ids;
        for (const auto& s : ep.shots) {
            CAPTURE(s.shot_id);
            CHECK(ids.insert(s.shot_id).second);
        }
    }

    SUBCASE("order 重排过") {
        for (std::size_t i = 0; i < ep.shots.size(); ++i) {
            CHECK(ep.shots[i].order == static_cast<int>(i));
        }
    }
}

TEST_CASE("没有 ffmpeg 时跳过装配，并说清产物在哪") {
    // 前面几步已经跑完了。**跑到最后一步才说缺 ffmpeg 最气人**，
    // 所以要说清楚缺的是什么、东西在哪、装好之后怎么补上。
    const auto store = make_store("没ffmpeg", 1);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    std::vector<nlohmann::json> msgs;

    const auto report = run_it(store, opts, rec, tok, &msgs);
    CHECK_FALSE(report.output.has_value());
    CHECK(report.ok());   // 不算失败——视频都出来了

    bool said = false;
    for (const auto& m : msgs) {
        const std::string s = m.dump();
        if (s.find("没有 ffmpeg") != std::string::npos &&
            s.find("shots/") != std::string::npos) {
            said = true;
        }
    }
    CHECK(said);
}


// ---------------------------------------------------------------------------
// 各阶段到底挑哪些镜头跑，和 Python 逐个比。
//
// 上面那些用例钉的是我们自己的意图——`contract_audit.py` 的工作单上
// 这个文件是最后一条。这一段把它补上。
//
// **挑错镜头是最难查的一类错。** 挑漏了，那一镜永远轮不到它:
// 不报错、不重试、日志里一行都没有，表现是"成片里少了一个镜头"，
// 而你会先怀疑分镜、怀疑渲染、怀疑装配，最后才想到是入口状态少写了一个。
// 挑多了则是白烧显卡：已经出好的镜头又渲一遍。
//
// 语料是**全枚举**的：每个 ShotStatus × 每个阶段 × force 开关。
// 期望值来自把 Python 那边的阶段类换成假货、跑真的 Pipeline 方法接出来的
// todo，不是照着源码抄的 predicate——抄歪了语料和代码会一起歪。
// ---------------------------------------------------------------------------

TEST_CASE("出首帧挑的是「没有能用首帧」的，不只是 AUDIO_DONE") {
    // **界面按有没有首帧文件数，引擎按状态挑，两边对不上。**
    //
    // 一镜首帧失败之后照样会被出片那一段收走（退回纯文生视频），于是它停在
    // FINAL_DONE / FALLBACK 而手里一张首帧都没有。界面上「只出首帧（差 N）」
    // 把它算进去了，引擎却一个都挑不到——点了跑完还是差 N，一句话都没有。
    //
    // 2026-09-13 全盘数过：203 个缺首帧的镜头里，12 个 FINAL_DONE、
    // 11 个 FALLBACK，都够不着。
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_挑首帧");
    std::error_code ec;
    fs::remove_all(root, ec);
    const models::ProjectPaths paths(root);
    fs::create_directories(paths.frames(), ec);

    auto add = [](models::Episode& ep, const std::string& id,
                  models::ShotStatus st, const char* frame,
                  const char* video) {
        models::Shot s;
        s.shot_id = id;
        s.scene_id = "sc01";
        s.order = static_cast<int>(ep.shots.size());
        s.status = st;
        if (frame) s.frame_path = frame;
        if (video) s.video_path = video;
        ep.shots.push_back(s);
    };

    models::Episode ep;
    ep.episode_id = "ep01";
    add(ep, "sh_audio",     models::ShotStatus::AUDIO_DONE, nullptr, nullptr);
    add(ep, "sh_final_ok",  models::ShotStatus::FINAL_DONE, "frames/ok.png", "v/a.mp4");
    add(ep, "sh_final_bad", models::ShotStatus::FINAL_DONE, nullptr, "v/b.mp4");
    add(ep, "sh_fallback",  models::ShotStatus::FALLBACK,   nullptr, nullptr);
    add(ep, "sh_planned",   models::ShotStatus::PLANNED,    nullptr, nullptr);
    add(ep, "sh_locked",    models::ShotStatus::LOCKED,     nullptr, "v/c.mp4");
    add(ep, "sh_dangling",  models::ShotStatus::FINAL_DONE, "frames/gone.png", "v/d.mp4");

    // 只有 ok.png 是真存在的；gone.png 记着但文件不在。
    std::ofstream(paths.frames() / paths::from_utf8("ok.png"), std::ios::binary)
        << "假的 PNG";

    const auto ids = [](const std::vector<models::Shot*>& v) {
        std::vector<std::string> out;
        for (const models::Shot* s : v) out.push_back(s->shot_id);
        return out;
    };

    const auto got = ids(pipeline::pick_for_frames(ep, paths, /*force=*/false));
    CAPTURE(got.size());

    SUBCASE("配音刚跑完的照旧收") {
        CHECK(std::find(got.begin(), got.end(), "sh_audio") != got.end());
    }
    SUBCASE("出过片但没首帧的要补上——这条就是那个 bug") {
        CHECK(std::find(got.begin(), got.end(), "sh_final_bad") != got.end());
        CHECK(std::find(got.begin(), got.end(), "sh_fallback") != got.end());
    }
    SUBCASE("首帧记着但文件不在，也要重出") {
        // 只看字段的话引擎以为有，出片时拿一个不存在的路径当起点。
        CHECK(std::find(got.begin(), got.end(), "sh_dangling") != got.end());
    }
    SUBCASE("已经有首帧的不动") {
        CHECK(std::find(got.begin(), got.end(), "sh_final_ok") == got.end());
    }
    SUBCASE("PLANNED 不收：时长还没锁") {
        // 配音失败的停在这里，带着估的时长。照它出首帧等于把错的时长
        // 焊进画面。界面上那个按钮发的是 ["audio","frames"]，配音那一段
        // 会先把它推到 AUDIO_DONE，同一轮里就收得到了。
        CHECK(std::find(got.begin(), got.end(), "sh_planned") == got.end());
    }
    SUBCASE("LOCKED 不收：人工确认过的不动") {
        CHECK(std::find(got.begin(), got.end(), "sh_locked") == got.end());
    }
    SUBCASE("force 之下谁都收") {
        const auto all = ids(pipeline::pick_for_frames(ep, paths, /*force=*/true));
        CHECK(all.size() == ep.shots.size());
    }
    SUBCASE("指定了镜头就只认这几个") {
        const auto one =
            ids(pipeline::pick_for_frames(ep, paths, false, {"sh_fallback"}));
        REQUIRE(one.size() == 1);
        CHECK(one[0] == "sh_fallback");
    }

    fs::remove_all(root, ec);
}

TEST_CASE("装配漏下谁要点名，不能只报一个数") {
    // **装配那道筛选本来是静默的。** 不可用的镜头直接不进片子，唯一的线索
    // 是「装配 16 个镜头」这个数——人得自己记得这一章有 18 镜才看得出来。
    // 2026-09-13 实机：walk_c ep01 两镜重跑过配音退回 audio_done，
    // 成片从 18 镜 61.8 秒变成 16 镜 54.3 秒，消息一个字都没提。
    //
    // 这一组同时钉住第一版的实现错：那一版把 `const Shot*` 存进表里，
    // 而 `sorted_shots()` 是按值返回的，出了 range-for 整张表全悬空，
    // 实机表现是 `std::bad_alloc` 直接打断装配。这里的 CHECK 读的就是
    // 循环之后的内容——真悬空了就读不出这些字。
    auto add = [](models::Episode& ep, const std::string& id,
                  models::ShotStatus st, const char* video) {
        models::Shot s;
        s.shot_id = id;
        s.scene_id = "sc01";
        s.order = static_cast<int>(ep.shots.size());
        s.status = st;
        if (video) s.video_path = video;
        ep.shots.push_back(s);
    };

    models::Episode ep;
    ep.episode_id = "ep01";
    add(ep, "sh_final",    models::ShotStatus::FINAL_DONE, "v/a.mp4");
    add(ep, "sh_draft",    models::ShotStatus::DRAFT_DONE, "v/b.mp4");
    add(ep, "sh_fallback", models::ShotStatus::FALLBACK,   "v/c.mp4");
    add(ep, "sh_locked",   models::ShotStatus::LOCKED,     "v/d.mp4");
    add(ep, "sh_audio",    models::ShotStatus::AUDIO_DONE, nullptr);
    add(ep, "sh_gone",     models::ShotStatus::FINAL_DONE, nullptr);
    add(ep, "sh_rejected", models::ShotStatus::FINAL_REJECTED, "v/e.mp4");

    SUBCASE("能进片子的四种状态") {
        CHECK(pipeline::assembly_usable(ep.shots[0]));
        CHECK(pipeline::assembly_usable(ep.shots[1]));
        CHECK(pipeline::assembly_usable(ep.shots[2]));
        CHECK(pipeline::assembly_usable(ep.shots[3]));
    }
    SUBCASE("状态够了但片子不在，一样不能进") {
        // 光看状态的话引擎以为有，装配时拿一个空路径去喂 ffmpeg。
        CHECK_FALSE(pipeline::assembly_usable(ep.shots[5]));
    }

    const auto out = pipeline::assembly_left_out(ep);

    SUBCASE("点的是没进去的那三个，不是全部") {
        REQUIRE(out.size() == 3);
        // **镜号单看一格，不从那句话里切。** 那句话会跟着语言换，冒号也
        // 跟着换；靠它回推镜号在别的语言下必错，见 `LeftOut` 上那段。
        CHECK(out[0].shot_id == "sh_audio");
        CHECK(out[1].shot_id == "sh_gone");
        CHECK(out[2].shot_id == "sh_rejected");
        CHECK(out[0].line.rfind("sh_audio：", 0) == 0);
        CHECK(out[1].line.rfind("sh_gone：", 0) == 0);
        CHECK(out[2].line.rfind("sh_rejected：", 0) == 0);
    }
    SUBCASE("说的是人话，不是枚举名") {
        // 这句是给人看的。status_zh 存在就是为了这一条。
        REQUIRE(out.size() == 3);
        CHECK(out[0].line.find("配音完成，还没出片") != std::string::npos);
        CHECK(out[0].line.find("audio_done") == std::string::npos);
        CHECK(out[2].line.find("成片未过闸门") != std::string::npos);
    }
    SUBCASE("片子不在要单说——和状态卡住不是一回事") {
        REQUIRE(out.size() == 3);
        CHECK(out[1].line.find("还没有视频文件") != std::string::npos);
        CHECK(out[2].line.find("还没有视频文件") == std::string::npos);
    }
    SUBCASE("全都能进就一个字都不说") {
        models::Episode all_ok;
        all_ok.episode_id = "ep02";
        add(all_ok, "a", models::ShotStatus::FINAL_DONE, "v/a.mp4");
        add(all_ok, "b", models::ShotStatus::LOCKED, "v/b.mp4");
        CHECK(pipeline::assembly_left_out(all_ok).empty());
    }
}

TEST_CASE("各阶段挑哪些镜头跑，和 Python 一样") {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/episode_pick.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    nlohmann::json g;
    in >> g;

    const auto statuses = g.at("statuses").get<std::vector<std::string>>();
    REQUIRE(statuses.size() >= 9);

    // 照语料的顺序造镜头，order 倒着编——数组顺序和 order 反着来，
    // 这样"按 order 排"和"按数组位置排"给出的答案不一样。
    auto build = [&]() {
        models::Episode ep;
        ep.episode_id = "ep01";
        const int n = static_cast<int>(statuses.size());
        for (int i = 0; i < n; ++i) {
            models::Shot s;
            s.shot_id = "sh_" + statuses[i];
            s.scene_id = "sc01";
            s.order = n - i;
            s.duration_s = 4.0;
            s.video_path = "video/" + statuses[i] + ".mp4";
            bool ok = false;
            for (int v = 0; v <= static_cast<int>(models::ShotStatus::LOCKED); ++v) {
                const auto st = static_cast<models::ShotStatus>(v);
                if (models::to_string(st) == statuses[i]) {
                    s.status = st;
                    ok = true;
                    break;
                }
            }
            // 语料里有个状态 C++ 侧认不出来，就是两边的枚举对不上了——
            // 那正是"挑漏一个状态"的源头，不能当成没看见
            REQUIRE_MESSAGE(ok, "C++ 侧没有这个状态：" << statuses[i]);
            ep.shots.push_back(s);
        }
        return ep;
    };

    for (const auto& row : g.at("stages")) {
        const std::string stage = row.at("stage").get<std::string>();
        const bool force = row.at("force").get<bool>();
        CAPTURE(stage);
        CAPTURE(force);

        std::set<models::ShotStatus> want;
        if (stage == "audio") {
            want = {models::ShotStatus::PLANNED};
        } else if (stage == "frames") {
            want = {models::ShotStatus::AUDIO_DONE};
        } else if (stage == "render_draft") {
            want = pipeline::render_entry_states(models::Tier::DRAFT);
        } else if (stage == "render_final") {
            want = pipeline::render_entry_states(models::Tier::FINAL);
        } else {
            FAIL("语料里有没认过的阶段：" << stage);
        }

        auto ep = build();
        const auto got = pipeline::pick(ep, want, force);

        std::vector<std::string> got_ids;
        for (const models::Shot* s : got) got_ids.push_back(s->shot_id);
        const auto want_ids = row.at("picked").get<std::vector<std::string>>();

        // 顺序也比：画面的连贯性是按 order 来的，挑对了但顺序错了
        // 一样出问题（而且更难看出来）
        CHECK(got_ids == want_ids);
    }
}

TEST_CASE("pick 返回的是指针，改了状态要能落到那一章上") {
    // 这一条不比 Python，钉的是 C++ 自己的一个坑：Python 那边
    // episode.sorted_shots() 返回的是同一批对象的引用，而 C++ 侧那个
    // 函数返回的是**拷贝**。照抄名字的话所有状态改动都写进临时对象，
    // 存盘时一个字段都没变——而且全程不报错。
    models::Episode ep;
    models::Shot a;
    a.shot_id = "sh001";
    a.order = 1;
    a.status = models::ShotStatus::PLANNED;
    ep.shots.push_back(a);

    auto todo = pipeline::pick(ep, {models::ShotStatus::PLANNED}, false);
    REQUIRE(todo.size() == 1);
    todo[0]->status = models::ShotStatus::AUDIO_DONE;

    CHECK(ep.shots[0].status == models::ShotStatus::AUDIO_DONE);
}

TEST_CASE("跳过草稿档时，成片档要收首帧刚做完的那批") {
    // 挂 Turbo LoRA 之后两档画质拉不开差距，草稿档就是白跑一遍。
    // 但**跳过它有个坑**：镜头状态停在 FRAME_DONE，而成片档原来只收
    // DRAFT_DONE——一个镜头都挑不到，还不报错，表现是"跑完了什么都没出"。
    const auto normal = pipeline::render_entry_states(models::Tier::FINAL, false);
    CHECK(normal.count(models::ShotStatus::DRAFT_DONE) == 1);
    CHECK(normal.count(models::ShotStatus::FRAME_DONE) == 0);

    const auto skipped = pipeline::render_entry_states(models::Tier::FINAL, true);
    CHECK(skipped.count(models::ShotStatus::DRAFT_DONE) == 1);
    CHECK(skipped.count(models::ShotStatus::FRAME_DONE) == 1);
    // 首帧失败那批（状态停在 AUDIO_DONE）也要收，否则整章卡在它们身上
    CHECK(skipped.count(models::ShotStatus::AUDIO_DONE) == 1);

    // 草稿档自己不受这个开关影响
    for (const bool skip : {true, false}) {
        const auto d = pipeline::render_entry_states(models::Tier::DRAFT, skip);
        CHECK(d.count(models::ShotStatus::FRAME_DONE) == 1);
        CHECK(d.count(models::ShotStatus::DRAFT_DONE) == 0);
    }
}

TEST_CASE("整条链走一遍：首帧步数不会被 Turbo 压两次") {
    // **单看 effective_spec 是对的，合起来是错的。**
    //
    // test_models_config 里那几条一直绿着（`effective_spec(s, 28)
    // .frame_steps == 28`），因为它们给的是表里的原始值。可实际调用链上
    // 传进去的已经不是原始值了：
    //
    //   Runtime::profile()  —— 先用 effective_spec 把 tiers[FINAL].steps
    //                          换成实跑的 6，好让 /api/hardware 和磁盘上
    //                          的成片对得上（commit f5cae4d）
    //   apply_project_spec  —— 拿这个 6 当"表里的值"再算一次
    //
    // final_steps 那一支幂等（turbo 恒给 6），frame_steps 不幂等，于是
    // 首帧从 20 步塌成 6 步。出图那一步没有 Turbo LoRA，6 步就是裸跑，
    // 首帧糊；而首帧是每一镜的起始图和跨镜头一致性的锚点。
    // 2026-09-13 实机撞到：进度条写着"出首帧 ep01_sh003（第 1/6 步）"。
    models::HardwareProfile profile;
    profile.tiers[models::Tier::DRAFT] = {models::Tier::DRAFT, 512, 288, 10};
    // Runtime::profile() 交出来的就是这个形状：steps 已经压过，
    // 原始值另存在 table_final_steps 里。
    profile.tiers[models::Tier::FINAL] = {models::Tier::FINAL, 544, 928, 6};
    profile.table_final_steps = 20;

    config::Settings s;
    REQUIRE(s.models.frame_steps == 0);        // 没人显式填
    s.models.video_lora = "";                  // 不挂 Turbo，省得去摸文件

    pipeline::apply_project_spec(s, profile);

    CHECK(s.models.frame_steps == 20);         // 不是 6
    CHECK(pipeline::frame_spec(profile, s).steps == 20);

    SUBCASE("再套一次也还是 20") {
        // 幂等：同一个 profile 被 apply 两遍不该越走越小。
        pipeline::apply_project_spec(s, profile);
        CHECK(s.models.frame_steps == 20);
    }

    SUBCASE("没记原始值的老 profile 退回读 tiers") {
        models::HardwareProfile old;
        old.tiers[models::Tier::FINAL] = {models::Tier::FINAL, 544, 928, 20};
        config::Settings s2;
        pipeline::apply_project_spec(s2, old);
        CHECK(s2.models.frame_steps == 20);
    }
}

TEST_CASE("首帧的规格：默认跟成片档的画幅，但不跟 Turbo 压出来的步数") {
    // 两条都**不报错**，只让出来的图"看着不太行"，所以钉在这儿。
    //
    // 1. 画幅（[video]）只盖成片档。首帧走草稿档 = 512×288 的锚点
    //    配 704×1280 的视频，放大两倍再用。
    // 2. 成片档的步数会被 Turbo LoRA 压到 6，但那个 LoRA 只挂在视频
    //    模型上。首帧跟着变成 6 步裸跑就糊了。
    models::HardwareProfile profile;
    profile.tiers[models::Tier::DRAFT] = {models::Tier::DRAFT, 512, 288, 12};
    profile.tiers[models::Tier::FINAL] = {models::Tier::FINAL, 704, 1280, 6};

    config::Settings s;
    REQUIRE(s.models.frame_tier == "final");   // 默认就该是成片档
    s.models.frame_steps = 28;                 // run.cpp 压步数前存下来的

    const auto spec = pipeline::frame_spec(profile, s);
    CHECK(spec.width == 704);
    CHECK(spec.height == 1280);
    CHECK(spec.steps == 28);   // 不是 6

    // 没存过就跟档位表里的数走
    config::Settings bare;
    CHECK(pipeline::frame_spec(profile, bare).steps == 6);

    // 小卡上有意降到草稿档
    config::Settings draft;
    draft.models.frame_tier = "draft";
    CHECK(pipeline::frame_spec(profile, draft).width == 512);
    CHECK(pipeline::frame_spec(profile, draft).steps == 12);

    // 档位表缺一档也得出得来东西，不能抛——档位表是推出来的
    models::HardwareProfile only_draft;
    only_draft.tiers[models::Tier::DRAFT] = {models::Tier::DRAFT, 512, 288, 12};
    CHECK(pipeline::frame_spec(only_draft, s).width == 512);
    CHECK(pipeline::frame_spec(models::HardwareProfile{}, s).width == 0);
}
