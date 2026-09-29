// 按对话挑模型（`llm/chat_pick.hpp`）、自己加的服务、本地编剧模型那一张单子。
//
// 用户 2026-09-26：「大模型选择应该会话隔离」「会话里的模型名和模型地址持久化
// 保存」「通过 api 和 key 和名字添加一个新大模型」「本地下载一个模型后……厂商
// 增加一个本地」。这几条钉的是**会真出事**的那几处：换了家还带着上一家的钥匙、
// 派出去的活丢了这条对话挑的模型、挑法没跟着对话搬家。

#include <doctest/doctest.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "http/llm_info.hpp"
#include "http/offload.hpp"
#include "llm/chat_pick.hpp"
#include "llm/local_client.hpp"
#include "util/paths.hpp"

#include "scoped_env.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

config::Settings base_settings() {
    config::Settings s;
    s.llm.backend = "remote";
    s.llm.base_url = "https://global.example.com/v1";
    s.llm.model = "global-model";
    s.llm.api_key = "global-key";
    s.llm.reasoning_effort = "high";
    s.llm.task_models = {{"chapter", "big-writer"}};
    s.llm.task_efforts = {{"storyboard", "low"}};
    return s;
}

fs::path fresh_dir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path() / paths::from_utf8("changji_pick_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

TEST_CASE("叠上对话挑的那一家：地址、模型、钥匙一起换") {
    changji::test::ScopedUserConfigDir iso("pick-key");
    const std::string other = "https://other.example.com/v1";
    config::write_api_key_for(other, "other-key");
    const auto s = base_settings();

    llm::ChatPick p;
    p.backend = "remote";
    p.base_url = other;
    p.model = "their-model";
    const auto c = llm::with_pick(s, p);
    CHECK(c.backend == "remote");
    CHECK(c.base_url == other);
    CHECK(c.model == "their-model");
    // ⚠️ 带着上一家的钥匙去敲这一家，回来是 401，看着像"这个模型不让你用"。
    CHECK(c.api_key == "other-key");
    // 人在这条对话里挑了一个模型：写正文也用它，不再按任务分流到全局那张表。
    CHECK(c.model_for("chapter") == "their-model");
    // 没挑想多久：跟全局，按任务分档的那张表也还在。
    CHECK(c.effort_for("storyboard") == "low");

    SUBCASE("刚换了家还没挑模型：留全局那个名字，不发一个空名字出去") {
        p.model.clear();
        CHECK(llm::with_pick(s, p).model == "global-model");
    }
    SUBCASE("挑了想多久：按任务分档的那张表让位给它") {
        p.effort = "max";
        const auto e = llm::with_pick(s, p);
        CHECK(e.effort_for("storyboard") == "max");
        CHECK(e.effort_for("script") == "max");
        // 写正文那一步自己的默认（量过的 low）照旧——原来输入框底下那颗改全局
        // `reasoning_effort` 时也管不到它，按对话分之后不该悄悄变了。
        CHECK(e.effort_for("chapter") == s.llm.effort_for("chapter"));
    }
    SUBCASE("空串不算挑过：落回全局和按任务分档的那张表") {
        // 2026-09-27 之前空串是一档（「爱想不想」= 不发那个字段），而智谱的默认是
        // max：一条对话存着空串，拆分镜一场想八九万字、一刻钟到半小时，全局配的
        // low 管不到它。那一档去掉了，空串按没挑读。
        p.effort = "";
        const auto e = llm::with_pick(s, p);
        CHECK(e.effort_for("storyboard") == "low");
        CHECK(e.effort_for("script") == "high");
        // 读盘、落盘同样不认它。
        CHECK_FALSE(llm::pick_from_json(nlohmann::json{{"backend", "remote"},
                                                       {"base_url", "https://x.example.com/v1"},
                                                       {"model", "m"},
                                                       {"effort", ""}})
                        .effort.has_value());
        CHECK_FALSE(llm::to_json(p).contains("effort"));
    }
}

TEST_CASE("挂在线程上：没立就是全局，嵌套出来恢复外面那个") {
    const auto s = base_settings();
    CHECK_FALSE(llm::current_pick().has_value());
    CHECK(llm::effective_llm(s).model == "global-model");
    {
        llm::ChatPick a;
        a.backend = "remote";
        a.base_url = "https://a.example.com/v1";
        a.model = "a";
        const llm::PickScope sa{a};
        CHECK(llm::effective_llm(s).model == "a");
        {
            // 明确地「没挑」：在一条挑过的线程上跑一件不属于任何对话的活。
            const llm::PickScope none{std::nullopt};
            CHECK(llm::effective_llm(s).model == "global-model");
        }
        CHECK(llm::effective_llm(s).model == "a");
    }
    CHECK_FALSE(llm::current_pick().has_value());
}

TEST_CASE("挪到后台线程上的活带着派活那条线程挑的模型") {
    // 写大纲、读网页那几件活是 `Offload` 挪到后台线程上干的。不抄过去的话，对话里
    // 挑了 A、写出来的大纲走的是全局那一家——一声不响。
    llm::ChatPick a;
    a.backend = "remote";
    a.base_url = "https://a.example.com/v1";
    a.model = "carried";

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::optional<llm::ChatPick> seen;
    {
        const llm::PickScope sa{a};
        http::Offload::instance().post([&] {
            const auto p = llm::current_pick();
            std::lock_guard lg(mu);
            seen = p;
            done = true;
            cv.notify_all();
        });
    }
    std::unique_lock lk(mu);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; }));
    REQUIRE(seen.has_value());
    CHECK(seen->model == "carried");
}

TEST_CASE("挑法落在对话旁边：读回来、清空、搬家、分叉、删掉") {
    const fs::path root = fresh_dir("disk");
    CHECK_FALSE(llm::load_chat_pick(root, "c1").has_value());

    llm::ChatPick p;
    p.backend = "remote";
    p.base_url = "https://a.example.com/v1";
    p.model = "m";
    p.effort = "low";
    llm::save_chat_pick(root, "c1", p);
    // 一直以来那一条（空编号）另存一份，不和 c1 串。
    CHECK(llm::chat_pick_path(root, "") == root / "chat.model.json");
    CHECK(llm::chat_pick_path(root, "c1") == root / "chats" / "c1.model.json");

    const auto back = llm::load_chat_pick(root, "c1");
    REQUIRE(back.has_value());
    CHECK(back->base_url == "https://a.example.com/v1");
    CHECK(back->model == "m");
    CHECK(back->effort == std::optional<std::string>("low"));
    CHECK_FALSE(llm::load_chat_pick(root, "").has_value());

    // 文件里**不许有密钥**：片子是会被拷来拷去的。
    {
        std::ifstream in(llm::chat_pick_path(root, "c1"));
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("key") == std::string::npos);
    }

    SUBCASE("分叉：新的那条接着用") {
        llm::copy_chat_pick(root, "c1", "c2");
        REQUIRE(llm::load_chat_pick(root, "c2").has_value());
        CHECK(llm::load_chat_pick(root, "c2")->model == "m");
    }
    SUBCASE("搬家：跟着过去，原处不留") {
        const fs::path to = fresh_dir("disk_to");
        llm::move_chat_pick(root, to, "c1");
        CHECK(llm::load_chat_pick(to, "c1").has_value());
        CHECK_FALSE(llm::load_chat_pick(root, "c1").has_value());
        std::error_code ec;
        fs::remove_all(to, ec);
    }
    SUBCASE("删掉") {
        llm::remove_chat_pick(root, "c1");
        CHECK_FALSE(llm::load_chat_pick(root, "c1").has_value());
    }
    SUBCASE("清空 = 删文件（跟全局）") {
        llm::save_chat_pick(root, "c1", llm::ChatPick{});
        CHECK_FALSE(fs::exists(llm::chat_pick_path(root, "c1")));
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("手改坏了的一份当没挑，不让对话开不了口") {
    const fs::path root = fresh_dir("bad");
    fs::create_directories(root / "chats");
    {
        std::ofstream out(llm::chat_pick_path(root, "c1"));
        out << "{ 这不是 json";
    }
    CHECK_FALSE(llm::load_chat_pick(root, "c1").has_value());
    {
        std::ofstream out(llm::chat_pick_path(root, "c1"));
        out << R"({"backend":"命令行","base_url":"x","model":"y"})";
    }
    CHECK_FALSE(llm::load_chat_pick(root, "c1").has_value());
    std::error_code ec;
    fs::remove_all(root, ec);
}

namespace {

/// 这一段里运行时那份换成 `s`，出去还原（挑模型会顺手改默认，改的就是它）。
struct RuntimeAs {
    config::Settings old = config::runtime().snapshot();
    explicit RuntimeAs(const config::Settings& s) { config::runtime().replace(s); }
    ~RuntimeAs() { config::runtime().replace(old); }
};

}  // namespace

TEST_CASE("对话挑模型的接口：换了家清掉上一家的模型名") {
    changji::test::ScopedUserConfigDir iso("pick-api");
    const fs::path root = fresh_dir("api");
    const std::string proj = paths::to_utf8(root);
    const auto s = base_settings();
    const RuntimeAs rt{s};

    auto r = http::get_chat_model(proj, "c1", s);
    CHECK(r.body.at("pick").is_null());
    CHECK(r.body.at("effective").at("model") == "global-model");
    CHECK(r.body.at("effective").at("effort") == "high");

    r = http::post_chat_model(
        json{{"project", proj}, {"chat", "c1"}, {"backend", "remote"},
             {"base_url", "https://a.example.com/v1/"}, {"model", "a-1"}},
        s);
    CHECK(r.body.at("effective").at("base_url") == "https://a.example.com/v1");
    CHECK(r.body.at("effective").at("model") == "a-1");

    // 只换家不带名字：名字清空（上一家的名字在这一家多半不存在）。
    r = http::post_chat_model(json{{"project", proj}, {"chat", "c1"}, {"backend", "remote"},
                                   {"base_url", "https://b.example.com/v1"}},
                              s);
    CHECK(r.body.at("effective").at("model") == "");
    r = http::post_chat_model(json{{"project", proj}, {"chat", "c1"}, {"model", "b-2"}}, s);
    CHECK(r.body.at("effective").at("model") == "b-2");

    // 另一条对话一个字都不受影响。
    CHECK(http::get_chat_model(proj, "c2", s).body.at("pick").is_null());

    r = http::post_chat_model(json{{"project", proj}, {"chat", "c1"}, {"effort", "low"}}, s);
    CHECK(r.body.at("effective").at("effort") == "low");
    CHECK(r.body.at("effective").at("model") == "b-2");

    // 最后一次挑的成了默认：新对话（没挑过的 c2）用它，地址和钥匙一起换。
    CHECK(config::runtime().snapshot().llm.model == "b-2");
    CHECK(config::runtime().snapshot().llm.base_url == "https://b.example.com/v1");
    CHECK(http::get_chat_model(proj, "c2", config::runtime().snapshot())
              .body.at("effective").at("model") == "b-2");
    // 也写进了配置文件，重启还在。
    CHECK(config::load_settings().llm.model == "b-2");

    // 跟全局：家和模型清掉，想多久留着。默认就是刚才最后挑的那个。
    r = http::post_chat_model(json{{"project", proj}, {"chat", "c1"}, {"backend", "global"}},
                              s);
    CHECK(r.body.at("effective").at("model") == "b-2");
    CHECK(r.body.at("effective").at("effort") == "low");

    // 想多久发空串 = 这条对话不再自己挑，跟全局；**不是**「不发字段、随服务默认」
    // 那一档（去掉了）。全局这会儿是 low（刚才那一下记成了默认），空串也不许把它
    // 冲成空。
    r = http::post_chat_model(json{{"project", proj}, {"chat", "c1"}, {"effort", ""}}, s);
    CHECK(r.body.at("effective").at("effort") == "low");
    const bool pick_has_effort =
        r.body.at("pick").is_object() && r.body.at("pick").contains("effort");
    CHECK_FALSE(pick_has_effort);
    CHECK(config::runtime().snapshot().llm.reasoning_effort == "low");

    CHECK_THROWS_AS(http::post_chat_model(json{{"project", proj},
                                               {"chat", "../x"},
                                               {"model", "m"}},
                                          s),
                    http::ApiError);
    CHECK_THROWS_AS(http::post_chat_model(json{{"project", proj},
                                               {"chat", "c1"},
                                               {"backend", "remote"},
                                               {"base_url", "ftp://x"}},
                                          s),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("换了家还没挑模型：默认不动") {
    changji::test::ScopedUserConfigDir iso("pick-half");
    const fs::path root = fresh_dir("half");
    const auto s = base_settings();
    const RuntimeAs rt{s};
    http::post_chat_model(json{{"project", paths::to_utf8(root)}, {"chat", "c1"},
                               {"backend", "remote"}, {"base_url", "https://z.example.com/v1"}},
                          s);
    CHECK(config::runtime().snapshot().llm.base_url == "https://global.example.com/v1");
    CHECK(config::runtime().snapshot().llm.model == "global-model");
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("每一家的模型单子记在盘上：新出来的标出来，看过就消掉") {
    changji::test::ScopedUserConfigDir iso("models-cache");
    const std::string url = "https://m.example.com/v1";
    const auto got = [&](std::vector<std::string> names) {
        return http::remember_models(url, json{{"models", names}});
    };
    // 头一回：全算看过。
    auto r = got({"a", "b"});
    CHECK(r.at("new") == json::array());
    // 多出来一个 c：标出来。
    r = got({"a", "b", "c"});
    CHECK(r.at("new") == json::array({"c"}));
    // 问不到：拿上一次的顶上，c 还是新的。
    r = http::remember_models(url, json{{"models", json::array()}, {"error", "连不上"}});
    CHECK(r.at("models") == json::array({"a", "b", "c"}));
    CHECK(r.at("cached") == true);
    CHECK(r.at("new") == json::array({"c"}));
    // 看过了：点消掉。
    http::post_llm_models_seen(json{{"base_url", url + "/"}});
    CHECK(got({"a", "b", "c"}).at("new") == json::array());
}

TEST_CASE("改一家：换地址密钥跟着搬、默认跟着换；清单上那一家只换密钥") {
    changji::test::ScopedUserConfigDir iso("provider-update");
    const std::string a = "http://10.0.0.8:8000/v1";
    const std::string b = "http://10.0.0.9:9000/v1";
    const std::string id =
        http::post_llm_provider_add(json{{"name", "公司那台"}, {"base_url", a}, {"api_key", "k-a"}})
            .body.at("id");
    http::remember_models(a, json{{"models", {"m1"}}});

    auto s = base_settings();
    s.llm.base_url = a;
    s.llm.model = "m1";
    const RuntimeAs rt{s};

    // 只改名字：地址、密钥都不动。
    auto r = http::post_llm_provider_update(json{{"id", id}, {"name", "新名字"}});
    CHECK(r.body.at("name") == "新名字");
    CHECK(r.body.at("moved") == false);
    CHECK(config::read_api_key_for(a) == "k-a");

    // 换地址、没给新密钥：旧的那把搬过去，旧地址名下的删掉。
    r = http::post_llm_provider_update(json{{"id", id}, {"base_url", b + "/"}});
    CHECK(r.body.at("base_url") == b);
    CHECK(r.body.at("moved") == true);
    CHECK(config::read_api_key_for(b) == "k-a");
    CHECK_FALSE(config::has_own_api_key(a));
    // 记下的模型单子跟着搬。
    CHECK(fs::exists(http::models_cache_path(b)));
    CHECK_FALSE(fs::exists(http::models_cache_path(a)));
    // 默认原来指着旧地址：跟着换过去，模型名不变。
    CHECK(config::runtime().snapshot().llm.base_url == b);
    CHECK(config::runtime().snapshot().llm.model == "m1");

    // 换密钥。
    http::post_llm_provider_update(json{{"id", id}, {"api_key", "k-b"}});
    CHECK(config::read_api_key_for(b) == "k-b");

    // 撞上清单上那一家的地址：不许。
    CHECK_THROWS_AS(http::post_llm_provider_update(
                        json{{"id", id}, {"base_url", "http://127.0.0.1:11434/v1"}}),
                    http::ApiError);

    // 清单上那一家：只换密钥，换地址不许；原来没按过「加」的，改完算加过。
    http::post_llm_provider_update(json{{"id", "deepseek"}, {"api_key", "k-ds"}});
    CHECK(config::read_api_key_for("https://api.deepseek.com/v1") == "k-ds");
    CHECK_THROWS_AS(http::post_llm_provider_update(
                        json{{"id", "deepseek"}, {"base_url", "https://x.example.com/v1"}}),
                    http::ApiError);
    const json all = http::get_llm_providers().body.at("providers");
    bool ds_added = false;
    for (const auto& p : all) {
        if (p.at("id") == "deepseek") ds_added = p.at("added").get<bool>();
    }
    CHECK(ds_added);

    CHECK_THROWS_AS(http::post_llm_provider_update(json{{"id", "没有这一家"}}), http::ApiError);
}

TEST_CASE("自己加一家：摆上单子，密钥按地址存，拿掉连密钥一起删") {
    changji::test::ScopedUserConfigDir iso("custom-providers");
    const auto listed = [](const std::string& id) -> json {
        // 先接住：range-for 直接挂在临时对象的成员上，临时对象当场就没了。
        const json all = http::get_llm_providers().body.at("providers");
        for (const auto& p : all) {
            if (p.at("id") == id) return p;
        }
        return json();
    };

    // 清单上那几家：没填密钥、没加过，不进单子。
    CHECK(listed("ollama").at("added") == false);

    const auto r = http::post_llm_provider_add(json{{"name", "公司那台"},
                                                    {"base_url", "http://10.0.0.8:8000/v1/"},
                                                    {"api_key", "k-1"}});
    const std::string id = r.body.at("id");
    const json mine = listed(id);
    REQUIRE(mine.is_object());
    CHECK(mine.at("name") == "公司那台");
    CHECK(mine.at("base_url") == "http://10.0.0.8:8000/v1");
    CHECK(mine.at("custom") == true);
    CHECK(mine.at("added") == true);
    CHECK(mine.at("key_set") == true);
    CHECK(config::read_api_key_for("http://10.0.0.8:8000/v1") == "k-1");
    // 名单文件里一个密钥字都没有。
    {
        std::ifstream in(http::custom_providers_path());
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("k-1") == std::string::npos);
    }

    // 清单上那一家按地址加：不另起一条，记成加过了（本机那几家不要密钥）。
    http::post_llm_provider_add(json{{"base_url", "http://127.0.0.1:11434/v1"}});
    CHECK(listed("ollama").at("added") == true);
    int ollamas = 0;
    const json all = http::get_llm_providers().body.at("providers");
    for (const auto& p : all) {
        if (p.at("base_url") == "http://127.0.0.1:11434/v1") ++ollamas;
    }
    CHECK(ollamas == 1);

    http::post_llm_provider_remove(json{{"id", id}});
    CHECK(listed(id).is_null());
    CHECK_FALSE(config::has_own_api_key("http://10.0.0.8:8000/v1"));
    http::post_llm_provider_remove(json{{"id", "ollama"}});
    CHECK(listed("ollama").at("added") == false);

    CHECK_THROWS_AS(http::post_llm_provider_add(json{{"name", "x"}, {"base_url", "10.0.0.8"}}),
                    http::ApiError);
    CHECK_THROWS_AS(http::post_llm_provider_add(json{{"base_url", "https://nameless.example/v1"}}),
                    http::ApiError);
}

TEST_CASE("本地那一家列的是盘上真有的编剧模型") {
    CHECK(llm::is_llm_weights("llm/我自己放的.gguf", 12));
    CHECK(llm::is_llm_weights("llm/X.GGUF", 12));
    // 清单上编剧那一组的文件，放在哪一层都认——但大小要对上（下到一半的不算）。
    CHECK_FALSE(llm::is_llm_weights("Qwen3-8B-Q4_K_M.gguf", 3));
    // 出片、出图那几组的编码器也是 gguf，挑进来当编剧会写出一堆乱码。
    CHECK_FALSE(llm::is_llm_weights("video/umt5-xxl-encoder-Q8_0.gguf", 12));
    CHECK_FALSE(llm::is_llm_weights("Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf", 4683072384ULL));
    CHECK_FALSE(llm::is_llm_weights("llm/mmproj-x.gguf", 12));
    CHECK_FALSE(llm::is_llm_weights("llm/readme.txt", 12));

    const fs::path dir = fresh_dir("local");
    fs::create_directories(dir / "llm");
    fs::create_directories(dir / "video");
    for (const char* n : {"llm/Mine-7B.gguf", "video/enc.gguf", "llm/notes.txt"}) {
        std::ofstream out(dir / n, std::ios::binary);
        out << "假的";
    }
    config::Settings s;
    s.models.dir = paths::to_utf8(dir);
    const auto r = http::get_llm_local(s);
    REQUIRE(r.body.at("models").size() == 1);
    CHECK(r.body.at("models").at(0).at("name") == "Mine-7B");
    CHECK(r.body.at("models").at(0).at("file") == "llm/Mine-7B.gguf");
    CHECK(r.body.contains("available"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
