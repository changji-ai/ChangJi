// 资产类编辑接口的对拍测试：/api/character /api/location /api/style。
//
// 这三个改的是全片共用的东西，会连带把未锁定的镜头退回未开工。
// 所以除了响应，还要比对「重置了几个镜头」和「镜头状态变成什么」——
// 只比响应的话，reset_shots 数字对了但实际没写盘照样过。
//
// 最要紧的一条语义是"按值比不按字段在不在比"：界面一次提交整张表单，
// 光看字段存在的话，改个音色也会把全片镜头退回重跑，
// 已经渲染好的成片档白白重来一遍。语料里专门有一条"提交同样的值"钉它。

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
                         paths::from_utf8("changji_资产_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

http::ApiResult dispatch(const std::string& url, const json& body) {
    if (url == "/api/character") return http::guard([&] { return http::post_character(body); });
    if (url == "/api/location")  return http::guard([&] { return http::post_location(body); });
    if (url == "/api/style")     return http::guard([&] { return http::post_style(body); });
    FAIL("语料里有没实现的接口: " << url);
    return {500, json::object()};
}

}  // namespace

TEST_CASE("改一个角色、一个场景：只退用到它的那几镜") {
    // 原来一律 reset_all_shots：给罗春梅换件外套，林知夏的独角戏也全退回待跑，
    // 下一次按「开始」白重出一两百镜。
    const fs::path root = fresh_copy("targeted");
    models::ProjectStore store(root);
    {
        models::Project p = store.load_project();
        models::Episode& ep = *p.episode_by_id("ep01");
        // 两镜都出过画面：一镜有林远、在天台；一镜谁都没有、在小巷。
        models::Shot& with = *ep.shot_by_id("ep01_s03_sh007");
        with.characters.clear();
        models::CharacterInShot c;
        c.char_id = "c_lin_yuan";
        with.characters.push_back(c);
        with.location_id = "loc_rooftop";
        with.status = models::ShotStatus::AUDIO_DONE;
        models::Shot& other = *ep.shot_by_id("ep01_s01_sh001");
        other.characters.clear();
        other.location_id = "loc_alley";
        other.status = models::ShotStatus::AUDIO_DONE;
        store.save_project(p);
    }
    const auto status_of = [&](const char* sid) {
        return std::string(models::to_string(
            store.load_project().episode_by_id("ep01")->shot_by_id(sid)->status));
    };

    const auto r = http::guard([&] {
        return http::post_character(json{{"project", paths::to_utf8(root)},
                                         {"char_id", "c_lin_yuan"},
                                         {"patch", {{"attire", "换了一件雨衣"}}}});
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("reset_shots") == 1);
    CHECK(status_of("ep01_s03_sh007") == "planned");
    CHECK(status_of("ep01_s01_sh001") == "audio_done");   // 画面里没有他：不动

    const auto l = http::guard([&] {
        return http::post_location(json{{"project", paths::to_utf8(root)},
                                        {"location_id", "loc_alley"},
                                        {"patch", {{"lighting", "路灯坏了一盏"}}}});
    });
    REQUIRE(l.status == 200);
    CHECK(l.body.at("reset_shots") == 1);
    CHECK(status_of("ep01_s01_sh001") == "planned");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("加一个角色：有台词的小角色补得进名单，同名的拒收") {
    // 读正文那一步只收主要人物：营门哨兵六句台词，剧本里全记成了女主说的。
    const fs::path root = fresh_copy("加角色");
    models::ProjectStore store(root);
    const json ok = {{"project", paths::to_utf8(root)}, {"name", "哨兵"},
                     {"identity", "二十岁上下的新兵，上岗第三天"},
                     {"body", "偏瘦，站桩时两个肘架得偏高"},
                     {"face", "寸头，脸晒得发红，眉毛淡"},
                     {"attire", "作训服，枪背在肩上"}};
    const auto r = http::guard([&] { return http::post_character_add(ok); });
    REQUIRE(r.status == 200);
    const std::string id = r.body.at("char_id").get<std::string>();
    CHECK(id.rfind("c_", 0) == 0);
    const auto lib = store.load_assets();
    REQUIRE(lib.characters.count(id) == 1);
    CHECK(lib.characters.at(id).name == "哨兵");
    CHECK(lib.characters.at(id).appearance.attire == "作训服，枪背在肩上");

    // 同一个人登记两遍：镜头引用哪个说不清，拒收
    CHECK(http::guard([&] { return http::post_character_add(ok); }).status == 409);
    // 外观缺一段：引擎那头的校验照旧拦
    json bad = ok;
    bad["name"] = "店员";
    bad["face"] = "";
    CHECK(http::guard([&] { return http::post_character_add(bad); }).status == 400);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("资产编辑接口与 Python 逐条对拍") {
    const json g = load_golden("endpoints_asset_edit");
    int idx = 0;

    for (const auto& c : g.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string url = c.at("url").get<std::string>();
        CAPTURE(name);
        CAPTURE(url);

        const fs::path root = fresh_copy(std::to_string(idx++));

        json body = {{"project", paths::to_utf8(root)}, {"patch", c.at("patch")}};
        for (auto it = c.at("extra").begin(); it != c.at("extra").end(); ++it) {
            body[it.key()] = it.value();
        }
        if (!c.at("reset_shots").is_null()) {
            body["reset_shots"] = c.at("reset_shots");
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

        // 写盘后的资产库要一致
        if (!c.at("assets_after").is_null()) {
            const models::ProjectStore store(root);
            const json after = store.load_assets();
            if (after != c.at("assets_after")) {
                MESSAGE("资产库对不上");
                MESSAGE("期望: " << c.at("assets_after").dump());
                MESSAGE("实得: " << after.dump());
            }
            CHECK(after == c.at("assets_after"));
        }

        // 镜头状态也要一致——reset_shots 的数字对了不代表真的写进去了
        if (!c.at("shot_status_after").is_null()) {
            const models::ProjectStore store(root);
            const models::Project p = store.load_project();
            const auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
            REQUIRE(sh != nullptr);
            CHECK(std::string(models::to_string(sh->status)) ==
                  c.at("shot_status_after").get<std::string>());
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("提交同样的值不该触发重跑") {
    // 这一条单独拎出来，因为它是最容易写错的：
    // 界面提交整张表单，判"字段在不在"的话每次保存都会把全片退回重跑，
    // 用户改个名字就得等几小时重新渲染。必须按**值**比。
    const fs::path root = fresh_copy("同值");

    const models::ProjectStore store(root);
    const std::string same_face =
        store.load_assets().characters.at("c_lin_yuan").appearance.face;

    const json body = {
        {"project", paths::to_utf8(root)},
        {"char_id", "c_lin_yuan"},
        {"patch", {{"face", same_face}}},
    };
    const auto r = http::guard([&] { return http::post_character(body); });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("reset_shots").get<int>() == 0);

    // 那个原本 audio_done 的镜头不该被动过
    const models::Project p = store.load_project();
    const auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
    REQUIRE(sh != nullptr);
    CHECK(sh->status == models::ShotStatus::AUDIO_DONE);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("重置跳过已锁定的镜头") {
    // LOCKED 是人工确认过的，改全片风格也不该把它推翻重来。
    const fs::path root = fresh_copy("锁定");
    {
        models::ProjectStore store(root);
        models::Project p = store.load_project();
        auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
        sh->status = models::ShotStatus::LOCKED;
        store.save_project(p);
    }

    const json body = {
        {"project", paths::to_utf8(root)},
        {"patch", {{"global_style", "彻底换个画风"}}},
    };
    const auto r = http::guard([&] { return http::post_style(body); });
    REQUIRE(r.status == 200);

    const models::ProjectStore store(root);
    const models::Project p = store.load_project();
    const auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
    REQUIRE(sh != nullptr);
    CHECK(sh->status == models::ShotStatus::LOCKED);

    std::error_code ec;
    fs::remove_all(root, ec);
}
