// 引导流程那七步的判定。
//
// **这里最容易出的错不是算错，是对不上号。** 侧边栏按 `key` 认人：
// `flow_steps()` 给出格子，`flow_assess()` 给出每格打不打勾。两边的 key
// 少一个对不上，那一格就永远是灰的——不报错，用户也不知道为什么走不下去。
// 2026-09-10 把分镜和制作合成一步时正是这个风险：改了列表忘了改判定。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <set>
#include <string>

#include "http/flow.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

json a_project() {
    return json{{"project_id", "p1"},
                {"episodes", json::array({json{{"episode_id", "ep01"},
                                               {"script", "有内容"}}})}};
}

json a_shot(const char* id, const char* status) {
    return json{{"shot_id", id}, {"status", status}, {"duration_s", 3.0}};
}

std::set<std::string> keys_of(const json& steps) {
    std::set<std::string> out;
    for (const auto& s : steps) out.insert(s.at("key").get<std::string>());
    return out;
}

}  // namespace

TEST_CASE("每一格都得有对应的判定，一个都不能少") {
    // 这就是那条真正要守的不变式。少一个 key 的表现是"某一格永远打不上勾"，
    // 而不是任何一种报错。
    const auto steps = keys_of(http::flow_steps());
    const json done =
        http::flow_assess(a_project(), json::array({a_shot("a", "planned")}),
                          json::array(), "ep01")
            .at("done");

    for (const auto& key : steps) {
        CAPTURE(key);
        CHECK_MESSAGE(done.contains(key), "这一格没人给它打勾");
    }
    // 反过来也查一遍：判定里多出来的 key 是死代码，
    // 说明列表那边删过一步而判定这边忘了跟。
    for (const auto& [key, _] : done.items()) {
        CAPTURE(key);
        CHECK_MESSAGE(steps.count(key) == 1, "判定里多出来一个没有格子的 key");
    }
}

TEST_CASE("「设定」这一格：理解过、图也画齐了才打勾") {
    // 用户 2026-09-17 定的顺序：理解故事 → 出图 → 这一章。前端拿这两个数
    // 决定「出图」那颗按钮和顶栏「这一章」出不出现。
    const json project =
        json{{"project_id", "p1"},
             {"episodes", json::array({json{{"episode_id", "ep01"},
                                            {"chapter_refs", json::array({"ch01"})},
                                            {"script_chars", 120}}})}};
    const json story = json{{"characters", json::array({json{{"name", "林晚"}}})}};
    json assets = json{
        {"characters", json::array({json{{"char_id", "lin_wan"},
                                         {"ref_front", "refs/a.png"},
                                         {"ref_three_quarter", "refs/b.png"},
                                         {"ref_back", "refs/c.png"}}})},
        {"locations", json::array({json{{"location_id", "store"},
                                        {"ref_empty", "refs/d.png"}}})}};

    const json full = http::flow_assess(project, json::array(), json::array(),
                                        "ep01", story, assets);
    CHECK(full.at("done").at("assets") == true);
    CHECK(full.at("counters").at("understood") == true);
    CHECK(full.at("counters").at("refsOk") == true);
    CHECK(full.at("counters").at("refsMissing") == 0);

    SUBCASE("缺一张图：理解过了，但没画齐") {
        assets["characters"][0]["ref_back"] = nullptr;
        const json r = http::flow_assess(project, json::array(), json::array(),
                                         "ep01", story, assets);
        CHECK(r.at("counters").at("understood") == true);
        CHECK(r.at("counters").at("refsMissing") == 1);
        CHECK(r.at("counters").at("refsOk") == false);
        CHECK(r.at("done").at("assets") == false);
    }

    SUBCASE("剧本没写：没理解完") {
        json p2 = project;
        p2["episodes"][0]["script_chars"] = 0;
        const json r = http::flow_assess(p2, json::array(), json::array(), "ep01",
                                         story, assets);
        CHECK(r.at("counters").at("understood") == false);
        CHECK(r.at("done").at("assets") == false);
    }

    SUBCASE("没传资产（老调用）：当没理解过") {
        const json r = http::flow_assess(project, json::array(), json::array(),
                                         "ep01", story);
        CHECK(r.at("counters").at("understood") == false);
        CHECK(r.at("done").at("assets") == false);
    }
}

TEST_CASE("「成片」这一格：出了一章的片就轮到它，合成出来了才打勾") {
    const json project =
        json{{"project_id", "p1"},
             {"episodes", json::array({json{{"episode_id", "ep01"},
                                            {"chapter_refs", json::array({"ch01"})}},
                                       json{{"episode_id", "ep02"},
                                            {"chapter_refs", json::array({"ch02"})}}})}};
    const json one = json::array({json{{"name", "ep01.mp4"}}});
    const json both = json::array({json{{"name", "ep01.mp4"}}, json{{"name", "ep02_01.mp4"}}});

    SUBCASE("出了一章：轮到它（顶栏看 filmedChapters），只是还有章没出") {
        const json r = http::flow_assess(project, json::array(), one, "ep01");
        CHECK(r.at("counters").at("filmedChapters") == 1);
        CHECK(r.at("counters").at("allFilmed") == false);
        CHECK(r.at("done").at("film") == false);
    }
    SUBCASE("每章都出了：还没合成") {
        const json r = http::flow_assess(project, json::array(), both, "ep01");
        CHECK(r.at("counters").at("allFilmed") == true);
        CHECK(r.at("done").at("film") == false);
    }
    SUBCASE("合成出来了：打勾") {
        const json film = json{{"files", json::array({json{{"name", "成片.mp4"}}})}};
        const json r = http::flow_assess(project, json::array(), both, "ep01", json(),
                                         json(), film);
        CHECK(r.at("done").at("film") == true);
    }
}

TEST_CASE("镜头、出片合成了一步「这一章」；成片是整部电影最后单独的一步") {
    // 几页合成一页之后，侧边栏留几格指向同一个地方只会让人以为点错了。
    // 2026-09-17 又拆出一格「成片」：它不是某一章的事——出了片的章按章序
    // 接成一部完整的电影、一个文件。整部电影只在那一格出现。
    const auto steps = keys_of(http::flow_steps());
    CHECK(steps.count("episode") == 1);
    CHECK(steps.count("storyboard") == 0);
    CHECK(steps.count("production") == 0);
    CHECK(steps.count("shots") == 0);
    CHECK(steps.count("film") == 1);
    CHECK(steps.count("publish") == 0);
    CHECK(steps.count("script") == 0);  // 剧本大纲那一格也没有了
    CHECK(steps.count("characters") == 0);  // 角色和场景并进「设定」了
    CHECK(steps.count("scenes") == 0);
    CHECK(steps.count("assets") == 1);
    CHECK(http::flow_steps().size() == 5);
}

TEST_CASE("「这一章」打勾看的是**这一章自己**的成片，三位章号不许串到两位上") {
    // 判据是 flow.cpp 里那个 film_of。它原来是 `name.find(episode_id)`，
    // 而章号到 99 以内是 ep%02d、**第 100 章起变成 ep100**（story_plan.cpp
    // 的 ep_id 里刻意写的），于是站在 ep10 上时 ep100～ep109 十个成片文件
    // 全算成它的——一部长篇动辄上百章，这不是假想的数。
    //
    // **前端有同一条规则的 JS 版**（api/labels.js 的 isFilmOf，
    // api/film-of.test.js 把边界一条条钉住了），而这一侧一条用例都没有。
    // 两边分头写的东西迟早分叉，所以这儿照着那份用例钉一遍。
    const auto film = [](const char* name) {
        return json::array({json{{"name", name}}});
    };
    const auto done_for = [&](const char* name, const char* ep) {
        return http::flow_assess(a_project(), json::array(), film(name), ep)
            .at("done")
            .at("episode")
            .get<bool>();
    };

    // 引擎自己出的那份：<章号>.mp4
    CHECK(done_for("ep01.mp4", "ep01"));
    CHECK(done_for("ep10.mp4", "ep10"));
    CHECK(done_for("ep107.mp4", "ep107"));

    // **三位不许串到两位**——修的就是这一条
    CHECK_FALSE(done_for("ep100.mp4", "ep10"));
    CHECK_FALSE(done_for("ep109.mp4", "ep10"));
    // 反过来也不许
    CHECK_FALSE(done_for("ep10.mp4", "ep100"));

    // 人手加的后缀还算这一章（体检里那条 2K 出路教人 --upscale 出一份）
    CHECK(done_for("ep01_2k.mp4", "ep01"));
    CHECK(done_for("ep01-final.mp4", "ep01"));
    CHECK(done_for("导演版_ep01.mp4", "ep01"));

    // 别的章一律不算
    CHECK_FALSE(done_for("ep02.mp4", "ep01"));
    CHECK_FALSE(done_for("trailer.mp4", "ep01"));
    CHECK_FALSE(done_for("ep011.mp4", "ep01"));

    // 同一个章号在名字里出现两次，有一处对得上就算——不扫完的话
    // `ep100_ep10.mp4` 会被第一处的失败带跑
    CHECK(done_for("ep100_ep10.mp4", "ep10"));

    // 没选章：这一层的策略是"有成片就算"（flow.cpp 里
    // `episode_id.empty() || film_of(...)` 那一句），和 film_of 本身
    // 对空章号回假不是一回事。
    CHECK(done_for("ep99.mp4", ""));
}

TEST_CASE("「故事」这一格") {
    const auto steps = keys_of(http::flow_steps());
    CHECK(steps.count("story") == 1);

    // 老项目没有 story.json，不给 story 也得判得出来，而且别的格子不受影响
    const auto none = http::flow_assess(a_project(), json::array(),
                                        json::array(), "ep01")
                          .at("done");
    CHECK_FALSE(none.at("story").get<bool>());
    CHECK(none.at("project").get<bool>());

    // 判据不看梗概：梗概是写故事的输入，光有梗概什么都还没发生
    const json only_premise = json{{"premise", "深夜便利店"},
                                   {"chapters", json::array()}};
    CHECK_FALSE(http::flow_assess(a_project(), json::array(), json::array(),
                                  "ep01", only_premise)
                    .at("done")
                    .at("story")
                    .get<bool>());

    // **有内容才算写了故事。** 空章不算——「直接开写」建的正是一章空的
    // （summary 和 text 都是空串），按"有没有章节"判的话它一按下去这一步
    // 就打勾、「下一步」那个点跳到设定去，而人正要开始写第一章。
    const json blank_chapter = json{
        {"chapters", json::array({json{{"chapter_id", "ch01"},
                                       {"title", "第一章"},
                                       {"summary", ""},
                                       {"text", ""}}})},
        {"plan", json::array()}};
    CHECK_FALSE(http::flow_assess(a_project(), json::array(), json::array(),
                                  "ep01", blank_chapter)
                    .at("done").at("story").get<bool>());

    // 只有正文没有梗概也算（从别处粘正文进来那条路）
    const json pasted = json{
        {"chapters", json::array({json{{"chapter_id", "ch01"},
                                       {"summary", ""},
                                       {"text", "那天晚上……"}}})},
        {"plan", json::array()}};
    CHECK(http::flow_assess(a_project(), json::array(), json::array(),
                            "ep01", pasted)
              .at("done").at("story").get<bool>());

    const json with_chapters = json{
        {"chapters", json::array({json{{"chapter_id", "ch01"},
                                       {"summary", "这一章发生了什么"}}})},
        {"plan", json::array({json{{"episode_id", "ep01"}},
                              json{{"episode_id", "ep02"}}})}};
    const auto got = http::flow_assess(a_project(), json::array(),
                                       json::array(), "ep01", with_chapters);
    CHECK(got.at("done").at("story").get<bool>());
    CHECK(got.at("counters").at("chapters").get<int>() == 1);
    CHECK(got.at("counters").at("plannedEpisodes").get<int>() == 2);
}

TEST_CASE("「这一章」这一格：判据是装配出这一章的成片") {
    const auto all_done = json::array(
        {a_shot("a", "final_done"), a_shot("b", "locked")});

    // 镜头全出完了，但还没装配——这一格不算完
    CHECK_FALSE(http::flow_assess(a_project(), all_done, json::array(), "ep01")
                    .at("done")
                    .at("episode")
                    .get<bool>());

    // 产物里有这一章的成片才算
    const auto outputs = json::array({json{{"name", "ep01.mp4"}}});
    const auto got = http::flow_assess(a_project(), all_done, outputs, "ep01");
    CHECK(got.at("done").at("episode").get<bool>());

    // 镜头出没出完另外报一个数，进度条和「还差几镜」用它
    CHECK(got.at("counters").at("shotsDone").get<bool>());
    CHECK_FALSE(http::flow_assess(a_project(),
                                  json::array({a_shot("a", "planned")}),
                                  outputs, "ep01")
                    .at("counters")
                    .at("shotsDone")
                    .get<bool>());
}

TEST_CASE("shotsDone：光有分镜表不算出完") {
    // 这个数原来是「镜头」那一格的判定，三格合一之后降级成一个计数，
    // 但那条规矩照旧：光有分镜表就算出完的话，界面上说这一章拍完了，
    // 而实际上一帧画面都还没出。
    const auto only_planned = json::array({a_shot("a", "planned")});
    CHECK_FALSE(http::flow_assess(a_project(), only_planned, json::array(),
                                  "ep01")
                    .at("counters")
                    .at("shotsDone")
                    .get<bool>());

    const auto all_done = json::array(
        {a_shot("a", "final_done"), a_shot("b", "locked")});
    CHECK(http::flow_assess(a_project(), all_done, json::array(), "ep01")
              .at("counters")
              .at("shotsDone")
              .get<bool>());

    // 一个镜头都没有时 0 == 0 会让"全都做完了"意外成立，单独挡一下
    CHECK_FALSE(http::flow_assess(a_project(), json::array(), json::array(),
                                  "ep01")
                    .at("counters")
                    .at("shotsDone")
                    .get<bool>());
}
