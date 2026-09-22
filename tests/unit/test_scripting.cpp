// 三个剧本接口的对拍测试。
//
// 语料由 tests/export_scripting_golden.py 走**真实的 FastAPI 路由**生成，
// 大模型换成一个吐固定内容的桩——真调模型的话每次输出都不一样，
// 接口的形状就没法对拍了。
//
// 除了响应体，语料还录了每次发给模型的**提示词**。这一条很重要：
// 只比响应体的话，提示词拼错了照样能通过（桩不看提示词），
// 但真跑起来模型的输出会悄悄变，而那种漂移要跑几十个镜头才看得出来。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/scripting.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/script.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/endpoints_scripting.json";
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

/// 每个用例一份干净拷贝。写一章那个接口会改 project.json 里的梗概，
/// 不隔离的话后面的用例读到的就是被改过的项目。
fs::path fresh_copy(const std::string& tag) {
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_剧本_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

/// 语料里的 project 是导出时那台机器上的绝对路径，换成本地这份拷贝。
json localize(const json& body, const fs::path& root) {
    json out = body;
    if (out.contains("project") && out.at("project").is_string()) {
        const std::string p = out.at("project").get<std::string>();
        // 只换指向语料项目的那个，"Z:/根本没有这个目录" 要原样留着——
        // 那条用例测的正是"项目不存在"。
        if (p.rfind("Z:", 0) != 0) out["project"] = paths::to_utf8(root);
    }
    return out;
}

http::ApiResult dispatch(const std::string& url, const json& body,
                         llm::Client& client, pipeline::CancelToken& tok) {
    return http::guard([&]() -> http::ApiResult {
        if (url == "/api/script/premise") {
            return http::post_script_premise(body, client, tok);
        }
        if (url == "/api/script/write") {
            return http::post_script_write(body, client, tok);
        }
        return http::post_script_trailer(body, client, tok);
    });
}

}  // namespace

TEST_CASE("三个剧本接口逐条钉住（其中含提示词全文）") {
    // 同 test_script.cpp 里那条：语料本来是冻住的 Python 答案，Python
    // 删掉之后它的用途变成快照。这里的 case 里含提示词全文，所以
    // 改提示词会连带改这份语料——2026-09-11 改过一次（7 处）。

    int idx = 0;
    for (const auto& c : golden().at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string url = c.at("url").get<std::string>();
        CAPTURE(name);
        CAPTURE(url);

        const fs::path root = fresh_copy(std::to_string(idx++));
        const json body = localize(c.at("body"), root);

        llm::ReplayClient client({c.at("llm_reply").get<std::string>()});
        pipeline::CancelToken tok;
        const http::ApiResult got = dispatch(url, body, client, tok);

        // ---- 状态码 ----
        const int want_status = c.at("cpp_status").get<int>();
        if (got.status != want_status) {
            MESSAGE("body: " << got.body.dump());
        }
        CHECK(got.status == want_status);

        // ---- 响应体 ----
        const std::string mode = c.at("compare").get<std::string>();
        if (mode == "full") {
            if (got.body != c.at("response")) {
                MESSAGE("期望 " << c.at("response").dump(1));
                MESSAGE("实得 " << got.body.dump(1));
            }
            CHECK(got.body == c.at("response"));
        } else if (mode == "detail_arr") {
            // pydantic 的报错文字里嵌着版本号和文档 URL，复刻既荒唐又会腐烂，
            // 而前端只是把它显示出来、不解析。只比 type 和 loc。
            REQUIRE(got.body.is_object());
            REQUIRE(got.body.contains("detail"));
            REQUIRE(got.body.at("detail").is_array());
            REQUIRE_FALSE(got.body.at("detail").empty());
            const json& mine = got.body.at("detail")[0];
            const json& theirs = c.at("response").at("detail")[0];
            CHECK(mine.at("type") == theirs.at("type"));
            // loc 的段数是 pydantic 按模型嵌套层数生成的，
            // 多一段少一段前端高亮的就是别的字段
            CHECK(mine.at("loc") == theirs.at("loc"));
        } else {
            REQUIRE(got.body.is_object());
            REQUIRE(got.body.contains("detail"));
            CHECK(got.body.at("detail").is_string());
            CHECK_FALSE(got.body.at("detail").get<std::string>().empty());
        }

        // ---- 提示词 ----
        //
        // 这一段才是真正防漂移的。响应体只比得出"解析对不对"，
        // 比不出"发给模型的东西对不对"。
        const auto& want_prompts = c.at("prompts");
        REQUIRE(client.calls().size() == want_prompts.size());
        for (std::size_t i = 0; i < want_prompts.size(); ++i) {
            const std::string want = want_prompts[i].get<std::string>();
            const std::string mine = client.calls()[i].prompt;
            if (mine != want) {
                std::size_t k = 0;
                while (k < mine.size() && k < want.size() && mine[k] == want[k]) ++k;
                MESSAGE("提示词第 " << i << " 条，第一处不同在字节 " << k);
                MESSAGE("期望…" << want.substr(k > 40 ? k - 40 : 0, 110));
                MESSAGE("实得…" << mine.substr(k > 40 ? k - 40 : 0, 110));
            }
            CHECK(mine == want);
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("模型吐垃圾时回 400 而不是 500，这是有意和 Python 不一样") {
    // 和 /api/bible 是同一个 bug：script._parse 里 _extract_json 抛的是
    // StoryboardError 不是 ScriptError，而路由只 catch ScriptError，
    // 于是这个最常见的失败穿到最外面变成 500。
    //
    // 用户看到的应该是那句"大模型输出里找不到合法 JSON：……"，
    // 不是一个没有任何信息的 Internal Server Error。
    bool found = false;
    for (const auto& c : golden().at("cases")) {
        if (c.at("cpp_note").is_null()) continue;
        found = true;
        CHECK(c.at("status").get<int>() == 500);      // Python
        CHECK(c.at("cpp_status").get<int>() == 400);  // 我们
    }
    CHECK_MESSAGE(found,
                  "语料里没有这条分歧了——Python 侧可能修好了，"
                  "那就该把方案里那一节删掉");
}

TEST_CASE("写一章会把梗概存回项目") {
    // 下次写新一章时直接回填，不用凭记忆重打。
    const fs::path root = fresh_copy("存梗概");
    const std::string premise = "林晚在天台等一个七年没出现的人。";

    // 先确认原来不是这个梗概，否则这个用例什么都没测到
    {
        const models::ProjectStore store(root);
        REQUIRE(store.load_project().premise != premise);
    }

    const json reply = golden().at("cases")[6].at("llm_reply");
    llm::ReplayClient client({reply.get<std::string>()});
    pipeline::CancelToken tok;
    const json body = {{"project", paths::to_utf8(root)}, {"premise", premise}};
    const auto r = http::guard([&] {
        return http::post_script_write(body, client, tok);
    });
    REQUIRE(r.status == 200);

    const models::ProjectStore store(root);
    CHECK(store.load_project().premise == premise);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("预告片的松紧标准比正片严") {
    // 预告片写长了比正片更要命：刷到第三秒还没看到钩子，人就划走了。
    // 正片超出预算 1.35 倍才算偏长，预告片超出 1 倍就算。
    const fs::path root = fresh_copy("松紧");
    const std::string project = paths::to_utf8(root);

    // 造一段刚好在两条线之间的对白：超过 budget 但不到 budget * 1.35
    const int budget20 = stages::budget_chars(20.0);
    std::string line;
    for (int i = 0; i < budget20 + budget20 / 10; ++i) line += "字";
    const json reply = {
        {"title", "t"}, {"logline", "l"},
        {"beats", json::array({
            json{{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", line}}})}};

    pipeline::CancelToken tok;
    {
        llm::ReplayClient c({reply.dump()});
        const auto r = http::guard([&] {
            return http::post_script_trailer(
                json{{"project", project}, {"duration_s", 20.0}}, c, tok);
        });
        REQUIRE(r.status == 200);
        CHECK(r.body.at("fit") == "偏长");
    }
    {
        llm::ReplayClient c({reply.dump()});
        const auto r = http::guard([&] {
            return http::post_script_write(
                json{{"project", project}, {"premise", "x"},
                     {"duration_s", 20.0}}, c, tok);
        });
        REQUIRE(r.status == 200);
        CHECK(r.body.at("fit") == "合适");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("预告片不拿自己当素材") {
    // 预告片挂在 trailer 这个 id 上。把它也当素材的话，重剪一次
    // 就是拿上一版预告去剪，越剪越像宣传语。
    const fs::path root = fresh_copy("不吃自己");
    models::ProjectStore store(root);
    models::Project p = store.load_project();

    models::Episode trailer;
    trailer.episode_id = http::kTrailerEpisodeId;
    trailer.title = "预告";
    trailer.script = "这段不该出现在提示词里：上一版预告的内容";
    p.episodes.push_back(trailer);
    store.save_project(p);

    const json reply = {
        {"title", "t"}, {"logline", "l"},
        {"beats", json::array({
            json{{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", "一句"}}})}};
    llm::ReplayClient c({reply.dump()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_script_trailer(
            json{{"project", paths::to_utf8(root)}}, c, tok);
    });
    REQUIRE(r.status == 200);

    REQUIRE(c.calls().size() == 1);
    CHECK(c.calls()[0].prompt.find("上一版预告的内容") == std::string::npos);
    // 返回里也不该把 trailer 列成素材
    for (const auto& id : r.body.at("from_episodes")) {
        CHECK(id != http::kTrailerEpisodeId);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("写一章时不拿预告当前文") {
    // 同上一条的另一面：拿预告当写正片的上下文，模型会开始抄自己的预告。
    const fs::path root = fresh_copy("前文不含预告");
    models::ProjectStore store(root);
    models::Project p = store.load_project();

    models::Episode trailer;
    trailer.episode_id = http::kTrailerEpisodeId;
    trailer.title = "预告";
    trailer.script = "预告里的宣传语，不该进前文";
    // 插在最前面，确保它排在被写的那一章之前
    p.episodes.insert(p.episodes.begin(), trailer);
    store.save_project(p);

    const json reply = {
        {"title", "t"}, {"logline", "l"},
        {"beats", json::array({
            json{{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", "一句"}}})}};
    llm::ReplayClient c({reply.dump()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_script_write(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"}}, c, tok);
    });
    REQUIRE(r.status == 200);
    REQUIRE(c.calls().size() == 1);
    CHECK(c.calls()[0].prompt.find("预告里的宣传语") == std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("前文只取这一章之前的几章") {
    // 把后面的也塞进去，模型会把还没发生的事当成已经发生的写。
    const fs::path root = fresh_copy("前文截断");
    models::ProjectStore store(root);
    models::Project p = store.load_project();
    // 语料项目里 ep01 有剧本、ep02 没有。给 ep02 也写上，
    // 然后请求写 ep02——前文里就只该有 ep01。
    for (auto& ep : p.episodes) {
        if (ep.episode_id == "ep02") ep.script = "第二集的内容，不该当自己的前文";
    }
    store.save_project(p);

    const json reply = {
        {"title", "t"}, {"logline", "l"},
        {"beats", json::array({
            json{{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", "一句"}}})}};
    llm::ReplayClient c({reply.dump()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_script_write(
            json{{"project", paths::to_utf8(root)},
                 {"premise", "梗概"},
                 {"episode_id", "ep02"}}, c, tok);
    });
    REQUIRE(r.status == 200);
    REQUIRE(c.calls().size() == 1);
    CHECK(c.calls()[0].prompt.find("不该当自己的前文") == std::string::npos);
    CHECK(r.body.at("continued_from") == true);

    std::error_code ec;
    fs::remove_all(root, ec);
}
