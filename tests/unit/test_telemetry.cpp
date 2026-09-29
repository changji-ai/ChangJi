// 匿名使用统计（src/telemetry/）：只发白名单上的、关着一个字节都不发、心跳 / 事件 / 汇总各在该发的时候发。
//
// 服务器那头的白名单在 server/src/appstats.js（用例 server/test/appstats.test.js）；这边钉的是
// 「发出去的长什么样」：顶层只有那几栏、计数只有那几个键、闸门代号不像代号的不发、
// 远程大模型只说是哪一家不说地址。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/telemetry.hpp"

namespace tel = changji::telemetry;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/// 发出去的每一趟：地址 + 解开的 JSON。
struct Wire {
    std::vector<std::pair<std::string, json>> sent;
    int status = 200;
    std::vector<json> of(const std::string& path) const {
        std::vector<json> out;
        for (const auto& [u, j] : sent)
            if (u.size() >= path.size() && u.compare(u.size() - path.size(), path.size(), path) == 0) out.push_back(j);
        return out;
    }
};

struct Rig {
    fs::path dir;
    Wire wire;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    bool configured = true;
    std::map<std::string, std::string> env;

    Rig() {
        dir = fs::temp_directory_path() / ("cj-tel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        tel::reset_for_tests();
        tel::Options o;
        o.dir = dir;
        o.version = "v2.5.9";
        o.gpu_build = "cuda";
        o.tick = std::chrono::milliseconds(0);   // 不起线程
        o.io.now = [this] { return now; };
        o.io.configured = [this] { return configured; };
        o.io.endpoint = [] { return std::string("https://t.example/"); };
        o.io.env = [this](const char* k) {
            const auto it = env.find(k);
            return it == env.end() ? std::string() : it->second;
        };
        o.io.post = [this](const std::string& url, const std::string& body) -> std::pair<int, std::string> {
            wire.sent.emplace_back(url, json::parse(body));
            return {wire.status, R"({"ok":true,"beat_s":300})"};
        };
        o.io.hw = [] { return json{{"gpu_vendor", "nvidia"}, {"gpu_name", "NVIDIA GeForce RTX 4090"}, {"vram_gb", 24.0}, {"ram_gb", 64.0}}; };
        o.io.setup = [] { return json{{"llm", "remote"}, {"llm_host", "zhipu"}, {"video_model", "minimax_h3_fl2va-Q4_K_M.gguf"}, {"peers", 1}}; };
        tel::start(std::move(o));
    }
    ~Rig() {
        tel::reset_for_tests();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void wait(std::chrono::seconds s) { now += s; }
};

const std::set<std::string> kIdentity{"v", "id", "ver", "ch", "shell", "os", "arch", "gpu", "lang"};

std::set<std::string> keys(const json& j) {
    std::set<std::string> k;
    for (const auto& [name, _] : j.items()) k.insert(name);
    return k;
}

}  // namespace

TEST_CASE("统计 · 版本号：正式版和 beta 算，本机编的不算") {
    CHECK(tel::release_version("v2.5.9"));
    CHECK(tel::release_version("v2.5.12-v2.5"));
    CHECK(tel::release_version("v10.0.123-feature_x"));
    CHECK_FALSE(tel::release_version("dev"));
    CHECK_FALSE(tel::release_version("v2.2-local-cuda"));
    CHECK_FALSE(tel::release_version("v2.5"));
    CHECK_FALSE(tel::release_version("2.5.9"));
    CHECK_FALSE(tel::release_version("v2.5.9x"));
}

TEST_CASE("统计 · 挡着的三种：CHANGJI_TELEMETRY=0、DO_NOT_TRACK、本机编的") {
    std::map<std::string, std::string> env;
    const auto get = [&env](const char* k) {
        const auto it = env.find(k);
        return it == env.end() ? std::string() : it->second;
    };
    CHECK(tel::blocked(get, "v2.5.9") == tel::Block::none);
    CHECK(tel::blocked(get, "dev") == tel::Block::local_build);
    env["CHANGJI_TELEMETRY_URL"] = "http://127.0.0.1:18092";   // 对着测试网关验：本机编的也放行
    CHECK(tel::blocked(get, "dev") == tel::Block::none);
    env["DO_NOT_TRACK"] = "1";
    CHECK(tel::blocked(get, "v2.5.9") == tel::Block::dnt);
    env["DO_NOT_TRACK"] = "0";
    CHECK(tel::blocked(get, "v2.5.9") == tel::Block::none);
    env["CHANGJI_TELEMETRY"] = "0";
    CHECK(tel::blocked(get, "v2.5.9") == tel::Block::env);
    env["CHANGJI_TELEMETRY"] = "OFF";
    CHECK(tel::blocked(get, "v2.5.9") == tel::Block::env);
}

TEST_CASE("统计 · 远程大模型只说是哪一家，不说地址") {
    CHECK(tel::host_class("https://open.bigmodel.cn/api/paas/v4") == "zhipu");
    CHECK(tel::host_class("https://api.z.ai/api/paas/v4") == "zhipu");
    CHECK(tel::host_class("https://api.openai.com/v1") == "openai");
    CHECK(tel::host_class("https://api.deepseek.com") == "deepseek");
    CHECK(tel::host_class("https://dashscope.aliyuncs.com/compatible-mode/v1") == "qwen");
    CHECK(tel::host_class("https://openrouter.ai/api/v1") == "openrouter");
    CHECK(tel::host_class("https://changji.xyz/v1") == "changji");
    CHECK(tel::host_class("http://localhost:11434/v1") == "ollama");
    CHECK(tel::host_class("http://127.0.0.1:1234/v1") == "lmstudio");
    CHECK(tel::host_class("http://192.168.20.51:8000/v1") == "lan");
    CHECK(tel::host_class("http://172.20.0.3:8000/v1") == "lan");
    CHECK(tel::host_class("http://gpu-box.local:8000/v1") == "lan");
    CHECK(tel::host_class("https://llm.my-company.example/v1") == "other");   // 自己公司的地址不往外说
    CHECK(tel::host_class("https://evil.com/?openai.com") == "other");       // 按主机名认，不按整串里有没有
    CHECK(tel::host_class("https://xyz.ai/v1") == "other");                  // 按域名整段比：不是 z.ai
    CHECK(tel::host_class("http://192.168.1.9:12345/v1") == "lan");          // 端口整个比：不是 :1234
    CHECK(tel::host_class("") == "");
}

TEST_CASE("统计 · 显卡：厂商、型号去掉本地化的附注和怪字符") {
    CHECK(tel::gpu_vendor_of("NVIDIA GeForce RTX 4070") == "nvidia");
    CHECK(tel::gpu_vendor_of("NVIDIA L20") == "nvidia");
    CHECK(tel::gpu_vendor_of("AMD Radeon RX 7900 XTX") == "amd");
    CHECK(tel::gpu_vendor_of("Intel(R) Arc(TM) A770 Graphics") == "intel");
    CHECK(tel::gpu_vendor_of("Apple M3 Max") == "apple");
    CHECK(tel::gpu_vendor_of("") == "none");
    CHECK(tel::clean_gpu_name("Apple M3 Max\xEF\xBC\x88\xE7\xBB\x9F\xE4\xB8\x80\xE5\x86\x85\xE5\xAD\x98\xEF\xBC\x89") == "Apple M3 Max");
    CHECK(tel::clean_gpu_name("Intel(R) Arc(TM) A770") == "Intel");
    CHECK(tel::clean_gpu_name("NVIDIA  GeForce <b>RTX</b>  4090 ") == "NVIDIA GeForce b RTX /b 4090");
    CHECK(tel::clean_gpu_name(std::string(200, 'A')).size() == 80);
}

TEST_CASE("统计 · 界面语言、模型文件名、在干什么") {
    CHECK(tel::lang_code("") == "zh");
    CHECK(tel::lang_code("zh_CN") == "zh");
    CHECK(tel::lang_code("zh_TW") == "zh_TW");
    CHECK(tel::lang_code("en_US.UTF-8") == "en");
    CHECK(tel::lang_code("pt-BR") == "pt_BR");
    CHECK(tel::lang_code("ja") == "ja");
    CHECK(tel::model_name("D:/models/video/minimax_h3_fl2va-Q4_K_M.gguf") == "minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(tel::model_name("C:\\Users\\林晚\\模型\\我的模型.gguf").empty());   // 人自己起的名字（和路径里的人名）不发
    CHECK(tel::model_name("").empty());
    CHECK(tel::state_of({}) == "idle");
    CHECK(tel::state_of({{"write_one", 1}}) == "writing");
    CHECK(tel::state_of({{"write_one", 1}, {"video", 2}}) == "rendering");
    CHECK(tel::state_of({{"video", 0}}) == "idle");
}

TEST_CASE("统计 · 头一拍：生一个安装编号、报「新装上」、发心跳；三种都只带那几栏") {
    Rig r;
    tel::tick_once();
    std::ifstream in(r.dir / "install_id");
    std::string id;
    std::getline(in, id);
    CHECK(id.size() == 32);

    const auto beats = r.wire.of("/t/v1/beat");
    REQUIRE(beats.size() == 1);
    CHECK(r.wire.sent.front().first == "https://t.example/t/v1/beat");   // 末尾的斜杠去掉了
    std::set<std::string> want = kIdentity;
    want.insert("state");
    CHECK(keys(beats[0]) == want);
    CHECK(beats[0]["id"] == id);
    CHECK(beats[0]["ver"] == "v2.5.9");
    CHECK(beats[0]["ch"] == "release");
    CHECK(beats[0]["gpu"] == "cuda");
    CHECK(beats[0]["state"] == "idle");

    const auto evs = r.wire.of("/t/v1/event");
    REQUIRE(evs.size() == 1);
    CHECK(evs[0]["kind"] == "installed");

    // 第二次起来不再报「新装上」
    r.wire.sent.clear();
    r.wait(std::chrono::seconds(10));
    tel::tick_once();
    CHECK(r.wire.of("/t/v1/event").empty());
    CHECK(r.wire.of("/t/v1/beat").empty());   // 五分钟一条，没到
}

TEST_CASE("统计 · 开始出片补一条心跳；出完一镜、写完一章各报一件事") {
    Rig r;
    tel::tick_once();
    r.wire.sent.clear();

    tel::task_begun("video");
    r.wait(std::chrono::seconds(31));
    tel::tick_once();
    auto beats = r.wire.of("/t/v1/beat");
    REQUIRE(beats.size() == 1);
    CHECK(beats[0]["state"] == "rendering");

    tel::task_ended("video", "shots", true, tel::End::ok, 60000);
    tel::task_ended("write_one", "story", false, tel::End::ok, 0);
    r.wire.sent.clear();
    r.wait(std::chrono::seconds(31));
    tel::tick_once();
    const auto evs = r.wire.of("/t/v1/event");
    REQUIRE(evs.size() == 2);
    CHECK(evs[0]["kind"] == "shot_rendered");
    CHECK(evs[1]["kind"] == "chapter_written");
    beats = r.wire.of("/t/v1/beat");
    REQUIRE(beats.size() == 1);   // 活干完了，状态变回空闲
    CHECK(beats[0]["state"] == "idle");
}

TEST_CASE("统计 · 汇总：计数、每一步成败耗时、闸门只收白名单上的；接成片带分钟") {
    Rig r;
    tel::tick_once();
    tel::task_begun("video");
    tel::task_ended("video", "shots", true, tel::End::ok, 60000);
    tel::task_begun("video");
    tel::task_ended("video", "shots", true, tel::End::fail, 5000);
    tel::task_ended("image", "assets", false, tel::End::cancel, 0);
    tel::task_ended("image", "assets", true, tel::End::ok, 8000);
    tel::task_ended("prompt_leak", "", true, tel::End::ok, 1);   // 表外的种类扔掉
    tel::count("project_new");
    tel::count("film_joined");
    tel::count("film_minutes", 12.34);
    tel::count("story_title", 1);                                // 表外的键扔掉
    tel::gate("too_short");
    tel::gate("too_short");
    tel::gate("整章几乎没有对白");                                // 不像代号的（那句原话）不发
    tel::gate("Bad Code");
    tel::event("film_joined", 12.34);

    r.wire.sent.clear();
    r.wait(std::chrono::minutes(6));
    tel::tick_once();
    const auto reps = r.wire.of("/t/v1/report");
    REQUIRE(reps.size() == 1);
    const json& rep = reps[0];
    std::set<std::string> want = kIdentity;
    want.insert({"hw", "setup", "days"});
    CHECK(keys(rep) == want);
    CHECK(rep["setup"]["llm_host"] == "zhipu");
    REQUIRE(rep["days"].size() == 1);
    const json& d = rep["days"][0];
    CHECK(keys(d["c"]) == std::set<std::string>{"project_new", "film_joined", "film_minutes", "shots_rendered", "refs_drawn"});
    CHECK(d["c"]["film_minutes"].get<double>() == doctest::Approx(12.3));
    CHECK(d["t"]["video"]["ok"] == 1);
    CHECK(d["t"]["video"]["fail"] == 1);
    CHECK(d["t"]["video"]["ms"] == 60000);   // 只算成了的那一件的耗时
    CHECK(d["t"]["image"]["cancel"] == 1);
    CHECK_FALSE(d["t"].contains("prompt_leak"));
    CHECK(d["g"] == json{{"too_short", 2}});
    const auto evs = r.wire.of("/t/v1/event");
    bool film = false;
    for (const auto& e : evs)
        if (e["kind"] == "film_joined") film = e["minutes"].get<double>() == doctest::Approx(12.3);
    CHECK(film);
}

TEST_CASE("统计 · 关着：一个字节都不发，也不在本机记账") {
    Rig r;
    r.configured = false;
    tel::tick_once();
    tel::count("project_new");
    tel::task_ended("video", "shots", false, tel::End::ok, 1);
    r.wait(std::chrono::hours(25));
    tel::tick_once();
    CHECK(r.wire.sent.empty());
    CHECK_FALSE(fs::exists(r.dir / "install_id"));   // 编号都不生

    // 再打开：关着那阵子的不补
    r.configured = true;
    tel::tick_once();
    r.wait(std::chrono::minutes(6));
    tel::tick_once();
    const auto reps = r.wire.of("/t/v1/report");
    REQUIRE(reps.size() == 1);
    CHECK(reps[0]["days"].empty());
}

TEST_CASE("统计 · DO_NOT_TRACK 设了就不发，设置里开着也一样") {
    Rig r;
    r.env["DO_NOT_TRACK"] = "1";
    tel::tick_once();
    CHECK(r.wire.sent.empty());
    CHECK_FALSE(tel::enabled());
    const json st = tel::status();
    CHECK(st["enabled"] == false);
    CHECK(st["configured"] == true);
    CHECK(st["blocked"] == "dnt");
}

TEST_CASE("统计 · 汇总发出去之后前几天的删掉，今天的留着接着记；没发出去一小时后再发") {
    Rig r;
    tel::tick_once();
    const auto t0 = r.now;
    r.now = t0 - std::chrono::hours(48);   // 前天记了一笔
    tel::count("project_new");
    r.now = t0;
    tel::count("oneclick");

    r.wire.status = 503;   // 头一回没发出去
    r.wait(std::chrono::minutes(6));
    tel::tick_once();
    CHECK(r.wire.of("/t/v1/report").size() == 1);
    r.wire.sent.clear();
    r.wait(std::chrono::minutes(30));
    tel::tick_once();
    CHECK(r.wire.of("/t/v1/report").empty());   // 还没到一小时

    r.wire.status = 200;
    r.wait(std::chrono::minutes(31));
    tel::tick_once();
    auto reps = r.wire.of("/t/v1/report");
    REQUIRE(reps.size() == 1);
    CHECK(reps[0]["days"].size() == 2);

    r.wire.sent.clear();
    r.wait(std::chrono::hours(24));
    tel::tick_once();
    reps = r.wire.of("/t/v1/report");
    REQUIRE(reps.size() == 1);
    CHECK(reps[0]["days"].size() <= 1);   // 前天那一格发过就删了
}

TEST_CASE("统计 · 设置页看原文：不生编号、不发，只拼出来给人看") {
    Rig r;
    const json st = tel::status();
    CHECK(st["enabled"] == true);
    CHECK(st["endpoint"] == "https://t.example");
    CHECK(st["preview"]["beat"]["state"] == "idle");
    CHECK(st["preview"]["event"]["kind"] == "shot_rendered");
    CHECK(st["preview"]["report"]["hw"]["gpu_name"] == "NVIDIA GeForce RTX 4090");
    CHECK(r.wire.sent.empty());
    CHECK_FALSE(fs::exists(r.dir / "install_id"));
}
