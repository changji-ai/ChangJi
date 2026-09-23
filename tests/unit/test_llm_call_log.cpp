// 提示词调用日志：记录器本体。
//
// 用户 2026-09-18：「将所有的提交提示词给大模型的地方和获取结果的地方都以日志
// 保存下来放到程序统计目录里，我要研究。」
//
// **判据只有一条：看着日志能还原一次调用。** 所以这儿钉的不是"函数返回值对不
// 对"，而是**捞出来的那几份文件够不够还原**：模型真正收到的是哪段字、它吐回来
// 什么、花了多久、哪一步用的哪个模型和温度。
//
// 五件"错了不会当场报错"的事，每一件都对应下面一条用例：
//
//   **写盘出问题不许影响生成**。磁盘满、没权限、目录建不了——记不下就算了。
//   反过来的话，加一个日志功能就把本来会成功的那一次生成弄砸了。
//
//   **密钥一个字节都不落盘**。这一条破了是不可逆的：日志会被拷给别人看、
//   会贴进工单，而拷的人不会先逐份读一遍。
//
//   **`index.jsonl` 永不删**。剪旧文件那段只删正文。删掉正文之后那些行还在，
//   只是点不开原文；连行一起删的话，这个功能就只剩最近那几天能用。
//
//   **同一毫秒内的 id 不能撞**。批量那几条本来就是并发的，撞了就是两次调用
//   共用一份文件——而账上还是两行，谁也看不出哪一行的正文被盖掉了。
//
//   **并发追加不许丢行、不许串行**。串了的那一行 jq 解不动，统计那头看到的
//   是"少了几次调用"，没人会往日志自己身上想。
//
//   **抹密钥不许误伤**。`disk-usage-exceeded` 被抹成 `di***` 的话，"什么提示
//   词换来一个 400"就没线索了——而那正是最想研究的一类。
//
//   **剪旧按落盘顺序**。按 id（= 开工时刻）排的话，跑了 166 秒的那次慢活第一
//   个被删，几十条几秒钟的短记录全留着：**最贵、最想看的那一次第一个被扔**。
//
//   **同一次调用不许入两遍账**。`total` 从此永久虚高一份，上限还没到就开始剪。
//
// ⚠️ **每一条都把 `CallLogOptions::root` 指到临时目录**，一个字节都不往这台
// 机器上真实的数据目录写。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/call_log.hpp"
#include "llm/client.hpp"
#include "pipeline/activity.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 一个自己收拾干净的临时根目录。
struct TmpRoot {
    fs::path dir;
    explicit TmpRoot(const std::string& tag)
        : dir(fs::temp_directory_path() / ("changji_calllog_" + tag)) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~TmpRoot() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    TmpRoot(const TmpRoot&) = delete;
    TmpRoot& operator=(const TmpRoot&) = delete;
};

llm::CallLogOptions opts(const fs::path& root,
                         std::uintmax_t max_bytes = 1024ull * 1024 * 1024) {
    llm::CallLogOptions o;
    o.enabled = true;
    o.root = root;
    o.max_bytes = max_bytes;
    return o;
}

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<nlohmann::json> read_index(const fs::path& dir) {
    std::vector<nlohmann::json> rows;
    std::ifstream f(dir / "index.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) rows.push_back(nlohmann::json::parse(line));
    }
    return rows;
}

std::vector<nlohmann::json> read_gates(const fs::path& dir) {
    std::vector<nlohmann::json> rows;
    std::ifstream f(dir / "gates.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) rows.push_back(nlohmann::json::parse(line));
    }
    return rows;
}

/// 整个目录里所有文件的内容接成一大串。给"密钥不落盘"那条用。
std::string slurp_tree(const fs::path& root) {
    std::string all;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            all += paths::to_utf8(it->path().filename());
            all += "\n";
            all += read_all(it->path());
            all += "\n";
        }
    }
    return all;
}

/// 没有项目时落在哪个子目录。
const char* kNoProject = "_无项目";

}  // namespace

TEST_CASE("一次调用记完之后，看着日志能还原它") {
    TmpRoot root("restore");
    const std::string prompt = "把第三章写出来。视角是唐海，天在下雨。";

    llm::Request req;
    req.schema_name = "chapter";

    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        id = log.id();
        log.set_endpoint("https://open.bigmodel.cn/api/paas/v4");
        log.set_model("glm-5.3", 0.5, "low");
        log.set_prompt(prompt);
        log.note_response(200,
                          R"({"usage":{"prompt_tokens":9130,)"
                          R"("completion_tokens":11402,"total_tokens":20532}})");
        log.set_finish_reason("stop");
        log.set_reply("雨是从后半夜开始下的。");
        log.set_thinking("先想清楚这一章的转折在哪儿。");
    }
    CHECK_FALSE(id.empty());

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    const auto rows = read_index(dir);
    REQUIRE(rows.size() == 1);
    const auto& r = rows[0];

    // **这几列永远都在**（没有就是空串 / 0 / false）。缺一列的话统计那头
    // 按键取会拿到 null，而 pandas 会把整列变成 object 类型。
    for (const char* k :
         {"id", "at", "backend", "kind", "schema_name", "model", "temperature",
          "reasoning_effort", "endpoint", "project", "episode_id", "task",
          "stream_id", "prompt_chars", "reply_chars", "thinking_chars", "ms",
          "finish_reason", "ok", "http_status", "error"}) {
        CHECK_MESSAGE(r.contains(k), "索引里少了这一列：", k);
    }

    CHECK(r["id"] == id);
    CHECK(r["backend"] == "remote");
    CHECK(r["kind"] == "complete");
    CHECK(r["schema_name"] == "chapter");
    CHECK(r["model"] == "glm-5.3");
    CHECK(r["temperature"] == 0.5);
    CHECK(r["reasoning_effort"] == "low");
    CHECK(r["endpoint"] == "https://open.bigmodel.cn/api/paas/v4");
    CHECK(r["finish_reason"] == "stop");
    CHECK(r["ok"] == true);
    CHECK(r["http_status"] == 200);
    CHECK(r["error"] == "");
    // 没有活、没有项目的那条线程上跑的：**空串，不是缺键**。
    CHECK(r["project"] == "");
    CHECK(r["task"] == "");
    CHECK(r["stream_id"] == "");
    // 时间戳带时区偏移，不然隔天看日志不知道是哪个时区的十七点。
    CHECK(r["at"].get<std::string>().size() ==
          std::string("2026-09-18T17:42:33.518+08:00").size());

    // **数的是「字」不是字节。** 中文一个字三字节，按字节算会让所有长度判断
    // 偏大三倍，和 token 数、和 CLAUDE.md 里那张 schema 字数表全对不上。
    CHECK(r["prompt_chars"] == text::utf8_len(prompt));
    CHECK(r["prompt_chars"].get<std::size_t>() < prompt.size());

    // usage 抽得出来（整段那条才有）。
    REQUIRE(r.contains("usage"));
    CHECK(r["usage"]["prompt_tokens"] == 9130);
    CHECK(r["usage"]["total_tokens"] == 20532);
    // 流式才有的那两个键这儿不该出现。
    CHECK_FALSE(r.contains("ms_to_first"));
    CHECK_FALSE(r.contains("tools_count"));

    // **提示词逐字节相等**：想比两次提示词差在哪儿就直接 diff，多一个 BOM、
    // 多一个尾换行都会让每一行都显示成改过。
    CHECK(read_all(dir / (id + ".prompt.txt")) == prompt);
    CHECK(read_all(dir / (id + ".reply.txt")) == "雨是从后半夜开始下的。");
    CHECK(read_all(dir / (id + ".thinking.txt")) == "先想清楚这一章的转折在哪儿。");
}

TEST_CASE("prompt.txt 存的是贴完 schema 的那一份，不是 req.prompt") {
    // 这一条是整件事的核心：模型收到的是 `schema_as_prompt` 拼出来的那段字，
    // 而 schema 是提示词里最大的一块（分镜那份 5600+ 字符）。存 req.prompt 的话
    // 日志里看不到真正的约束，"研究提示词"就无从谈起。
    //
    // ⚠️ `build_payload` 在 client.cpp 的匿名 namespace 里，用例够不着；这儿照
    // 它那一句原样拼。`schema_as_prompt` 是公开的，两边是同一个函数。
    TmpRoot root("schema");

    nlohmann::ordered_json schema = nlohmann::ordered_json::object();
    schema["type"] = "object";
    schema["properties"] = nlohmann::ordered_json::object();
    schema["properties"]["chapter_title"] = nlohmann::ordered_json::object();
    schema["properties"]["chapter_title"]["type"] = "string";

    llm::Request req;
    req.prompt = "写第三章";
    req.schema = schema;
    req.schema_name = "chapter";

    nlohmann::ordered_json msg = nlohmann::ordered_json::object();
    msg["role"] = "user";
    msg["content"] = llm::schema_as_prompt(req.prompt, req.schema);
    nlohmann::ordered_json payload = nlohmann::ordered_json::object();
    payload["model"] = "glm-5.3";
    payload["messages"] = nlohmann::ordered_json::array();
    payload["messages"].push_back(msg);

    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        id = log.id();
        log.set_prompt_from_payload(payload);
    }

    const std::string on_disk =
        read_all(root.dir / paths::from_utf8(kNoProject) / (id + ".prompt.txt"));
    CHECK(on_disk.find("写第三章") != std::string::npos);
    CHECK(on_disk.find("chapter_title") != std::string::npos);
    // 一条 user 消息的那一支**不加抬头**，出来的就是模型收到的原文。
    CHECK(on_disk.find("=== user ===") == std::string::npos);
}

TEST_CASE("render_prompt：一条 user 原样出，多条带抬头") {
    // 原样出那一支是为了能 diff：加了抬头的话，两次同一步的提示词比起来
    // 第一行永远在，而真正的差别淹在里面。
    nlohmann::ordered_json one = nlohmann::ordered_json::array();
    nlohmann::ordered_json m = nlohmann::ordered_json::object();
    m["role"] = "user";
    m["content"] = "把第三章写出来。";
    one.push_back(m);
    CHECK(llm::render_prompt(one) == "把第三章写出来。");

    // chat 那条：整段来回渲染成人能读的一份。
    nlohmann::ordered_json many = nlohmann::ordered_json::array();
    nlohmann::ordered_json sys = nlohmann::ordered_json::object();
    sys["role"] = "system";
    sys["content"] = "你是编剧。";
    many.push_back(sys);

    nlohmann::ordered_json usr = nlohmann::ordered_json::object();
    usr["role"] = "user";
    usr["content"] = "从网上找找今天有什么热点";
    many.push_back(usr);

    nlohmann::ordered_json call = nlohmann::ordered_json::object();
    call["id"] = "call_1";
    call["type"] = "function";
    call["function"] = nlohmann::ordered_json::object();
    call["function"]["name"] = "hot_topics";
    call["function"]["arguments"] = R"({"n":10})";
    nlohmann::ordered_json asst = nlohmann::ordered_json::object();
    asst["role"] = "assistant";
    asst["content"] = nullptr;   // 带 tool_calls 的那条 content 可以是 null
    asst["tool_calls"] = nlohmann::ordered_json::array();
    asst["tool_calls"].push_back(call);
    many.push_back(asst);

    nlohmann::ordered_json tool = nlohmann::ordered_json::object();
    tool["role"] = "tool";
    tool["tool_call_id"] = "call_1";
    tool["content"] = R"({"items":[]})";
    many.push_back(tool);

    const std::string out = llm::render_prompt(many);
    CHECK(out.find("=== system ===\n你是编剧。") != std::string::npos);
    CHECK(out.find("=== user ===\n从网上找找今天有什么热点") != std::string::npos);
    CHECK(out.find("=== assistant (tool_calls: hot_topics) ===") != std::string::npos);
    CHECK(out.find("=== tool (call_1) ===") != std::string::npos);
    // 工具参数单独一行，不混进正文——不然"模型这一轮说了什么"就不准了。
    CHECK(out.find("→ hot_topics {\"n\":10}") != std::string::npos);
    // content 是 null 的那条不许写成字面的 "null"。
    CHECK(out.find("null") == std::string::npos);
    // 块与块之间空一行。
    CHECK(out.find("\n\n=== user ===") != std::string::npos);
}

TEST_CASE("砸了的那一次照样记，而且记得更全") {
    // **失败那一半正是最想研究的**：什么提示词换来一个 400、什么提示词让
    // schema 本地校验没过。
    TmpRoot root("fail");
    const std::string body = R"({"error":{"message":"model not found"}})";
    const std::string said = "大模型返回格式异常：说不出是哪一步";

    llm::Request req;
    req.schema_name = "storyboard";

    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        id = log.id();
        log.set_prompt("拆第三章的分镜");
        log.note_response(400, body);
        log.fail(said, 400);
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    const auto rows = read_index(dir);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["ok"] == false);
    CHECK(rows[0]["http_status"] == 400);
    // **翻给用户的那句原话**：研究时要能一眼看出当时用户看到的是什么。
    CHECK(rows[0]["error"] == said);

    const std::string err = read_all(dir / (id + ".error.txt"));
    CHECK(err.rfind(said, 0) == 0);                     // 第一段是那句话
    CHECK(err.find("---- 对面原始回包（HTTP 400）----") != std::string::npos);
    CHECK(err.find("model not found") != std::string::npos);   // 第二段是原始回包
}

TEST_CASE("对面回 200、我们这头校验没过：ok=false 而 http_status=200，正文照留") {
    // 这一类是研究最想要的：回包好好的，是 schema 本地校验没过。
    // 那几千字正文**必须留着**——不然最想看的那一次反而是空的。
    TmpRoot root("localfail");
    llm::Request req;
    req.schema_name = "storyboard";

    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        id = log.id();
        log.set_prompt("拆第三章的分镜");
        log.note_response(200, R"({"choices":[{"message":{"content":"…"}}]})");
        log.set_reply("{\"shots\":[]}");   // extract_content 抽出来的那一段
        // schema_validate 抛的 LlmError 没有状态码，所以传 0。
        log.fail("大模型输出不符合 storyboard Schema：shots 是空的", 0);
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    const auto rows = read_index(dir);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["ok"] == false);
    // **传 0 不许把已经记下的 200 盖掉。**
    CHECK(rows[0]["http_status"] == 200);
    CHECK(read_all(dir / (id + ".reply.txt")) == "{\"shots\":[]}");
}

TEST_CASE("密钥一个字节都不落盘") {
    // 第一道门是"整个 headers 都不记，只记 base_url"，密钥本来到不了这儿。
    // 这一条钉的是第二道：真有人（或者某个网关的错误回显）把密钥递进来了，
    // 落盘之前也要抹掉。**破了是不可逆的**——日志会被原样拷给别人看。
    TmpRoot root("secret");
    const std::string key = "sk-changji-test-0123456789abcdef";

    llm::Request req;
    {
        llm::CallLog log{"remote", "chat", req, opts(root.dir)};
        log.set_endpoint("https://open.bigmodel.cn/api/paas/v4 Bearer " + key);
        log.set_prompt("随便写点什么");
        log.note_response(401,
                          R"({"error":"invalid key","authorization":"Bearer )" + key +
                              R"("})");
        log.fail("密钥不对：Bearer " + key, 401);
    }

    const std::string all = slurp_tree(root.dir);
    CHECK(all.find(key) == std::string::npos);
    // 抹掉的只是值，别的字要留着——不然看不出当时到底哪儿不对了。
    CHECK(all.find("invalid key") != std::string::npos);
    CHECK(all.find("open.bigmodel.cn") != std::string::npos);
    // **`Bearer` 这个词留着。** 它是认证方案名，不是密钥的一部分；
    // 留着才看得出"这儿本来有个 Authorization 值"。
    // 这一条 2026-09-18 之前钉反了（断言连 `Bearer` 一起没），而那么写的代价
    // 是散文也跟着缺字：对面回的 "the bearer of this token is invalid"
    // 会变成 "the *** this token is invalid"——`of` 没了。
    CHECK(all.find("Bearer ***") != std::string::npos);
}

TEST_CASE("抹密钥不吃掉句子：bearer 当普通词用的时候后面那个词要留着") {
    // `bearer ` 连词带值一起吞的话，对面回的错误说明会缺字，而那句话正是
    // 读日志的人唯一的线索。
    TmpRoot root("bearer_prose");
    llm::Request req;
    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        id = log.id();
        log.set_prompt("拆分镜");
        log.fail("the bearer of this token is not allowed", 403);
    }
    const std::string err =
        read_all(root.dir / paths::from_utf8(kNoProject) / (id + ".error.txt"));
    // `of` 还在——被吞掉的只有紧跟 `bearer ` 的那一串。
    CHECK(err.find("of this token is not allowed") != std::string::npos);
}

TEST_CASE("写盘出问题不影响生成：建不了目录、关着的时候，都不抛") {
    // **本来会成功的那一次，不许因为记日志而失败。**
    TmpRoot root("nowrite");
    const fs::path blocker = root.dir / "im_a_file";
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "x";
    }

    llm::Request req;
    req.schema_name = "chapter";

    // root 是一个**已经存在的普通文件**，底下开不出任何目录。
    // 不用 CHECK_NOTHROW 包：抛出来的话 doctest 会把这条判成"意外的异常"，
    // 判据一样，而 CHECK_NOTHROW 是单参数宏，花括号里的逗号会把它拆散。
    {
        llm::CallLog log{"remote", "complete", req, opts(blocker)};
        log.set_endpoint("https://example.invalid");
        log.set_prompt("写第三章");
        log.note_response(200, "{}");
        log.set_reply("一段正文");
    }
    // 那个文件还是文件：没被当成目录动过，也没被清掉。
    CHECK(fs::is_regular_file(blocker));

    // 关着的时候整个不动手：不建目录、不取 id。
    TmpRoot off("disabled");
    llm::CallLogOptions no = opts(off.dir);
    no.enabled = false;
    {
        llm::CallLog log{"remote", "complete", req, no};
        CHECK(log.id().empty());
        log.set_prompt("写第三章");
    }
    CHECK_FALSE(fs::exists(off.dir / paths::from_utf8(kNoProject)));
}

TEST_CASE("中文项目名照原样建目录，活的身份跟着落进索引") {
    // 项目目录允许是「山里的 信号」这种（中文 + 空格），路径转换一律走
    // paths::to_utf8 / from_utf8——MSVC 上 path.string() 会走 ANSI 代码页，
    // 中文路径当场抛 std::system_error（见 util/paths.hpp 那两段）。
    TmpRoot root("chinese");
    const std::string project = "/tmp/电影/山里的 信号";

    llm::Request req;
    req.schema_name = "chapter";
    {
        pipeline::Activity act{"llm", project, "ep03", "写正文 · 第 3 章"};
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        log.set_prompt("写第三章");
    }

    const fs::path dir = root.dir / paths::from_utf8("山里的 信号");
    REQUIRE(fs::exists(dir / "index.jsonl"));
    const auto rows = read_index(dir);
    REQUIRE(rows.size() == 1);
    // 目录名只够分文件夹，**索引里记的是绝对路径**——研究时要能认出是哪一部。
    CHECK(rows[0]["project"] == project);
    CHECK(rows[0]["episode_id"] == "ep03");
    CHECK(rows[0]["task"] == "写正文 · 第 3 章");
}

TEST_CASE("剪旧文件：正文没了，index.jsonl 每一行都还在") {
    // **这是最容易被下一个人"顺手优化"掉的一条。** 一行几百字节，而它是统计
    // 的唯一依据；连行一起删的话，这个功能就只剩最近那几天能用。
    TmpRoot root("prune");
    const std::string filler(1000, 'x');

    llm::Request req;
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        llm::CallLog log{"remote", "complete", req, opts(root.dir, 3000)};
        ids.push_back(log.id());
        log.set_prompt(filler);   // 1000 字节
        log.set_reply(filler);    // 1000 字节，一次调用共 2000
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    CHECK(read_index(dir).size() == 3);   // **三行一行不少**
    // 上限 3000，一次 2000：第二次写完就超了，最旧那次的正文被剪掉。
    CHECK_FALSE(fs::exists(dir / (ids[0] + ".prompt.txt")));
    CHECK_FALSE(fs::exists(dir / (ids[0] + ".reply.txt")));   // 一次剪一个 id 的全部
    CHECK(fs::exists(dir / (ids[2] + ".prompt.txt")));
}

TEST_CASE("同一毫秒里连开一千个，id 互不相同") {
    // 撞了就是两次调用共用一份文件，而账上还是两行——**谁也看不出哪一行的
    // 正文被盖掉了**。时间戳精确到毫秒也不够，靠的是末尾那个进程内自增序号。
    TmpRoot root("ids");
    llm::Request req;

    std::set<std::string> seen;
    std::vector<std::unique_ptr<llm::CallLog>> logs;
    logs.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        logs.push_back(std::make_unique<llm::CallLog>("remote", "complete", req,
                                                      opts(root.dir)));
        seen.insert(logs.back()->id());
    }
    CHECK(seen.size() == 1000);
}

TEST_CASE("几条线程一起记：不丢行，也不串行") {
    // 四个出入口可能在不同线程上同时记（批量那几条本来就是并发的）。
    // 串了的那一行 jq 解不动，而统计那头看到的是"少了几次调用"。
    TmpRoot root("threads");
    constexpr int kThreads = 8;
    constexpr int kEach = 25;

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&root] {
            llm::Request req;
            req.schema_name = "chapter";
            for (int i = 0; i < kEach; ++i) {
                llm::CallLog log{"remote", "complete", req, opts(root.dir)};
                log.set_endpoint("https://open.bigmodel.cn/api/paas/v4");
                log.set_prompt("写一章");
                log.set_reply("一段正文");
            }
        });
    }
    for (auto& t : ts) t.join();

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    // read_index 里那句 json::parse 本身就是"没串行"的判据：串了解不动，
    // 会当场抛出来。
    const auto rows = read_index(dir);
    CHECK(rows.size() == static_cast<std::size_t>(kThreads * kEach));
    std::set<std::string> ids;
    for (const auto& r : rows) ids.insert(r["id"].get<std::string>());
    CHECK(ids.size() == static_cast<std::size_t>(kThreads * kEach));
}

TEST_CASE("extract_usage：有就抽出来，没有和不是 JSON 都回 null 且不抛") {
    const auto ok = llm::extract_usage(
        R"({"choices":[],"usage":{"prompt_tokens":9130,"completion_tokens":11402,)"
        R"("total_tokens":20532}})");
    REQUIRE(ok.is_object());
    CHECK(ok["prompt_tokens"] == 9130);
    CHECK(ok["completion_tokens"] == 11402);
    CHECK(ok["total_tokens"] == 20532);

    CHECK(llm::extract_usage(R"({"choices":[]})").is_null());
    // 命令行那条后端的 stdout 根本不是 JSON，`note_response` 照样会调它。
    CHECK_NOTHROW(llm::extract_usage("这不是 JSON，是模型直接印出来的一段话"));
    CHECK(llm::extract_usage("这不是 JSON，是模型直接印出来的一段话").is_null());
}

TEST_CASE("流式那条：ms_to_first 只有真收到过第一段正文时才有") {
    TmpRoot root("stream");
    llm::Request req;
    req.schema_name = "chapter";

    {
        llm::CallLog log{"remote", "complete_stream", req, opts(root.dir)};
        log.set_prompt("写第三章");
        log.append_thinking("先想");       // 收流回调里只往内存里接
        log.append_thinking("再想");
        log.mark_first_token();
        log.mark_first_token();            // 第二次起是空操作
        log.set_reply("雨是从后半夜开始下的。");
    }
    {
        llm::CallLog log{"remote", "complete_stream", req, opts(root.dir)};
        log.set_prompt("写第四章");
        log.set_reply("");                 // 一个字都没吐
    }

    const auto rows = read_index(root.dir / paths::from_utf8(kNoProject));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].contains("ms_to_first"));
    CHECK(rows[0]["thinking_chars"] == text::utf8_len("先想再想"));
    // 流式那条**拿不到 usage**（发的 payload 里没有 stream_options），
    // 照实：这些行不该有这个键。
    CHECK_FALSE(rows[0].contains("usage"));
    CHECK_FALSE(rows[1].contains("ms_to_first"));
}

TEST_CASE("chat 那条：工具表另存一份，tools_count 记个数") {
    TmpRoot root("tools");
    llm::Request req;

    nlohmann::ordered_json tools = nlohmann::ordered_json::array();
    for (const char* name : {"hot_topics", "read_page"}) {
        nlohmann::ordered_json t = nlohmann::ordered_json::object();
        t["type"] = "function";
        t["function"] = nlohmann::ordered_json::object();
        t["function"]["name"] = name;
        tools.push_back(t);
    }

    std::string id;
    {
        llm::CallLog log{"remote", "chat", req, opts(root.dir)};
        id = log.id();
        log.set_prompt("从网上找找今天有什么热点");
        log.set_tools(tools);
        // tool_calls 回来那一次 content 常常是空串，reply_chars 就是 0——
        // **正常**，模型这一轮说的话在 tools_count 和下一轮的 prompt 里。
        log.set_reply("");
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    const auto rows = read_index(dir);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["tools_count"] == 2);
    CHECK(rows[0]["reply_chars"] == 0);
    CHECK(read_all(dir / (id + ".tools.json")).find("hot_topics") != std::string::npos);

    // 空工具表 = 不记，连键都不出现（chat 之外那三条根本不发工具）。
    TmpRoot bare("tools_empty");
    {
        llm::CallLog log{"remote", "chat", req, opts(bare.dir)};
        log.set_prompt("随便聊聊");
        log.set_tools(nlohmann::ordered_json::array());
    }
    const auto bare_rows = read_index(bare.dir / paths::from_utf8(kNoProject));
    REQUIRE(bare_rows.size() == 1);
    CHECK_FALSE(bare_rows[0].contains("tools_count"));
}

TEST_CASE("抹密钥只抹密钥：disk-usage-exceeded 一个字不动") {
    // `sk-` 不判词首边界的话，任何含 `sk-` 的词后面再跟够长度就整段没了：
    // 对面回的 `disk-usage-exceeded` 落进 error.txt 和索引的 `error` 栏时变成
    // `di***`，**而那段字正是研究「什么提示词换来一个 400」时唯一的线索**。
    // 同族的还有 `risk-control`、`task-management`。
    TmpRoot root("redact_word");
    const std::string key = "sk-changji-0123456789abcdef";
    const std::string body =
        R"({"error":{"code":"disk-usage-exceeded","hint":"risk-control",)"
        R"("detail":"task-management rejected, key )" + key + R"( 无效"}})";
    const std::string said = "对面回了 400：disk-usage-exceeded（密钥 " + key + "）";

    llm::Request req;
    req.schema_name = "storyboard";

    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        id = log.id();
        log.set_prompt("拆第三章的分镜");
        log.note_response(400, body);
        log.fail(said, 400);
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    const std::string err = read_all(dir / (id + ".error.txt"));
    // 该抹的抹了：整个目录里一个字节都找不着那把密钥。
    CHECK(slurp_tree(root.dir).find(key) == std::string::npos);
    // 不该抹的一个字没动——三个词都带 `sk-`，而它们都不在词首。
    CHECK(err.find("disk-usage-exceeded") != std::string::npos);
    CHECK(err.find("risk-control") != std::string::npos);
    CHECK(err.find("task-management") != std::string::npos);

    const auto rows = read_index(dir);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["error"].get<std::string>().find("disk-usage-exceeded") !=
          std::string::npos);
}

TEST_CASE("剪旧按落盘顺序排：先写完的先删，不是先开工的先删") {
    // id 的时间戳是**构造**那一刻的，落盘却在**析构**。按 id 排的话，一次流式
    // 拆分镜跑 166 秒（真实日志里那一行 `"ms":166842.4`），这 166 秒里批量那几条
    // 写完了几十条几秒钟的短记录，超上限时 `begin()` 正是这次长的自己——7 KB
    // 提示词加 13 KB 思考当场被删，那几十条短的全留着。
    // **最贵、最想看的那一次第一个被扔**，正好反了。
    TmpRoot root("prune_order");
    const std::string filler(1000, 'x');   // 一次调用两个文件，共 2000 字节
    llm::Request req;

    std::string slow_id, quick_id;
    {
        // 先开工的那条慢的，最后才落盘。
        auto slow = std::make_unique<llm::CallLog>("remote", "complete_stream", req,
                                                   opts(root.dir, 3000));
        slow_id = slow->id();
        slow->set_prompt(filler);
        slow->set_reply(filler);
        {
            // 后开工、先落盘的那条短的。
            llm::CallLog quick{"remote", "complete", req, opts(root.dir, 3000)};
            quick_id = quick.id();
            quick.set_prompt(filler);
            quick.set_reply(filler);
        }
        slow.reset();   // 两次共 4000 > 3000：就在这一下超上限
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    // 先落盘的那次被剪掉，**跑了半天的那次留着**。
    CHECK_FALSE(fs::exists(dir / (quick_id + ".prompt.txt")));
    CHECK_FALSE(fs::exists(dir / (quick_id + ".reply.txt")));   // 一次剪一个 id 的全部
    CHECK(fs::exists(dir / (slow_id + ".prompt.txt")));
    CHECK(fs::exists(dir / (slow_id + ".reply.txt")));
    CHECK(read_index(dir).size() == 2);   // 行照旧一行不少
}

TEST_CASE("本进程刚写下的那一次，扫描不许再算一遍") {
    // 正文文件在拿到剪枝那把锁**之前**就写完了，所以本进程第一次落盘时，
    // `scan_once_locked` 从磁盘上看见的正是自己刚写下的那几个文件。算两遍的话
    // 这一次的 2000 字节记成 4000，上限 3000 当场就"超了"——**刚写下的正文被
    // 自己剪掉**，而开关明明是开的、index.jsonl 还在长，症状和"日志坏了"对不上。
    // 多线程那头是同一个根子：B 写完文件还没拿到锁，A 扫了一圈把 B 记进账，
    // B 随后再记一遍自己，`total` 从此永久虚高一份。
    TmpRoot root("double_count");
    const std::string filler(1000, 'x');
    llm::Request req;

    std::string id;
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir, 3000)};
        id = log.id();
        log.set_prompt(filler);
        log.set_reply(filler);
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    CHECK(fs::exists(dir / (id + ".prompt.txt")));
    CHECK(fs::exists(dir / (id + ".reply.txt")));
}

TEST_CASE("关掉之后，出入口照样交进来，一个字都不许攒") {
    // 契约上写的是「不记就整个不动手」，而出入口那头不看开关。关掉之后
    // `append_thinking` 还在**收流回调里**往一个字符串上接——拆分镜那一步实测
    // 思考十几万字，整份在内存里攒着，最后被析构直接扔掉。
    TmpRoot off("disabled_setters");
    llm::CallLogOptions no = opts(off.dir);
    no.enabled = false;

    llm::Request req;
    req.schema_name = "storyboard";
    {
        llm::CallLog log{"remote", "complete_stream", req, no};
        for (int i = 0; i < 1000; ++i) log.append_thinking("这一镜该不该用 static。");
        log.set_thinking("整份思考");
        log.set_prompt(std::string(100000, 'x'));
        log.set_reply("一段正文");
        log.note_response(200, R"({"usage":{"prompt_tokens":9130}})");
        CHECK(log.thinking_chars() == 0);
    }
    CHECK_FALSE(fs::exists(off.dir / paths::from_utf8(kNoProject)));

    // 开着的时候照样攒——不然上面那条零值是白拿的。
    TmpRoot on("enabled_setters");
    {
        llm::CallLog log{"remote", "complete_stream", req, opts(on.dir)};
        log.append_thinking("先想");
        log.append_thinking("再想");
        CHECK(log.thinking_chars() == text::utf8_len("先想再想"));
    }
}

TEST_CASE("call_log_max_mb 大到离谱时，上限不许塌成 0") {
    // 设置页上手滑多按几个 0（或者 config.toml 里写 `call_log_max_mb = 1e30`）：
    // double → uintmax_t 装不下是 UB，实测常得 0，于是剪枝把刚写下的正文文件
    // **当场删光**，而开关明明是开的、index.jsonl 还在长。
    config::LLMConfig cfg;
    cfg.call_log = true;

    cfg.call_log_max_mb = 1024.0;
    CHECK(llm::call_log_options(cfg).max_bytes == 1024ull * 1024 * 1024);

    cfg.call_log_max_mb = 1e30;
    CHECK(llm::call_log_options(cfg).max_bytes > 1024ull * 1024 * 1024);

    // 0 和负数照旧是"一个字节都不留"。
    cfg.call_log_max_mb = 0.0;
    CHECK(llm::call_log_options(cfg).max_bytes == 0);
    cfg.call_log_max_mb = -5.0;
    CHECK(llm::call_log_options(cfg).max_bytes == 0);
}

TEST_CASE("两个项目里同一个 id：各算各的，剪的时候一份都不许漏") {
    // `g_seq` 每个进程从 1 起（call_log.cpp 那段警告写着），两个 changji 实例
    // 同一毫秒各开第一次调用，两个项目目录里就各有一份同名的 `<id>.*`。
    //
    // 账上只按 id 做键的话：folder 被后扫到的那个盖掉、字节数却是**两份相加**
    // ——剪到它时只删掉一个目录里的文件，`total` 按两份减，于是**另一个目录
    // 那几份从此不在账上、也不会再被扫到**（一趟进程只扫一次），磁盘上永久
    // 多出这一份且不收敛。所以键是 (id, 项目目录)。
    TmpRoot root("same_id_two_projects");
    const std::string id = "20260918-120000-000-0001";
    const std::string filler(1000, 'x');

    for (const char* folder : {"项目甲", "项目乙"}) {
        const fs::path dir = root.dir / paths::from_utf8(folder);
        fs::create_directories(dir);
        std::ofstream f(dir / paths::from_utf8(id + ".prompt.txt"), std::ios::binary);
        f << filler;
    }

    // 写一次，顺手触发那趟扫描；上限压到比盘上已有的还小，逼它剪。
    {
        llm::CallLog log{"remote", "complete", llm::Request{}, opts(root.dir, 500)};
        log.set_prompt("新的一次");
    }

    // **两个目录里那份都得没**。只按 id 记的话会剩下一个成为孤儿。
    CHECK_FALSE(fs::exists(root.dir / paths::from_utf8("项目甲") /
                           paths::from_utf8(id + ".prompt.txt")));
    CHECK_FALSE(fs::exists(root.dir / paths::from_utf8("项目乙") /
                           paths::from_utf8(id + ".prompt.txt")));
}

TEST_CASE("剪枝要认得全五种正文文件，加了新的一种而漏登记时这条会红") {
    // 后缀表（kBodySuffixes）是落盘、扫描、剪枝三处共用的那一张。加第六种文件
    // 时漏掉表里那一行的后果是：**那种文件永远剪不掉、也不计入上限**，而磁盘
    // 上限还以为自己管着。收成一处收不住"加的时候忘了登记"，所以让它会响
    // （CLAUDE.md 第八条那三档里的第二档）。
    const char* kAllSuffixes[] = {".prompt.txt", ".reply.txt", ".thinking.txt",
                                  ".tools.json", ".error.txt"};
    const std::string filler(400, 'x');
    // 一次调用把五种都写出来：有思考、有工具表、还失败了。
    const auto write_one = [&](llm::CallLog& log) {
        log.set_prompt(filler);
        log.set_reply(filler);
        log.set_thinking(filler);
        log.set_tools(nlohmann::ordered_json::array({{{"name", "hot_topics"}}}));
        log.fail("对面回了 400", 400);
    };

    // ---- 先证明这一次真把五种都写出来了 ----
    //
    // ⚠️ **少了这一段，下面那句就是绿得毫无意义的**：一种从没被写出来过的文件
    // 同样"不存在"，只数不存在的话，哪怕剪枝一个字都没干也照样过。
    {
        TmpRoot roomy("prune_all_kinds_write");
        std::string id;
        {
            llm::CallLog log{"remote", "chat", llm::Request{}, opts(roomy.dir)};
            id = log.id();
            write_one(log);
        }
        const fs::path dir = roomy.dir / paths::from_utf8(kNoProject);
        for (const char* suffix : kAllSuffixes) {
            CHECK_MESSAGE(fs::exists(dir / paths::from_utf8(id + suffix)),
                          ("这一次应该写出 " + std::string(suffix) +
                           "，没写出来的话下面那段等于没测").c_str());
        }
    }

    // ---- 再证明超上限时五种一起被剪掉 ----
    {
        TmpRoot tight("prune_all_kinds_prune");
        std::string id;
        {
            llm::CallLog log{"remote", "chat", llm::Request{}, opts(tight.dir, 100)};
            id = log.id();
            write_one(log);
        }
        const fs::path dir = tight.dir / paths::from_utf8(kNoProject);
        for (const char* suffix : kAllSuffixes) {
            CHECK_MESSAGE(!fs::exists(dir / paths::from_utf8(id + suffix)),
                          (std::string(suffix) +
                           " 没被剪掉——后缀表（kBodySuffixes）里是不是漏登记了？").c_str());
        }
        // index.jsonl 照旧不删。
        CHECK(read_index(dir).size() == 1);
    }
}

// ---------------------------------------------------------------- 闸门那一半

// 用户 2026-09-19：「记闸门打回理由，做」。
//
// 闸门（「整章几乎没有对白」「正文在复读」）是在调用回来**之后**才判的，
// 判完这一次调用的记录器早就析构了。所以裁决另记一行，靠 `call_id` 对回去。
//
// 盯的四件事，每一件错了都不会当场报错：
//
//   **对得回那一次调用**。对不上的话「什么提示词换来一次打回」整列作废，
//   而那正是这份日志最想答的问题。
//
//   **取走就清掉**。HTTP 的线程是池子里轮着用的，读一眼不清的话，一次没有
//   调用的判定会捡到上一个请求留下的 id——把闸门记在一次毫不相干的调用头上。
//
//   **通过的也记**。只记打回的话，通过率和「配成一对的训练数据」都要回头猜。
//
//   **记不下不许影响生成**。这一条比 `CallLog` 那边还要紧：一半的调用点在
//   `catch` 里，在那儿再抛一次就把真正那句错误话盖掉了——界面上显示的会是
//   "磁盘满"而不是"整章几乎没有对白"。

TEST_CASE("闸门打回：记下哪一次调用、哪条闸门、第几轮") {
    TmpRoot root("gate_reject");
    const std::string project =
        paths::to_utf8(root.dir / paths::from_utf8("山里的信号"));

    llm::Request req;
    req.schema_name = "chapter";

    std::string id;
    {
        pipeline::Activity act{"write", project, "ep03", "正在写 ch03"};
        llm::CallLog log{"remote", "complete_stream", req, opts(root.dir)};
        id = log.id();
        log.set_model("glm-4.7-flash", 0.85, "");
        log.set_prompt("把第三章写出来");
        log.set_reply(R"({"scenes":[]})");

        // **闸门是在这之后才判的。** 记录器这会儿还活着只是因为它在同一个
        // 作用域里；真实那条路上 `complete()` 早就返回、它早就析构了。
    }

    llm::GateVerdict v;
    v.call_id = llm::take_last_call_id();
    // 对得回那一次调用——这一条是整件事的地基。
    CHECK(v.call_id == id);
    CHECK_FALSE(v.call_id.empty());

    v.stage = "chapter";
    v.subject = "ch03";
    v.gate = "no_dialogue";
    v.reason = "整章几乎没有对白（65 段里只有 0 处）：对话被整段转述掉了。重试一次";
    v.attempt = 2;
    v.attempts = 3;
    v.strict = true;
    v.ok = false;
    {
        // 记的时候活还在，项目那三样要跟着记下来。
        pipeline::Activity act{"write", project, "ep03", "正在写 ch03"};
        llm::note_gate(v, opts(root.dir));
    }

    const fs::path dir = root.dir / paths::from_utf8("山里的信号");
    const auto rows = read_gates(dir);
    REQUIRE(rows.size() == 1);
    const auto& r = rows[0];

    // **这几列永远都在。** 缺一列的话统计那头按键取会拿到 null，
    // 而 pandas 会把整列变成 object 类型。
    for (const char* k : {"at", "call_id", "stage", "subject", "gate", "reason",
                          "attempt", "attempts", "strict", "ok", "project",
                          "episode_id", "task"}) {
        CHECK_MESSAGE(r.contains(k), "闸门账里少了这一列：", k);
    }

    CHECK(r["call_id"] == id);
    CHECK(r["stage"] == "chapter");
    CHECK(r["subject"] == "ch03");
    CHECK(r["gate"] == "no_dialogue");
    CHECK(r["reason"].get<std::string>().find("整章几乎没有对白") !=
          std::string::npos);
    CHECK(r["attempt"] == 2);
    CHECK(r["attempts"] == 3);
    CHECK(r["strict"] == true);
    CHECK(r["ok"] == false);
    CHECK(r["episode_id"] == "ep03");
    CHECK(r["task"] == "正在写 ch03");

    // **两份账 join 得起来。** 这一条才是"能还原一次打回"的全部依据：
    // 提示词在 index.jsonl 那一行指着的 .prompt.txt 里，打回的理由在这儿。
    const auto calls = read_index(dir);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0]["id"] == r["call_id"]);
    CHECK(fs::exists(dir / paths::from_utf8(id + ".prompt.txt")));
}

TEST_CASE("闸门：取走就清掉，没有调用的裁决一个字都不记") {
    TmpRoot root("gate_take");
    llm::Request req;
    req.schema_name = "chapter";
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        log.set_prompt("写第三章");
    }

    const std::string first = llm::take_last_call_id();
    CHECK_FALSE(first.empty());
    // **第二次是空的。** 不清的话，下一件没有调用的活（粘回来的稿子）会把
    // 闸门记在这一次的头上，而账上看不出来。
    CHECK(llm::take_last_call_id().empty());

    // 没有 id 的裁决整个不动手——连目录都不建。
    llm::GateVerdict v;
    v.call_id = llm::take_last_call_id();
    v.stage = "chapter";
    v.subject = "ch09";
    v.gate = "repetition";
    v.reason = "正文在复读";
    llm::note_gate(v, opts(root.dir));

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    CHECK_FALSE(fs::exists(dir / "gates.jsonl"));
}

TEST_CASE("闸门：通过的那一次也记") {
    // 只记打回的话，「这条闸门拦掉了多少」分母没有，而配一对训练数据
    // （被打回的那一版 + 最后过了的那一版）也无从配起。
    TmpRoot root("gate_pass");
    llm::Request req;
    req.schema_name = "chapter";
    {
        llm::CallLog log{"remote", "complete", req, opts(root.dir)};
        log.set_prompt("写第三章");
    }

    llm::GateVerdict v;
    v.call_id = llm::take_last_call_id();
    v.stage = "chapter";
    v.subject = "ch03";
    v.attempt = 3;
    v.attempts = 3;
    v.strict = false;   // 最后一轮软闸关掉
    v.ok = true;
    llm::note_gate(v, opts(root.dir));

    const auto rows = read_gates(root.dir / paths::from_utf8(kNoProject));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["ok"] == true);
    CHECK(rows[0]["gate"] == "");      // 过了就没有闸门代号
    CHECK(rows[0]["reason"] == "");
    CHECK(rows[0]["strict"] == false);
    CHECK(rows[0]["attempt"] == 3);
}

TEST_CASE("闸门：关掉记录就一个字节都不写") {
    TmpRoot root("gate_off");
    llm::Request req;
    req.schema_name = "chapter";
    {
        llm::CallLogOptions off = opts(root.dir);
        off.enabled = false;
        llm::CallLog log{"remote", "complete", req, off};
        log.set_prompt("写第三章");
        // 关着的时候连 id 都不取，闸门那头自然也对不上任何东西
        CHECK(log.id().empty());
    }

    llm::GateVerdict v;
    v.call_id = "20260919-010203-456-0001";   // 就算硬塞一个
    v.stage = "chapter";
    v.gate = "repetition";
    llm::CallLogOptions off = opts(root.dir);
    off.enabled = false;
    llm::note_gate(v, off);

    CHECK(slurp_tree(root.dir).empty());
}

TEST_CASE("闸门：记不下也不许抛") {
    // 一半的调用点在 catch 里。在那儿再抛一次，界面上显示的就会是"磁盘满"
    // 而不是"整章几乎没有对白"——真正的原因被记账这件事盖掉了。
    TmpRoot root("gate_boom");
    // 把"根"指到一个**普通文件**上：底下建不了任何目录。
    const fs::path blocked = root.dir / paths::from_utf8("不是目录");
    { std::ofstream f(blocked, std::ios::binary); f << "x"; }

    llm::CallLogOptions o = opts(blocked);
    llm::GateVerdict v;
    v.call_id = "20260919-010203-456-0001";
    v.stage = "chapter";
    v.subject = "ch03";
    v.gate = "repetition";
    v.reason = "正文在复读";
    CHECK_NOTHROW(llm::note_gate(v, o));
}

TEST_CASE("闸门：剪旧文件吃不到 gates.jsonl") {
    // 和 index.jsonl 同一条规矩：正文可以没，统计的依据不能没。
    // 「这半年里哪条闸门最费钱」正是要跨越那几天才答得出来的。
    TmpRoot root("gate_prune");
    llm::Request req;
    req.schema_name = "chapter";

    // 上限压到 1 字节：下一次落盘就会把之前的正文全剪掉。
    for (int i = 0; i < 3; ++i) {
        std::string id;
        {
            llm::CallLog log{"remote", "complete", req, opts(root.dir, 1)};
            id = log.id();
            log.set_prompt(std::string(2048, 'x'));
        }
        llm::GateVerdict v;
        v.call_id = llm::take_last_call_id();
        v.stage = "chapter";
        v.subject = "ch0" + std::to_string(i);
        v.gate = "repetition";
        v.reason = "正文在复读";
        llm::note_gate(v, opts(root.dir, 1));
    }

    const fs::path dir = root.dir / paths::from_utf8(kNoProject);
    // 正文被剪光了
    int bodies = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (paths::to_utf8(it->path().filename()).find(".prompt.txt") !=
            std::string::npos) {
            ++bodies;
        }
    }
    CHECK(bodies <= 1);
    // 三次裁决一行不少
    CHECK(read_gates(dir).size() == 3);
    CHECK(read_index(dir).size() == 3);
}
