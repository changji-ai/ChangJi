// 大模型客户端的测试。
//
// HTTP 是假的——注入一个记账的回调。测的是三件纯逻辑：
// 请求怎么拼、错误怎么翻成人话、返回怎么抽内容。
//
// 这三件事里第二件最容易被当成"不重要"而写马虎，但它恰恰是用户最常撞上的：
// 地址填错、模型名写错、密钥过期。Python 那边一开始就是 raise_for_status()
// 了事，结果这类失败一路穿到最外面变成一句「Internal Server Error」，
// 用户完全不知道该去改什么。stages/_llm.py 那个模块就是为此加的。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "llm/local_client.hpp"
#include "llm/schema_validate.hpp"
#include "pipeline/jobs.hpp"

#include "scoped_env.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

config::LLMConfig test_cfg() {
    config::LLMConfig c;
    c.base_url = "http://127.0.0.1:11434/v1";
    c.model = "qwen3:14b";
    c.api_key = "ollama";
    c.temperature = 0.7;
    c.timeout_s = 300.0;
    return c;
}

/// 默认那套（OpenRouter + 按任务分流），只是补上密钥。
/// 默认那家云服务（2026-09-14 起是智谱 bigmodel.cn）+ 一把密钥。
///
/// **地址不写死在这儿**，跟着 `LLMConfig` 的默认走：这个 fixture 要问的
/// 一直是"默认配置下发出去的是什么"，换家的时候该跟着变的正是它。
config::LLMConfig test_cfg_cloud() {
    config::LLMConfig c;
    c.api_key = "测试密钥";
    return c;
}

/// 记下每次请求，按顺序吐出预设的响应。
struct FakeHttp {
    struct Call {
        std::string url;
        json body;
        std::map<std::string, std::string> headers;
        double timeout_s = 0;
    };
    std::vector<Call> calls;
    std::vector<llm::HttpResponse> responses;

    llm::HttpPost fn() {
        return [this](const std::string& url, const std::string& body,
                      const std::map<std::string, std::string>& headers,
                      double timeout_s) -> llm::HttpResponse {
            calls.push_back(
                Call{url, json::parse(body, nullptr, false), headers, timeout_s});
            if (calls.size() <= responses.size()) return responses[calls.size() - 1];
            return llm::HttpResponse{500, "{}", std::nullopt};
        };
    }
};

llm::HttpResponse ok(const std::string& content) {
    const json body = {
        {"choices", json::array({json{{"message", {{"content", content}}}}})}};
    return llm::HttpResponse{200, body.dump(), std::nullopt};
}

/// 把 `llm_log` 那棵树里的 `index.jsonl` 全读出来。
///
/// **项目子目录名不写死。** 这几条用例跑在没有活、没有项目的线程上，落的是
/// `_无项目`——把那个名字抄到用例里的话，哪天那个兜底名一改，这儿红的地方
/// 会是"文件不存在"，看不出是改了名。
std::vector<json> read_log_index(const std::filesystem::path& root) {
    std::vector<json> rows;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().filename() != "index.jsonl") {
            continue;
        }
        std::ifstream f(it->path(), std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) rows.push_back(json::parse(line));
        }
    }
    return rows;
}

/// 那棵树里每个文件的名字和内容接成一大串。
/// 问的是"这个字有没有落到盘上"，问哪个文件里无关紧要。
std::string slurp_log_tree(const std::filesystem::path& root) {
    std::string all;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        all += paths::to_utf8(it->path().filename()) + "\n";
        std::ifstream f(it->path(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        all += ss.str() + "\n";
    }
    return all;
}

llm::Request simple_req() {
    llm::Request r;
    r.prompt = "写一章剧本";
    r.schema = nlohmann::ordered_json{{"type", "object"}};
    r.schema_name = "script";
    r.temperature = 0.7;
    return r;
}

}  // namespace

TEST_CASE("请求体的形状：schema 贴在提示词里，不发 response_format") {
    // **2026-09-14 起只有这一种发法。** 原来挂着一部四档退档梯子
    // （json_schema → 削过的 schema → json_object → 什么都不发），为的是
    // 兜住各家对 response_format 支持得七零八落。它的代价一直很实在：
    // 任何一个别的 400（比如 glm-5.3 不收 thinking 的关闭值）都会被它读成
    // "这家不支持 json_schema"，于是**悄悄**退到最宽那一档接着生成，
    // 日志上看一切正常——实跑十次全中。
    const auto cfg = test_cfg();
    const json p = json(llm::build_payload(cfg, simple_req()));

    CHECK(p.at("model") == "qwen3:14b");
    CHECK(p.at("temperature") == 0.7);
    REQUIRE(p.at("messages").is_array());
    REQUIRE(p.at("messages").size() == 1);
    CHECK(p.at("messages")[0].at("role") == "user");
    // 一个字段都不发
    CHECK_FALSE(p.contains("response_format"));

    SUBCASE("schema 以文字接在提示词后面") {
        const std::string content =
            p.at("messages")[0].at("content").get<std::string>();
        CHECK(content.rfind("写一章剧本", 0) == 0);   // 提示词本身在最前面
        CHECK(content.find("\"type\": \"object\"") != std::string::npos);
        CHECK(content.size() > std::string("写一章剧本").size());
    }

    SUBCASE("没给 schema 就只发提示词本身") {
        llm::Request r = simple_req();
        r.schema = nlohmann::ordered_json();
        const json q = json(llm::build_payload(cfg, r));
        CHECK(q.at("messages")[0].at("content") == "写一章剧本");
        CHECK_FALSE(q.contains("response_format"));
    }
}

TEST_CASE("温度：req 没意见就用配置里的，有意见就听它的") {
    // **这一条钉的是一个真空转过的旋钮。** build_payload 原来发的是
    // cfg.temperature，req 里那个从来没人读——于是 kChapterTemperature
    // （写正文 0.5）在远端那条路上一直没生效，而远端正是现在的默认后端。
    // 两条路都返回 200，所以这件事只能靠读代码发现。
    config::LLMConfig cfg = test_cfg();
    cfg.temperature = 0.35;

    SUBCASE("空着：用用户在设置页上定的那个") {
        llm::Request r = simple_req();
        r.temperature.reset();
        const json p = json(llm::build_payload(cfg, r));
        CHECK(p.at("temperature") == doctest::Approx(0.35));
    }

    SUBCASE("填了：这一步说了算") {
        llm::Request r = simple_req();
        r.temperature = 0.5;
        const json p = json(llm::build_payload(cfg, r));
        CHECK(p.at("temperature") == doctest::Approx(0.5));
    }
}

TEST_CASE("温度按任务分档：编东西的放开，拆结构的收紧") {
    // **不是每一步都该跑同一个温度。** 大纲和选题是从无到有编东西，温度低了
    // 永远是那几个套路（用户的判词「每次写文章的内容都差不多」）；分镜和定妆
    // 是把已有的东西转成结构，发挥在那儿一律是错——编一个剧本里没有的道具，
    // 后面每一镜都得跟着它错下去。
    //
    // 做成相对偏移而不是绝对值表：绝对值表会把设置页那个旋钮架空。
    config::LLMConfig cfg;
    cfg.temperature = 0.7;

    CHECK(cfg.temperature_for("story_outline") == doctest::Approx(0.95));
    CHECK(cfg.temperature_for("premises") == doctest::Approx(0.95));
    CHECK(cfg.temperature_for("trailer") == doctest::Approx(0.95));
    CHECK(cfg.temperature_for("storyboard") == doctest::Approx(0.28));
    CHECK(cfg.temperature_for("bible") == doctest::Approx(0.28));
    CHECK(cfg.temperature_for("story_analysis") == doctest::Approx(0.28));
    // 认不出的、以及写剧本写正文那几步，跟着基准走
    CHECK(cfg.temperature_for("script") == doctest::Approx(0.7));
    CHECK(cfg.temperature_for("chapter") == doctest::Approx(0.7));
    CHECK(cfg.temperature_for("") == doctest::Approx(0.7));

    SUBCASE("用户那个旋钮还推得动所有档") {
        cfg.temperature = 0.3;
        CHECK(cfg.temperature_for("story_outline") == doctest::Approx(0.55));
        CHECK(cfg.temperature_for("storyboard") == doctest::Approx(0.12));
        CHECK(cfg.temperature_for("script") == doctest::Approx(0.3));
    }

    SUBCASE("发散那档封在 1.1：再高多数服务开始吐坏 JSON") {
        cfg.temperature = 1.4;
        CHECK(cfg.temperature_for("story_outline") == doctest::Approx(1.1));
    }

    SUBCASE("真发出去的就是这个数") {
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        config::LLMConfig c = test_cfg();
        c.temperature = 0.7;
        llm::RemoteClient rc(c, http.fn());
        pipeline::CancelToken tok;
        llm::Request r = simple_req();
        r.temperature.reset();
        r.schema_name = "story_outline";
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("temperature").get<double>() ==
              doctest::Approx(0.95));
    }

    SUBCASE("这一步自己填了温度的，分档管不着它") {
        // 写正文那步有自己测出来的数（kChapterTemperature），别被档位盖掉。
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        llm::RemoteClient rc(test_cfg(), http.fn());
        pipeline::CancelToken tok;
        llm::Request r = simple_req();
        r.temperature = 0.5;
        r.schema_name = "storyboard";
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("temperature").get<double>() ==
              doctest::Approx(0.5));
    }
}

TEST_CASE("哪些地址要 API Key") {
    // 判错的代价不对称：把云当成本机，用户会被初始化页放过去、体检也报绿，
    // 然后在第一次写剧本时撞上 401；反过来只是多提示一句。所以认不准就当要。
    config::LLMConfig c;

    SUBCASE("默认那一档是智谱，必须自己填密钥") {
        // **这里曾经内置过一把智谱的免费密钥，2026-09-13 用户要求删掉。**
        // 别再往回加：密钥明文编进二进制，strings 一抓就有，也会进 git。
        // ——2026-09-14 默认又换回智谱了，但**密钥这件事不跟着回来**：
        // 默认那个 glm-4.7-flash 不要钱，人还是得自己去领一把。
        CHECK(c.base_url.find("bigmodel.cn") != std::string::npos);
        CHECK(c.backend == "remote");
        CHECK(c.api_key.empty());
        CHECK(c.needs_api_key());
    }

    SUBCASE("本机和局域网的不要") {
        for (const char* url : {"http://127.0.0.1:11434/v1",
                                "http://localhost:1234/v1",
                                "http://192.168.1.20:8000/v1",
                                "http://10.0.0.5:8000/v1"}) {
            CAPTURE(url);
            c.base_url = url;
            CHECK_FALSE(c.needs_api_key());
        }
    }

    SUBCASE("进程内那条压根不用问") {
        c.backend = "local";
        CHECK_FALSE(c.needs_api_key());
    }

    SUBCASE("别的云都要") {
        for (const char* url : {"https://api.deepseek.com/v1",
                                "https://api.siliconflow.cn/v1",
                                "https://api.z.ai/api/paas/v4"}) {
            CAPTURE(url);
            c.base_url = url;
            CHECK(c.needs_api_key());
        }
    }

    SUBCASE("发出去的 Authorization 头就是配置里那把") {
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        config::LLMConfig mine = test_cfg();
        mine.api_key = "sk-or-v1-测试";
        llm::RemoteClient rc(mine, http.fn());
        pipeline::CancelToken tok;
        rc.complete(simple_req(), tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].headers.at("Authorization") == "Bearer sk-or-v1-测试");
    }
}

TEST_CASE("密钥跟着地址走：一把钥匙对一家服务") {
    // **2026-09-20 实撞**：加腾讯那条通道时把 base_url 换了、密钥没换，
    // 回的是 401——而 401 长得像"这个模型不让你用"，人会去查一把其实
    // 没错的密钥。原来只有一个 `api_key` 文件，换地址时旧钥匙还留在那儿。
    changji::test::ScopedUserConfigDir iso("apikey-per-url");

    const std::string zhipu = "https://open.bigmodel.cn/api/paas/v4";
    const std::string tx = "https://api.lkeap.cloud.tencent.com/plan/v3";

    SUBCASE("各存各的，换回去还在") {
        config::write_api_key_for(zhipu, "key-zhipu");
        config::write_api_key_for(tx, "key-tx");
        CHECK(config::read_api_key_for(zhipu) == "key-zhipu");
        CHECK(config::read_api_key_for(tx) == "key-tx");
        // 两家不能落到同一个文件上，不然后写的会盖掉先写的。
        CHECK(config::api_key_path_for(zhipu) != config::api_key_path_for(tx));
    }

    SUBCASE("协议和结尾斜杠不算区别") {
        // `http://x/v1` 和 `https://x/v1/` 是同一家；存成两份的话换一次
        // 协议密钥就"丢"了。
        config::write_api_key_for("https://a.example.com/v1", "k");
        CHECK(config::read_api_key_for("http://a.example.com/v1") == "k");
        CHECK(config::read_api_key_for("https://a.example.com/v1/") == "k");
    }

    SUBCASE("这一家没存过就退回老的单文件") {
        // 升上来的用户手里那把钥匙不能因为换了存法就消失。
        config::write_api_key_file("legacy-key");
        CHECK(config::read_api_key_for("https://never.seen/v1") == "legacy-key");
    }

    SUBCASE("问「哪几家填过」不能用 read_api_key_for 判空") {
        // ⚠️ `read_api_key_for` **有意退回那个老的单文件**（上面那条），
        // 于是**只要填过任何一家，问谁都说填过**。
        //
        // 2026-09-21 实撞：界面上那条厂商单子本该只摆填过密钥的几家
        //（没填的摆出来只有一个用处——让人挑中之后挨一个 401，而 401 长得像
        // "这个模型不让你用"），结果十五家全摆着。
        config::write_api_key_file("legacy-key");
        config::write_api_key_for(zhipu, "key-zhipu");

        CHECK(config::has_own_api_key(zhipu));
        CHECK_FALSE(config::has_own_api_key(tx));
        // ⚠️ **这时候 `read_api_key_for(tx)` 是空的**（上一条那个 SUBCASE
        // 讲的就是为什么）：已经有一家单独存过了，老的那把不再"问谁都给"。
        // 两个函数都得判对，而它们判的不是一回事。
        CHECK(config::read_api_key_for(tx).empty());

        // 空白不算填过（写空串是删掉）。
        config::write_api_key_for(zhipu, "");
        CHECK_FALSE(config::has_own_api_key(zhipu));
    }

    SUBCASE("清空就是删掉这一家，不碰别家") {
        config::write_api_key_for(zhipu, "key-zhipu");
        config::write_api_key_for(tx, "key-tx");
        config::write_api_key_for(tx, "");
        CHECK(config::read_api_key_for(zhipu) == "key-zhipu");
        // tx 这一家没了，于是退回老单文件（这儿是空的）
        CHECK(config::read_api_key_for(tx).empty());
    }
}

TEST_CASE("升上来那把钥匙一次性搬到配置里指着的那一家名下") {
    // 老版本只有一个单文件，而那把钥匙属于**它当时指着的那一家**。
    // 不搬的话它会一直当"问谁都给"的兜底。搬只发生一次（`api_keys/` 一空
    // 才搬），所以后来换的那几家不会被它污染。
    changji::test::ScopedUserConfigDir iso("apikey-migrate");

    config::write_api_key_file("legacy-key");
    CHECK_FALSE(config::any_own_api_key());

    const auto s = config::load_settings();
    CHECK(s.llm.api_key == "legacy-key");
    // 搬过去了：这一家名下自己有一份。
    CHECK(config::has_own_api_key(s.llm.base_url));
    CHECK(config::any_own_api_key());
    // 搬完之后，没配过的那几家拿不到它。
    CHECK(config::read_api_key_for("https://never.seen/v1").empty());

    // 再读一次不会再搬，也不会把别家的覆盖掉。
    config::write_api_key_for("https://other.example/v1", "key-other");
    const auto again = config::load_settings();
    CHECK(again.llm.api_key == "legacy-key");
    CHECK(config::read_api_key_for("https://other.example/v1") == "key-other");
}

TEST_CASE("密钥单独一个文件，不混进 config.toml") {
    // **config.toml 那个文件到处跑**：部署是整包 tar 推到服务器，排查问题
    // 时整份贴进聊天窗口。密钥混在里面的话每一次都是一次泄漏，而且泄漏时
    // 没有任何迹象。所以它单独一个文件，单独存、单独换、单独 gitignore。
    changji::test::ScopedUserConfigDir iso("apikey");

    CHECK(config::user_api_key_path().parent_path() ==
          config::user_config_path().parent_path());
    CHECK(config::user_api_key_path().filename() == "api_key");

    SUBCASE("没有那个文件就是空的，不报错") {
        CHECK(config::read_api_key_file().empty());
    }

    SUBCASE("写进去能原样读回来") {
        config::write_api_key_file("sk-or-v1-abc123");
        CHECK(config::read_api_key_file() == "sk-or-v1-abc123");
    }

    SUBCASE("首尾空白要剃掉") {
        // 用编辑器存出来的多半带一个结尾换行。带着它发出去的
        // Authorization 头会被网关判成非法，报 401——而那会把人支去查一个
        // 其实填对了的密钥。
        config::write_api_key_file("  sk-or-v1-abc123\n\n");
        CHECK(config::read_api_key_file() == "sk-or-v1-abc123");
    }

    SUBCASE("清空就是删掉，不留一个空文件让人猜") {
        config::write_api_key_file("sk-or-v1-abc123");
        config::write_api_key_file("");
        CHECK(config::read_api_key_file().empty());
        CHECK_FALSE(std::filesystem::exists(config::user_api_key_path()));
    }

    SUBCASE("它压过 config.toml 里那份") {
        // 老配置里写了 [llm].api_key 的照样认，但单独那个文件在就以它为准。
        std::error_code ec;
        std::filesystem::create_directories(
            config::user_config_path().parent_path(), ec);
        std::ofstream(config::user_config_path())
            << "[llm]\napi_key = \"老配置里那把\"\n";
        CHECK(config::load_settings(std::nullopt).llm.api_key == "老配置里那把");

        config::write_api_key_file("单独存的那把");
        CHECK(config::load_settings(std::nullopt).llm.api_key == "单独存的那把");
    }
}

TEST_CASE("按任务分流：哪一步用哪个模型") {
    // 用户 2026-09-13 定的方向："发挥各自的优势，不同功能使用不同的模型"。
    // 这条流水线要两种本事：写正文要文采，拆分镜要听话（分镜那份 schema
    // 有六十多个类型定义和一串枚举，文采在那儿一点用都没有）。
    config::LLMConfig c;

    // **默认不分流。** 2026-09-14 换到智谱之后，默认那一档
    // （glm-4.7-flash）是这家**唯一免费**的模型，分流无从分起——与其
    // 写九行一模一样的模型名，不如空着全落到兜底那个。
    // 想分流照 config.toml 模板里注释掉的那段抄（写作 glm-5.3 / 结构
    // glm-5.3-flash）。
    CHECK(c.task_models.empty());
    CHECK(c.model_for("chapter") == c.model);
    CHECK(c.model_for("storyboard") == c.model);

    // 机制本身照旧：配了就按配的走，写作那几步和结构那几步分得开。
    c.task_models["chapter"] = "glm-5.3";
    c.task_models["premises"] = "glm-5.3";
    c.task_models["storyboard"] = "glm-5.3-flash";
    CHECK(c.model_for("chapter") != c.model_for("storyboard"));
    CHECK(c.model_for("chapter") == c.model_for("premises"));

    SUBCASE("没点名的任务落到兜底那个") {
        CHECK(c.model_for("bible") == c.model);
        CHECK(c.model_for("这个任务还没有") == c.model);
        CHECK(c.model_for("") == c.model);
    }

    SUBCASE("配置里填空串也算没配") {
        // 不这么判的话，用户想"这一步用回默认"就只能把整个键删掉，
        // 而设置页上清空一个输入框比删一行配置自然得多。
        c.task_models["chapter"] = "";
        CHECK(c.model_for("chapter") == c.model);
    }

    SUBCASE("真发出去的 model 是分流之后那个") {
        // **不是 cfg.model。** 分流要是没接上，九个任务会全用兜底那个，
        // 而那件事不报错——只是正文忽然变难看。
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        config::LLMConfig cfg = test_cfg_cloud();
        cfg.task_models["chapter"] = "glm-5.3";
        llm::Request r = simple_req();
        r.schema_name = "chapter";
        llm::RemoteClient rc(cfg, http.fn());
        pipeline::CancelToken tok;
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("model") == "glm-5.3");
        CHECK(http.calls[0].body.at("model") != cfg.model);
    }
}

TEST_CASE("退回 json_object 时 schema 要写进提示词") {
    // **不写的话这一下模型连字段名都不知道。** response_format 只剩
    // {"type":"json_object"}，而我们的提示词里从来没有字段名——分镜那份
    // 光 schema 就几十个字段加一串枚举。原来这条退路的注释写着"全靠提示词
    // 里那句『只输出 JSON』"，那句话保不住任何东西。
    llm::Request r = simple_req();
    r.schema = nlohmann::ordered_json::parse(
        R"({"type":"object","properties":{"hook":{"type":"string","minLength":6}}})");

    const std::string out = llm::schema_as_prompt(r.prompt, r.schema);
    CHECK(out.find("写一章剧本") == 0);      // 原提示词在最前面，不能被挤走
    CHECK(out.find("\"hook\"") != std::string::npos);
    // 抄的是**原始** schema：这里它是给模型读的文字，minLength 读得懂就有用
    CHECK(out.find("minLength") != std::string::npos);
    // **换行留着、缩进的空格不留。** 这份 schema 是整条提示词里最大的一
    // 块（分镜那份真正发出去的 7438 字符，两千上下是缩进），而缩进一个字
    // 都不告诉人任何事。压成一行才是真的看不出嵌套——连括号配对都要一个
    // 一个数——所以换行必须在。2026-09-17 从两格一路降到零格，
    // 每一步都只动排版，一个约束都没动。
    CHECK(out.find("\n") != std::string::npos);
    CHECK(out.find("{\n\"type\"") != std::string::npos);
    CHECK(out.find("{\n \"type\"") == std::string::npos);    // 不是一格
    CHECK(out.find("{\n  \"type\"") == std::string::npos);   // 更不是两格

    SUBCASE("没有 schema 就原样返回，不要平白多一段废话") {
        CHECK(llm::schema_as_prompt("就这一句", nlohmann::ordered_json()) ==
              "就这一句");
    }
}

TEST_CASE("从返回里抽内容") {
    const json body = {
        {"choices", json::array({json{{"message", {{"content", "结果"}}}}})}};
    CHECK(llm::extract_content(body.dump()) == "结果");

    SUBCASE("形状不对要报错，不能返回空串") {
        // 返回空串的话，下一步的 JSON 解析会说"找不到合法 JSON"，
        // 把问题指到错误的地方——真正的问题是这个服务的返回不是 OpenAI 格式。
        CHECK_THROWS_AS(llm::extract_content("{}"), llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content(R"({"choices": []})"), llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content(R"({"choices": [{}]})"),
                        llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content(R"({"choices": [{"message": {}}]})"),
                        llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content("不是 JSON"), llm::LlmError);
    }
}

TEST_CASE("状态码翻成人话") {
    const auto cfg = test_cfg();

    SUBCASE("404 分两种") {
        // Ollama 地址不对和模型没拉下来都回 404。一律说「地址填错了」
        // 会把人支到错误的地方去查。
        const std::string no_model = llm::explain_status(
            cfg, 404, R"({"error": "model 'qwen3:14b' not found"})");
        CHECK(no_model.find("ollama pull qwen3:14b") != std::string::npos);
        CHECK(no_model.find("地址填错") == std::string::npos);

        const std::string no_route =
            llm::explain_status(cfg, 404, R"({"error": "404 page not found"})");
        CHECK(no_route.find("地址填错") != std::string::npos);
        CHECK(no_route.find("/v1") != std::string::npos);
        CHECK(no_route.find("ollama pull") == std::string::npos);
    }

    SUBCASE("认证失败要指出去哪儿填密钥") {
        // **指的是项目页那个模型弹窗，不是设置页。** 大模型的地址、模型名、
        // 密钥、温度 2026-09-14 全搬进了项目页「模型」那一行点开的窗；设置页
        // 那一节整个删了（SettingsView 里那段注释：「密钥根本不经过这一页」）。
        // 这条用例原来钉着「设置页」，把人支去一个没有那个框的页面。
        for (const int code : {401, 403}) {
            const std::string s = llm::explain_status(cfg, code, "{}");
            CHECK(s.find("API Key") != std::string::npos);
            CHECK(s.find("项目页") != std::string::npos);
            CHECK(s.find(std::to_string(code)) != std::string::npos);
        }
    }

    SUBCASE("401 的两种情况说两句不同的话") {
        config::LLMConfig c;   // 默认：智谱 + 空密钥

        // 一、默认状态：密钥还没填。说"密钥不对"会把人支去检查一个他
        //     根本没填过的东西，而且该提哪儿领要说清楚。
        REQUIRE(c.api_key.empty());
        const std::string fresh = llm::explain_status(c, 401, "{}");
        CHECK(fresh.find("还没填") != std::string::npos);
        CHECK(fresh.find("bigmodel.cn") != std::string::npos);
        CHECK(fresh.find("不对") == std::string::npos);

        // 二、换了一家云服务、密钥还没填。说"密钥不对"会把人支去检查
        //     一个他根本没填过的东西。
        c.base_url = "https://api.deepseek.com/v1";
        c.model = "deepseek-chat";
        const std::string empty = llm::explain_status(c, 401, "{}");
        CHECK(empty.find("还没填") != std::string::npos);
        CHECK(empty.find("不对") == std::string::npos);

        // 三、真填了一个错的。
        c.api_key = "填了但是错的";
        CHECK(llm::explain_status(c, 401, "{}").find("不对") !=
              std::string::npos);
    }

    SUBCASE("403 要分清「密钥不对」和「这个模型不让你用」") {
        // **2026-09-13 实测撞上的。** OpenRouter 上
        // thinkingmachines/inkling:free 回 403 说 "only available on
        // agentic harnesses"——密钥完全正常（同一把密钥列得出 445 个模型），
        // 是这个模型的免费档限定入口。照老话术报的话，用户会去反复换一把
        // 其实没问题的密钥，而真正该做的是换一个模型。
        config::LLMConfig c = test_cfg();
        c.api_key = "sk-or-v1-好好的";
        const std::string gated = llm::explain_status(
            c, 403,
            R"({"error":{"code":403,"message":"thinkingmachines/inkling:free is only available on agentic harnesses. Try plugging it into a coding agent or productivity app listed on https://openrouter.ai/apps"}})");
        CHECK(gated.find("换一个模型") != std::string::npos);
        CHECK(gated.find("密钥本身多半没问题") != std::string::npos);
        CHECK(gated.find("API Key 不对") == std::string::npos);
        // 服务的原话要原样带上——那才是唯一说清了原因的东西
        CHECK(gated.find("agentic harnesses") != std::string::npos);

        SUBCASE("认不出的 403 还是回到「八成是密钥不对」") {
            // 宁可漏判：漏判只是退回原来那句，而服务原话照样跟在后面；
            // 误判是在密钥真过期时把人支去换模型。
            const std::string plain = llm::explain_status(
                c, 403, R"({"error":"forbidden"})");
            CHECK(plain.find("API Key 不对") != std::string::npos);
        }
    }

    SUBCASE("429 要分清「太频繁」和「没钱」") {
        CHECK(llm::explain_status(cfg, 429, "{}").find("太频繁") !=
              std::string::npos);

        // 智谱把"余额不足/没有可用资源包"也回 429（code 1113）。报成
        // "等一会儿再试"的话，用户会一直等一件永远不会自己好起来的事。
        const std::string broke = llm::explain_status(
            cfg, 429,
            R"({"error":{"code":"1113","message":"余额不足或无可用资源包，请充值。"}})");
        CHECK(broke.find("余额") != std::string::npos);
        CHECK(broke.find("太频繁") == std::string::npos);
        // 指一条出路：这家免费的那个叫什么，直接说出来
        CHECK(broke.find("glm-4.7-flash") != std::string::npos);
    }

    SUBCASE("服务端错误") {
        CHECK(llm::explain_status(cfg, 503, "{}").find("服务自己出错") !=
              std::string::npos);
    }

    SUBCASE("每条都带上当前模型名") {
        // 用户配了好几个模型来回切，不说是哪个的话没法判断
        for (const int code : {400, 401, 404, 429, 500}) {
            CAPTURE(code);
            CHECK(llm::explain_status(cfg, code, "{}").find("当前模型：qwen3:14b") !=
                  std::string::npos);
        }
    }

    SUBCASE("服务说的原话要带上") {
        const std::string s =
            llm::explain_status(cfg, 400, R"({"error": "context too long"})");
        CHECK(s.find("服务说：context too long") != std::string::npos);
    }

    SUBCASE("响应不是 JSON 也要能翻") {
        const std::string s =
            llm::explain_status(cfg, 502, "<html>Bad Gateway</html>");
        CHECK(s.find("服务自己出错") != std::string::npos);
        CHECK(s.find("Bad Gateway") != std::string::npos);
    }

    SUBCASE("超长的服务返回要截断") {
        // 有些网关会回一整页 HTML。原样贴出来会把错误框撑爆。
        const std::string s = llm::explain_status(cfg, 500, std::string(5000, 'x'));
        CHECK(s.size() < 500);
    }

    SUBCASE("中文的截断不会切出半个字") {
        std::string cn = R"({"error": ")";
        for (int i = 0; i < 500; ++i) cn += "错";
        cn += R"("})";
        const std::string s = llm::explain_status(cfg, 500, cn);
        // 200 个「错」是 600 字节
        CHECK(s.find(std::string("错")) != std::string::npos);
        // 不能出现非法 UTF-8——能塞进 json 就说明是合法的
        CHECK_NOTHROW(json(s).dump());
    }
}

TEST_CASE("远端客户端跑通一次") {
    FakeHttp http;
    http.responses.push_back(ok("{\"title\": \"雨夜天台\"}"));
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    const std::string out = c.complete(simple_req(), tok);
    CHECK(out == "{\"title\": \"雨夜天台\"}");

    REQUIRE(http.calls.size() == 1);
    CHECK(http.calls[0].url == "http://127.0.0.1:11434/v1/chat/completions");
    CHECK(http.calls[0].headers.at("Authorization") == "Bearer ollama");
    CHECK(http.calls[0].timeout_s == 300.0);
    // 只发一次，而且一个 response_format 都不带——schema 在提示词里
    CHECK_FALSE(http.calls[0].body.contains("response_format"));
    CHECK(http.calls[0].body.at("messages")[0].at("content").get<std::string>().find(
              "\"type\": \"object\"") != std::string::npos);
}

TEST_CASE("连不上的时候不重试") {
    // 传输层失败重试一遍没有意义，只是让用户多等一个超时。
    FakeHttp http;
    llm::HttpResponse down;
    down.status = 0;
    down.transport_error = "Connection refused";
    http.responses.push_back(down);
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    try {
        c.complete(simple_req(), tok);
        FAIL("该抛异常");
    } catch (const llm::LlmError& e) {
        const std::string msg = e.what();
        CHECK(msg.find("连不上大模型服务") != std::string::npos);
        CHECK(msg.find("http://127.0.0.1:11434/v1") != std::string::npos);
        CHECK(msg.find("Connection refused") != std::string::npos);
        // **这句话人是在对话里读到的**（引擎把它原样交给场记，场记原样说
        // 出来）。所以它不能只是一句"连不上"加一截英文——得说下一步去看
        // 哪儿。2026-09-21 之前就是那样：中文界面上挂着
        // `Could not establish connection`，一个字的去处都没有。
        CHECK(msg.find("地址对不对") != std::string::npos);
        CHECK(msg.find("设置") != std::string::npos);
        // 而且要说清**不是**哪一种——两种说法指向两个完全不同的地方。
        CHECK(msg.find("不是超时") != std::string::npos);
    }
    CHECK(http.calls.size() == 1);
}

TEST_CASE("取消") {
    SUBCASE("请求前就取消，一次都不发") {
        FakeHttp http;
        http.responses.push_back(ok("不该被用到"));
        llm::RemoteClient c(test_cfg(), http.fn());
        pipeline::CancelToken tok;
        tok.request();
        CHECK_THROWS_AS(c.complete(simple_req(), tok), llm::LlmError);
        CHECK(http.calls.empty());
    }

    SUBCASE("退回重试之前会再查一次") {
        // 同步的 HTTP 调用中途打不断，能查的只有请求之间。
        // 不查的话，用户点了停止还要再等一个完整的超时。
        FakeHttp http;
        http.responses.push_back(llm::HttpResponse{400, "{}", std::nullopt});
        http.responses.push_back(ok("不该被用到"));
        llm::RemoteClient c(test_cfg(), http.fn());

        pipeline::CancelToken tok;
        // 第一次请求发出去之后取消
        llm::HttpPost inner = http.fn();
        llm::HttpPost wrapped =
            [&](const std::string& u, const std::string& b,
                const std::map<std::string, std::string>& h, double t) {
                auto r = inner(u, b, h, t);
                tok.request();
                return r;
            };
        llm::RemoteClient c2(test_cfg(), wrapped);
        CHECK_THROWS_AS(c2.complete(simple_req(), tok), llm::LlmError);
        CHECK(http.calls.size() == 1);   // 只发了第一次
    }
}

TEST_CASE("回放客户端") {
    llm::ReplayClient c({"第一次", "第二次"});
    pipeline::CancelToken tok;

    CHECK(c.complete(simple_req(), tok) == "第一次");
    CHECK(c.complete(simple_req(), tok) == "第二次");
    // 录到头了要响亮地失败，不能悄悄重放最后一条——
    // 那样测试会"通过"但测的是错的东西
    CHECK_THROWS_AS(c.complete(simple_req(), tok), llm::LlmError);

    CHECK(c.calls().size() == 3);
    CHECK(c.calls()[0].prompt == "写一章剧本");
}

TEST_CASE("默认配置下 make_client 给的就是远端，而且跑得通") {
    // 三条后端：远端、命令行（2026-09-18）、进程内（2026-09-14 删、09-19
    // 接回来）。这条钉的是"默认配置下走远端、而且跑得通"。换后端立刻生效
    // 那件事另有用例，在 test_llm_command.cpp 里；进程内那条在测试目标里
    // 没编进 llama.cpp，退回远端那一支见下一条。
    auto dummy_post = [](const std::string&, const std::string&,
                         const std::map<std::string, std::string>&, double) {
        llm::HttpResponse r;
        r.status = 200;
        r.body = R"({"choices":[{"message":{"content":"好"}}]})";
        return r;
    };
    const auto c = llm::make_client(dummy_post);
    REQUIRE(c != nullptr);
    pipeline::CancelToken tok;
    // **这一趟不带 schema。** 这条用例要钉的是"make_client 只给得出远端
    // 这一条，而且它跑得通"，不是校验。原来用的是 simple_req()——它带着
    // `{"type":"object"}`，而假响应是裸文本「好」，于是 4c90bd0 给远端那条
    // 加上结构化校验之后这条就一直红着。**红的是夹具不是实现**：一个带
    // schema 的请求回一段裸文本，本来就该被拒。
    llm::Request plain;
    plain.prompt = "写一章剧本";
    plain.temperature = 0.7;
    CHECK(c->complete(plain, tok) == "好");
}

TEST_CASE("配了 local 而这个二进制没编 llama.cpp：退回远端，别把流水线堵死") {
    // 测试目标不链 llama.cpp（CMakeLists 里 CHANGJI_HAVE_LLAMA 只给 changji
    // 那个目标定义），所以 make_local_client 回 nullptr。SwitchingClient
    // 那头要退回远端——用户多半只是拿了个不带 llama 的构建，而远端那条只要
    // 地址填了就能用。当场抛的话每一次生成都失败，而报错指向配置。
    int posted = 0;
    auto post = [&posted](const std::string&, const std::string&,
                          const std::map<std::string, std::string>&, double) {
        ++posted;
        llm::HttpResponse r;
        r.status = 200;
        r.body = R"({"choices":[{"message":{"content":"远端答的"}}]})";
        return r;
    };
    config::LLMConfig cfg;
    cfg.backend = "local";
    cfg.base_url = "http://127.0.0.1:11434/v1";
    const auto c = llm::make_client(llm::ConfigProvider([cfg] { return cfg; }), post);
    REQUIRE(c != nullptr);
    pipeline::CancelToken tok;
    llm::Request plain;
    plain.prompt = "写一章";
    plain.temperature = 0.7;
    CHECK(c->complete(plain, tok) == "远端答的");
    CHECK(posted == 1);
    // 进程内那条在这个目标里本来就造不出来
    CHECK(llm::make_local_client([cfg] { return cfg; }) == nullptr);
}

// ---- 起服务时不预热 ----
//
// 这里原来有一组"预热：这几种情况一个线程都不该起"。**整个预热都去掉了**
// ——用户 2026-09-11 定的："用的时候才加载是对的，不做启动预载"。
//
// 留这段话是因为它反过来说明了现在的规矩：`register_llm_slot` 只登记，
// **装是借出时才发生的**。起服务之后显存上应该看不到大模型；看到了就是
// 哪儿又偷偷预装了。

// ── 远端 SSE ────────────────────────────────────────────────────────
//
// 远端那条原来是整段到的（Client::complete 的默认实现：跑一遍同步的，
// 整段回调一次）。云端 API 上写一章、写大纲于是没有"边写边看"，界面干等
// 一两分钟最后一下子蹦出来。用户 2026-09-12：「远端那条也上 SSE」。
//
// 下面这几条盯的是**退路**：接了 SSE 不能让任何一种服务变得更糟。

namespace {

/// 假的流式发送：把预先摆好的几段字节喂给 on_chunk。
struct FakeStream {
    struct Call {
        json body;
        std::map<std::string, std::string> headers;
    };
    std::vector<Call> calls;
    /// 每次调用喂哪些段。calls 用完了就回 500。
    std::vector<std::vector<std::string>> chunks;
    std::vector<int> statuses;      ///< 对应每次调用的状态码，默认 200
    std::string error_body;         ///< 状态码 >= 400 时回的体
    /// 对应每次调用：喂完那几段之后断掉，回这句传输层错误。空串 = 不断。
    std::vector<std::string> broke;

    llm::HttpPostStream fn() {
        return [this](const std::string&, const std::string& body,
                      const std::map<std::string, std::string>& headers, double,
                      const llm::OnChunk& on_chunk) -> llm::HttpResponse {
            calls.push_back(Call{json::parse(body, nullptr, false), headers});
            const std::size_t i = calls.size() - 1;
            const int status = i < statuses.size() ? statuses[i] : 200;
            llm::HttpResponse r;
            r.status = status;
            if (status >= 400) {
                r.body = error_body;
                return r;
            }
            if (i < chunks.size()) {
                for (const auto& c : chunks[i]) {
                    if (!on_chunk(c.data(), c.size())) break;
                }
            }
            if (i < broke.size() && !broke[i].empty()) {
                r.status = 0;
                r.transport_error = broke[i];
            }
            return r;
        };
    }
};

std::string sse_chunk(const std::string& content) {
    return "data: {\"choices\":[{\"delta\":{\"content\":\"" + content +
           "\"}}]}\n\n";
}

/// 一条 SSE 事件。**data 那段用 json 拼再 dump**，不手写转义：工具调用那几
/// 条里 arguments 本身是一串 JSON 文本，手写的话三层引号一定会错，而错了的
/// 表现是"这条解不出来被静默跳过"——用例照样绿。
std::string sse_raw(const json& data) { return "data: " + data.dump() + "\n\n"; }

/// 一条 delta 事件。
std::string sse_delta(const json& delta, const std::string& finish = "") {
    json choice = {{"delta", delta}};
    if (!finish.empty()) choice["finish_reason"] = finish;
    return sse_raw(json{{"choices", json::array({choice})}});
}

}  // namespace

TEST_CASE("远端 SSE：边收边回调，拼出来的是全文") {
    FakeHttp http;
    FakeStream stream;
    // 内容要长得像 JSON：simple_req() 是带 schema 的，而"回了 200 却不是
    // JSON 就退一步"那条规矩会在散文上触发（见下面那个用例）。
    stream.chunks = {{sse_chunk("{\\\"title\\\": \\\"第一"),
                      sse_chunk("章\\\"}"), "data: [DONE]\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    const std::string out = c.complete(simple_req(), tok, [&](const std::string& p) {
        pieces.push_back(p);
    });

    CHECK(out == "{\"title\": \"第一章\"}");
    // **给的是增量不是累计**：一段几千字的正文，每次带全文的话光字符串
    // 拷贝就比生成还贵。
    CHECK(pieces == std::vector<std::string>{"{\"title\": \"第一", "章\"}"});
    // 整段那条一次都不该走
    CHECK(http.calls.empty());
    // 请求里要带 stream: true，不然服务端回的是整段
    REQUIRE(stream.calls.size() == 1);
    CHECK(stream.calls[0].body.at("stream") == true);
    CHECK(stream.calls[0].headers.at("Accept") == "text/event-stream");
}

TEST_CASE("远端 SSE：不给 stream_post 就还是整段那条") {
    // TTS 那边和一堆测试拿 RemoteClient 当普通客户端用，不该被迫注入
    // 第二个函数。没有它时行为和以前**一个字都不差**。
    FakeHttp http;
    http.responses = {ok("{\"ch\":\"整段回来的\"}")};
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    const std::string out =
        c.complete(simple_req(), tok, [&](const std::string& p) { pieces.push_back(p); });
    CHECK(out == "{\"ch\":\"整段回来的\"}");
    // 默认实现会把整段回调一次——**不是不回调**，否则流式那条路上
    // 什么都收不到，而且不报错，只是编辑器里一直空着。
    CHECK(pieces == std::vector<std::string>{"{\"ch\":\"整段回来的\"}"});
}

TEST_CASE("远端 SSE：服务端压根不认 stream，回了一份普通 JSON") {
    // 这条是"不会变得更糟"的核心：老服务照样能用，只是那一下是整段到的。
    FakeHttp http;
    FakeStream stream;
    // 200，但没有一条 SSE，body 里是整份普通响应
    stream.chunks = {{}, {}};
    llm::HttpPostStream raw = stream.fn();
    llm::HttpPostStream wrapped =
        [&raw](const std::string& url, const std::string& body,
               const std::map<std::string, std::string>& headers, double t,
               const llm::OnChunk& on_chunk) {
            llm::HttpResponse r = raw(url, body, headers, t, on_chunk);
            r.body = ok("{\"ch\":\"整段那份\"}").body;   // 服务端把整份 JSON 一次性回了
            return r;
        };

    llm::RemoteClient c(test_cfg(), http.fn(), wrapped);
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    const std::string out =
        c.complete(simple_req(), tok, [&](const std::string& p) { pieces.push_back(p); });

    CHECK(out == "{\"ch\":\"整段那份\"}");
    CHECK(pieces == std::vector<std::string>{"{\"ch\":\"整段那份\"}"});
}

TEST_CASE("远端 SSE：流里报错要抛，不能当成写完了") {
    FakeHttp http;
    FakeStream stream;
    stream.chunks = {
        {"data: {\"error\":{\"message\":\"model not found\"}}\n\n"},
        {"data: {\"error\":{\"message\":\"model not found\"}}\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    // 先回 200 再在流里说"这个模型没有"是常见做法。当成生成完了的话，
    // 用户拿到的是一段空正文外加一句"写好了"。
    CHECK_THROWS_AS(c.complete(simple_req(), tok, [](const std::string&) {}),
                    llm::LlmError);
}

TEST_CASE("远端 SSE：中途取消要断掉，别让它继续生成") {
    FakeHttp http;
    FakeStream stream;
    stream.chunks = {{sse_chunk("头一段"), sse_chunk("不该有的"), "data: [DONE]\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    CHECK_THROWS_AS(c.complete(simple_req(), tok,
                               [&](const std::string& p) {
                                   pieces.push_back(p);
                                   tok.request();   // 收到第一段就叫停
                               }),
                    llm::LlmError);
    // 叫停之后不该再有第二段——on_chunk 返回 false，传输层会断开
    CHECK(pieces.size() == 1);
}

TEST_CASE("断在半路、正文还没来：原样再发一趟，只一趟") {
    // 2026-09-25 实测：智谱 glm-5.3 连着五次调用断了四次，全是
    // `Failed to read connection`、全断在思考那一段（正文 0 字）、离超时
    // 远得很。不重发的话对话那一轮整个作废，而人接下来做的就是原话再发一遍。
    const std::string kBroke = "Failed to read connection";
    FakeHttp http;
    pipeline::CancelToken tok;

    SUBCASE("整段生成（流式）：第一趟只想了一半就断，第二趟写完") {
        FakeStream stream;
        stream.chunks = {{sse_delta(json{{"reasoning_content", "先想想"}})},
                         {sse_delta(json{{"reasoning_content", "再想想"}}),
                          sse_chunk("{\\\"title\\\": \\\"雨\\\"}"), "data: [DONE]\n\n"}};
        stream.broke = {kBroke};
        llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
        llm::Request req = simple_req();
        std::string thought;
        req.on_thinking = [&](const std::string& p) { thought += p; };
        const std::string out = c.complete(req, tok, [](const std::string&) {});
        CHECK(out == "{\"title\": \"雨\"}");
        CHECK(stream.calls.size() == 2);
        // 发的是**同一份**请求
        CHECK(stream.calls[0].body == stream.calls[1].body);
        // 思考那一栏要说一声，不然它从半截上重新想一遍，看着像犯糊涂
        CHECK(thought.find("先想想") < thought.find("重发"));
        CHECK(thought.find("重发") < thought.find("再想想"));
    }
    SUBCASE("正文已经来过半句就不重发：界面上会冒两遍") {
        FakeStream stream;
        stream.chunks = {{sse_chunk("{\\\"title\\\": \\\"雨")}};
        stream.broke = {kBroke};
        llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
        CHECK_THROWS_AS(c.complete(simple_req(), tok, [](const std::string&) {}),
                        llm::LlmError);
        CHECK(stream.calls.size() == 1);
    }
    SUBCASE("两趟都断：只重发一次，报错里说已经重发过、不叫人调超时") {
        FakeStream stream;
        stream.chunks = {{}, {}, {}};
        stream.broke = {kBroke, kBroke, kBroke};
        llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
        try {
            c.complete(simple_req(), tok, [](const std::string&) {});
            FAIL("该抛异常");
        } catch (const llm::LlmError& e) {
            const std::string msg = e.what();
            CHECK(msg.find("重发过一次") != std::string::npos);
            CHECK(msg.find("timeout_s") == std::string::npos);
            CHECK(msg.find(kBroke) != std::string::npos);
        }
        CHECK(stream.calls.size() == 2);
    }
    SUBCASE("等满了超时才断的不重发：再等一个超时也一样") {
        FakeStream stream;
        stream.chunks = {{}, {}};
        stream.broke = {kBroke, ""};
        auto cfg = test_cfg();
        cfg.timeout_s = 0.0;     // 花了多久都不算"离超时还远"
        llm::RemoteClient c(cfg, http.fn(), stream.fn());
        CHECK_THROWS_AS(c.complete(simple_req(), tok, [](const std::string&) {}),
                        llm::LlmError);
        CHECK(stream.calls.size() == 1);
    }
    SUBCASE("带工具的对话（流式）：第一趟断在思考里，第二趟调上工具") {
        FakeStream stream;
        stream.chunks = {{sse_delta(json{{"reasoning_content", "看看项目"}})},
                         {sse_delta(json{{"tool_calls", json::array({json{
                              {"index", 0}, {"id", "c1"},
                              {"function", json{{"name", "project_state"},
                                                {"arguments", "{}"}}}}})}}),
                          sse_delta(json::object(), "tool_calls"), "data: [DONE]\n\n"}};
        stream.broke = {kBroke};
        llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
        llm::Request opts;
        std::string thought;
        opts.on_thinking = [&](const std::string& p) { thought += p; };
        const llm::ChatReply r = c.chat({}, json::array(), opts, tok);
        CHECK(stream.calls.size() == 2);
        REQUIRE(r.tool_calls.size() == 1);
        CHECK(r.tool_calls[0].name == "project_state");
        CHECK(thought.find("重发") != std::string::npos);
    }
    SUBCASE("正文来过一截、调用方收得回（on_restart）：先收回再重发") {
        FakeStream stream;
        stream.chunks = {{sse_chunk("改好了：罗")},
                         {sse_chunk("改好了：罗春梅的穿着。"), sse_delta(json::object(), "stop"),
                          "data: [DONE]\n\n"}};
        stream.broke = {kBroke};
        llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
        llm::Request opts;
        std::string shown;
        int restarts = 0;
        opts.on_token = [&](const std::string& p) { shown += p; };
        opts.on_restart = [&] {
            ++restarts;
            shown.clear();
        };
        const llm::ChatReply r = c.chat({}, json::array(), opts, tok);
        CHECK(stream.calls.size() == 2);
        CHECK(restarts == 1);
        CHECK(r.content == "改好了：罗春梅的穿着。");
        CHECK(shown == "改好了：罗春梅的穿着。");   // 没有冒两遍
    }
    SUBCASE("正文来过一截、调用方没给 on_restart：照旧不重发") {
        FakeStream stream;
        stream.chunks = {{sse_chunk("改好了：罗")}};
        stream.broke = {kBroke};
        llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
        llm::Request opts;
        opts.on_token = [](const std::string&) {};
        CHECK_THROWS_AS(c.chat({}, json::array(), opts, tok), llm::LlmError);
        CHECK(stream.calls.size() == 1);
    }
    SUBCASE("整段那条同样重发一趟") {
        llm::HttpResponse broke;
        broke.status = 0;
        broke.transport_error = kBroke;
        http.responses = {broke, ok("{\"title\": \"雨\"}")};
        llm::RemoteClient c(test_cfg(), http.fn());
        CHECK(c.complete(simple_req(), tok) == "{\"title\": \"雨\"}");
        CHECK(http.calls.size() == 2);
    }
}

/**
 * 这一层是用来接住"整份输出坏了"的——被 length 截断、内容过滤掐掉、
 * 压根不是 JSON。**一个字段没填好不是那一类**，而在这儿 return 一个错的
 * 代价是整份作废：一章十七镜、十分钟的戏，全没。
 *
 * 两条都是实跑撞出来的，所以一起钉在这儿。
 */
TEST_CASE("校验层：一个字段没填好，不该把整份输出作废") {
    using changji::llm::validate_json_schema;
    using nlohmann::json;
    using ordered = nlohmann::ordered_json;

    SUBCASE("枚举填了表外的值：放行") {
        // 2026-09-16 实测：一章十七镜里第三镜的 face_pose 填了个表外的值，
        // 整个 /api/plan 回 400，十七镜全没了——而下游 drop_unknown_enums
        // 本来就会把它抹成默认值，一镜都不该丢。
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{
                 "face_pose":{"type":"string","enum":["front","back"]}}})");
        CHECK_FALSE(validate_json_schema(json{{"face_pose", "三分之二侧"}}, schema)
                        .has_value());
    }

    SUBCASE("字符串写短了：放行；写空了：拦") {
        // 2026-09-16 实测：写了十分钟、三场戏都在，就因为第三场的 worse
        // 短了几个字，一个字都没留下：
        //   大模型输出不符合 chapter Schema：$.scenes[2].worse 太短
        // 这些 minLength 本来就是"推一把"的数（last_line 的下限还从 12 降到
        // 过 4），而走远端 API 的模型根本不按 GBNF 生成，它对它们只是建议。
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{
                 "worse":{"type":"string","minLength":8}}})");
        CHECK_FALSE(validate_json_schema(json{{"worse", "少了退路"}}, schema)
                        .has_value());
        // 空串是另一回事：那不是"写短了"，是这一栏压根没写
        const auto empty = validate_json_schema(json{{"worse", ""}}, schema);
        REQUIRE(empty.has_value());
        CHECK(empty->find("$.worse") != std::string::npos);
    }

    SUBCASE("多写了一个键：放行") {
        // 下游是按名字取字段的，多出来那个根本碰不到。而带推理的模型会把
        // 思考编成键名塞进来——在这儿判废就是拿一次十分钟的正文去换一个
        // 没人读的键。项目自己在 /api/run 上早就立过同一条规矩。
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{"a":{"type":"string"}},
                "additionalProperties":false})");
        CHECK_FALSE(
            validate_json_schema(json{{"a", "好"}, {"模型自己加的备注", "x"}},
                                 schema)
                .has_value());
    }

    SUBCASE("数组里的一项缺字段：放行；顶层缺：拦") {
        // 2026-09-17 实撞：
        //     $.scenes.s1.beats[12] 缺少必填字段 characters
        // 二十六拍里一拍没填「画面里有谁」，一次五分钟的改编整份作废——
        // 而下游本来就受得住（script.cpp 那句 `it != item.end()`，缺了就是
        // 空数组，正是 schema 自己写的「环境拍、空镜填空数组」）。
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{
                 "beats":{"type":"array","items":{
                   "type":"object",
                   "properties":{"text":{"type":"string"},
                                 "characters":{"type":"array"}},
                   "required":["text","characters"]}}},
                "required":["beats"]})");
        // 第二拍缺 characters：整份还是能用
        const json one_bad = {{"beats", json::array({
            json{{"text", "他推门进来"}, {"characters", json::array({"曾老板"})}},
            json{{"text", "窗外下着雨"}},
        })}};
        CHECK_FALSE(validate_json_schema(one_bad, schema).has_value());
        // 顶层缺 beats：那是整份没有内容，照拦
        CHECK(validate_json_schema(json::object(), schema).has_value());
    }

    SUBCASE("真坏了的还得拦住") {
        // 这一层的本职：类型不对、缺必填、整份走错分支
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{"n":{"type":"integer"}},
                "required":["n"]})");
        CHECK(validate_json_schema(json{{"n", "不是数"}}, schema).has_value());
        CHECK(validate_json_schema(json::object(), schema).has_value());
        // 数组的下限是地板，不是建议——少写了几场，下游没法凭空补出来
        const auto arr = ordered::parse(
            R"({"type":"object","properties":{
                 "scenes":{"type":"array","minItems":3}}})");
        CHECK(validate_json_schema(json{{"scenes", json::array({1, 2})}}, arr)
                  .has_value());
    }

    SUBCASE("数组写多了：放行。多和少不对称") {
        // 2026-09-16 / 17 两次实撞：
        //     大模型输出不符合 script Schema：$.scenes.s2.beats 的项目太多
        // 一次五分钟的改编，就因为第二场多写了一拍，整份作废。而多出来的
        // 那一拍下游本来就吃得下——拍子会变成镜头，多一个少一个不是结构问题。
        const auto arr = ordered::parse(
            R"({"type":"object","properties":{
                 "beats":{"type":"array","minItems":2,"maxItems":3}}})");
        CHECK_FALSE(
            validate_json_schema(json{{"beats", json::array({1, 2, 3, 4})}}, arr)
                .has_value());
        // 少写了还是拦
        CHECK(validate_json_schema(json{{"beats", json::array({1})}}, arr)
                  .has_value());
    }
}

TEST_CASE("没人引用的 $defs 不贴给模型看") {
    // **这些定义是随模型的 JSON Schema 整份带进来的**，而各阶段只挑用得上
    // 的那几个字段、还把枚举内联在属性上。于是定义本身成了孤儿，却照样
    // 占着提示词。2026-09-17 量的分镜那份：CameraAngle / CameraMove /
    // ShotStatus / Transition / Lens 五个一次都没被引用，合计 635 字符，
    // 其中 CameraMove 那串枚举和属性上内联的那份一模一样——模型读两遍。
    const auto schema = nlohmann::ordered_json::parse(R"({
      "type": "object",
      "properties": {
        "size": {"$ref": "#/$defs/ShotSize"},
        "move": {"type": "string", "enum": ["static", "push_in"]}
      },
      "$defs": {
        "ShotSize": {"enum": ["MS", "CU"]},
        "CameraMove": {"enum": ["static", "push_in"]},
        "ShotStatus": {"enum": ["planned", "final_done"]}
      }
    })");
    const std::string out = llm::schema_as_prompt("写一句", schema);
    CHECK(out.find("ShotSize") != std::string::npos);       // 被引用，留着
    CHECK(out.find("CameraMove") == std::string::npos);     // 没人引用，摘掉
    CHECK(out.find("ShotStatus") == std::string::npos);
    // 内联在属性上的那串枚举不受影响
    CHECK(out.find("push_in") != std::string::npos);

    SUBCASE("传递引用也要留：被留下的定义自己引用的那些") {
        const auto s2 = nlohmann::ordered_json::parse(R"({
          "type": "object",
          "properties": {"a": {"$ref": "#/$defs/A"}},
          "$defs": {
            "A": {"type": "object", "properties": {"b": {"$ref": "#/$defs/B"}}},
            "B": {"enum": ["x"]},
            "C": {"enum": ["never"]}
          }
        })");
        const std::string o2 = llm::schema_as_prompt("写一句", s2);
        CHECK(o2.find("\"B\"") != std::string::npos);
        CHECK(o2.find("never") == std::string::npos);
    }

    SUBCASE("全都用得上就原样不动") {
        const auto s3 = nlohmann::ordered_json::parse(R"({
          "properties": {"a": {"$ref": "#/$defs/A"}},
          "$defs": {"A": {"enum": ["x"]}}
        })");
        CHECK(llm::schema_as_prompt("写一句", s3).find("\"A\"") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// schema 贴过去那一份：摘掉排版和噪声，一个约束都不许丢
// ---------------------------------------------------------------------------

TEST_CASE("schema 贴过去：pydantic 的 title 摘掉，叫 title 的字段留着") {
    // ⚠️ **这两件事只差一层。** `title` 作为**注解**是纯噪声（值就是键名换
    // 个写法）；而 `properties` 底下那一层的键是**字段名**——剧本那份
    // schema 里就真有一个叫 title 的字段（一章的标题）。递归摘的话会把它
    // 整个删掉，模型于是再也不填标题，而且一声不响。
    const nlohmann::ordered_json schema = {
        {"type", "object"},
        {"title", "Episode"},            // 注解：该摘
        {"properties",
         {{"title",                       // 字段名：该留
           {{"type", "string"}, {"title", "Title"}, {"description", "这一章叫什么"}}},
          {"shot_id", {{"type", "string"}, {"title", "Shot Id"}}}}},
        {"required", {"title", "shot_id"}},
    };
    const std::string sent = llm::schema_as_prompt("提示词", schema);

    CHECK(sent.find("\"Episode\"") == std::string::npos);
    CHECK(sent.find("\"Shot Id\"") == std::string::npos);
    CHECK(sent.find("\"Title\"") == std::string::npos);
    // 字段本身、它的约束和描述一条不少
    CHECK(sent.find("\"title\"") != std::string::npos);
    CHECK(sent.find("这一章叫什么") != std::string::npos);
    CHECK(sent.find("\"required\"") != std::string::npos);
}

TEST_CASE("schema 贴过去：换行留着，缩进的空格不留") {
    // 换行要留：压成一行之后连括号配对都要一个一个数。而缩进的空格一个字
    // 都不告诉人任何事，它却是整条提示词里最大的一块里最没用的那一层。
    const nlohmann::ordered_json schema = {
        {"type", "object"},
        {"properties", {{"a", {{"type", "string"}}}}},
    };
    const std::string sent = llm::schema_as_prompt("提示词", schema);
    CHECK(sent.find('\n') != std::string::npos);
    // 行首不许有空格
    std::size_t at = 0;
    while ((at = sent.find('\n', at)) != std::string::npos) {
        ++at;
        const bool flush = at >= sent.size() || sent[at] != ' ';
        CHECK_MESSAGE(flush, "行首还有缩进");
    }
}

TEST_CASE("有人要看思考就走流式，哪怕没人要逐字") {
    // ⚠️ **整段那条只能在收完之后把整段思考给出去。**
    //
    // 「改编成剧本」这一步实测跑 11 分半，而这 11 分半里任务页面上那一行
    // 一个字都没有，跑完那一下才蹦出 19 万字（用户 2026-09-17：「思考的
    // 内容还是看不到」）。思考的用处全在跑的过程里——它是唯一能回答
    // "它还活着吗、在想什么"的东西，跑完再给等于没给。
    //
    // 这条钉的是**那个分流判据**：以前只看 `on_token`，于是所有不需要逐字
    // 的调用（改编、分镜、定妆）全走整段那条。
    bool streamed = false;
    llm::RemoteClient c(
        test_cfg(),
        // 整段那条：走到这儿就说明判错了
        [](const std::string&, const std::string&,
           const std::map<std::string, std::string>&,
           double) { return ok("{}"); },
        // 流式那条
        [&streamed](const std::string&, const std::string&,
                    const std::map<std::string, std::string>&, double,
                    const llm::OnChunk& on_chunk) {
            streamed = true;
            const std::string sse =
                "data: {\"choices\":[{\"delta\":{\"content\":\"{}\"}}]}\n\n"
                "data: [DONE]\n\n";
            on_chunk(sse.data(), sse.size());
            return llm::HttpResponse{200, "", std::nullopt};
        });

    llm::Request r = simple_req();
    r.on_thinking = [](const std::string&) {};
    pipeline::CancelToken tok;
    c.complete(r, tok);
    CHECK(streamed);
}

TEST_CASE("配错了的错要认得出来：批量据此停下") {
    // ⚠️ **2026-09-17 实撞：一次补七章分镜，第三章开始密钥失效，而「一章砸
    // 了不拖垮后面几章」这条让它又试了五章，每章同一个 401**——人白等二十
    // 分钟，回来看到五条一模一样的报错。
    //
    // 那条规矩针对的是**内容**问题（这一章模型没写好，下一章换个剧本也许就
    // 好了）；而密钥不对、地址不对、模型名不存在，不会在下一章自己变好。
    SUBCASE("401 / 403 / 404 是配错了") {
        for (int code : {401, 403, 404}) {
            CAPTURE(code);
            const llm::LlmError e("随便一句", code);
            CHECK(e.status() == code);
            CHECK(e.is_config_error());
        }
    }
    SUBCASE("限流和 5xx 不算——换个时间真会好，接着跑是对的") {
        for (int code : {429, 500, 502, 503}) {
            CAPTURE(code);
            CHECK_FALSE(llm::LlmError("随便一句", code).is_config_error());
        }
    }
    SUBCASE("不是因为状态码抛的，status 是 0") {
        const llm::LlmError e("大模型返回的不是 JSON");
        CHECK(e.status() == 0);
        CHECK_FALSE(e.is_config_error());
    }
}

TEST_CASE("对面回 4xx 时，状态码要挂到异常上") {
    // 只有一句人话的话，批量那头只能去正则匹配「401」三个字——而措辞是会改
    // 的（`explain_status` 那几句一直在调），匹配挂了不会报错，只会悄悄退回
    // "接着试五集"。
    FakeHttp http;
    http.responses.push_back(llm::HttpResponse{401, R"({"code":"401"})", std::nullopt});
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;
    try {
        c.complete(simple_req(), tok);
        FAIL("该抛的没抛");
    } catch (const llm::LlmError& e) {
        CHECK(e.status() == 401);
        CHECK(e.is_config_error());
    }
}

TEST_CASE("接上提示词日志之后，出入口的行为一个字没变") {
    // 这一条钉的**不是日志写对了没有**（那在 test_llm_call_log.cpp 里），
    // 是「四个出入口接上记录器之后对外还是原来那个样子」：同样的输入、
    // 同样的返回、同样的异常。
    //
    // 异常那一半最值得钉：catch 里那句必须是**裸 `throw;`**。写成
    // `throw e;` 会把 LlmError 切片成 std::exception，`status()` 和
    // `is_config_error()` 当场失效——而批量那几条靠 is_config_error 止损
    // （见 client.hpp 上那段「一次补七章分镜，第三章开始密钥失效，
    // 又试了五章」）。切片了不会编译报错，只会让人白等二十分钟。
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cj_llm_log_client_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    // 研究用的那个环境变量（call_log.hpp §CallLogOptions::root）：这儿拿它
    // 把整条落盘路径从用户真正的数据目录上挪开，**一个字节都不往那儿写**。
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR", paths::to_utf8(root));

    SUBCASE("开关关着：返回值照旧，而且一次盘都不碰") {
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = false;
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(simple_req(), tok) == "{}");
        CHECK_MESSAGE(!std::filesystem::exists(root),
                      "关着的时候连目录都不该建");
    }

    SUBCASE("开关开着：整段那条的返回值、发出去的那份都没变样") {
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        FakeHttp http;
        http.responses.push_back(ok(R"({"a":1})"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(simple_req(), tok) == R"({"a":1})");
        REQUIRE(http.calls.size() == 1);
        // 记日志不许往请求体里加东西，也不许多发一趟
        CHECK(http.calls[0].body.at("messages").size() == 1);
        CHECK_FALSE(http.calls[0].body.contains("response_format"));
    }

    SUBCASE("开关开着：401 那句话和 status 逐字不变") {
        const llm::HttpResponse deny{401, R"({"error":"bad key"})", std::nullopt};
        // 先问一遍"不记日志时它说什么"，再拿记日志时那句和它逐字比——
        // 比死措辞的话，explain_status 那几句一调整这条就假红。
        std::string plain;
        {
            config::LLMConfig off = test_cfg();
            off.call_log = false;
            FakeHttp h;
            h.responses.push_back(deny);
            llm::RemoteClient c(off, h.fn());
            pipeline::CancelToken tok;
            try {
                c.complete(simple_req(), tok);
                FAIL("该抛的没抛");
            } catch (const llm::LlmError& e) {
                plain = e.what();
            }
        }
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        FakeHttp http;
        http.responses.push_back(deny);
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        try {
            c.complete(simple_req(), tok);
            FAIL("该抛的没抛");
        } catch (const llm::LlmError& e) {
            CHECK(std::string(e.what()) == plain);
            CHECK(e.status() == 401);
            CHECK(e.is_config_error());   // 切片了这一条会红
        }
    }

    SUBCASE("开关开着：schema 本地校验没过，抛的还是 LlmError") {
        // 这一次对面好好地回了 200，是**我们这头**没认——研究最想看的正是
        // 它，而记录器在这一步之前就该把正文收下了（见 client.cpp 里
        // extract_content 提前那一段）。这条只管"行为没变"。
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        llm::Request r = simple_req();
        r.schema = nlohmann::ordered_json{{"type", "object"}};
        FakeHttp http;
        http.responses.push_back(ok("这压根不是 JSON"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK_THROWS_AS(c.complete(r, tok), llm::LlmError);
    }

    SUBCASE("开关开着：流式那条边生边给，一段都不少") {
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        llm::RemoteClient c(
            cfg,
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&,
               double) { return ok("{}"); },
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&, double,
               const llm::OnChunk& on_chunk) {
                const std::string sse =
                    "data: {\"choices\":[{\"delta\":{\"content\":\"{\"}}]}\n\n"
                    "data: {\"choices\":[{\"delta\":{\"content\":\"}\"}}]}\n\n"
                    "data: [DONE]\n\n";
                on_chunk(sse.data(), sse.size());
                return llm::HttpResponse{200, "", std::nullopt};
            });
        pipeline::CancelToken tok;
        std::string got;
        CHECK(c.complete(simple_req(), tok,
                         [&got](const std::string& p) { got += p; }) == "{}");
        // 逐字那一路一段都不许被记录器吃掉
        CHECK(got == "{}");
    }

    SUBCASE("开关开着：带工具那条回来的还是原来那个 ChatReply") {
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        FakeHttp http;
        http.responses.push_back(ok("查到了"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        std::vector<llm::Message> msgs{{"system", "你是场记", {}, ""},
                                       {"user", "找找今天有什么热点", {}, ""}};
        llm::Request opts;
        opts.schema_name = "story_from_web";
        const llm::ChatReply reply =
            c.chat(msgs, nlohmann::ordered_json::array(), opts, tok);
        CHECK(reply.content == "查到了");
        CHECK(reply.tool_calls.empty());
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("messages").size() == 2);
        // 空工具表不许被塞进请求体里
        CHECK_FALSE(http.calls[0].body.contains("tools"));
    }

    std::filesystem::remove_all(root, ec);
}


TEST_CASE("索引里那三栏记的是真发出去的那份，不是 req 里填的那份") {
    // **这是整件事的目的所在。** 用户要研究的就是"换一个参数，出来的东西
    // 差在哪儿"，而账上这一栏对不上的话，两桶数据发的其实是同一份请求，
    // 差异全是噪声。
    //
    // 失败场景（真的）：`pipeline/storyboard_run.cpp` **无条件**写
    // `req.reasoning_effort = "high"`；而 `build_payload` 只在模型名以
    // `glm-5` 打头时才真把它写进请求体（见那儿「只对认得这个参数的模型说」
    // 那段）。`task_models` 默认是空的，拆分镜落到兜底的 `glm-4.7-flash`,
    // 请求体里一个 `reasoning_effort` 都没有，账上却写着 "high"。
    // docs/提示词日志.md 上那句「真发出去才有值」说的就是这一栏。
    //
    // 所以这几条都**两头对着看**：发出去的那份里有没有、账上那一栏是什么。
    // 只看账上的话，把 set_model 挪回 build_payload 前面这种改动照样过。
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cj_llm_log_payload_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR", paths::to_utf8(root));

    config::LLMConfig cfg = test_cfg();
    cfg.call_log = true;
    llm::Request req = simple_req();
    req.reasoning_effort = "high";   // 上游那一下是无条件写的
    req.temperature = 0.42;          // 盖过 cfg.temperature，账上该跟着它

    SUBCASE("整段那条：模型不是 glm-5，请求体里没有，账上就得是空的") {
        cfg.model = "glm-4.7-flash";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        REQUIRE_FALSE(http.calls[0].body.contains("reasoning_effort"));
        const auto rows = read_log_index(root);
        REQUIRE(rows.size() == 1);
        CHECK_MESSAGE(rows[0].at("reasoning_effort") == "",
                      "请求体里一个字都没发，账上不许写着 high");
        CHECK(rows[0].at("model") == "glm-4.7-flash");
        CHECK(rows[0].at("temperature").get<double>() == doctest::Approx(0.42));
    }

    SUBCASE("整段那条：模型是 glm-5，请求体里有，账上就得有") {
        cfg.model = "glm-5.3-flash";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("reasoning_effort") == "high");
        const auto rows = read_log_index(root);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].at("reasoning_effort") == "high");
        CHECK(rows[0].at("model") == "glm-5.3-flash");
    }

    SUBCASE("流式那条：payload 在 run() 里才拼，记的还得是它") {
        // 这一条的 set_model 原来在 run() 外面、build_payload 之前——
        // 三条路里它最容易改错，所以单独钉一条。
        cfg.model = "glm-4.7-flash";
        json sent;
        llm::RemoteClient c(
            cfg,
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&,
               double) { return ok("{}"); },
            [&sent](const std::string&, const std::string& body,
                    const std::map<std::string, std::string>&, double,
                    const llm::OnChunk& on_chunk) {
                sent = json::parse(body, nullptr, false);
                const std::string sse =
                    "data: {\"choices\":[{\"delta\":{\"content\":\"{}\"}}]}\n\n"
                    "data: [DONE]\n\n";
                on_chunk(sse.data(), sse.size());
                return llm::HttpResponse{200, "", std::nullopt};
            });
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok, [](const std::string&) {}) == "{}");
        REQUIRE(sent.is_object());
        REQUIRE_FALSE(sent.contains("reasoning_effort"));
        const auto rows = read_log_index(root);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].at("reasoning_effort") == "");
        CHECK(rows[0].at("model") == "glm-4.7-flash");
        CHECK(rows[0].at("temperature").get<double>() == doctest::Approx(0.42));
    }

    SUBCASE("带工具那条：同一个 glm-5 的门，同一条道理") {
        cfg.model = "glm-4.7-flash";
        FakeHttp http;
        http.responses.push_back(ok("查到了"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        std::vector<llm::Message> msgs{{"user", "找找今天有什么热点", {}, ""}};
        llm::Request opts;
        opts.schema_name = "story_from_web";
        opts.reasoning_effort = "high";
        opts.temperature = 0.42;
        CHECK(c.chat(msgs, nlohmann::ordered_json::array(), opts, tok).content ==
              "查到了");
        REQUIRE(http.calls.size() == 1);
        REQUIRE_FALSE(http.calls[0].body.contains("reasoning_effort"));
        const auto rows = read_log_index(root);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].at("reasoning_effort") == "");
        CHECK(rows[0].at("model") == "glm-4.7-flash");
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("想多久：req 没填就问配置，[llm.effort] 那一档压过全局") {
    // 2026-09-19 之前这个档位是各步代码里写死的 `req.reasoning_effort =
    // "high"`，于是两件事同时成立：出大纲那几步没人写，走的是智谱的默认
    // max（实测一趟十几分钟不回）；而模型窗里那个档位调了也不算数。
    //
    // 现在的规矩是：`req` 里填了用填的，没填问 `cfg.effort_for(schema_name)`。
    // 下面这几条按**发出去的请求体**判——只看配置对象的话，
    // "读了配置但忘了写进 payload" 这种改动照样过。
    config::LLMConfig cfg = test_cfg();
    cfg.model = "glm-5.3";           // 门开着，不然发不出去
    llm::Request req = simple_req();
    // **用一个没有自己默认的步骤测全局那条。** 正文（chapter）2026-09-20
    // 起自己默认 low（见 `effort_for` 里 kPerTask 那段），拿它测"全局默认"
    // 测的就不是全局了。
    req.schema_name = "story_analysis";
    REQUIRE(req.reasoning_effort.empty());

    SUBCASE("req 空着：发出去的是配置里那个默认") {
        CHECK(cfg.reasoning_effort == "high");   // 默认就得是 high
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("reasoning_effort") == "high");
    }

    SUBCASE("配置改了，发出去的跟着改——模型窗那个旋钮就是靠这一条") {
        cfg.reasoning_effort = "low";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK_MESSAGE(http.calls[0].body.at("reasoning_effort") == "low",
                      "配置里调低了，发出去的还是 high：旋钮被架空了");
    }

    SUBCASE("正文自己默认 low，不跟全局") {
        // 2026-09-20 拍的：写正文一次 315 秒 → 64 秒，思考从正文的 8.2 倍
        // 掉到 0.4 倍，而闸门第一轮通过率不比 high 差。理由和数写在
        // `effort_for` 里那段。
        cfg.reasoning_effort = "high";
        llm::Request ch = simple_req();
        ch.schema_name = "chapter";
        FakeHttp h2;
        h2.responses.push_back(ok("{}"));
        llm::RemoteClient c2(cfg, h2.fn());
        pipeline::CancelToken t2;
        CHECK(c2.complete(ch, t2) == "{}");
        REQUIRE(h2.calls.size() == 1);
        CHECK(h2.calls[0].body.at("reasoning_effort") == "low");
    }

    SUBCASE("配置点名了就压过那个默认") {
        cfg.reasoning_effort = "high";
        cfg.task_efforts["story_analysis"] = "max";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("reasoning_effort") == "max");
    }

    SUBCASE("别的任务不受那一档影响") {
        cfg.reasoning_effort = "high";
        cfg.task_efforts["storyboard"] = "minimal";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("reasoning_effort") == "high");
    }

    SUBCASE("配置留空：一个字段都不发") {
        cfg.reasoning_effort = "";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK_FALSE(http.calls[0].body.contains("reasoning_effort"));
        CHECK_FALSE(http.calls[0].body.contains("thinking"));
    }

    SUBCASE("req 填了就以 req 为准（单测和特例的口子）") {
        cfg.reasoning_effort = "low";
        req.reasoning_effort = "max";
        FakeHttp http;
        http.responses.push_back(ok("{}"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(req, tok) == "{}");
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("reasoning_effort") == "max");
    }
}

TEST_CASE("想多久：档位拼错了要在起服务时就拦住") {
    // 拼错一个字母的代价是服务回 400，而那一下会让**所有**生成一起挂。
    config::LLMConfig cfg = test_cfg();
    cfg.reasoning_effort = "hight";
    auto errs = cfg.validate();
    REQUIRE_FALSE(errs.empty());
    CHECK(errs.front().find("llm.reasoning_effort") != std::string::npos);

    cfg.reasoning_effort = "high";
    CHECK(cfg.validate().empty());

    cfg.task_efforts["chapter"] = "ludicrous";
    errs = cfg.validate();
    REQUIRE_FALSE(errs.empty());
    CHECK(errs.front().find("llm.effort.chapter") != std::string::npos);

    // 空串是"不发这个字段"，合法。
    cfg.task_efforts.clear();
    cfg.reasoning_effort = "";
    CHECK(cfg.validate().empty());
}

TEST_CASE("网关收下 stream 却回一份普通 JSON：正文救回来，思考也要救回来") {
    // 这条退路本来就在（build_payload 上那段注释说的就是这类网关），原来
    // 里面只有 completion_reason + extract_content：正文救得回来，**思考
    // 整段丢掉**——thinking.txt 不写、thinking_chars 是 0。另外两条路都已经
    // 无条件抽了，只有这一支漏了。
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cj_llm_log_nostream_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR", paths::to_utf8(root));

    json msg;
    msg["content"] = "{}";
    msg["reasoning_content"] = "changji_think_marker 先看看这一章在说什么";
    json choice;
    choice["message"] = msg;
    choice["finish_reason"] = "stop";
    json body;
    body["choices"] = json::array({choice});

    config::LLMConfig cfg = test_cfg();
    cfg.call_log = true;
    // 一段 SSE 都不推，直接回一份普通的 JSON——这就是"收下了 stream 却当
    // 整段回"的网关。
    llm::RemoteClient c(
        cfg,
        [](const std::string&, const std::string&,
           const std::map<std::string, std::string>&,
           double) { return ok("{}"); },
        [&body](const std::string&, const std::string&,
                const std::map<std::string, std::string>&, double,
                const llm::OnChunk&) {
            return llm::HttpResponse{200, body.dump(), std::nullopt};
        });

    llm::Request req = simple_req();
    std::string seen;
    req.on_thinking = [&seen](const std::string& t) { seen += t; };
    pipeline::CancelToken tok;
    CHECK(c.complete(req, tok, [](const std::string&) {}) == "{}");

    // 界面那一头照样收得到（另外两条路就是这么给的）
    CHECK(seen.find("changji_think_marker") != std::string::npos);

    const auto rows = read_log_index(root);
    REQUIRE(rows.size() == 1);
    CHECK_MESSAGE(rows[0].at("thinking_chars").get<int>() > 0,
                  "正文救回来了，思考不能丢");
    CHECK_MESSAGE(slurp_log_tree(root).find("changji_think_marker") !=
                      std::string::npos,
                  "thinking.txt 得真写出来，光记个字数没法拿去读");

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("日志那头怎么坏，都不许把这一次生成带走") {
    // call_log.hpp 上那条「写盘出问题不许影响生成」原来只落在**析构**那一半，
    // **构造**那一半是漏的：`call_log_options` 里那句
    // `paths::from_utf8(paths::env("CHANGJI_LLM_LOG_DIR"))` 走
    // `std::filesystem::u8path`，MSVC 上转不动的串会抛 `filesystem_error`,
    // 而它**不是 LlmError**——批量那几条只 `catch (const LlmError&)`，
    // 一整批当场终止。四个出入口现在都把构造包了一层（见 client.cpp 的
    // open_call_log）。
    //
    // ⚠️ **这条用例在 POSIX 上触发不了那次转换失败**：u8path 在这儿不校验
    // 字节。它钉的是能跨平台钉的那一半——日志目录根本建不出来时，成功那条
    // 的返回值、砸了那条的异常类型和 status，都和"没开日志"时一个样。
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "cj_llm_log_hostile";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    const std::filesystem::path blocker = base / "not_a_dir";
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "我是个普通文件，底下建不出目录";
    }
    // 根指到一个普通文件底下：建目录、写 index.jsonl 每一步都会失败。
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR",
                                  paths::to_utf8(blocker / "llm_log"));

    SUBCASE("成功那条：返回值一个字没变") {
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        FakeHttp http;
        http.responses.push_back(ok(R"({"a":1})"));
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        CHECK(c.complete(simple_req(), tok) == R"({"a":1})");
    }

    SUBCASE("砸了那条：抛的还是 LlmError，status 还挂着") {
        config::LLMConfig cfg = test_cfg();
        cfg.call_log = true;
        FakeHttp http;
        http.responses.push_back(
            llm::HttpResponse{401, R"({"error":"bad key"})", std::nullopt});
        llm::RemoteClient c(cfg, http.fn());
        pipeline::CancelToken tok;
        try {
            c.complete(simple_req(), tok);
            FAIL("该抛的没抛");
        } catch (const llm::LlmError& e) {
            CHECK(e.status() == 401);
            CHECK(e.is_config_error());
        }
    }

    SUBCASE("命令行那条也一样") {
        config::LLMConfig cfg;
        cfg.backend = "command";
        cfg.command = "";
        cfg.call_log = true;
        llm::CommandClient cli(cfg);
        pipeline::CancelToken tok;
        llm::Request req;
        req.prompt = "写一章剧本";
        CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                             doctest::Contains("llm.command"), llm::LlmError);
    }

    std::filesystem::remove_all(base, ec);
}

TEST_CASE("chat 走流式：正文一段段给，工具调用按 index 合回来") {
    // 2026-09-19 用户报的：故事页右下角那颗（上网看热点写这一章）按下去，
    // 编辑器一百多秒一个字都不动。那条走的是 chat()，而 chat() 原来**只有
    // 整段 POST 一条路**——正文没有出口，连思考都要等这一轮收完才一次性给。
    //
    // 这条用例钉的是三件事：请求带 stream、正文逐段回调、工具调用按 index
    // 从几十条碎片合回一个。第三件是这条路独有的：complete 那边没有工具。
    FakeHttp http;
    FakeStream stream;
    stream.chunks = {{
        // 头一条只有 role，什么都不该回调
        sse_delta(json{{"role", "assistant"}}),
        // 思考单独一路，不许混进正文
        sse_delta(json{{"reasoning_content", "先看看热榜"}}),
        // 工具调用：第一条带 index + id + 名字，后面只给 arguments 的一小段
        sse_delta(json{{"tool_calls", json::array({json{
                           {"index", 0},
                           {"id", "call_a"},
                           {"function", json{{"name", "web_search"},
                                             {"arguments", "{\"qu"}}}}})}}),
        sse_delta(json{{"tool_calls", json::array({json{
                           {"index", 0},
                           {"function", json{{"arguments", "ery\":\"夜班\"}"}}}}})}}),
        sse_delta(json::object(), "tool_calls"),
        "data: [DONE]\n\n",
    }};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    std::string thought;
    llm::Request opts;
    opts.schema_name = "web_chapter";
    opts.on_token = [&](const std::string& p) { pieces.push_back(p); };
    opts.on_thinking = [&](const std::string& p) { thought += p; };

    const llm::ChatReply r = c.chat({}, json::array({json{{"type", "function"}}}), opts, tok);

    CHECK(http.calls.empty());        // 整段那条一次都不该走
    REQUIRE(stream.calls.size() == 1);
    CHECK(stream.calls[0].body.at("stream") == true);
    CHECK(stream.calls[0].headers.at("Accept") == "text/event-stream");
    CHECK(thought == "先看看热榜");
    CHECK(pieces.empty());            // 这一轮它只调工具，没说话
    CHECK(r.finish_reason == "tool_calls");
    REQUIRE(r.tool_calls.size() == 1);
    CHECK(r.tool_calls[0].id == "call_a");
    CHECK(r.tool_calls[0].name == "web_search");
    // **合起来才是一份能解的 JSON。** 不合的话这儿是 `{"qu`，
    // 而上层 run_web_tool 会回一句"参数不是 json"，看着像模型犯浑。
    CHECK(r.tool_calls[0].arguments == "{\"query\":\"夜班\"}");

    SUBCASE("它开口写的那一轮，正文是一段一段来的") {
        FakeStream s2;
        s2.chunks = {{sse_chunk("{\\\"text\\\": \\\"凌晨"),
                      sse_chunk("两点\\\"}"), "data: [DONE]\n\n"}};
        llm::RemoteClient c2(test_cfg(), http.fn(), s2.fn());
        std::vector<std::string> got;
        llm::Request o2;
        o2.on_token = [&](const std::string& p) { got.push_back(p); };
        const llm::ChatReply w = c2.chat({}, json::array(), o2, tok);
        CHECK(got == std::vector<std::string>{"{\"text\": \"凌晨", "两点\"}"});
        CHECK(w.content == "{\"text\": \"凌晨两点\"}");
        CHECK(w.tool_calls.empty());
    }
}

TEST_CASE("chat：没人要看就还是整段那条") {
    // **别让这条路自己变流式。** 没人接的时候走 SSE 只是多一份解析、
    // 多一个"服务端不认 stream"的分支，白担风险。
    FakeHttp http;
    http.responses = {ok("{\"title\":\"整段回来的\"}")};
    FakeStream stream;
    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    llm::Request opts;   // on_token / on_thinking 都不填
    const llm::ChatReply r = c.chat({}, json::array(), opts, tok);
    CHECK(r.content == "{\"title\":\"整段回来的\"}");
    CHECK(stream.calls.empty());
    REQUIRE(http.calls.size() == 1);
    CHECK_FALSE(http.calls[0].body.contains("stream"));
}

TEST_CASE("chat 流式：服务端不认 stream，回了一份普通 JSON") {
    // 和 complete_stream 那条同一条退路：老服务照样能用，只是那一下是整段
    // 到的。**不为同一个请求再发一次**——重复生成、重复计费。
    FakeHttp http;
    const json whole = {
        {"choices", json::array({json{
                        {"message", json{{"content", "{\"text\":\"整段\"}"},
                                         {"reasoning_content", "想了想"}}},
                        {"finish_reason", "stop"}}})}};
    int stream_calls = 0;
    llm::RemoteClient c(test_cfg(), http.fn(),
                        [&](const std::string&, const std::string&,
                            const std::map<std::string, std::string>&, double,
                            const llm::OnChunk&) {
                            ++stream_calls;   // 一条 SSE 都不推，直接回整份
                            return llm::HttpResponse{200, whole.dump(), std::nullopt};
                        });
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    std::string thought;
    llm::Request opts;
    opts.on_token = [&](const std::string& p) { pieces.push_back(p); };
    opts.on_thinking = [&](const std::string& p) { thought += p; };
    const llm::ChatReply r = c.chat({}, json::array(), opts, tok);
    CHECK(r.content == "{\"text\":\"整段\"}");
    CHECK(pieces == std::vector<std::string>{"{\"text\":\"整段\"}"});
    CHECK(thought == "想了想");
    CHECK(r.finish_reason == "stop");
    CHECK(stream_calls == 1);
    CHECK(http.calls.empty());   // 不许为同一个请求再发一次
}

// ---- 「关掉思考」被拒：换 low 再发一趟 ----
//
// 2026-09-23 实撞：用户配着 `reasoning_effort = "off"`，glm-5.3 走
// `…/api/paas/v4` 回 400「该模型始终思考，不支持关闭思考」，读故事那一步
// 整个挂掉。同一个模型走 `…/api/coding/paas/v4` 却收 disabled——能不能关
// 跟着地址变，所以只能被拒一次、记下来。
//
// ⚠️ 每条用例用**自己的地址**：备忘是进程里的，按「地址 + 模型」记，
// 共用一个地址的话后跑的那条第一趟就已经是 low，测不到退档那一下。

namespace {

const char* kAlwaysThinks =
    R"({"error":{"code":"1210","message":"该模型始终思考，不支持关闭思考；请使用 low、high 或 max。"}})";

config::LLMConfig always_thinks_cfg(const std::string& base_url) {
    config::LLMConfig c = test_cfg();
    c.base_url = base_url;
    c.model = "glm-5.3";
    return c;
}

llm::Request off_req() {
    llm::Request r = simple_req();
    r.reasoning_effort = "off";
    return r;
}

bool sent_off(const json& body) {
    return body.contains("thinking") && body["thinking"].value("type", "") == "disabled";
}

bool sent_low(const json& body) {
    return body.contains("thinking") && body["thinking"].value("type", "") == "enabled" &&
           body.value("reasoning_effort", "") == "low";
}

}  // namespace

TEST_CASE("关不掉思考：整段那条换 low 再发一趟，之后直接发 low") {
    FakeHttp http;
    http.responses = {llm::HttpResponse{400, kAlwaysThinks, std::nullopt},
                      ok("{\"a\":1}"), ok("{\"a\":2}")};
    llm::RemoteClient c(always_thinks_cfg("http://always-thinks-whole/v4"), http.fn());
    pipeline::CancelToken tok;

    CHECK(c.complete(off_req(), tok) == "{\"a\":1}");
    REQUIRE(http.calls.size() == 2);
    CHECK(sent_off(http.calls[0].body));
    CHECK(sent_low(http.calls[1].body));

    // 记住了：第二次不再先挨一个 400。
    CHECK(c.complete(off_req(), tok) == "{\"a\":2}");
    REQUIRE(http.calls.size() == 3);
    CHECK(sent_low(http.calls[2].body));
}

TEST_CASE("关不掉思考：别的 400 照旧报错，不重发") {
    // 只认「发的是 disabled、回的在说思考」。别的 400（模型名写错之类）
    // 重发一遍也还是 400，只是白多一趟、账上多一行。
    FakeHttp http;
    http.responses = {llm::HttpResponse{400, R"({"error":{"code":"1211","message":"模型不存在"}})",
                                        std::nullopt}};
    llm::RemoteClient c(always_thinks_cfg("http://always-thinks-other400/v4"), http.fn());
    pipeline::CancelToken tok;
    CHECK_THROWS_AS(c.complete(off_req(), tok), llm::LlmError);
    CHECK(http.calls.size() == 1);

    // 没发 disabled 时回包里就算提到思考，也不是我们这一档的事。
    FakeHttp h2;
    h2.responses = {llm::HttpResponse{400, kAlwaysThinks, std::nullopt}};
    llm::RemoteClient c2(always_thinks_cfg("http://always-thinks-notoff/v4"), h2.fn());
    llm::Request high = simple_req();
    high.reasoning_effort = "high";
    CHECK_THROWS_AS(c2.complete(high, tok), llm::LlmError);
    CHECK(h2.calls.size() == 1);
}

TEST_CASE("关不掉思考：流式那条同样换 low 再发一趟") {
    FakeHttp http;
    FakeStream stream;
    stream.statuses = {400, 200};
    stream.error_body = kAlwaysThinks;
    stream.chunks = {{}, {sse_chunk("{\\\"t\\\": 1}"), "data: [DONE]\n\n"}};
    llm::RemoteClient c(always_thinks_cfg("http://always-thinks-stream/v4"), http.fn(),
                        stream.fn());
    pipeline::CancelToken tok;
    std::string got;
    CHECK(c.complete(off_req(), tok, [&](const std::string& p) { got += p; }) ==
          "{\"t\": 1}");
    CHECK(got == "{\"t\": 1}");
    REQUIRE(stream.calls.size() == 2);
    CHECK(sent_off(stream.calls[0].body));
    CHECK(sent_low(stream.calls[1].body));
    CHECK(stream.calls[1].body.at("stream") == true);
    CHECK(http.calls.empty());
}

TEST_CASE("关不掉思考：带工具那条（整段和流式）同样换 low") {
    pipeline::CancelToken tok;
    llm::Request opts;
    opts.reasoning_effort = "off";
    {
        FakeHttp http;
        http.responses = {llm::HttpResponse{400, kAlwaysThinks, std::nullopt},
                          ok("整段回来的")};
        llm::RemoteClient c(always_thinks_cfg("http://always-thinks-chat/v4"), http.fn());
        CHECK(c.chat({}, json::array(), opts, tok).content == "整段回来的");
        REQUIRE(http.calls.size() == 2);
        CHECK(sent_off(http.calls[0].body));
        CHECK(sent_low(http.calls[1].body));
    }
    {
        FakeHttp http;
        FakeStream stream;
        stream.statuses = {400, 200};
        stream.error_body = kAlwaysThinks;
        stream.chunks = {{}, {sse_chunk("流式回来的"), "data: [DONE]\n\n"}};
        llm::RemoteClient c(always_thinks_cfg("http://always-thinks-chat-live/v4"),
                            http.fn(), stream.fn());
        llm::Request live = opts;
        std::string got;
        live.on_token = [&](const std::string& p) { got += p; };
        CHECK(c.chat({}, json::array(), live, tok).content == "流式回来的");
        CHECK(got == "流式回来的");
        REQUIRE(stream.calls.size() == 2);
        CHECK(sent_off(stream.calls[0].body));
        CHECK(sent_low(stream.calls[1].body));
        CHECK(http.calls.empty());
    }
}
