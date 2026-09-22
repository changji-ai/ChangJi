// 两个长任务的测试。
//
// 这两个和前面五个接口不一样：**立刻返回，活干在工作线程上**。
// 所以测的东西也不一样——不是"返回体对不对"，是：
//
//   一，起任务的那一刻回什么（started / total / 409）
//   二，跑完之后盘上是什么（建出的章节、存下的分镜）
//   三，中途出错和中途取消各自留下什么
//
// 没走对拍语料。Python 那边这两个的产出是异步写进 WriteState 的，
// 用 TestClient 录不到中间过程，只能录到最终快照——而中间过程
// （一章写砸了接着往下写）恰恰是这里最容易写错的地方。
// 所以这里直接对着行为写断言，并在注释里说清每条对应 Python 的哪一段。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/batch.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

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

fs::path fresh_copy(const std::string& tag) {
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_批量_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

std::string script_reply(const std::string& title) {
    return json{
        {"title", title},
        {"logline", title + "的一句话"},
        {"beats", json::array({
            json{{"kind", "action"}, {"speaker", ""}, {"text", "夜。天台。"}},
            json{{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", "一句台词"}},
        })}}.dump();
}

/// 每个用例前先把全局 job 表清干净。
/// 不清的话上一个用例留下的状态会让 running 判断出错。
void reset_jobs() {
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();
}

/// 等这个槽真正跑完（不是 running 变 false，那个在取消时立刻就变）。
void wait_done() { pipeline::jobs().wait_idle(); }

json write_snapshot() {
    return pipeline::jobs().snapshot(pipeline::JobKind::Write);
}

}  // namespace

TEST_CASE("写全片：把每一章都建出来并落库") {
    reset_jobs();
    const fs::path root = fresh_copy("写全片");
    const models::ProjectStore store(root);
    const std::size_t before = store.load_project().episodes.size();

    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{script_reply("第一话"), script_reply("第二话"),
                                 script_reply("第三话")});

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)},
                 {"premise", "林晚在天台等一个七年没出现的人。"},
                 {"episodes", 3}}, client);
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("started") == true);
    CHECK(r.body.at("total") == 3);

    wait_done();

    const models::Project after = store.load_project();
    CHECK(after.episodes.size() == before + 3);
    // 梗概被存下来了。下次打开界面时回填，不用凭记忆重打。
    CHECK(after.premise == "林晚在天台等一个七年没出现的人。");

    const json snap = write_snapshot();
    CHECK(snap.at("running") == false);
    CHECK(snap.at("done") == 3);
    CHECK(snap.at("total") == 3);
    CHECK(snap.at("episodes").size() == 3);
    CHECK(snap.at("message") == "写完了 3 章");
    CHECK(snap.at("error").is_null());

    // 每一章的记录都带上说话人和字数，界面靠它判断写长了没有
    for (const auto& e : snap.at("episodes")) {
        CHECK_FALSE(e.at("episode_id").get<std::string>().empty());
        CHECK(e.contains("speakers"));
        CHECK(e.contains("dialogue_chars"));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("批量写全片用的是四段的 schema，不是平的") {
    // **2026-09-13 实跑这条路撞到的。** 这儿原来用 `script_schema()`——平的
    // 那份，只有 minItems 4 的地板、没有时间结构。那正是 2026-09-12 改四段
    // 之前的行为，也正是「60 秒的章写出 13 秒的剧本」的来源：
    //
    //     ep01   7 行 /  3 句台词 / 293 字
    //     ep02  19 行 /  8 句台词 / 477 字
    //     ep03   3 行 /  1 句台词 / 122 字
    //
    // 同一天走单章那条出的是 17 拍 / 对白 171 字 /「合适」。**改四段那次
    // 漏了这条路**，而单章那条有测试盯着、这条没有。
    reset_jobs();
    const fs::path root = fresh_copy("全片四段");
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{script_reply("第一话"), script_reply("第二话")});

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)},
                 {"premise", "林晚在天台等一个七年没出现的人。"},
                 {"episodes", 2},
                 {"duration_s", 60.0}}, client);
    });
    REQUIRE(r.status == 200);
    wait_done();

    REQUIRE(client->calls().size() == 2);
    for (const auto& call : client->calls()) {
        CAPTURE(call.schema.dump().substr(0, 200));
        const auto& props = call.schema.at("properties");
        // 四段各一个键，平的那份只有一个 beats
        for (const char* key : {"opening", "escalation", "payoff", "cliff"}) {
            CHECK(props.contains(key));
        }
        CHECK_FALSE(props.contains("beats"));
    }

    SUBCASE("每章的形状重摇，不是全片一个模子") {
        // 写死比例的话 60 秒永远是 5/28/21/6，连着看几章是一个样。
        // 段的秒数落在 schema 的 description 里，两章不该字节相同。
        const std::string a = client->calls()[0].schema.dump();
        const std::string b = client->calls()[1].schema.dump();
        // 摇到同一个形状也是可能的，所以这里不断言"一定不同"——
        // 只断言**两份都带着时间结构**，形状本身由 random_shape 管，
        // 它自己有测试。
        CHECK(a.find("秒") != std::string::npos);
        CHECK(b.find("秒") != std::string::npos);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("章的编号不跳号，而且跳过预告") {
    // 预告片挂在 trailer 上。把它也数进去的话，
    // 有了预告之后新建的第二章会跳号变成 ep03。
    reset_jobs();
    const fs::path root = fresh_copy("编号");
    models::ProjectStore store(root);
    models::Project p = store.load_project();
    // 语料项目有 ep01、ep02。再塞一个 trailer 进去。
    models::Episode trailer;
    trailer.episode_id = "trailer";
    trailer.title = "预告";
    p.episodes.push_back(trailer);
    store.save_project(p);

    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{script_reply("新一话")});
    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 1}}, client);
    });
    REQUIRE(r.status == 200);
    wait_done();

    const models::Project after = store.load_project();
    bool has_ep03 = false;
    for (const auto& ep : after.episodes) {
        if (ep.episode_id == "ep03") has_ep03 = true;
        // 不该出现 ep04——那说明 trailer 被数进去了
        CHECK(ep.episode_id != "ep04");
    }
    CHECK(has_ep03);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("一章写砸了不拖垮后面几章") {
    // 跑一晚上，早上发现第二章挂了导致后面几章都没动，
    // 那这一晚上就白熬了。
    reset_jobs();
    const fs::path root = fresh_copy("写砸");
    const models::ProjectStore store(root);
    const std::size_t before = store.load_project().episodes.size();

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        script_reply("好的一话"),
        "抱歉，我做不到。",          // 这一章会失败
        script_reply("又一话"),
    });

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 3}}, client);
    });
    REQUIRE(r.status == 200);
    wait_done();

    // 三章都"处理过"了，但只建出两章
    const json snap = write_snapshot();
    CHECK(snap.at("done") == 3);
    CHECK(snap.at("episodes").size() == 3);
    CHECK(store.load_project().episodes.size() == before + 2);

    // 失败那条要留在列表里，而且 episode_id 是空的——这一章根本没建出来
    int failures = 0;
    for (const auto& e : snap.at("episodes")) {
        if (e.contains("error")) {
            ++failures;
            CHECK(e.at("episode_id") == "");
            CHECK_FALSE(e.at("error").get<std::string>().empty());
        }
    }
    CHECK(failures == 1);
    // 整个任务不算失败——它跑完了
    CHECK(snap.at("error").is_null());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("同一个槽上不能并发") {
    // Python 那边写全片和批量出分镜共用一个 WriteState，
    // 所以这条限制是照抄的，不是我加的。
    reset_jobs();
    const fs::path root = fresh_copy("并发");

    // 录得足够多，让第一个任务在第二个请求进来时还在跑
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back(script_reply("话"));
    auto client = std::make_shared<llm::ReplayClient>(many);

    const auto r1 = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 20}}, client);
    });
    REQUIRE(r1.status == 200);

    const auto r2 = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 1}}, client);
    });
    CHECK(r2.status == 409);
    CHECK(r2.body.at("detail") == "已经在写了");

    SUBCASE("批量出分镜说的是另一句话") {
        // 用户看到"已经在写了"会去找哪里在写剧本，
        // 而实际情况是那个槽被别的事占着。
        const auto r3 = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)}}, client);
        });
        CHECK(r3.status == 409);
        CHECK(r3.body.at("detail") == "剧本那边还在忙");
    }

    pipeline::jobs().cancel(pipeline::JobKind::Write);
    wait_done();

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("两个长任务的停止文案不一样") {
    // 同一个槽上跑的两件事，说法不该一样。
    // "已经写好的几章留着" 和 "已经出好的分镜留着" 说的是不同的东西。
    reset_jobs();
    const fs::path root = fresh_copy("停止文案");
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back(script_reply("话"));

    {
        auto client = std::make_shared<llm::ReplayClient>(many);
        http::guard([&] {
            return http::post_script_series(
                json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                     {"episodes", 20}}, client);
        });
        pipeline::jobs().cancel(pipeline::JobKind::Write);
        CHECK(write_snapshot().at("error") == "已手动停止。已经写好的几章留着。");
        wait_done();
    }
    {
        // 让 ep02 也有剧本，这样 plan/all 有活干
        models::ProjectStore store(root);
        models::Project p = store.load_project();
        for (auto& ep : p.episodes) {
            if (ep.episode_id == "ep02") ep.script = "林晚：一句台词";
        }
        store.save_project(p);

        auto client = std::make_shared<llm::ReplayClient>(many);
        const auto r = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)},
                     {"overwrite", true}}, client);
        });
        REQUIRE(r.status == 200);
        pipeline::jobs().cancel(pipeline::JobKind::Write);
        CHECK(write_snapshot().at("error") == "已手动停止。已经出好的分镜留着。");
        wait_done();
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("批量出分镜只挑有剧本又没分镜的") {
    reset_jobs();
    const fs::path root = fresh_copy("挑集");
    // 语料项目：ep01 有剧本有分镜，ep02 没剧本
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{"{}"});

    const auto r = http::guard([&] {
        return http::post_plan_all(
            json{{"project", paths::to_utf8(root)}}, client);
    });
    // ep01 已经有分镜、ep02 没剧本，所以一个都不该有。
    // **这不是错**：这颗按钮的用法就是隔一阵按一下把新写的章补上，按下去
    // 本来就没有漏的是常态。2026-09-16 前这儿回 400，界面上弹一个红框说
    // "一切正常"。现在回 200 + started:false，让界面自己挑话说。
    CHECK(r.status == 200);
    CHECK(r.body.at("started") == false);
    CHECK(r.body.at("episodes").empty());

    SUBCASE("镜头是空壳的那一章，照样算没分镜") {
        // **判据是"有能用的镜头"，不是"shots 数组非空"。** 空壳镜头
        //（shot_id 是空串）指不到任何文件、进不了任何一步，可数组非空就把
        // 这一章挡在补分镜之外——人看到「没有要补的」，而那一章明明是空的，
        // 一键跑完整部电影也救不回来。
        //
        // 正常流程产不出这种东西（2026-09-17 那个"写回空壳"的 bug 已修，
        // 见 stages/render.cpp 的 Done::ran），但存盘被截断、手工改坏
        // project.json 一样留得下，而**这条判据本来就该这么写**。
        {
            models::ProjectStore store(root);
            models::Project pj = store.load_project();
            models::Episode* ep = pj.episode_by_id("ep01");
            REQUIRE(ep != nullptr);
            REQUIRE_FALSE(ep->shots.empty());
            for (auto& sh : ep->shots) sh = models::Shot{};   // 抹成空壳
            store.save_project(pj);
        }
        auto c3 = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"{}"});
        const auto r3 = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)}}, c3);
        });
        REQUIRE(r3.status == 200);
        CHECK(r3.body.at("episodes") == json::array({"ep01"}));
    }

    SUBCASE("勾了覆盖就把有剧本的都算上") {
        std::vector<std::string> many;
        for (int i = 0; i < 10; ++i) many.push_back("{}");
        auto c2 = std::make_shared<llm::ReplayClient>(many);
        const auto r2 = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)},
                     {"overwrite", true}}, c2);
        });
        REQUIRE(r2.status == 200);
        CHECK(r2.body.at("episodes") == json::array({"ep01"}));
        wait_done();
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("参数校验") {
    reset_jobs();
    const fs::path root = fresh_copy("校验");
    const std::string project = paths::to_utf8(root);
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{"{}"});

    const auto check = [&](const json& body, int want) {
        const auto r = http::guard([&] {
            return http::post_script_series(body, client);
        });
        CHECK(r.status == want);
        return r;
    };

    check(json{{"project", project}}, 422);                       // 缺 premise
    check(json{{"project", project}, {"premise", "x"}, {"episodes", 0}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"episodes", 21}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"duration_s", 0}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"duration_s", 9999}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"typo", 1}}, 422);

    // 项目不存在是 400 不是 422——那不是校验问题
    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", "Z:/没有这个目录"}, {"premise", "x"}}, client);
    });
    CHECK(r.status == 400);

    // 全都没起起来
    CHECK_FALSE(pipeline::jobs().running(pipeline::JobKind::Write));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("409 优先于项目不存在") {
    // Python 那边是先判 running 再 load_project。两个都错时回哪一个
    // 是可观测的，所以顺序要照抄。
    reset_jobs();
    const fs::path root = fresh_copy("顺序");
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back(script_reply("话"));
    auto client = std::make_shared<llm::ReplayClient>(many);

    http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 20}}, client);
    });

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", "Z:/没有这个目录"}, {"premise", "x"}}, client);
    });
    CHECK(r.status == 409);   // 不是 400

    pipeline::jobs().cancel(pipeline::JobKind::Write);
    wait_done();

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ── 写正文：打回之后先改稿 ────────────────────────────────────────────

namespace {

/// 一份能过所有闸的章（形状同 test_story_outline.cpp 里那份）。
/// `with_dialogue` 关掉就通篇转述——撞「整章几乎没有对白」那道软闸。
std::string chapter_reply(bool with_dialogue) {
    const auto scene = [&](int base, const char* where, const char* turn,
                           const char* last) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            const int n = base + i;
            if (with_dialogue && i % 3 == 1) {
                paras.push_back("她抬起头，把第" + std::to_string(n) +
                                "把伞往柜台上一推：“这一把不是你的。”");
            } else {
                paras.push_back("第" + std::to_string(n) +
                                "个杯子被放回架子上，水顺着杯沿滑到她的指尖，凉的。");
            }
        }
        return json{{"where", where},
                    {"pov", "林晚"},
                    {"who", json::array({"林晚", "陈默"})},
                    {"goal", "把伞要回来"},
                    {"obstacle", "他不认这把伞"},
                    {"worse", "她发现伞根本不是他带来的"},
                    {"turn", turn},
                    {"paragraphs", paras},
                    {"last_line", last}};
    };
    return json{{"scenes",
                 json::array({scene(1, "深夜，便利店", "伞柄上刻着别人的名字",
                                    "他把伞柄转过来，刻的是另一个名字。"),
                              scene(100, "凌晨，后巷", "来的人不是他",
                                    "门口的风铃响了，进来的人没有带伞。")})}}
        .dump();
}

/// 给项目放一份只有大纲、没有正文的故事。
void seed_story(const models::ProjectStore& store) {
    models::Story st;
    st.logline = "林晚在便利店等一把伞。";
    models::Chapter c;
    c.chapter_id = "ch01";
    c.title = "雨夜";
    c.summary = "林晚在便利店等一把伞，来的人不是她等的那个。";
    st.chapters.push_back(c);
    store.save_story(st);
}

}  // namespace

TEST_CASE("写正文：打回之后先改稿，改稿提示词带着上一稿和清单") {
    // 用户 2026-09-19：「别整章重掷，把坏在第几处回给模型」。
    // 第一稿通篇转述（撞「整章几乎没有对白」），第二次发出去的必须是
    // 「上一稿 + 要改的地方」那份，不是同一份提示词再来一遍。
    reset_jobs();
    const fs::path root = fresh_copy("改稿");
    const models::ProjectStore store(root);
    seed_story(store);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        chapter_reply(/*with_dialogue=*/false), chapter_reply(/*with_dialogue=*/true)});

    const auto r = http::guard([&] {
        return http::post_story_chapters(json{{"project", paths::to_utf8(root)}},
                                         client);
    });
    REQUIRE(r.status == 200);
    wait_done();

    REQUIRE(client->calls().size() == 2);
    const std::string& first = client->calls()[0].prompt;
    const std::string& second = client->calls()[1].prompt;
    // 第一次是普通那份
    CHECK(first.find("【要改的地方】") == std::string::npos);
    // 第二次是改稿：原来的规矩还在、上一稿在、清单在、只改点到的地方
    CHECK(second.find("硬性要求") != std::string::npos);
    CHECK(second.find("【你上一稿】") != std::string::npos);
    CHECK(second.find("第1个杯子被放回架子上") != std::string::npos);
    CHECK(second.find("【要改的地方】") != std::string::npos);
    CHECK(second.find("1. 整章几乎没有对白") != std::string::npos);
    CHECK(second.find("第 1、2 场一句都没有") != std::string::npos);
    CHECK(second.find("只改上面点到的地方") != std::string::npos);
    // schema 和温度照旧
    CHECK(client->calls()[1].schema_name == "chapter");
    CHECK(client->calls()[1].schema == client->calls()[0].schema);

    // 落库的是改过的那份，而且这一章没记成"写砸了"
    const models::Story after = store.load_story();
    REQUIRE(after.chapters.size() == 1);
    CHECK(after.chapters[0].text.find("这一把不是你的") != std::string::npos);
    const json snap = write_snapshot();
    CHECK(snap.at("message") == "写完了 1 章");
    REQUIRE(snap.at("episodes").size() == 1);
    CHECK_FALSE(snap.at("episodes")[0].contains("error"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("写正文：解析不动的稿没法改，下一轮照旧重掷") {
    reset_jobs();
    const fs::path root = fresh_copy("重掷");
    const models::ProjectStore store(root);
    seed_story(store);

    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{"这不是 JSON", chapter_reply(true)});

    http::guard([&] {
        return http::post_story_chapters(json{{"project", paths::to_utf8(root)}},
                                         client);
    });
    wait_done();

    REQUIRE(client->calls().size() == 2);
    // 手里没稿，第二次发的还是普通那份
    CHECK(client->calls()[1].prompt.find("【要改的地方】") == std::string::npos);
    CHECK(client->calls()[1].prompt == client->calls()[0].prompt);
    CHECK(store.load_story().chapters[0].text.find("这一把不是你的") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}
