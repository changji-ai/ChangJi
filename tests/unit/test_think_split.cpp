// 本地那条路上思考和正文混在同一条 token 流里，靠 ThinkSplitter 拆。
// 这几条钉的是拆错的几种形状：标签被 token 边界切开、模板先替模型写了
// <think>、模型压根不思考、</think> 后面那两个换行。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "infer/think_split.hpp"

using changji::infer::ThinkSplitter;

namespace {

struct Taken {
    std::string think;
    std::string content;
    std::vector<std::string> content_pieces;
};

/// 把一串 token 喂进去，收两路。
Taken run(const std::vector<std::string>& pieces, bool already_open = false) {
    Taken t;
    ThinkSplitter s("<think>", "</think>", already_open);
    s.on_thinking([&t](const std::string& p) { t.think += p; });
    s.on_content([&t](const std::string& p) {
        t.content += p;
        t.content_pieces.push_back(p);
    });
    for (const auto& p : pieces) s.feed(p);
    s.finish();
    // 两路攒的和回调收到的必须是同一份
    CHECK(s.thinking() == t.think);
    CHECK(s.content() == t.content);
    return t;
}

}  // namespace

TEST_CASE("Qwen3 那种：先 <think>…</think> 再正文") {
    const auto t = run({"<think>", "\n先想", "一想", "\n</think>", "\n\n{\"a\"", ":1}"});
    CHECK(t.think == "\n先想一想\n");
    // </think> 后面那两个换行不是正文
    CHECK(t.content == "{\"a\":1}");
}

TEST_CASE("标签被 token 边界切成两半也认得出") {
    const auto t = run({"<thi", "nk>想", "了</th", "ink>\n正", "文"});
    CHECK(t.think == "想了");
    CHECK(t.content == "正文");
}

TEST_CASE("模板先替模型写了 <think>：一开口就在思考里") {
    const auto t = run({"想想\n", "</think>\n", "正文"}, /*already_open=*/true);
    CHECK(t.think == "想想\n");
    CHECK(t.content == "正文");
}

TEST_CASE("不思考的模型：来什么都是正文，一个字不丢") {
    const auto t = run({"{\"a\"", ":", "1}"});
    CHECK(t.think.empty());
    CHECK(t.content == "{\"a\":1}");
    // 增量照原样往下推，不攒着
    CHECK(t.content_pieces.size() == 3);
}

TEST_CASE("像半个标签的尾巴先留着，流结束时按当前状态放出去") {
    ThinkSplitter s("<think>", "</think>", false);
    std::string content;
    s.on_content([&content](const std::string& p) { content += p; });
    s.feed("a<th");
    // 还没法判断 "<th" 是不是标签，先只放 "a"
    CHECK(content == "a");
    s.feed("x");   // 原来不是标签
    CHECK(content == "a<thx");
    s.feed("<");
    s.finish();
    CHECK(content == "a<thx<");
}

TEST_CASE("标签给空串就是不认：整段当正文") {
    ThinkSplitter s("", "", true);
    std::string content;
    s.on_content([&content](const std::string& p) { content += p; });
    s.feed("<think>不拆</think>正文");
    s.finish();
    CHECK(content == "<think>不拆</think>正文");
    CHECK_FALSE(s.in_thinking());
}
