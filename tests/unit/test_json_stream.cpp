// 从正在生成的 JSON 里边生边抠一个字段。
//
// 这一组的重点是**流式独有的那几种切法**：一个转义序列被切在两个 token
// 中间、一个 \uXXXX 被切成四段、一个汉字的三个字节分三次到。攒齐了再解析
// 的代码永远碰不到这些，而它们错了的表现是正文里凭空多出几个反斜杠、
// 或者冒出一个乱码字——而那些字会**原样落进正文**，一路活到分镜表里去。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "stages/chapter_write.hpp"
#include "stages/json_stream.hpp"

using changji::stages::JsonFieldStreamer;
using json = nlohmann::json;

namespace {

/// 按给定的块大小喂进去，返回解出来的全部。
std::string stream_in(const std::string& raw, std::size_t chunk) {
    JsonFieldStreamer s("text");
    std::string got;
    for (std::size_t i = 0; i < raw.size(); i += chunk) {
        got += s.feed(raw.substr(i, chunk));
    }
    return got;
}

}  // namespace

TEST_CASE("整段进来：抠出 text 的值") {
    const std::string raw = json{{"text", "第一段。"}, {"hooks", json::array()}}.dump();
    JsonFieldStreamer s("text");
    CHECK(s.feed(raw) == "第一段。");
    CHECK(s.done());
    // 收完之后再喂什么都不吐——后面那串 hooks 不该进正文
    CHECK(s.feed("随便什么").empty());
}

TEST_CASE("一个字节一个字节进来，结果一样") {
    // **这是这组用例存在的理由。** llama.cpp 一次给一个 token，而一个汉字
    // 可能被切成三段、一个 \n 可能和它的反斜杠分两次到。
    const std::string raw =
        json{{"text", "第一段。\n\n第二段：他说「走吧」。"}}.dump();
    const std::string want = "第一段。\n\n第二段：他说「走吧」。";
    for (std::size_t chunk : {1u, 2u, 3u, 5u, 17u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == want);
    }
}

TEST_CASE("转义：\\n \\t \\\" \\\\ 都还原") {
    const std::string want = "换行\n制表\t引号\"反斜杠\\斜杠/";
    const std::string raw = json{{"text", want}}.dump();
    for (std::size_t chunk : {1u, 2u, 3u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == want);
    }
}

TEST_CASE("\\uXXXX 被切成四段也认得") {
    // 有的后端会把中文编成 中文。四位十六进制分几次到都要收得住。
    const std::string raw = R"({"text":"中文"})";
    for (std::size_t chunk : {1u, 2u, 3u, 5u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == "中文");
    }
}

TEST_CASE("代理对：BMP 外的字也拼得回来") {
    // emoji 和生僻字是一对 \uXXXX。只认高位不等低位的话，吐出去的是
    // 半个码点——那就是一段非法 UTF-8，一路流到提示词和字幕。
    const std::string raw = R"({"text":"😀好"})";
    for (std::size_t chunk : {1u, 3u, 7u, 4096u}) {
        CAPTURE(chunk);
        const std::string got = stream_in(raw, chunk);
        CHECK(got == "\xF0\x9F\x98\x80好");
    }
}

TEST_CASE("前面别的字段不吐出去") {
    // 只推 text。JSON 外壳和 hooks 那一串不该出现在编辑器里。
    const std::string raw =
        R"({"title":"这一章的名字","text":"正文在这儿","hooks":[{"text":"钩子"}]})";
    for (std::size_t chunk : {1u, 4u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == "正文在这儿");
    }
}

TEST_CASE("同名键在嵌套对象里不算数") {
    // hooks 里每一条也有 text。它们在 text 收完之后才出现，所以 done 了就
    // 不会被误收——这一条把那个顺序钉住。
    const std::string raw =
        R"({"text":"正文","hooks":[{"text":"钩子一"},{"text":"钩子二"}]})";
    CHECK(stream_in(raw, 1) == "正文");
    CHECK(stream_in(raw, 4096) == "正文");
}

TEST_CASE("值不是字符串就跳过，别把数字当正文") {
    const std::string raw = R"({"text":123,"other":"x"})";
    CHECK(stream_in(raw, 1).empty());
}

TEST_CASE("没生完也拿得到已经生出来的那些") {
    // 这才是常态：模型还在写，JSON 还没闭合。
    JsonFieldStreamer s("text");
    CHECK(s.feed(R"({"text":"写到一)") == "写到一");
    CHECK_FALSE(s.done());
    CHECK(s.feed("半") == "半");
    CHECK(s.text() == "写到一半");
    CHECK_FALSE(s.done());
}

// ---------------------------------------------------------------------------
// 一串字符串：章节正文的真实形状
// ---------------------------------------------------------------------------
//
// **这一组是补一个真出过的洞。** c41821d 把章节正文从 `{"text": "..."}` 改成
// `{"paragraphs": ["...", "..."]}`，而流式这一层还在找名叫 text 的字符串字段，
// 于是一个字都抠不出来——界面上就是"AI 写作没有热更新插入内容"，而后端不报
// 任何错、正文最后照样落库，查起来毫无线索。

TEST_CASE("一串字符串：一段一项，按换行拼回去") {
    const std::string raw =
        json{{"paragraphs", json::array({"第一段。", "第二段。", "第三段。"})},
             {"hooks", json::array()}}
            .dump();
    // **分隔符必须和 parse_chapter 拼 paragraphs 用的那个一致**，否则边看边
    // 写的和最后落库的段距不一样，而那种不一致没人会想到来查流式这一层。
    const std::string want = "第一段。\n第二段。\n第三段。";
    for (std::size_t chunk : {1u, 2u, 3u, 7u, 4096u}) {
        CAPTURE(chunk);
        JsonFieldStreamer s("paragraphs");
        std::string got;
        for (std::size_t i = 0; i < raw.size(); i += chunk) {
            got += s.feed(raw.substr(i, chunk));
        }
        CHECK(got == want);
        CHECK(s.done());
    }
}

TEST_CASE("一串字符串：最后一项后面不留分隔符") {
    // 补在下一项开头、不补在上一项结尾。补在结尾的话最后一项后面会多出一个，
    // 而流式是边看边写的——那个多出来的换行会一直挂在光标前面。
    JsonFieldStreamer s("paragraphs");
    const std::string got = s.feed(R"({"paragraphs":["甲。","乙。"]})");
    CHECK(got == "甲。\n乙。");
    CHECK(s.done());
}

TEST_CASE("一串字符串：数组里的转义和 u 转义照样认") {
    const std::string raw = R"({"paragraphs":["他说：\"走\"。","中文\n下一行"]})";
    JsonFieldStreamer s("paragraphs");
    CHECK(s.feed(raw) == "他说：\"走\"。\n中文\n下一行");
}

TEST_CASE("一串字符串：收完就不再收后面的 hooks") {
    // hooks 里每一条也有 text 字段，而且它整个也是个数组。paragraphs 收完
    // done 了就该闭嘴——不然钩子说明会被当成正文流进编辑器。
    const std::string raw =
        R"({"paragraphs":["正文一。","正文二。"],"hooks":[{"text":"钩子","after":"正文二。"}]})";
    JsonFieldStreamer s("paragraphs");
    CHECK(s.feed(raw) == "正文一。\n正文二。");
    CHECK(s.done());
}

TEST_CASE("一串字符串：空数组也收得住") {
    JsonFieldStreamer s("paragraphs");
    CHECK(s.feed(R"({"paragraphs":[],"hooks":[]})").empty());
    CHECK(s.done());
}

TEST_CASE("老形状还认：text 是一个字符串") {
    // 粘贴导入和改 schema 之前存的草稿走的还是老形状，parse_chapter 也还认它。
    JsonFieldStreamer s("text");
    CHECK(s.feed(R"({"text":"整段正文。"})") == "整段正文。");
    CHECK(s.done());
}

// ---------------------------------------------------------------------------
// 把两处字符串钉在一起
// ---------------------------------------------------------------------------

TEST_CASE("一个汉字被切在两个 token 中间，不能吐半个出去") {
    // **2026-09-12 实跑掉了一整章就是因为这个。** 广播那一层是
    // `json{{"text", fresh}}.dump()`，fresh 里有半个 UTF-8 字符时它当场抛
    // `[json.exception.type_error.316] incomplete UTF-8 string`，异常一路
    // 冒到写章节那条循环，那一章直接算失败。
    //
    // 改成 repeating 之后流过这一层的字多了三倍，撞上的概率也大了三倍。
    const std::string raw = R"({"paragraphs":["雨夜"]})";
    changji::stages::JsonFieldStreamer field("paragraphs");

    std::string got;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const std::string one = field.feed(std::string(1, raw[i]));
        // 每一次吐出来的都必须是合法 UTF-8——这正是广播那一层的要求
        CHECK_NOTHROW(nlohmann::json{{"text", one}}.dump());
        got += one;
    }
    CHECK(got == "雨夜");
    CHECK(field.text() == "雨夜");
}

TEST_CASE("流式抠的那个字段，必须真的在 schema 里") {
    // **这一条是为了让 2026-09-11 那个 bug 不可能再发生。**
    //
    // 当时 schema 从 `text` 改成 `paragraphs`，而流式那一层还在找 text，
    // 于是它一个字都抠不出来：编辑器一两分钟一动不动，后端不报任何错
    // （正文照样解析、落库、重算章节计划），查起来毫无线索。
    //
    // 2026-09-12 又挪了一层：正文改成一场一个数组（scenes[].paragraphs）。
    // 所以这条现在守两件事——键还在 schema 里，而且它在**场**里面。
    const auto& schema = changji::stages::chapter_schema(3, 20);
    REQUIRE(schema.contains("properties"));
    REQUIRE(schema.at("properties").contains(changji::stages::kChapterScenesField));

    const auto& scene = schema.at("properties")
                            .at(changji::stages::kChapterScenesField)
                            .at("items");
    REQUIRE(scene.at("properties").contains(changji::stages::kChapterBodyField));

    // 而且它得是**一串字符串**：JsonFieldStreamer 的数组那条才用得上。
    const auto& field = scene.at("properties").at(changji::stages::kChapterBodyField);
    CHECK(field.at("type") == "array");
    CHECK(field.at("items").at("type") == "string");

    // required 里也得有它，否则模型可以整个不写
    bool required = false;
    for (const auto& r : scene.at("required")) {
        if (r == changji::stages::kChapterBodyField) required = true;
    }
    CHECK(required);
}

TEST_CASE("流式：一场一个 paragraphs，要一路收到底") {
    // **不开 repeating 就只看得到第一场。** 正文 2026-09-12 改成
    // scenes[].paragraphs 之后，一份 JSON 里这个键出现好几次；状态机
    // 收完第一个 `]` 就 done() 的话，编辑器里只出现三分之一的正文，
    // 而后端照样解析、落库、重算章节计划——又是一个不报错的故障。
    const std::string raw =
        R"({"scenes":[{"where":"深夜便利店","pov":"林晚","goal":"要回伞",)"
        R"("obstacle":"他不认","turn":"伞柄上刻着别人的名字",)"
        R"("paragraphs":["第一场第一段","第一场第二段"]},)"
        R"({"where":"天台","pov":"林晚","goal":"问清楚","obstacle":"他不说",)"
        R"("turn":"他把伞扔了下去","paragraphs":["第二场第一段","第二场第二段"]}]})";

    changji::stages::JsonFieldStreamer field(changji::stages::kChapterBodyField,
                                             true);
    std::string got;
    for (const char c : raw) got += field.feed(std::string(1, c));

    CHECK(got == "第一场第一段\n第一场第二段\n第二场第一段\n第二场第二段");
    // repeating 开着的时候永远不 done：还有没有下一场，这一层不知道。
    CHECK_FALSE(field.done());

    // 不开 repeating 的话只收得到第一场——这就是那个故障的样子。
    changji::stages::JsonFieldStreamer once(changji::stages::kChapterBodyField);
    std::string partial;
    for (const char c : raw) partial += once.feed(std::string(1, c));
    CHECK(partial == "第一场第一段\n第一场第二段");
    CHECK(once.done());
}
