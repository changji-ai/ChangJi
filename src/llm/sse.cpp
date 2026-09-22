#include "llm/sse.hpp"

#include <nlohmann/json.hpp>

namespace changji::llm {

namespace {

/// 去掉行首行尾的空白和 \r。
///
/// **\r 必须去。** SSE 的行尾是 \r\n，留着的话 `[DONE]` 比不上、
/// JSON 也解不了——而表现是"流到最后没停"或者"一个字都没有"。
std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == ' ' ||
                     s[b - 1] == '\t')) {
        --b;
    }
    return s.substr(a, b - a);
}

}  // namespace

void SseDeltas::take_line(std::string line, std::string& out) {
    line = trim(line);
    if (line.empty()) return;               // 事件之间的空行
    if (line[0] == ':') return;             // 注释行，有的服务拿它当心跳
    if (line.rfind("data:", 0) != 0) return;  // event:/id:/retry: 一律不管

    const std::string payload = trim(line.substr(5));
    if (payload.empty()) return;
    if (payload == "[DONE]") {
        done_ = true;
        return;
    }

    // **解不了就跳过这一行，不要抛。** 半路截断的行不会到这儿（凑够一行
    // 才解），但服务端塞点别的进来是可能的，而为了一行坏数据把整段生成
    // 掀翻不值。
    const auto j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    if (const auto err = j.find("error"); err != j.end() && error_.empty()) {
        if (err->is_string()) {
            error_ = err->get<std::string>();
        } else if (err->is_object()) {
            const auto m = err->find("message");
            error_ = (m != err->end() && m->is_string()) ? m->get<std::string>()
                                                         : err->dump();
        } else {
            error_ = err->dump();
        }
        return;
    }

    const auto choices = j.find("choices");
    if (choices == j.end() || !choices->is_array() || choices->empty()) return;
    const auto& first = (*choices)[0];
    if (!first.is_object()) return;

    if (const auto finish = first.find("finish_reason");
        finish != first.end() && finish->is_string()) {
        finish_reason_ = finish->get<std::string>();
    }

    // delta.content 是流式那条；message.content 是有些服务在最后一条里
    // 给全文（Ollama 的某些版本就这样）。**两个都认，但只取 delta**——
    // message 那条是累计的，取了会把全文再加一遍。
    const auto delta = first.find("delta");
    if (delta == first.end() || !delta->is_object()) return;

    // **思考单独收，不并进正文。** 各家的字段名不一样：
    //   智谱（bigmodel.cn / z.ai）  reasoning_content
    //   OpenRouter / 一批兼容网关   reasoning
    // 两个都认——认错了的代价只是这一段思考没显示出来，而把它并进正文的
    // 代价是思考稿直接流进用户的编辑器。
    for (const char* k : {"reasoning_content", "reasoning"}) {
        const auto r = delta->find(k);
        if (r != delta->end() && r->is_string()) {
            thinking_ += r->get<std::string>();
            break;
        }
    }

    // **工具调用要按 index 合。** 流里一次工具调用是拆成几十条来的：
    // 第一条带 index + id + function.name，后面每条只带 function.arguments
    // 的一小段。不合的话攒出来的是一堆半截，而调用方看到的是"它没调工具"。
    //
    // `index` 缺席的服务（有的网关只发一个工具调用时省掉它）按"接着上一个"
    // 算——那正是它们的意思。
    if (const auto tc = delta->find("tool_calls");
        tc != delta->end() && tc->is_array()) {
        for (const auto& item : *tc) {
            if (!item.is_object()) continue;
            std::size_t idx = tool_calls_.empty() ? 0 : tool_calls_.size() - 1;
            if (const auto i = item.find("index"); i != item.end() &&
                                                   i->is_number_integer() &&
                                                   i->get<long long>() >= 0) {
                idx = static_cast<std::size_t>(i->get<long long>());
            }
            if (idx >= tool_calls_.size()) tool_calls_.resize(idx + 1);
            SseToolCall& call = tool_calls_[idx];
            if (const auto id = item.find("id"); id != item.end() && id->is_string()) {
                // **只在原来是空的时候填。** 有的服务每条都重复 id，
                // 照单接的话拼成 call_xcall_xcall_x。
                if (call.id.empty()) call.id = id->get<std::string>();
            }
            const auto fn = item.find("function");
            if (fn == item.end() || !fn->is_object()) continue;
            if (const auto n = fn->find("name"); n != fn->end() && n->is_string()) {
                if (call.name.empty()) call.name = n->get<std::string>();
            }
            if (const auto a = fn->find("arguments"); a != fn->end() && a->is_string()) {
                call.arguments += a->get<std::string>();
            }
        }
    }

    const auto content = delta->find("content");
    if (content == delta->end() || !content->is_string()) return;
    out += content->get<std::string>();
}

std::string SseDeltas::feed(const char* data, std::size_t len) {
    std::string out;
    buf_.append(data, len);
    std::size_t start = 0;
    for (std::size_t i = 0; i < buf_.size(); ++i) {
        if (buf_[i] != '\n') continue;
        take_line(buf_.substr(start, i - start), out);
        start = i + 1;
    }
    // **剩下这半行要留着。** 一行被切在两个包中间是常态，丢掉的表现是
    // 正文里凭空少几个字——而那种错没人会往传输层想。
    buf_.erase(0, start);
    return out;
}

}  // namespace changji::llm
