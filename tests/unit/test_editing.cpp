// 阶段 3 编辑接口的对拍测试。
//
// 和只读接口的区别：编辑会写盘，所以每个用例跑在**一份全新的项目副本**上。
// 语料里除了响应还记着「改完之后那个镜头长什么样」，两边都要对上——
// 只比响应的话，"saved: true" 但字段写错了照样过。

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

/// 复制一份原始项目到临时目录。每个用例一份，互不污染。
fs::path fresh_copy(const std::string& tag) {
    const fs::path dst = fs::temp_directory_path() /
                         paths::from_utf8("changji_编辑_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

}  // namespace

TEST_CASE("POST /api/shot 与 Python 逐条对拍") {
    const json g = load_golden("endpoints_shot_edit");
    int idx = 0;

    for (const auto& c : g.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string tag = std::to_string(idx++);

        // 章/镜头不存在那两条不写盘，直接用原始项目
        const bool mutates = !c.at("dir").is_null();
        const fs::path root = mutates ? fresh_copy(tag) : pristine_project();

        json body = {
            {"project", paths::to_utf8(root)},
            {"episode_id", c.contains("episode_id")
                               ? c.at("episode_id").get<std::string>()
                               : std::string("ep01")},
            {"shot_id", c.contains("shot_id")
                            ? c.at("shot_id").get<std::string>()
                            : std::string("ep01_s03_sh007")},
            {"patch", c.at("patch")},
        };

        const http::ApiResult got = http::guard([&] { return http::post_shot(body); });

        CHECK(got.status == c.at("status").get<int>());

        // 语料里标了 compare 的含义：
        //   full  —— body 逐字段深比较
        //   shape —— 只比状态码，外加 detail 是个非空字符串
        //
        // shape 那几条是 pydantic 自己生成的报错文字，里面嵌着它的版本号和
        // 文档 URL（errors.pydantic.dev/2.13/...）。在 C++ 里复刻那串东西
        // 既荒唐又会随上游版本腐烂，而前端只是把它显示出来、不解析。
        // 所以这是一条**有意的偏离**，不是没对齐——记在方案的破契约白名单里。
        const std::string mode = c.value("compare", "full");
        if (mode == "shape") {
            REQUIRE(got.body.is_object());
            REQUIRE(got.body.contains("detail"));
            CHECK(got.body.at("detail").is_string());
            CHECK_FALSE(got.body.at("detail").get<std::string>().empty());
        } else {
            if (got.body != c.at("body")) {
                MESSAGE("期望 body: " << c.at("body").dump());
                MESSAGE("实得 body: " << got.body.dump());
            }
            CHECK(got.body == c.at("body"));
        }

        // 成功的用例还要比对写盘后的镜头
        if (!c.at("shot_after").is_null()) {
            const models::ProjectStore store(root);
            const models::Project p = store.load_project();
            const models::Episode* ep = p.episode_by_id("ep01");
            REQUIRE(ep != nullptr);
            const models::Shot* sh = ep->shot_by_id("ep01_s03_sh007");
            REQUIRE(sh != nullptr);

            const json after = *sh;
            if (after != c.at("shot_after")) {
                // 只打不同的字段，整个镜头对比看不出哪儿不对
                for (auto it = c.at("shot_after").begin();
                     it != c.at("shot_after").end(); ++it) {
                    if (!after.contains(it.key()) || after.at(it.key()) != it.value()) {
                        MESSAGE("字段 " << it.key() << " 期望 " << it.value().dump()
                                        << " 实得 "
                                        << (after.contains(it.key())
                                                ? after.at(it.key()).dump()
                                                : std::string("<缺失>")));
                    }
                }
            }
            CHECK(after == c.at("shot_after"));
        }

        if (mutates) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
}

TEST_CASE("校验不过时原文件一个字节都不能变") {
    // Python 那边是 model_validate 出新对象、验过了才赋回原对象，
    // 中途失败原对象一个字段都没动。这个性质要保住，
    // 否则一次失败的编辑会在盘上留下半改的镜头。
    const fs::path root = fresh_copy("原子性");

    std::ifstream in(root / "project.json", std::ios::binary);
    const std::string before((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    in.close();

    // 一个合法字段加一个会让整体校验失败的字段
    const json body = {
        {"project", paths::to_utf8(root)},
        {"episode_id", "ep01"},
        {"shot_id", "ep01_s03_sh007"},
        {"patch", {{"beat", "改了"}, {"duration_s", 999}}},
    };
    const auto r = http::guard([&] { return http::post_shot(body); });
    CHECK(r.status == 400);

    std::ifstream in2(root / "project.json", std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in2)),
                            std::istreambuf_iterator<char>());
    in2.close();

    CHECK_MESSAGE(before == after,
                  "校验失败却写盘了——beat 那个合法字段被留在了文件里");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("枚举取值非法要被拒绝，不能静默回落") {
    // nlohmann 的枚举反序列化在认不出取值时**静默回落到表里第一项**。
    // 不自己回头验一次的话，前端传个 "XXL" 会被悄悄改成 ECU，
    // 用户看到景别莫名其妙变了，而后端报的是成功。
    const fs::path root = fresh_copy("枚举");
    const json body = {
        {"project", paths::to_utf8(root)},
        {"episode_id", "ep01"},
        {"shot_id", "ep01_s03_sh007"},
        {"patch", {{"shot_size", "XXL"}}},
    };
    const auto r = http::guard([&] { return http::post_shot(body); });
    CHECK(r.status == 400);

    // 而且盘上的景别没被改
    const models::ProjectStore store(root);
    // 必须先把 Project 存进具名变量。写成
    // store.load_project().episode_by_id(...)->shot_by_id(...) 是悬空指针：
    // load_project 按值返回临时对象，整个表达式结束时它就析构了，
    // 而 episode_by_id 返回的是指向它内部的指针。
    const models::Project p = store.load_project();
    const auto* sh = p.episode_by_id("ep01")->shot_by_id("ep01_s03_sh007");
    REQUIRE(sh != nullptr);
    CHECK(sh->shot_size == models::ShotSize::MCU);  // 原值

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 整镜换台词（`dialogue_lines`，2026-09-23） ----
//
// `dialogue_texts` 是逐条改字，条数必须对上。代理原来只能走它，于是没台词
// 的镜头加不上台词、有两句的也改不了。`dialogue_lines` 是"整镜换成这几句"。

namespace {

models::Shot shot_of(const fs::path& root, const std::string& sid) {
    models::ProjectStore store(root);
    const models::Project p = store.load_project();
    const models::Episode* ep = p.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    for (const auto& s : ep->shots) {
        if (s.shot_id == sid) return s;
    }
    FAIL("没有这一镜 " << sid);
    return {};
}

http::ApiResult edit(const fs::path& root, const std::string& sid, const json& patch) {
    const json body = {{"project", paths::to_utf8(root)}, {"episode_id", "ep01"},
                       {"shot_id", sid}, {"patch", patch}};
    return http::guard([&] { return http::post_shot(body); });
}

}  // namespace

TEST_CASE("整镜换台词：没台词的镜头能加一句旁白") {
    const fs::path root = fresh_copy("加台词");
    REQUIRE(shot_of(root, "ep01_s01_sh001").dialogue.empty());

    const auto r = edit(root, "ep01_s01_sh001",
                        {{"dialogue_lines", json::array({{{"char_id", nullptr},
                                                          {"text", "那年夏天雨特别多。"}}})}});
    CHECK(r.status == 200);
    const auto s = shot_of(root, "ep01_s01_sh001");
    REQUIRE(s.dialogue.size() == 1);
    CHECK_FALSE(s.dialogue[0].char_id.has_value());
    CHECK(s.dialogue[0].text == "那年夏天雨特别多。");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("整镜换台词：说话的人不在画面里就拒，不替它把人加进去") {
    const fs::path root = fresh_copy("加台词换人");
    std::ifstream in(root / "project.json", std::ios::binary);
    const std::string before((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    in.close();

    // sh001 画面里一个人都没有。
    const auto r = edit(root, "ep01_s01_sh001",
                        {{"dialogue_lines", json::array({{{"char_id", "lin_yuan"},
                                                          {"text", "你来了。"}}})}});
    CHECK(r.status == 400);
    std::ifstream in2(root / "project.json", std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in2)),
                            std::istreambuf_iterator<char>());
    CHECK(before == after);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("整镜换台词：两句换成一句，同一个人说的那句留着情绪") {
    const fs::path root = fresh_copy("两句换一句");
    const auto old = shot_of(root, "ep01_s03_sh007");
    REQUIRE(old.dialogue.size() == 2);
    REQUIRE(old.dialogue[0].char_id == std::optional<std::string>("lin_yuan"));

    const auto r = edit(root, "ep01_s03_sh007",
                        {{"dialogue_lines", json::array({{{"char_id", "lin_yuan"},
                                                          {"text", "你来晚了。"}}})}});
    CHECK(r.status == 200);
    CHECK(r.body.at("reset_to_planned").get<bool>());
    const auto s = shot_of(root, "ep01_s03_sh007");
    REQUIRE(s.dialogue.size() == 1);
    CHECK(s.dialogue[0].text == "你来晚了。");
    CHECK(s.dialogue[0].emotion == old.dialogue[0].emotion);
    // 字换了，配音作废。
    CHECK_FALSE(s.dialogue[0].audio_path.has_value());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("整镜换台词：原样送回来什么都不动，配过的音不扔") {
    const fs::path root = fresh_copy("原样");
    const auto old = shot_of(root, "ep01_s03_sh007");
    json lines = json::array();
    for (const auto& d : old.dialogue) {
        lines.push_back({{"char_id", d.char_id ? json(*d.char_id) : json(nullptr)},
                         {"text", d.text}});
    }
    const auto r = edit(root, "ep01_s03_sh007", {{"dialogue_lines", lines}});
    CHECK(r.status == 200);
    CHECK_FALSE(r.body.at("reset_to_planned").get<bool>());
    const auto s = shot_of(root, "ep01_s03_sh007");
    REQUIRE(s.dialogue.size() == old.dialogue.size());
    CHECK(s.dialogue[0].audio_path == old.dialogue[0].audio_path);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("整镜换台词：两种改法一起给就拒") {
    const fs::path root = fresh_copy("两种一起");
    const auto r = edit(root, "ep01_s03_sh007",
                        {{"dialogue_texts", json::array({"a", "b"})},
                         {"dialogue_lines", json::array({{{"text", "c"}}})}});
    CHECK(r.status == 400);
    std::error_code ec;
    fs::remove_all(root, ec);
}
