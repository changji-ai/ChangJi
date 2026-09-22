// /api/bible 和 /api/plan 的对拍测试。
//
// 语料由 tests/export_planning_golden.py 走真实路由生成，大模型换成桩。
//
// 这两个接口和三个剧本接口不同的地方：**它们写盘**。所以除了响应体，
// 还要比对落盘之后的 assets.json 和 project.json——响应体对了不代表
// 存下去的东西对，而存错了要到下一步跑分镜时才发现。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/episodes.hpp"
#include "http/planning.hpp"
#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/endpoints_planning.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

fs::path pristine_project() {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/project_expectations.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    json exp;
    in >> exp;
    return paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           paths::from_utf8(exp.at("root_name").get<std::string>());
}

json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到 " << paths::to_utf8(p));
    json j;
    in >> j;
    return j;
}

void write_json(const fs::path& p, const json& j) {
    std::ofstream out(p, std::ios::binary);
    REQUIRE(out.good());
    out << j.dump();
}

fs::path fresh_copy(const std::string& tag, bool clear_assets) {
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_出片_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    if (clear_assets) {
        // 只清角色和场景，style 原样留着——多写或少写一个顶层键，
        // 资产库整份加载失败，那个失败会以 400 出现在别的地方。
        json a = read_json(dst / "assets.json");
        a["characters"] = json::object();
        a["locations"] = json::object();
        write_json(dst / "assets.json", a);
    }
    return dst;
}

json localize(const json& body, const fs::path& root) {
    json out = body;
    if (out.contains("project") && out.at("project").is_string()) {
        const std::string p = out.at("project").get<std::string>();
        if (p.rfind("Z:", 0) != 0) out["project"] = paths::to_utf8(root);
    }
    return out;
}

/// 按语料里录的调用顺序吐返回。
///
/// /api/plan 一次请求里可能先调圣经再调分镜，两次的结构完全不同，
/// 所以不能只给一条。
std::vector<std::string> replies_for(const json& c) {
    std::vector<std::string> out;
    for (const auto& p : c.at("prompts")) {
        out.push_back(p.at("stage") == "bible"
                          ? c.at("llm_bible").get<std::string>()
                          : c.at("llm_storyboard").get<std::string>());
    }
    // 语料里没录到调用（早早就 4xx 了）也要给一条，
    // 否则 C++ 侧万一真调了，报的是"回放录到头了"而不是真实原因
    if (out.empty()) out.push_back("{}");
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 「和 Python 逐字节一致」那几条，2026-09-17 退役
// ---------------------------------------------------------------------------
//
// 语料是当年冻下来的 Python 答案，用来证明移植没走样。**Python 引擎
// 2026-09-10 就删了**，那个用途从那天起就没了；断言还在，实际效果变成
// 「提示词和 schema 永远改不动」。
//
// 这一天里它挡住了三处量出来的改进：`camera_move` 的分类式禁令（模型对每
// 一镜重新论证一遍，一场戏想十五万字）、从没被填过的 `visual_desc`
//（两个项目 76% 和 100% 是空的）、以及 schema 的排版。用户拍板退役。
//
// **换掉的不是"有守卫"，是"守卫钉的是什么"**：钉结构（字段在不在、必填掉
// 没掉）而不是钉字节。前者是真出事——模型按错的名字产出、或者整个略过一栏，
// 全程不报错；后者只是让人改不动一句话。
//
TEST_CASE("写盘要刷新 updated_at") {
    // 上面那条用例比对 project.json 时把 updated_at 剔掉了，
    // 那就得单独确认它真的被刷新了——不刷的话，前端靠它判断
    // "项目有没有变过"的逻辑会一直认为没变。
    const fs::path root = fresh_copy("时间戳", true);
    const json before = read_json(root / "project.json");

    llm::ReplayClient cl({
        golden().at("cases")[0].at("llm_bible").get<std::string>(),
        golden().at("cases")[10].at("llm_storyboard").get<std::string>()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_plan(
            json{{"project", paths::to_utf8(root)},
                 // 台词要和桩回的分镜对得上：分镜漏掉剧本里的台词现在会被
                 // 拦下来（check_coverage），而这条用例测的是别的事。
                 {"script", "林晚：你说过会来的"}, {"episode_id", "ep01"}}, cl, tok);
    });
    REQUIRE_MESSAGE(r.status == 200, r.body.dump());

    const json after = read_json(root / "project.json");
    CHECK(after.at("updated_at") != before.at("updated_at"));
    // created_at 不能动
    CHECK(after.at("created_at") == before.at("created_at"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("角色圣经是合并不是替换") {
    // 角色和场景是全片共用的库。第五章的场景要出的时候，前四章的还在里面。
    // 整个换掉的话，那些场景连同它们的空景图一起没了，而分镜表里还留着
    // 指向它们的 id，跑起来直接报「场景未注册」。
    const fs::path root = fresh_copy("合并", false);
    const json before = read_json(root / "assets.json");
    REQUIRE(before.at("characters").contains("c_lin_yuan"));

    const json& c = golden().at("cases")[0];
    llm::ReplayClient client({c.at("llm_bible").get<std::string>()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_bible(
            json{{"project", paths::to_utf8(root)}, {"script", "剧本"}},
            client, tok);
    });
    REQUIRE(r.status == 200);

    const json after = read_json(root / "assets.json");
    // 原有的角色还在
    CHECK(after.at("characters").contains("c_lin_yuan"));
    // 新的也进来了
    CHECK(after.at("characters").contains("c_lin_wan"));
    CHECK(after.at("characters").contains("c_chen_mo"));
    // 原有的场景一个都没丢
    for (const auto& kv : before.at("locations").items()) {
        CAPTURE(kv.key());
        CHECK(after.at("locations").contains(kv.key()));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("不覆盖时不退回已渲染的镜头") {
    // 只新增没覆盖的话，老镜头用的还是原来那份设定，不用动。
    // 无条件退回的话，用户点一次"补角色"就把跑了一晚上的成果全废了。
    const fs::path root = fresh_copy("不覆盖", false);
    const json& c = golden().at("cases")[0];

    pipeline::CancelToken tok;
    llm::ReplayClient client({c.at("llm_bible").get<std::string>()});
    const auto r = http::guard([&] {
        return http::post_bible(
            json{{"project", paths::to_utf8(root)}, {"script", "剧本"}},
            client, tok);
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("reset_shots") == 0);

    SUBCASE("覆盖时要退回") {
        // 外观变了等于全片提示词都变了，已渲染的镜头和新设定对不上，
        // 留着比重跑更糟：用户以为改生效了，成片里却混着两套设定。
        const fs::path root2 = fresh_copy("要覆盖", false);
        llm::ReplayClient c2({c.at("llm_bible").get<std::string>()});
        const auto r2 = http::guard([&] {
            return http::post_bible(
                json{{"project", paths::to_utf8(root2)},
                     {"script", "剧本"},
                     {"overwrite", true}}, c2, tok);
        });
        REQUIRE(r2.status == 200);
        // 语料项目里有一个 audio_done 的镜头，它该被退回
        CHECK(r2.body.at("reset_shots").get<int>() > 0);

        std::error_code ec2;
        fs::remove_all(root2, ec2);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("plan 不校验多余字段，bible 校验") {
    // PlanRequest 没写 extra="forbid"，pydantic 默认忽略多余字段。
    // 给它加上校验就会拒掉 Python 能接受的请求。
    const fs::path root = fresh_copy("多余字段", true);
    const std::string project = paths::to_utf8(root);
    const json& plan_case = golden().at("cases")[0];
    pipeline::CancelToken tok;

    {
        // bible 要拒
        llm::ReplayClient c({"{}"});
        const auto r = http::guard([&] {
            return http::post_bible(
                json{{"project", project}, {"typo", 1}}, c, tok);
        });
        CHECK(r.status == 422);
    }
    {
        // plan 要收
        llm::ReplayClient c({
            plan_case.at("llm_bible").get<std::string>(),
            golden().at("cases")[9].at("llm_storyboard").get<std::string>()});
        const auto r = http::guard([&] {
            return http::post_plan(
                json{{"project", project}, {"script", "林晚：一句台词"},
                     {"episode_id", "ep01"}, {"typo", 1}}, c, tok);
        });
        // 不该因为 typo 而 422
        CHECK(r.status != 422);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("圣经找剧本的三级回落") {
    // 优先级：请求里给的 > 指定那一章的 > 第一章有内容的。
    // 三级都空才报错。角色设定是全片共用的，拿哪一章出都行，
    // 但总得有一章写好了。
    const fs::path root = fresh_copy("找剧本", false);
    const std::string project = paths::to_utf8(root);
    const json& c = golden().at("cases")[0];
    pipeline::CancelToken tok;

    SUBCASE("请求里给了就用它") {
        llm::ReplayClient cl({c.at("llm_bible").get<std::string>()});
        http::guard([&] {
            return http::post_bible(
                json{{"project", project}, {"script", "请求里给的剧本"}}, cl, tok);
        });
        REQUIRE(cl.calls().size() == 1);
        CHECK(cl.calls()[0].prompt.find("请求里给的剧本") != std::string::npos);
    }

    SUBCASE("没给就用项目里第一章有内容的") {
        llm::ReplayClient cl({c.at("llm_bible").get<std::string>()});
        http::guard([&] {
            return http::post_bible(json{{"project", project}}, cl, tok);
        });
        REQUIRE(cl.calls().size() == 1);
        // 语料项目 ep01 有剧本、ep02 没有
        const models::ProjectStore store(root);
        const models::Project p = store.load_project();
        const models::Episode* ep = p.episode_by_id("ep01");
        REQUIRE(ep != nullptr);
        CHECK(cl.calls()[0].prompt.find(ep->script) != std::string::npos);
    }

    SUBCASE("一章都没写就报错") {
        const fs::path empty = fresh_copy("没剧本", false);
        models::ProjectStore store(empty);
        models::Project p = store.load_project();
        for (auto& ep : p.episodes) ep.script.clear();
        store.save_project(p);

        llm::ReplayClient cl({"{}"});
        const auto r = http::guard([&] {
            return http::post_bible(
                json{{"project", paths::to_utf8(empty)}}, cl, tok);
        });
        CHECK(r.status == 400);
        // 一次模型都不该调
        CHECK(cl.calls().empty());

        std::error_code ec2;
        fs::remove_all(empty, ec2);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("plan 只在资产库为空或强制时才重出圣经") {
    // 已有就不重做，避免覆盖用户改过的设定。
    const json& sb_existing = golden().at("cases")[9].at("llm_storyboard");
    const json& bible = golden().at("cases")[0].at("llm_bible");
    pipeline::CancelToken tok;

    SUBCASE("资产库有角色，只调一次分镜") {
        const fs::path root = fresh_copy("不重出", false);
        llm::ReplayClient cl({sb_existing.get<std::string>()});
        const auto r = http::guard([&] {
            return http::post_plan(
                json{{"project", paths::to_utf8(root)},
                     // 同上：台词要和桩回的分镜对得上
                     {"script", "林晚：你说过会来的"}, {"episode_id", "ep01"}},
                cl, tok);
        });
        REQUIRE_MESSAGE(r.status == 200, r.body.dump());
        CHECK(cl.calls().size() == 1);   // 只有分镜

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("资产库是空的，先出圣经再出分镜") {
        const fs::path root = fresh_copy("要重出", true);
        llm::ReplayClient cl({bible.get<std::string>(),
                              golden().at("cases")[10]
                                  .at("llm_storyboard").get<std::string>()});
        const auto r = http::guard([&] {
            return http::post_plan(
                json{{"project", paths::to_utf8(root)},
                     // 同上：台词要和桩回的分镜对得上
                     {"script", "林晚：你说过会来的"}, {"episode_id", "ep01"}},
                cl, tok);
        });
        REQUIRE_MESSAGE(r.status == 200, r.body.dump());
        CHECK(cl.calls().size() == 2);

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("plan 粘回来的是按场装的 JSON 数组：场次靠 scene 对齐，回包也按场装") {
    // 用户 2026-09-18：「把每一场的 json 合并成 json 数组」。复制出去（回包
    // 里的 `scenes`）和粘回来（`paste`）是同一个形状。这条走整段
    // /api/plan：两场剧本 + 故意倒着放的两段，一个模型都不调，回包按场排好。
    const fs::path root = fresh_copy("场数组", false);
    pipeline::CancelToken tok;
    const std::string script =
        "【第1场 · 夜 · 内 · 天台】\n林晚：你说过会来的\n"
        "【第2场 · 日 · 内 · 病房】\n林晚：我来了。晚了七年。";
    const auto shot = [](const char* prompt, const char* line) {
        return json{
            {"shot_id", "ep01_sh001"}, {"scene_id", "loc_rooftop"}, {"order", 0},
            {"first_frame_prompt", prompt}, {"shot_size", "MS"},
            {"camera_angle", "eye_level"}, {"duration_s", 3.0},
            {"characters", json::array({json{{"char_id", "c_lin_yuan"},
                                             {"expression", "克制"},
                                             {"action", "站着"},
                                             {"face_pose", "front"}}})},
            {"dialogue", json::array({json{{"char_id", "c_lin_yuan"}, {"text", line}}})},
            {"transition_in", "cut"}};
    };
    // 第 2 场放前面：顺序乱了也得按 scene 认回去
    const json pasted = json::array({
        json{{"scene", 2}, {"shots", json::array({shot("病房，日光", "我来了。晚了七年。")})}},
        json{{"scene", 1}, {"shots", json::array({shot("夜间天台，雨中", "你说过会来的")})}},
    });
    llm::ReplayClient cl({"{}"});
    const auto r = http::guard([&] {
        return http::post_plan(json{{"project", paths::to_utf8(root)},
                                    {"script", script},
                                    {"episode_id", "ep01"},
                                    {"paste", pasted.dump()}},
                               cl, tok);
    });
    REQUIRE_MESSAGE(r.status == 200, r.body.dump());
    CHECK(cl.calls().empty());   // 粘回来的就一个模型都不调
    CHECK(r.body.at("shots") == 2);

    const json& scenes = r.body.at("scenes");
    REQUIRE(scenes.is_array());
    REQUIRE_MESSAGE(scenes.size() == 2, scenes.dump());
    CHECK(scenes[0].at("scene") == 1);
    CHECK(scenes[0].at("scene_id") == "s1");
    CHECK(scenes[1].at("scene") == 2);
    CHECK(scenes[1].at("scene_id") == "s2");
    REQUIRE(scenes[0].at("shots").size() == 1);
    REQUIRE(scenes[1].at("shots").size() == 1);
    // 各场的台词跟着 scene 走，不跟粘贴顺序走
    CHECK(scenes[0].at("shots")[0].at("dialogue")[0].at("text") == "你说过会来的");
    CHECK(scenes[1].at("shots")[0].at("dialogue")[0].at("text") == "我来了。晚了七年。");
    // 编号按引擎的来：两场合起来从 001 连着编
    CHECK(scenes[0].at("shots")[0].at("shot_id") == "ep01_sh001");
    CHECK(scenes[1].at("shots")[0].at("shot_id") == "ep01_sh002");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("分镜记得自己是照哪一版剧本拆的") {
    // 剧本重写之后，这一章已经拆好的分镜还是照**老剧本**拆的——而在这一栏
    // 加进来之前，没有任何地方说得出这件事：镜头页照常显示 151 镜，批量补
    // 分镜也照常报「没有要补的」（它问的是"有没有能用的镜头"）。
    //
    // 这一栏只用来说话，**不自动重拆**：重拆会覆盖整张表，手改过的台词、
    // 调过的顺序、锁住的镜头全没（见 http/episodes.hpp 的 post_script）。
    const fs::path root = fresh_copy("分镜版本", false);
    pipeline::CancelToken tok;

    const std::string script_v1 =
        "【第1场 · 夜 · 内 · 天台】\n林晚：你说过会来的";
    const json pasted = json::array({json{
        {"scene", 1},
        {"shots", json::array({json{
             {"shot_id", "ep01_sh001"}, {"scene_id", "loc_rooftop"}, {"order", 0},
             {"first_frame_prompt", "夜间天台，雨中"}, {"shot_size", "MS"},
             {"camera_angle", "eye_level"}, {"duration_s", 3.0},
             {"characters", json::array({json{{"char_id", "c_lin_yuan"},
                                              {"expression", "克制"},
                                              {"action", "站着"},
                                              {"face_pose", "front"}}})},
             {"dialogue", json::array({json{{"char_id", "c_lin_yuan"},
                                            {"text", "你说过会来的"}}})},
             {"transition_in", "cut"}}})}}});

    // 粘回来那条路一个模型都不调，所以这条用例不用桩回分镜。
    llm::ReplayClient cl({"{}"});
    const auto planned = http::guard([&] {
        return http::post_plan(json{{"project", paths::to_utf8(root)},
                                    {"script", script_v1},
                                    {"episode_id", "ep01"},
                                    {"paste", pasted.dump()}},
                               cl, tok);
    });
    REQUIRE_MESSAGE(planned.status == 200, planned.body.dump());

    models::ProjectStore store(root);
    {
        const models::Project p = store.load_project();
        const models::Episode* ep = p.episode_by_id("ep01");
        REQUIRE(ep != nullptr);
        CHECK_MESSAGE(ep->shots_from == models::chapter_text_fingerprint(script_v1),
                      "拆完没记下照的是哪一版剧本");
        CHECK_FALSE(ep->shots_from.empty());
        CHECK_FALSE(ep->shots_stale());
    }

    SUBCASE("剧本重写之后：这张表过期了，而且引擎说得出来") {
        // 走的是「理解故事」和「批量改编」用的那条路：post_script 不带
        // regenerate，分镜一个字都不动。
        llm::ReplayClient again({"{}"});
        const auto saved = http::guard([&] {
            return http::post_script(
                json{{"project", paths::to_utf8(root)},
                     {"episode_id", "ep01"},
                     {"script", "【第1场 · 日 · 内 · 病房】\n林晚：我来了。晚了七年。"}},
                again, tok);
        });
        REQUIRE_MESSAGE(saved.status == 200, saved.body.dump());
        CHECK_MESSAGE(saved.body.at("regenerated") == false,
                      "不带 regenerate 就不该重拆，重拆会覆盖整张表");

        const models::Project p = store.load_project();
        const models::Episode* ep = p.episode_by_id("ep01");
        REQUIRE(ep != nullptr);
        CHECK_MESSAGE(ep->shots.size() == 1, "分镜不该被自动冲掉");
        CHECK(ep->shots_stale());

        // 界面靠这一条说话。挂在信封上，不挂在每一镜上。
        const auto got = http::guard(
            [&] { return http::get_shots(paths::to_utf8(root), "ep01"); });
        REQUIRE_MESSAGE(got.status == 200, got.body.dump());
        CHECK(got.body.at("shots_stale") == true);
    }

    SUBCASE("重拆一遍就跟上了") {
        const std::string script_v2 =
            "【第1场 · 日 · 内 · 病房】\n林晚：我来了。晚了七年。";
        json redo = pasted;
        redo[0]["shots"][0]["dialogue"][0]["text"] = "我来了。晚了七年。";
        llm::ReplayClient again({"{}"});
        const auto r = http::guard([&] {
            return http::post_plan(json{{"project", paths::to_utf8(root)},
                                        {"script", script_v2},
                                        {"episode_id", "ep01"},
                                        {"paste", redo.dump()}},
                                   again, tok);
        });
        REQUIRE_MESSAGE(r.status == 200, r.body.dump());
        const models::Project p = store.load_project();
        CHECK_FALSE(p.episode_by_id("ep01")->shots_stale());
    }

    SUBCASE("老项目不冤枉：没有这个键就是说不清，不是过期") {
        // 升级前存下的 project.json 没有 `shots_from`。照 script_from 那套
        // 算法（空的就当对不上）的话，全项目每一章会在升级那一刻同时挂上
        // 牌子——人人有份等于没说。
        json raw = read_json(root / "project.json");
        bool erased = false;
        for (auto& e : raw.at("episodes")) {
            if (e.contains("shots_from")) {
                e.erase("shots_from");
                erased = true;
            }
        }
        REQUIRE(erased);
        write_json(root / "project.json", raw);

        const models::Project p = store.load_project();
        const models::Episode* ep = p.episode_by_id("ep01");
        REQUIRE(ep != nullptr);
        REQUIRE_FALSE(ep->shots.empty());
        REQUIRE_FALSE(ep->script.empty());
        CHECK_FALSE(ep->shots_stale());
    }

    SUBCASE("一镜都没有：那是还没拆，不是照旧剧本拆的") {
        models::Project p = store.load_project();
        models::Episode* ep = p.episode_by_id("ep01");
        REQUIRE(ep != nullptr);
        ep->shots.clear();
        CHECK_FALSE(ep->shots_stale());
    }

    SUBCASE("复制一章：副本照的是同一版剧本") {
        const auto dup = http::guard([&] {
            return http::post_episode_action(json{{"project", paths::to_utf8(root)},
                                                  {"episode_id", "ep01"},
                                                  {"action", "duplicate"}});
        });
        REQUIRE_MESSAGE(dup.status == 200, dup.body.dump());
        const models::Project p = store.load_project();
        const models::Episode* copy =
            p.episode_by_id(dup.body.at("episode_id").get<std::string>());
        REQUIRE(copy != nullptr);
        CHECK(copy->shots_from == p.episode_by_id("ep01")->shots_from);
        CHECK_FALSE(copy->shots_stale());
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}
