// /api/llm/providers 和 /api/llm/models 的测试。
//
// 这两个原本在方案的破契约白名单里。**决策 4 之后那个理由不成立了**——
// 用远端大模型不再是过渡状态，是长期形态之一（树莓派没有跑 14B 的内存）。
// 所以这里测的是"和 Python 形状一致"，外加新增字段是**向后兼容**的。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/prompt_peek.hpp"
#include "http/llm_info.hpp"
#include "util/paths.hpp"

#include "scoped_env.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

config::Settings test_settings(const fs::path& models_dir = {}) {
    config::Settings s;
    s.llm.base_url = "http://127.0.0.1:11434/v1";
    s.llm.model = "qwen3:14b";
    s.llm.api_key = "ollama";
    s.llm.timeout_s = 300.0;
    if (!models_dir.empty()) {
        s.models.dir = paths::to_utf8(models_dir);
        s.models.llm = "Qwen3-14B-Q4_K_M.gguf";
    }
    return s;
}

struct FakeGet {
    std::vector<std::string> urls;
    std::vector<double> timeouts;
    /// 每一次发出去的 `Authorization`。**问"用的是哪把钥匙"只能看这个。**
    std::vector<std::string> auths;
    llm::HttpResponse reply;

    http::HttpGet fn() {
        return [this](const std::string& url,
                      const std::map<std::string, std::string>& headers,
                      double timeout_s) {
            urls.push_back(url);
            timeouts.push_back(timeout_s);
            const auto it = headers.find("Authorization");
            auths.push_back(it == headers.end() ? std::string() : it->second);
            return reply;
        };
    }
};

llm::HttpResponse ok_body(const json& j) {
    return llm::HttpResponse{200, j.dump(), std::nullopt};
}

fs::path make_models_dir(const std::string& tag,
                         const std::vector<std::string>& names) {
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_模型_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const auto& n : names) {
        std::ofstream out(dir / paths::from_utf8(n), std::ios::binary);
        out << "假的";
    }
    return dir;
}

}  // namespace

TEST_CASE("换一家去问模型：用那一家自己的钥匙") {
    // ⚠️ 密钥是**按地址各存一把**的。换平台那一下界面上的地址已经变了，
    // 而 `settings` 里那把还是上一家的——照它发就必然 401，**而 401 长得像
    // "这个模型不让你用"**，人会去查一把其实没错的密钥。
    //
    // 2026-09-21 实撞：两家的密钥都存好了，从 DeepSeek 切回智谱，模型列表
    // 空着说 401；同一把钥匙直接 curl 智谱的 `/models` 是 200。
    changji::test::ScopedUserConfigDir iso("models-per-url-key");

    const std::string a = "https://a.example.com/v1";
    const std::string b = "https://b.example.com/v1";
    config::write_api_key_for(a, "key-a");
    config::write_api_key_for(b, "key-b");

    config::Settings s = test_settings();
    s.llm.base_url = a;
    s.llm.api_key = "key-a";

    SUBCASE("问另一家，换成另一家那把") {
        FakeGet g;
        g.reply = ok_body({{"data", json::array()}});
        http::post_llm_models(json{{"base_url", b}}, s, g.fn());
        REQUIRE(g.urls.size() == 1);
        CHECK(g.urls.at(0) == b + "/models");
        CHECK(g.auths.at(0) == "Bearer key-b");
    }

    SUBCASE("没存过那一家就空着——那时候 401 正是实情") {
        FakeGet g;
        g.reply = ok_body({{"data", json::array()}});
        http::post_llm_models(json{{"base_url", "https://never.seen/v1"}}, s,
                              g.fn());
        REQUIRE(g.auths.size() == 1);
        CHECK(g.auths.at(0) == "Bearer ");
    }

    SUBCASE("身上带着一把的优先（设置页里刚敲进框、还没存下的那把）") {
        FakeGet g;
        g.reply = ok_body({{"data", json::array()}});
        http::post_llm_models(json{{"base_url", b}, {"api_key", "手敲的"}}, s,
                              g.fn());
        REQUIRE(g.auths.size() == 1);
        CHECK(g.auths.at(0) == "Bearer 手敲的");
    }

    SUBCASE("没换地址就不动钥匙") {
        FakeGet g;
        g.reply = ok_body({{"data", json::array()}});
        http::post_llm_models(json{{"base_url", a}}, s, g.fn());
        REQUIRE(g.auths.size() == 1);
        CHECK(g.auths.at(0) == "Bearer key-a");
    }
}

TEST_CASE("平台清单和 Python 一致") {
    const auto r = http::get_llm_providers();
    CHECK(r.status == 200);
    REQUIRE(r.body.at("providers").is_array());
    // 这份清单是从 Python 的 LLM_PROVIDERS 生成的，不是手抄的。
    // 十几家平台每家四个字段，手抄必然错，而且各家地址会变——
    // 手抄的那份只会越来越旧且没有任何迹象提示它旧了。
    CHECK(r.body.at("providers").size() >= 15);

    for (const auto& p : r.body.at("providers")) {
        CAPTURE(p.at("id").get<std::string>());
        // 前端靠 id 匹配"当前地址是哪一家"，靠 base_url 一键填入
        CHECK(p.contains("id"));
        CHECK(p.contains("name"));
        CHECK(p.contains("base_url"));
        CHECK(p.contains("local"));
        CHECK(p.contains("note"));
        CHECK(p.at("local").is_boolean());
        const std::string url = p.at("base_url").get<std::string>();
        CHECK(url.rfind("http", 0) == 0);
    }

    SUBCASE("本机那几家标了 local") {
        bool found_ollama = false;
        for (const auto& p : r.body.at("providers")) {
            if (p.at("id") == "ollama") {
                found_ollama = true;
                CHECK(p.at("local") == true);
                CHECK(p.at("base_url") == "http://127.0.0.1:11434/v1");
            }
        }
        CHECK(found_ollama);
    }
}

TEST_CASE("模型列表：拿得到的时候") {
    FakeGet get;
    get.reply = ok_body(json{{"data", json::array({
        json{{"id", "qwen3:14b"}},
        json{{"id", "llama3:8b"}},
        json{{"id", "qwen3:14b"}},   // 重复的要去掉
    })}});

    const auto r = http::get_llm_models(test_settings(), get.fn());
    CHECK(r.status == 200);
    // 去重且排序，对应 Python 的 sorted(set(names))
    CHECK(r.body.at("models") == json::array({"llama3:8b", "qwen3:14b"}));
    CHECK(r.body.at("current") == "qwen3:14b");
    // 成功时**没有 error 键**。形状是契约。
    CHECK_FALSE(r.body.contains("error"));

    REQUIRE(get.urls.size() == 1);
    CHECK(get.urls[0] == "http://127.0.0.1:11434/v1/models");
    // 超时写死 10 秒，不用 llm.timeout_s（那个默认 300 秒）。
    // 拿 300 秒来问一个列表，服务不在的时候设置页会转五分钟圈。
    CHECK(get.timeouts[0] == 10.0);
}

TEST_CASE("模型列表：拿不到的三种情况") {
    // 三种都返回**空列表加一句原因**，不抛异常。
    // 列不出来不该让整个设置页打不开——界面退回手打就行。
    const auto check_failed = [](const llm::HttpResponse& reply,
                                 const std::string& want_in_error) {
        FakeGet get;
        get.reply = reply;
        const auto r = http::get_llm_models(test_settings(), get.fn());
        CHECK(r.status == 200);
        CHECK(r.body.at("models") == json::array());
        REQUIRE(r.body.contains("error"));
        const std::string err = r.body.at("error").get<std::string>();
        CAPTURE(err);
        CHECK(err.find(want_in_error) != std::string::npos);
        // 失败时**没有 current 键**，和 Python 一致
        CHECK_FALSE(r.body.contains("current"));
    };

    llm::HttpResponse down;
    down.status = 0;
    down.transport_error = "Connection refused";
    check_failed(down, "连不上");
    check_failed(llm::HttpResponse{500, "{}", std::nullopt}, "返回 500");
    check_failed(llm::HttpResponse{200, "<html>", std::nullopt}, "不是 JSON");
}

TEST_CASE("模型列表能认几种返回形状") {
    // 各家服务的 /models 返回不完全一样。认不出来就是空列表，
    // 而空列表在界面上和"服务没起来"长得一样，用户会去查错的地方。
    const auto names = [](const json& reply) {
        FakeGet get;
        get.reply = ok_body(reply);
        return http::get_llm_models(test_settings(), get.fn()).body.at("models");
    };

    // 标准 OpenAI：{"data": [{"id": ...}]}
    CHECK(names(json{{"data", json::array({json{{"id", "a"}}})}}) ==
          json::array({"a"}));
    // 顶层直接是数组
    CHECK(names(json::array({json{{"id", "b"}}})) == json::array({"b"}));
    // 数组里是纯字符串
    CHECK(names(json::array({"c", "d"})) == json::array({"c", "d"}));
    // data 是空的
    CHECK(names(json{{"data", json::array()}}) == json::array());
    // 完全不认识的形状，空列表但不崩
    CHECK(names(json{{"whatever", 1}}) == json::array());
}

TEST_CASE("本机 gguf 列表已经空了，但字段还在") {
    // **2026-09-14 把进程内后端整个删了**，本机那份 gguf 一个都选不了，
    // 所以这一段不再去扫目录——摆出来只会让人以为还能在本机跑。
    //
    // **字段本身留着**：少一个键会让还没更新的页面在取值时炸，
    // 而这一层没法知道对面是新版还是旧版。
    const fs::path dir = make_models_dir(
        "列表", {"Qwen3-14B-Q4_K_M.gguf", "llama3.gguf", "readme.txt",
                 "Wan2.2.GGUF", "不是模型.safetensors"});

    FakeGet get;
    get.reply = ok_body(json{{"data", json::array({json{{"id", "远端模型"}}})}});
    const auto r = http::get_llm_models(test_settings(dir), get.fn());

    // 远端那份原样在
    CHECK(r.body.at("models") == json::array({"远端模型"}));
    CHECK(r.body.at("current") == "qwen3:14b");

    // 本机那份是空的，但三个键都在——目录里明明有 gguf 也不列
    const json& local = r.body.at("local");
    CHECK(local.at("dir") == "");
    CHECK(local.at("current") == "");
    CHECK(local.at("files") == json::array());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("远端连不上时 local 字段照样在") {
    // 键少一个会让页面在取值时炸，所以连不上也要把这个空壳给出去。
    const fs::path dir = make_models_dir("离线", {"a.gguf"});
    FakeGet get;
    get.reply.status = 0;
    get.reply.transport_error = "Connection refused";

    const auto r = http::get_llm_models(test_settings(dir), get.fn());
    CHECK(r.body.at("models") == json::array());
    CHECK(r.body.contains("error"));
    CHECK(r.body.at("local").at("files") == json::array());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("模型目录不存在也不报错") {
    // 装好程序还没下模型是常态。这时候返回空列表，
    // 不该让设置页打不开。
    config::Settings s = test_settings();
    s.models.dir = "Z:/这个目录不存在";
    FakeGet get;
    get.reply = ok_body(json{{"data", json::array()}});
    const auto r = http::get_llm_models(s, get.fn());
    CHECK(r.status == 200);
    CHECK(r.body.at("local").at("files") == json::array());
}

TEST_CASE("认识的那几家带一本小抄：known") {
    // **为什么非有这个不可**（2026-09-13 实测）：智谱的 /models
    // 只回 glm-4.5 / 4.5-air / 4.6 / 4.7 / 5 / 5-turbo / 5.1 / 5.2 /
    // 5.3 / 5.3-flash——**glm-4.7-flash 不在里面而它能用**，
    // 而它正好是我们的默认。只照 /models 渲染下拉的话，
    // 默认那个模型在自己的下拉里是找不到的。
    config::Settings s = test_settings();
    s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4";
    s.llm.model = "glm-4.7-flash";

    FakeGet get;
    // 照着实测的样子回——**故意不含 glm-4.7-flash**。
    get.reply = ok_body(json{{"data", json::array({
                                 json{{"id", "glm-4.6"}},
                                 json{{"id", "glm-5.3"}},
                             })}});
    const auto r = http::get_llm_models(s, get.fn());

    // models 仍然只装服务真答应的那些：这两个字段**不合并**。
    // 混进小抄之后，前端那句"这台服务上没有 X"就会在模型真的不存在时
    // 也不吭声。合并是前端的事。
    CHECK(r.body.at("models") == json::array({"glm-4.6", "glm-5.3"}));

    REQUIRE(r.body.contains("known"));
    const json& known = r.body.at("known");
    REQUIRE(known.is_array());
    REQUIRE(!known.empty());

    std::vector<std::string> ids;
    for (const auto& k : known) {
        CHECK(k.contains("id"));
        // **每一项都得有一句话。** 一串 glm-4.5/4.6/4.7/5/5.1/5.2/5.3
        // 摆在那儿，要紧的两件事——哪个不要钱、哪个会写——名字上一个字
        // 都看不出来。
        CHECK(k.contains("note"));
        CHECK_FALSE(k.at("note").get<std::string>().empty());
        ids.push_back(k.at("id").get<std::string>());
    }
    // 头一个是推荐顺序的头一个，前端换家时就挑它，所以别随手改顺序。
    CHECK(ids.front() == "glm-4.7-flash");
    CHECK(std::find(ids.begin(), ids.end(), "glm-5.3") != ids.end());

    SUBCASE("z.ai 是同一套后端，同样给") {
        s.llm.base_url = "https://api.z.ai/api/paas/v4";
        FakeGet g2;
        g2.reply = ok_body(json{{"data", json::array()}});
        CHECK_FALSE(http::get_llm_models(s, g2.fn()).body.at("known").empty());
    }

    SUBCASE("不认识的家给空的，不瞎猜") {
        // 这是本我们自己维护的小抄，不是模型总表。认不出的地址上
        // 编几个名字出来，比不给更糟。
        FakeGet g2;
        g2.reply = ok_body(json{{"data", json::array()}});
        const auto r2 = http::get_llm_models(test_settings(), g2.fn());
        CHECK(r2.body.at("known") == json::array());
    }
}

TEST_CASE("连不上也要给小抄") {
    // 刚装好还没填密钥时 /models 必然 401，**而那正是最需要
    // 「这家都有什么、该挑哪个」的时候**。这时候把下拉渲染成空的，
    // 人只能回去手打一个自己也不确定的名字。
    config::Settings s = test_settings();
    s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4";

    for (const int status : {401, 429, 500}) {
        CAPTURE(status);
        FakeGet get;
        get.reply = llm::HttpResponse{status, "{}", std::nullopt};
        const auto r = http::get_llm_models(s, get.fn());
        // 失败时的老契约一个字不动：空 models、有 error、没有 current。
        CHECK(r.body.at("models") == json::array());
        CHECK(r.body.contains("error"));
        CHECK_FALSE(r.body.contains("current"));
        // 新加的这份照给。
        CHECK_FALSE(r.body.at("known").empty());
    }

    SUBCASE("连都连不上也一样") {
        FakeGet get;
        get.reply.status = 0;
        get.reply.transport_error = "Connection refused";
        const auto r = http::get_llm_models(s, get.fn());
        CHECK(r.body.at("models") == json::array());
        CHECK_FALSE(r.body.at("known").empty());
    }
}

// ---------------------------------------------------------------------------
// 「少想一点」只发给认得它的模型
//
// 2026-09-17 加的。智谱的 reasoning_effort 默认是 max，而拆分镜那一步在思考
// 里把整张分镜表逐镜写完了，再用 JSON 写一遍——同一份东西写两遍。
//
// ⚠️ 这儿钉的是**别发错家**：这是智谱家的参数，别家收到不认识的字段可能
// 直接 400，而那会让所有生成一起挂，比"想得久"严重得多。
// ---------------------------------------------------------------------------

TEST_CASE("reasoning_effort：认得的模型才发") {
    const auto payload = [](const std::string& model, const std::string& effort) {
        changji::config::LLMConfig cfg;
        cfg.model = model;
        changji::llm::Request req;
        req.prompt = "写点什么";
        req.reasoning_effort = effort;
        return changji::llm::build_payload(cfg, req);
    };

    SUBCASE("glm-5 那几个收") {
        for (const char* m : {"glm-5.3-flash", "glm-5.3", "glm-5.2-air", "GLM-5.3"}) {
            CAPTURE(m);
            const auto p = payload(m, "high");
            CHECK(p.contains("reasoning_effort"));
            CHECK(p["reasoning_effort"] == "high");
            // thinking 要一起开，不然 effort 那一项不生效（文档：
            // "thinking 开启时生效"）。
            REQUIRE(p.contains("thinking"));
            CHECK(p["thinking"]["type"] == "enabled");
        }
    }

    SUBCASE("别家一个字段都不多发") {
        for (const char* m : {"deepseek-reasoner", "qwen3-32b", "moonshot-v1-8k",
                              "glm-4.5-air", "gpt-4o"}) {
            CAPTURE(m);
            const auto p = payload(m, "high");
            CHECK_FALSE(p.contains("reasoning_effort"));
            CHECK_FALSE(p.contains("thinking"));
        }
    }

    SUBCASE("req 不填就问配置——2026-09-19 起「不填」不再等于「不发」") {
        // 原来这条叫「不填就什么都不发」。那天起档位从各步代码里搬进了配置
        // （`[llm] reasoning_effort`，默认 high），于是 `req` 空着的意思从
        // "不发" 变成了 "按配置来"——出大纲那几步一直没人写，靠的就是这条
        // 兜底（在那之前它们走的是智谱的默认 max，一趟十几分钟不回）。
        changji::config::LLMConfig cfg;
        cfg.model = "glm-5.3-flash";
        REQUIRE(cfg.reasoning_effort == "high");
        changji::llm::Request req;
        req.prompt = "写点什么";
        const auto p = changji::llm::build_payload(cfg, req);
        CHECK(p.at("reasoning_effort") == "high");
    }

    SUBCASE("配置也清空了才是真的一个字段都不发") {
        changji::config::LLMConfig cfg;
        cfg.model = "glm-5.3-flash";
        cfg.reasoning_effort = "";
        changji::llm::Request req;
        req.prompt = "写点什么";
        const auto p = changji::llm::build_payload(cfg, req);
        CHECK_FALSE(p.contains("reasoning_effort"));
        CHECK_FALSE(p.contains("thinking"));
    }
}

// ---------------------------------------------------------------------------
// 「只看不发」拿到的，必须就是真发出去的那一份
//
// 用户 2026-09-17 要"复制提示词拿到别处去跑，再把内容粘回来"。复制出去的
// 那段字差一个字，他在别处跑出来的东西就对不上我们的解析器——而那种错没有
// 任何报错：要么"粘回来解析失败"，要么更糟，"解析成功但内容不是我要的"。
//
// 所以这儿钉的是 peek_prompt 用的就是 schema_as_prompt，而不是另拼一份。
// ---------------------------------------------------------------------------

TEST_CASE("只看不发：回的就是 schema_as_prompt 拼的那一份") {
    changji::llm::Request req;
    req.prompt = "把这段话改成剧本";
    req.schema_name = "script";
    req.schema = nlohmann::json{{"type", "object"},
                                {"properties", {{"text", {{"type", "string"}}}}}};

    const auto r = changji::http::peek_prompt(req);
    CHECK(r.status == 200);
    CHECK(r.body["peek"] == true);
    CHECK(r.body["stage"] == "script");
    CHECK(r.body["prompt"] ==
          changji::llm::schema_as_prompt(req.prompt, req.schema));
    // schema 是贴在提示词后面的文字（不走 response_format），所以字段名要在。
    CHECK(r.body["prompt"].get<std::string>().find("把这段话改成剧本") !=
          std::string::npos);
    CHECK(r.body["prompt"].get<std::string>().find("\"text\"") !=
          std::string::npos);
}

TEST_CASE("只看不发：空 schema 不二次包壳") {
    // 拆分镜那条把三场拼好的全文当 prompt 传进来、schema 留空，
    // 再包一层"只输出一个 JSON 对象"的话，复制出去的那段就多了一段废话。
    changji::llm::Request req;
    req.prompt = "===== 第 1/3 场 =====\n已经拼好的那一大段";
    req.schema_name = "storyboard";
    const auto r = changji::http::peek_prompt(req);
    CHECK(r.body["prompt"] == req.prompt);
}

TEST_CASE("只看不发：take_peek 取完要把那个键删掉") {
    // 不删的话下游的 forbid_extra 会把整个请求拒成 422——而那时候界面上
    // 只会说"多了一个字段"，看不出和这颗按钮有关系。
    nlohmann::json body{{"project", "/tmp/p"}, {"peek", true}};
    CHECK(changji::http::take_peek(body));
    CHECK_FALSE(body.contains("peek"));

    nlohmann::json off{{"project", "/tmp/p"}, {"peek", false}};
    CHECK_FALSE(changji::http::take_peek(off));
    CHECK_FALSE(off.contains("peek"));

    nlohmann::json none{{"project", "/tmp/p"}};
    CHECK_FALSE(changji::http::take_peek(none));
}

// ---------------------------------------------------------------------------
// 粘回来那一大段按场次头切开
//
// 拆分镜是**按场跑**的，一章三场就是三份提示词、三份结果。复制出去那份带
// 「===== 第 N/M 场 …… =====」的头，粘回来按同一个头切回去。
//
// ⚠️ 切错的后果不报错：少一段的话后面几场整体错位一场，而错位出来的分镜表
// 看着是合法的，人要等到出片才发现第二场的画面配着第三场的台词。
// ---------------------------------------------------------------------------

TEST_CASE("按场次头把粘回来的那段切开") {
    const std::string blob =
        "===== 第 1/3 场：夜 · 外 · 后门货场 =====\n"
        "{\"shots\":[1]}\n"
        "===== 第 2/3 场：日 · 外 · 后门货场 =====\n"
        "{\"shots\":[2]}\n"
        "===== 第 3/3 场：日 · 内 · 传达室 =====\n"
        "{\"shots\":[3]}\n";
    const auto parts = changji::http::split_by_scene(blob);
    REQUIRE(parts.size() == 3);
    CHECK(parts[0].find("[1]") != std::string::npos);
    CHECK(parts[1].find("[2]") != std::string::npos);
    CHECK(parts[2].find("[3]") != std::string::npos);
    // 头那一行本身不能留在内容里——它不是 JSON，解析器会当垃圾。
    CHECK(parts[0].find("=====") == std::string::npos);

    SUBCASE("一个头都没有：整段当一段") {
        // 整章一次拆那条路只有一段，那时候不该逼人去写一个分隔头。
        const auto one = changji::http::split_by_scene("{\"shots\":[]}");
        REQUIRE(one.size() == 1);
        CHECK(one[0] == "{\"shots\":[]}");
    }

    SUBCASE("空的就是空的，不要凭空造一段") {
        CHECK(changji::http::split_by_scene("").empty());
    }

    SUBCASE("一份 JSON 数组：按 scene 对齐，不靠位置") {
        // 用户 2026-09-18：「把每一场的 json 合并成 json 数组」。
        // 故意把第 2 场写在前面——文本头那条路会整体错位一场，这条路靠
        // 每一项自带的场号排回去。
        const auto p = changji::http::split_by_scene(
            "[{\"scene\":2,\"shots\":[2]},{\"scene\":1,\"shots\":[1]},"
            "{\"scene\":3,\"shots\":[3]}]");
        REQUIRE(p.size() == 3);
        CHECK(p[0].find("[1]") != std::string::npos);
        CHECK(p[1].find("[2]") != std::string::npos);
        CHECK(p[2].find("[3]") != std::string::npos);
        // 每一段重新包成 {"shots":[...]}，下游逐场解析那条路一字不改
        CHECK(p[0].rfind("{\"shots\"", 0) == 0);
    }

    SUBCASE("{\"scenes\":[...]} 包一层也认") {
        const auto p = changji::http::split_by_scene(
            "{\"scenes\":[{\"scene\":1,\"shots\":[1]},{\"scene\":2,\"shots\":[2]}]}");
        REQUIRE(p.size() == 2);
        CHECK(p[1].find("[2]") != std::string::npos);
    }

    SUBCASE("没写 scene 的按原顺序") {
        const auto p = changji::http::split_by_scene(
            "[{\"shots\":[7]},{\"shots\":[8]}]");
        REQUIRE(p.size() == 2);
        CHECK(p[0].find("[7]") != std::string::npos);
    }

    SUBCASE("单场的 {\"shots\":[...]} 不是数组形状，照旧当一段") {
        // 整章一次拆那条路粘回来就是这个样子，不能被数组那条路误吃。
        const auto p = changji::http::split_by_scene("{\"shots\":[5]}");
        REQUIRE(p.size() == 1);
        CHECK(p[0] == "{\"shots\":[5]}");
    }

    SUBCASE("数组里缺 shots 的不算这种形状，回落到文本头") {
        const auto p = changji::http::split_by_scene("[{\"scene\":1}]");
        // 文本头一个都没有 → 整段当一段（老行为）
        REQUIRE(p.size() == 1);
    }

    SUBCASE("头之前的话不算进任何一场") {
        // 人从聊天窗口抄回来时前面常带一句"好的，这是结果："。
        const auto p = changji::http::split_by_scene(
            "好的，这是结果：\n===== 第 1/1 场：夜 =====\n{\"shots\":[9]}\n");
        REQUIRE(p.size() == 1);
        CHECK(p[0].find("好的") == std::string::npos);
        CHECK(p[0].find("[9]") != std::string::npos);
    }
}
