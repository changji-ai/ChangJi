// 阶段 2 只读接口的对拍测试。
//
// 语料是**真实 FastAPI 路由的响应**，不是照着路由代码重拼的 dict——
// 那样只能测出写脚本的人对代码的理解。export_golden.py 起一个 TestClient
// 打真接口，拿到什么存什么，含三条错误分支。
//
// 比较标准按方案第三节：结构兼容，key 顺序不管。nlohmann 的 operator==
// 是顺序无关的深比较，正好对应那条标准。

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path
                    << "（语料在版本库里，Python 引擎删掉之后不再重新生成）");
    json j;
    in >> j;
    return j;
}

/// 语料是用固定的 vram_gb_override 导出的，这边必须给同一个值。
/// 不钉的话档位会跟着跑测试那台机器的显卡走，换台机器就红。
config::Settings pinned_settings(double vram) {
    config::Settings s;
    s.vram_gb_override = vram;
    return s;
}

/// 按 url 把用例分派到对应的接口函数。
/// 语料里那台机器的语料目录。用项目路径反推：项目就在它下面。
std::string golden_dir_of(const json& g) {
    std::string proj = g.value("project_path", std::string());
    std::replace(proj.begin(), proj.end(), '\\', '/');
    const std::size_t slash = proj.rfind('/');
    return slash == std::string::npos ? proj : proj.substr(0, slash);
}

/// 语料里的绝对路径 → 本机的。只认语料目录那一段前缀，别的原样留着
/// （原样留着才会在断言里露出来，而不是被悄悄改成看起来对的东西）。
std::string g_corpus_golden;   // 语料那台机器的语料目录，跑之前填

std::string local_path(std::string v) {
    if (g_corpus_golden.empty()) return v;
    std::string fwd = v;
    std::replace(fwd.begin(), fwd.end(), '\\', '/');
    if (fwd.rfind(g_corpus_golden, 0) == 0) {
        return std::string(CHANGJI_GOLDEN_DIR) + fwd.substr(g_corpus_golden.size());
    }
    return v;
}

/// 比较前把两边的语料目录都换成 <GOLDEN>，反斜杠换斜杠。
///
/// 仓库检出在哪儿不是契约的一部分，但**路径出现在回包里**
/// （项目的 root、"这里不是一个项目目录：…" 那句错误消息），
/// 不归一化就只能在生成语料的那台机器上比。
json normalize_paths(const json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::replace(s.begin(), s.end(), '\\', '/');
        for (const std::string& base :
             {std::string(CHANGJI_GOLDEN_DIR), g_corpus_golden}) {
            if (base.empty()) continue;
            std::string b = base;
            std::replace(b.begin(), b.end(), '\\', '/');
            std::size_t at;
            while ((at = s.find(b)) != std::string::npos) {
                s.replace(at, b.size(), "<GOLDEN>");
            }
        }
        return s;
    }
    if (v.is_array()) {
        json out = json::array();
        for (const auto& e : v) out.push_back(normalize_paths(e));
        return out;
    }
    if (v.is_object()) {
        json out = json::object();
        for (auto it = v.begin(); it != v.end(); ++it) {
            out[it.key()] = normalize_paths(it.value());
        }
        return out;
    }
    return v;
}

http::ApiResult dispatch(const json& c, const config::Settings& s) {
    const std::string url = c.at("url").get<std::string>();
    const json& p = c.at("params");
    // **语料里的路径是生成那台机器的绝对路径**，在别的机器上不存在——
    // 原样喂给接口，回的是"这里不是一个项目目录"，和语料里的项目摘要
    // 当然不等。映射到本机语料目录下的同名目录。
    const auto param = [&](const char* k) -> std::string {
        if (!p.contains(k)) return std::string();
        return local_path(p.at(k).get<std::string>());
    };

    if (url == "/api/hardware") return http::guard([&] { return http::get_hardware(s); });
    if (url == "/api/settings") return http::guard([&] { return http::get_settings(s); });
    if (url == "/api/projects") return http::guard([&] { return http::get_projects(s); });
    if (url == "/api/project")
        return http::guard([&] { return http::get_project(param("path")); });
    if (url == "/api/assets")
        return http::guard([&] { return http::get_assets(param("path")); });
    if (url == "/api/shots")
        return http::guard([&] {
            return http::get_shots(param("path"), param("episode_id"));
        });

    FAIL("语料里有没实现的接口: " << url);
    return {500, json::object()};
}

}  // namespace

TEST_CASE("六个只读接口的响应与 Python 一致") {
    const json g = load_golden("endpoints_readonly");
    const auto settings = pinned_settings(g.at("pinned_vram_gb").get<double>());
    g_corpus_golden = golden_dir_of(g);

    for (const auto& c : g.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        const http::ApiResult got = dispatch(c, settings);

        // 状态码必须一致。错误分支尤其重要：前端按状态码分流，
        // 400 和 404 走的是两条不同的提示。
        CHECK(got.status == c.at("status").get<int>());

        // body 深比较，key 顺序不管。路径先归一化——仓库检出在哪儿
        // 不是契约的一部分，但它出现在回包里。
        json want = normalize_paths(c.at("body"));
        json have = normalize_paths(got.body);

        // **显卡型号只比形状，不比值。** 那是"这台机器插的什么卡"，
        // 换台机器必然不同，不是行为契约。显存已经用 pinned_vram_gb
        // 钉住了，所以下面档位表那一堆还是逐条比的——真正决定行为的是它。
        if (name == "hardware" && have.is_object() && have.contains("gpu")) {
            // **契约是"非空字符串 **或** null"，不是"一定有卡"。**
            //
            // get_hardware 探不到卡时回的就是 null。原来这里要求它必须是
            // 非空字符串，等于假定"跑测试的机器插着显卡"——本机和 Mac 上
            // 碰巧成立，而 **CI 的 Linux runner 一张卡都没有**，这一条
            // 就永远过不去，整条流水线卡在单元测试上编都编不了。
            // 2026-09-11 第一次把流水线跑起来时就卡在这儿。
            const auto& g = have["gpu"];
            const bool gpu_ok =
                g.is_null() ||
                (g.is_string() && !g.get<std::string>().empty());
            CHECK_MESSAGE(gpu_ok, "gpu 要么是非空字符串，要么是 null");
            have.erase("gpu");
            want.erase("gpu");
        }

        if (have != want) {
            // 失败时把差异指出来，否则一整个 JSON 对比看不出哪儿不对
            MESSAGE("期望: " << want.dump(2));
            MESSAGE("实得: " << have.dump(2));
        }
        CHECK(have == want);
    }
}

TEST_CASE("错误响应的形状与 FastAPI 的 HTTPException 一致") {
    const json g = load_golden("endpoints_readonly");
    const auto settings = pinned_settings(g.at("pinned_vram_gb").get<double>());

    int checked = 0;
    for (const auto& c : g.at("cases")) {
        if (c.at("status").get<int>() < 400) continue;
        ++checked;
        CAPTURE(c.at("name").get<std::string>());

        const http::ApiResult got = dispatch(c, settings);
        // FastAPI 的 HTTPException 回的是 {"detail": "..."}，
        // 前端只认这个形状，多一层或换个键名都会让错误提示变成空白
        REQUIRE(got.body.is_object());
        REQUIRE(got.body.contains("detail"));
        CHECK(got.body.at("detail").is_string());
        CHECK_FALSE(got.body.at("detail").get<std::string>().empty());
    }
    CHECK_MESSAGE(checked >= 3, "语料里应该有至少三条错误分支");
}

TEST_CASE("项目路径为空时是 400 不是 500") {
    // 这一条单独拎出来：漏了的话表现是用户没选项目时看到
    // "Internal Server Error"，而不是"没有指定项目目录"
    const auto r = http::guard([] { return http::get_project(""); });
    CHECK(r.status == 400);
    CHECK(r.body.at("detail").get<std::string>() == "没有指定项目目录");
}

TEST_CASE("接口层不会因为异常而崩") {
    // guard 的契约：任何异常都变成 5xx 的 JSON，不能穿出去。
    // 阶段 0 的 doctor 就是因为异常穿到 std::terminate 而静默死掉的。
    const auto r = http::guard([]() -> http::ApiResult {
        throw std::runtime_error("故意抛一个");
    });
    CHECK(r.status == 500);
    REQUIRE(r.body.contains("detail"));
    // 真实信息要带出来。FastAPI 默认那句 "Internal Server Error"
    // 什么也没说，用户报障时只能贴一张没用的截图。
    CHECK(r.body.at("detail").get<std::string>().find("故意抛一个") !=
          std::string::npos);
}
