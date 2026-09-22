#pragma once

// 从网上找热点，写眼前这一章。
//
// 用户 2026-09-18：故事页右下角那颗不再弹输入框，「点击直接让大语言模型使用
// tools 从网上获取热门内容改写成一个完整的故事」；接着定：「这次写的就只是
// 这一章内容」——和这一页别的 AI 动作一样，只动眼前这一章。
//
// 一条带工具的对话：模型自己决定看热榜、搜什么、读哪一页（stages/web_tools），
// 看够了写成这一章的正文。提示词一句话，结构在 schema 里——和理解故事同一条
// 规矩。前面有章的话把前一章的结尾带上，接着写。

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/client.hpp"
#include "models/character.hpp"
#include "models/story.hpp"
#include "stages/web_tools.hpp"

namespace changji::stages {

struct WebChapter {
    std::string title;    ///< 这一章的标题（模型起的）
    std::string text;     ///< 正文
    std::string source;   ///< 从哪条热点来的，一句话
};

/// 正文在那份回答里叫什么。**流式那条要按它抠字段**，所以不写死在两处：
/// 写死的话改了 schema 就静悄悄地一个字都抠不出来，后端不报任何错——
/// c41821d 那次章节正文改名就是这么坏掉的（见 stages/json_stream.hpp）。
inline constexpr const char* kWebChapterBodyField = "text";

/// 最后那份回答的结构：标题、正文、来源。
const nlohmann::ordered_json& web_chapter_schema();

/// 开场那两条：system + user（user 里带着上下文和 schema）。
/// 上下文 = 这部电影的一句话（有的话）+ 前一章的结尾 + 这一章的标题。
std::vector<llm::Message> web_chapter_opening(models::StyleLine style_line,
                                              const models::Story& story,
                                              const std::string& chapter_id);

/// 把最后那份回答解成这一章。正文是空的就抛 StoryError。
WebChapter parse_web_chapter(const std::string& raw);

struct WebStoryHooks {
    /// 报一句在干什么：「在看热搜」「在搜「x」」「在读 …」「在写」
    std::function<void(const std::string&)> on_step;
    std::function<void(const std::string&)> on_thinking;
    /// 正文**一段一段往外走**，推给编辑器。给的是增量。
    ///
    /// `restart` = 这是新一轮的第一段。一趟对话十轮起，前面几轮在看热榜、
    /// 读网页，真开口写是后面某一轮的事；而模型完全可能写到一半改主意
    /// 又去查一样东西，下一轮从头写起。不说清这一下的话，编辑器会把两轮
    /// 的字首尾接起来——**接出来的那一章前半截是废稿**，而它看着很正常。
    ///
    /// 抠的是 `kWebChapterBodyField` 那一栏，JSON 外壳和标题不推。
    std::function<void(const std::string& piece, bool restart)> on_text;
};

/// 整条对话：最多 max_rounds 轮工具；模型一开口写就收。
WebChapter write_chapter_from_web(llm::Client& client, const WebTools& web,
                                  models::StyleLine style_line, const models::Story& story,
                                  const std::string& chapter_id, pipeline::CancelToken& tok,
                                  const WebStoryHooks& hooks, int max_rounds = 10);

}  // namespace changji::stages
