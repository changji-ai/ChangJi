// 图标条：哪几格有东西。
//
// **这一层最安静的坏法是「点该出现没出现」**——人永远不知道有东西可看，
// 而且不报任何错。所以判据全钉在这儿：每一条都是"刚好还不算有"和"刚好算有"
// 那一对。
//
// 判据一律写成**正面条件**（CLAUDE.md 第四条）。下面那几条 `刚好还不算有`
// 就是在钉这个：大纲写完有十二章但一个字正文都没有、一章十七个空壳镜头，
// 两种都长得很像"已经有了"。

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <string>

#include "http/rail.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "util/paths.hpp"
#include "desktop_sources.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;
using namespace changji;

namespace {

/// 按 key 找那一格。
bool present_of(const std::vector<http::RailSlot>& slots, const std::string& key) {
    for (const auto& s : slots) {
        if (s.key == key) return s.present;
    }
    FAIL("图标条里没有这一格：" << key);
    return false;
}

}  // namespace

TEST_CASE("空项目：一格都不亮") {
    const auto slots = http::rail_present(json::object(), json::object(), json::object());
    REQUIRE(slots.size() == 5);
    for (const auto& s : slots) {
        CAPTURE(s.key);
        CHECK_FALSE(s.present);
        // 名字不能是空的：界面上那就是一个点不开的空图标。
        CHECK(!s.label.empty());
    }
}

TEST_CASE("故事：有章节不算，有正文才算") {
    // **这儿用的是 `/api/story` 真正的回包形状**：包了一层，顶层的
    // `chapters` 是数目不是数组。照着想当然的形状写用例，就会出现"用例全绿
    // 而界面一格不亮"——2026-09-21 实撞过一次。
    json story;
    story["story"] = json::object();
    story["chapters"] = 2;
    story["written"] = 0;
    story["empty"] = false;

    // 大纲刚写完：两章齐了，一个字正文都没有。这时候点进「故事」只有
    // 一张空表——图标不该亮。
    CHECK_FALSE(present_of(http::rail_present(json::object(), story, json::object()), "story"));

    story["written"] = 1;
    CHECK(present_of(http::rail_present(json::object(), story, json::object()), "story"));
}

TEST_CASE("镜头：空壳不算") {
    // 一章 17 个 `shot_id` 是空串的空壳照样让数组非空，而回包里的 `shots`
    // 这个数已经把它们剔掉了（readonly.cpp）。这儿要跟着用那个数。
    json p;
    p["episodes"] = json::array({{{"episode_id", "ep01"}, {"shots", 0}}});
    CHECK_FALSE(present_of(http::rail_present(p, json::object(), json::object()), "shots"));

    p["episodes"][0]["shots"] = 17;
    CHECK(present_of(http::rail_present(p, json::object(), json::object()), "shots"));
}

TEST_CASE("剧本：字数是 0 就不算") {
    json p;
    p["episodes"] = json::array({{{"episode_id", "ep01"}, {"script_chars", 0}}});
    CHECK_FALSE(present_of(http::rail_present(p, json::object(), json::object()), "script"));

    p["episodes"][0]["script_chars"] = 1232;
    CHECK(present_of(http::rail_present(p, json::object(), json::object()), "script"));
}

TEST_CASE("设定：没名字的空壳不算") {
    json p;
    p["characters"] = json::array({{{"name", ""}}});
    p["locations"] = json::array();
    CHECK_FALSE(present_of(http::rail_present(p, json::object(), json::object()), "assets"));

    p["characters"][0]["name"] = "董平";
    CHECK(present_of(http::rail_present(p, json::object(), json::object()), "assets"));
}

TEST_CASE("片子：预告也算——它也是能看的东西") {
    json o;
    o["files"] = json::array();
    o["previews"] = json::array();
    CHECK_FALSE(present_of(http::rail_present(json::object(), json::object(), o), "film"));

    o["previews"] = json::array({{{"name", "ep01.mp4"}}});
    CHECK(present_of(http::rail_present(json::object(), json::object(), o), "film"));
}

TEST_CASE("少一份回包也算得出来，不抛") {
    // 老项目没有 story.json、成片目录还没建，都是常事。那几档按"还没有"算，
    // 别的格子不受影响。
    json p;
    p["episodes"] = json::array({{{"episode_id", "ep01"}, {"script_chars", 900}}});
    std::vector<http::RailSlot> slots;
    CHECK_NOTHROW(slots = http::rail_present(p, json(), json()));
    CHECK(present_of(slots, "script"));
    CHECK_FALSE(present_of(slots, "story"));
}

// ---- 引擎多一格，桌面端就得跟 ----
//
// 图标条那几格是**引擎定的**（`rail_present`），而桌面端有两处要按 key 分：
// 画哪个图标（`SlotIcon.qml`）、开哪块面板（`Peek.qml` 那张 `customSlots`）。
//
// **漏掉一处都不报错。** 漏了图标就是一个点得开但空白的方框；漏了面板名单
// 就是那一格退回"一段纯文本"，而旁边可能还立着一块专门为它写的面板。
//
// 做法和 `test_enums.cpp` 那条一样（CLAUDE.md 第八条：收不动就让它会响）：
// 直接读源码。区别是这一条跨语言——读的是 QML。
TEST_CASE("图标条那几格：桌面端画得出图标、开得出面板，一格都不许漏") {
    if (!changji_test::desktop_sources()) return;
    const auto slots = http::rail_present(json::object(), json::object(), json::object());
    REQUIRE(!slots.empty());

    const auto slurp = [](const std::string& rel) {
        const std::string path = std::string(CHANGJI_DESKTOP_QML_DIR) + "/" + rel;
        std::ifstream in(path);
        REQUIRE_MESSAGE(in.good(), "读不到 " << path);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string icon = slurp("SlotIcon.qml");
    const std::string peek = slurp("Peek.qml");

    // `customSlots` 那一行：`readonly property var customSlots: [...]`
    const auto list_at = peek.find("customSlots:");
    REQUIRE_MESSAGE(list_at != std::string::npos, "Peek.qml 里找不到 customSlots");
    const auto list_end = peek.find(']', list_at);
    REQUIRE(list_end != std::string::npos);
    const std::string list = peek.substr(list_at, list_end - list_at);

    for (const auto& s : slots) {
        CAPTURE(s.key);
        // 画得出图标。**两样都要有**：一个模子，和 `glyph()` 里认它的那一行。
        //
        // ⚠️ 2026-09-21 改过一次判据。原来找的是 `slotKey === "那个键"`
        // ——那会儿十七个形状全摆在 `SlotIcon.qml` 里，靠 `visible` 挑一个
        // 露出来。改成"只造用得着的那一个"之后（省下 22 MB，见
        // `test_qml_icons.cpp`），那个字符串整个没了，这条用例当场红了
        // ——**它是对的**：少一行 `case` 的后果正是"这一格什么都不画"。
        CHECK(icon.find("id: g_" + s.key) != std::string::npos);
        CHECK(icon.find("case \"" + s.key + "\": return g_" + s.key) != std::string::npos);
        // 开得出面板（名单里有它）
        CHECK(list.find("\"" + s.key + "\"") != std::string::npos);
    }

    // 反过来：名单里多出一个引擎没有的 key，说明改名之后有一处没跟。
    // 数的是**几个字符串**（一对引号算一个），不是几个引号。
    const std::size_t in_list =
        static_cast<std::size_t>(std::count(list.begin(), list.end(), '"')) / 2;
    CHECK(in_list == slots.size());
}

TEST_CASE("那几格的顺序就是流水线的顺序") {
    // 图标条本身要能当进度看。顺序乱了，人第一眼读到的"到哪儿了"就是错的。
    const auto slots = http::rail_present(json::object(), json::object(), json::object());
    REQUIRE(slots.size() == 5);
    CHECK(slots[0].key == "story");
    CHECK(slots[1].key == "assets");
    CHECK(slots[2].key == "script");
    CHECK(slots[3].key == "shots");
    CHECK(slots[4].key == "film");
}

// ---- 就地看一眼：一章都没有的时候别漏出接口报错 ----
//
// 剧本和镜头这两格是**按章看**的。一章都没有时 `get_peek` 原来照样去问工具，
// 工具回的是一句接口参数的报错——**「要说是哪一章（episode_id）。」**——
// 而那句话原样摆到界面上：人点开「剧本」，看见的是一个英文参数名。
// 2026-09-21 拿一个刚建的空项目挨个点开时撞见。
//
// 这一条钉住两件事：不许漏出参数名，而且那句话要**说清下一步**——一格空着
// 的时候人要的正是"那我该干什么"。
// ---- 就地看一眼：别把人支回已经做完的那一步 ----
//
// 「没有剧本」和「没有正文」是两回事。正文写完、还没理解过的项目也是一章
// 剧本都没有，而对它说「先得有正文——说一句『写第一章』」，是让人回去重做
// 一件已经做完的事。2026-09-21 拿一个写完八章正文、还没理解的项目点开
// 「剧本」撞见。
//
// 空着那一格说的话**必须是真正的下一步**——人点开一个空格子，要的就是这个。
TEST_CASE("就地看一眼：正文有了但还没剧本，说的是「理解故事」") {
    const auto tmp = fs::temp_directory_path() /
                     ("changji_peek_next_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    models::ProjectStore::create(tmp, "t_next", "下一步", models::StyleLine::REALISTIC);

    models::ProjectStore store(tmp);
    const std::string project = paths::to_utf8(tmp);

    // 一、光有大纲、一个字正文都没有：还是「写第一章」。
    {
        models::Story st = store.load_story();
        models::Chapter c;
        c.chapter_id = "ch01";
        c.title = "背叛";
        c.summary = "曾老板发现账上没钱了";
        st.chapters.push_back(c);
        store.save_story(st);

        const auto r = http::get_peek(project, "script", "");
        REQUIRE(r.status == 200);
        const std::string text = r.body.value("text", std::string());
        CAPTURE(text);
        CHECK(text.find("写第一章") != std::string::npos);
    }

    // 二、正文写上了（章还是一章都没有）：该说「理解故事」。
    {
        models::Story st = store.load_story();
        REQUIRE(!st.chapters.empty());
        st.chapters[0].text = "曾老板盯着电脑屏幕，余额数字刺眼，零元。";
        store.save_story(st);

        const auto r = http::get_peek(project, "script", "");
        REQUIRE(r.status == 200);
        const std::string text = r.body.value("text", std::string());
        CAPTURE(text);
        CHECK(text.find("理解故事") != std::string::npos);
        // **不能再把人支回去写正文**——那一步他已经做完了。
        CHECK(text.find("写第一章") == std::string::npos);
    }

    fs::remove_all(tmp, ec);
}

// ---- 就地看一眼：面板上不许出现 ep01 ----
//
// 上面那一条钉的是"参数名不许露"。这一条是同一族的另一半：**标识符也不许
// 露**。`script_read` 原来回的第一句是 `ep01 的剧本：`，而 `/api/peek` 把
// 工具回的那段**原样摆到面板上**——人点开「剧本」，第一行就是一个英文
// 标识符。2026-09-21 在桌面端实拍到。
//
// 界面上一律说「章」（CLAUDE.md 开头那条）：`ep01` / `ch01` 是磁盘上那份
// project.json 的键，改不动，所以只改说法。
TEST_CASE("就地看一眼：面板上说「第 1 章」，不说 ep01") {
    const auto tmp = fs::temp_directory_path() /
                     ("changji_peek_word_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    models::ProjectStore::create(tmp, "t_word", "说法", models::StyleLine::REALISTIC);

    {
        models::ProjectStore store(tmp);
        models::Project p = store.load_project();
        models::Episode ep;
        ep.episode_id = "ep01";
        ep.title = "背叛";
        ep.script = "曾老板：钱呢？";
        p.episodes.push_back(ep);
        store.save_project(p);
    }

    const std::string project = paths::to_utf8(tmp);
    const auto r = http::get_peek(project, "script", "");
    REQUIRE(r.status == 200);
    const std::string text = r.body.value("text", std::string());
    CAPTURE(text);

    CHECK(text.find("第 1 章") != std::string::npos);
    CHECK(text.find("ep01") == std::string::npos);
    // 剧本本身还得在——别把整段话连内容一起改没了。
    CHECK(text.find("钱呢") != std::string::npos);

    fs::remove_all(tmp, ec);
}

TEST_CASE("就地看一眼：一章都没有时说人话，不漏接口报错") {
    const auto tmp = fs::temp_directory_path() /
                     ("changji_peek_empty_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    models::ProjectStore::create(tmp, "t_empty", "空的", models::StyleLine::REALISTIC);
    const std::string project = paths::to_utf8(tmp);

    for (const char* key : {"script", "shots"}) {
        CAPTURE(key);
        const auto r = http::get_peek(project, key, "");
        REQUIRE(r.status == 200);
        const std::string text = r.body.value("text", std::string());
        CAPTURE(text);
        CHECK(!text.empty());
        // 参数名一个都不许露出来。
        CHECK(text.find("episode_id") == std::string::npos);
        // 而且要说下一步：这两句里都有一个「说一句」或者「才好」。
        const bool tells_next = text.find("说一句") != std::string::npos ||
                                text.find("才好") != std::string::npos;
        CHECK(tells_next);
    }

    fs::remove_all(tmp, ec);
}

TEST_CASE("就地看一眼：读成的说 failed=false，读砸的说 failed=true") {
    // 面板上那段话读成了是摘要、读砸了是一句报错，两者原来长得一样（2026-09-23
    // 起面板把后者画成出错色）。判据是这一栏，不是去认那句话。
    const auto tmp = fs::temp_directory_path() /
                     ("changji_peek_failed_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    models::ProjectStore::create(tmp, "t_failed", "读砸", models::StyleLine::REALISTIC);
    {
        models::ProjectStore store(tmp);
        models::Project p = store.load_project();
        models::Episode ep;
        ep.episode_id = "ep01";
        ep.script = "曾老板：钱呢？";
        p.episodes.push_back(ep);
        store.save_project(p);
    }
    const std::string project = paths::to_utf8(tmp);

    const auto good = http::get_peek(project, "script", "ep01");
    REQUIRE(good.status == 200);
    CHECK_FALSE(good.body.value("failed", true));

    // project.json 写坏：读剧本那个工具没做成。
    std::ofstream(tmp / "project.json") << "{\"episodes\": [";
    const auto bad = http::get_peek(project, "script", "ep01");
    REQUIRE(bad.status == 200);
    CAPTURE(bad.body.dump());
    CHECK(bad.body.value("failed", false));
    CHECK_FALSE(bad.body.value("text", std::string()).empty());

    fs::remove_all(tmp, ec);
}

TEST_CASE("就地看一眼：项目读不动时照实说，不装成「还没有剧本」") {
    // 没指定章时要先读项目挑一章。原来读砸了就吞掉，落进「一章都没有」那句，
    // 于是写坏的项目点开剧本看见的是「还没有剧本。先得有正文——说一句『写第一章』」。
    const auto tmp = fs::temp_directory_path() /
                     ("changji_peek_broken_" +
                      std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    models::ProjectStore::create(tmp, "t_broken", "写坏", models::StyleLine::REALISTIC);
    std::ofstream(tmp / "project.json") << "{\"episodes\": [";
    const std::string project = paths::to_utf8(tmp);

    for (const char* key : {"script", "shots"}) {
        CAPTURE(key);
        const auto r = http::get_peek(project, key, "");
        REQUIRE(r.status == 200);
        CAPTURE(r.body.dump());
        CHECK(r.body.value("failed", false));
        const std::string text = r.body.value("text", std::string());
        CHECK(text.find("还没有") == std::string::npos);
        CHECK(text.find("说一句") == std::string::npos);
    }
    fs::remove_all(tmp, ec);
}
