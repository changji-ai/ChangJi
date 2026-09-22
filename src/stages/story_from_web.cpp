#include "stages/story_from_web.hpp"

#include <memory>

#include "stages/json_extract.hpp"
#include "stages/json_stream.hpp"
#include "stages/prompts.inc.hpp"
#include "stages/story_outline.hpp"   // StoryError
#include "util/cancel_words.hpp"
#include "util/text.hpp"

namespace changji::stages {

using ordered = nlohmann::ordered_json;
using models::Story;
using models::StyleLine;

namespace {

/// 最后 n 个字（按 UTF-8 字符数），切在字符边界上；截过的前面加省略号。
std::string tail_chars(const std::string& s, std::size_t n) {
    std::size_t count = 0;
    std::size_t i = s.size();
    while (i > 0 && count < n) {
        --i;
        // 不是续字节（10xxxxxx）的才算一个字符的开头
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++count;
    }
    return i == 0 ? s : "……" + s.substr(i);
}

}  // namespace

const ordered& web_chapter_schema() {
    static const ordered schema = [] {
        ordered props = ordered::object();
        props["title"] = {{"type", "string"}, {"description", "这一章的标题，几个字"}};
        props[kWebChapterBodyField] = {
            {"type", "string"},
            {"description", "这一章的正文，小说体，一千五百字上下，段与段之间空一行"}};
        props["source"] = {{"type", "string"}, {"description", "从网上哪条热点改的，一句话"}};
        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"title", kWebChapterBodyField};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::vector<llm::Message> web_chapter_opening(StyleLine style_line, const Story& story,
                                              const std::string& chapter_id) {
    std::vector<llm::Message> msgs;
    llm::Message sys;
    sys.role = "system";
    sys.content = prompt::story_from_web::kSystem;
    msgs.push_back(sys);

    std::string user = prompt::story_from_web::kUser;
    user += style_line == StyleLine::ANIME ? "画风是动漫电影。" : "画风是真人写实电影。";
    if (!story.logline.empty() || !story.premise.empty()) {
        user += prompt::story_from_web::kCtxStory;
        user += story.logline.empty() ? story.premise : story.logline;
    }
    // 前一章的结尾：接着写靠它。只带最后几百字——这一步要的是接得上，
    // 不是复述。
    const models::Chapter* prev = nullptr;
    const models::Chapter* mine = nullptr;
    for (const auto& c : story.chapters) {
        if (c.chapter_id == chapter_id) {
            mine = &c;
            break;
        }
        if (!text::strip_ws(c.text).empty()) prev = &c;
    }
    if (prev != nullptr) {
        user += prompt::story_from_web::kCtxPrev;
        user += tail_chars(prev->text, 600);
    }
    if (mine != nullptr && !mine->title.empty()) {
        user += prompt::story_from_web::kCtxThis;
        user += mine->title;
    }
    llm::Message u;
    u.role = "user";
    u.content = llm::schema_as_prompt(user, web_chapter_schema());
    msgs.push_back(u);
    return msgs;
}

WebChapter parse_web_chapter(const std::string& raw) {
    nlohmann::json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");
    WebChapter out;
    out.title = text::clean_field(data.value("title", std::string()));
    out.source = text::clean_field(data.value("source", std::string()));
    out.text = text::strip_ws(data.value(kWebChapterBodyField, std::string()));
    if (out.text.empty()) throw StoryError("正文是空的");
    return out;
}

namespace {

std::string step_label(const llm::ToolCall& c) {
    nlohmann::json args = nlohmann::json::object();
    try {
        if (!text::strip_ws(c.arguments).empty()) args = nlohmann::json::parse(c.arguments);
    } catch (const std::exception&) {
    }
    if (!args.is_object()) args = nlohmann::json::object();
    if (c.name == "hot_topics") return "在看热搜";
    if (c.name == "web_search") return "在搜「" + args.value("query", std::string()) + "」";
    if (c.name == "fetch_page") {
        return "在读 " + text::truncate_utf8(args.value("url", std::string()), 60);
    }
    return "在用 " + c.name;
}

}  // namespace

WebChapter write_chapter_from_web(llm::Client& client, const WebTools& web,
                                  StyleLine style_line, const Story& story,
                                  const std::string& chapter_id, pipeline::CancelToken& tok,
                                  const WebStoryHooks& hooks, int max_rounds) {
    std::vector<llm::Message> msgs = web_chapter_opening(style_line, story, chapter_id);
    llm::Request opts;
    opts.schema_name = "web_chapter";
    opts.on_thinking = hooks.on_thinking;
    const ordered specs = web_tool_specs();
    const auto step = [&](const std::string& s) {
        if (hooks.on_step) hooks.on_step(s);
    };

    for (int round = 0; round < max_rounds; ++round) {
        if (tok.cancelled()) throw llm::LlmError(util::kCancelled);
        // **每一轮一台新的状态机。** 这一轮可能在调工具（一个字正文都没有），
        // 也可能开口写；写到一半又去查一样东西的话，下一轮是从头写起。留着
        // 上一轮那台的话，它停在上一轮 JSON 的半中间，新这一轮的 `"text":`
        // 再也认不出来——静悄悄地一个字都不推。
        //
        // 攒在 shared_ptr 里而不是栈上：`opts.on_token` 比这一圈活得长
        // （opts 在循环外），捕引用的话下一圈就是悬空的。
        if (hooks.on_text) {
            auto field = std::make_shared<JsonFieldStreamer>(kWebChapterBodyField);
            auto fresh = std::make_shared<bool>(true);
            opts.on_token = [field, fresh, &hooks](const std::string& piece) {
                const std::string got = field->feed(piece);
                if (got.empty()) return;   // JSON 外壳和标题那一串不推
                hooks.on_text(got, *fresh);
                *fresh = false;
            };
        }
        const llm::ChatReply r = client.chat(msgs, specs, opts, tok);
        if (!r.tool_calls.empty()) {
            llm::Message a;
            a.role = "assistant";
            a.content = r.content;
            a.tool_calls = r.tool_calls;
            msgs.push_back(a);
            for (const auto& c : r.tool_calls) {
                if (tok.cancelled()) throw llm::LlmError(util::kCancelled);
                step(step_label(c));
                std::string out;
                try {
                    out = run_web_tool(web, c.name, c.arguments);
                } catch (const std::exception& e) {
                    out = std::string("工具出错：") + e.what();
                }
                llm::Message t;
                t.role = "tool";
                t.tool_call_id = c.id;
                t.content = out;
                msgs.push_back(t);
            }
            continue;
        }
        step("在写");
        return parse_web_chapter(r.content);
    }
    throw StoryError("看了 " + std::to_string(max_rounds) + " 轮网页还没开始写，停在这儿");
}

}  // namespace changji::stages
