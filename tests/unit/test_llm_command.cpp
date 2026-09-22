// 命令行后端：把本机装着的 claude / codex 当大模型用。
//
// 用户 2026-09-18：「增加对 claudecode 和 codex 命令版的支持」。
//
// **这里一个真模型都不叫。** 拿 cat / findstr 当替身：提示词从标准输入进去、
// 原样印出来，那正是这条路要保证的"一段字进、一段字出"。真去叫 claude 的话
// 这套测试要花钱、要联网、还要那台机器登录过——三个条件哪个不满足都会红，
// 而红的原因和代码无关。
//
// 盯四件事，每一件都是"错了不会当场报错"的那一类：
//
//   **提示词真的送进去了**。走的是标准输入不是命令行参数——几万字塞不进
//   参数（Windows 32 KB 封顶）。这条错了的表现是模型在写一个空题目。
//
//   **schema 要贴在提示词后面**。远端那条路本来就是这么发的
//   （schema_as_prompt），两边共用同一个函数；这边漏了的话，模型在"随便
//   吐个 JSON"，而调用方按字段去解析它。
//
//   **「没装」和「装了但报错」要分开说**。两句话把人指向完全不同的方向。
//
//   **跑通但一个字没吐，不能当成空回答**。下游按 schema 解析空串会报一句
//   和这件事毫无关系的解析错。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"

#include "scoped_env.hpp"

using namespace changji;

namespace {

/// 原样回显标准输入的那个命令。
config::LLMConfig echo_cfg() {
    config::LLMConfig c;
    c.backend = "command";
#ifdef _WIN32
    c.command = "findstr.exe";
    c.command_args = {"^"};  // 匹配每一行 = 原样回显
#else
    c.command = "cat";
    c.command_args = {};
#endif
    c.command_timeout_s = 30.0;
    return c;
}

/// 把 `llm_log` 那棵树里的 `index.jsonl` 全读出来。
/// **项目子目录名不写死**：这几条跑在没有活、没有项目的线程上。
std::vector<nlohmann::json> read_log_index(const std::filesystem::path& root) {
    std::vector<nlohmann::json> rows;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().filename() != "index.jsonl") {
            continue;
        }
        std::ifstream f(it->path(), std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) rows.push_back(nlohmann::json::parse(line));
        }
    }
    return rows;
}

/// 那棵树里每个文件的内容接成一大串。问的是"这个字有没有落到盘上"。
std::string slurp_log_tree(const std::filesystem::path& root) {
    std::string all;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        all += ss.str();
        all += "\n";
    }
    return all;
}

}  // namespace

TEST_CASE("命令行后端：提示词从标准输入进去，印出来的就是回答") {
    llm::CommandClient cli(echo_cfg());
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "changji_prompt_marker 这一章写什么";
    const std::string out = cli.complete(req, tok);
    CHECK(out.find("changji_prompt_marker") != std::string::npos);
    CHECK(out.find("这一章写什么") != std::string::npos);
}

TEST_CASE("命令行后端：几万字的提示词也要整份送到") {
    // 真实的提示词就是这个量级。参数那条路在 Windows 上会被 32 KB 顶回来，
    // 而标准输入没有这个限制——这条盯着别有人"顺手"改回去用参数传。
    llm::CommandClient cli(echo_cfg());
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "开头marker\n";
    while (req.prompt.size() < 120000) req.prompt += "正文一行。\n";
    req.prompt += "结尾marker";
    const std::string out = cli.complete(req, tok);
    CHECK(out.find("开头marker") != std::string::npos);
    CHECK_MESSAGE(out.find("结尾marker") != std::string::npos,
                  "只送到了前面一截——多半是被参数长度或者管道缓冲截了");
}

TEST_CASE("命令行后端：schema 贴在提示词后面，和远端那条一个样") {
    llm::CommandClient cli(echo_cfg());
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "拆这一章的镜头";
    req.schema = nlohmann::ordered_json{
        {"type", "object"},
        {"properties", {{"changji_field_marker", {{"type", "string"}}}}}};
    const std::string out = cli.complete(req, tok);
    CHECK_MESSAGE(out.find("changji_field_marker") != std::string::npos,
                  "schema 没跟着提示词一起发出去");
    // 和远端那条共用同一个函数，不是各写一份
    CHECK(out.find(llm::schema_as_prompt(req.prompt, req.schema)) != std::string::npos);
}

TEST_CASE("命令行后端：没装那个程序，要说「找不到」而不是别的") {
    config::LLMConfig c = echo_cfg();
    c.command = "changji_这个程序不存在_98765";
    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "随便";
    CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                         doctest::Contains("找不到"), llm::LlmError);
}

TEST_CASE("命令行后端：没填 command 要当场说清，不是跑到一半才失败") {
    config::LLMConfig c;
    c.backend = "command";
    c.command.clear();
    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "随便";
    CHECK_THROWS_AS(cli.complete(req, tok), llm::LlmError);
}

TEST_CASE("命令行后端：退出码非 0 时，把它自己那句话带出来") {
    // 实测最常见的一条是「Failed to authenticate: OAuth session expired」，
    // 那正是用户需要知道的。换成"命令行后端失败"等于把理由吃掉。
    config::LLMConfig c = echo_cfg();
#ifdef _WIN32
    c.command = "findstr.exe";
    c.command_args = {"/c:这个一定匹配不上的字符串_zzz"};  // 匹配不到就退 1
#else
    c.command = "grep";
    c.command_args = {"这个一定匹配不上的字符串_zzz"};
#endif
    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "别的内容";
    CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                         doctest::Contains("退出码"), llm::LlmError);
}

TEST_CASE("默认参数表：claude 认得出，而且不许出现 --bare") {
    const auto a = llm::default_command_args("claude");
    REQUIRE(!a.empty());
    const std::string joined = [&] {
        std::string s;
        for (const auto& x : a) s += x + " ";
        return s;
    }();
    CHECK(joined.find("-p") != std::string::npos);
    // **--bare 会把 OAuth 和钥匙串一概关掉**，而走这条路的人图的正是订阅
    // 登录。实测加了就是「Not logged in · Please run /login」。
    CHECK_MESSAGE(joined.find("--bare") == std::string::npos,
                  "--bare 会让订阅登录失效");
    // 它默认是个带 Bash 和文件编辑的 agent，而我们只要一段字
    CHECK_MESSAGE(joined.find("--restricted") != std::string::npos,
                  "没把会跑命令、改文件的那些工具去掉");
}

TEST_CASE("默认参数表：带路径、带扩展名、大小写都要认得出来") {
    // 配置里填的可能是 `claude`，也可能是绝对路径或者 Windows 上的 .cmd
    CHECK(!llm::default_command_args("/Users/x/.local/bin/claude").empty());
    CHECK(!llm::default_command_args("C:\\tools\\Claude.CMD").empty());
    CHECK(!llm::default_command_args("codex").empty());
    // 认不出的不猜：交给用户自己在 command_args 里写
    CHECK(llm::default_command_args("my-own-llm").empty());
}

TEST_CASE("接上提示词日志之后，命令行这条的行为一个字没变") {
    // 同 test_llm_client.cpp 里那条：钉的**不是日志写对了没有**，是
    // 「接上记录器之后这个出入口对外还是原来那个样子」。
    //
    // 命令行这条尤其要钉 `proc::run` 的那几句翻译（找不到 / 超时 / 退出码
    // 非零）：它们全是给用户看的原话，而记录器的 catch 里那句必须是**裸
    // `throw;`**——`throw e;` 会把 LlmError 切片成 std::exception，
    // 调用方 `catch (const LlmError&)` 就整个接不住了。
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cj_llm_log_command_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    // 把落盘路径挪到临时目录，**一个字节都不往用户真正的数据目录写**。
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR", paths::to_utf8(root));

    SUBCASE("开关关着：回显的还是那段字，而且一次盘都不碰") {
        config::LLMConfig c = echo_cfg();
        c.call_log = false;
        llm::CommandClient cli(c);
        pipeline::CancelToken tok;
        llm::Request req;
        req.prompt = "写一章剧本";
        CHECK(cli.complete(req, tok) == "写一章剧本");
        CHECK_MESSAGE(!std::filesystem::exists(root),
                      "关着的时候连目录都不该建");
    }

    SUBCASE("开关开着：回显的还是那段字，schema 照旧贴在后面") {
        config::LLMConfig c = echo_cfg();
        c.call_log = true;
        llm::CommandClient cli(c);
        pipeline::CancelToken tok;
        llm::Request req;
        req.prompt = "写一章剧本";
        req.schema = nlohmann::ordered_json{{"type", "object"}};
        const std::string out = cli.complete(req, tok);
        CHECK(out.rfind("写一章剧本", 0) == 0);
        // 和远端那条共用同一个函数，记日志不许在中间插一手
        CHECK(out.find(llm::schema_as_prompt(req.prompt, req.schema)) !=
              std::string::npos);
    }

    SUBCASE("开关开着：没填 command 抛的还是那句，且照旧是 LlmError") {
        // 记录器是**摆在这句抛之前**的（没配好也是一次真实的失败），
        // 所以这一句最容易被接线弄坏。
        config::LLMConfig c;
        c.backend = "command";
        c.command = "";
        c.call_log = true;
        llm::CommandClient cli(c);
        pipeline::CancelToken tok;
        llm::Request req;
        req.prompt = "写一章剧本";
        CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                             doctest::Contains("llm.command"), llm::LlmError);
    }

    SUBCASE("开关开着：找不到那个程序，说的还是「不在 PATH 上」") {
        config::LLMConfig c = echo_cfg();
        c.call_log = true;
        c.command = "changji_一定不存在的命令_zzz";
        c.command_args = {};
        llm::CommandClient cli(c);
        pipeline::CancelToken tok;
        llm::Request req;
        req.prompt = "写一章剧本";
        CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                             doctest::Contains("找不到"), llm::LlmError);
    }

    std::filesystem::remove_all(root, ec);
}


TEST_CASE("命令行后端：参数里的密钥不许落进日志") {
    // **这一条破了是不可逆的**：日志是拿来拷给别人一起看的（docs/提示词日志.md
    // 上写着「拷这个目录给别人之前先想一下」），拷的人不会先逐份读一遍。
    //
    // 失败场景（真的）：`[llm].command_args` 在设置页上是自由文本、配置接口
    // 一个字都不过滤。填成 `["-p","--api-key","d3b4f0a1e2.9Kx2mQ7v"]` 跑一次，
    // 那行 `endpoint` 就是整把密钥。`redact_secrets` 拦不住它——它认的是
    // `Bearer xxx`、`sk-` 打头的长串、JSON 里那几个键的值，而命令行参数是
    // **两个独立的 argv**，智谱的密钥又形如 `d3b4f0a1e2.9Kx2mQ7v`，
    // 三种形状一种都不沾。
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cj_llm_log_secret_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR", paths::to_utf8(root));

    // 拿一个**肯定不存在**的命令：这一次必然砸在"找不到"上，不碰这台机器上
    // 任何真程序，而账还是照记（失败那一半正是最想研究的）。
    config::LLMConfig c;
    c.backend = "command";
    c.command = "changji_一定不存在的命令_zzz";
    c.command_args = {"-p", "--api-key", "d3b4f0a1e2.9Kx2mQ7v",
                      "--auth-token=9Kx2mQ7vSECRET", "--restricted"};
    c.call_log = true;
    c.command_timeout_s = 30.0;

    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "写一章剧本";
    CHECK_THROWS_AS(cli.complete(req, tok), llm::LlmError);

    const auto rows = read_log_index(root);
    REQUIRE(rows.size() == 1);
    const std::string endpoint = rows[0].at("endpoint").get<std::string>();
    CAPTURE(endpoint);
    CHECK_MESSAGE(endpoint.find("d3b4f0a1e2.9Kx2mQ7v") == std::string::npos,
                  "跟在 --api-key 后面的那个 argv 得整个换成 ***");
    CHECK_MESSAGE(endpoint.find("9Kx2mQ7vSECRET") == std::string::npos,
                  "--auth-token=VALUE 这种写法，等号后面也得换掉");
    // 抹的是值，不是开关名：研究时要认的「这次带的哪几个开关」照样看得见。
    CHECK(endpoint.find("--api-key") != std::string::npos);
    CHECK(endpoint.find("--auth-token=") != std::string::npos);
    CHECK(endpoint.find("***") != std::string::npos);
    // 后面那个参数不许被连累
    CHECK(endpoint.find("--restricted") != std::string::npos);
    // 整棵树都不许有——那句翻给用户的话也会落进 error.txt
    CHECK(slurp_log_tree(root).find("d3b4f0a1e2.9Kx2mQ7v") == std::string::npos);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("命令行后端：只有开关才能点名下一个，值里带 token 不许连累后面那个开关") {
    // **判的是"这个 argv 像不像密钥"而不是"它是不是开关"时，抹的就是错的那个。**
    // `--append-system-prompt "回答要 token 精简"` 里那句话含 token，
    // 于是紧跟的 `--restricted` 被整个换成 `***`——而 endpoint 那一栏的用处
    // 正是「这次带了哪几个开关」，`--restricted` 恰好是关掉改文件那些工具的
    // 那一条，日志上看不出带没带。同族的还有 `--max-tokens 8192`（值被抹）。
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cj_llm_log_flagpos_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const test::ScopedEnv log_dir("CHANGJI_LLM_LOG_DIR", paths::to_utf8(root));

    config::LLMConfig c;
    c.backend = "command";
    c.command = "changji_一定不存在的命令_zzz";
    c.command_args = {"-p", "--append-system-prompt", "回答要 token 精简",
                      "--restricted", "--max-tokens", "8192"};
    c.call_log = true;
    c.command_timeout_s = 30.0;

    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "写一章剧本";
    CHECK_THROWS_AS(cli.complete(req, tok), llm::LlmError);

    const auto rows = read_log_index(root);
    REQUIRE(rows.size() == 1);
    const std::string endpoint = rows[0].at("endpoint").get<std::string>();
    CAPTURE(endpoint);
    // 那句提示词是值不是开关，点名不了下一个。
    CHECK(endpoint.find("--restricted") != std::string::npos);
    // `--max-tokens` 是开关而且带 token，它后面那个数才该被抹。
    CHECK(endpoint.find("--max-tokens ***") != std::string::npos);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("运行期换后端：下一次调用就走新的那条，不用重启") {
    // **2026-09-18 实测撞到的**：`POST /api/connections` 把 `llm_backend` 从
    // remote 改成 command，回执的 `changed` 里有它、`config.toml` 也真写进去了，
    // 紧接着那次调用落进提示词日志的还是 `"backend":"remote"`、
    // `"endpoint":"https://open.bigmodel.cn/..."`，**重启引擎才变**。
    //
    // 根子是"选哪条后端"曾经在工厂里 `if` 一次，而调用点把结果存进函数局部
    // `static`（http/server.cpp 的 script_client / batch_client），那个 `if`
    // 就跟着冻在进程第一次叫模型那一刻。地址、模型、密钥都走 ConfigProvider
    // 每次现读，所以**偏偏只有"走哪条后端"不生效**——而界面上已经显示
    // "已应用"了，是最难查的那一种错。
    //
    // 这条用例**不重造 client**（正是 static 那个形状），只换配置。
    auto cfg = std::make_shared<config::LLMConfig>();
    cfg->call_log = false;   // 不往这台机器真实的数据目录写
    REQUIRE(cfg->backend == "remote");

    int remote_hits = 0;
    auto post = [&remote_hits](const std::string&, const std::string&,
                               const std::map<std::string, std::string>&, double) {
        ++remote_hits;
        llm::HttpResponse r;
        r.status = 200;
        r.body = R"({"choices":[{"message":{"content":"远端答的"}}]})";
        return r;
    };

    const auto client =
        llm::make_client(llm::ConfigProvider([cfg] { return *cfg; }), post);
    REQUIRE(client != nullptr);

    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "写一章剧本";

    // 一、起步走远端
    CHECK(client->complete(req, tok) == "远端答的");
    CHECK(remote_hits == 1);

    // 二、换成命令行。**同一个 client 对象**，一次都没重造。
    config::LLMConfig to_command = echo_cfg();
    to_command.call_log = false;
    *cfg = to_command;
    // 那个命令原样回显标准输入，所以拿回来的就是提示词本身——
    // 这一下同时证明了"真走了命令行"和"提示词真喂进去了"。
    CHECK(client->complete(req, tok) == "写一章剧本");
    CHECK_MESSAGE(remote_hits == 1, "换成命令行之后，远端那条一次都不该再被碰");

    // 三、换回远端也得立刻生效——**两个方向都要验**，不然"一换就永远走命令行"
    // 同样能让上面两条过。
    config::LLMConfig back;
    back.call_log = false;
    *cfg = back;
    CHECK(client->complete(req, tok) == "远端答的");
    CHECK(remote_hits == 2);
}

TEST_CASE("认不出的 backend 按远端跑，不当场把流水线堵死") {
    // 配置里把 backend 写成错别字时，宁可按默认那条跑通，也不要抛一句
    // 「不认识的后端」——那会让所有生成一起挂，而人看到的是一句和自己刚
    // 改的那一项对不上的话。填错了由 Settings::validate 那头去说。
    auto cfg = std::make_shared<config::LLMConfig>();
    cfg->call_log = false;
    cfg->backend = "remotee";   // 手滑

    auto post = [](const std::string&, const std::string&,
                   const std::map<std::string, std::string>&, double) {
        llm::HttpResponse r;
        r.status = 200;
        r.body = R"({"choices":[{"message":{"content":"远端答的"}}]})";
        return r;
    };

    const auto client =
        llm::make_client(llm::ConfigProvider([cfg] { return *cfg; }), post);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "写一章剧本";
    CHECK(client->complete(req, tok) == "远端答的");
}
