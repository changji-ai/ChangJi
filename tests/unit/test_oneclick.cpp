// 一键成片：六步一路跑到底。
//
// 这里钉的是**按下去那一刻**该发生和不该发生的事——链条本身要跑几十分钟、
// 六步里有四步要调大模型，整条真跑不在单元测试的范围里。而按下去那一刻的
// 判断错了，代价恰恰最大：第一步就把人写好的故事整个换掉。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/oneclick.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/jobs.hpp"
#include "stages/story_outline.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_一键_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

models::ProjectStore make_store(const std::string& tag) {
    return models::ProjectStore::create(temp_root(tag), "yu_ye", "雨夜天台");
}

/// 什么都不回的客户端。这几条用例走不到调模型那一步（都在闸上就回来了），
/// 真走到了也该当场看出来——回一句空的，解析必然失败。
std::shared_ptr<llm::Client> mute() {
    return std::make_shared<llm::ReplayClient>(std::vector<std::string>{});
}

/// 出片那一层不接真的。这几条用例一步都跑不到那儿。
http::RunDeps no_deps() { return http::RunDeps{}; }

void reset_jobs() {
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

}  // namespace

TEST_CASE("一键成片：写过正文的项目不带 overwrite 就别动它") {
    // **第一步就会把整个故事换掉**，而那是人几个钟头的东西。这道闸要在
    // 起活之前判：起了活再抛的话，那句话落进任务状态的 error 里，页面上
    // 是一条"失败的任务"，而人按下去那一刻什么提示都没有。
    reset_jobs();
    auto store = make_store("别动它");
    models::Story story;
    models::Chapter c;
    c.chapter_id = "ch01";
    c.title = "第一章";
    c.text = "她把伞立在门口，水顺着伞骨一路淌到脚边。";
    story.chapters.push_back(c);
    store.save_story(story);

    const auto r = http::guard([&] {
        return http::post_oneclick(
            json{{"project", paths::to_utf8(store.root())}}, mute(), no_deps());
    });
    CHECK(r.status == 409);
    // 话里要说清有几章会被顶掉——「确定吗」是问不出答案的问法。
    CHECK(r.body.at("detail").get<std::string>().find("1 章") !=
          std::string::npos);

    std::error_code ec;
    fs::remove_all(store.root(), ec);
}

TEST_CASE("一键成片：别的片子在出片也按得下去——出片那一步排队，不当场 409") {
    // 原来这儿钉的是「出片那个槽忙着就 409」：那是 RunQueue 之前的事——第六步
    // 撞上别人在出片就是一个 409，前面五步白跑。现在第二件出片进队列排着、轮到
    // 了自己开始，于是别的片子出一个钟头的片，这一部的一键成片不必干等着。
    reset_jobs();
    auto store = make_store("槽忙着");

    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [](pipeline::JobProgress& p) {
                               while (!p.cancelled()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(5));
                               }
                           });
    const auto r = http::guard([&] {
        return http::post_oneclick(
            json{{"project", paths::to_utf8(store.root())}}, mute(), no_deps());
    });
    CHECK(r.status != 409);

    reset_jobs();
    std::error_code ec;
    fs::remove_all(store.root(), ec);
}

TEST_CASE("一键成片：没指定项目就 400，不是起一件跑不动的活") {
    reset_jobs();
    const auto r = http::guard(
        [&] { return http::post_oneclick(json::object(), mute(), no_deps()); });
    CHECK(r.status == 400);
}

TEST_CASE("只写一章的大纲：把「多一两章少一两章都行」那条收回来") {
    // **光把章数填成 1 不管用。** 规则表里明写着「章数按故事本身的需要来。
    // 上面那个数只是量级，多一两章少一两章都行」——照着填 1，模型给两三章
    // 正是那条规则要它做的事。所以只要一章时提示词里要多一段把它收回，
    // 并且说出什么是允许的（一章讲完、有结尾）。
    const std::string one = stages::build_outline_prompt(
        "她在天台等一个七年没出现的人。", models::StoryScale::SHORT,
        models::StyleLine::REALISTIC, "", 0, /*chapters=*/1);
    const std::string many = stages::build_outline_prompt(
        "她在天台等一个七年没出现的人。", models::StoryScale::SHORT,
        models::StyleLine::REALISTIC, "", 0);

    // 章数那个数真的换了
    CHECK(one.find("写成 1 章") != std::string::npos);
    CHECK(many.find("写成 4 章") != std::string::npos);
    // **收回那一句只在要一章时出现**。引的是原话不是条号——
    // 表一改条号就指歪（CLAUDE.md 第六条）。
    CHECK(one.find("这一次不适用") != std::string::npos);
    CHECK(many.find("这一次不适用") == std::string::npos);
    // 说出允许的那一面：一章自己是个完整的故事。
    CHECK(one.find("完整的故事") != std::string::npos);
}
