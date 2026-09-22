// POST /api/run 的测试。
//
// 后端是注入的，所以整个队列几毫秒跑完。这里测的不是画面，是**编排**：
// 谁能开跑、开不了跑时回什么码、一章挂了后面几章还跑不跑、
// 阶段名写错了错误落在哪儿。最后一条尤其容易做错——做成 400 的话
// 前端弹的是错误框，而 Python 那边它是任务列表里的一条失败记录。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

#include "http/run.hpp"
#include "models/character.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_开跑_" + tag);
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
    s.duration_s = 4.0;
    return s;
}

/// 建一个项目：episodes 里每一项是 {episode_id, 镜头数}。镜头数 0 表示没分镜。
models::ProjectStore make_store(
    const std::string& tag,
    const std::vector<std::pair<std::string, int>>& episodes) {
    const fs::path root = temp_root(tag);
    auto store = models::ProjectStore::create(root, "yu_ye", "雨夜天台");

    models::Project project = store.load_project();
    for (const auto& [id, n] : episodes) {
        models::Episode ep;
        ep.episode_id = id;
        ep.title = id;
        for (int i = 0; i < n; ++i) {
            ep.shots.push_back(make_shot(id + "_sh" + std::to_string(i + 1), i));
        }
        project.episodes.push_back(ep);
    }
    store.save_project(project);

    models::AssetLibrary assets;
    assets.style.global_style = "电影感";
    assets.style.aspect_ratio = "9:16";
    store.save_assets(assets);
    return store;
}

/// 记下每个后端被调了几次，顺便能装成失败。
struct Fakes {
    std::vector<std::string> frames;
    std::vector<std::string> videos;

    static void stub(const fs::path& dest) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary);
        out << "假的";
    }

    http::RunDeps deps() {
        http::RunDeps d;
        d.settings = [] { return config::Settings{}; };
        d.profile = [] {
            models::HardwareProfile p;
            p.vram_gb = 6.0;
            p.tiers = models::tiers_for_vram(6.0);
            p.detected = true;
            return p;
        };
        d.backends = [this](const config::Settings&,
                            const models::ProjectStore&) {
            pipeline::Backends b;
            b.frame = [this](const models::Shot& s, const stages::PromptBundle&,
                             const models::TierSpec&, const fs::path& dest,
                             pipeline::CancelToken&, const infer::StepCallback&) {
                frames.push_back(s.shot_id);
                stub(dest);
            };
            b.video = [this](const models::Shot& s, const stages::RenderPlan&,
                             const std::optional<fs::path>&, const fs::path& dest,
                             pipeline::CancelToken&, const infer::StepCallback&) {
                videos.push_back(s.shot_id);
                stub(dest);
            };
            return b;
        };
        return d;
    }
};

/// 开跑并等它结束。任务跑在 job 表的线程上，不等的话下一条用例会撞 409。
http::ApiResult run_and_wait(const json& body, Fakes& fakes) {
    const auto r = http::post_run(body, fakes.deps());
    pipeline::jobs().wait_idle();
    return r;
}

/// 上一条用例留下的任务收干净了再开始。
///
/// **队列也要清。** 「点了就排队」之后，上一条用例排下的那件会在槽一空
/// 出来就自己起跑——下一条用例于是撞在一件它根本不知道的活上，而且是
/// 随机时序（单独跑绿、一起跑红）。
void quiesce() {
    http::post_run_queue_clear();
    pipeline::jobs().wait_idle();
    http::post_run_queue_clear();
}

std::string project_arg(const models::ProjectStore& store) {
    return paths::to_utf8(store.root());
}

}  // namespace

TEST_CASE("开跑立刻返回，把队列一起给出来") {
    // 同步跑完再返回的话，前端那个请求要挂几十分钟，中途还会被代理掐断。
    quiesce();
    const auto store = make_store("开跑", {{"ep01", 2}});
    Fakes fakes;

    const auto r = run_and_wait(
        {{"project", project_arg(store)}, {"episode_id", "ep01"}}, fakes);

    CHECK(r.status == 200);
    CHECK(r.body["started"] == true);
    CHECK(r.body["queue"] == json::array({"ep01"}));
    CHECK(fakes.frames.size() == 2);
    // **两镜各一次，不是各两次。** 草稿档默认不跑了——挂 Turbo LoRA
    // 之后两档画质拉不开差距，那一遍是白跑（一章 54 分钟）。
    // 想要两档的显式传 skip_draft: false，下面那条用例钉的就是它。
    CHECK(fakes.videos.size() == 2);
}

TEST_CASE("草稿档：默认不跑，显式要才跑") {
    // 这条把"默认值是什么"钉死。默认改过一次（2026-09-10），
    // 而默认值这种东西改了不会有任何编译错误——只有用例会红。
    quiesce();
    const auto store = make_store("草稿默认", {{"ep01", 2}});
    Fakes fakes;

    SUBCASE("默认：只出成片档") {
        run_and_wait({{"project", project_arg(store)}, {"episode_id", "ep01"}},
                     fakes);
        CHECK(fakes.videos.size() == 2);
    }
    SUBCASE("显式要草稿档：两档都出") {
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"skip_draft", false}},
                     fakes);
        CHECK(fakes.videos.size() == 4);
    }
}

TEST_CASE("已经在跑时排队，不是把人赶走") {
    // 用户 2026-09-20：「如果有新点击不是叫用户等而是加入队列」。
    //
    // 原来这儿回 409「已经在跑 ep01 了」——人要做的事变成"记着这件事、
    // 过二十分钟回来再点一次"，而那正是机器该替他记的。
    //
    // 排上之后要钉住两件事：**这一下一个镜头都不许碰**（排着不等于开跑），
    // 以及**说得出排第几个**（「排上了」而不说位置，和「知道了」没区别）。
    quiesce();
    const auto store = make_store("排队", {{"ep01", 1}});

    // 占住槽：这个任务等到我们放行才结束
    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress&) {
                               while (!release.load()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(2));
                               }
                           });

    Fakes fakes;
    const json body{{"project", project_arg(store)}, {"episode_id", "ep01"}};
    const auto first = http::post_run(body, fakes.deps());
    CHECK(first.status == 202);
    CHECK(first.body.at("queued") == true);
    CHECK(first.body.at("started") == false);
    CHECK(first.body.at("position") == 1);
    // 在跑的是哪一章还是要说——只说"排上了"的话，人不知道前面是什么。
    CHECK(first.body.at("running") == "ep01");
    CHECK(fakes.frames.empty());   // **排着不等于开跑**

    // **一模一样的不重复排**：手快点两下是常事，而第二遍什么都不会做。
    const auto again = http::post_run(body, fakes.deps());
    CHECK(again.status == 202);
    CHECK(again.body.at("position") == 1);

    // 队列问得出来，而且标清是哪一部电影的
    const auto status = http::get_run_status(project_arg(store));
    CHECK(status.body.at("queue").at("total") == 1);
    CHECK(status.body.at("queue").at("items")[0].at("mine") == true);
    // 正在跑的那一轮不是这个项目起的（上面是直接占槽的假任务，没带项目），
    // 所以 mine 是假——页面靠它决定要不要显示「停下」。
    CHECK(status.body.at("mine") == false);

    // 清得掉：点错了要能撤
    CHECK(http::post_run_queue_clear().body.at("cleared") == 1);
    CHECK(http::get_run_status("").body.at("queue").at("total") == 0);

    release = true;
    pipeline::jobs().wait_idle();
    quiesce();
    CHECK(fakes.frames.empty());
}

TEST_CASE("排着的那件，槽一空出来就自己跑起来") {
    // 排队的全部意义在这一条上：**人不用回来再点一次**。
    quiesce();
    const auto store = make_store("轮到它", {{"ep01", 1}});

    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress&) {
                               while (!release.load()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(2));
                               }
                           });

    Fakes fakes;
    const auto queued = http::post_run(
        {{"project", project_arg(store)}, {"episode_id", "ep01"}}, fakes.deps());
    REQUIRE(queued.status == 202);

    release = true;
    // 前一件跑完 → 钩子 → Offload → 起下一件。等它真跑完。
    pipeline::jobs().wait_idle();
    for (int i = 0; i < 400 && fakes.frames.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        pipeline::jobs().wait_idle();
    }
    CHECK_FALSE(fakes.frames.empty());   // ← 没有钩子的话这儿永远是空的
    quiesce();
}

TEST_CASE("不认识的键照收，但要列回去") {
    // **不 forbid 是有意的**：前端和引擎的版本不一定同步升，多一个键就 422
    // 会让整个功能挂掉。但默不作声的代价是真的——2026-09-13 我把 shot_ids
    // 写成 only_shots，引擎当成"没指定镜头"，于是「重出这一镜」变成整章
    // 重渲 18 镜、跑了二十分钟，而我一直以为它只渲了一镜。
    quiesce();
    Fakes fakes;
    const auto store = make_store("未知字段", {{"ep01", 2}});

    const auto r = run_and_wait(
        json{{"project", project_arg(store)},
             {"episode_id", "ep01"},
             {"stages", json::array({"audio"})},
             {"only_shots", json::array({"ep01_sh001"})}},   // 写错的那个
        fakes);

    CHECK(r.status == 200);
    CHECK(r.body.at("started") == true);
    REQUIRE(r.body.contains("ignored_fields"));
    const auto ig = r.body.at("ignored_fields").get<std::vector<std::string>>();
    REQUIRE(ig.size() == 1);
    CHECK(ig[0] == "only_shots");

    SUBCASE("全是认识的键就不带这一项——正常用法下必须是静默的") {
        quiesce();
        Fakes f2;
        const auto ok = run_and_wait(
            json{{"project", project_arg(store)},
                 {"episode_id", "ep01"},
                 {"force", true},
                 {"shot_ids", json::array({"ep01_sh001"})},
                 {"stages", json::array({"audio"})}},
            f2);
        CHECK(ok.status == 200);
        CHECK_FALSE(ok.body.contains("ignored_fields"));
    }
}

TEST_CASE("必填字段缺了是 422，不是 400") {
    // pydantic 的校验错误是 422，而且 detail 是数组不是字符串。
    // 回成 400 加一句话的话，前端拿到的形状不对，错误提示会是空的。
    quiesce();
    Fakes fakes;

    SUBCASE("没有 project") {
        try {
            http::post_run({{"episode_id", "ep01"}}, fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 422);
            const auto& d = e.detail();
            REQUIRE(d.contains("detail"));
            CHECK(d["detail"][0]["loc"] == json::array({"body", "project"}));
            CHECK(d["detail"][0]["type"] == "missing");
        }
    }

    SUBCASE("没有 episode_id") {
        // all_episodes 会忽略 episode_id，但字段本身仍然是必填的——
        // Python 那边它没有默认值。
        try {
            http::post_run({{"project", "C:/x"}, {"all_episodes", true}},
                           fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 422);
            CHECK(e.detail()["detail"][0]["loc"] ==
                  json::array({"body", "episode_id"}));
        }
    }
}

TEST_CASE("项目路径为空或者读不出来是 400") {
    quiesce();
    Fakes fakes;

    SUBCASE("空路径") {
        try {
            http::post_run({{"project", ""}, {"episode_id", "ep01"}},
                           fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 400);
            CHECK(std::string(e.what()).find("项目目录") != std::string::npos);
        }
    }

    SUBCASE("目录不是项目") {
        const fs::path empty = temp_root("空目录");
        try {
            http::post_run(
                {{"project", paths::to_utf8(empty)}, {"episode_id", "ep01"}},
                fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 400);
        }
    }
}

TEST_CASE("all_episodes 只排有分镜的章") {
    // 没分镜的章排进去只会在任务里报一条"还没有分镜表"，
    // 而用户点的是"跑整个项目"，看到一串失败会以为整个项目坏了。
    quiesce();
    const auto store =
        make_store("全片", {{"ep01", 1}, {"ep02", 0}, {"ep03", 2}});
    Fakes fakes;

    const auto r = run_and_wait({{"project", project_arg(store)},
                                 {"episode_id", "ep99"},   // 被忽略
                                 {"all_episodes", true},
                                 {"skip_final", true}},
                                fakes);

    CHECK(r.body["queue"] == json::array({"ep01", "ep03"}));
    CHECK(fakes.frames.size() == 3);

    SUBCASE("队列进度在快照里") {
        const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
        CHECK(snap["queue_total"] == 2);
        CHECK(snap["queue_done"] == 2);
    }
}

TEST_CASE("all_episodes 但一章分镜都没有时 400") {
    quiesce();
    const auto store = make_store("空项目", {{"ep01", 0}});
    Fakes fakes;
    try {
        http::post_run({{"project", project_arg(store)},
                        {"episode_id", "ep01"},
                        {"all_episodes", true}},
                       fakes.deps());
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
        CHECK(std::string(e.what()).find("分镜表") != std::string::npos);
    }
}

TEST_CASE("一章出错不拖垮后面几章") {
    // 量产时跑一晚上，早上发现第二章挂了导致后面十章都没动，
    // 那这一晚上就白熬了。
    quiesce();
    const auto store = make_store("挂一集", {{"ep01", 1}, {"ep02", 1}});
    {
        // 把 ep02 的分镜清掉，让它在任务里抛"还没有分镜表"
        models::Project p = store.load_project();
        p.episode_by_id("ep02")->shots.clear();
        store.save_project(p);
    }
    Fakes fakes;

    // 显式给队列：all_episodes 会把没分镜的过滤掉，这里要的正是它不被过滤
    const auto r1 = run_and_wait({{"project", project_arg(store)},
                                  {"episode_id", "ep02"},
                                  {"skip_final", true}},
                                 fakes);
    CHECK(r1.status == 200);

    const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    REQUIRE(snap["error"].is_string());
    const std::string err = snap["error"];
    CAPTURE(err);
    CHECK(err.find("ep02") != std::string::npos);
    CHECK(err.find("分镜表") != std::string::npos);
    // 失败也算跑完一章，队列要往前走，否则进度条永远停在那儿
    CHECK(snap["queue_done"] == 1);
}

TEST_CASE("stages 空数组走全流程，给了内容才只跑那几个") {
    // Python 判的是 `if req.stages:`——空列表是假值。
    // 把两者合并成"空就是全跑"会让 stages:["  "] 变成重跑整章，
    // 而那是几十分钟的差别。
    quiesce();

    SUBCASE("空数组 = 全跑") {
        const auto store = make_store("空阶段", {{"ep01", 1}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array()}},
                     fakes);
        CHECK(fakes.frames.size() == 1);
        // 一镜一次：草稿档默认不跑
        CHECK(fakes.videos.size() == 1);
    }

    SUBCASE("只跑配音和首帧") {
        // 首帧的入口状态是 AUDIO_DONE，所以配音要一起跑。
        // 只给 frames 的话一个镜头都挑不出来——那正是下一条用例钉的。
        const auto store = make_store("只首帧", {{"ep01", 2}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array({"audio", "frames"})}},
                     fakes);
        CHECK(fakes.frames.size() == 2);
        CHECK(fakes.videos.empty());
    }

    SUBCASE("跳过配音直接跑首帧，一个镜头都挑不出来") {
        // **这是有意的。** 时长决定帧数，帧数决定画面。跳过配音的话
        // 镜头时长还是分镜给的估算值，配音出来装不进去。
        const auto store = make_store("跳配音", {{"ep01", 2}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array({"frames"})}},
                     fakes);
        CHECK(fakes.frames.empty());
    }

    SUBCASE("全是空白的阶段名 = 一个都不跑") {
        const auto store = make_store("空白阶段", {{"ep01", 1}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array({"  "})}},
                     fakes);
        CHECK(fakes.frames.empty());
        CHECK(fakes.videos.empty());
    }
}

TEST_CASE("阶段名写错时错在任务里，不是 400") {
    // Python 那边这个校验在 run_stages 内部，也就是在任务线程上。
    // 做成 400 的话前端弹的是错误框，而现在的行为是任务列表里一条失败记录。
    quiesce();
    const auto store = make_store("错阶段", {{"ep01", 1}});
    Fakes fakes;

    const auto r = run_and_wait({{"project", project_arg(store)},
                                 {"episode_id", "ep01"},
                                 {"stages", json::array({"render"})}},
                                fakes);
    CHECK(r.status == 200);   // 开跑本身是成功的

    const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    REQUIRE(snap["error"].is_string());
    const std::string err = snap["error"];
    CAPTURE(err);
    CHECK(err.find("不认识的阶段 render") != std::string::npos);
    // 要说清有哪些可选的，不然用户只能去翻文档
    CHECK(err.find("assemble、audio、draft、final、frames") != std::string::npos);
    CHECK(fakes.frames.empty());
}

TEST_CASE("阶段名两边的空白要去掉") {
    // 前端从输入框里拿的字符串常带空格，而这个字段现在没有下拉约束。
    const auto only = http::parse_stages({" frames ", "draft"});
    REQUIRE(only.size() == 2);
    CHECK(only[0] == pipeline::Stage::Frames);
    CHECK(only[1] == pipeline::Stage::Draft);
}

TEST_CASE("多余的键要忽略，不能 422") {
    // RunRequest 是个普通的 BaseModel，pydantic 默认忽略多余字段。
    // 这里 forbid 的话，前端多传一个键整个请求就挂了——
    // 而前端和后端的版本不一定同步升。
    quiesce();
    const auto store = make_store("多余键", {{"ep01", 1}});
    Fakes fakes;

    const auto r = run_and_wait({{"project", project_arg(store)},
                                 {"episode_id", "ep01"},
                                 {"skip_final", true},
                                 {"这个键后端不认识", 42}},
                                fakes);
    CHECK(r.status == 200);
    CHECK(fakes.frames.size() == 1);
}

// ---- GET /api/run/preview ----
//
// 预演的价值在于**报大不报小**。报小了的预演比没有预演更糟：
// 人以为十几秒，走开了，回来发现还在跑第三章。

namespace {

/// 反斜杠。写成字面量的话，这个文件经过的每一层转义都可能吃掉一次。
constexpr char kBackslash = static_cast<char>(92);

models::HardwareProfile preview_profile(bool measured) {
    models::HardwareProfile p;
    p.vram_gb = 6.0;
    p.tiers = models::tiers_for_vram(6.0);
    p.detected = true;
    if (measured) {
        p.tiers[models::Tier::DRAFT].measured_seconds = 30.0;
        p.tiers[models::Tier::FINAL].measured_seconds = 300.0;
    } else {
        // tiers_for_vram 是**一定**会填 measured_seconds 的（按显存推的
        // 静态表），所以"没标定"要手动造出来。真会出现的场合是配置里
        // 手写了档位参数、而那台机器还没跑过一镜。
        for (auto& [tier, spec] : p.tiers) spec.measured_seconds.reset();
    }
    return p;
}

/// 找 stages 里某一阶段的镜头数。没有那一项返回 -1。
int stage_shots(const nlohmann::json& body, const std::string& name) {
    for (const auto& s : body.at("stages")) {
        if (s.at("stage") == name) return s.at("shots").get<int>();
    }
    return -1;
}

void set_status(const models::ProjectStore& store, const std::string& shot_id,
                models::ShotStatus st) {
    models::Project p = store.load_project();
    for (auto& ep : p.episodes) {
        if (models::Shot* s = ep.shot_by_id(shot_id)) s->status = st;
    }
    store.save_project(p);
}

}  // namespace

TEST_CASE("预演：一个镜头会一路走完后面所有阶段") {
    // 只按当前状态归到一个阶段的话，会告诉人"配音 2 镜，粗估 16 秒"，
    // 而实际上那两镜还要出首帧、跑草稿、跑成片，得等十几分钟。
    const auto store = make_store("预演", {{"ep01", 2}});

    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, /*skip_draft=*/false, false,
                                         preview_profile(false));
    REQUIRE(r.status == 200);
    // 两镜都是 PLANNED，四个阶段各两镜
    CHECK(stage_shots(r.body, "audio") == 2);
    CHECK(stage_shots(r.body, "frames") == 2);
    CHECK(stage_shots(r.body, "draft") == 2);
    CHECK(stage_shots(r.body, "final") == 2);
    CHECK(r.body["shots"] == 2);
    CHECK(r.body["idle"] == false);
    CHECK(r.body["episodes"] == json::array({"ep01"}));
}

TEST_CASE("预演：已完成的镜头不算进去") {
    const auto store = make_store("预演跳过", {{"ep01", 3}});
    set_status(store, "ep01_sh1", models::ShotStatus::FINAL_DONE);
    set_status(store, "ep01_sh2", models::ShotStatus::FRAME_DONE);

    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, /*skip_draft=*/false, false,
                                         preview_profile(false));
    // sh1 完成了，sh2 从草稿开始，sh3 从配音开始
    CHECK(stage_shots(r.body, "audio") == 1);
    CHECK(stage_shots(r.body, "frames") == 1);
    CHECK(stage_shots(r.body, "draft") == 2);
    CHECK(stage_shots(r.body, "final") == 2);

    SUBCASE("全完成时 idle 为真，stages 是空的") {
        set_status(store, "ep01_sh2", models::ShotStatus::FINAL_DONE);
        set_status(store, "ep01_sh3", models::ShotStatus::FINAL_DONE);
        const auto r2 = http::get_run_preview(project_arg(store), "ep01", false,
                                              false, /*skip_draft=*/false,
                                              false, preview_profile(false));
        CHECK(r2.body["idle"] == true);
        CHECK(r2.body["stages"].empty());
        // 没事可做时不报时间，报"0 秒"会让人以为是估算坏了
        CHECK(r2.body["estimate_text"] == "");
        CHECK(r2.body["estimate_s"] == 0);
    }
}

TEST_CASE("预演：人工确认过的镜头不重跑，除非明确要求") {
    // LOCKED 是用户说"这一镜就这样了"。force 之外任何情况下动它，
    // 都等于把人工确认的结果覆盖掉。
    const auto store = make_store("锁定", {{"ep01", 2}});
    set_status(store, "ep01_sh1", models::ShotStatus::LOCKED);

    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, /*skip_draft=*/false, false,
                                         preview_profile(false));
    CHECK(stage_shots(r.body, "audio") == 1);

    SUBCASE("force 时连锁定的也算") {
        const auto r2 = http::get_run_preview(project_arg(store), "ep01", false,
                                              false, /*skip_draft=*/false,
                                              true, preview_profile(false));
        CHECK(stage_shots(r2.body, "audio") == 2);
        CHECK(stage_shots(r2.body, "final") == 2);
    }
}

TEST_CASE("预演：skip_final 时不算成片档") {
    const auto store = make_store("跳成片预演", {{"ep01", 2}});
    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         true, /*skip_draft=*/false, false,
                                         preview_profile(true));
    CHECK(stage_shots(r.body, "final") == -1);   // 整项都不出现
    CHECK(stage_shots(r.body, "draft") == 2);

    // 时间里也不能含成片档：2*30 草稿 + 2*8 配音 + 2*12 首帧 = 100
    CHECK(r.body["estimate_s"] == 100);
    CHECK(r.body["estimate_text"] == "2 分钟");
}

TEST_CASE("预演：成片档要按项目自己的画幅折算，不能照抄档位表") {
    // 真跑那条路开跑前会调 config::effective_spec，把画幅换成项目
    // changji.toml 里 [video] 那一档、把步数换成 Turbo 的。预演要是
    // 照抄档位表里的数字，报出来的时间能和实际差一个数量级——
    // 实测过一次：预演说"2.1 小时"，实际跑完九分钟。
    //
    // 这里不钉死具体秒数（它跟着档位表和 round32 走，改表就得改这儿），
    // 钉的是**同一批镜头、只把画幅从 720p 换成 2k，报出来的时间必须变长**。
    const auto store = make_store("预演画幅", {{"ep01", 2}});

    const auto at_720p = http::get_run_preview(project_arg(store), "ep01",
                                               false, false,
                                               /*skip_draft=*/true, false,
                                               preview_profile(true));

    // 项目级配置盖在全局上，[video] 就写在这儿——和
    // POST /bff/project/video 落的是同一个文件。
    {
        std::ofstream toml(store.root() / "changji.toml");
        toml << "[video]\n";
        toml << "orientation = \"portrait\"\n";
        toml << "quality = \"2k\"\n";
    }

    const auto at_2k = http::get_run_preview(project_arg(store), "ep01", false,
                                             false, /*skip_draft=*/true, false,
                                             preview_profile(true));

    const auto s720 = at_720p.body["estimate_s"].get<long long>();
    const auto s2k = at_2k.body["estimate_s"].get<long long>();
    CHECK(s720 > 0);
    // 2k 是 1440×2560，720p 是 544×928——像素数差七倍多，
    // 时间不可能一样。一样就说明这一处又在照抄档位表了。
    CHECK(s2k > s720);
}

TEST_CASE("预演：没标定过就只算配音和首帧那部分") {
    // 档位表里没有实测耗时时 estimate_episode 回空。
    // 那时候不能把渲染当成 0 秒——但也没有别的数可报，
    // 所以报出来的是一个偏小的数，这一条钉的是"至少不为零"。
    const auto store = make_store("没标定", {{"ep01", 1}});
    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, /*skip_draft=*/false, false,
                                         preview_profile(false));
    CHECK(r.body["estimate_s"] == 20);   // 8 + 12
    CHECK(r.body["estimate_text"] == "20 秒");
}

TEST_CASE("预演：all_episodes 只看有分镜的章") {
    const auto store =
        make_store("预演全片", {{"ep01", 1}, {"ep02", 0}, {"ep03", 2}});
    const auto r = http::get_run_preview(project_arg(store), "", true, false,
                                         /*skip_draft=*/false, false,
                                         preview_profile(false));
    CHECK(r.body["episodes"] == json::array({"ep01", "ep03"}));
    CHECK(r.body["shots"] == 3);
}

TEST_CASE("预演：没有可跑的章时 400") {
    const auto store = make_store("预演空", {{"ep01", 1}});
    try {
        http::get_run_preview(project_arg(store), "ep99", false, false, false, false,
                              preview_profile(false));
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
        CHECK(std::string(e.what()).find("先出分镜") != std::string::npos);
    }
}

// ---- GET /api/outputs ----

TEST_CASE("成片列表：没有 output 目录时回空数组") {
    // 回 404 或者报错的话，一个刚建好还没跑过的项目一打开就是红的。
    const auto store = make_store("没成片", {{"ep01", 1}});
    std::error_code ec;
    fs::remove_all(store.paths().output(), ec);

    const auto r = http::get_outputs(project_arg(store));
    CHECK(r.status == 200);
    CHECK(r.body["files"] == json::array());
}

TEST_CASE("成片列表：新的在前，只列 mp4") {
    const auto store = make_store("成片", {{"ep01", 1}});
    const fs::path out = store.paths().output();
    std::error_code ec;
    fs::create_directories(out, ec);

    const auto write = [&](const std::string& name, std::size_t bytes) {
        std::ofstream f(out / paths::from_utf8(name), std::ios::binary);
        f << std::string(bytes, 'x');
    };
    write("第一集.mp4", 3 * 1024 * 1024);
    write("说明.txt", 10);
    // 时间要拉开，不然两个文件同一秒，顺序就成了目录遍历顺序
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write("第二集.mp4", 1536 * 1024);

    const auto r = http::get_outputs(project_arg(store));
    REQUIRE(r.body["files"].size() == 2);   // txt 不算
    CHECK(r.body["files"][0]["name"] == "第二集.mp4");
    CHECK(r.body["files"][1]["name"] == "第一集.mp4");

    SUBCASE("大小保留一位小数") {
        CHECK(r.body["files"][0]["size_mb"] == doctest::Approx(1.5));
        CHECK(r.body["files"][1]["size_mb"] == doctest::Approx(3.0));
    }

    SUBCASE("rel 是相对项目根的，用正斜杠") {
        // 存绝对路径的话项目拷到别的机器就断链；用反斜杠的话
        // 在 Windows 上存的项目拿到 Linux 上读不了。
        const std::string rel = r.body["files"][0]["rel"];
        CAPTURE(rel);
        CHECK(rel.find(kBackslash) == std::string::npos);
        CHECK(rel.rfind("output/", 0) == 0);
    }

    SUBCASE("mtime 是真正的 Unix 秒") {
        // 直接用 file_time_type::time_since_epoch() 的话，MSVC 给的是
        // 1601 纪元的秒数——前端按 Unix 秒算会得出 2381 年，
        // 然后一律显示"刚刚"。每个文件都显示"刚刚"，看起来像是没坏。
        const long long mt = r.body["files"][0]["mtime"];
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        CAPTURE(mt);
        CAPTURE(now);
        CHECK(std::llabs(mt - now) < 300);
    }
}

TEST_CASE("成片列表：项目路径为空是 400") {
    try {
        http::get_outputs("");
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
    }
}

// ---------------------------------------------------------------------------
// 队列按阶段排（order = "stage"）。
//
// 多卡时按章排的代价：每一章都要经历一次配音 → 首帧 → 草稿 → 成片 → 装配，
// 其间工作进程反复空转、每个阶段结尾都有一条尾巴、每换阶段都换一次模型。
// 按阶段排就是所有章先出首帧，再所有章出草稿……
//
// 这里用一条**合并的**调用序列来证明顺序：Fakes 里 frames 和 videos 是
// 两个表，分开记是看不出"第二章的首帧在第一章的视频之前"的。
// ---------------------------------------------------------------------------

namespace {

struct OrderedFakes {
    std::vector<std::string> seq;   ///< "F:ep01_sh1" / "V:ep01_sh1"，按发生顺序

    http::RunDeps deps() {
        http::RunDeps d;
        d.settings = [] { return config::Settings{}; };
        d.profile = [] {
            models::HardwareProfile p;
            p.vram_gb = 6.0;
            p.tiers = models::tiers_for_vram(6.0);
            p.detected = true;
            return p;
        };
        d.backends = [this](const config::Settings&, const models::ProjectStore&) {
            pipeline::Backends b;
            b.frame = [this](const models::Shot& s, const stages::PromptBundle&,
                             const models::TierSpec&, const fs::path& dest,
                             pipeline::CancelToken&, const infer::StepCallback&) {
                seq.push_back("F:" + s.shot_id);
                Fakes::stub(dest);
            };
            b.video = [this](const models::Shot& s, const stages::RenderPlan&,
                             const std::optional<fs::path>&, const fs::path& dest,
                             pipeline::CancelToken&, const infer::StepCallback&) {
                seq.push_back("V:" + s.shot_id);
                Fakes::stub(dest);
            };
            return b;
        };
        return d;
    }
};

std::size_t first_index(const std::vector<std::string>& v, const std::string& x) {
    return static_cast<std::size_t>(
        std::find(v.begin(), v.end(), x) - v.begin());
}

}  // namespace

TEST_CASE("order=stage：所有章先出首帧，再所有章出视频") {
    quiesce();
    const auto store = make_store("按阶段", {{"ep01", 2}, {"ep02", 2}});
    OrderedFakes f;

    const auto r = http::post_run({{"project", project_arg(store)},
                                   {"episode_id", "ep01"},
                                   {"all_episodes", true},
                                   {"skip_final", true},
                                   // **要草稿档**：这条测的是阶段顺序，
                                   // 得有视频调用才看得出先后。草稿默认
                                   // 关掉之后加上 skip_final 就一条都没有了。
                                   {"skip_draft", false},
                                   {"order", "stage"}},
                                  f.deps());
    pipeline::jobs().wait_idle();
    CHECK(r.status == 200);

    // 两章四镜：四条首帧全在任何一条视频之前
    REQUIRE(f.seq.size() == 8);
    const std::size_t last_frame = std::max({
        first_index(f.seq, "F:ep01_sh1"), first_index(f.seq, "F:ep01_sh2"),
        first_index(f.seq, "F:ep02_sh1"), first_index(f.seq, "F:ep02_sh2")});
    const std::size_t first_video = std::min({
        first_index(f.seq, "V:ep01_sh1"), first_index(f.seq, "V:ep01_sh2"),
        first_index(f.seq, "V:ep02_sh1"), first_index(f.seq, "V:ep02_sh2")});
    CAPTURE(f.seq);
    CHECK(last_frame < first_video);
    // 阶段之内仍然按章的顺序
    CHECK(first_index(f.seq, "F:ep01_sh1") < first_index(f.seq, "F:ep02_sh1"));
    CHECK(first_index(f.seq, "V:ep01_sh1") < first_index(f.seq, "V:ep02_sh1"));

    SUBCASE("队列进度按 阶段×章 计") {
        // 跳了成片档：配音、首帧、草稿、装配 四个阶段 × 两章
        const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
        CHECK(snap["queue_total"] == 8);
        CHECK(snap["queue_done"] == 8);
    }
}

TEST_CASE("默认 order=episode：第一章的视频在第二章的首帧之前") {
    // 这条钉的是"默认行为一个字没变"——Python 就是一章跑完再跑下一章。
    quiesce();
    const auto store = make_store("按集", {{"ep01", 1}, {"ep02", 1}});
    OrderedFakes f;

    http::post_run({{"project", project_arg(store)},
                    {"episode_id", "ep01"},
                    {"all_episodes", true},
                    {"skip_final", true},
                    // 同上：要有视频调用才看得出先后
                    {"skip_draft", false}},
                   f.deps());
    pipeline::jobs().wait_idle();

    CAPTURE(f.seq);
    CHECK(first_index(f.seq, "V:ep01_sh1") < first_index(f.seq, "F:ep02_sh1"));
}

TEST_CASE("order 只认 episode 和 stage") {
    quiesce();
    const auto store = make_store("错order", {{"ep01", 1}});
    Fakes fakes;
    try {
        http::post_run({{"project", project_arg(store)},
                        {"episode_id", "ep01"},
                        {"order", "random"}},
                       fakes.deps());
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
    }
    pipeline::jobs().wait_idle();
}

TEST_CASE("预演的默认和实际出片的默认必须一致") {
    // **一个和实际不符的预演比不给还糟**：用户按它安排时间。
    //
    // 草稿档 2026-09-10 起默认不跑，而预演当时还在按两档算——
    // 报"草稿 22 镜加成片 22 镜、1.8 小时"，实际只跑成片、Turbo 6 步。
    const auto store = make_store("预演默认", {{"ep01", 2}});

    // **要用带标定的档位表。** 没标定时估时落到一个兜底数，
    // 两边一样，这条用例就测不出东西了——第一版就是这么写的，
    // 断言 40 < 40 当场红。
    const auto skipped = http::get_run_preview(project_arg(store), "ep01",
                                               false, false,
                                               /*skip_draft=*/true, false,
                                               preview_profile(true));
    CHECK(stage_shots(skipped.body, "draft") == -1);   // 整项都不出现
    CHECK(stage_shots(skipped.body, "final") == 2);

    const auto kept = http::get_run_preview(project_arg(store), "ep01", false,
                                            false, /*skip_draft=*/false, false,
                                            preview_profile(true));
    CHECK(stage_shots(kept.body, "draft") == 2);

    // 跳过一档之后估时必须变小——不变的话说明估算没跟着走，
    // 而那正是"预演和实际不符"的样子。
    CHECK(skipped.body["estimate_s"].get<double>() <
          kept.body["estimate_s"].get<double>());
}

TEST_CASE("前 n 分钟：预览和真按下去那一下说的是同一件事") {
    // **两处判据要一致**，不然按钮底下那行预估报的是另一件事，而人按它
    // 安排时间。两条都挡同一个组合：前 n 分钟是一章之内的事。
    const auto store = make_store("预览一致", {{"ep01", 3}});

    const auto posted = http::guard([&] {
        return http::post_run(nlohmann::json{{"project", project_arg(store)},
                                             {"episode_id", "ep01"},
                                             {"preview_s", 60},
                                             {"all_episodes", true}},
                              http::RunDeps{});
    });
    CHECK(posted.status == 400);

    const auto peeked = http::guard([&] {
        return http::get_run_preview(project_arg(store), "ep01",
                                     /*all_episodes=*/true, false, true, false,
                                     models::HardwareProfile{}, /*preview_s=*/60);
    });
    CHECK(peeked.status == 400);
}

TEST_CASE("前 n 分钟：预估只算前缀那几镜，不是整章") {
    // 不这么算的话，人按「只做前 1 分钟」，下面那行却报着整章的镜数和
    // 一个几小时的数——**报大了的预演和报小了一样糟**，它说的是另一件事。
    const auto store = make_store("预估前缀", {{"ep01", 6}});

    const auto whole = http::guard([&] {
        return http::get_run_preview(project_arg(store), "ep01", false, false,
                                     true, false, models::HardwareProfile{});
    });
    REQUIRE(whole.status == 200);
    const auto few = http::guard([&] {
        return http::get_run_preview(project_arg(store), "ep01", false, false,
                                     true, false, models::HardwareProfile{},
                                     /*preview_s=*/1.0);
    });
    REQUIRE(few.status == 200);

    // 1 秒钟只够第一镜。
    CHECK(few.body.at("shots").get<int>() == 1);
    CHECK(whole.body.at("shots").get<int>() > 1);
    // 挑中的那几镜要回给页面——点亮哪几格照这份名单，不让前端自己按时长
    // 再算一遍（算法两份，漂开那天点亮的和真做的不是同一批）。
    REQUIRE(few.body.contains("preview"));
    CHECK(few.body.at("preview").at("shot_ids").size() == 1);
    CHECK(few.body.at("preview").at("enough") == true);
    // 不要这一档时**一个字都不多回**：老界面拿到不认识的键会当噪声，
    // 而这一块的存在与否正是"这一次是不是预告"的标记。
    CHECK_FALSE(whole.body.contains("preview"));
}
