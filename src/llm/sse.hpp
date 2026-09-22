#pragma once

// 把 OpenAI 那套 SSE（Server-Sent Events）流解成一段段文字。
//
// 远端大模型那条**原来是整段到的**：Client::complete 的默认实现跑一遍同步
// 的，然后把整段回调一次。写一章、写大纲在云端 API 上于是没有"边写边看"
// ——界面干等一两分钟，最后一下子蹦出来。用户 2026-09-12：「远端那条也
// 上 SSE」。
//
// 流的样子（各家都照 OpenAI 抄的）：
//
//     data: {"choices":[{"delta":{"role":"assistant"}}]}
//
//     data: {"choices":[{"delta":{"content":"第一"}}]}
//
//     data: {"choices":[{"delta":{"content":"章"}}]}
//
//     data: [DONE]
//
// **单独成文件是为了能测。** 这一层要处理的全是"只有真流才会碰上"的情况：
// 一行被切在两个 TCP 包中间、`data:` 后面有没有空格、心跳注释行、
// delta 里没有 content（头一条只有 role）、服务端把 error 塞进 data 里。
// 攒齐了再解析的代码永远碰不到这些，而它们错了的表现是"少了几个字"或者
// "整段是空的"——都不报错。

#include <cstddef>
#include <string>
#include <vector>

namespace changji::llm {

/// 流里攒出来的一次工具调用。
///
/// **这一层不认识 `llm::ToolCall`**（sse.hpp 不依赖 client.hpp，为的是能
/// 单独测），所以自己有一份同形状的。client.cpp 收尾时转一道。
struct SseToolCall {
    std::string id;
    std::string name;
    std::string arguments;   ///< JSON 文本，**流里是一小段一小段来的**
};

/// 边收边解的 SSE 流。喂字节，拿这一段里新解出来的**正文增量**。
///
/// **正文和思考分两路。** 现在的模型都要"先想再写"，而各家都把思考放在
/// 单独的字段里（智谱 `reasoning_content`、OpenRouter `reasoning`），
/// 不混进 `content`。混着收的话，思考稿会被当成正文流进编辑器。
class SseDeltas {
public:
    /// 收一段字节，返回这一段解出来的新**正文**（可能是空串）。
    std::string feed(const char* data, std::size_t len);
    std::string feed(const std::string& s) { return feed(s.data(), s.size()); }

    /// 取走攒着的**思考**增量，取完清空。
    ///
    /// 和正文分开是因为两者去向不同：正文进编辑器，思考只给界面上那个
    /// "正在想什么"的浮层看。做成 take 而不是让 feed 多返回一个值，
    /// 是为了不动 feed 的签名——它被一批用例钉着。
    std::string take_thinking() {
        std::string out;
        out.swap(thinking_);
        return out;
    }

    /// 这一趟模型要调的工具，按 `index` 攒好的。没有就是空的。
    ///
    /// **工具调用在流里是拆开来的**：第一条给 index + id + 函数名，
    /// 后面几十条只给 `function.arguments` 的一小段（`{"qu` / `ery":"` /
    /// …）。不按 index 合起来的话，拿到的是几十个名字为空、参数是半截
    /// JSON 的工具调用——而表现是"模型明明在搜，引擎说它没调工具"。
    const std::vector<SseToolCall>& tool_calls() const { return tool_calls_; }

    /// 收到 `data: [DONE]` 了。
    bool done() const { return done_; }

    /// 服务端报告的完成原因（stop / length / content_filter 等）。
    const std::string& finish_reason() const { return finish_reason_; }

    /// 服务端在流里塞了 error。空串表示没有。
    ///
    /// **这个要单独报。** 有些服务先回 200 再在流里说"这个模型没有"，
    /// 当成"生成完了"处理的话，用户拿到的是一段空正文外加一句"写好了"。
    const std::string& error() const { return error_; }

private:
    void take_line(std::string line, std::string& out);

    std::string buf_;       ///< 还没凑够一行的那半截
    std::string thinking_;  ///< 攒着的思考增量，等人来取
    std::vector<SseToolCall> tool_calls_;   ///< 按 index 攒的工具调用
    std::string error_;
    std::string finish_reason_;
    bool done_ = false;
};

}  // namespace changji::llm
