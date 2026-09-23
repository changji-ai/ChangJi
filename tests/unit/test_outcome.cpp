// 对话里带着的东西（`agent/outcome.hpp`）：派出去的活做出了什么、同步那几个
// 工具当场挂上什么、几件成几件没成。
//
// 这一层坏了**不报错**：比漏了的那几样只是不在对话里出现，人以为这一回
// 什么都没做出来；比多了的那几样（出片顺手改了状态、配音压了时长）会让每
// 一次出片都多出一张"分镜改了"的卡。下面几条各钉一边。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/outcome.hpp"
#include "agent/tools.hpp"
#include "agent/transcript.hpp"
#include "pipeline/task_board.hpp"
#include "util/paths.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace changji;

namespace {

fs::path temp_dir(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto p = fs::temp_directory_path() /
                   ("changji_outcome_" + tag + "_" + std::to_string(stamp));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void touch(const fs::path& p, const std::string& body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

/// 把 mtime 往后拨几秒：**同一秒里写两次，mtime 一模一样**（CLAUDE.md 里
/// 那条 HFS+ 的坑换个地方也一样），而"原地重出"正是靠 mtime 认的。
void bump(const fs::path& p, int secs = 10) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(secs));
}

json shot(const std::string& id, const std::string& line, bool framed) {
    json s;
    s["shot_id"] = id;
    s["order"] = 0;
    s["shot_size"] = "MS";
    s["camera_move"] = "static";
    s["duration_s"] = 4.0;
    s["status"] = "planned";
    if (!line.empty()) s["dialogue"] = json::array({{{"char_id", "c_a"}, {"text", line}}});
    if (framed) s["frame_path"] = "frames/" + id + ".png";
    return s;
}

void write_project(const fs::path& dir, const json& shots, const std::string& script) {
    json ep;
    ep["episode_id"] = "ep01";
    ep["title"] = "发现";
    ep["script"] = script;
    ep["shots"] = shots;
    json proj;
    proj["schema_version"] = 1;
    proj["project_id"] = "p_test";
    proj["title"] = "带着的东西";
    proj["episodes"] = json::array({ep});
    std::ofstream(dir / "project.json") << proj.dump(2);
}

void write_assets(const fs::path& dir) {
    json a;
    a["characters"] = {{"c_a", {{"char_id", "c_a"}, {"name", "董平"},
                                {"ref_front", "refs/c_a_front.png"}}}};
    a["locations"] = {{"loc_a", {{"location_id", "loc_a"}, {"name", "码头"},
                                 {"ref_empty", "refs/loc_a_empty.png"}}}};
    std::ofstream(dir / "assets.json") << a.dump(2);
}

/// 数一数某一族里有几件、有没有某个标题。
int count_kind(const json& media, const std::string& kind) {
    int n = 0;
    for (const auto& m : media) n += m.value("kind", std::string()) == kind;
    return n;
}

const json* find_title(const json& media, const std::string& title) {
    for (const auto& m : media) {
        if (m.value("title", std::string()) == title) return &m;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("做出来的东西：之前就有的不算，新出的和原地重出的才算") {
    const auto dir = temp_dir("changed");
    write_assets(dir);
    touch(dir / "refs/c_a_front.png");
    touch(dir / "refs/loc_a_empty.png");
    touch(dir / "frames/ep01_sh001.png");
    write_project(dir, json::array({shot("ep01_sh001", "你来了。", true),
                                    shot("ep01_sh002", "", false)}),
                  "旧剧本");

    const agent::Baseline before = agent::take_baseline(dir);

    // 做了三件事：sh001 的首帧**原地**重出（路径一个字没变）、sh002 头一回
    // 有了首帧、这一章出了成片；剧本也改了一稿。参考图一张没动。
    bump(dir / "frames/ep01_sh001.png");
    touch(dir / "frames/ep01_sh002.png");
    touch(dir / "output/ep01.mp4");
    write_project(dir, json::array({shot("ep01_sh001", "你来了。", true),
                                    shot("ep01_sh002", "", true)}),
                  "新剧本，改了一稿");

    const json media = agent::changed_media(dir, before);
    CAPTURE(media.dump(2));

    CHECK(count_kind(media, "image") == 2);
    CHECK(find_title(media, "第 1 章 sh001 · 首帧") != nullptr);
    CHECK(find_title(media, "第 1 章 sh002 · 首帧") != nullptr);
    // 参考图没动，**不许出现**——不然每一回出片都把整套参考图再摆一遍。
    CHECK(find_title(media, "董平 · 正面") == nullptr);

    const json* film = find_title(media, "第 1 章 · 成片");
    REQUIRE(film != nullptr);
    CHECK(film->value("kind", std::string()) == "video");
    CHECK(film->value("rel", std::string()) == "output/ep01.mp4");
    // 封面是这一章第一张有的首帧。
    CHECK(film->value("poster", std::string()) == "frames/ep01_sh001.png");

    const json* script = find_title(media, "剧本 · 第 1 章");
    REQUIRE(script != nullptr);
    CHECK(script->value("text", std::string()).find("新剧本，改了一稿") != std::string::npos);

    // 排序：字在前、图在中、片在后。
    REQUIRE(media.size() >= 4);
    CHECK(media.front().value("kind", std::string()) == "text");
    CHECK(media.back().value("kind", std::string()) == "video");

    fs::remove_all(dir);
}

TEST_CASE("做出来的东西：出片顺手改掉的状态和时长不算「分镜改了」") {
    // 配音会把镜头压短、出片会把状态推到 final——那是出片的副作用。算进来的话
    // 每一回出片都多一张「分镜 · 第 1 章」，而人一个字都没改。
    const auto dir = temp_dir("board");
    write_project(dir, json::array({shot("ep01_sh001", "你来了。", false)}), "剧本");
    const agent::Baseline before = agent::take_baseline(dir);

    json s = shot("ep01_sh001", "你来了。", false);
    s["status"] = "final_ok";
    s["duration_s"] = 2.4;
    write_project(dir, json::array({s}), "剧本");
    CHECK(agent::changed_media(dir, before).empty());

    // 台词真改了，才算。
    write_project(dir, json::array({shot("ep01_sh001", "你终于来了。", false)}), "剧本");
    const json media = agent::changed_media(dir, before);
    CAPTURE(media.dump(2));
    CHECK(find_title(media, "分镜 · 第 1 章") != nullptr);

    fs::remove_all(dir);
}

TEST_CASE("做出来的东西：盘上一样都没有也不抛") {
    // 空目录（项目还没建全）：盘面是空的，比出来也是空的，一个异常都不许冒。
    const auto dir = temp_dir("empty");
    agent::Baseline before;
    CHECK_NOTHROW(before = agent::take_baseline(dir));
    CHECK(before.pieces.empty());
    CHECK(agent::changed_media(dir, before).empty());
    fs::remove_all(dir);
}

TEST_CASE("改一镜：只列改了的那几栏，从什么改成什么") {
    const auto dir = temp_dir("shot_change");
    touch(dir / "frames/ep01_sh002.png");
    json before = shot("ep01_sh002", "婷婷？", true);
    json after = before;
    after["shot_size"] = "ECU";
    after["duration_s"] = 5.5;
    after["dialogue"] = json::array({{{"char_id", "c_a"}, {"text", "婷婷……是你吗？"}}});

    const json media = agent::shot_change_media(dir, "ep01", "ep01_sh002", before, after);
    CAPTURE(media.dump(2));
    const json* diff = find_title(media, "第 1 章 sh002 · 改动");
    REQUIRE(diff != nullptr);
    const std::string text = diff->value("text", std::string());
    CHECK(text.find("景别：MS → ECU") != std::string::npos);
    CHECK(text.find("时长：4.0s → 5.5s") != std::string::npos);
    CHECK(text.find("台词：婷婷？ → 婷婷……是你吗？") != std::string::npos);
    // 没改的那一栏不列。
    CHECK(text.find("运镜") == std::string::npos);
    // 那一镜的首帧跟着摆：看得出改的是哪一镜。
    CHECK(count_kind(media, "image") == 1);

    fs::remove_all(dir);
}

TEST_CASE("读设定：工具回话之外，那几张参考图挂在这一条上") {
    const auto dir = temp_dir("assets_read");
    write_assets(dir);
    write_project(dir, json::array(), "");
    touch(dir / "refs/c_a_front.png");
    // 场景那张故意不在盘上：**不在的不摆**，别摆一块空板子。

    agent::ToolContext ctx;
    ctx.project = paths::to_utf8(dir);
    const std::string out = agent::run_tool(ctx, "assets_read", "{}");
    CAPTURE(out);
    CAPTURE(ctx.media.dump(2));
    REQUIRE(ctx.media.size() == 1);
    CHECK(ctx.media[0].value("title", std::string()) == "董平 · 正面");
    CHECK(ctx.media[0].value("rel", std::string()) == "refs/c_a_front.png");
    // 回给模型的那段话里不带路径：那是给人看的。
    CHECK(out.find("refs/") == std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("带着的东西跟着那一条落库，读回来还在") {
    const auto dir = temp_dir("transcript");
    agent::Turn t;
    t.role = "system";
    t.text = "刚才派出去的活跑完了。";
    t.media = json::array({agent::media_item("image", "frames/a.png", "第 1 章 sh001")});
    t.at = 1;
    agent::append_turn(dir, t);

    agent::Turn plain;
    plain.role = "user";
    plain.text = "好";
    agent::append_turn(dir, plain);

    const auto back = agent::load_transcript(dir);
    REQUIRE(back.size() == 2);
    REQUIRE(back[0].media.is_array());
    REQUIRE(back[0].media.size() == 1);
    CHECK(back[0].media[0].value("rel", std::string()) == "frames/a.png");
    // 没带东西的那一条，文件里不写这一栏（一行一条的文件要耐读）。
    CHECK_FALSE(agent::to_json(back[1]).contains("media"));

    fs::remove_all(dir);
}

TEST_CASE("跑完了的账：只报这一回派出去的，没成的带着原话") {
    const auto dir = temp_dir("report");
    const std::string project = paths::to_utf8(dir);

    {
        // 上一回的活：比门槛早，不许算进这一回。
        pipeline::Task old("image", "画参考图 · 上一回的", project);
        old.begin();
    }
    const std::uint64_t floor = agent::task_floor_now();
    {
        pipeline::Task ok("image", "出首帧 · 第 1 章 sh001", project);
        ok.begin();
    }
    {
        pipeline::Task bad("image", "出首帧 · 第 1 章 sh003", project);
        bad.begin();
        bad.fail("显存不够");
    }

    const agent::WorkReport rep = agent::work_report(project, floor);
    const std::string& report = rep.text;
    CAPTURE(report);
    CHECK(rep.failed);
    // 拆开的那一份：没成的一节在最前，带着原话。
    CAPTURE(rep.report.dump(2));
    REQUIRE(rep.report.at("sections").size() == 2);
    const json& first = rep.report.at("sections").at(0);
    CHECK(first.at("kind") == "failed");
    CHECK(first.at("rows").at(0).at("title") == "出首帧 · 第 1 章 sh003");
    CHECK(first.at("rows").at(0).at("why") == "显存不够");
    CHECK(rep.report.at("sections").at(1).at("kind") == "done");
    CHECK(report.find("没成的：") != std::string::npos);
    CHECK(report.find("出首帧 · 第 1 章 sh003：显存不够") != std::string::npos);
    CHECK(report.find("做完的：") != std::string::npos);
    CHECK(report.find("出首帧 · 第 1 章 sh001") != std::string::npos);
    CHECK(report.find("上一回的") == std::string::npos);
    // 没成的排在最前：人（和模型）要先看见的是它。
    CHECK(report.find("没成的：") < report.find("做完的："));

    // 这一回什么都没派：一个字都不说（「跑完了」后面不挂空的节头）。
    CHECK(agent::work_report(project, agent::task_floor_now()).text.empty());

    fs::remove_all(dir);
}
