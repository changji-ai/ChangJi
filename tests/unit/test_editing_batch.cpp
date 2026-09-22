// 批量编辑接口的对拍测试。
//
// 这三个接口一次动很多镜头，所以每一个都有一条"哪些不该动"的规则。
// 那些规则比接口本身重要——错了就是一次误操作废掉几小时的渲染，
// 而且往往要等到播片时才发现。语料里对每条规则都有专门的用例。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "http/editing.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

fs::path pristine_project() {
    const json exp = load_golden("project_expectations");
    return paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           paths::from_utf8(exp.at("root_name").get<std::string>());
}

fs::path fresh_copy(const std::string& tag) {
    const fs::path dst = fs::temp_directory_path() /
                         paths::from_utf8("changji_批量_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

http::ApiResult dispatch(const std::string& url, const json& body) {
    if (url == "/api/shots/batch")
        return http::guard([&] { return http::post_shots_batch(body); });
    if (url == "/api/shots/reorder")
        return http::guard([&] { return http::post_shots_reorder(body); });
    if (url == "/api/shots/link_locations")
        return http::guard([&] { return http::post_shots_link_locations(body); });
    FAIL("语料里有没实现的接口: " << url);
    return {500, json::object()};
}

}  // namespace

TEST_CASE("批量编辑接口与 Python 逐条对拍") {
    const json g = load_golden("endpoints_batch_edit");
    int idx = 0;

    for (const auto& c : g.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string url = c.at("url").get<std::string>();
        CAPTURE(name);
        CAPTURE(url);

        const fs::path root = fresh_copy(std::to_string(idx++));

        json body = {{"project", paths::to_utf8(root)}};
        for (auto it = c.at("req").begin(); it != c.at("req").end(); ++it) {
            body[it.key()] = it.value();
        }

        const http::ApiResult got = dispatch(url, body);
        CHECK(got.status == c.at("status").get<int>());

        if (c.value("compare", "full") == "shape") {
            REQUIRE(got.body.is_object());
            REQUIRE(got.body.contains("detail"));
        } else {
            if (got.body != c.at("body")) {
                MESSAGE("期望 body: " << c.at("body").dump());
                MESSAGE("实得 body: " << got.body.dump());
            }
            CHECK(got.body == c.at("body"));
        }

        // 写盘后的镜头状态要一致。只比响应的话，changed 数字对了
        // 但没真写进去照样过。
        if (!c.at("shots_after").is_null()) {
            const models::ProjectStore store(root);
            // Project 必须存进具名变量，见下面那条注释
            const models::Project p = store.load_project();
            const models::Episode* ep = p.episode_by_id("ep01");
            REQUIRE(ep != nullptr);

            json after = json::array();
            for (const auto& s : ep->shots) {
                after.push_back({
                    {"shot_id", s.shot_id},
                    {"order", s.order},
                    {"status", models::to_string(s.status)},
                    {"location_id", s.location_id.has_value()
                                        ? json(*s.location_id) : json(nullptr)},
                    {"gate_notes", s.gate_notes},
                    {"attempts", s.attempts},
                });
            }
            if (after != c.at("shots_after")) {
                MESSAGE("镜头状态对不上");
                MESSAGE("期望: " << c.at("shots_after").dump());
                MESSAGE("实得: " << after.dump());
            }
            CHECK(after == c.at("shots_after"));
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("批量重置跳过锁定的镜头") {
    // 锁定的镜头是人工确认过的，批量重置不该动它们，
    // 否则一次误操作就把已经审过的片全废了。
    const fs::path root = fresh_copy("跳过锁定");
    {
        models::ProjectStore store(root);
        models::Project p = store.load_project();
        auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
        sh->status = models::ShotStatus::LOCKED;
        store.save_project(p);
    }

    const json body = {
        {"project", paths::to_utf8(root)},
        {"episode_id", "ep01"},
        {"action", "reset"},
    };
    const auto r = http::guard([&] { return http::post_shots_batch(body); });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("skipped_locked").get<int>() == 1);

    const models::ProjectStore store(root);
    const models::Project p = store.load_project();
    const auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
    CHECK(sh->status == models::ShotStatus::LOCKED);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("解锁时按有没有视频决定退回哪个状态") {
    // 有成片就回 final_done，没有就回 planned。
    // 一律回 planned 的话，已经渲染好的镜头会被白白重跑一遍。
    const fs::path root = fresh_copy("解锁");
    {
        models::ProjectStore store(root);
        models::Project p = store.load_project();
        auto* ep = p.episode_by_id("ep01");
        auto* with_video = ep->shot_by_id("ep01_s03_sh007");
        with_video->status = models::ShotStatus::LOCKED;
        with_video->video_path = "shots/final/a.mp4";
        auto* without = ep->shot_by_id("ep01_s01_sh001");
        without->status = models::ShotStatus::LOCKED;
        store.save_project(p);
    }

    const json body = {
        {"project", paths::to_utf8(root)},
        {"episode_id", "ep01"},
        {"action", "unlock"},
    };
    const auto r = http::guard([&] { return http::post_shots_batch(body); });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("changed").get<int>() == 2);

    const models::ProjectStore store(root);
    // 这里踩过一次 SIGSEGV：写成 store.load_project().episode_by_id(...) 的话，
    // load_project 返回的临时 Project 在表达式结束时就析构了，
    // episode_by_id 给出的指针指向已释放的内存。必须先存进具名变量。
    const models::Project p = store.load_project();
    const auto* ep = p.episode_by_id("ep01");
    CHECK(ep->shot_by_id("ep01_s03_sh007")->status == models::ShotStatus::FINAL_DONE);
    CHECK(ep->shot_by_id("ep01_s01_sh001")->status == models::ShotStatus::PLANNED);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("重排不改任何镜头的状态") {
    // 换顺序不改画面，已经渲染好的还能用。
    // 顺手把状态也重置的话，调一次节奏就要重跑整章。
    const fs::path root = fresh_copy("重排不重跑");

    const json body = {
        {"project", paths::to_utf8(root)},
        {"episode_id", "ep01"},
        {"shot_ids", json::array({"ep01_s03_sh007", "ep01_s01_sh001"})},
    };
    const auto r = http::guard([&] { return http::post_shots_reorder(body); });
    REQUIRE(r.status == 200);

    const models::ProjectStore store(root);
    const models::Project p = store.load_project();
    const auto* ep = p.episode_by_id("ep01");
    // 原本 audio_done 的那个还是 audio_done
    CHECK(ep->shot_by_id("ep01_s03_sh007")->status == models::ShotStatus::AUDIO_DONE);
    // 但顺序变了
    CHECK(ep->shot_by_id("ep01_s03_sh007")->order == 0);
    CHECK(ep->shot_by_id("ep01_s01_sh001")->order == 1);

    std::error_code ec;
    fs::remove_all(root, ec);
}
