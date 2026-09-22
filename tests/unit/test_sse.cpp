// SSE 解析。远端大模型那条"边写边看"全靠它。
//
// 这一层的错法都不报错，只是少字或者整段空：一行被切在两个包中间、
// `data:` 后面没有空格、\r\n 的 \r 没去掉、头一条 delta 里只有 role。
// 攒齐了再解析的代码永远碰不到这些，所以一条条摆在这儿。

#include <doctest/doctest.h>

#include <string>

#include "llm/sse.hpp"

using changji::llm::SseDeltas;

namespace {

std::string data_line(const std::string& content) {
    return "data: {\"choices\":[{\"delta\":{\"content\":\"" + content +
           "\"}}]}\n\n";
}

}  // namespace

TEST_CASE("SSE：一段段喂，拼出来的就是全文") {
    SseDeltas sse;
    std::string all;
    all += sse.feed(data_line("第一"));
    all += sse.feed(data_line("章"));
    all += sse.feed("data: [DONE]\n\n");
    CHECK(all == "第一章");
    CHECK(sse.done());
    CHECK(sse.error().empty());
}

TEST_CASE("SSE：一行被切在两个包中间") {
    // **这条是流式独有的**：TCP 不保证一个包正好是一行。丢掉半行的表现是
    // 正文里凭空少几个字，而那种错没人会往传输层想。
    SseDeltas sse;
    const std::string whole = data_line("完整的一句话");
    std::string all;
    for (std::size_t cut = 1; cut < whole.size(); cut += 7) {
        SseDeltas one;
        std::string got;
        got += one.feed(whole.substr(0, cut));
        got += one.feed(whole.substr(cut));
        CAPTURE(cut);
        CHECK(got == "完整的一句话");
    }
    // 一个字节一个字节喂也要对
    for (char c : whole) all += sse.feed(&c, 1);
    CHECK(all == "完整的一句话");
}

TEST_CASE("SSE：\\r\\n、没空格的 data:、注释行、心跳") {
    SseDeltas sse;
    std::string all;
    // 行尾是 \r\n：\r 不去掉的话 JSON 解不了，而且 [DONE] 也比不上
    all += sse.feed("data: {\"choices\":[{\"delta\":{\"content\":\"甲\"}}]}\r\n\r\n");
    // data 后面没空格，合法
    all += sse.feed("data:{\"choices\":[{\"delta\":{\"content\":\"乙\"}}]}\n\n");
    // 注释行（有的网关拿它当心跳）
    all += sse.feed(": ping\n\n");
    // 别的字段一律不管
    all += sse.feed("event: message\nid: 7\n\n");
    CHECK(all == "甲乙");
    CHECK_FALSE(sse.done());

    all += sse.feed("data: [DONE]\r\n\r\n");
    CHECK(sse.done());
}

TEST_CASE("SSE：delta 里没有 content 的那些条不算数") {
    SseDeltas sse;
    std::string all;
    // 头一条通常只有 role
    all += sse.feed("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n");
    // 结束那条只有 finish_reason
    all += sse.feed("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n");
    // content 是 null（有的服务这么发）
    all += sse.feed("data: {\"choices\":[{\"delta\":{\"content\":null}}]}\n\n");
    CHECK(all.empty());

    // **message.content 不能当增量取。** 有的服务在最后一条里给全文，
    // 取了会把全文再加一遍——表现是正文出现两遍，而它"看着像模型写的"。
    all += sse.feed("data: {\"choices\":[{\"message\":{\"content\":\"整段全文\"}}]}\n\n");
    CHECK(all.empty());
}

TEST_CASE("SSE：思考走另一路，不许混进正文") {
    // **现在的模型都要"先想再写"。** 各家把思考放在单独的字段里，混着收的话
    // 思考稿会直接流进用户的编辑器——那是这一条要挡住的东西。
    SUBCASE("智谱：reasoning_content") {
        SseDeltas d;
        std::string body;
        body += d.feed(
            "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"先想想\"}}]}\n\n");
        body += d.feed(
            "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"这一章\"}}]}\n\n");
        body += d.feed("data: {\"choices\":[{\"delta\":{\"content\":\"第一章\"}}]}\n\n");
        CHECK(body == "第一章");                    // 正文里一个思考的字都没有
        CHECK(d.take_thinking() == "先想想这一章");   // 攒着的思考一次取走
        CHECK(d.take_thinking().empty());            // 取完就清空
    }

    SUBCASE("OpenRouter 那些：reasoning") {
        SseDeltas d;
        const std::string body =
            d.feed("data: {\"choices\":[{\"delta\":{\"reasoning\":\"嗯\",\"content\":\"甲\"}}]}\n\n");
        CHECK(body == "甲");
        CHECK(d.take_thinking() == "嗯");
    }

    SUBCASE("同一条里两个字段都有，只收一次") {
        // 网关转发时偶尔两个名字都带上。收两遍的话浮层里每句都是双份。
        SseDeltas d;
        d.feed(
            "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"啊\",\"reasoning\":\"啊\"}}]}\n\n");
        CHECK(d.take_thinking() == "啊");
    }

    SUBCASE("只有思考、一个正文字都没有也不算错") {
        // 想了十分钟还没开始写，是常态，不是故障。
        SseDeltas d;
        CHECK(d.feed("data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"…\"}}]}\n\n")
                  .empty());
        CHECK(d.error().empty());
        CHECK(d.take_thinking() == "…");
    }
}

TEST_CASE("SSE：坏行跳过，不掀翻整段") {
    SseDeltas sse;
    std::string all;
    all += sse.feed(data_line("前"));
    all += sse.feed("data: {这不是 JSON\n\n");
    all += sse.feed("data: [1,2,3]\n\n");          // 是 JSON 但不是对象
    all += sse.feed("data: {\"choices\":[]}\n\n");  // 空 choices
    all += sse.feed(data_line("后"));
    CHECK(all == "前后");
}

TEST_CASE("SSE：服务端在流里报错要单独说，不能当成写完了") {
    // 先回 200 再在流里说"这个模型没有"是常见做法。当成生成完了处理的话，
    // 用户拿到的是一段空正文外加一句"写好了"。
    SseDeltas sse;
    const std::string got =
        sse.feed("data: {\"error\":{\"message\":\"model not found\"}}\n\n");
    CHECK(got.empty());
    CHECK(sse.error() == "model not found");

    SseDeltas plain;
    plain.feed("data: {\"error\":\"rate limited\"}\n\n");
    CHECK(plain.error() == "rate limited");
}
