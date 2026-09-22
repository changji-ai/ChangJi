#pragma once

// 账本上那一行，动的是哪一格。
//
// 界面上那一句「场记正在动这一格」要靠它：**长跑的活（写正文、拆镜头、
// 出片）是派出去的**——代理那头的工具当场就回「开始写正文。」然后结束了，
// 真正在动的那几分钟全在账本上。只看工具的话，那句话只亮几十毫秒。
//
// ⚠️ **`kind` 那套词是给网页那套用的**（`TasksView` 里 `kindOf(r.kind)` 认
// 着它挑图标和标签），**改不得**。所以这儿只读不写：kind → 格名。
//
// ⚠️ **`image` 一词两用**：参考图（设定那一格）和出首帧（镜头那一格）都用它。
// 所以它在这张表里回空串，由那两处自己说（`Task` 多一个 `slot` 参数）。
// 猜一个的话总有一半是错的，而按错了的那一句比没有更糟。
//
// 表要全：`test_task_slot.cpp` 直接读 `cpp/src` 里每一个起任务的地方，
// 少分类一个当场红（做法同 test_enums / test_tool_slot）。

#include <string>
#include <string_view>

namespace changji::util {

/// 账本上这一行动的是哪一格。认不出、或者一词两用的，回空串。
inline std::string slot_of_task_kind(std::string_view kind) {
    // 故事：大纲、前提、理解、写一章、改一章、从网上写一章
    if (kind == "outline" || kind == "premise" || kind == "analyze" ||
        kind == "write_one" || kind == "revise" || kind == "story_web") {
        return "story";
    }
    // 设定：读故事提人物场景、人物小传
    if (kind == "understand" || kind == "bible") return "assets";
    // 剧本
    if (kind == "script") return "script";
    // 镜头：排分镜、配音、出片（这几样的结果都长在那面墙上）
    if (kind == "plan" || kind == "tts" || kind == "video") return "shots";
    // 片子：前 n 分钟
    if (kind == "trailer") return "film";
    return {};
}

/// 这个 kind **本来就不对应哪一格**（不是"忘了分类"）。
///
/// 和上面那张表分开写，是为了让守卫分得清"没有"和"漏了"——合成一个函数的话
/// 新加的 kind 默认落进"没有"，而那正是要抓的那一档。
inline bool task_kind_has_no_slot(std::string_view kind) {
    // `llm` 是代理自己在想，`say` 是朗读一句试听，`run` / `write` 是长跑活
    // 那个壳（它底下每一件自己有 kind）。
    // `image` 一词两用，由起任务那两处自己说（见文件头）。
    return kind == "llm" || kind == "say" || kind == "run" ||
           kind == "write" || kind == "image";
}

}  // namespace changji::util
