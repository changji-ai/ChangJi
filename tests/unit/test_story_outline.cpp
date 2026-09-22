// 大纲生成与故事接口。
//
// 提示词这一份是新写的、没有 Python 对应物，所以**不做逐字节对拍**——
// 那样只会把提示词冻死。这里钉的是那几条不能松的规矩：
//
//   · schema 里不许出现任何外观字段（给了模型就会写一遍长相，
//     而那份长相和后面美术那一步出的必然对不上）
//   · 章节 id 由程序生成，不听模型的
//   · 关系的两端必须是登记过的人，指不到的直接丢
//   · /api/story/outline **只回草稿不落库**
//
// 大模型一律用 ReplayClient 桩。这台机器上不验 LLM 质量。

#include <doctest/doctest.h>

#include <fstream>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <vector>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/batch.hpp"
#include "http/episodes.hpp"
#include "http/planning.hpp"
#include "http/scripting.hpp"
#include "http/story_api.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/bible.hpp"
#include "stages/story_understand.hpp"
#include "stages/script_story.hpp"
#include "stages/chapter_write.hpp"
#include "stages/prompts.inc.hpp"
#include "stages/story_analyze.hpp"
#include "stages/story_import.hpp"
#include "stages/json_extract.hpp"
#include "stages/json_partial.hpp"
#include "stages/repetition.hpp"
#include "stages/story_outline.hpp"
#include "stages/story_plan.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
using namespace changji::models;
using changji::stages::build_outline_prompt;
using changji::stages::outline_schema;
using changji::stages::parse_outline;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 一份像样的模型返回。各用例在它上面改。
json good_outline() {
    return json{
        {"logline", "一把伞牵出五年前的事"},
        {"genre", "都市情感"},
        {"tone", "克制"},
        {"characters",
         json::array({
             json{{"name", "林晚"},
                  {"identity", "便利店夜班店员"},
                  {"want", "把那把伞还回去然后彻底了断"},
                  {"arc", "从躲着走到敢直视"}},
             json{{"name", "陈默"},
                  {"identity", "回城出差的建筑师"},
                  {"want", "问出当年她为什么不告而别"},
                  {"arc", "从质问到放下"}},
         })},
        {"relations",
         json::array({
             json{{"a", "林晚"},
                  {"b", "陈默"},
                  {"kind", "前任"},
                  {"tension", "谁都欠对方一句没说出口的道歉"}},
         })},
        {"locations",
         json::array({
             json{{"name", "便利店"},
                  {"what", "高架桥下那家 24 小时便利店"},
                  {"when", "深夜，冷白顶光"}},
         })},
        {"chapters",
         json::array({
             json{{"title", "雨夜重逢"},
                  {"summary", "他推门进来，伞还在手里。"},
                  {"hook", "她认出那把伞"},
                  {"characters", json::array({"林晚", "陈默"})},
                  {"locations", json::array({"便利店"})}},
             json{{"title", "五年前那把伞"},
                  {"summary", "回到五年前的那个雨夜。"},
                  {"hook", "他没有回头"},
                  {"characters", json::array({"林晚"})},
                  {"locations", json::array({"便利店"})}},
         })},
    };
}

fs::path fresh_project(const std::string& tag) {
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_故事接口_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    ProjectStore::create(root, "gushi", "故事接口测试");
    return root;
}

std::string p_str(const fs::path& p) { return paths::to_utf8(p); }

}  // namespace

TEST_CASE("schema 里不许有外观字段") {
    const auto& props = outline_schema().at("properties");
    const auto& ch = props.at("characters").at("items").at("properties");

    // 这几个是资产库那边的字段。出现在这里就意味着模型会在故事层写一遍长相，
    // 而 bible 那一步还会再写一遍，两份必然不一致——那正是要防的漂移。
    for (const char* banned : {"face", "body", "attire", "appearance", "look"}) {
        CHECK_MESSAGE(!ch.contains(banned),
                      "大纲 schema 里冒出了外观字段 " << banned);
    }
    CHECK(ch.contains("name"));
    CHECK(ch.contains("want"));

    // 章节必须有 hook：这一章停在哪就看它。没有钩子的话写剧本那一步只能
    // 退回「最后一场演完的那个落点」，收口就说不出为什么在那儿。
    const auto& chap = props.at("chapters").at("items");
    CHECK(chap.at("properties").contains("hook"));
    bool hook_required = false;
    for (const auto& r : chap.at("required")) {
        if (r == "hook") hook_required = true;
    }
    CHECK(hook_required);
}

TEST_CASE("地点和出场人要写进 required，不然 14B 一个都不给") {
    // **这一条钉的是一次实跑。** 2026-09-11 从零走一遍：出大纲给了 3 个
    // 人物、3 条关系（都在 required 里），而 `locations` 是空数组，每一章
    // 的 `characters` 和 `locations` 也全是空的——那几项当时都不在 required
    // 里。读一遍正文（analyze）出来的一模一样。不是模型读不懂，是语法采样
    // 允许它们缺席，它就缺席。
    //
    // 缺了不是"少一点信息"：设定页的「场景」那一格永远是空的，空景图无从
    // 谈起；再往后排分镜时，不知道这一章在哪儿发生、谁在场——而那正是
    // 第二步全部的输入。
    const auto top_level = [](const nlohmann::ordered_json& schema,
                              const char* who) {
        CAPTURE(who);
        bool top_locations = false;
        bool top_relations = false;
        for (const auto& r : schema.at("required")) {
            if (r == "locations") top_locations = true;
            if (r == "relations") top_relations = true;
        }
        CHECK_MESSAGE(top_locations, who << " 的 locations 不在 required 里");
        // relations 同理，而且它更险：**采用那一步会拿空数组盖掉已经有的
        // 那几条**。实跑里出大纲给了 3 条，读完正文变成 0 条。
        CHECK_MESSAGE(top_relations, who << " 的 relations 不在 required 里");
        // 空数组也是合法的数组，所以光 required 不够，还要有下限
        CHECK(schema.at("properties").at("locations").at("minItems") == 1);
    };
    top_level(outline_schema(), "大纲");
    top_level(changji::stages::analyze_schema(), "读故事");

    // 每章那两份名单**只要求「读故事」那一份**。
    //
    // 大纲那边加过，加完出一份大纲从三十几秒变成 278 秒，还截断在半截
    // JSON 上——语法一收紧，14B 就一路写到 token 上限也收不了口。而这两项
    // 本来就是照着正文读出来的准，大纲阶段凭空想的不准。
    const auto& chap =
        changji::stages::analyze_schema().at("properties").at("chapters").at("items");
    bool ch_chars = false;
    bool ch_locs = false;
    for (const auto& r : chap.at("required")) {
        if (r == "characters") ch_chars = true;
        if (r == "locations") ch_locs = true;
    }
    CHECK_MESSAGE(ch_chars, "读故事的每章 characters 不在 required 里");
    CHECK_MESSAGE(ch_locs, "读故事的每章 locations 不在 required 里");
    CHECK(chap.at("properties").at("characters").at("minItems") == 1);
    CHECK(chap.at("properties").at("locations").at("minItems") == 1);
}

TEST_CASE("提示词：章数跟着体量走，不跟时长走") {
    const std::string s = build_outline_prompt("深夜便利店", StoryScale::SHORT,
                                               StyleLine::REALISTIC);
    const std::string m = build_outline_prompt("深夜便利店", StoryScale::MEDIUM,
                                               StyleLine::REALISTIC);
    const std::string l = build_outline_prompt("深夜便利店", StoryScale::LONG,
                                               StyleLine::REALISTIC);

    CHECK(s.find("写成 4 章左右") != std::string::npos);
    CHECK(m.find("写成 8 章左右") != std::string::npos);
    CHECK(l.find("写成 16 章左右") != std::string::npos);

    // 梗概进得去
    CHECK(m.find("深夜便利店") != std::string::npos);
    // 「不要写长相」这条必须在提示词里，schema 挡得住字段挡不住它写进 summary
    CHECK(m.find("不要写任何长相") != std::string::npos);
    // 完整故事要有结尾，这是和「无限续写」的分界
    CHECK(m.find("有结尾的完整故事") != std::string::npos);
    // 实跑时四章写的是同一个场面（便利店、门铃、她拿着伞进来）换三个角度，
    // 根子在大纲：每章必须把故事往前挪
    CHECK(m.find("每一章都要把故事往前挪一步") != std::string::npos);
}

TEST_CASE("提示词：画风和关键词") {
    const std::string anime =
        build_outline_prompt("梗概", StoryScale::MEDIUM, StyleLine::ANIME);
    CHECK(anime.find("动漫电影") != std::string::npos);

    const std::string real =
        build_outline_prompt("梗概", StoryScale::MEDIUM, StyleLine::REALISTIC);
    CHECK(real.find("真人写实电影") != std::string::npos);

    const std::string kw = build_outline_prompt("梗概", StoryScale::MEDIUM,
                                                StyleLine::REALISTIC, "重生复仇");
    CHECK(kw.find("往这个方向想：重生复仇") != std::string::npos);
    // 没给关键词时那一段整块不出现
    CHECK(real.find("往这个方向想") == std::string::npos);
}

TEST_CASE("解析大纲") {
    const Story s =
        parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);

    CHECK(s.premise == "深夜便利店");
    CHECK(s.scale == StoryScale::MEDIUM);
    CHECK(s.source == StorySource::AI);
    CHECK(s.logline == "一把伞牵出五年前的事");
    REQUIRE(s.characters.size() == 2);
    CHECK(s.characters[0].name == "林晚");
    REQUIRE(s.relations.size() == 1);
    CHECK(s.relations[0].tension == "谁都欠对方一句没说出口的道歉");

    REQUIRE(s.chapters.size() == 2);
    // 章节 id 是程序生成的，模型说了不算
    CHECK(s.chapters[0].chapter_id == "ch01");
    CHECK(s.chapters[1].chapter_id == "ch02");
    CHECK(s.chapters[0].title == "雨夜重逢");
    // 大纲阶段没有正文，钩子挂在 0 上——正文为空时 0 既是章首也是章尾
    REQUIRE(s.chapters[0].hooks.size() == 1);
    CHECK(s.chapters[0].hooks[0].at_char == 0);
    CHECK(s.chapters[0].hooks[0].text == "她认出那把伞");
    CHECK(s.chapters[0].characters.size() == 2);

    // 自己产出的东西要能过自己的校验
    CHECK(s.validate().empty());
}

TEST_CASE("解析：把会坏事的东西挡在外面") {
    SUBCASE("关系指向没登记的人——丢掉，不能留") {
        json j = good_outline();
        j["relations"].push_back(json{{"a", "林晚"},
                                      {"b", "查无此人"},
                                      {"kind", "邻居"},
                                      {"tension", "无"}});
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.relations.size() == 1);
        CHECK(s.validate().empty());
    }

    SUBCASE("人物重名——只留第一个") {
        json j = good_outline();
        j["characters"].push_back(
            json{{"name", "林晚"}, {"identity", "另一个林晚"}, {"want", "x"}});
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.characters.size() == 2);
        CHECK(s.characters[0].identity == "便利店夜班店员");
    }

    SUBCASE("没名字的人物——丢掉，否则成片里会出现一个叫空串的角色") {
        json j = good_outline();
        j["characters"].push_back(json{{"name", "  "}, {"identity", "路人"}});
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.characters.size() == 2);
    }

    SUBCASE("章节里冒出没登记的人——过滤掉") {
        json j = good_outline();
        j["chapters"][0]["characters"].push_back("查无此人");
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.chapters[0].characters.size() == 2);
    }

    SUBCASE("一章都没有——报错，不要静悄悄产出一个空故事") {
        json j = good_outline();
        j["chapters"] = json::array();
        CHECK_THROWS_AS(parse_outline(j.dump(), "梗概", StoryScale::MEDIUM),
                        stages::StoryError);
    }

    SUBCASE("根本不是 JSON") {
        CHECK_THROWS_AS(parse_outline("模型今天想聊点别的", "梗概",
                                      StoryScale::MEDIUM),
                        stages::StoryError);
    }

    SUBCASE("裹着 ```json 外壳也要能读出来") {
        const std::string raw = "```json\n" + good_outline().dump() + "\n```";
        const Story s = parse_outline(raw, "梗概", StoryScale::MEDIUM);
        CHECK(s.chapters.size() == 2);
    }
}

TEST_CASE("GET /api/story：没有 story.json 时回空故事，不是 404") {
    const fs::path root = fresh_project("空");
    const auto r = http::get_story(p_str(root));
    CHECK(r.status == 200);
    CHECK(r.body.at("empty").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 0);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story：梗概同时写回 project.json") {
    const fs::path root = fresh_project("梗概");
    const auto r = http::post_story(json{{"project", p_str(root)},
                                         {"premise", "  深夜便利店，前任推门进来。  "},
                                         {"scale", "long"},
                                         {"episode_duration_s", 90.0}});
    CHECK(r.status == 200);

    ProjectStore store(root);
    const Story s = store.load_story();
    CHECK(s.premise == "深夜便利店，前任推门进来。");
    CHECK(s.scale == StoryScale::LONG);
    CHECK(s.episode_duration_s == doctest::Approx(90.0));

    // 老流程的写剧本提示词读的是 Project::premise，两边必须是同一句
    CHECK(store.load_project().premise == "深夜便利店，前任推门进来。");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story：体量拼错了要报错，不能悄悄当成短篇") {
    const fs::path root = fresh_project("体量");
    CHECK_THROWS_AS(
        http::post_story(json{{"project", p_str(root)}, {"scale", "midium"}}),
        http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/outline：写完直接落盘") {
    const fs::path root = fresh_project("草稿");
    llm::ReplayClient client({good_outline().dump()});
    pipeline::CancelToken tok;

    const auto r = http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"}}, client, tok);

    CHECK(r.status == 200);
    CHECK(r.body.at("adopted").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 2);
    // 大纲阶段没正文，一章一条
    CHECK(r.body.at("episodes").get<int>() == 2);

    // **直接进 story.json。** 草稿那一屏 2026-09-17 退役（理由在
    // write_outline 上）：刷新、切页、换台机器看，都从正式那份读。
    ProjectStore store(root);
    CHECK(store.load_story().chapters.size() == 2);
    CHECK(store.load_story().plan.size() == 2);
    CHECK(store.load_project().premise == "深夜便利店");
    // 梗概写回去的那一下不能把刚同步出来的章节表盖掉（2026-09-18 抓到的）
    CHECK(store.load_project().episodes.size() == 2);

    // 提示词确实拼过并发出去了
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("深夜便利店") != std::string::npos);
    CHECK(client.calls()[0].schema_name == "story_outline");

    std::error_code ec;
    fs::remove_all(root, ec);
}

namespace {

/// 一个**卡住不返回**的大模型客户端：`release()` 之前 complete() 一直等。
///
/// 回放那个几毫秒就写完，"正在跑"的窗口根本抓不住——上一版用例就是
/// 因此写成了 `if (contains) CHECK(...)`，等于没测，而真实二进制里
/// 那本账恰恰是坏的。
class BlockingClient : public llm::Client {
public:
    explicit BlockingClient(std::string reply) : reply_(std::move(reply)) {}
    std::string complete(const llm::Request&, pipeline::CancelToken&) override {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return released_; });
        return reply_;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::string reply_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool released_ = false;
};

}  // namespace

TEST_CASE("生成中途刷新：GET /api/story 要说有一轮在跑、听哪条流") {
    // 用户 2026-09-13 实测：点下去 8 秒就刷新了。那时草稿还没落盘，页面
    // 不知道后台有活在跑，一片空白——"还是不行"。
    const fs::path root = fresh_project("中途刷新");
    BlockingClient client(good_outline().dump());
    pipeline::CancelToken tok;

    const auto started = http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"},
             {"stream", "outline-test-1"}, {"async", true}},
        client, tok);
    CHECK(started.status == 202);

    // **202 一回来账上就得有它**——页面拿到 202 之后马上就会去问。
    // 客户端卡着不返回，所以这一眼一定落在"正在跑"的窗口里；查不到就是
    // 账坏了，不是时机不对。
    const auto right_after = http::get_story(p_str(root));
    REQUIRE(right_after.body.contains("outline_running"));
    CHECK(right_after.body.at("outline_running") == "outline-test-1");

    // 放行。写完：落盘，账擦掉
    client.release();
    bool settled = false;
    for (int i = 0; i < 200 && !settled; ++i) {
        const auto now = http::get_story(p_str(root));
        settled = now.body.at("chapters").get<int>() == 2 &&
                  !now.body.contains("outline_running");
        if (!settled) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(settled);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("生成中途刷新：经路由那条路（async 被剥掉）也要记账") {
    // **真实服务走的就是这条。** server.cpp 的 script_route 先把 async
    // 剥掉再把处理函数扔到后台，所以 post_story_outline 收到的 body 里
    // 只有 stream、没有 async。2026-09-13 栽过：账只记在 async 分支里，
    // 服务里从来没记上，而当时的用例又写成 `if (contains) CHECK`，等于没测。
    const fs::path root = fresh_project("路由那条路");
    BlockingClient client(good_outline().dump());
    pipeline::CancelToken tok;

    std::thread worker([&] {
        http::post_story_outline(
            json{{"project", p_str(root)}, {"premise", "深夜便利店"},
                 {"stream", "outline-test-3"}},   // 没有 async
            client, tok);
    });

    // 等它跑进 write_outline 登记（客户端卡着，窗口开着不会关）
    bool seen = false;
    for (int i = 0; i < 200 && !seen; ++i) {
        const auto now = http::get_story(p_str(root));
        seen = now.body.contains("outline_running") &&
               now.body.at("outline_running") == "outline-test-3";
        if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(seen);

    client.release();
    worker.join();
    // 写完：账擦掉，落盘
    const auto after = http::get_story(p_str(root));
    CHECK_FALSE(after.body.contains("outline_running"));
    CHECK(after.body.at("chapters").get<int>() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("生成中途刷新：写砸了那句话也要留给下一眼") {
    // job_error 只往那条流上广播一次，页面刷新过就没人听见——盘上没草稿、
    // 账也擦了，页面一片空白，连"为什么"都没有。实测就是"还是不行"。
    const fs::path root = fresh_project("中途砸了");
    // 回放一段解不出大纲的东西，让 write_outline 抛 502
    llm::ReplayClient client({"这不是 JSON"});
    pipeline::CancelToken tok;
    http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"},
             {"stream", "outline-test-2"}, {"async", true}},
        client, tok);

    // 等它砸完（账擦掉），然后那句错必须在 GET /api/story 里
    std::string err;
    for (int i = 0; i < 200 && err.empty(); ++i) {
        const auto now = http::get_story(p_str(root));
        if (now.body.contains("outline_error")) {
            err = now.body.at("outline_error").get<std::string>();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    CHECK_FALSE(err.empty());
    CHECK(err.find("大纲") != std::string::npos);
    // **留着，不清**：两台设备同时开着，读一次就清只有先问到的那台看得见。
    CHECK(http::get_story(p_str(root)).body.contains("outline_error"));
    CHECK_FALSE(http::get_story(p_str(root)).body.contains("outline_running"));
    // 下一份写成了，那句话就该没了（commit_story 里清）
    llm::ReplayClient good({good_outline().dump()});
    http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"}}, good, tok);
    CHECK_FALSE(http::get_story(p_str(root)).body.contains("outline_error"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/outline：会顶掉写好的正文时要拦在按下去之前") {
    // 大纲直接落盘，所以这道闸只能在**开跑之前**——八分钟之后再 409
    // 等于白跑一趟模型。
    const fs::path root = fresh_project("大纲顶掉");
    ProjectStore store(root);
    Story old;
    old.premise = "老故事";
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "写过的一章";
    c.text = "这一章已经有正文了。";
    old.chapters.push_back(c);
    store.save_story(old);

    llm::ReplayClient client({good_outline().dump()});
    pipeline::CancelToken tok;
    // 不带 overwrite：409，而且模型一次都没调
    CHECK_THROWS_AS(http::post_story_outline(
                        json{{"project", p_str(root)}, {"premise", "新梗概"}},
                        client, tok),
                    http::ApiError);
    CHECK(client.calls().empty());
    CHECK(store.load_story().written_chapters() == 1);

    // 带上 overwrite 才写
    const auto r = http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "新梗概"}, {"overwrite", true}},
        client, tok);
    CHECK(r.status == 200);
    CHECK(store.load_story().chapters.size() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/revise：空章上的 [0, 0) 是「从头写」，不是范围不对") {
    // 故事页 2026-09-17 起只有这一条 AI 路：选中了改选中的，没选就整章，
    // 章还是空的就是从头写——都走这条接口。空区间要认。
    const fs::path root = fresh_project("空章从头写");
    http::post_story_adopt(json{
        {"project", p_str(root)},
        {"story", json{{"premise", ""},
                       {"scale", "medium"},
                       {"episode_duration_s", 60},
                       {"chapters", json::array({json{{"chapter_id", "ch01"},
                                                      {"title", "第一章"},
                                                      {"summary", ""},
                                                      {"text", ""}}})}}},
        {"overwrite", true}});

    llm::ReplayClient client(
        {json{{"text", "深夜，便利店的灯还亮着。"}, {"note", "开了个头"}}.dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_story_revise(json{{"project", p_str(root)},
                                                {"chapter_id", "ch01"},
                                                {"from_char", 0},
                                                {"to_char", 0},
                                                {"instruction", "写个开头"}},
                                           client, tok);
    CHECK(r.status == 200);
    CHECK(r.body.at("text").get<std::string>() == "深夜，便利店的灯还亮着。");
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("这一章还是空的") != std::string::npos);

    // 位置真错了照样拒
    CHECK_THROWS_AS(http::post_story_revise(json{{"project", p_str(root)},
                                                 {"chapter_id", "ch01"},
                                                 {"from_char", 3},
                                                 {"to_char", 1},
                                                 {"instruction", "x"}},
                                            client, tok),
                    http::ApiError);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/adopt：落库并算出章节计划") {
    const fs::path root = fresh_project("采用");
    const Story draft =
        parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);

    const auto r = http::post_story_adopt(
        json{{"project", p_str(root)}, {"story", json(draft)}});
    CHECK(r.status == 200);
    CHECK(r.body.at("adopted").get<bool>());

    ProjectStore store(root);
    const Story saved = store.load_story();
    CHECK(saved.chapters.size() == 2);
    CHECK(saved.plan.size() == 2);
    CHECK(saved.plan[0].episode_id == "ep01");
    CHECK(saved.plan[0].hook == "她认出那把伞");
    // 梗概照样同步回 project.json
    CHECK(store.load_project().premise == "深夜便利店");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/adopt：会顶掉写好的正文时要拦一下") {
    const fs::path root = fresh_project("顶掉");
    ProjectStore store(root);

    // 先存一份已经展开过正文的故事
    Story old;
    old.premise = "老故事";
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "写过的一章";
    c.text = "这一章已经有正文了。";
    old.chapters.push_back(c);
    store.save_story(old);

    const Story draft =
        parse_outline(good_outline().dump(), "新梗概", StoryScale::MEDIUM);

    // 不带 overwrite：409，正文还在
    CHECK_THROWS_AS(http::post_story_adopt(
                        json{{"project", p_str(root)}, {"story", json(draft)}}),
                    http::ApiError);
    CHECK(store.load_story().written_chapters() == 1);

    // 带上 overwrite 才换
    const auto r = http::post_story_adopt(json{{"project", p_str(root)},
                                               {"story", json(draft)},
                                               {"overwrite", true}});
    CHECK(r.status == 200);
    CHECK(store.load_story().chapters.size() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/plan：改每章时长，条数不变（一章一条），时长落盘") {
    const fs::path root = fresh_project("重算");
    ProjectStore store(root);

    Story s;
    s.premise = "梗概";
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "雨夜重逢";
    for (int i = 0; i < 3000; ++i) c.text += "字";
    for (int at = 300; at < 3000; at += 300) {
        Hook h;
        h.at_char = at;
        h.text = "钩子";
        c.hooks.push_back(h);
    }
    s.chapters.push_back(c);
    store.save_story(s);

    const auto few = http::post_story_plan(
        json{{"project", p_str(root)}, {"duration_s", 120.0}});
    const int few_n = few.body.at("episodes").get<int>();

    const auto many = http::post_story_plan(
        json{{"project", p_str(root)}, {"duration_s", 30.0}});
    const int many_n = many.body.at("episodes").get<int>();

    // 一章一条：时长改多少，条数都是章数。以前这儿钉的是「30 秒切出的比
    // 120 秒多」，那是切章那套算法的事，2026-09-16 起没有了。
    CHECK(few_n == 1);
    CHECK(many_n == 1);
    // 重算的结果要落盘，不是只回给前端；整章都在
    const Story saved = store.load_story();
    REQUIRE(saved.plan.size() == 1);
    CHECK(saved.plan[0].from_char == 0);
    CHECK(saved.plan[0].to_char == 3000);
    CHECK(saved.plan[0].title == "雨夜重逢");
    CHECK(saved.episode_duration_s == doctest::Approx(30.0));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/plan：还没有故事就说清楚") {
    const fs::path root = fresh_project("没故事");
    CHECK_THROWS_AS(http::post_story_plan(json{{"project", p_str(root)}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("多余字段一律 422") {
    const fs::path root = fresh_project("多余");
    try {
        http::post_story(json{{"project", p_str(root)}, {"episodes", 3}});
        FAIL("应该抛");
    } catch (const http::ApiError& e) {
        // 「我要写 N 集」这个入参没有了，传上来要被顶回去
        CHECK(e.status() == 422);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 期 3：圣经的名单从故事来，不再从第一章剧本里找 ----

namespace {

/// 美术那一步的模型返回。
json good_bible() {
    return json{
        {"characters",
         json::array({
             json{{"key", "lin_wan"},
                  {"name", "林晚"},
                  {"identity", "二十七八岁女性，克制"},
                  {"body", "偏瘦，肩背挺"},
                  {"face", "齐肩黑直发，圆眼，单眼皮"},
                  {"attire", "便利店藏青制服外套"}},
             json{{"key", "chen_mo"},
                  {"name", "陈默"},
                  {"identity", "三十出头男性，沉静"},
                  {"body", "中等身量"},
                  {"face", "短寸黑发，方脸，浓眉"},
                  {"attire", "深灰风衣"}},
         })},
        {"locations",
         json::array({
             json{{"key", "store_night"},
                  {"name", "便利店"},
                  {"space", "临街玻璃门，两排货架"},
                  {"lighting", "夜间冷白顶光，玻璃上有雨痕"},
                  {"palette", "冷青加一点暖黄"}},
         })},
        {"global_style", "夜戏，低饱和，轻微颗粒"},
    };
}

Story sample_story() {
    return parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);
}

}  // namespace

TEST_CASE("渲染给美术看的那一段：名单一个都不能少") {
    const std::string t = stages::render_story_for_bible(sample_story());

    // 名单是这一段的全部意义。漏一个人，后面分镜里就指不到它。
    CHECK(t.find("林晚") != std::string::npos);
    CHECK(t.find("陈默") != std::string::npos);
    CHECK(t.find("便利店") != std::string::npos);

    // 调子要在名单前面——放后面的话模型把人都写完了才读到"克制"
    CHECK(t.find("克制") < t.find("林晚"));

    // 关系给进去了。这是老路径完全没有的东西
    CHECK(t.find("前任") != std::string::npos);
    CHECK(t.find("谁都欠对方一句没说出口的道歉") != std::string::npos);

    // 欲望进去是让美术判断气质用的
    CHECK(t.find("他要的是") != std::string::npos);

    // 地点的时间和光要带上，lighting 要照着它写
    CHECK(t.find("深夜，冷白顶光") != std::string::npos);

    // 分章只当调子参考
    CHECK(t.find("雨夜重逢") != std::string::npos);
}

TEST_CASE("从故事出的圣经提示词：名单给定，只定妆") {
    const Story story = sample_story();
    const std::string p =
        stages::build_bible_prompt_from_story(story, StyleLine::REALISTIC);

    // 这条是新老两条路的分界：老的是"找出角色"，新的是"给这份名单定妆"
    CHECK(p.find("一个不许多，一个不许少") != std::string::npos);
    // 名字必须照抄，后面每一镜按名字找角色。**真正管住它的是语法**：
    // name 上挂着真名单的 enum（bible.cpp），名单外的名字生成不出来；
    // 表里这句是给模型一个说法，两边都在。
    CHECK(p.find("逐字一样") != std::string::npos);
    // 剧作信息不许写进外观
    CHECK(p.find("不要把它们写进外观") != std::string::npos);
    CHECK(p.find("真人写实") != std::string::npos);
    // **face 那条 2026-09-17 从表里搬走了**：它只管 face 一栏，而那一栏的
    // description 里本来就写着「这段会在几十个镜头里逐字复用，写得具体且
    // 不要含糊」。schema 是以文字贴在提示词后面发的（llm::schema_as_prompt），
    // 所以模型照样收得到——查的是合起来那一份。
    const std::string full =
        llm::schema_as_prompt(p, stages::bible_schema());
    CHECK(full.find("face") != std::string::npos);
    CHECK(full.find("逐字复用") != std::string::npos);
    // 故事那一段确实拼进去了
    CHECK(p.find(stages::render_story_for_bible(story)) != std::string::npos);

    const std::string anime =
        stages::build_bible_prompt_from_story(story, StyleLine::ANIME);
    CHECK(anime.find("二次元动漫") != std::string::npos);
}

TEST_CASE("POST /api/bible：项目里有故事就从故事出") {
    const fs::path root = fresh_project("圣经故事");
    ProjectStore store(root);
    Story story = sample_story();
    store.save_story(story);

    llm::ReplayClient client({good_bible().dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_bible(json{{"project", p_str(root)}}, client, tok);

    CHECK(r.status == 200);
    // 回包要说清这次名单是从哪来的——「为什么这次多出来三个人」靠它解释
    CHECK(r.body.at("source").get<std::string>() == "story");
    CHECK(r.body.at("added_characters").size() == 2);

    // 发出去的提示词走的是故事那条
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("一个不许多，一个不许少") !=
          std::string::npos);
    // 而且**没有**去读任何一章剧本
    CHECK(client.calls()[0].prompt.find("读下面的剧本") == std::string::npos);

    CHECK(store.load_assets().characters.size() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/bible：没有故事的老项目照旧走剧本那条") {
    const fs::path root = fresh_project("圣经剧本");
    ProjectStore store(root);
    Project project = store.load_project();
    Episode ep;
    ep.episode_id = "ep01";
    ep.title = "雨夜重逢";
    ep.script = "林晚：你还留着它。\n陈默把伞放在柜台上。";
    project.episodes.push_back(ep);
    store.save_project(project);

    llm::ReplayClient client({good_bible().dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_bible(json{{"project", p_str(root)}}, client, tok);

    CHECK(r.status == 200);
    CHECK(r.body.at("source").get<std::string>() == "script");
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("读下面的剧本") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/bible：source 能强制走哪条") {
    const fs::path root = fresh_project("圣经强制");
    ProjectStore store(root);
    store.save_story(sample_story());
    Project project = store.load_project();
    Episode ep;
    ep.episode_id = "ep01";
    ep.script = "林晚：你还留着它。";
    project.episodes.push_back(ep);
    store.save_project(project);

    SUBCASE("有故事也能按剧本出") {
        llm::ReplayClient client({good_bible().dump()});
        pipeline::CancelToken tok;
        const auto r = http::post_bible(
            json{{"project", p_str(root)}, {"source", "script"}}, client, tok);
        CHECK(r.body.at("source").get<std::string>() == "script");
    }

    SUBCASE("source 只认三个值") {
        llm::ReplayClient client({good_bible().dump()});
        pipeline::CancelToken tok;
        try {
            http::post_bible(
                json{{"project", p_str(root)}, {"source", "novel"}}, client, tok);
            FAIL("应该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 422);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/bible：点名要故事但项目里没有") {
    const fs::path root = fresh_project("圣经没故事");
    llm::ReplayClient client({good_bible().dump()});
    pipeline::CancelToken tok;
    try {
        http::post_bible(json{{"project", p_str(root)}, {"source", "story"}},
                         client, tok);
        FAIL("应该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
    }
    // 一次模型都不该调
    CHECK(client.calls().empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 梗概不是必填的 ----
//
// 三个入口里只有「我自己有个想法」那条是从手写的一句话开始的；
// 给几个关键词、或者什么都不给让它来一个，同样正当。把梗概做成硬门槛
// 等于又把人摁回空白框前面发呆，而选题本来就是最难从零开始的一步。

TEST_CASE("没给梗概：提示词让模型自己定选题") {
    const std::string none =
        build_outline_prompt("", StoryScale::MEDIUM, StyleLine::REALISTIC);
    CHECK(none.find("这部电影讲什么**由你定**") != std::string::npos);
    // 「这部电影讲的是：」后面本来要跟梗概，没梗概时整段都不该出现
    CHECK(none.find("这部电影讲的是") == std::string::npos);

    const std::string with =
        build_outline_prompt("深夜便利店", StoryScale::MEDIUM, StyleLine::REALISTIC);
    CHECK(with.find("这部电影讲的是") != std::string::npos);
    CHECK(with.find("由你定") == std::string::npos);

    // 只给关键词也算「没给梗概」，方向照样带进去
    const std::string kw = build_outline_prompt("", StoryScale::MEDIUM,
                                                StyleLine::REALISTIC, "重生复仇");
    CHECK(kw.find("往这个方向想：重生复仇") != std::string::npos);
    CHECK(kw.find("由你定") != std::string::npos);
}

TEST_CASE("schema 里有 premise，给模型一个地方写它定的选题") {
    CHECK(outline_schema().at("properties").contains("premise"));
}

TEST_CASE("梗概谁说了算") {
    json j = good_outline();
    j["premise"] = "模型自己想的那个选题";

    SUBCASE("用户没给：收模型的") {
        const Story s = parse_outline(j.dump(), "", StoryScale::MEDIUM);
        CHECK(s.premise == "模型自己想的那个选题");
    }

    SUBCASE("用户给了：一个字不动，不让它改写") {
        const Story s = parse_outline(j.dump(), "用户写的那一句", StoryScale::MEDIUM);
        CHECK(s.premise == "用户写的那一句");
    }

    SUBCASE("两边都没有：空着，但别的照样解析得出来") {
        json empty = good_outline();
        const Story s = parse_outline(empty.dump(), "", StoryScale::MEDIUM);
        CHECK(s.premise.empty());
        CHECK(s.chapters.size() == 2);
    }
}

TEST_CASE("POST /api/story/outline：一个字都没有也照写") {
    const fs::path root = fresh_project("空梗概");
    json reply = good_outline();
    reply["premise"] = "模型自己想的那个选题";
    llm::ReplayClient client({reply.dump()});
    pipeline::CancelToken tok;

    // premise 不传、项目上也没有——原来这里是 400
    const auto r =
        http::post_story_outline(json{{"project", p_str(root)}}, client, tok);
    CHECK(r.status == 200);
    CHECK(r.body.at("story").at("premise").get<std::string>() ==
          "模型自己想的那个选题");
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("由你定") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 期 4：从故事写一章 ----

namespace {

/// 一份展开过正文的故事：两章各 1000 字，各带一个钩子。
Story written_story() {
    Story s = sample_story();
    for (int i = 0; i < 2; ++i) {
        Chapter& c = s.chapters[i];
        c.text.clear();
        for (int k = 0; k < 1000; ++k) c.text += (i == 0 ? "甲" : "乙");
        c.hooks.clear();
        Hook h;
        h.at_char = 500;
        h.text = i == 0 ? "她认出那把伞" : "他没有回头";
        c.hooks.push_back(h);
    }
    s.plan = changji::stages::plan_episodes(s, 30.0);
    return s;
}

}  // namespace

TEST_CASE("取一条计划覆盖的那段正文：按字符切，不按字节") {
    const Story s = written_story();
    REQUIRE(s.plan.size() >= 2);

    for (const auto& p : s.plan) {
        const std::string t = changji::stages::episode_text(s, p);
        // 切出来必须是完整的汉字。劈成半个的话字符数会对不上
        CHECK(t.size() % 3 == 0);
        CHECK_FALSE(t.empty());
    }

    // 第一条从头开始
    CHECK(s.plan[0].from_char == 0);
    // 相邻两条首尾相接，不重不漏
    for (std::size_t i = 1; i < s.plan.size(); ++i) {
        if (s.plan[i].from_chapter == s.plan[i - 1].to_chapter) {
            CHECK(s.plan[i].from_char == s.plan[i - 1].to_char);
        }
    }
}

TEST_CASE("上下文：前情是压缩过的，而且只到这一章之前") {
    Story s = written_story();
    // 手工造一条落在第二章的计划条目，好让前情里有东西
    EpisodePlan p;
    p.episode_id = "ep09";
    p.target_duration_s = 60.0;
    p.from_chapter = "ch02";
    p.from_char = 0;
    p.to_chapter = "ch02";
    p.to_char = 1000;
    p.hook = "他没有回头";

    const auto scenes = changji::stages::chapter_scene_plan(s, p);
    const std::string ctx = changji::stages::render_script_context(s, p, "", scenes);

    // 前情提要在，而且是**每章一句的梗概**，不是正文原文
    CHECK(ctx.find("【前情提要】") != std::string::npos);
    CHECK(ctx.find("他推门进来") != std::string::npos);
    // 第一章的正文（一千个「甲」）不该整段搬进来
    CHECK(ctx.find("甲甲甲甲甲甲甲甲甲甲") == std::string::npos);

    // 这一章的正文在
    CHECK(ctx.find("【这一章】") != std::string::npos);
    CHECK(ctx.find("乙乙乙") != std::string::npos);

    // **停在哪**——这一条老路线完全没有
    CHECK(ctx.find("【这一章要停在】他没有回头") != std::string::npos);

    // 人物和关系是压缩的全局记忆
    CHECK(ctx.find("林晚") != std::string::npos);
    CHECK(ctx.find("前任") != std::string::npos);
}

TEST_CASE("第一章没有前情") {
    const Story s = written_story();
    const auto scenes = changji::stages::chapter_scene_plan(s, s.plan[0]);
    const std::string ctx =
        changji::stages::render_script_context(s, s.plan[0], "", scenes);
    CHECK(ctx.find("【前情提要】") == std::string::npos);
}

TEST_CASE("上一章的结尾接得上，而且不从半行中间起") {
    const Story s = written_story();
    const std::string prev = "林晚：你还留着它。\n陈默把伞放在柜台上，没有说话。";
    const auto scenes = changji::stages::chapter_scene_plan(s, s.plan[0]);
    const std::string ctx =
        changji::stages::render_script_context(s, s.plan[0], prev, scenes);
    CHECK(ctx.find("【上一章是这么结束的】") != std::string::npos);
    CHECK(ctx.find("陈默把伞放在柜台上") != std::string::npos);

    // 很长的剧本只取尾巴，而且从行首起
    std::string longer;
    for (int i = 0; i < 60; ++i) longer += "林晚：这是第 x 句台词。\n";
    longer += "陈默：最后一句。";
    const std::string tail = changji::stages::script_tail(longer);
    CHECK(tail.find("陈默：最后一句。") != std::string::npos);
    CHECK(tail.rfind("林晚：", 0) == 0);
}

// 这儿原来有一条「提示词：这一集要发生什么已经定好了」，钉的是
// build_script_prompt_from_story——集模式那条路的剧本提示词（按秒写、
// 四段按秒排、字数预算跟着算）。2026-09-16 用户定了只留章模式，那个函数
// 和它独占的四串提示词一起删了。章模式那条的提示词由下面
// 「照着章写剧本」那几条钉着。

/// 往老项目的 changji.toml 里写一行 `[assembly].episode_s`。
///
/// **这一项 2026-09-18 已经没人读了**（成片切段随电影平台一起拔掉，
/// 那之后它只剩一个回落值的用处，收成了 stages::kDefaultChapterS）。
/// 写它只为证明一件事：老配置里留着它，项目照样打开、不报错。
/// settings 的 take() 是「键存在才覆盖」，不 take 就是忽略——别引入校验。
static void write_legacy_episode_s(const fs::path& root, double seconds) {
    std::ofstream f(root / "changji.toml", std::ios::app);
    f << "\n[assembly]\nepisode_s = " << seconds << "\n";
}

TEST_CASE("POST /api/story/episodes：落成章节，一章一个") {
    const fs::path root = fresh_project("落成章节");
    ProjectStore store(root);
    const Story s = written_story();
    store.save_story(s);

    const auto r = http::post_story_episodes(json{{"project", p_str(root)}});
    CHECK(r.status == 200);
    // **一章一个，不照章节计划。** 用户 2026-09-16：「落成剧集改成一章一个」，
    // 同一天又定了只留章模式。这儿原来先往 toml 里写 episode_s = 0 切回老路线、
    // 再钉「一条计划一个章节」——那条路已经没有了。
    // 语料里 plan 有 4 条而章只有 2 章，所以这两个数不一样，钉的是章数。
    CHECK(r.body.at("created").size() == s.chapters.size());

    Project project = store.load_project();
    REQUIRE(project.episodes.size() == s.chapters.size());
    const Episode& first = project.episodes[0];
    CHECK_FALSE(first.chapter_refs.empty());
    CHECK(first.chapter_refs[0] == "ch01");

    SUBCASE("再来一次：只补元数据，写好的剧本一个字不动") {
        Project p2 = store.load_project();
        p2.episodes[0].script = "林晚：这是已经写好的剧本。";
        store.save_project(p2);

        const auto again =
            http::post_story_episodes(json{{"project", p_str(root)}});
        CHECK(again.body.at("created").empty());
        CHECK(again.body.at("updated").size() == s.chapters.size());
        CHECK(store.load_project().episodes[0].script ==
              "林晚：这是已经写好的剧本。");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("章节表自动跟着章走，不用按任何按钮") {
    // 用户 2026-09-16 选的是「自动做，不要按钮」——一章一条是机械映射。
    // **这条从头到尾不叫 post_story_episodes**：它要证明的正是「不按也对齐」。
    const fs::path root = fresh_project("自动对齐");
    ProjectStore store(root);

    Story s = written_story();
    REQUIRE(s.chapters.size() >= 2);

    SUBCASE("存一次故事，章节表就出来了") {
        store.save_story(s);
        // 走接口存（post_story 里挂着对齐那一句）
        const auto r = http::post_story(json{{"project", p_str(root)},
                                             {"premise", "自动对齐试一下"}});
        CHECK(r.status == 200);

        const Project project = store.load_project();
        CHECK(project.episodes.size() == s.chapters.size());
        CHECK(project.episodes[0].episode_id == "ep01");
        REQUIRE_FALSE(project.episodes[0].chapter_refs.empty());
        CHECK(project.episodes[0].chapter_refs[0] == "ch01");
    }

    SUBCASE("老配置里写着 episode_s 的，照样打开、照样跟着章节走") {
        // 两次迁移叠在这一条上：
        //   · 2026-09-16 删掉「集模式 / 章模式」那个开关之后，0 不再表示
        //     "走老路线"（老路线是「一个章节都不建」，正是这条要挡住的坑）。
        //   · 2026-09-18 `[assembly].episode_s` 整个从配置里去掉了。
        //     settings 的 take() 是「键存在才覆盖」，这一项不再 take 就是
        //     **忽略**它——没有校验、不报错。用户 2026-09-18 定死的：
        //     老项目的 changji.toml 里写着 episode_s 时不许报错。
        //
        // 所以这儿逐个值试：读得动、不抛，而且章节表照样跟着章走。
        for (const double legacy : {0.0, 90.0}) {
            CAPTURE(legacy);
            const fs::path old_root =
                fresh_project("老配置迁移" + std::to_string(static_cast<int>(legacy)));
            ProjectStore old_store(old_root);
            write_legacy_episode_s(old_root, legacy);
            // 先单独钉「读得动」：下面走接口时抛的可能是别的原因，
            // 分开写才说得清是配置本身没炸。
            CHECK_NOTHROW(config::load_settings(old_root));
            old_store.save_story(written_story());
            http::post_story(json{{"project", p_str(old_root)}, {"premise", "老配置"}});
            const Project migrated = old_store.load_project();
            CHECK_FALSE(migrated.episodes.empty());
            REQUIRE_FALSE(migrated.episodes[0].chapter_refs.empty());
            CHECK(migrated.episodes[0].chapter_refs[0] == "ch01");
            std::error_code ec2;
            fs::remove_all(old_root, ec2);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/episodes：一章一条") {
    // 用户 2026-09-16：「落成剧集改成一章一个。」原来按 story.plan 一条
    // 一条建——那张表当年是把章正文按时长切出来的，一章能切成好几条。
    const fs::path root = fresh_project("一章一条");
    ProjectStore store(root);
    Story s = written_story();
    // 让第 1 章占两条：章节计划里两条都结束在 ch01
    REQUIRE(s.plan.size() >= 2);
    s.plan[0].to_chapter = "ch01";
    s.plan[1].to_chapter = "ch01";
    s.plan[0].target_duration_s = 60.0;
    s.plan[1].target_duration_s = 60.0;
    store.save_story(s);

    const auto r = http::post_story_episodes(json{{"project", p_str(root)}});
    CHECK(r.status == 200);

    Project project = store.load_project();
    // 一章一个，不是一条计划一个
    CHECK(project.episodes.size() == s.chapters.size());
    CHECK(r.body.at("created").size() == s.chapters.size());

    // id 跟着章号走：ch01 → ep01
    const Episode& first = project.episodes[0];
    CHECK(first.episode_id == "ep01");
    REQUIRE(first.chapter_refs.size() == 1);
    CHECK(first.chapter_refs[0] == "ch01");
    CHECK(first.title == s.chapters[0].title);

    // 这一章值多长：原来数的是章节计划里结束在这一章的条目（两条 60 秒 = 120）。
    // 2026-09-16 起按正文字数估（每秒消化多少字那个系数），不数计划条目
    // ——短章被并进一条时一条都数不到、跨章的条目把整段记到后一章头上。
    const double want_s =
        s.chapters[0].text_len() > 0
            ? s.chapters[0].text_len() / changji::stages::kProseCharsPerSecond
            : 60.0;
    CHECK(first.target_duration_s == doctest::Approx(want_s));

    SUBCASE("再来一次：只补元数据，写好的剧本一个字不动") {
        Project p2 = store.load_project();
        p2.episodes[0].script = "林晚：这是已经写好的剧本。";
        store.save_project(p2);
        const auto again =
            http::post_story_episodes(json{{"project", p_str(root)}});
        CHECK(again.body.at("created").empty());
        CHECK(again.body.at("updated").size() == s.chapters.size());
        CHECK(store.load_project().episodes[0].script ==
              "林晚：这是已经写好的剧本。");
    }

    SUBCASE("按老路线建过的那几个不删，单独报出来") {
        Project p2 = store.load_project();
        Episode stray;
        stray.episode_id = "ep99";
        stray.script = "按章节计划建的，已经出过片";
        p2.episodes.push_back(stray);
        store.save_project(p2);

        const auto again =
            http::post_story_episodes(json{{"project", p_str(root)}});
        const auto orphans = again.body.at("orphans");
        REQUIRE(orphans.size() == 1);
        CHECK(orphans[0] == "ep99");
        // 不删：它可能已经出过片
        bool still = false;
        for (const auto& e : store.load_project().episodes) {
            if (e.episode_id == "ep99") still = true;
        }
        CHECK(still);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/episodes：章模式下没有章节") {
    const fs::path root = fresh_project("章模式没章节");
    CHECK_THROWS_AS(http::post_story_episodes(json{{"project", p_str(root)}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/episodes：还没有章节计划") {
    const fs::path root = fresh_project("没章节计划");
    CHECK_THROWS_AS(http::post_story_episodes(json{{"project", p_str(root)}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 粘贴导入：三种来源里的第二条 ----

TEST_CASE("认出作者自己分的章") {
    const std::string novel =
        "第一章 雨夜重逢\n"
        "他推门进来，伞还在手里。\n"
        "林晚认出了那把伞。\n"
        "第二章 五年前那把伞\n"
        "回到五年前的那个雨夜。\n"
        "第三章 便利店打烊\n"
        "卷帘门落下来。";

    const auto chs = changji::stages::split_pasted(novel);
    REQUIRE(chs.size() == 3);
    CHECK(chs[0].chapter_id == "ch01");
    CHECK(chs[0].title == "第一章 雨夜重逢");
    CHECK(chs[1].title == "第二章 五年前那把伞");
    // 标题行本身不进正文
    CHECK(chs[0].text.find("第一章") == std::string::npos);
    CHECK(chs[0].text.find("他推门进来") != std::string::npos);
    // 段落边界登记成候选切点，不然一整章只有章界一个候选
    CHECK_FALSE(chs[0].hooks.empty());
    // 钩子文本留空：段落边界不是真钩子，不编一句假的出来
    CHECK(chs[0].hooks[0].text.empty());
}

TEST_CASE("认标题：宁可漏认不要错认") {
    std::string t;
    CHECK(changji::stages::split_pasted("## 楔子\n正文一\n## 第一章\n正文二").size() == 2);

    // 正文里提到「第三章」的长句子不该被当成标题——错认会让那一章从
    // 半句话开始，而且章名是一整句废话
    const std::string tricky =
        "第一章\n"
        "他翻开那本书，第三章的页脚被人折过，折痕很深，像是反复读过很多遍。\n"
        "第二章\n"
        "她没有回答。";
    const auto chs = changji::stages::split_pasted(tricky);
    REQUIRE(chs.size() == 2);
    CHECK(chs[0].text.find("折痕很深") != std::string::npos);
}

TEST_CASE("一个标题都没有：按字数在段落边界上切") {
    // 六十段，每段约 100 字
    std::string novel;
    for (int i = 0; i < 60; ++i) {
        for (int k = 0; k < 100; ++k) novel += "字";
        novel += "\n";
    }

    const auto chs = changji::stages::split_pasted(novel, 1000);
    CHECK(chs.size() >= 4);

    for (const auto& c : chs) {
        // 切出来必须是完整的汉字
        CHECK(c.text.size() % 3 == 0);
        CHECK_FALSE(c.title.empty());
        CHECK_FALSE(c.text.empty());
    }

    // 一个字都不能丢。数「字」本身，不数换行——换行在章界上会被
    // strip_ws 削掉，算进来只会让这条断言变成在量空白。
    std::size_t kept = 0;
    for (const auto& c : chs) {
        for (const auto& ch : changji::text::utf8_chars(c.text)) {
            if (ch == "字") ++kept;
        }
    }
    CHECK(kept == 60 * 100);
}

TEST_CASE("切出来的章直接一章一条，整章都在") {
    std::string novel;
    for (int i = 0; i < 40; ++i) {
        for (int k = 0; k < 100; ++k) novel += "字";
        novel += "\n";
    }
    Story s;
    s.chapters = changji::stages::split_pasted(novel, 2000);
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);

    // 一章一条，不管一章多长：这儿每章约 2000 字，远超 60 秒的 900 字容量
    REQUIRE(s.plan.size() == s.chapters.size());
    CHECK(s.validate().empty());
    for (std::size_t i = 0; i < s.plan.size(); ++i) {
        const auto& p = s.plan[i];
        const Chapter& c = s.chapters[i];
        CHECK(p.from_chapter == c.chapter_id);
        CHECK(p.to_chapter == c.chapter_id);
        CHECK(p.from_char == 0);
        CHECK(p.to_char == c.text_len());
    }
}

TEST_CASE("空的和切不出来的") {
    CHECK(changji::stages::split_pasted("").empty());
    CHECK(changji::stages::split_pasted("   \n  \n ").empty());
    // 一行字也算一章
    CHECK(changji::stages::split_pasted("就这一句。").size() == 1);
}

TEST_CASE("POST /api/story/import：切完直接落盘") {
    const fs::path root = fresh_project("粘贴");
    const std::string novel =
        "第一章 雨夜重逢\n他推门进来。\n第二章 五年前\n回到五年前。";

    const auto r = http::post_story_import(
        json{{"project", p_str(root)}, {"text", novel}});
    CHECK(r.status == 200);
    CHECK(r.body.at("adopted").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 2);
    CHECK(r.body.at("written").get<int>() == 2);  // 粘进来的就是有正文的
    CHECK(r.body.at("story").at("source").get<std::string>() == "pasted");
    // 人物还提不出来，前端要靠这个数提醒人下一步
    CHECK(r.body.at("needs_analysis").get<bool>());

    ProjectStore store(root);
    CHECK(store.load_story().chapters.size() == 2);

    SUBCASE("会顶掉写好的正文时要拦一下") {
        CHECK_THROWS_AS(http::post_story_import(
                            json{{"project", p_str(root)}, {"text", novel}}),
                        http::ApiError);
        CHECK(http::post_story_import(json{{"project", p_str(root)},
                                           {"text", novel},
                                           {"overwrite", true}})
                  .status == 200);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/import：空文本") {
    const fs::path root = fresh_project("粘空的");
    CHECK_THROWS_AS(http::post_story_import(
                        json{{"project", p_str(root)}, {"text", "   "}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 读一遍现成的正文，把结构提出来 ----

namespace {

/// 一份粘进来、已经切好章的故事。
Story pasted_story() {
    Story s;
    s.source = StorySource::PASTED;
    const std::string novel =
        "第一章 雨夜重逢\n"
        "他推门进来，伞还在手里。\n"
        "林晚认出了那把伞。\n"
        "第二章 五年前那把伞\n"
        "回到五年前的那个雨夜。\n"
        "他没有回头。";
    s.chapters = changji::stages::split_pasted(novel);
    return s;
}

/// 老形状：单个 hook + hook_after。留一条用例盯着它还能读。
json good_analysis_legacy() {
    return json{
        {"logline", "一把伞牵出五年前的事"},
        {"genre", "都市情感"},
        {"tone", "克制"},
        {"characters",
         json::array({json{{"name", "林晚"},
                           {"identity", "便利店夜班店员"},
                           {"want", "把伞还回去"},
                           {"arc", "从躲到面对"}}})},
        {"relations", json::array()},
        {"locations",
         json::array({json{{"name", "便利店"}, {"what", "临街那家"}, {"when", "深夜"}}})},
        {"chapters",
         json::array({
             json{{"chapter_id", "ch01"},
                  {"summary", "他带着伞回来了。"},
                  {"hook", "她认出那把伞"},
                  {"hook_after", "他推门进来，伞还在手里。"},
                  {"characters", json::array({"林晚"})},
                  {"locations", json::array({"便利店"})}},
             json{{"chapter_id", "ch02"},
                  {"summary", "回到五年前。"},
                  {"hook", "他没有回头"},
                  {"hook_after", "回到五年前的那个雨夜。"}},
         })},
    };
}

/// 新形状：每章好几个钩子，各自带一句原文当锚。
json good_analysis() {
    json j = good_analysis_legacy();
    for (auto& c : j["chapters"]) {
        const std::string hook = c.value("hook", std::string());
        const std::string after = c.value("hook_after", std::string());
        c.erase("hook");
        c.erase("hook_after");
        c["hooks"] = json::array({json{{"text", hook}, {"after", after}}});
    }
    return j;
}

}  // namespace

TEST_CASE("节选：每章都在，中间省略要标出来") {
    Story s;
    for (int i = 0; i < 3; ++i) {
        Chapter c;
        c.chapter_id = "ch0" + std::to_string(i + 1);
        c.title = "第 " + std::to_string(i + 1) + " 章";
        c.text = "开头这一句。";
        for (int k = 0; k < 8000; ++k) c.text += "中";
        c.text += "结尾这一句。";
        s.chapters.push_back(c);
    }

    const std::string t = changji::stages::render_chapters_for_analysis(s);
    // 三章一章都不能少——漏掉一整章比每章少几百字糟糕得多
    for (int i = 1; i <= 3; ++i) {
        CHECK(t.find("ch0" + std::to_string(i)) != std::string::npos);
    }
    // 头尾都要，中间省略
    CHECK(t.find("开头这一句") != std::string::npos);
    CHECK(t.find("结尾这一句") != std::string::npos);
    CHECK(t.find("（中间略）") != std::string::npos);
    // 总量受控，不会把整本书塞进去
    CHECK(changji::text::utf8_len(t) < 20000);

    // 短章整章给，不省略
    Story small;
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "短的";
    c.text = "就这么几个字。";
    small.chapters.push_back(c);
    const std::string t2 = changji::stages::render_chapters_for_analysis(small);
    CHECK(t2.find("（中间略）") == std::string::npos);
}

TEST_CASE("没正文的章：读故事既不看它，也不许替它说话") {
    // 用户 2026-09-19：「理解故事里的人物和场景也是，都没内容怎么就知道这个
    // 人会在空白章节中呢」。
    //
    // 实跑那一趟就在 llm_log 里（八章只写了第一章）：提示词里 ch02–ch08 各
    // 是一个光标题加一片空白，回来**八章全带着梗概、出场人、地点**——「陈默
    // 按照片线索来到废弃纺织厂，在梧桐树下挖出当年埋藏的铁盒」，而那一章一个
    // 字都没有。落库之后它还把大纲原来写的梗概整份换掉了。
    //
    // 两头各挡一道，挡的是两件事：**给它看什么**，和**它说什么算数**。
    Story s = pasted_story();
    REQUIRE(s.chapters.size() == 2);
    s.chapters[1].text.clear();          // 第二章还没写
    s.chapters[1].hooks.clear();         // 钩子记的是正文里的位置
    s.chapters[1].scenes.clear();
    s.chapters[1].summary = "大纲写的：回到五年前那个雨夜。";
    s.chapters[1].characters = {"林晚"};
    s.chapters[1].locations = {"便利店"};

    SUBCASE("提示词里没有它——连标题都不给") {
        const std::string t = changji::stages::render_chapters_for_analysis(s);
        CHECK(t.find("ch01") != std::string::npos);
        CHECK(t.find("他推门进来") != std::string::npos);
        // **标题也不给。** 光一个「五年前那把伞」就够模型编出一整章了，
        // 那正是实跑里发生的事。
        CHECK(t.find("ch02") == std::string::npos);
        CHECK(t.find("五年前那把伞") == std::string::npos);
    }

    SUBCASE("省下来的额度归写出来的那几章，不白占着") {
        // `per` 原来按**全部**章分：一本八章只写了一章的书，那一章只拿到
        // 八分之一，剩下 7/8 让七章空的占着没用。
        Story one;
        Chapter c;
        c.chapter_id = "ch01";
        c.title = "写了的";
        c.text = "开头这一句。";
        for (int k = 0; k < 4000; ++k) c.text += "中";
        c.text += "结尾这一句。";
        one.chapters.push_back(c);
        for (int i = 2; i <= 8; ++i) {
            Chapter blank;
            blank.chapter_id = "ch0" + std::to_string(i);
            blank.title = "还没写的";
            one.chapters.push_back(blank);
        }
        const std::string t = changji::stages::render_chapters_for_analysis(one);
        // 一章独占整份预算，这 4000 字整章给得下，不用掐中间
        CHECK(t.find("（中间略）") == std::string::npos);
        CHECK(t.find("结尾这一句") != std::string::npos);
    }

    SUBCASE("模型硬要替空章说话，落库这头不认") {
        // 提示词里没发给它，它照样可能顺着第一章往下编一个 ch02 出来——
        // 那时候只有落库这一道拦得住。
        json j = good_analysis();
        for (auto& c : j["chapters"]) {
            if (c.value("chapter_id", std::string()) != "ch02") continue;
            c["summary"] = "模型编的：他在纺织厂梧桐树下挖出一个铁盒。";
            c["characters"] = json::array({"林晚"});
            c["locations"] = json::array({"便利店"});
            c["hooks"] = json::array({json{{"text", "编的钩子"}, {"after", ""}}});
        }
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        REQUIRE(got.chapters.size() == 2);
        const Chapter& blank = got.chapters[1];
        CHECK(blank.summary == "大纲写的：回到五年前那个雨夜。");
        CHECK(blank.characters == std::vector<std::string>{"林晚"});  // 大纲那份，原样
        CHECK(blank.locations == std::vector<std::string>{"便利店"});
        CHECK(blank.hooks.empty());
        CHECK(blank.text.empty());
    }

    SUBCASE("章号抄成了整行抬头，照样认得出是哪一章") {
        // **2026-09-19 实跑逮到的**：提示词里每章的抬头是
        // 「【ch01】错位的发条」，只发一章过去那一趟，模型把 `chapter_id`
        // 整行抄了回来。`chapter_by_id` 查不到 → 那一条整个丢掉 → **读了
        // 一遍等于没读**：人物表更新了，章里的梗概、出场人、钩子一个字没
        // 动，而且一声不响。
        json j = good_analysis();
        for (auto& c : j["chapters"]) {
            const std::string id = c.value("chapter_id", std::string());
            c["chapter_id"] = "【" + id + "】错位的发条";
            if (id == "ch01") c["summary"] = "读出来的：他带着那把伞回来了。";
        }
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        CHECK(got.chapters[0].summary == "读出来的：他带着那把伞回来了。");
        // 空章那一条照样不认——两道闸互不顶替
        CHECK(got.chapters[1].summary == "大纲写的：回到五年前那个雨夜。");
    }

    SUBCASE("写过正文的那一章照常重填") {
        // 这一条是上面那道闸的另一半：别顺手把该改的也挡掉了。
        json j = good_analysis();
        for (auto& c : j["chapters"]) {
            if (c.value("chapter_id", std::string()) != "ch01") continue;
            c["summary"] = "读出来的：他带着那把伞回来了。";
        }
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        CHECK(got.chapters[0].summary == "读出来的：他带着那把伞回来了。");
        CHECK_FALSE(got.chapters[0].characters.empty());
    }
}

TEST_CASE("提示词：读，不要改写") {
    // **要盯的是模型真正收到的那一整份，不是指令那一半。**
    //
    // schema 不走 response_format，是**以文字贴在提示词后面**发的
    // （llm::schema_as_prompt，2026-09-14 起只有这一种发法）。所以字段的
    // description 和硬性要求那张表一样都到模型手上——这正是"这条规则能
    // 从表里砍掉"的前提。这一条原来只查 build_analyze_prompt 的输出，
    // 于是 2026-09-17 把「不要写长相」「chapter_id 照抄」从表里砍掉
    // （identity / chapter_id 的 description 里逐字就有）时它当场红了，
    // 而模型收到的东西一个字没少。改成查合起来那一份。
    const Story s = pasted_story();
    const std::string p =
        changji::stages::build_analyze_prompt(s, StyleLine::REALISTIC);
    const std::string full =
        llm::schema_as_prompt(p, changji::stages::analyze_schema());

    // 这几条只有硬性要求那张表里有——schema 一点忙都帮不上
    CHECK(p.find("只提文本里真实出现的东西") != std::string::npos);
    CHECK(p.find("统一用出现得最多的那个") != std::string::npos);
    // after 抄不到原句时那条钩子被静默丢掉，schema 拦不住
    CHECK(p.find("一字不差") != std::string::npos);
    CHECK(p.find("不要把省略号当成情节") != std::string::npos);
    // 正文本身要带过去
    CHECK(p.find("ch01") != std::string::npos);

    // 「不要写长相」留在表里：**字段描述只管得住那一栏**，而长相会写进
    // summary（下面「提示词：写大纲」那条上记着实测结论）。原著里本来就
    // 写着长相，读这一步顺手就抄过来了。
    CHECK(p.find("不要写长相") != std::string::npos);

    // 这几条从表里砍掉了，**但模型照样收得到**——在 schema 那一半里
    CHECK(full.find("chapter_id") != std::string::npos);
    CHECK(full.find("归纳不是摘抄") != std::string::npos);
    // 原来钉的是「一章会切成好几集，别只给章尾那一个」。2026-09-18 按时长
    // 切集那条链拔掉之后那句话成了假话（一章就是一个片段，没有"好几集"），
    // hooks 的描述改成了「最后一个必须是章尾」。钉的东西没变：**多标几个
    // 钩子、并且章尾那一个要认得出来**，这话只在 schema 那一半里。
    CHECK(full.find("最后一个必须是章尾") != std::string::npos);
    CHECK(p.find("最后一个必须是章尾") == std::string::npos);
    // 「不要改写正文、不要续写」那一条：**靠 schema 的形状**，不靠措辞
    // ——输出里根本没有正文那一类字段，而且多编一个键语法层就过不去。
    CHECK(full.find("additionalProperties") != std::string::npos);
}

TEST_CASE("人物表里要有「他说话什么样」") {
    // **2026-09-12 加的，因为所有人说话都一个腔调。** 人物表里有身份、
    // 欲望、弧光，唯独没有「怎么开口」，于是正文里每个人的台词都像同一个
    // 人写的——而对白是电影最主要的东西。
    const auto& cs = outline_schema().at("properties").at("characters").at("items");
    REQUIRE(cs.at("properties").contains("voice"));
    bool required = false;
    for (const auto& r : cs.at("required")) {
        if (r == "voice") required = true;
    }
    CHECK(required);
    // **required 只保证键在，不保证有内容。** 2026-09-12 实跑，加进
    // required 的头一轮三个人物的 voice 全是空串——空字符串是合法的
    // JSON 字符串，语法采样照样让它过，那一轮的改动一个字都没生效。
    CHECK(cs.at("properties").at("voice").at("minLength").get<int>() >= 6);

    // **怕什么和要什么一样重要**：人物的核心驱动力往往不是欲望而是恐惧，
    // 90% 的人设翻车死于「全能感」。（原话在 models/story.hpp 的 fear 那一栏。）
    REQUIRE(cs.at("properties").contains("fear"));
    CHECK(cs.at("properties").at("fear").at("minLength").get<int>() >= 6);
    bool fear_required = false;
    for (const auto& r : cs.at("required")) {
        if (r == "fear") fear_required = true;
    }
    CHECK(fear_required);
    // 挨着 want 填，模型才会让它们互相顶着
    std::vector<std::string> ck;
    for (auto it = cs.at("properties").begin(); it != cs.at("properties").end();
         ++it) {
        ck.push_back(it.key());
    }
    const auto pos = [&](const std::string& k) {
        return std::find(ck.begin(), ck.end(), k) - ck.begin();
    };
    CHECK(pos("fear") == pos("want") + 1);

    // 读故事那一步用的是同一份人物块，两边不一致的话每个消费者都要分支
    CHECK(changji::stages::analyze_schema().at("properties").at("characters") ==
          outline_schema().at("properties").at("characters"));
}

TEST_CASE("schema：每一章都要说清抖出什么") {
    // **2026-09-12 加的，因为四章零反转。** 实跑的大纲是「前任回来 → 打
    // 电话 → 坦白 → 和解」：每章都在推进，但没有一章让人重新理解前面发生
    // 过的事。卖座的电影每隔几章抖出一个身份/关系/事实/动机的反转，
    // 网文那边叫「信息差」。措辞 14B 不一定听，进 required 它才没得选。
    const auto& ch = outline_schema().at("properties").at("chapters").at("items");
    REQUIRE(ch.at("properties").contains("reveal"));

    bool required = false;
    for (const auto& r : ch.at("required")) {
        if (r == "reveal") required = true;
    }
    CHECK(required);

    // **排在 summary 前面。** 先定抖什么，那几句梗概才会围着它写；反过来
    // 它会先把梗概写完，再回头凑一个「反转」，凑出来的是同一件事换个说法。
    std::vector<std::string> keys;
    const auto& props = ch.at("properties");
    for (auto it = props.begin(); it != props.end(); ++it) keys.push_back(it.key());
    const auto at = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(at("reveal") < at("summary"));
}

TEST_CASE("schema：人物那三块和大纲那份长一样") {
    const auto& a = changji::stages::analyze_schema().at("properties");
    const auto& o = outline_schema().at("properties");
    // 下游认的是同一个形状，两边不一致的话每个消费者都要分支
    CHECK(a.at("characters") == o.at("characters"));
    CHECK(a.at("relations") == o.at("relations"));
    CHECK(a.at("locations") == o.at("locations"));
    // 章名不给模型改——那是作者自己写的
    CHECK_FALSE(a.at("chapters").at("items").at("properties").contains("title"));
    // **hooks 这一栏要在，而且每条都带 after。** after 是唯一的定位手段：
    // 模型报不准字符偏移，只能抄一句原文回来、程序自己去正文里查
    // （为什么见 chapter_write.hpp 的 DraftHook::after）。查出来的位置
    // story_analyze 拿去和已有候选对位，对不上才新加一条。
    // 最后那一条是章尾：写剧本那一步在场的 turn 都空着时，往回取它当这一
    // 章的收口（script_story.cpp）。**别再拿「前面几集收在哪」当理由**
    // ——按时长切集那条链 2026-09-18 拔掉了。
    const auto& ch = a.at("chapters").at("items").at("properties");
    CHECK(ch.contains("hooks"));
    CHECK(ch.at("hooks").at("type") == "array");
    CHECK(ch.at("hooks").at("items").at("properties").contains("after"));
}

TEST_CASE("schema：正文一场一个数组，场数和段数都由语法卡住") {
    // 2026-09-11 实跑：正文只是一个 text 字符串时，14B 三章里两章把梗概原样
    // 抄进去就收工（一百来字），另一次写到 8192 token 都没收口。「写满三千
    // 字」它不听，进 schema 变成语法约束它才没得选。
    //
    // 2026-09-12 再改一层：**正文按场分组**。没有「场」这个单位的时候，
    // 模型把整章梗概平摊成四十个一句话的段落，通篇是概述不是场景——
    // 用户的判词是「只能叫剧本不能叫小说」。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& props = s.at("properties");
    CHECK_FALSE(props.contains("text"));
    CHECK_FALSE(props.contains("paragraphs"));  // 正文不在顶层了
    REQUIRE(props.contains("scenes"));
    // **下限就是目标值。** 让它少写一场，那一场会长到一千八百字，收口只能
    // 落在场中间说不出为什么的地方（2026-09-12 实跑）。
    CHECK(props.at("scenes").at("minItems").get<int>() == 3);
    CHECK(props.at("scenes").at("maxItems").get<int>() == 4);

    const auto& scene = props.at("scenes").at("items");
    const auto& sp = scene.at("properties");
    // **一场戏要素齐全**：在哪、跟谁走、要什么、谁拦着、局面变成什么。
    for (const char* k : {"where", "pov", "goal", "obstacle", "turn"}) {
        CHECK(sp.contains(k));
    }
    // **turn 排在正文前面**：先知道这一场停在哪，才写得到那儿去。
    auto keys = std::vector<std::string>();
    for (auto it = sp.begin(); it != sp.end(); ++it) keys.push_back(it.key());
    const auto pos = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(pos("where") < pos("paragraphs"));
    CHECK(pos("turn") < pos("paragraphs"));

    REQUIRE(sp.contains("paragraphs"));
    CHECK(sp.at("paragraphs").at("type") == "array");
    // 下限是目标段数的一半。**别再往上提**：2026-09-12 提到四分之三试过，
    // 段长是降下来了，但模型没话说时语法不让它停，会反复尝试跳到下一个
    // 字段、把 token 烧光、JSON 没收尾，整章落成 0 字。
    // 整个来回见 chapter_write.cpp 里 min_items 那段注释。
    CHECK(sp.at("paragraphs").at("minItems").get<int>() >= 8);
    CHECK(sp.at("paragraphs").at("minItems").get<int>() <= 12);
    CHECK(sp.at("paragraphs").at("maxItems").get<int>() <= 34);
    // 段长的上下限。**maxLength 不是段长的旋钮**——2026-09-12 把它从 300
    // 压到 150 试过，段长中位纹丝不动（78 → 83），只削掉长尾，而且真卡住
    // 时会把句子从中间切断。它的活儿是拦住病态的千字长段。塑形靠段数。
    CHECK(sp.at("paragraphs").at("items").at("maxLength").get<int>() >= 300);
    // **下限要低到放得进一句短对白。** 2026-09-12 晚之前是 20，而
    // 「"别碰。"」只有 5 个字——语法不让它收引号，模型只能挂个动作或体感
    // 上去凑长度。实测 319 段里不到 20 字的只有 1.6%，20~29 字那一档 52 段，
    // 20 处是一堵墙；而真实网文段长中位 33、大量是短对白。
    // 别再为了"拧字数"把它提上去：字数不是质量信号。
    CHECK(sp.at("paragraphs").at("items").at("minLength").get<int>() <= 6);
    CHECK(sp.at("paragraphs").at("items").at("minLength").get<int>() >= 2);
    CHECK(sp.at("paragraphs").at("items").at("type") == "string");
    REQUIRE(s.at("required").size() == 1);
    CHECK(s.at("required")[0] == "scenes");

    // 目标再小，下限也不会低到能一段交差、一场交差
    const auto tiny = changji::stages::chapter_schema(1, 2);
    CHECK(tiny.at("properties").at("scenes").at("minItems").get<int>() >= 2);
    CHECK(tiny.at("properties").at("scenes").at("items").at("properties")
              .at("paragraphs").at("minItems").get<int>() >= 6);
}

TEST_CASE("并回去：每章那两份名单是重填，不是往上堆") {
    // **2026-09-11 实跑出来的样子**：走完"出大纲 → 写正文 → 读故事"，
    // 某一章的出场人物是 `['陈默','林景明','陈默','林景明','苏婉']`，
    // 地点那份还混着同一个地方的两种叫法。
    //
    // 两条原因：大纲那一步已经往里写过一份，而这儿只 push 不 clear；
    // 模型自己也会把同一个名字写两遍。
    //
    // 钉的是**勾上「覆盖已有」那一档**（2026-09-19 分出两档之后）：这一栏
    // 只有两种下场，整份换掉或者一个字不动，往上堆是第三种，那正是上面
    // 两种重复的来路。不勾那一档在下面「只补不顶」那一组。
    Story s = pasted_story();
    // 假装大纲那一步已经填过（真实流程就是这样）
    s.chapters[0].characters = {"林晚", "旧的名字"};
    s.chapters[0].locations = {"便利店", "旧的地方"};

    json j = good_analysis();
    // 模型把同一个人写了两遍——它真会这么干
    j["chapters"][0]["characters"] = json::array({"林晚", "林晚"});
    j["chapters"][0]["locations"] = json::array({"便利店", "便利店"});

    const Story got = changji::stages::apply_analysis(s, j.dump(), true);
    CHECK(got.chapters[0].characters == std::vector<std::string>{"林晚"});
    CHECK(got.chapters[0].locations == std::vector<std::string>{"便利店"});
}

TEST_CASE("并回去：模型没给关系时不要把已有的抹掉") {
    // relations 一度不在 required 里，模型给的是空数组，而这儿会拿它盖掉
    // 大纲写好的那几条——界面上看着像"这个故事没有人物关系"。
    // schema 那条已经补上了（见「地点和出场人要写进 required」），
    // 这一条守的是并回去这一步：给了空数组，至少别比原来更糟。
    Story s = pasted_story();
    Relation r;
    r.a = "林晚";
    r.b = "他";
    r.kind = "前任";
    r.tension = "五年前那把伞";
    s.relations.push_back(r);

    json j = good_analysis();
    j["relations"] = json::array();   // 模型什么都没给
    const Story got = changji::stages::apply_analysis(s, j.dump(), true);
    // 空数组进来时，两端还在不在人物表里都无从判断——这里只钉一件事：
    // 结果不该比原来更糟。
    CHECK(got.relations.size() <= s.relations.size());
}

// ---------------------------------------------------------------------------
// 不勾「覆盖已有」那一档：只补不顶。
//
// 用户 2026-09-19：「第一章的梗概被顶掉这个不行，改成不勾就不动已有的章」。
// 走法是新建项目 → 写第一章 → 理解故事 → 再写一章 → 再点理解故事。第二下
// 本该只去读新写的那章，而原来这儿整本重落。
//
// 这一组钉的都是"第二下"：`already_read()` 就是第一下跑完、外加人手改过
// 几笔的样子。
// ---------------------------------------------------------------------------

namespace {

/// 第一次「理解故事」跑完的样子：ch01 读过了（还被人手改过几笔），
/// ch02 是刚写出正文、还没读过的那章。
Story already_read() {
    Story s = pasted_story();
    s.logline = "第一次读出来的一句话";

    StoryCharacter c;
    c.name = "林晚";
    c.identity = "手改过的身份";   // 人在设定页上改的
    c.want = "手改过的欲望";
    s.characters.push_back(c);

    StoryLocation l;
    l.name = "便利店";
    l.what = "手改过的说明";
    s.locations.push_back(l);

    Relation r;
    r.a = "林晚";
    r.b = "他";
    r.kind = "前任";
    r.tension = "第一次读出来的那根弦";
    s.relations.push_back(r);
    StoryCharacter him;
    him.name = "他";
    s.characters.push_back(him);

    s.chapters[0].summary = "第一次读出来的梗概，人还改过";
    s.chapters[0].characters = {"林晚"};
    s.chapters[0].locations = {"便利店"};
    Hook h;
    h.at_char = 3;
    h.text = "第一次读出来的钩子";
    s.chapters[0].hooks.push_back(h);

    // 新写的那章：正文有了，读出来的那几栏还空着。钩子那儿躺着一条
    // story_reverse 机械登记的段落边界候选——只有位置，没有说法。
    s.chapters[1].summary.clear();
    s.chapters[1].characters.clear();
    s.chapters[1].locations.clear();
    Hook cand;
    cand.at_char = 0;
    s.chapters[1].hooks.push_back(cand);
    return s;
}

/// 第二下模型回来的东西：ch01 它照样给了一份（发过去的就是全本），
/// ch02 是这一下真要的。
json second_read() {
    json j = good_analysis();
    j["logline"] = "模型这一趟重写的一句话";
    j["chapters"][0]["summary"] = "模型这一趟重写的梗概";
    j["chapters"][0]["characters"] = json::array({"林晚", "他"});
    j["chapters"][0]["locations"] = json::array({"五年前的街"});
    j["chapters"][1]["characters"] = json::array({"他", "林晚"});
    j["chapters"][1]["locations"] = json::array({"五年前的街"});
    // **改已有那条，别再 push 一条同名的**：同名会被去重丢掉，两档都读不到
    // 这个身份，勾上那条对照就变成了假绿。
    j["characters"][0]["identity"] = "模型这一趟给的身份";
    j["characters"][0]["want"] = "模型这一趟给的欲望";
    j["characters"].push_back(json{{"name", "他"}, {"identity", "打伞的那个"}});
    j["locations"].push_back(
        json{{"name", "五年前的街"}, {"what", "下着雨"}, {"when", "夜里"}});
    j["relations"] = json::array({
        // 同一条边反着写：不判无序对的话，每读一遍就多出一条
        json{{"a", "他"}, {"b", "林晚"}, {"kind", "前任"}},
    });
    return j;
}

}  // namespace

TEST_CASE("只补不顶：读过的那一章，梗概、出场人、地点一个字不动") {
    const Story s = already_read();
    const Story got =
        changji::stages::apply_analysis(s, second_read().dump(), false);

    // 用户那一句就是这一行
    CHECK(got.chapters[0].summary == "第一次读出来的梗概，人还改过");
    CHECK(got.chapters[0].characters == std::vector<std::string>{"林晚"});
    CHECK(got.chapters[0].locations == std::vector<std::string>{"便利店"});

    // 钩子也不动：`put` 那条"有说法的不覆盖"挡得住改写，挡不住**新加**，
    // 而钩子一变 commit_story 就会重算章节计划。
    //
    // 钉"和进来时一模一样"而不是钉条数——粘贴导入本来就给每章铺了一排段落
    // 边界候选（`story_import::paragraph_hooks`，说法全是空的）。
    REQUIRE(got.chapters[0].hooks.size() == s.chapters[0].hooks.size());
    for (std::size_t i = 0; i < got.chapters[0].hooks.size(); ++i) {
        CHECK(got.chapters[0].hooks[i].at_char == s.chapters[0].hooks[i].at_char);
        CHECK(got.chapters[0].hooks[i].text == s.chapters[0].hooks[i].text);
    }
    CHECK(std::any_of(got.chapters[0].hooks.begin(), got.chapters[0].hooks.end(),
                      [](const Hook& h) { return h.text == "第一次读出来的钩子"; }));

    // 故事级那几栏同理：同名的人保留手改过的那条，一句话梗概也不换
    CHECK(got.logline == "第一次读出来的一句话");
    const auto* lin = [&]() -> const StoryCharacter* {
        for (const auto& c : got.characters) {
            if (c.name == "林晚") return &c;
        }
        return nullptr;
    }();
    REQUIRE(lin != nullptr);
    CHECK(lin->identity == "手改过的身份");
    CHECK(lin->want == "手改过的欲望");
}

TEST_CASE("只补不顶：新写出来的那一章，该补的全补上") {
    // 不动已有的章**不等于什么都不干**——这一下就是为新写的那章来的。
    const Story s = already_read();
    const Story got =
        changji::stages::apply_analysis(s, second_read().dump(), false);

    CHECK(got.chapters[1].summary == "回到五年前。");
    CHECK(got.chapters[1].characters == std::vector<std::string>{"他", "林晚"});
    CHECK(got.chapters[1].locations ==
          std::vector<std::string>{"五年前的街"});

    // 机械候选那一条只有位置没说法，算"还没人给过说法"，该填
    const bool told = std::any_of(
        got.chapters[1].hooks.begin(), got.chapters[1].hooks.end(),
        [](const Hook& h) { return !h.text.empty(); });
    CHECK(told);

    // 新出现的人和地方追加进全片名单，旧的一个不少
    const auto has = [](const auto& v, const std::string& n) {
        return std::any_of(v.begin(), v.end(),
                           [&](const auto& x) { return x.name == n; });
    };
    CHECK(has(got.characters, "林晚"));
    CHECK(has(got.characters, "他"));
    CHECK(has(got.locations, "便利店"));
    CHECK(has(got.locations, "五年前的街"));
}

TEST_CASE("只补不顶：同一条关系反着写，不许攒成两条") {
    // 「前任」这条边 a→b 和 b→a 是同一条。按有序对收的话，每点一次
    // 理解故事就多出一条反着写的，分镜提示词里这段关系会出现两遍。
    const Story s = already_read();
    const Story got =
        changji::stages::apply_analysis(s, second_read().dump(), false);

    int n = 0;
    for (const auto& r : got.relations) {
        if ((r.a == "林晚" && r.b == "他") || (r.a == "他" && r.b == "林晚")) {
            ++n;
            // 留下的得是**旧的那条**。只数条数的话，"清空再填"和"合并"
            // 给出的都是 1，这条用例就什么都没测。
            CHECK(r.tension == "第一次读出来的那根弦");
        }
    }
    CHECK(n == 1);
}

TEST_CASE("只补不顶：那一栏空着的时候照样填") {
    // 判据是**逐栏**的，不是整章一刀切：新写出正文的那一章往往梗概已经
    // 有了（大纲阶段写的），而出场人、地点还空着——那几栏正是要补的。
    Story s = already_read();
    s.chapters[0].locations.clear();   // 就这一栏空着
    s.logline.clear();                 // 故事级那几栏也是逐栏各判各的

    const Story got =
        changji::stages::apply_analysis(s, second_read().dump(), false);
    CHECK(got.chapters[0].summary == "第一次读出来的梗概，人还改过");  // 还是不动
    CHECK(got.chapters[0].locations ==
          std::vector<std::string>{"五年前的街"});                    // 这栏填上
    CHECK(got.logline == "模型这一趟重写的一句话");                    // 这栏也填上
}

TEST_CASE("勾上就是全部重出：同一份输入，第一章整份换掉") {
    // 两档得真的不一样，不然「覆盖已有」那个勾就是个摆设。
    const Story s = already_read();
    const Story got =
        changji::stages::apply_analysis(s, second_read().dump(), true);

    CHECK(got.chapters[0].summary == "模型这一趟重写的梗概");
    CHECK(got.chapters[0].characters == std::vector<std::string>{"林晚", "他"});
    CHECK(got.chapters[0].locations == std::vector<std::string>{"五年前的街"});
    CHECK(got.logline == "模型这一趟重写的一句话");
    for (const auto& c : got.characters) {
        if (c.name == "林晚") CHECK(c.identity == "模型这一趟给的身份");
    }
}

TEST_CASE("只补不顶：模型一个人都没读出来，照样要报错") {
    // **守卫钉的是"这一趟读出来几个人"，不是合并完剩几个。** 不勾那一档
    // 旧名单本来就在，拿合并后的数去判的话，模型返回空数组也能蒙混过去
    // ——而那正是这条守卫要抓的（读了一遍等于没读）。
    const Story s = already_read();
    json j = second_read();
    j["characters"] = json::array();
    CHECK_THROWS_AS(changji::stages::apply_analysis(s, j.dump(), false),
                    changji::stages::StoryError);
}

TEST_CASE("并回去：正文一个字不动，钩子落在那句话后面") {
    const Story s = pasted_story();
    const std::string before = s.chapters[0].text;

    const Story got =
        changji::stages::apply_analysis(s, good_analysis().dump(), true);

    // 正文、章名、章号照旧
    CHECK(got.chapters[0].text == before);
    CHECK(got.chapters[0].title == s.chapters[0].title);
    CHECK(got.chapters[0].chapter_id == "ch01");

    // 结构补上了
    CHECK(got.logline == "一把伞牵出五年前的事");
    REQUIRE(got.characters.size() == 1);
    CHECK(got.characters[0].name == "林晚");
    CHECK(got.chapters[0].summary == "他带着伞回来了。");

    // 钩子定位：落在 hook_after 那句话**之后**
    bool found = false;
    for (const auto& h : got.chapters[0].hooks) {
        if (h.text != "她认出那把伞") continue;
        found = true;
        const auto chars = changji::text::utf8_chars(got.chapters[0].text);
        REQUIRE(h.at_char > 0);
        REQUIRE(h.at_char <= static_cast<int>(chars.size()));
        CHECK(chars[static_cast<std::size_t>(h.at_char) - 1] == "。");
    }
    CHECK(found);

    CHECK(got.validate().empty());
}

TEST_CASE("并回去：模型糊弄时的几种情况") {
    const Story s = pasted_story();

    SUBCASE("after 查不到——那一条丢掉，不能都堆到章尾") {
        // 一章有好几个钩子，查不到的全往章尾堆的话，章尾会被一个中间情节的
        // 说法占掉，而这一章的结尾写的就是别处的事。
        json j = good_analysis();
        j["chapters"][0]["hooks"][0]["after"] = "正文里根本没有这句话";
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        for (const auto& h : got.chapters[0].hooks) {
            CHECK(h.text != "她认出那把伞");
        }
        CHECK(got.validate().empty());
    }

    SUBCASE("老形状（单个 hook + hook_after）还能读，查不到时兜底挂章尾") {
        json j = good_analysis_legacy();
        j["chapters"][0]["hook_after"] = "正文里根本没有这句话";
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        bool found = false;
        for (const auto& h : got.chapters[0].hooks) {
            if (h.text == "她认出那把伞") {
                found = true;
                CHECK(h.at_char == got.chapters[0].text_len());
            }
        }
        CHECK(found);
        CHECK(got.validate().empty());
    }

    SUBCASE("一章标好几个钩子，各就各位") {
        json j = good_analysis();
        j["chapters"][0]["hooks"] = json::array({
            json{{"text", "他进来了"}, {"after", "他推门进来，伞还在手里。"}},
            json{{"text", "她认出那把伞"}, {"after", "林晚认出了那把伞。"}},
        });
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        int named = 0;
        for (const auto& h : got.chapters[0].hooks) {
            if (!h.text.empty()) ++named;
        }
        // 两个都落下去了——这正是「12 集只有 3 集停在真悬念上」要修的地方
        CHECK(named == 2);
        CHECK(got.validate().empty());
    }

    SUBCASE("编了个不存在的章号——跳过，别把它当新章") {
        json j = good_analysis();
        j["chapters"].push_back(json{{"chapter_id", "ch99"},
                                     {"summary", "查无此章"},
                                     {"hooks", json::array()}});
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        CHECK(got.chapters.size() == s.chapters.size());
        CHECK(got.validate().empty());
    }

    SUBCASE("章节里冒出没登记的人——过滤掉") {
        json j = good_analysis();
        j["chapters"][0]["characters"].push_back("查无此人");
        const Story got = changji::stages::apply_analysis(s, j.dump(), true);
        for (const auto& n : got.chapters[0].characters) {
            CHECK(n == "林晚");
        }
    }

    SUBCASE("一个人都没读出来——报错，不要产出一份没人的故事") {
        json j = good_analysis();
        j["characters"] = json::array();
        CHECK_THROWS_AS(changji::stages::apply_analysis(s, j.dump(), true),
                        stages::StoryError);
    }
}

/// 理解故事那一次调用的回答：结构（good_analysis）加上长相和样貌。
json good_understanding() {
    json u = good_analysis();
    for (auto& c : u["characters"]) {
        c["key"] = "lin_wan";
        c["body"] = "偏瘦，肩背挺";
        c["face"] = "齐肩黑直发，圆眼，单眼皮";
        c["attire"] = "便利店藏青制服外套";
    }
    int k = 0;
    for (auto& l : u["locations"]) {
        l["key"] = "place_" + std::to_string(k++);
        l["space"] = "临街玻璃门，两排货架";
        l["lighting"] = "夜间冷白顶光";
        l["palette"] = "冷青加一点暖黄";
    }
    u["global_style"] = "夜戏，低饱和，轻微颗粒";
    return u;
}

TEST_CASE("理解故事：一句话提示词，没有规矩表；结构和长相在同一份里") {
    // 用户 2026-09-17：「理解故事本来就不应该给很多提示词……只要让它输出
    // 结构化的内容就行」。规矩表没有了，结构在 schema 里；故事里有两个人
    // 就是两个人，不许硬要三到五个。
    const std::string p =
        changji::stages::build_understand_prompt(pasted_story(), StyleLine::REALISTIC);
    CHECK(p.find("按你的理解补") != std::string::npos);
    CHECK(p.find("雨夜重逢") != std::string::npos);   // 章节在
    CHECK(p.find("硬性要求") == std::string::npos);
    // 章节前面一条禁令都没有；唯一一个「不要」是尾巴上那句"只输出 JSON，
    // 不要任何解释文字"——那是输出格式，不是读法。
    CHECK(p.find("不要") > p.find("章节如下"));
    CHECK(p.find("只输出 JSON") != std::string::npos);

    const json s = changji::stages::understand_schema();
    const std::string dumped = s.dump();
    CHECK(dumped.find("minItems") == std::string::npos);
    CHECK(dumped.find("maxItems") == std::string::npos);
    const auto& cp = s.at("properties").at("characters").at("items").at("properties");
    for (const char* k : {"name", "want", "arc", "key", "body", "face", "attire"}) {
        CAPTURE(k);
        CHECK(cp.contains(k));
    }
    const auto& lp = s.at("properties").at("locations").at("items").at("properties");
    for (const char* k : {"name", "key", "space", "lighting", "palette"}) {
        CAPTURE(k);
        CHECK(lp.contains(k));
    }
    CHECK(s.at("properties").contains("global_style"));
    CHECK(s.at("properties").at("chapters").at("items").at("properties").contains("hooks"));
}

TEST_CASE("POST /api/story/understand_once：一份回答，结构进故事、长相进库") {
    const fs::path root = fresh_project("理解一次");
    ProjectStore store(root);
    store.save_story(pasted_story());

    llm::ReplayClient client({good_understanding().dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_story_understand_once(json{{"project", p_str(root)}}, client, tok);
    CHECK(r.status == 200);
    REQUIRE(client.calls().size() == 1);   // 一次调用，不是两次
    CHECK(client.calls()[0].schema_name == "story_understanding");

    const Story s = store.load_story();
    REQUIRE(s.characters.size() == 1);
    CHECK(s.characters[0].name == "林晚");
    CHECK(s.chapters[0].summary.size() > 0);

    const AssetLibrary lib = store.load_assets();
    REQUIRE(lib.characters.size() == 1);
    CHECK(lib.characters.begin()->second.name == "林晚");
    CHECK(lib.characters.begin()->second.appearance.face == "齐肩黑直发，圆眼，单眼皮");
    CHECK_FALSE(lib.locations.empty());

    SUBCASE("没正文不让读") {
        const fs::path bare = fresh_project("理解没正文");
        ProjectStore(bare).save_story(
            parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM));
        CHECK_THROWS_AS(http::post_story_understand_once(json{{"project", p_str(bare)}},
                                                         client, tok),
                        http::ApiError);
        std::error_code e2;
        fs::remove_all(bare, e2);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/understand_once：第二遍不勾，读过的那章一个字不动") {
    // 用户 2026-09-19 的走法：新建项目 → 写第一章 → 理解故事 → 再写一章 →
    // 再点理解故事。**第二下本该只去读新写的那章。**
    //
    // 上面那一组用例钉的是 `apply_analysis` 这个纯函数，这一条钉的是**那个
    // 勾真的从接口传到了它**——中间少传一个参数，纯函数那边再对也白搭。
    const fs::path root = fresh_project("理解两遍");
    ProjectStore store(root);
    store.save_story(pasted_story());
    pipeline::CancelToken tok;

    // 第一遍：读出来了
    llm::ReplayClient c1({good_understanding().dump()});
    http::post_story_understand_once(json{{"project", p_str(root)}}, c1, tok);
    REQUIRE_FALSE(store.load_story().chapters[0].summary.empty());

    // 人回到故事页上手改了一笔
    {
        Story s = store.load_story();
        s.chapters[0].summary = "人手改过的梗概";
        store.save_story(s);
    }

    // 第二遍（写了新的一章之后）：模型这一趟给的是另一份
    json again = good_understanding();
    again["chapters"][0]["summary"] = "模型第二趟重写的梗概";
    llm::ReplayClient c2({again.dump()});
    http::post_story_understand_once(
        json{{"project", p_str(root)}, {"overwrite", false}}, c2, tok);
    CHECK(store.load_story().chapters[0].summary == "人手改过的梗概");

    SUBCASE("勾上「覆盖已有」才换") {
        llm::ReplayClient c3({again.dump()});
        http::post_story_understand_once(
            json{{"project", p_str(root)}, {"overwrite", true}}, c3, tok);
        CHECK(store.load_story().chapters[0].summary == "模型第二趟重写的梗概");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/understand：一件活，读一遍全出来") {
    // 设定页只有一颗「理解故事」：提结构 → 定长相 → 章对集 → 逐章写剧本
    // → 逐章拆分镜（最后这一步 2026-09-19 并进来的，见 batch.cpp 顶上
    // 那段"还缺什么"）。
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();

    const fs::path root = fresh_project("理解故事");
    ProjectStore store(root);
    Story s = pasted_story();
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);
    http::sync_episodes_to_chapters(store, s);   // 章对集：剧本那一步按它挑

    // 读那一趟的提示词，只该出现在第一趟里
    const auto analyzed = [](const std::shared_ptr<llm::ReplayClient>& c) {
        int n = 0;
        for (const auto& call : c->calls()) {
            if (call.prompt.find("按你的理解补") != std::string::npos) ++n;
        }
        return n;
    };

    // 剧本那两条回的是坏的：一章写砸不拖垮整件活（同 script/all 的规矩）
    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        good_understanding().dump(), "这不是 JSON", "这不是 JSON", "这不是 JSON",
        "这不是 JSON"});
    const auto r = http::post_story_understand(json{{"project", p_str(root)}}, client);
    CHECK(r.status == 202);
    CHECK(r.body.at("started") == true);
    // 读一遍 + 两章剧本 + 那两章的分镜。**这是个预估**：两章剧本待会儿都
    // 会写砸（回的是坏 JSON），写砸就拆不了分镜，跑到分镜那一步总数会收到
    // 真实值 3——下面 `done == total` 盯的就是收没收。
    CHECK(r.body.at("total").get<int>() == 5);
    pipeline::jobs().wait_idle();

    const json snap = pipeline::jobs().snapshot(pipeline::JobKind::Write);
    CHECK(snap.at("running") == false);
    CHECK_MESSAGE(snap.at("error").is_null(), snap.dump());
    CHECK(snap.at("done") == snap.at("total"));
    // 结构进了故事，长相进了库
    CHECK_FALSE(store.load_story().characters.empty());
    CHECK_FALSE(store.load_assets().characters.empty());
    CHECK(analyzed(client) == 1);

    SUBCASE("再点一次不重读：只补缺的剧本") {
        auto again = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
            "这不是 JSON", "这不是 JSON", "这不是 JSON", "这不是 JSON"});
        const auto r2 = http::post_story_understand(json{{"project", p_str(root)}}, again);
        CHECK(r2.body.at("total").get<int>() == 4);   // 两章剧本 + 那两章的分镜
        pipeline::jobs().wait_idle();
        CHECK(analyzed(again) == 0);
    }

    SUBCASE("overwrite 才重来") {
        auto again = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
            good_understanding().dump(), "这不是 JSON", "这不是 JSON", "这不是 JSON",
            "这不是 JSON"});
        const auto r2 = http::post_story_understand(
            json{{"project", p_str(root)}, {"overwrite", true}}, again);
        CHECK(r2.body.at("total").get<int>() == 5);
        pipeline::jobs().wait_idle();
        CHECK(analyzed(again) == 1);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("正文指纹：没正文是空串，改一个字就变") {
    // 「理解故事」里读那一趟靠它决定要不要重读（Project::understood_from），
    // 镜头页那块「照旧剧本拆的」牌子也靠它（Episode::shots_from，只是算在
    // 剧本上）。**空的一段必须得到空串**：老项目没有那两个键，读出来也是
    // 空串，两边对得上才不会在升级那一刻全项目一起误判。
    CHECK(chapter_text_fingerprint("") == "");
    CHECK(chapter_text_fingerprint("   \n\t ") == "");
    CHECK_FALSE(chapter_text_fingerprint("他推门进来。").empty());
    // 前后空白不算改过：编辑器落一个尾空行不该让整本书重读一遍
    CHECK(chapter_text_fingerprint("他推门进来。") ==
          chapter_text_fingerprint("  他推门进来。\n"));
    CHECK(chapter_text_fingerprint("他推门进来。") !=
          chapter_text_fingerprint("他推门进来了。"));

    Story a = pasted_story();
    CHECK_FALSE(story_text_fingerprint(a).empty());

    SUBCASE("加一章空大纲章不算改：读的是正文") {
        Story b = a;
        Chapter blank;
        blank.chapter_id = "ch03";
        blank.title = "第三章";
        blank.summary = "还没写";
        b.chapters.push_back(blank);
        CHECK(story_text_fingerprint(b) == story_text_fingerprint(a));
    }

    SUBCASE("改一章正文就算改") {
        Story b = a;
        b.chapters[1].text += "雨停了。";
        CHECK(story_text_fingerprint(b) != story_text_fingerprint(a));
    }

    SUBCASE("两章正文对调也算改") {
        Story b = a;
        std::swap(b.chapters[0].text, b.chapters[1].text);
        CHECK(story_text_fingerprint(b) != story_text_fingerprint(a));
    }

    SUBCASE("一个字都没写：空串") {
        Story b;
        Chapter blank;
        blank.chapter_id = "ch01";
        b.chapters.push_back(blank);
        CHECK(story_text_fingerprint(b) == "");
    }
}

namespace {

/// 摆一部「读过了、八章都有剧本」的片子出来——用户 2026-09-19 那部的形状
/// （他那部是八章、七章还没正文、五章还没分镜）。
///
/// `scripts` / `shots` 说这两章各自有没有。`blank_second` 把第二章的正文
/// 抹掉：那才是他那部片子真正的形状，也是「一个字没写的章哪来的剧本和
/// 分镜」那一条的前提。
fs::path understood_project(const std::string& tag, bool scripts, bool shots,
                            bool blank_second = false) {
    const fs::path root = fresh_project(tag);
    ProjectStore store(root);
    Story s = pasted_story();
    if (blank_second) {
        s.chapters[1].text.clear();
        // 钩子和场记的都是正文里的字符位置，正文没了它们也不能留——留着
        // 存盘那一步会拦（「钩子位置 12 超出正文范围 0..0」）。
        s.chapters[1].hooks.clear();
        s.chapters[1].scenes.clear();
        s.chapters[1].summary = "回到五年前那个雨夜。";   // 只剩大纲那三五句
    }
    // 人物表非空 = 已经理解过
    StoryCharacter who;
    who.name = "林晚";
    who.identity = "便利店夜班店员";
    s.characters.push_back(who);
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);
    http::sync_episodes_to_chapters(store, s);

    Project p = store.load_project();
    for (Episode& ep : p.episodes) {
        if (scripts) ep.script = "场景一 便利店 夜\n旧稿 · " + ep.episode_id + "。";
        if (shots) {
            Shot sh;
            sh.shot_id = ep.episode_id + "_s01_sh001";
            sh.scene_id = ep.episode_id + "_s01";
            ep.shots.push_back(sh);
        }
    }
    p.understood_from = story_text_fingerprint(s);
    store.save_project(p);

    // **设定库也要有人。** 拆分镜那一步（post_plan）在角色设定空着的时候
    // 会先跑一趟定妆再拆——设定库空着的话，用例数到的是那一趟，不是分镜。
    AssetLibrary assets;
    Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "便利店夜班店员";
    assets.characters["c_lin_wan"] = lin;
    assets.style.global_style = "夜戏，低饱和";
    assets.style.aspect_ratio = "16:9";
    store.save_assets(assets);
    return root;
}

/// 录下的这几次调用里，读故事那一趟有几次。
int read_calls(const std::shared_ptr<llm::ReplayClient>& c) {
    int n = 0;
    for (const auto& call : c->calls()) {
        if (call.schema_name == "story_understanding") ++n;
    }
    return n;
}

/// 录下的这几次调用里，写剧本有几次。
int script_calls(const std::shared_ptr<llm::ReplayClient>& c) {
    int n = 0;
    for (const auto& call : c->calls()) {
        if (call.schema_name == "script") ++n;
    }
    return n;
}

/// 录下的这几次调用里，拆分镜有几次。
int storyboard_calls(const std::shared_ptr<llm::ReplayClient>& c) {
    int n = 0;
    for (const auto& call : c->calls()) {
        if (call.schema_name == "storyboard") ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("理解故事：不勾就只补缺，缺的包括分镜") {
    // 用户 2026-09-19：八章的片子只写了第一章正文，点「理解故事」——账上
    // 两条 `理解故事 · 8 章 / done 8/8 / 0.0 秒 / 理解完了`，页面一动不动。
    // 那部片子八章都有剧本（凭大纲写的），但 **ep04–ep08 一个分镜都没有**
    // ——那一下本该去补那五章的分镜，而这一步当时根本不在这颗按钮里。
    //
    // 定下来的规矩（用户当天）：**不勾「覆盖已有」就只补缺**，没剧本的写
    // 剧本、没分镜的拆分镜，已经有的一个字不动；勾上才全部重出。
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();

    SUBCASE("剧本有、分镜缺：只拆分镜，不重写剧本") {
        const fs::path root = understood_project("理解缺分镜", true, false);
        ProjectStore store(root);
        const std::string ep01_was = store.load_project().episodes[0].script;

        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"这不是 JSON", "这不是 JSON"});
        const auto r = http::post_story_understand(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("total").get<int>() == 2);   // 两章分镜，不读也不写剧本
        pipeline::jobs().wait_idle();
        CHECK(read_calls(client) == 0);
        CHECK(script_calls(client) == 0);
        CHECK(storyboard_calls(client) == 2);
        CHECK(store.load_project().episodes[0].script == ep01_was);
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("剧本缺：先写剧本，写出来的那一章接着拆分镜") {
        const fs::path root = understood_project("理解缺剧本", false, false);
        auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
            // 两章剧本都写砸——**写砸的章不该跟着去拆分镜**（没剧本拆不了）
            "这不是 JSON", "这不是 JSON"});
        const auto r = http::post_story_understand(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("total").get<int>() == 4);   // 两章剧本 + 两章分镜
        pipeline::jobs().wait_idle();
        CHECK(script_calls(client) == 2);
        CHECK(storyboard_calls(client) == 0);        // 一份剧本都没写成
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("什么都不缺：不起活，也不报错") {
        // 原来这儿会起一件活、0 秒跑完报「理解完了」——从人那头看和
        // 「这颗按钮坏了」一模一样。
        const fs::path root = understood_project("理解啥都不缺", true, true);
        auto idle = std::make_shared<llm::ReplayClient>(std::vector<std::string>{});
        const auto r = http::post_story_understand(json{{"project", p_str(root)}}, idle);
        CHECK(r.status == 200);
        CHECK(r.body.at("started") == false);
        CHECK(r.body.at("blank_chapters").get<int>() == 0);
        CHECK(idle->calls().empty());
        CHECK_FALSE(pipeline::jobs().running(pipeline::JobKind::Write));
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("剧本有正文却改过：不勾就不动它") {
        // **这一条是防回头的。** 当天先试过一版"正文改过的剧本也算缺"，
        // 用户当场否掉：按下去之前说不出会动哪几章。改正文要重出的话，
        // 勾「覆盖已有」。
        const fs::path root = understood_project("理解改了正文", true, true);
        ProjectStore store(root);
        Story next = store.load_story();
        next.chapters[0].text += "门口的风铃响了一下。";
        store.save_story(next);
        const std::string ep01_was = store.load_project().episodes[0].script;

        // 正文变了，读那一趟照样要重来（三步里只有它按"新不新"算）
        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{good_understanding().dump()});
        const auto r = http::post_story_understand(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("total").get<int>() == 1);   // 只有读那一趟
        pipeline::jobs().wait_idle();
        CHECK(read_calls(client) == 1);
        CHECK(script_calls(client) == 0);            // 剧本一个字不动
        CHECK(storyboard_calls(client) == 0);
        const Project p = store.load_project();
        CHECK(p.episodes[0].script == ep01_was);
        CHECK(p.understood_from == story_text_fingerprint(store.load_story()));
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("勾了「覆盖已有」：读、剧本、分镜全部重出") {
        const fs::path root = understood_project("理解全重出", true, true);
        auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
            good_understanding().dump(), "这不是 JSON", "这不是 JSON"});
        const auto r = http::post_story_understand(
            json{{"project", p_str(root)}, {"overwrite", true}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("total").get<int>() == 5);   // 读 1 + 剧本 2 + 分镜 2
        pipeline::jobs().wait_idle();
        CHECK(read_calls(client) == 1);
        CHECK(script_calls(client) == 2);
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("没正文的那些章一步都不走") {
        // 用户那部片子八章只写了一章。**七章空的一步都不该走**——不重写
        // 剧本，也不拿那几份凭大纲写的旧稿去拆分镜。整段理由在下面
        // 「一个字没写的章…」那条用例上。
        const fs::path root = fresh_project("理解只写了一章");
        ProjectStore store(root);
        Story s = pasted_story();
        s.chapters[1].text = "";           // 第二章还没写
        s.chapters[1].hooks.clear();       // 钩子记的是正文里的位置，跟着走
        s.chapters[1].scenes.clear();
        s.plan = changji::stages::plan_episodes(s, 60.0);
        store.save_story(s);
        http::sync_episodes_to_chapters(store, s);
        {
            Project p = store.load_project();
            REQUIRE(p.episodes.size() == 2);
            for (Episode& ep : p.episodes) ep.script = "凭大纲写的旧稿。";
            store.save_project(p);
        }
        // 没正文的那一章不能带钩子：钩子是正文里的字符位置，落在空章上
        // 校验会拦（「钩子位置 12 超出正文范围 0..0」）。
        json u = good_understanding();
        for (auto& c : u["chapters"]) {
            if (c.value("chapter_id", std::string()) == "ch02") c["hooks"] = json::array();
        }
        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{u.dump(), "这不是 JSON"});
        const auto r = http::post_story_understand(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 202);
        // 读一遍（没读过）+ **写过正文的那一章**的分镜。剧本两章都有，
        // 一个字不动；ch02 没正文，分镜也不给它拆。
        CHECK(r.body.at("total").get<int>() == 2);
        pipeline::jobs().wait_idle();
        CHECK_MESSAGE(pipeline::jobs().snapshot(pipeline::JobKind::Write).at("error").is_null(),
                      pipeline::jobs().snapshot(pipeline::JobKind::Write).dump());
        CHECK(script_calls(client) == 0);
        CHECK(storyboard_calls(client) == 1);
        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("一个字没写的章，剧本和分镜都不给它出") {
    // 用户 2026-09-19：「我一个字没写的章节哪来的剧本和分镜」。
    //
    // 他那部片子八章只写了第一章正文，而 ep02–ep08 各躺着三千多字剧本、
    // 七八十个镜头、五分多钟的片长——全是这几颗批量按钮照**三五句章节
    // 梗概**扩写出来的（写剧本那一步在没正文时退回用梗概，见
    // stages/script_story.cpp 里【这一章】那一段）。人回到那一章，看见的
    // 是一份自己从没写过的戏。
    //
    // 规矩：**没正文的章，这几条路一步都不走**。它缺的是正文，不是剧本。
    // 判据收在 models::chapter_written，四条路各挡一道，这条用例四条都走
    // 一遍——挡漏一条，那一条就能把整章戏凭空写出来。
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();

    SUBCASE("这句话本身：有正文才算数") {
        Story s = pasted_story();
        s.chapters[1].text.clear();
        Episode one;
        one.episode_id = "ep01";
        one.chapter_refs = {"ch01"};
        Episode two;
        two.episode_id = "ep02";
        two.chapter_refs = {"ch02"};
        CHECK(changji::models::chapter_written(s, one));
        CHECK_FALSE(changji::models::chapter_written(s, two));

        // 没挂章的（老项目、手动加的章、预告片）：问这句话不成立，一律
        // false，放不放行由各调用点自己定。
        Episode loose;
        loose.episode_id = "trailer";
        CHECK_FALSE(changji::models::chapter_written(s, loose));

        // 挂着一个故事里没有的章：**不是"有正文"**。这一种要单独钉——
        // 写成"是不是空章"的实现会在这儿答错，而答错的后果是照着一份
        // 不存在的正文往下写。
        Episode ghost;
        ghost.episode_id = "ep09";
        ghost.chapter_refs = {"ch99"};
        CHECK_FALSE(changji::models::chapter_written(s, ghost));
    }

    SUBCASE("理解故事：只给写过正文的那一章写剧本、拆分镜") {
        const fs::path root = understood_project("空章不理解", false, false, true);
        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"这不是 JSON"});
        const auto r = http::post_story_understand(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("total").get<int>() == 2);   // ch01 的剧本 + 它的分镜
        pipeline::jobs().wait_idle();
        CHECK(script_calls(client) == 1);            // 不是 2
        ProjectStore store(root);
        const Episode* ep02 = store.load_project().episode_by_id("ep02");
        REQUIRE(ep02 != nullptr);
        CHECK(ep02->script.empty());
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("勾了「覆盖已有」也不出：那是重出的开关，不是凭空写的开关") {
        const fs::path root = understood_project("空章不覆盖", true, true, true);
        ProjectStore store(root);
        const std::string was = store.load_project().episode_by_id("ep02")->script;
        auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
            good_understanding().dump(), "这不是 JSON", "这不是 JSON"});
        const auto r = http::post_story_understand(
            json{{"project", p_str(root)}, {"overwrite", true}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("total").get<int>() == 3);   // 读 1 + ch01 剧本 1 + ch01 分镜 1
        pipeline::jobs().wait_idle();
        CHECK(script_calls(client) == 1);
        CHECK(store.load_project().episode_by_id("ep02")->script == was);
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("批量改编：空章不在单子上，而且说得出还差几章正文") {
        const fs::path root = understood_project("空章不批量改编", false, false, true);
        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"这不是 JSON"});
        const auto r = http::post_script_all(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 202);
        CHECK(r.body.at("episodes") == json::array({"ep01"}));
        pipeline::jobs().wait_idle();

        // 写过的那一章也有了剧本之后，这颗按钮就没活干了——**而"没活干"
        // 和"都齐了"不是一回事**：还差一章正文。不报这个数的话，界面上
        // 说的是「每一章都已经有剧本了」，人正是为那一章来的。
        {
            ProjectStore store(root);
            Project p = store.load_project();
            p.episode_by_id("ep01")->script = "写好了的一稿。";
            store.save_project(p);
        }
        auto idle = std::make_shared<llm::ReplayClient>(std::vector<std::string>{});
        const auto again = http::post_script_all(json{{"project", p_str(root)}}, idle);
        CHECK(again.status == 200);
        CHECK(again.body.at("started") == false);
        CHECK(again.body.at("blank_chapters").get<int>() == 1);
        CHECK(idle->calls().empty());
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("批量补分镜：盘上躺着的那份梗概剧本也不拆") {
        // **这一条要单独钉。** "没正文就没剧本"挡不住它：这几份剧本是
        // 规矩定下来之前写出来的，有剧本、没正文，判据少问一句就是七八十
        // 个镜头。而「批量补分镜」当时自己抄了一份判据，另外两条加的东西
        // 它没跟上——这条用例同时钉着"三处共用一份"。
        const fs::path root = understood_project("空章不补分镜", true, false, true);
        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"这不是 JSON"});
        // 这一条起得来时回的是 200 + started:true（不是 202，它和另外两条
        // 不一样，见 post_plan_all 末尾那句）。
        const auto r = http::post_plan_all(json{{"project", p_str(root)}}, client);
        REQUIRE(r.status == 200);
        CHECK(r.body.at("started") == true);
        CHECK(r.body.at("episodes") == json::array({"ep01"}));
        pipeline::jobs().wait_idle();
        ProjectStore store(root);
        CHECK(store.load_project().episode_by_id("ep02")->shots.empty());
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("单章改编：挡在源头，400 加一句照着能做的话") {
        // 批量那三条各挡一道，可这一颗按钮在剧本页上就摆着。挡在写剧本
        // 那一步自己身上，新调用点才不用每个都记得先问一句。
        const fs::path root = understood_project("空章不单改", false, false, true);
        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"这不是 JSON"});
        pipeline::CancelToken tok;
        const auto r = http::guard([&] {
            return http::post_script_write(
                json{{"project", p_str(root)}, {"episode_id", "ep02"}, {"premise", ""}},
                *client, tok);
        });
        CHECK(r.status == 400);
        // 话要说到"接下来干什么"：光说"不行"的话，人只能猜。
        const std::string said = r.body.dump();
        CHECK(said.find("还没有正文") != std::string::npos);
        CHECK(said.find("故事页") != std::string::npos);
        CHECK(client->calls().empty());   // 一个字都没发给模型

        // 写过正文的那一章照常发给模型。**判据是"发出去了"**，不是这一趟
        // 的状态码——这儿喂的是一句不是 JSON 的回答（同这一族别的用例），
        // 解析那关照样会 400，而那是另一回事。
        http::guard([&] {
            return http::post_script_write(
                json{{"project", p_str(root)}, {"episode_id", "ep01"}, {"premise", ""}},
                *client, tok);
        });
        CHECK(client->calls().size() == 1);
        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("批量写剧本：判据和「理解故事」那一条是同一份") {
    // 共用 batch.cpp 里那条 script_missing——抄两份的话改一处漏一处，
    // 表现是同一颗按钮在两页上说法不一样。
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();

    SUBCASE("一章都不缺：200 + started false") {
        const fs::path root = understood_project("批量不缺", true, false);
        auto idle = std::make_shared<llm::ReplayClient>(std::vector<std::string>{});
        const auto r = http::post_script_all(json{{"project", p_str(root)}}, idle);
        CHECK(r.status == 200);
        CHECK(r.body.at("started") == false);
        CHECK(idle->calls().empty());
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("正文改过不算缺：要重出得勾「覆盖已有」") {
        const fs::path root = understood_project("批量改了正文", true, false);
        ProjectStore store(root);
        Story next = store.load_story();
        next.chapters[0].text += "门口的风铃响了一下。";
        store.save_story(next);

        auto idle = std::make_shared<llm::ReplayClient>(std::vector<std::string>{});
        const auto r = http::post_script_all(json{{"project", p_str(root)}}, idle);
        CHECK(r.body.at("started") == false);
        CHECK(idle->calls().empty());

        auto client = std::make_shared<llm::ReplayClient>(
            std::vector<std::string>{"这不是 JSON", "这不是 JSON"});
        const auto forced = http::post_script_all(
            json{{"project", p_str(root)}, {"overwrite", true}}, client);
        REQUIRE(forced.status == 202);
        CHECK(forced.body.at("episodes").size() == 2);
        pipeline::jobs().wait_idle();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("POST /api/story/from_web：写好只换这一章") {
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();
    const fs::path root = fresh_project("从网上写");
    ProjectStore store(root);
    store.save_story(pasted_story());   // 两章，都有字
    const Story was = store.load_story();

    // 模型不用工具，直接写；上网那一层给个永远 404 的
    const std::string reply = json{{"title", "雨夜"},
                                   {"source", "百度热搜：某条"},
                                   {"text", "凌晨两点，走廊只亮一半。"}}
                                  .dump();
    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{reply});
    const llm::HttpGet dead = [](const std::string&, const std::map<std::string, std::string>&,
                                 double) { return llm::HttpResponse{404, "", std::nullopt}; };

    SUBCASE("这一章有字：要带 overwrite") {
        CHECK_THROWS_AS(http::post_story_from_web(
                            json{{"project", p_str(root)}, {"chapter_id", "ch02"}}, client, dead),
                        http::ApiError);
        CHECK(client->calls().empty());
    }

    SUBCASE("带了：只换 ch02，ch01 一个字不动") {
        const auto r = http::post_story_from_web(
            json{{"project", p_str(root)}, {"chapter_id", "ch02"}, {"overwrite", true}}, client,
            dead);
        CHECK(r.status == 202);
        pipeline::jobs().wait_idle();
        const json snap = pipeline::jobs().snapshot(pipeline::JobKind::Write);
        CHECK_MESSAGE(snap.at("error").is_null(), snap.dump());

        const Story s = store.load_story();
        REQUIRE(s.chapters.size() == 2);
        CHECK(s.chapters[0].text == was.chapters[0].text);
        CHECK(s.chapters[1].text == "凌晨两点，走廊只亮一半。");
        // 粘进来的章标题「五年前那把伞」不是默认名，不换
        CHECK(s.chapters[1].title == was.chapters[1].title);
        CHECK(store.load_project().episodes.size() == 2);
        // 开场带着前一章的结尾
        CHECK(client->calls()[0].prompt.find("林晚认出了那把伞") != std::string::npos);
    }

    SUBCASE("没这一章：404") {
        CHECK_THROWS_AS(http::post_story_from_web(
                            json{{"project", p_str(root)}, {"chapter_id", "ch09"}}, client, dead),
                        http::ApiError);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/understand：没正文不让理解") {
    // 大纲写出来的故事只有梗概没正文，读一遍读的是空气。
    const fs::path root = fresh_project("没正文别理解");
    ProjectStore store(root);
    store.save_story(parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM));
    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{});
    CHECK_THROWS_AS(http::post_story_understand(json{{"project", p_str(root)}}, client),
                    http::ApiError);
    CHECK(client->calls().empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/analyze：落盘，而且重算了章节计划") {
    const fs::path root = fresh_project("读一遍");
    ProjectStore store(root);
    Story s = pasted_story();
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);

    llm::ReplayClient client({good_analysis().dump()});
    pipeline::CancelToken tok;
    const auto r =
        http::post_story_analyze(json{{"project", p_str(root)}}, client, tok);

    CHECK(r.status == 200);
    CHECK(r.body.at("adopted").get<bool>());
    CHECK_FALSE(r.body.at("needs_analysis").get<bool>());
    CHECK(r.body.at("story").at("characters").size() == 1);
    // 正文一个字不动，人物直接进盘
    CHECK(store.load_story().characters.size() == 1);
    CHECK(store.load_story().written_chapters() == s.written_chapters());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/analyze：没正文可读") {
    const fs::path root = fresh_project("没正文");
    ProjectStore store(root);
    // 大纲写出来的故事本来就带人物表，不用走这一步
    store.save_story(parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM));

    llm::ReplayClient client({good_analysis().dump()});
    pipeline::CancelToken tok;
    CHECK_THROWS_AS(
        http::post_story_analyze(json{{"project", p_str(root)}}, client, tok),
        http::ApiError);
    CHECK(client.calls().empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 逐章展开正文 ----

namespace {

/// 一段够长的正文，开头带个记号好认。
///
/// 走 /api/story/chapter 那几条要过 kChapterMinRatio 的闸门（目标三千字的
/// 五分之一 = 600 字），十几个字的夹具会被当成"模型没写"顶回来——那正是
/// 闸门该干的事，所以夹具要写够长，不是把闸门调松。
/// 一段够长、而且**不重复**的正文。
///
/// 原来是 `mark + 700 个"字"`。复读守卫上线之后那份语料一律判废——而它
/// 判得对：700 个一模一样的字本来就是复读机。这是第二次栽在同一件事上，
/// 上一次是字数下限。**守卫抓到的是语料，那就改语料**：把合成的正文做成
/// 真的不重样，否则用例证明的只是"我们能绕过自己的守卫"。
std::string long_body(const std::string& mark) {
    static const char* kWho[] = {"林然", "沈悠", "陈默", "老板娘"};
    static const char* kDo[] = {"推开玻璃门", "把伞收起来", "看了一眼钟",
                                "拿起柜台上的杯子", "转身走向货架",
                                "停在雨里没有动"};
    static const char* kHow[] = {"雨声压过了店里的音乐", "灯管闪了一下",
                                 "他没有回头", "空气里有泡面的味道",
                                 "外面的车灯扫过墙面", "谁也没有先开口"};
    std::string s = mark;
    for (int i = 0; s.size() < 2400; ++i) {
        s += kWho[i % 4];
        s += kDo[(i * 3 + 1) % 6];
        s += "，";
        s += kHow[(i * 5 + 2) % 6];
        s += "，第" + std::to_string(i) + "次。\n\n";
    }
    return s;
}

/// 模型写回来的一章：正文 + 几个可以收口的地方。
json good_chapter(const std::string& body,
                  const std::vector<std::pair<std::string, std::string>>& hooks = {}) {
    json hs = json::array();
    for (const auto& [why, after] : hooks) {
        hs.push_back(json{{"text", why}, {"after", after}});
    }
    return json{{"text", body}, {"hooks", hs}};
}

/// 大纲写出来的故事：有梗概有钩子，没有正文。
Story outline_only_story() {
    return parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);
}

}  // namespace

TEST_CASE("一场的篇幅跟着时长走") {
    // **一场的目标字数从 `episode_duration_s` 那个时长换算**（那个名字是
    // 历史留下的，今天它是这一章打算演多久）。写死一千字的时候，30 秒
    // 那一档里一场就长得盖过整段，收口只能落在场中间——2026-09-12 实跑，
    // 停在场尾从 86% 掉到 63%。
    Story s = outline_only_story();

    for (double d : {30.0, 60.0, 120.0, 300.0}) {
        s.episode_duration_s = d;
        CAPTURE(d);
        const int per_ep = changji::stages::prose_budget_chars(d);
        const int per_scene = changji::stages::scene_target_chars(s);

        // 夹在 600 和 1500 之间：短了写不成一场戏，长了一场就盖过好几段
        CHECK(per_scene >= changji::stages::kSceneMinChars);
        CHECK(per_scene <= changji::stages::kSceneMaxChars);
        // 落在那个区间里的时长，一场正好是那一段
        if (per_ep >= changji::stages::kSceneMinChars &&
            per_ep <= changji::stages::kSceneMaxChars) {
            CHECK(per_scene == per_ep);
        }

        // 一章的场数是「一章多少字 ÷ 一场多少字」，夹在 2~5
        const int n = changji::stages::chapter_target_scenes(s);
        CHECK(n >= 2);
        CHECK(n <= 5);
    }

    // 时长短的时候要多切几场才跟得上；时长长的时候场数回落
    s.episode_duration_s = 30.0;
    const int many = changji::stages::chapter_target_scenes(s);
    s.episode_duration_s = 120.0;
    CHECK(changji::stages::chapter_target_scenes(s) <= many);
}

TEST_CASE("一章该写多长：章是故事单元，不按时长量") {
    Story s = outline_only_story();

    // **一章要比「一个时长单位的容量」厚得多**，否则章就薄得撑不起一个
    // 故事单元。这条是端到端实跑时用户指出来的：早先按「它要撑起几段 ×
    // 每段容量」算，而大纲阶段一章一条，于是每章正好写一条的量。
    // （`kEpisodesPerChapter` 是历史留下的名字，今天它就是那个倍数。）
    for (double d : {30.0, 60.0, 90.0, 180.0}) {
        s.episode_duration_s = d;
        s.plan = changji::stages::plan_episodes(s, d);
        const int target = changji::stages::chapter_target_chars(s);
        const int cap = changji::stages::prose_budget_chars(d);
        CAPTURE(d);
        CHECK(target >= cap * changji::stages::kEpisodesPerChapter);
        CHECK(target >= changji::stages::kChapterTargetChars);
    }

    // 章节计划里排了几条不影响章的篇幅——章的长短是故事的事，不是时长的事
    s.episode_duration_s = 60.0;
    s.plan.clear();
    const int no_plan = changji::stages::chapter_target_chars(s);
    s.plan = changji::stages::plan_episodes(s, 60.0);
    CHECK(changji::stages::chapter_target_chars(s) == no_plan);
}

TEST_CASE("写满一章的量也还是一条，整章都在") {
    // 以前这条钉的是「写满一章就该切出好几集」。2026-09-16 起一章一条，
    // 多长由内容定；2026-09-18 按时长切段那条链也整个拔掉了，一章就是
    // 成片里的一段。
    Story s = outline_only_story();
    s.episode_duration_s = 30.0;

    // **每一行都要不一样。** 三十行一模一样的一百个「字」是复读机，
    // 复读守卫判得对；这里要的只是"够长、段落边界够多"。
    std::string body;
    for (int i = 0; i < 30; ++i) {
        body += "第" + std::to_string(i) + "段：";
        for (int k = 0; k < 96; ++k) body += "字";
        body += "\n";
    }
    s = changji::stages::apply_chapter(
        s, "ch01",
        changji::stages::parse_chapter(json{{"text", body}}.dump()));
    s.plan = changji::stages::plan_episodes(s, 30.0);

    int from_ch01 = 0;
    for (const auto& p : s.plan) {
        if (p.from_chapter != "ch01") continue;
        ++from_ch01;
        CHECK(p.from_char == 0);
        CHECK(p.to_char == s.chapter_by_id("ch01")->text_len());
    }
    CHECK(from_ch01 == 1);
}

TEST_CASE("模型的草稿纸摘掉：引用块和强调标记") {
    // **2026-09-13 用全新项目跑批量写正文时撞到的。** ch02 四十八段里十五段
    // 是 markdown 引用块，内容是模型在跟自己讨论提示词：
    //     > 注意：根据规则20，每一场都要有人说话……
    //     > 重新审视规则：「不要冒出没在人物表里的人」。
    // 原有的自言自语过滤靠词表（JSON / 请忽略 / 用户要求），一条都没命中。
    const std::string good1 = "林浅推开停尸间的门，冷气扑面而来，她按住了胸口。";
    const std::string good2 = "走廊尽头的灯忽明忽暗，她听见身后有脚步声跟上来。";
    const std::string good3 = "她攥紧手里那张纸，指节发白，转身迎向那道影子。";
    const std::string aside =
        "> 注意：根据规则20，每一场都要有人说话。此场景若只有林浅一人，"
        "必须让另一个人进来或改为有人的地方。";

    const json draft = {
        {"scenes",
         {{{"where", "停尸间"},
           {"turn", "她发现尸体还在呼吸"},
           {"paragraphs", {good1, aside, good2, good3}}}}}};
    const auto d = changji::stages::parse_chapter(draft.dump());

    REQUIRE(d.scenes.size() == 1);
    // 引用块那一段整段摘掉——它不是小说
    CHECK(d.text.find("根据规则20") == std::string::npos);
    CHECK(d.text.find(">") == std::string::npos);
    // 正常段落一个不少
    CHECK(d.text.find(good1) != std::string::npos);
    CHECK(d.text.find(good2) != std::string::npos);
    CHECK(d.text.find(good3) != std::string::npos);
    // **场次表和正文要一起摘**：场的位置是按段落数数出来的，
    // 只摘正文的话两边就对不上了。
    CHECK(d.scenes[0].paragraphs.size() == 3);

    SUBCASE("行首列表符号削掉，但负号留住") {
        // **2026-09-13 实跑正文里的原样**：
        //     --1层停尸间的门再次打开，手电筒的光束在黑暗中晃动……
        // 第一个减号是 markdown 列表符号，第二个是「负一层」的负号。
        // 它会一路走进章节计划的钩子、剧本和字幕。
        const std::string marked =
            "--1层停尸间的门再次打开，手电筒的光束在黑暗中晃动。";
        const std::string neg = "-1层的空气冷得刺骨，福尔马林的味道浓重。";
        const json d3 = {
            {"scenes",
             {{{"where", "停尸间"},
               {"turn", "她看见新贴的标签"},
               {"paragraphs", {marked, neg, good3}}}}}};
        const auto got = changji::stages::parse_chapter(d3.dump());
        REQUIRE(got.scenes.size() == 1);
        CHECK(got.scenes[0].paragraphs.size() == 3);   // 一段都没少
        // 列表符号削掉，负一层原样留着
        CHECK(got.scenes[0].paragraphs[0] ==
              "-1层停尸间的门再次打开，手电筒的光束在黑暗中晃动。");
        CHECK(got.scenes[0].paragraphs[1] == neg);
    }

    SUBCASE("强调标记只去符号、不摘段") {
        // 它出现在**正常段落**里，摘段会把内容一起摘掉。
        const std::string withMark =
            "内容只有四个字：**“别多管闲事。”**她盯着屏幕，手指僵在半空。";
        const json d2 = {
            {"scenes",
             {{{"where", "办公室"},
               {"turn", "她收到威胁短信"},
               {"paragraphs", {withMark, good2, good3}}}}}};
        const auto got = changji::stages::parse_chapter(d2.dump());
        REQUIRE(got.scenes.size() == 1);
        CHECK(got.scenes[0].paragraphs.size() == 3);   // 一段都没少
        CHECK(got.text.find("**") == std::string::npos);
        CHECK(got.text.find("别多管闲事") != std::string::npos);
        CHECK(got.text.find("手指僵在半空") != std::string::npos);
    }
}

TEST_CASE("提示词：只写这一章，带的是压缩的全局记忆") {
    Story s = outline_only_story();
    s.chapters[0].text = "第一章已经写好的正文。他推门进来。";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);

    // 分界线：只写这一章
    CHECK(p.find("只写这一章") != std::string::npos);
    // 2026-09-12：写作单位是「场」，不是一堆段落。没有这个单位的时候模型
    // 把整章梗概平摊成一串镜头，写出来是概述不是场景。
    //
    // **2026-09-16：这两条搬家了，没有取消。** 「一场戏是什么」和「一段推进
    // 一两秒钟的事」原来在提示词正文里各占一段，而 scenes / paragraphs 两个
    // 字段的 description 里逐字就有——描述贴在字段上，模型填那一栏时一定读到，
    // 比正文里隔着两千字的一句强。所以这儿改成查它们在 schema 里。
    CHECK(p.find("写场面，不写概述") != std::string::npos);
    {
        const auto sch = changji::stages::chapter_schema(3, 22);
        const auto& sp =
            sch.at("properties").at("scenes").at("items").at("properties");
        const std::string scenes_desc =
            sch.at("properties").at("scenes").at("description");
        CHECK(scenes_desc.find("一个地方、一段连着的时间里") != std::string::npos);
        const std::string paras_desc = sp.at("paragraphs").at("description");
        CHECK(paras_desc.find("一段推进一两秒钟的事") != std::string::npos);
    }
    CHECK(p.find(std::to_string(changji::stages::chapter_target_scenes(s)) +
                 " 场戏") != std::string::npos);
    CHECK(p.find(std::to_string(changji::stages::chapter_scene_chars(s)) +
                 " 字上下") != std::string::npos);
    // 每一场停在自己的 turn 上，那就是这一场的收口
    CHECK(p.find("每一场停在它的 turn 上") != std::string::npos);
    CHECK(p.find("最后一场的 turn 要落到这件事上") != std::string::npos);
    // 不许贴情绪标签，但要写内心：拆成身体和当下那句心里话
    CHECK(p.find("神情复杂") != std::string::npos);
    CHECK(p.find("一场只跟着一个人走") != std::string::npos);
    // 长相归美术那一步，但**身体要在场上**——上一版这条被模型扩大成
    // 「不要描写人」，人物在场景里没有身体
    CHECK(p.find("不要写长相") != std::string::npos);
    CHECK(p.find("身体要在场") != std::string::npos);
    // 五感里至少有一个不靠眼睛
    CHECK(p.find("不靠眼睛的细节，一场里要有三四处") != std::string::npos);
    CHECK(p.find("从上一章停下的地方接着走") != std::string::npos);
    CHECK(p.find("【这是第一章】") == std::string::npos);

    // 压缩的全局记忆：人物、关系在，前情是每章一句
    CHECK(p.find("林晚") != std::string::npos);
    CHECK(p.find("前任") != std::string::npos);
    CHECK(p.find("【前情提要】") != std::string::npos);
    CHECK(p.find("他推门进来，伞还在手里") != std::string::npos);  // ch01 的梗概

    // 上一章结尾接语气
    CHECK(p.find("【上一章是这么结束的】") != std::string::npos);
    CHECK(p.find("第一章已经写好的正文") != std::string::npos);

    // 这一章要写什么、停在哪
    CHECK(p.find("【这一章】五年前那把伞") != std::string::npos);
    CHECK(p.find("【最后一场的 turn 要落到这件事上】他没有回头") !=
          std::string::npos);

    // 第一章没有前情，也没有上一章
    const std::string first = changji::stages::build_chapter_prompt(
        s, "ch01", StyleLine::REALISTIC);
    CHECK(first.find("【前情提要】") == std::string::npos);
    CHECK(first.find("【上一章是这么结束的】") == std::string::npos);
    // 第一章那个位置不能空着：实跑时 14B 两次都只写出一百来个字
    CHECK(first.find("【这是第一章】") != std::string::npos);
    // **第一章还要专门催对白。** 实测它的对白比例总是全书最低（14%、8%、
    // 20%，而后三章都在 27%~48%）——没有前情可接，模型就一个人在那儿看
    // 和想。读者认人靠的是听他们说话。
    CHECK(first.find("开头几段之内就要有人开口") != std::string::npos);
    CHECK(first.find("怎么称呼对方") != std::string::npos);
}

TEST_CASE("并回去：场的位置是数出来的，不是模型报的") {
    // **上一版靠模型抄一句原文回来（hooks[].after），程序再去正文里查。**
    // 抄错一个字那一场就落不下去，只能收在一个说不出为什么的段落边界上。
    // 现在正文是一场一场写的，第几段结束就是第几场结束——程序自己数。
    Story s = outline_only_story();

    const std::string a1 = "他把伞立在门边，水顺着伞骨往下淌。";
    const std::string a2 = "收银台那台关东煮机器在响，热气糊住了玻璃。";
    const std::string b1 = "天台的风比楼下大，铁门在身后合上。";
    const std::string b2 = "她没回头，手指扣着栏杆上那道缺口。";

    const json draft = {
        {"scenes",
         {{{"where", "深夜，便利店，只有冷柜的白光"},
           {"pov", "林晚"},
           {"goal", "把伞要回来"},
           {"obstacle", "他不认这把伞"},
           {"turn", "伞柄上刻着的不是她的名字"},
           {"paragraphs", {a1, a2}}},
          {{"where", "凌晨，楼顶天台，天还没亮"},
           {"pov", "林晚"},
           {"goal", "问清楚那个名字"},
           {"obstacle", "他一句话都不说"},
           {"turn", "他把伞从天台扔了下去"},
           {"paragraphs", {b1, b2}}}}}};

    const auto d = changji::stages::parse_chapter(draft.dump());
    REQUIRE(d.scenes.size() == 2);
    s = changji::stages::apply_chapter(s, "ch01", d);

    const Chapter* c = s.chapter_by_id("ch01");
    REQUIRE(c != nullptr);
    REQUIRE(c->scenes.size() == 2);

    // 正文就是各场的段落顺次拼起来的，段间一个换行
    CHECK(c->text == a1 + "\n" + a2 + "\n" + b1 + "\n" + b2);

    // 第一场收在第二段之后那个位置；两场首尾相接，末场顶到章尾
    const int first_end = static_cast<int>(
        text::utf8_len(a1 + "\n" + a2 + "\n"));
    CHECK(c->scenes[0].from_char == 0);
    CHECK(c->scenes[0].to_char == first_end);
    CHECK(c->scenes[1].from_char == first_end);
    CHECK(c->scenes[1].to_char == c->text_len());

    // 场的底子留着：写剧本那一步要知道这一场在哪、跟谁走
    CHECK(c->scenes[0].pov == "林晚");
    CHECK(c->scenes[0].where.find("便利店") != std::string::npos);

    // **每一场的末尾都成了有说法的切点**，而说法是**那一场的收尾那一句**，
    // 不是 turn。turn 老写成在脑子里发生的事（「他意识到当年误解了真相」），
    // 而且一章里两场常常是同一件事换个说法；收尾那一句被 last_line 那一栏
    // 约束成「turn 发生的那一刻」，必然是动作或台词，而且场场不同。
    bool at_first = false, at_end = false;
    for (const auto& h : c->hooks) {
        if (h.at_char == first_end && h.text == a2) at_first = true;
        if (h.at_char == c->text_len() && h.text == b2) at_end = true;
    }
    CHECK(at_first);
    CHECK(at_end);
    // turn 照旧存在场次表里：写剧本那一步拿得到
    CHECK(c->scenes[0].turn == "伞柄上刻着的不是她的名字");
}

TEST_CASE("并回去：老形状还认（顶层 paragraphs、顶层 text）") {
    // 改 schema 之前存下来的草稿、粘贴导入那条路都走这儿。认不出来的话
    // 那些故事一打开正文就是空的。
    Story s = outline_only_story();
    const json older = {{"paragraphs", {"他推门进来的时候，风也跟着进来了。",
                                        "她没有抬头，手里的杯子还冒着热气。"}}};
    s = changji::stages::apply_chapter(
        s, "ch01", changji::stages::parse_chapter(older.dump()));
    const Chapter* c = s.chapter_by_id("ch01");
    REQUIRE(c != nullptr);
    CHECK(c->text_len() > 20);
    CHECK(c->scenes.empty());  // 没有场就是没有，收口退回按段落边界找
}

TEST_CASE("正文在贴情绪标签就打回") {
    // 实跑那一章（chapter_check8 / ch02）1674 个字里，「神情复杂」
    // 「眼中满是惊讶与疑问」「心中涌起难以言喻的情绪」这类说法出现了十几次。
    // 它们把感受替读者做完了，是那份正文读起来像分镜表的主要原因之一。
    json scenes = json::array();
    json paras = json::array();
    for (int i = 0; i < 12; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她站在那里，神情复杂地看着他走远。");
    }
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"goal", "要回伞"},
                      {"obstacle", "他不认"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras}});
    CHECK_THROWS_AS(
        changji::stages::parse_chapter(json{{"scenes", scenes}}.dump()),
        changji::stages::StoryError);

    // 偶尔冒一个不算：阈值是每千字三个，整章至少四个才判。
    json ok_paras = json::array();
    for (int i = 0; i < 12; ++i) {
        ok_paras.push_back("第" + std::to_string(i) +
                           "段：她把杯子放下，水在桌面上洇出一圈。");
    }
    ok_paras.push_back("他没说话，神情复杂地看了她一眼，然后推门出去了。");
    // 每场都要有对白，否则先撞上另一道闸
    ok_paras.push_back("她把抹布搭在台面上：“伞放那儿吧。”");
    json one = json::array();
    one.push_back({{"where", "深夜，便利店"},
                   {"pov", "林晚"},
                   {"goal", "要回伞"},
                   {"obstacle", "他不认"},
                   {"turn", "伞柄上刻着别人的名字"},
                   {"paragraphs", ok_paras}});
    CHECK_NOTHROW(changji::stages::parse_chapter(json{{"scenes", one}}.dump()));
}

TEST_CASE("在场的人是数组，不是一个字符串") {
    // 上一版是一个字符串加 minLength，模型填了「林夏, 无他人」就绕过去了
    // ——2026-09-12 实跑，那一章对白只有 4%。措辞拦不住的用语法拦：数组的
    // minItems 进 GBNF 是硬的。这和第一轮把正文从 text 改成 paragraphs
    // 是同一招。**「是数组」这一半到今天仍然管用，钉住。**
    //
    // ⚠️ **下限从 2 降到 1 是有意的（0d2c984）**，chapter_write.cpp 里写着
    // 理由：「单人场是合法的：独处时仍然可以有实时行动、电话或环境阻力。
    // 对白比例由正文守卫检查，不再靠凭空塞第二个人来保证。」
    // 这条用例原来钉 `>= 2`，从那以后一直红着——**红了不改比不写更糟**，
    // 它会把人训练成"看见红的先跳过"。改成钉现在的设计：至少一人（空的
    // 那种仍然拦得住），上限三人（画面里立不住更多）。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& sp = s.at("properties").at("scenes").at("items").at("properties");
    REQUIRE(sp.contains("who"));
    CHECK(sp.at("who").at("type") == "array");
    CHECK(sp.at("who").at("minItems").get<int>() >= 1);
    CHECK(sp.at("who").at("maxItems").get<int>() <= 3);

    // 落库之后是一行顿号分隔的名字
    json paras = json::array();
    for (int i = 0; i < 14; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
    }
    paras.push_back("他停在门口：“伞我带来了。”");
    paras.push_back("她没抬头：“放那儿吧。”");
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"who", json::array({"林晚", "陈默"})},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"worse", "她发现伞根本不是他带来的"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    REQUIRE(d.scenes.size() == 1);
    CHECK(d.scenes[0].who == "林晚、陈默");
}

TEST_CASE("整章几乎没对白就打回") {
    // 门槛从「整章两处」提到「至少一成的段落有人说话」：两处那个下限太松，
    // 三跑基线里每一跑都有一章掉到个位数（4%、1%、25%），而它们都过了
    // 两处那道闸。那种章不是没人在场，是模型把对话整段转述掉了。
    const auto make = [](int spoken) {
        json paras = json::array();
        for (int i = 0; i < 30; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        for (int i = 0; i < spoken; ++i) {
            paras.push_back("他停在门口，手扶着门框：“伞我带来了" +
                            std::to_string(i) + "。”");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };
    // 32 段里两处对白 = 6%，过了老闸（整章两处），过不了新闸（10%）
    CHECK_THROWS_AS(changji::stages::parse_chapter(make(2)),
                    changji::stages::StoryError);
    // 34 段里四处 = 12%，够了。**门槛试过 8%，更慢也更散，退回 10%**：
    // 三跑实测生成时间 685 → 825 秒，最低那章从 9~20 松回 0~23。
    CHECK_NOTHROW(changji::stages::parse_chapter(make(4)));
    // 软闸：最后一次尝试照收
    CHECK_NOTHROW(changji::stages::parse_chapter(make(2), 0, false));
}

TEST_CASE("每一道闸打回时都带着自己的代号") {
    // 用户 2026-09-19：「记闸门打回理由，做」。代号落进 gates.jsonl 那一列，
    // 「哪条闸门最费钱」按它 group by。
    //
    // **为什么代号和那句话要分两样记**：那句话里带着这一次的数和这一次的词
    // （「65 段里只有 0 处」「「神情复杂」这类出现了 14 次」），**每一次都
    // 不一样，分不了组**。只记那句话的话，一万次打回就是一万个桶。
    //
    // 漏给代号不会当场报错——那一次归进空代号那一桶，统计少数一笔而已。
    // 所以这条用例钉的是「非空」和「是哪一个」两件事。
    const auto code_of = [](const std::string& raw, int min_chars = 0,
                            bool strict = true) {
        try {
            changji::stages::parse_chapter(raw, min_chars, strict);
        } catch (const changji::stages::StoryError& e) {
            return e.code();
        }
        return std::string{};
    };

    // 解析不动
    CHECK(code_of("这不是 JSON") == "bad_json");
    CHECK(code_of(json("就一个字符串").dump()) == "bad_json");
    // 一个字都没写
    CHECK(code_of(json{{"text", ""}}.dump()) == "no_body");
    // 短得离谱（模型把章标题填进了正文）
    CHECK(code_of(json{{"text", "第三章 雨夜"}}.dump(), 300) == "too_short");

    // 整章几乎没对白（软闸）
    const auto quiet = [] {
        json paras = json::array();
        for (int i = 0; i < 30; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    }();
    CHECK(code_of(quiet) == "no_dialogue");
    // 软闸关掉就放过去——**代号也跟着没有**，不然账上会多出一次没发生的打回
    CHECK(code_of(quiet, 0, false).empty());
}

TEST_CASE("模型把 JSON 字段名当正文写出来的，摘掉") {
    // 2026-09-12 实跑逮到的，用户先看出来的（"好多明显是凑字数的字符"）。
    // 一场的内容讲完了，但 paragraphs 的 minItems 还没满，而 minItems 是
    // GBNF 硬约束——语法里没有"结束数组"这个选项，模型只好把下一个字段名
    // 当成一段正文吐出来。三跑 1715 段里 29 段这样，最糟一章占了 22 段。
    //
    // 闸已经一起调低了（min_items 从目标的三分之二降到一半），这道是兜底。
    const json draft = {
        {"scenes",
         {{{"where", "深夜，便利店"},
           {"pov", "林晚"},
           {"who", {"林晚", "陈默"}},
           {"goal", "把伞要回来"},
           {"obstacle", "他不认这把伞"},
           {"worse", "他把伞收进了柜台底下"},
           {"turn", "伞柄上刻着的不是她的名字"},
           {"paragraphs",
            {
                "他把伞立在门边，水顺着伞骨往下淌。",
                // 冒号后面还有真话：摘掉字段名那截，正文留下
                "last_line\": \"现在，该还债了。\"",
                // 半角冒号、没带引号，字段还是它自己编的
                "turn_note\": 他主动挑破了这件事。",
                // 全角冒号那一种
                "last_line：“你到底是谁？”",
                // 后面只剩 JSON 标点：整段丢掉
                "last_line\": null}]}] }",
                "她没回头，手指扣着栏杆上那道缺口。",
            }}}}}};

    const auto d = changji::stages::parse_chapter(draft.dump());
    REQUIRE(d.scenes.size() == 1);
    const auto& ps = d.scenes[0].paragraphs;

    // 纯标点那一段没了，别的都留着（六段进、五段出）
    CHECK(ps.size() == 5);
    for (const auto& p : ps) {
        CHECK(p.find("last_line") == std::string::npos);
        CHECK(p.find("turn_note") == std::string::npos);
    }
    // 能救的救了：正文还在，包在外面的那层半角引号摘掉了，
    // 中文引号是对白自己的，留着
    CHECK(ps[1] == "现在，该还债了。");
    CHECK(ps[2] == "他主动挑破了这件事。");
    CHECK(ps[3] == "“你到底是谁？”");
    CHECK(ps[4] == "她没回头，手指扣着栏杆上那道缺口。");
}

TEST_CASE("正常正文里带冒号的句子别误伤") {
    // 上面那道闸是按"ASCII 标识符 + 冒号"认的。中文正文里的冒号
    // （"他说：……"）前面是中文，落不进这个形状；而真要有人写
    // "Plan B：往北走"，也该原样留着。
    const json draft = {
        {"scenes",
         {{{"where", "深夜，便利店"},
           {"pov", "林晚"},
           {"who", {"林晚", "陈默"}},
           {"goal", "把伞要回来"},
           {"obstacle", "他不认这把伞"},
           {"worse", "他把伞收进了柜台底下"},
           {"turn", "伞柄上刻着的不是她的名字"},
           {"paragraphs",
            {
                "他低声说：“这把伞我留着。”",
                "她想起那句话：谁先开口谁就输了。",
            }}}}}};

    const auto d = changji::stages::parse_chapter(draft.dump());
    REQUIRE(d.scenes.size() == 1);
    const auto& ps = d.scenes[0].paragraphs;
    REQUIRE(ps.size() == 2);
    CHECK(ps[0] == "他低声说：“这把伞我留着。”");
    CHECK(ps[1] == "她想起那句话：谁先开口谁就输了。");
}

TEST_CASE("分镜的话按小句摘掉，不摘整段") {
    // 「镜头拉远」「画面渐暗」是分镜的语言不是小说的语言，而后面另有一步
    // 专门把正文变成拍子。提示词里写了不要这么写，但 2026-09-12 量方差
    // 那两跑里一跑干净、另一跑又冒出来——规则不是没写，是在方差里时有
    // 时无。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        paras.push_back("她没抬头，抹布又抹了一遍：“放那儿吧。”");
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };

    // 前半句是正经正文，整段丢掉就把内容一起丢了
    const auto d = changji::stages::parse_chapter(
        body("纸条落在收银台的一角，镜头定格在上面那行字。"));
    CHECK(d.text.find("纸条落在收银台的一角") != std::string::npos);
    CHECK(d.text.find("镜头定格") == std::string::npos);

    // **单字不收。** 照片的画面、摄影机的镜头都是实物，不能误伤
    const auto keep = changji::stages::parse_chapter(
        body("照片的画面已经褪色，边角卷起一块，她用指甲压平。"));
    CHECK(keep.text.find("照片的画面已经褪色") != std::string::npos);

    // 摘完剩不下什么就还回原样：宁可留一句分镜话，也别把一段摘成半截
    const auto whole = changji::stages::parse_chapter(body("画面渐暗。"));
    CHECK(whole.text.find("画面渐暗") != std::string::npos);
}

TEST_CASE("一段里只有右引号就补回左引号") {
    // 2026-09-12 实跑：最后一章的钩子是「苏妍点头微笑。”好的。”」——两个
    // 都是右引号。normalize_quotes 只在整章没有 “ 时才动手，而这一章别处
    // 是正常的，所以这一段漏过去了，原样落进正文、落进章节计划的钩子、落进
    // 字幕。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        paras.push_back("她没抬头，抹布又抹了一遍：“放那儿吧。”");
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };

    const auto fixed = changji::stages::parse_chapter(
        body("苏妍点头微笑。”好的。”"));
    CHECK(fixed.text.find("“好的。”") != std::string::npos);

    // **引号跨段的写法不碰**：一个人连说几段，中间那几段只有右引号是正当的。
    // 这里右引号是奇数个，不动。
    const auto keep = changji::stages::parse_chapter(
        body("她顿了顿，接着说下去。”这些年我一直在等。"));
    CHECK(keep.text.find("”这些年我一直在等。") != std::string::npos);
}

TEST_CASE("每一场都要说清局面更糟在哪儿") {
    // **2026-09-12 加的，因为一章三场原地打转。** 实跑那一章三场都在同一个
    // 地方对着同一样东西，三个收尾是同一个手势的变奏（手指僵在半空 /
    // 手指在伞柄上方停住 / 指尖即将碰到又缩回），三场的收口长得一样。
    //
    // 编剧的老规矩是「通过事情的扭转，使情况比这场戏刚开始时更加恶劣」，
    // 电影那边叫「每一场都要有信息增量」。局面更糟这件事没法重复三遍。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& sp = s.at("properties").at("scenes").at("items").at("properties");
    REQUIRE(sp.contains("worse"));
    CHECK(sp.at("worse").at("minLength").get<int>() >= 6);

    bool required = false;
    for (const auto& r :
         s.at("properties").at("scenes").at("items").at("required")) {
        if (r == "worse") required = true;
    }
    CHECK(required);

    // **排在 turn 前面**：先定这一场把局面推到多糟，turn 才是那件事落地的
    // 那一刻；反过来 turn 已经写完了，worse 只能补一个说法，补出来的多半
    // 是 turn 换个说法。
    std::vector<std::string> keys;
    for (auto it = sp.begin(); it != sp.end(); ++it) keys.push_back(it.key());
    const auto at = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(at("worse") < at("turn"));
    CHECK(at("obstacle") < at("worse"));
}

TEST_CASE("最后一句是单独一栏，落库之后接在这一场末尾") {
    // **禁令拦不住就别再加第四道。** 「写完 turn 就停，别再加一段点题」
    // 在提示词、schema 描述、解析守卫里各说了一遍，三道都没拦住——实跑里
    // 一半的章还是在 turn 后面补一段「那一刻，她终于可以告诉自己……」，
    // 而那一段正好落在这一场的收口上。把最后一句抬成一个字段，语法里就没有
    // 位置再写下一段了。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& scene = s.at("properties").at("scenes").at("items");
    const auto& sp = scene.at("properties");
    REQUIRE(sp.contains("last_line"));
    CHECK(sp.at("last_line").at("maxLength").get<int>() <= 120);  // 一句话的量

    bool required = false;
    for (const auto& r : scene.at("required")) {
        if (r == "last_line") required = true;
    }
    CHECK(required);

    // **排在 paragraphs 后面**：它是收尾，不是开头。
    std::vector<std::string> keys;
    for (auto it = sp.begin(); it != sp.end(); ++it) keys.push_back(it.key());
    const auto at = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(at("paragraphs") < at("last_line"));

    // 落库之后它就是这一场的最后一段，没人分得出它当初是单独一栏
    json paras = json::array();
    for (int i = 0; i < 14; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
    }
    paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
    paras.push_back("她没抬头，抹布在台面上又抹了一遍：“放那儿吧。”");
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                      {"pov", "林晚"},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    REQUIRE(d.scenes.size() == 1);
    CHECK(d.scenes[0].paragraphs.back() == "她把伞柄转过来，刻着的不是她的名字。");
    CHECK(d.text.size() >= 20);
    CHECK(d.text.rfind("她把伞柄转过来，刻着的不是她的名字。") ==
          d.text.size() - std::string("她把伞柄转过来，刻着的不是她的名字。").size());
}

TEST_CASE("换个说法的同一件事，也算撞车") {
    // **2026-09-12 实跑，这两场的 turn 就是相邻两场的收口**：
    // 一个字都不连着一样，按字面比的守卫不响，而观众看到的是同一件事
    // 演两遍——第二场的信息增量是零。
    // **「换个说法的同一件事」抓不到，而且量过了：** 相邻两字的 Dice
    // 系数，这一对是 0.30，而下面那对真的不同的事是 0.27——分不开。
    // 那个信号在语义里，不在字面里。这一条钉住"我们知道它漏"。
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "林悦认出男人是沈嘉诚，并意识到那把伞对她有特殊意义",
        "林悦意识到这把伞对她意义非凡，而对方显然知道这一点"));

    // 一字不差的、互相包含的，照旧要抓住
    CHECK(changji::stages::scenes_repeat_beat("他把伞从天台扔了下去",
                                              "他把伞从天台扔了下去"));
    CHECK(changji::stages::scenes_repeat_beat(
        "伞柄上刻着别人的名字", "她翻过伞柄，伞柄上刻着别人的名字，不是她的"));

    // **真的不同的两件事不能误伤。** 软闸误伤的代价是一次生成变三次。
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "林悦抓住沈嘉诚手腕，质问他的选择",
        "陈阿姨递出一个信封，里面装着一张旧照片"));
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "她把收银单据折成纸船放进水槽", "他在门口停下，没有回头"));
    // 同一个人做的两件不同的事，也不能算撞
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "林悦把伞递给他", "林悦把信收进抽屉锁好"));

    // 太短的不判：一句话不到八个字，重合是巧合
    CHECK_FALSE(changji::stages::scenes_repeat_beat("他走了", "他走了"));
}

TEST_CASE("一段话写两遍就打回") {
    // 复读守卫要一句出现三次才响，而实跑里常见的是同一段二十几个字的话
    // 在一章里出现**两次**——够不着那道闸，却已经是读者能看出来的原地
    // 打转（上一轮 ch03、ch04 各有两段）。
    const auto make = [](bool dup) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        paras.push_back("她没抬头，抹布在台面上又抹了一遍：“放那儿吧。”");
        if (dup) {
            paras.push_back("第3段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };
    CHECK_THROWS_AS(changji::stages::parse_chapter(make(true)),
                    changji::stages::StoryError);
    CHECK_NOTHROW(changji::stages::parse_chapter(make(false)));

    // 软闸：最后一次尝试照收，0 字比「写得一般」差得多
    CHECK_NOTHROW(changji::stages::parse_chapter(make(true), 0, false));
}

TEST_CASE("整章一句对白都没有就打回") {
    // 2026-09-12 实跑四章里有一章通篇零对白（65 段全是叙述）。下一步是把
    // 这段正文改成剧本——正文里没人说话，那一章出来就是默片。和剧本那边
    // 「整章一句台词都没有」是同一道闸。
    const auto make = [](bool spoken) {
        json paras = json::array();
        for (int i = 0; i < 20; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        if (spoken) {
            // 门槛已经从「整章两处」提到「至少一成的段落」，22 段要三处
            paras.push_back("他停下来，手扶着门框：“伞我带来了。”");
            paras.push_back("她没抬头，抹布在台面上又抹了一遍：“放那儿吧。”");
            paras.push_back("他把伞靠在柜台边上：“那我走了。”");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras}});
        return json{{"scenes", scenes}}.dump();
    };
    // 下限是「至少一成的段落有人说话」：两千字里连几句话都没人说，
    // 那不是独角戏，是对话被整段转述掉了。
    CHECK_THROWS_AS(changji::stages::parse_chapter(make(false)),
                    changji::stages::StoryError);
    CHECK_NOTHROW(changji::stages::parse_chapter(make(true)));

    // **老形状不查。** 粘贴导入和改 schema 之前的草稿不是照着现在这份
    // 提示词写的，拿现在的规矩卡它们只会把打得开的故事变成打不开的。
    std::string plain;
    for (int i = 0; i < 700; ++i) plain += "字";
    CHECK_NOTHROW(changji::stages::parse_chapter(json{{"text", plain}}.dump()));
}

TEST_CASE("说话方式要一路带到正文那一步") {
    Story s = outline_only_story();
    s.characters[0].voice = "短句，从不把话说完，生气时反而更小声";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(p.find("说话：短句，从不把话说完") != std::string::npos);

    // 怕什么也要一路带到正文：只写进人物表的话，就是「小传里有、戏里
    // 没有」，人物行为看着突兀
    s.characters[0].fear = "怕被人看见她根本没打算走";
    const std::string q = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(q.find("他怕的是：怕被人看见她根本没打算走") != std::string::npos);

    // 弧光也要进来：大纲里填了、人物表里存着，而写正文这一步原来拿不到，
    // 模型写每一章时不知道这个人要往哪儿走
    s.characters[0].arc = "躲着走变成敢直视";
    const std::string r = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(r.find("他会从躲着走变成敢直视") != std::string::npos);

    // 没写的人不多这一段——粘贴导入的故事和老项目都没有这一栏
    s.characters[0].voice.clear();
    const std::string none = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(none.find("说话：") == std::string::npos);
}

TEST_CASE("抖出什么要一路带到正文那一步") {
    // 大纲里定了反转，正文那一步不知道的话，那一章照样写成「又见了一面」。
    Story s = outline_only_story();
    s.chapters[1].reveal = "那把伞不是他拿走的，是她母亲塞给他的";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(p.find("这一章要抖出来的是：那把伞不是他拿走的") != std::string::npos);
    // 要它演出来，不是让谁总结一句
    CHECK(p.find("让人看见、听见") != std::string::npos);

    // 没定反转的章不多这一行——粘贴导入的故事和老项目都没有这一栏
    s.chapters[1].reveal.clear();
    const std::string none = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(none.find("这一章要抖出来的是") == std::string::npos);
}

TEST_CASE("正文里的裸引号把 JSON 断了：转义掉再解一遍") {
    // 2026-09-20 采语料实撞：`chapter=low` 下 glm-5.3 把对白写成 ASCII 直
    // 引号而不转义，一个裸 `"` 就把整份 JSON 断在那儿。355 次里坏 70 次，
    // 全是 finish_reason=stop、字数正常的好稿——内容写完了，只是引号没转义。
    const std::string broken =
        R"({"scenes":[{"where":"招待所","pov":"韩秀兰","goal":"问出签字的人",)"
        R"("obstacle":"他不认","turn":""她说你迟早会翻。"他说，"她比谁都懂你。"",)"
        R"("paragraphs":["她敲了门。","屋里应了一声。"]}]})";

    // 先证明它真的解不动——不然下面那句"救回来了"是假的。
    REQUIRE(nlohmann::json::parse(broken, nullptr, false).is_discarded());

    const auto j = changji::stages::extract_json(broken);
    REQUIRE(j.contains("scenes"));
    const std::string turn = j["scenes"][0]["turn"].get<std::string>();
    CHECK(turn.find("她说你迟早会翻") != std::string::npos);
    CHECK(turn.find("她比谁都懂你") != std::string::npos);
    // 段落一个没丢：断在 turn 那一栏，后面的东西本来是整份一起作废的。
    CHECK(j["scenes"][0]["paragraphs"].size() == 2);

    SUBCASE("本来就合法的一份，一个字都不许动") {
        const std::string good =
            R"({"a":"他说：\"好\"。","b":["x","y"]})";
        const auto k = changji::stages::extract_json(good);
        CHECK(k["a"].get<std::string>() == "他说：\"好\"。");
        CHECK(k["b"].size() == 2);
    }
}

TEST_CASE("对白用 ASCII 双引号写的：成对才换，落单一个都不动") {
    // **2026-09-20 采蒸馏语料时逮到的。** `chapter=low` 下 glm-5.3 的 355 次
    // 正文调用里，70 次（20%）把对白写成 ASCII 直引号；先是整份 JSON 被裸
    // 引号断掉（json_extract 那一层管），救回来之后闸门又看不见这些对白，
    // 同一章被 no_dialogue 打回——和 2026-09-12 单引号那次一模一样：
    // **一个引号的写法吃掉两道闸。**
    //
    // 但双引号这条 2026-09-12 撤过一次，撤的理由是真的：叙述句里落单的双
    // 引号被配对之后，里外颠倒、叙述被劈成对白。所以这次带着「数成对」那道
    // 闸回来，下面第二段就是钉它的。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras}});
        return json{{"scenes", scenes}}.dump();
    };

    SUBCASE("成对的换成中文双引号") {
        const auto d = changji::stages::parse_chapter(
            body("他停在门口：\"伞我带来了。\"她没抬头：\"放那儿吧。\""));
        CHECK(d.text.find("“伞我带来了。”") != std::string::npos);
        CHECK(d.text.find("“放那儿吧。”") != std::string::npos);
        CHECK(d.text.find('"') == std::string::npos);
    }

    SUBCASE("奇数个：一个都不换") {
        // 2026-09-12 那次坏就坏在这儿。配对之后会变成
        // 「今天是个新起点。“她说给自己听。”只是我自己走。」——里外颠倒。
        // strict 关掉：这一段本来就没对白，这里要验的只是引号没被动过。
        const auto keep = changji::stages::parse_chapter(
            body("今天是个新起点。\"她说给自己听。\"只是我自己走。\""), 0, false);
        CHECK_MESSAGE(keep.text.find('"') != std::string::npos,
                      "落单的双引号被配对了：叙述会被劈成对白");
        CHECK(keep.text.find("“") == std::string::npos);
    }

    SUBCASE("整章已经有弯引号时，直引号一个都不动") {
        // 它会写 “” 的话，剩下那些直引号多半是英寸、代码或者引用。
        const auto d = changji::stages::parse_chapter(
            body("她说：“我知道了。”墙上写着 \"EXIT\"。"), 0, false);
        CHECK(d.text.find("\"EXIT\"") != std::string::npos);
    }
}

TEST_CASE("对白用 ASCII 单引号写的，也换成中文双引号") {
    // **2026-09-12 实跑：一个引号的写法吃掉了两道闸。** 整章对白写成
    // '这一次，我们不走回头路了。'，守卫只认弯引号，于是这一章算「一句
    // 对白都没有」，被打回两次；第三次宽松放行，而宽松那次连「两场不能
    // 撞同一件事」也一并跳过——两场的收口一字不差。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras}});
        return json{{"scenes", scenes}}.dump();
    };

    const auto d = changji::stages::parse_chapter(
        body("他停在门口：'伞我带来了。'她没抬头：'放那儿吧。'"));
    CHECK(d.text.find("“伞我带来了。”") != std::string::npos);
    CHECK(d.text.find("'") == std::string::npos);

    // **落单的撇号不动。** don't、Lin's 里那个不是引号，换了就成了半个
    // 引号挂在句子中间。
    // strict 关掉：这一段本来就没对白，这里要验的只是撇号
    const auto keep = changji::stages::parse_chapter(
        body("他低声说了句什么，听着像 don't。"), 0, false);
    CHECK(keep.text.find("don't") != std::string::npos);
}

TEST_CASE("整段复读先摘掉，摘不干净才打回") {
    // 复读守卫原来是硬闸，三次都撞上去那一章就空了——2026-09-12 把温度降到
    // 0.5 之后实跑，ch03 的「我只是怕你会后悔。」出现四次，三次尝试全被拦，
    // 那一章落成 0 字。降温度本来就更容易走进复读循环，两件事撞一块了。
    const auto build = [](int repeats) {
        json paras = json::array();
        for (int i = 0; i < 16; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口：“伞我带来了。”");
        paras.push_back("她没抬头：“放那儿吧。”");
        paras.push_back("他又说了一句：“那我走了。”");
        for (int i = 0; i < repeats; ++i) {
            paras.push_back("他低声说：“我只是怕你会后悔。”");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };

    // 同一句出现四次：摘成一次，其余的照收，不废整章
    const auto d = changji::stages::parse_chapter(build(4));
    CHECK(d.text.find("我只是怕你会后悔") != std::string::npos);
    std::size_t at = 0;
    int times = 0;
    while ((at = d.text.find("我只是怕你会后悔", at)) != std::string::npos) {
        ++times;
        at += 3;
    }
    CHECK(times == 1);
    // 正文没被摘残
    CHECK(d.text.find("伞我带来了") != std::string::npos);
    CHECK(changji::text::utf8_len(d.text) > 300);
}

TEST_CASE("收尾只说「说了一句」、不给内容，就打回") {
    // 2026-09-12 实跑的两个钩子：「他低声说了一句，没人回答。」「他又低声
    // 说了一句，语气比刚才更坚定。」——都没说他说了什么。收尾那一句是整章
    // 的钩子，观众听不见那句话，这一章就等于没有结尾。
    const auto make = [](const std::string& last) {
        json paras = json::array();
        for (int i = 0; i < 16; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口：“伞我带来了。”");
        paras.push_back("她没抬头：“放那儿吧。”");
        paras.push_back("他把伞靠在柜台边：“那我走了。”");
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", last}});
        return json{{"scenes", scenes}}.dump();
    };

    // 只管每章最后一场：这里只有一场，它就是章尾
    CHECK_THROWS_AS(changji::stages::parse_chapter(
                        make("他低声说了一句，没人回答。")),
                    changji::stages::StoryError);
    // 把话写出来就过
    CHECK_NOTHROW(changji::stages::parse_chapter(
        make("他低声说：“这伞不是我的。”")));
    // 不涉及说话的收尾照旧不管
    CHECK_NOTHROW(changji::stages::parse_chapter(
        make("她把伞柄转过来，刻着的不是她的名字。")));
    // 软闸：最后一次尝试照收
    CHECK_NOTHROW(changji::stages::parse_chapter(
        make("他低声说了一句，没人回答。"), 0, false));

    // **中间那几场不管。** 每场都管的那一版实跑生成时间翻倍（420 → 850
    // 秒）——模型一直写转述、一直被打回，而时间翻倍就说明它满足不了。
    // 章尾那个钩子最要紧（这一章留给下一章的悬念），先只守它。
    // 两场的段落也要各写各的，否则撞上「一段话不许写两遍」那道闸
    int serial = 0;
    const auto body = [&]() {
        json out = json::array();
        for (int i = 0; i < 16; ++i) {
            out.push_back("第" + std::to_string(++serial) +
                          "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        out.push_back("他停在门口：“伞我带来了" + std::to_string(serial) + "。”");
        out.push_back("她没抬头：“放那儿吧" + std::to_string(serial) + "。”");
        out.push_back("他把伞靠在柜台边：“那我走了" + std::to_string(serial) + "。”");
        return out;
    };
    // 两场的 turn 必须不同，否则先撞上「两场不能停在同一件事上」那道闸
    const auto scene = [&](const std::string& turn, const std::string& last) {
        return json{{"where", "深夜，便利店"},
                    {"pov", "林晚"},
                    {"who", json::array({"林晚", "陈默"})},
                    {"goal", "把伞要回来"},
                    {"obstacle", "他不认这把伞"},
                    {"worse", "她发现伞根本不是他带来的"},
                    {"turn", turn},
                    {"paragraphs", body()},
                    {"last_line", last}};
    };
    json two = json::array();
    two.push_back(scene("伞柄上刻着别人的名字",
                        "他低声说了一句，没人回答。"));       // 中间那场：放过
    two.push_back(scene("她把工牌扣在了柜台上",
                        "他低声说：“这伞不是我的。”"));       // 章尾：合格
    CHECK_NOTHROW(changji::stages::parse_chapter(json{{"scenes", two}}.dump()));
}

TEST_CASE("模型的自言自语摘掉那一段，不废整章") {
    // 语法把模型关在 JSON 字符串里，它想解释自己的时候那些话就落进某一段
    // 正文（实跑原样：一段 1164 字的「不符合用户要求的“只输出 JSON”，
    // 请忽略此部分内容……」）。原来是见着就打回，而它是硬闸——这是最后
    // 一道还能把整章清成 0 字的闸。
    json paras = json::array();
    for (int i = 0; i < 16; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
    }
    paras.push_back("他停在门口：“伞我带来了。”");
    paras.push_back("她没抬头：“放那儿吧。”");
    paras.push_back("他把伞靠在柜台边：“那我走了。”");
    paras.push_back("以上内容不符合用户要求的只输出 JSON，请忽略此部分内容。");
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"who", json::array({"林晚", "陈默"})},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"worse", "她发现伞根本不是他带来的"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});

    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    CHECK(d.text.find("请忽略") == std::string::npos);
    CHECK(d.text.find("JSON") == std::string::npos);
    // 正文还在，没被废掉
    CHECK(d.text.find("伞我带来了") != std::string::npos);
    CHECK(changji::text::utf8_len(d.text) > 300);
}

TEST_CASE("复读摘的是句，不是段") {
    // **第一版摘的是整段重复，挡不住。** 2026-09-12 实跑里复读的是一句话
    // ——「你知道我最恨什么吗？」在三个不同的段落里各出现一次，段级去重
    // 一句都抓不到，守卫照样判，三次撞完那一章还是空的。摘的粒度必须和
    // 守卫量的粒度一样（repetition.hpp 的 kRepeatMaxSame 数的是句）。
    json paras = json::array();
    for (int i = 0; i < 16; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
    }
    paras.push_back("他停在门口：“伞我带来了。”");
    paras.push_back("她没抬头：“放那儿吧。”");
    paras.push_back("他把伞靠在柜台边：“那我走了。”");
    // 同一句话散在三个不同的段落里——段级去重抓不到这种
    paras.push_back("他看着她的背影。你知道我最恨什么吗？他没说出口。");
    paras.push_back("她转过身来。你知道我最恨什么吗？这句话卡在他喉咙里。");
    paras.push_back("门开了又合上。你知道我最恨什么吗？他终究没问。");
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"who", json::array({"林晚", "陈默"})},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"worse", "她发现伞根本不是他带来的"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});

    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    std::size_t at = 0;
    int times = 0;
    while ((at = d.text.find("你知道我最恨什么吗", at)) != std::string::npos) {
        ++times;
        at += 3;
    }
    CHECK(times == 1);
    // 那几段里别的话留着，不是整段丢掉
    CHECK(d.text.find("他看着她的背影") != std::string::npos);
    CHECK(d.text.find("门开了又合上") != std::string::npos);
}

TEST_CASE("大纲提示词：每按一次都换一批姓") {
    // **治的是用户那句「角色的名字也都差不多」。** 根子是
    // build_outline_prompt 原来是个纯函数：同一个入口点十次，模型拿到的是
    // 同一串字节十次，于是十次都给你陈默和林晚（story_analyze.cpp 的注释里
    // 记着一次真实输出：['陈默','林景明','陈默','林景明','苏婉']）。
    using changji::stages::build_outline_names;

    SUBCASE("variation = 0 不拼——语料和别的用例靠这一档") {
        CHECK(build_outline_names(0).empty());
    }

    SUBCASE("给够几个姓，而且不重样") {
        // **不去比对词表本身。** 词表是构建期生成的（prompts.inc.hpp 在
        // build 目录里，测试目标没有那条 include 路径），而这里要问的也不是
        // "有没有照抄那张表"，是"给出来的东西能不能用"：几个、重不重。
        const std::string head = "姓从这几个里挑：";
        for (std::uint32_t v : {1u, 7u, 999u, 0x1234abcdu, 0xffffffffu}) {
            CAPTURE(v);
            const std::string got = build_outline_names(v);
            const auto at = got.find(head);
            REQUIRE(at != std::string::npos);
            const auto from = at + head.size();
            const std::string line = got.substr(from, got.find('\n', from) - from);

            std::vector<std::string> parts;
            for (std::size_t i = 0, j; ; i = j + 3) {   // 「、」是 3 个字节
                j = line.find("、", i);
                parts.push_back(line.substr(i, j == std::string::npos
                                                   ? std::string::npos
                                                   : j - i));
                if (j == std::string::npos) break;
            }
            CHECK(parts.size() == 6);   // kSurnamesPick
            std::set<std::string> seen(parts.begin(), parts.end());
            CHECK(seen.size() == parts.size());   // 同一个姓不能给两遍
            for (const auto& x : parts) CHECK_FALSE(x.empty());
        }
    }

    SUBCASE("换个种子就换一批——这是它存在的全部理由") {
        // 三十个种子里至少要抽出二十种不同的组合。要求不高，但足以抓住
        // "种子没接上"和"哈希把低位摊平了"这两种坏法——它们的表现都是
        // 几十个种子只出三四种结果。
        std::set<std::string> combos;
        for (std::uint32_t v = 1; v <= 30; ++v) combos.insert(build_outline_names(v));
        CHECK(combos.size() >= 20);
    }
}

TEST_CASE("大纲提示词：选题空间只在什么都没填时才拼") {
    // **用户给了方向就不能再塞随机的场域。** 他写「外卖员和程序员」，
    // 我们塞「远洋渔船」，出来的东西他认不出是自己要的。
    //
    // 而"什么都没有，你来一个"那条路是雷同最重的一条：提示词里一个变量
    // 都没有。选题空间正是补在那儿。
    using changji::stages::build_outline_prompt;
    using changji::models::StoryScale;
    using changji::models::StyleLine;
    const std::string mark = "这一次从下面这几样里起手";
    const std::string names = "【这个故事里的人怎么取名】";
    const std::uint32_t v = 0x5eed1234;

    SUBCASE("梗概和关键词都空：拼") {
        const std::string got =
            build_outline_prompt("", StoryScale::SHORT, StyleLine::REALISTIC, "", v);
        CHECK(got.find(mark) != std::string::npos);
    }

    SUBCASE("有梗概：不拼") {
        const std::string got = build_outline_prompt(
            "外卖员和程序员", StoryScale::SHORT, StyleLine::REALISTIC, "", v);
        CHECK(got.find(mark) == std::string::npos);
    }

    SUBCASE("只有关键词：也不拼，关键词就是他给的方向") {
        const std::string got = build_outline_prompt(
            "", StoryScale::SHORT, StyleLine::REALISTIC, "重生 复仇", v);
        CHECK(got.find(mark) == std::string::npos);
    }

    SUBCASE("姓氏那一段三条路都拼——名字和他给的方向不冲突") {
        for (const char* pre : {"", "外卖员和程序员"}) {
            for (const char* kw : {"", "重生 复仇"}) {
                CAPTURE(pre);
                CAPTURE(kw);
                const std::string got = build_outline_prompt(
                    pre, StoryScale::SHORT, StyleLine::REALISTIC, kw, v);
                CHECK(got.find(names) != std::string::npos);
            }
        }
    }

    SUBCASE("variation = 0 时两段都不拼，提示词回到老样子") {
        const std::string got =
            build_outline_prompt("", StoryScale::SHORT, StyleLine::REALISTIC, "", 0);
        CHECK(got.find(mark) == std::string::npos);
        CHECK(got.find(names) == std::string::npos);
    }
}

TEST_CASE("写正文用的温度比默认低") {
    // **默认 0.7 太散。** 2026-09-12 把同一份代码连跑两组三遍，四章里对白
    // 最低那一章，一组是 21%（19~27），另一组是 1%（0~32）——同样的提示词、
    // 同样的 schema，一章能写成 35% 也能写成 0%。0% 的章拍出来就是一段
    // 默片，对成片是坏掉的交付物。
    //
    // ⚠️ **这条用例只问这个常量是几，不问它有没有发出去。** 事实上它有很长
    // 一段时间根本没发出去：远端那条路的 build_payload 读的是
    // cfg.temperature，req 里那个没人看，而远端是默认后端——用例一直绿着，
    // 旋钮一直空转。"发出去了"由 test_llm_client.cpp 的
    // 「温度：req 没意见就用配置里的，有意见就听它的」钉着，两条缺一不可。
    CHECK(changji::stages::kChapterTemperature < 0.7);
    CHECK(changji::stages::kChapterTemperature >= 0.3);  // 太低会写成说明书
}

TEST_CASE("题材和调子要必填，而且要一路带到正文") {
    // **2026-09-12 实跑逮到的**：拿一句规则怪谈的梗概跑，大纲把规则、命案、
    // 顶罪、监控录像全写对了，伏笔也前后咬合，而正文出来是都市情感的腔调。
    // 查下来根子是 genre / tone 两栏**空着**——没进 required 模型就不填，
    // 而写正文那一步既没渲染 genre、又拿不到 tone，**根本不知道这是个
    // 悬疑故事**，只好按那几十条（照都市情感调出来的）写作规矩的默认口味写。
    const auto& props = outline_schema().at("properties");
    for (const char* k : {"genre", "tone"}) {
        CAPTURE(k);
        REQUIRE(props.contains(k));
        // minLength 不能省：进 required 只保证键在，不保证有内容（voice
        // 那一栏栽过同样的跟头）
        CHECK(props.at(k).at("minLength").get<int>() >= 2);
        bool required = false;
        for (const auto& r : outline_schema().at("required")) {
            if (r == k) required = true;
        }
        CHECK(required);
    }

    Story s = outline_only_story();
    s.genre = "规则怪谈";
    s.tone = "冷硬";
    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(p.find("【题材】规则怪谈") != std::string::npos);
    CHECK(p.find("【调子】冷硬") != std::string::npos);
    // 光报出题材不够，还要说清那几条规矩在这个题材里怎么用
    CHECK(p.find("照这个题材的路数用") != std::string::npos);
}

TEST_CASE("前面埋下的东西要单拎给后面的章") {
    // 混在前情提要里模型看不见——前情是「已经发生过的，不要重写」，而埋下
    // 的东西恰恰是还没兑现、等着后面某一章去收的。不单列的话每一章的反转
    // 都是当场冒出来的，观众没有「原来如此」那一下。
    Story s = outline_only_story();
    s.chapters[0].plant = "柜台下那只锁着的铁盒";
    s.chapters[1].plant = "他袖口那道没解释的疤";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(p.find("【前面埋下、还没收的】") != std::string::npos);
    CHECK(p.find("柜台下那只锁着的铁盒") != std::string::npos);
    CHECK(p.find("这一章要埋下：他袖口那道没解释的疤") != std::string::npos);

    // 第一章前面没有东西可收，就不出现那一段
    const std::string first = changji::stages::build_chapter_prompt(
        s, "ch01", StyleLine::REALISTIC);
    CHECK(first.find("【前面埋下、还没收的】") == std::string::npos);
}

TEST_CASE("提示词：没有这一章就抛") {
    const Story s = outline_only_story();
    CHECK_THROWS_AS(
        changji::stages::build_chapter_prompt(s, "ch99", StyleLine::REALISTIC),
        stages::StoryError);
}

TEST_CASE("解析：正文短得离谱的不收") {
    // **实跑时真撞上了**：模型把章标题填进正文字段，四章各写出 1~2 个字，
    // 而这些被静默存了下来——故事看着有四章，每章却只剩一两个字，到写剧本
    // 那一步才发现无米下锅。和剧本那边「整章一句台词都没有」一个道理。
    const std::string tiny = json{{"text", "伞"}}.dump();
    CHECK_THROWS_AS(changji::stages::parse_chapter(tiny, 600), stages::StoryError);
    // 下限给 0 表示不查——拼提示词的单测用得着
    CHECK(changji::stages::parse_chapter(tiny, 0).text == "伞");

    std::string ok;
    for (int i = 0; i < 700; ++i) ok += "字";
    CHECK(changji::text::utf8_len(
              changji::stages::parse_chapter(json{{"text", ok}}.dump(), 600).text) == 700);
}

TEST_CASE("提示词：正文是主要产出，不是顺带的") {
    Story s = outline_only_story();
    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch01", StyleLine::REALISTIC);
    // 篇幅现在是按场给的：每一场多少字。整章那个数模型够不着，
    // 一场一千字它写得到——而下限由 schema 的 minItems/minLength 兜着。
    CHECK(p.find("每一场 ") != std::string::npos);
    // **2026-09-16：「正文是主要产出」这句话从提示词正文搬进了 paragraphs
    // 的 description。** 要求没变，住处变了——而且变强了：这句话现在就贴在
    // 要填正文的那一栏上。提示词正文里那一段是纯重复，占字不办事。
    const auto sch = changji::stages::chapter_schema(3, 22);
    const std::string paras_desc = sch.at("properties")
                                       .at("scenes")
                                       .at("items")
                                       .at("properties")
                                       .at("paragraphs")
                                       .at("description");
    CHECK(paras_desc.find("这一场的正文") != std::string::npos);
    CHECK(paras_desc.find("不是梗概") != std::string::npos);
}

TEST_CASE("解析：守卫数得到的句子，摘除就一定要摘得掉") {
    // 2026-09-16 实测，hulian-test ch08 写了十分钟然后整章作废：
    //   异步作业砸了：大模型没写出能用的正文：正文在复读：
    //   这一句出现了 3 次：「他微笑，嘴角上扬。」
    //
    // 根子是**两边用了不同的归一函数**。守卫用 repeat_key（只剥首尾引号），
    // 摘除用 chapter_write 自己的 bare（连句内标点一起剥）。同一句：
    //   repeat_key → 「他微笑，嘴角上扬。」9 字 ≥ 8 → 守卫数它，三次判废
    //   bare       → 「他微笑嘴角上扬」  7 字 < 8 → 摘除跳过，一次不摘
    // 那一句于是按构造救不回来：守卫必判、摘除永远不碰，重试只是再掷骰子。
    //
    // 这个用例钉的是那条不变量：**守卫数得到的，摘除就得摘得掉**。
    const std::string dup = "他微笑，嘴角上扬。";
    REQUIRE(changji::text::utf8_len(changji::stages::repeat_key(dup)) >=
            changji::stages::kRepeatMinSentenceChars);

    std::string body;
    for (int i = 0; i < 10; ++i) {
        body += "他走到窗边，看着楼下第 " + std::to_string(i) + " 辆车开过去。\n";
    }
    // 三句，分散在不同段落里——段级那道碰不到，只能靠句级那道
    body += dup + "远处传来汽笛。\n";
    body += "他停下脚步。" + dup + "\n";
    body += "雨点打在窗上。" + dup + "\n";

    // 摘不掉的话这一句会抛 StoryError("正文在复读：…")
    const auto d = changji::stages::parse_chapter(json{{"text", body}}.dump(), 10);
    int hits = 0;
    for (size_t at = d.text.find(dup); at != std::string::npos;
         at = d.text.find(dup, at + 1)) {
        ++hits;
    }
    CHECK(hits == 1);
    // 同段里跟着那一句的别的话不能被连累
    CHECK(d.text.find("远处传来汽笛") != std::string::npos);
    CHECK(d.text.find("他停下脚步") != std::string::npos);
    CHECK(d.text.find("雨点打在窗上") != std::string::npos);
}

TEST_CASE("解析：一字不差的重复段，守卫没响也要丢") {
    // 2026-09-16 实测 hulian-test ch08 的结尾：
    //     他微笑，嘴角上扬，风声呼啸，灰尘在光束里漂浮。
    //     他微笑，嘴角上扬，风声呼啸，灰尘在光束里漂浮。
    // 相邻两段一个字都不差，就这么存进了 story.json。
    //
    // 原因是**整块去重都挂在 `if (!check_repetition(...).ok)` 里**——复读
    // 守卫不响就一次都不跑，而这一章的复读没到守卫的阈值。丢一个和前面
    // 一字不差的段落不可能丢错：那就是"写了两遍"的定义。
    std::string body;
    for (int i = 0; i < 12; ++i) {
        body += "他走到窗边，看着楼下第 " + std::to_string(i) + " 辆车开过去。\n";
    }
    const std::string dup = "他微笑，嘴角上扬，风声呼啸，灰尘在光束里漂浮。";
    body += dup + "\n" + dup + "\n";

    const auto d = changji::stages::parse_chapter(json{{"text", body}}.dump(), 10);
    // 守卫本来就没响（十二段各不相同，只有一处重复），所以这一段是常开那道
    // 拦下的
    int hits = 0;
    for (size_t at = d.text.find(dup); at != std::string::npos;
         at = d.text.find(dup, at + 1)) {
        ++hits;
    }
    CHECK(hits == 1);
    // 别的段一段都不能少
    CHECK(d.text.find("第 0 辆车") != std::string::npos);
    CHECK(d.text.find("第 11 辆车") != std::string::npos);

    SUBCASE("短段原样重复是正当的，不丢") {
        // 「"嗯。"」「他没说话。」这种重复是手法，不是复读
        std::string short_body;
        for (int i = 0; i < 12; ++i) {
            short_body += "他走到窗边，看着楼下第 " + std::to_string(i) + " 辆车。\n";
        }
        short_body += "他没说话。\n又走了几步。\n他没说话。\n";
        const auto sd =
            changji::stages::parse_chapter(json{{"text", short_body}}.dump(), 10);
        int n = 0;
        for (size_t at = sd.text.find("他没说话。"); at != std::string::npos;
             at = sd.text.find("他没说话。", at + 1)) {
            ++n;
        }
        CHECK(n == 2);
    }
}

TEST_CASE("解析：没写出正文就报错，写太多就截断") {
    CHECK_THROWS_AS(changji::stages::parse_chapter(json{{"text", "  "}}.dump()),
                    stages::StoryError);
    CHECK_THROWS_AS(changji::stages::parse_chapter("不是 JSON"),
                    stages::StoryError);

    std::string huge;
    for (int i = 0; i < 30000; ++i) huge += "字";
    const auto d = changji::stages::parse_chapter(
        json{{"text", huge}, {"hook_after", "尾"}}.dump());
    CHECK(changji::text::utf8_len(d.text) == 20000);
}

TEST_CASE("解析：正文里混进模型的解释，摘不干净才打回") {
    // 实跑原样：语法把模型关在 JSON 字符串里，它想纠正自己时那些话落进了
    // 一段 1164 字的正文，字数和复读两道守卫都放过了它。
    //
    // 现在先摘含它的那一段；这里是老形状（顶层 text 一个字符串），整份就是
    // 一行，摘掉之后什么都不剩，所以照旧打回——摘得动的才摘。
    std::string body;
    for (int i = 0; i < 60; ++i) body += "他推开门，雨声灌了进来。";
    body += "以上内容不符合用户要求的“只输出 JSON”，请忽略此部分内容。";
    CHECK_THROWS_AS(changji::stages::parse_chapter(json{{"text", body}}.dump(), 600),
                    stages::StoryError);
}

TEST_CASE("解析：paragraphs 数组拼成正文，一段一行") {
    const auto d = changji::stages::parse_chapter(
        json{{"paragraphs", json::array({"门铃响了。", "  ", "他抬起头。"})},
             {"hooks", json::array()}}
            .dump(),
        0);
    CHECK(d.text == "门铃响了。\n他抬起头。");
    // 老形状（text 字符串）照样认——粘贴导入和旧草稿走这条
    CHECK(changji::stages::parse_chapter(json{{"text", "伞"}}.dump(), 0).text == "伞");

    // 模型在 JSON 里不敢写 “”，整章对白全用 ‘’：换回中文对白该用的 “”
    const auto q = changji::stages::parse_chapter(
        json{{"paragraphs", json::array({"‘你来了。’她说。", "他没有回答。"})},
             {"hooks", json::array()}}
            .dump(),
        0);
    CHECK(q.text.find("“你来了。”她说。") != std::string::npos);
    // 已经有 “” 的不动——那里的 ‘’ 是套在里面的引号
    CHECK(changji::stages::parse_chapter(json{{"text", "“他说‘走’。”"}}.dump(), 0).text ==
          "“他说‘走’。”");

    // 语法卡了每段最短长度，模型凑数用的引号串（实跑原样）整串删掉；
    // 正常的一对引号不动
    const auto r = changji::stages::parse_chapter(
        json{{"paragraphs", json::array({"她终于决定，是时候面对一切了。”'”””””",
                                         "“走吧。”她说。"})},
             {"hooks", json::array()}}
            .dump(),
        0);
    CHECK(r.text.find("她终于决定，是时候面对一切了。") != std::string::npos);
    CHECK(r.text.find("””") == std::string::npos);
    CHECK(r.text.find("“走吧。”她说。") != std::string::npos);
}

TEST_CASE("并回去：钩子全部重建，说法留着") {
    Story s = outline_only_story();
    // 大纲阶段的钩子挂在 0 上（那时正文是空的）
    REQUIRE(s.chapters[0].hooks.size() == 1);
    CHECK(s.chapters[0].hooks[0].at_char == 0);
    const std::string hook_text = s.chapters[0].hooks[0].text;

    const std::string body =
        "他推门进来，伞还在手里。\n"
        "林晚抬起头。\n"
        "那把伞的骨架断了一根。\n"
        "她认出来了。";
    const Story got = changji::stages::apply_chapter(
        s, "ch01", changji::stages::parse_chapter(
                       good_chapter(body, {{"她认出那把伞", "那把伞的骨架断了一根。"}}).dump()));

    const Chapter& c = got.chapters[0];
    CHECK(c.text == body);

    // **原来挂在 0 上那个不能留**：正文进来之后它就成了「切在章首」
    for (const auto& h : c.hooks) {
        CHECK(h.at_char > 0);
    }

    // 段落边界都登记成候选了
    CHECK(c.hooks.size() >= 3);

    // 大纲那个钩子的说法要留着，并且落在 hook_after 那句之后
    bool found = false;
    for (const auto& h : c.hooks) {
        if (h.text != hook_text) continue;
        found = true;
        const auto chars = changji::text::utf8_chars(c.text);
        REQUIRE(h.at_char > 0);
        REQUIRE(h.at_char <= static_cast<int>(chars.size()));
        CHECK(chars[static_cast<std::size_t>(h.at_char) - 1] == "。");
    }
    CHECK(found);
    CHECK(got.validate().empty());

    // 别的章一个字没动
    CHECK(got.chapters[1].text.empty());
    CHECK(got.chapters[1].summary == s.chapters[1].summary);
}

TEST_CASE("并回去：hook_after 查不到就挂章尾") {
    Story s = outline_only_story();
    const Story got = changji::stages::apply_chapter(
        s, "ch01",
        changji::stages::parse_chapter(
            good_chapter("就这么一段。\n没有第二段。",
                         {{"她认出那把伞", "正文里没有这句"}})
                .dump()));
    const Chapter& c = got.chapters[0];
    bool at_end = false;
    for (const auto& h : c.hooks) {
        if (!h.text.empty() && h.at_char == c.text_len()) at_end = true;
    }
    CHECK(at_end);
    CHECK(got.validate().empty());
}

TEST_CASE("POST /api/story/chapter：写完落库，章节计划跟着盖到整章") {
    const fs::path root = fresh_project("展开一章");
    ProjectStore store(root);
    Story s = outline_only_story();
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);
    const std::size_t before = s.plan.size();

    // 三千字的一章。每段都要不一样，理由同上面那处：一模一样的三十段是复读机。
    std::string body;
    for (int i = 0; i < 30; ++i) {
        body += "第" + std::to_string(i) + "段：";
        for (int k = 0; k < 96; ++k) body += "字";
        body += "\n";
    }
    llm::ReplayClient client({good_chapter(body).dump()});
    pipeline::CancelToken tok;

    const auto r = http::post_story_chapter(
        json{{"project", p_str(root)}, {"chapter_id", "ch01"}}, client, tok);
    CHECK(r.status == 200);
    CHECK(r.body.at("chars").get<int>() > 2000);
    // 60 秒的容量才 900 字，而一章的目标是三千——章是故事单元，不按时长量
    CHECK(r.body.at("target_chars").get<int>() ==
          changji::stages::kChapterTargetChars);

    // **这一个是直接落库的**，不像别的几个回草稿
    const Story saved = store.load_story();
    CHECK(saved.written_chapters() == 1);
    // 一章一条：章变长了条数不变，但这一条的区间要盖到新写的整章——
    // 章节计划是存下来的，不重算就还是展开前那条 [0, 0)
    CHECK(saved.plan.size() == before);
    const Chapter* written = saved.chapter_by_id("ch01");
    REQUIRE(written != nullptr);
    bool found = false;
    for (const auto& p : saved.plan) {
        if (p.from_chapter != "ch01") continue;
        found = true;
        CHECK(p.from_char == 0);
        CHECK(p.to_char == written->text_len());
    }
    CHECK(found);
    CHECK(saved.validate().empty());

    SUBCASE("已经有正文了要显式 overwrite") {
        llm::ReplayClient c2({good_chapter(long_body("重写的正文。")).dump()});
        pipeline::CancelToken t2;
        CHECK_THROWS_AS(
            http::post_story_chapter(
                json{{"project", p_str(root)}, {"chapter_id", "ch01"}}, c2, t2),
            http::ApiError);
        CHECK(c2.calls().empty());

        llm::ReplayClient c3({good_chapter(long_body("重写的正文。")).dump()});
        pipeline::CancelToken t3;
        const auto again = http::post_story_chapter(
            json{{"project", p_str(root)},
                 {"chapter_id", "ch01"},
                 {"overwrite", true}},
            c3, t3);
        CHECK(again.status == 200);
        CHECK(store.load_story().chapters[0].text.rfind("重写的正文。", 0) == 0);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapter：没有这一章") {
    const fs::path root = fresh_project("没这章");
    ProjectStore store(root);
    store.save_story(outline_only_story());
    llm::ReplayClient client({good_chapter(long_body("x")).dump()});
    pipeline::CancelToken tok;
    try {
        http::post_story_chapter(
            json{{"project", p_str(root)}, {"chapter_id", "ch99"}}, client, tok);
        FAIL("应该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 404);
    }
    CHECK(client.calls().empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("展开正文之后，写剧本拿到的是真正文不是梗概") {
    Story s = outline_only_story();
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);

    // 没展开正文时，episode_text 返回空，上下文里只能摆梗概
    CHECK(changji::stages::episode_text(s, s.plan[0]).empty());
    const std::string before = changji::stages::render_script_context(
        s, s.plan[0], "", changji::stages::chapter_scene_plan(s, s.plan[0]));
    CHECK(before.find("他推门进来，伞还在手里。") != std::string::npos);  // 梗概

    // 同 long_body 那段注释：合成的正文要真的不重样，否则复读守卫判废，
    // 而它判得对。
    const std::string body = long_body("这是真正的正文内容。");
    s = changji::stages::apply_chapter(
        s, "ch01", changji::stages::parse_chapter(good_chapter(body).dump()));
    s.plan = changji::stages::plan_episodes(s, 60.0);

    CHECK_FALSE(changji::stages::episode_text(s, s.plan[0]).empty());
    const std::string after = changji::stages::render_script_context(
        s, s.plan[0], "", changji::stages::chapter_scene_plan(s, s.plan[0]));
    CHECK(after.find("这是真正的正文内容") != std::string::npos);
}

// ---- 批量展开正文 ----

namespace {

/// 等这一轮长跑作业跑完。跑在工作线程上，测试里得等它。
void wait_writer_done() {
    for (int i = 0; i < 600; ++i) {
        if (!changji::pipeline::jobs().running(changji::pipeline::JobKind::Write)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FAIL("批量展开跑了十几秒还没结束");
}

}  // namespace

TEST_CASE("POST /api/story/chapters：一口气展开，每写完一章就落库") {
    const fs::path root = fresh_project("批量展开");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);
    REQUIRE(s.chapters.size() == 2);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        good_chapter(long_body("第一章的正文。\n他推门进来。\n"),
                     {{"他终于来了", "他推门进来。"}})
            .dump(),
        good_chapter(long_body("第二章的正文。\n她终于开口。\n"),
                     {{"她开口了", "她终于开口。"}})
            .dump(),
    });

    const auto r = http::post_story_chapters(json{{"project", p_str(root)}}, client);
    CHECK(r.status == 200);
    CHECK(r.body.at("started").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 2);

    wait_writer_done();

    const Story saved = store.load_story();
    CHECK(saved.written_chapters() == 2);
    CHECK(saved.chapters[0].text.find("第一章的正文") != std::string::npos);
    CHECK(saved.chapters[1].text.find("第二章的正文") != std::string::npos);
    CHECK(saved.validate().empty());

    // **第二章的提示词里要带着第一章的结尾。** 每一轮重读盘上的故事就是
    // 为了这个——不重读的话每一章都以为自己接的是空的上一章。
    REQUIRE(client->calls().size() == 2);
    CHECK(client->calls()[0].prompt.find("【上一章是这么结束的】") ==
          std::string::npos);
    CHECK(client->calls()[1].prompt.find("【上一章是这么结束的】") !=
          std::string::npos);
    CHECK(client->calls()[1].prompt.find("他推门进来") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：砸了再要一次，第二次成了就当没事") {
    // 实跑里最常见的砸法是模型把章节标题填进了正文字段，于是正文只有十几
    // 个字。采样带随机种子，再要一次通常就对了——2026-09-11 实跑四章砸了
    // 两章，而这两章的失败彼此无关。
    const fs::path root = fresh_project("砸了重来");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM);
    store.save_story(s);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        "模型今天想聊点别的",  // 第一章头一次：不是 JSON
        json{{"text", long_body("第一章第二次写出来了。")}}.dump(),
        json{{"text", long_body("第二章写出来了。")}}.dump(),
    });
    http::post_story_chapters(json{{"project", p_str(root)}}, client);
    wait_writer_done();

    const Story saved = store.load_story();
    CHECK(saved.chapters[0].text.find("第一章第二次") != std::string::npos);
    CHECK(saved.chapters[1].text.find("第二章") != std::string::npos);
    CHECK(client->calls().size() == 3);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：两次都砸了才算砸，别的照写") {
    const fs::path root = fresh_project("写砸一章");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM);
    store.save_story(s);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        "模型今天想聊点别的",    // 第一章头一次
        "还是想聊点别的",        // 第一章重试
        "第三次也没写",          // 第一章最后一次（软闸关了，但这是硬闸）
        json{{"text", long_body("第二章写出来了。")}}.dump(),
    });
    http::post_story_chapters(json{{"project", p_str(root)}}, client);
    wait_writer_done();

    const Story saved = store.load_story();
    CHECK(saved.chapters[0].text.empty());
    CHECK_FALSE(saved.chapters[1].text.empty());
    // **就多要两次，不是要到成功为止。** 提示词真有毛病时，重试到底只会
    // 把一次失败变成一小时失败。第三次会把软闸关掉（能用但不够好的收下），
    // 而这里三次都不是 JSON——硬闸，收不了。
    CHECK(client->calls().size() == 4);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：停下来说的是「正文」那一句，不是写作槽的兜底") {
    // **2026-09-18 撞过。** 展开正文这条的停止文案一度被收编成
    // `pipeline::kWriteStoppedMessage`（写作槽的兜底），理由是"字一样"。
    // 字一样是巧合：展开的是章**正文**，另外两件写作活留下的是剧本。
    // 收编之后，谁把那个兜底改得贴合「写全片」，这一条就一声不响地跟着
    // 改口——而当时**没有任何用例会红**。
    //
    // 所以这儿比的是**字面量**、不是常量：改 `kStoryChaptersStoppedMessage`
    // 会红（那正是要人停下来想一想的时刻），改兜底不会（它够不着这一条）。
    const fs::path root = fresh_project("正文停止文案");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM);
    store.save_story(s);
    REQUIRE(s.chapters.size() == 2);

    std::vector<std::string> many;
    for (int i = 0; i < 8; ++i) {
        many.push_back(json{{"text", long_body("正文。")}}.dump());
    }
    auto client = std::make_shared<llm::ReplayClient>(many);
    REQUIRE(http::post_story_chapters(json{{"project", p_str(root)}}, client)
                .status == 200);
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    CHECK(pipeline::jobs().snapshot(pipeline::JobKind::Write).at("error") ==
          "已手动停止。已经写好的几章留着。");
    wait_writer_done();

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：拦住的几种情况") {
    const fs::path root = fresh_project("批量拦住");
    ProjectStore store(root);
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{json{{"text", "x"}}.dump()});

    SUBCASE("还没有故事") {
        CHECK_THROWS_AS(
            http::post_story_chapters(json{{"project", p_str(root)}}, client),
            http::ApiError);
    }

    SUBCASE("每一章都有正文了") {
        Story s;
        Chapter c;
        c.chapter_id = "ch01";
        c.title = "写过的";
        c.text = "已经有正文了。";
        s.chapters.push_back(c);
        store.save_story(s);
        try {
            http::post_story_chapters(json{{"project", p_str(root)}}, client);
            FAIL("应该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 400);
        }
        CHECK(client->calls().empty());
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("大纲流式：半份 JSON 里挑得出东西，而且不许抛") {
    // 写大纲要三四十秒，攒齐了再蹦出来的话那几十秒界面上一个字都没有。
    // 所以边写边推——推的就是这个函数从半份 JSON 里挑出来的几样。
    //
    // ⚠️ **半份 JSON 里任何字段都可能是 null。** `"logline":` 刚写完还没
    // 开始写值的那一帧就是。拿 value(..., "") 去取的话 nlohmann 在 null 上
    // 抛 type_error——一条生成会为了推一帧进度整个挂掉。
    using changji::stages::PartialJson;

    const std::string full =
        R"({"premise":"她回到老家","logline":"葬礼上遇见前任","genre":"都市",)"
        R"("tone":"克制","characters":[{"name":"林岚","identity":"记者"}],)"
        R"("chapters":[{"title":"回家","summary":"她下了车。","hook":"门没锁"},)"
        R"({"title":"葬礼","summary":"雨很大。"}]})";

    // 一个字一个字喂，每一帧都要挑得出东西且不抛
    PartialJson p;
    for (std::size_t i = 0; i < full.size(); ++i) {
        p.feed(full.substr(i, 1));
        const auto snap = p.snapshot();
        if (!snap.is_object()) continue;
        json msg;
        CHECK_NOTHROW(msg = http::outline_progress_payload(snap));
        // 形状永远是全的，字段可以是空串——界面按"有没有字"决定摆不摆，
        // 缺字段的话那边要写一堆 ?. 才不炸。
        CHECK(msg.contains("logline"));
        CHECK(msg.at("chapters").is_array());
    }

    // 最后一帧：该有的都有
    const auto done = http::outline_progress_payload(p.snapshot());
    CHECK(done.at("premise") == "她回到老家");
    CHECK(done.at("logline") == "葬礼上遇见前任");
    CHECK(done.at("genre") == "都市");
    CHECK(done.at("characters").size() == 1);
    CHECK(done.at("characters")[0].at("name") == "林岚");
    REQUIRE(done.at("chapters").size() == 2);
    CHECK(done.at("chapters")[0].at("title") == "回家");
    CHECK(done.at("chapters")[0].at("hook") == "门没锁");
    // 没写到的字段是空串，不是 null——界面直接往模板里塞
    CHECK(done.at("chapters")[1].at("hook") == "");

    // null 和缺字段都要当空串，一个都不许抛
    const json weird = {{"logline", nullptr},
                        {"chapters", {{{"title", nullptr}}, 42, "不是对象"}}};
    json msg;
    CHECK_NOTHROW(msg = http::outline_progress_payload(weird));
    CHECK(msg.at("logline") == "");
    // 不是对象的那两项直接跳过，不要在列表里留个空壳
    CHECK(msg.at("chapters").size() == 1);
    CHECK(msg.at("chapters")[0].at("title") == "");
}

TEST_CASE("POST /api/story/outline：body 里给了 variation 就用给的") {
    // **这一条是「对照组」成立的前提。** 量"改完到底有没有变得不一样"时，
    // 得先有一组"和改之前一模一样"的跑法——送 variation = 0，两段随机的
    // 底子都不拼，提示词逐字节还是老样子。没有这一档的话，十次都不一样也
    // 说明不了是这次改的功劳。
    const fs::path root = fresh_project("对照组");
    pipeline::CancelToken tok;
    const json base{{"project", p_str(root)}, {"premise", "深夜便利店"}};

    llm::ReplayClient a({good_outline().dump()});
    json ba = base;
    ba["variation"] = 0;
    CHECK(http::post_story_outline(ba, a, tok).status == 200);

    llm::ReplayClient b({good_outline().dump()});
    CHECK(http::post_story_outline(ba, b, tok).status == 200);

    REQUIRE(a.calls().size() == 1);
    REQUIRE(b.calls().size() == 1);
    // 同一个 variation 两次调用，提示词一个字节都不差
    CHECK(a.calls()[0].prompt == b.calls()[0].prompt);
    CHECK(a.calls()[0].prompt.find("【这个故事里的人怎么取名】") ==
          std::string::npos);

    SUBCASE("不给这个字段就现摇一个，两次不一样") {
        llm::ReplayClient c({good_outline().dump()});
        llm::ReplayClient d({good_outline().dump()});
        CHECK(http::post_story_outline(base, c, tok).status == 200);
        CHECK(http::post_story_outline(base, d, tok).status == 200);
        CHECK(c.calls()[0].prompt != d.calls()[0].prompt);
        CHECK(c.calls()[0].prompt.find("【这个故事里的人怎么取名】") !=
              std::string::npos);
    }

    SUBCASE("给个非零的数，两次还是一样") {
        json bv = base;
        bv["variation"] = 987654;
        llm::ReplayClient c({good_outline().dump()});
        llm::ReplayClient d({good_outline().dump()});
        CHECK(http::post_story_outline(bv, c, tok).status == 200);
        CHECK(http::post_story_outline(bv, d, tok).status == 200);
        CHECK(c.calls()[0].prompt == d.calls()[0].prompt);
        CHECK(c.calls()[0].prompt.find("【这个故事里的人怎么取名】") !=
              std::string::npos);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/outline：带 stream 也照样回那份草稿") {
    // 流式是**加的一条路**，不是换一条：不带 stream 的老客户端、curl、
    // 对拍脚本走的还是原来那条，一个字没变；带了 stream 也只是多推几帧，
    // 最后那份 body 必须一模一样。
    const fs::path root = fresh_project("流式大纲");
    pipeline::CancelToken tok;

    llm::ReplayClient plain({good_outline().dump()});
    const auto a = http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"}}, plain, tok);

    llm::ReplayClient streamed({good_outline().dump()});
    const auto b = http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"},
             {"stream", "outline-test"}},
        streamed, tok);

    CHECK(a.status == 200);
    CHECK(b.status == 200);
    CHECK(a.body == b.body);

    // 提示词也不该因为流式而变。
    //
    // **不能直接比整串了。** 2026-09-14 起每按一次「写大纲」都会摇一个新的
    // 底子（几个姓 + 一种名字形状），两次调用抽到的必然不同——那正是它存在
    // 的理由，见 stages::build_outline_names。所以把那一段摘掉再比：
    // 摘掉之后还一模一样，才说明流式没动别的地方。
    REQUIRE(streamed.calls().size() == 1);
    const auto without_names = [](std::string t) {
        const std::string head = "【这个故事里的人怎么取名】";
        const std::string tail = "梗概里已经出现过的人名照用，一个字不要改。";
        const auto i = t.find(head);
        const auto j = t.find(tail);
        REQUIRE(i != std::string::npos);
        REQUIRE(j != std::string::npos);
        t.erase(i, j + tail.size() - i);
        return t;
    };
    CHECK(without_names(streamed.calls()[0].prompt) ==
          without_names(plain.calls()[0].prompt));
    // 摘之前是不一样的——不然上面那一下就等于什么都没验
    CHECK(streamed.calls()[0].prompt != plain.calls()[0].prompt);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("写章正文：【地方】只给这一章用得着的那几个") {
    // **拿措辞治"给多了"是治不好的。** 原来这儿发的是全片地点清单，然后在
    // 硬性要求里花一条叫模型「用得着哪一两个就只写那一两个，跑遍全城说明
    // 是在拿地点凑场数」。二十个地方摆在眼前，模型自然会用。章自己带着
    // locations 名单，照它筛就行——上下文小一截，那条规则也跟着没了。
    Story st = sample_story();
    st.locations.clear();
    for (const char* n : {"天台", "便利店", "停车场", "警局"}) {
        models::StoryLocation l;
        l.name = n;
        l.what = std::string(n) + "的样子";
        st.locations.push_back(l);
    }
    REQUIRE(st.chapters.size() >= 1);
    st.chapters[0].locations = {"天台", "警局"};

    const std::string p =
        stages::build_chapter_prompt(st, st.chapters[0].chapter_id,
                                     StyleLine::REALISTIC);
    // **只看【地方】那一段**：别处（梗概、章摘要）提到某个地名是另一回事，
    // 这一条钉的是"清单里发了谁"。
    const std::size_t from = p.find("【地方】（这一章用得着的）");
    REQUIRE(from != std::string::npos);
    std::size_t to = p.find("【", from + 3);
    if (to == std::string::npos) to = p.size();
    const std::string block = p.substr(from, to - from);
    CHECK(block.find("天台") != std::string::npos);
    CHECK(block.find("警局") != std::string::npos);
    // 这一章用不着的不发过去
    CHECK(block.find("便利店") == std::string::npos);
    CHECK(block.find("停车场") == std::string::npos);
    // 规则表里那一条没了
    CHECK(p.find("跑遍全城") == std::string::npos);
}

TEST_CASE("写章正文：章里没填地点时照旧给全份，提醒贴在名单旁边") {
    // 大纲那条路上 locations 是"给了就收、没给也不拦"（story_outline.cpp
    // 里写着为什么不敢收紧）。空着的时候宁可多给，也不能让这一章没有地方
    // 可写——那时候把那句提醒贴在名单上，比写在两千字之前的规则表里管用。
    Story st = sample_story();
    st.locations.clear();
    for (const char* n : {"天台", "便利店"}) {
        models::StoryLocation l;
        l.name = n;
        st.locations.push_back(l);
    }
    st.chapters[0].locations.clear();

    const std::string p =
        stages::build_chapter_prompt(st, st.chapters[0].chapter_id,
                                     StyleLine::REALISTIC);
    CHECK(p.find("整个故事的清单") != std::string::npos);
    CHECK(p.find("跑遍全城") != std::string::npos);
    CHECK(p.find("天台") != std::string::npos);
    CHECK(p.find("便利店") != std::string::npos);
}

TEST_CASE("写剧本：【地方】只发这一章用得着的") {
    // 同 stages/chapter_write.cpp 那一处：发全片清单等于请模型跑遍全城，
    // 然后只能拿话去拦。章自己带着 locations 名单，照它筛。
    Story st = sample_story();
    st.locations.clear();
    for (const char* n : {"天台", "便利店", "停车场"}) {
        models::StoryLocation l;
        l.name = n;
        st.locations.push_back(l);
    }
    REQUIRE(!st.chapters.empty());
    st.chapters[0].locations = {"天台"};

    const auto plan = stages::chapter_plan(st, st.chapters[0].chapter_id, 60.0);
    const auto scenes = stages::chapter_scene_plan(st, plan);
    const std::string p =
        stages::render_script_context(st, plan, "", scenes);

    const std::size_t from = p.find("【地方】（这一章用得着的");
    REQUIRE(from != std::string::npos);
    std::size_t to = p.find("【", from + 3);
    if (to == std::string::npos) to = p.size();
    const std::string block = p.substr(from, to - from);
    CHECK(block.find("天台") != std::string::npos);
    CHECK(block.find("便利店") == std::string::npos);
    CHECK(block.find("停车场") == std::string::npos);
    // 场次头只能用清单里的名字这一条，筛不筛都要在
    CHECK(block.find("一字不改") != std::string::npos);
}

TEST_CASE("写剧本：章里没填地点就照旧给全份") {
    Story st = sample_story();
    st.locations.clear();
    for (const char* n : {"天台", "便利店"}) {
        models::StoryLocation l;
        l.name = n;
        st.locations.push_back(l);
    }
    st.chapters[0].locations.clear();

    const auto plan = stages::chapter_plan(st, st.chapters[0].chapter_id, 60.0);
    const auto scenes = stages::chapter_scene_plan(st, plan);
    const std::string p =
        stages::render_script_context(st, plan, "", scenes);
    const std::size_t from = p.find("【地方】（整个故事的清单");
    REQUIRE(from != std::string::npos);
    std::size_t to = p.find("【", from + 3);
    if (to == std::string::npos) to = p.size();
    const std::string block = p.substr(from, to - from);
    CHECK(block.find("天台") != std::string::npos);
    CHECK(block.find("便利店") != std::string::npos);
}

// ── 改稿：打回时稿子和清单一起带出来 ──────────────────────────────────
//
// 用户 2026-09-19：「别整章重掷，把坏在第几处回给模型」。在这之前每一道闸
// 都是"见着就抛"，抛出去的只有一句话，稿子随异常一起没了；上层能做的只剩
// 把同一份提示词再发一遍——chapter_write.cpp 里自己写着「重试只是再掷一次
// 骰子」。

namespace {

/// 一份能过所有闸的稿。两场、每场十几段、三成对白、句句不同。
/// `with_dialogue` 关掉就是一份通篇转述的稿——正好撞「整章几乎没有对白」。
json passable_chapter(bool with_dialogue = true) {
    const auto scene = [&](int base, const char* where, const char* turn,
                           const char* last) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            const int n = base + i;
            if (with_dialogue && i % 3 == 1) {
                paras.push_back("她抬起头，把第" + std::to_string(n) +
                                "把伞往柜台上一推：“这一把不是你的。”");
            } else {
                paras.push_back("第" + std::to_string(n) +
                                "个杯子被放回架子上，水顺着杯沿滑到她的指尖，凉的。");
            }
        }
        return json{{"where", where},
                    {"pov", "林晚"},
                    {"who", json::array({"林晚", "陈默"})},
                    {"goal", "把伞要回来"},
                    {"obstacle", "他不认这把伞"},
                    {"worse", "她发现伞根本不是他带来的"},
                    {"turn", turn},
                    {"paragraphs", paras},
                    {"last_line", last}};
    };
    return json{{"scenes",
                 json::array({scene(1, "深夜，便利店", "伞柄上刻着别人的名字",
                                    "他把伞柄转过来，刻的是另一个名字。"),
                              scene(100, "凌晨，后巷", "来的人不是他",
                                    "门口的风铃响了，进来的人没有带伞。")})}};
}

}  // namespace

TEST_CASE("打回时稿子和整份清单一起带出来，不是只剩一句话") {
    // 一份没对白的稿，最后一段还在点题：两道闸都该记上，稿子也要在。
    json draft = passable_chapter(/*with_dialogue=*/false);
    draft["scenes"][1]["last_line"] = "那一刻，她终于明白了。";
    const std::string raw = draft.dump();

    bool caught = false;
    try {
        changji::stages::parse_chapter(raw);
    } catch (const changji::stages::ChapterRejected& e) {
        caught = true;
        REQUIRE(e.problems().size() == 2);
        // 顺序就是闸门原来的先后：第一条是原来会被抛出来的那一道
        CHECK(e.problems()[0].code == "no_dialogue");
        CHECK(e.problems()[1].code == "on_the_nose");
        CHECK(e.code() == "no_dialogue");
        CHECK(std::string(e.what()).find("另有 1 处") != std::string::npos);
        // 每一条都说清在哪：改稿时模型该去的正是那一处
        CHECK(e.problems()[0].what.find("第 1、2 场一句都没有") != std::string::npos);
        CHECK(e.problems()[1].what.find("第 2 场") != std::string::npos);
        CHECK(e.problems()[1].what.find("终于") != std::string::npos);
        // 稿子还在，而且是清理过的那份
        REQUIRE(e.draft().scenes.size() == 2);
        CHECK(e.draft().scenes[0].paragraphs.size() == 15);   // 14 段 + last_line
        // 打回那句话里不再带「重试一次」——它会原样进改稿清单
        CHECK(std::string(e.what()).find("重试一次") == std::string::npos);
    }
    CHECK(caught);

    // 软闸关掉就过——两道都是软闸
    CHECK_NOTHROW(changji::stages::parse_chapter(raw, 0, false));

    // 手里没稿的那几种照旧是裸的 StoryError：没有稿子可改，只能重掷
    bool plain = false;
    try {
        changji::stages::parse_chapter("这不是 JSON");
    } catch (const changji::stages::ChapterRejected&) {
        FAIL("解析不动的稿没有草稿，不该是 ChapterRejected");
    } catch (const changji::stages::StoryError& e) {
        plain = true;
        CHECK(e.code() == "bad_json");
    }
    CHECK(plain);

    // 能过的照旧过
    CHECK_NOTHROW(changji::stages::parse_chapter(passable_chapter().dump()));
}

TEST_CASE("draft_to_json 还原成 schema 的形状") {
    const auto d = changji::stages::parse_chapter(passable_chapter().dump());
    const auto j = changji::stages::draft_to_json(d);
    REQUIRE(j.contains("scenes"));
    REQUIRE(j["scenes"].size() == 2);
    const auto& s0 = j["scenes"][0];
    // who 是数组（收稿时拼成了「甲、乙」，还原要拆回去）
    REQUIRE(s0["who"].is_array());
    CHECK(s0["who"].size() == 2);
    CHECK(s0["who"][0] == "林晚");
    // last_line 从 paragraphs 末尾拆回去：模型看到的形状要和 schema 一样
    CHECK(s0["paragraphs"].size() == 14);
    CHECK(s0["last_line"] == "他把伞柄转过来，刻的是另一个名字。");
    for (const char* k : {"where", "pov", "goal", "obstacle", "worse", "turn"}) {
        CAPTURE(k);
        CHECK(s0.contains(k));
    }

    // 老形状（顶层 text）退回 {"text": …}
    const auto old = changji::stages::parse_chapter(json{{"text", "一句正文。"}}.dump());
    const auto oj = changji::stages::draft_to_json(old);
    CHECK(oj.contains("text"));
    CHECK_FALSE(oj.contains("scenes"));
}

TEST_CASE("改稿提示词：原来的规矩一字不少，后面接上一稿和清单，只改点到的地方") {
    Story s = outline_only_story();
    const auto d = changji::stages::parse_chapter(passable_chapter().dump());
    const std::vector<changji::stages::ChapterProblem> problems = {
        {"no_dialogue", "整章几乎没有对白（28 段里只有 0 处，第 1、2 场一句都没有）"},
        {"on_the_nose", "第 2 场的最后一段在点题（「终于」）"},
    };
    const std::string p = changji::stages::build_chapter_revision_prompt(
        s, "ch01", StyleLine::REALISTIC, d, problems);
    const std::string base =
        changji::stages::build_chapter_prompt(s, "ch01", StyleLine::REALISTIC);

    // 原来那份一字不少地在前面（去掉末尾那段「只输出 JSON」再比）
    const std::string tail = changji::stages::prompt::chapter_write::kTail;
    REQUIRE(base.size() > tail.size());
    const std::string base_body = base.substr(0, base.size() - tail.size());
    CHECK(p.compare(0, base_body.size(), base_body) == 0);
    CHECK(p.find("硬性要求") != std::string::npos);

    // 上一稿、清单、要求，按这个顺序，最后才是「只输出 JSON」那段
    const auto at = [&](const char* needle) { return p.find(needle); };
    REQUIRE(at("【你上一稿】") != std::string::npos);
    REQUIRE(at("【要改的地方】") != std::string::npos);
    REQUIRE(at("只改上面点到的地方") != std::string::npos);
    CHECK(at("【你上一稿】") < at("【要改的地方】"));
    CHECK(at("【要改的地方】") < at("只改上面点到的地方"));
    CHECK(at("只改上面点到的地方") < p.rfind(tail));
    // 「只输出 JSON」那段只出现一次，而且在最末尾——它后面紧接着贴 schema
    CHECK(p.find(tail) == p.rfind(tail));
    CHECK(p.size() - p.rfind(tail) == tail.size());

    // 清单逐条编号、原话不动
    CHECK(at("1. 整章几乎没有对白（28 段里只有 0 处") != std::string::npos);
    CHECK(at("2. 第 2 场的最后一段在点题") != std::string::npos);
    // 上一稿贴的是清理过的那份，段落原文在里面
    CHECK(at("这一把不是你的") != std::string::npos);
    CHECK(at("他把伞柄转过来，刻的是另一个名字。") != std::string::npos);
    // 缩进 0：换行留着，缩进的空格不留（和 schema 贴过去那份同一个规矩）
    CHECK(at("\n  \"where\"") == std::string::npos);
    CHECK(at("\n\"where\"") != std::string::npos);
}

TEST_CASE("漏了闭引号的那一行补上再解：Qwen3-4B 实跑逮到的形状") {
    // 一段正文以单引号的对白收尾，模型把 ' 当成收口，忘了写那个 "。
    // 整份只坏这一处，原来三道抽取一条都救不回来，整章作废。
    const std::string raw =
        "{\n"
        "  \"scenes\": [\n"
        "    {\n"
        "      \"where\": \"老街转角的恒表铺\",\n"
        "      \"paragraphs\": [\n"
        "        \"她喉咙里卡着一句话，'我...我忘了给它上发条。',\n"
        "        \"消毒水味混着汗水腥气，蒋雨的指尖触到冷金属表面。\"\n"
        "      ],\n"
        "      \"last_line\": \"指针停在十三点位置\"\n"
        "    }\n"
        "  ]\n"
        "}\n";
    const auto j = changji::stages::extract_json(raw);
    REQUIRE(j.contains("scenes"));
    const auto& paras = j["scenes"][0]["paragraphs"];
    REQUIRE(paras.size() == 2);
    CHECK(paras[0] == "她喉咙里卡着一句话，'我...我忘了给它上发条。'");
    CHECK(paras[1] == "消毒水味混着汗水腥气，蒋雨的指尖触到冷金属表面。");
    CHECK(j["scenes"][0]["last_line"] == "指针停在十三点位置");

    // **这一道的判据仍然是窄的**：引号成对的行一个字不动，漏在中间的它不猜。
    //
    // 但 `{"a": "x" y", "b": 1}` 这一种从 2026-09-20 起**解得出来了**，
    // 猜的人不是它，是后面那道 `escape_stray_quotes`（正文里的裸引号转义掉
    // 再解一遍，见那儿的注释）：`x` 后面那个引号跟着的是 ` y` 而不是
    // `,:]}`，于是当成正文里的引号补转义，读成 `a = x" y`。
    //
    // 那一道明写着自己会在"模型漏了两个字符串之间的逗号"上猜错，代价换的是
    // 20% 的整章不再作废。这儿钉住它现在的读法——哪天那道梯子撤了或者改了
    // 判据，这条会红，而不是悄悄变回抛异常。
    CHECK(changji::stages::extract_json("{\"a\": \"x\" y\", \"b\": 1}")["a"] ==
          "x\" y");
    CHECK(changji::stages::extract_json("{\"a\": \"逗号在,里面\"}")["a"] == "逗号在,里面");
}

TEST_CASE("改编成剧本：多于一场时一场一次调用，不是整章一次") {
    // **2026-09-20 量出来的：瓶颈是单次产出长度，不是模型大小。**
    //
    // 这一步整章一次要模型吐 10~17K 字的 JSON，而超过一个长度谁都撑不住——
    // `finish_reason` 是 stop（正常写完），但 `scenes` 开成 `{` 关成 `]`。
    // 实测的断点：Qwen3-8B 约 6K 字（5345/6056 合法，9071 起全坏），
    // glm-5.3 约 13K 字（10799/13045 合法，13703 起全坏）。
    // **换更大的模型只是把断点往后推。**
    //
    // 拆开之后（每场产出中位 3901 字）8B 的剧本调用从 0/6 变成 27/30。
    //
    // 这条用例钉的就是"拆了没有"：一章三场就该发三次，每次的 schema 里
    // 只有那一场的键。合回整章一次的话它会红——而真发出去只会"偶尔坏"，
    // 坏的时候还长得像模型不行。
    const fs::path root = fresh_project("剧本逐场");
    ProjectStore store(root);
    Story s = sample_story();
    // **场是从正文里切出来的**（chapter_scene_plan 读 chapter.scenes 的
    // from_char/to_char），所以这一章得先有正文、再有几场。sample_story()
    // 那一章一场都没有，直接用的话下面那条 REQUIRE 就会红——而红在前提上，
    // 等于这条用例什么都没测。
    Chapter& ch = s.chapters.front();
    ch.text.clear();
    for (int i = 0; i < 90; ++i) ch.text += "她把抹布拧干，水滴落在地板上。";
    const int len = ch.text_len();
    ch.scenes.clear();
    for (int i = 0; i < 3; ++i) {
        models::Scene sc;
        sc.from_char = len * i / 3;
        sc.to_char = len * (i + 1) / 3;
        sc.where = "第" + std::to_string(i + 1) + "场：便利店，夜";
        sc.pov = "林晚";
        ch.scenes.push_back(sc);
    }
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);

    const auto scenes = changji::stages::chapter_scene_plan(s, s.plan.front());
    REQUIRE(scenes.size() > 1);          // 不然这条用例没在测东西

    // 每场回一份只带那一场的剧本。ReplayClient 按顺序发。
    std::vector<std::string> replies;
    for (const auto& sc : scenes) {
        // **得有对白**：整章一句台词都没有的话，下游那道「默片」闸会拦下来
        // ——那时候红的是闸门不是这条用例要钉的东西。
        json beats = json::array();
        for (int i = 0; i < std::max(3, sc.min_beats); ++i) {
            const bool head = (i == 0);
            const bool talk = (i % 2 == 1);
            beats.push_back({{"kind", head ? "scene" : (talk ? "dialogue" : "action")},
                             {"speaker", talk ? "林晚" : ""},
                             {"text", head ? "夜 · 内 · 便利店"
                                           : (talk ? "伞我带来了。" : "她把抹布拧干。")},
                             {"characters", json::array()},
                             {"delivery", ""}, {"subtext", ""}, {"fx", ""}});
        }
        replies.push_back(json{{"title", "打烊"},
                               {"logline", "一句话"},
                               {"scenes", {{sc.key, {{"beats", beats}}}}}}.dump());
    }

    llm::ReplayClient client(replies);
    pipeline::CancelToken tok;
    const auto r = http::post_script_write(
        json{{"project", p_str(root)}, {"episode_id", s.plan.front().episode_id},
             {"premise", "县城便利店的一夜"}},
        client, tok);
    CHECK(r.status == 200);

    // **判据一：发了几次。** 一场一次。
    CHECK_MESSAGE(client.calls().size() == scenes.size(),
                  "该一场一次，实际发了 " << client.calls().size() << " 次");

    // **判据二：每次的 schema 里只有那一场。** 只数次数的话，
    // "发了三次但每次都要整章"照样绿。
    // 场的键在 `properties.scenes.properties` 底下，顶层只有
    // title / logline / scenes 三个。
    for (std::size_t i = 0; i < client.calls().size(); ++i) {
        const auto& props = client.calls()[i]
                                .schema.at("properties").at("scenes").at("properties");
        CAPTURE(i);
        CHECK(props.contains(scenes[i].key));
        for (std::size_t k = 0; k < scenes.size(); ++k) {
            if (k != i) CHECK_FALSE(props.contains(scenes[k].key));
        }
    }

    // **判据三：各场的拍子合起来了**，不是只留最后一场。
    const std::string script = r.body.value("script", std::string());
    CHECK_FALSE(script.empty());
    CHECK(script.find("她把抹布拧干。") != std::string::npos);
}

TEST_CASE("展开一章：JSON 没收好这种非配置错，再掷一次而不是当场报错") {
    // 2026-09-19 拿 Qwen3-4B 实跑：第一稿只是形状坏了，原来单章这条当场 502，
    // 人只能自己再按一下——而按一下发的还是同一份提示词。配错了（401/403/404）
    // 和人按停的照旧不重来。
    const fs::path root = fresh_project("坏JSON重掷");
    ProjectStore store(root);
    Story s = outline_only_story();
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);

    std::string body;
    for (int i = 0; i < 30; ++i) {
        body += "第" + std::to_string(i) + "段：";
        for (int k = 0; k < 96; ++k) body += "字";
        body += "\n";
    }
    llm::ReplayClient client({"这不是 JSON，{半截", good_chapter(body).dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_story_chapter(
        json{{"project", p_str(root)}, {"chapter_id", "ch01"}}, client, tok);
    CHECK(r.status == 200);
    REQUIRE(client.calls().size() == 2);
    // 手里没稿，第二次发的还是普通那份，不是改稿
    CHECK(client.calls()[1].prompt == client.calls()[0].prompt);
    CHECK(store.load_story().written_chapters() == 1);
}

TEST_CASE("对白写成『』或「」也认：整章没有 “ 时换回来，守卫才数得到") {
    // 2026-09-19 Qwen3-4B 实跑：改稿那一轮把对白补上了，全是『这表……能修吗？』，
    // 而 no_dialogue 只数 “——这一章在它眼里还是零对白，靠软闸放行才落库。
    json paras = json::array();
    for (int i = 0; i < 12; ++i) {
        paras.push_back("第" + std::to_string(i) + "个杯子被放回架子上，水顺着杯沿滑到她的指尖，凉的。");
        if (i % 3 == 1) {
            paras.push_back("她抬起头，把第" + std::to_string(i) + "把伞往柜台上一推：『这一把不是你的。』");
        }
    }
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"who", json::array({"林晚", "陈默"})},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"worse", "她发现伞根本不是他带来的"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "他把伞柄转过来，刻的是另一个名字。"}});
    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    CHECK(d.text.find("『") == std::string::npos);
    CHECK(d.text.find("“这一把不是你的。”") != std::string::npos);

    // 「」同理
    std::string raw = json{{"scenes", scenes}}.dump();
    std::string::size_type at = 0;
    while ((at = raw.find("『", at)) != std::string::npos) raw.replace(at, std::string("『").size(), "「");
    at = 0;
    while ((at = raw.find("』", at)) != std::string::npos) raw.replace(at, std::string("』").size(), "」");
    const auto d2 = changji::stages::parse_chapter(raw);
    CHECK(d2.text.find("“这一把不是你的。”") != std::string::npos);

    // **已经有 “ 的一章，角括号原样留着**——那时候它多半是书名号的用法
    // （「『修表匠』那一章」），换成引号就把一句话劈成两半。
    json mixed = json::array();
    for (int i = 0; i < 12; ++i) {
        mixed.push_back("第" + std::to_string(i) + "个杯子被放回架子上，水顺着杯沿滑到她的指尖，凉的。");
        if (i % 3 == 1) {
            mixed.push_back("她抬起头，把第" + std::to_string(i) + "把伞往柜台上一推：“这一把不是你的。”");
        }
    }
    mixed.push_back("她把书推过去：“你看看『修表匠』那一章。”");
    scenes[0]["paragraphs"] = mixed;
    const auto d3 = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    CHECK(d3.text.find("『修表匠』") != std::string::npos);
}

TEST_CASE("写正文要说清少想一格：不说的话智谱按 max 来，整章会写在思考里") {
    // 2026-09-19 实测逮到（llm_log 里 20260919-203018-178-0003）：glm-5.3
    // 写一章跑了 **977 秒，思考 169394 字、正文 0 字**，finish_reason 是
    // length。翻开那份 thinking.txt——整章连分场带对白全写完了，末了一句
    // 「All good. Now produce final JSON.」，而输出预算已经用光。界面上的
    // 样子就是：展开整章按下去十六分钟，编辑器一个字都没长出来，最后弹一句
    // 「输出达到长度上限」。
    //
    // 拆分镜那一步 2026-09-17 撞的是同一件事，药方也是这一行
    // （pipeline/storyboard_run.cpp）。写正文这一步当时漏了。
    const Story st = sample_story();
    REQUIRE_FALSE(st.chapters.empty());
    const llm::Request req =
        http::chapter_request(st, st.chapters[0].chapter_id, StyleLine::REALISTIC);

    // **判据在请求体上，不在 `req` 那一栏上。** 2026-09-19 这个档位从
    // chapter_request 里那行 `= "high"` 搬进了配置，2026-09-20 又从 high
    // 降到 low——搬的和降的都是"取什么值"，**要守的东西一直没变：写正文这一
    // 趟真发出去的那份里得有这个字段**。不发就是按智谱的默认 max 来，而 max
    // 下模型会把整章写在思考里（977 秒、思考 169394 字、正文 0 字）。
    //
    // 降到 low 的数（2026-09-20 实测）：一次 315 秒 → 64 秒，思考从正文的
    // 8.2 倍掉到 0.4 倍，闸门第一轮通过率 50%（n=48）→ 67%（n=16，样本小，
    // 只能说不比 high 差）。代价（JSON 写得糙）先在 a06acec 治过才改的默认。
    config::LLMConfig cfg;
    cfg.model = "glm-5.3";
    const json sent = json(llm::build_payload(cfg, req));
    CHECK(sent.at("reasoning_effort") == "low");

    SUBCASE("打回之后那一轮也带着：改稿和头一稿发的是同一套旋钮") {
        stages::ChapterDraft d;
        d.text = "正文";
        std::vector<stages::ChapterProblem> ps;
        ps.push_back({"too_short", "只写出了 12 个字"});
        const stages::ChapterRejected rejected(d, ps);
        const llm::Request again = http::chapter_revision_request(
            st, st.chapters[0].chapter_id, StyleLine::REALISTIC, rejected);
        const json sent_again = json(llm::build_payload(cfg, again));
        CHECK(sent_again.at("reasoning_effort") == "low");
    }
}
