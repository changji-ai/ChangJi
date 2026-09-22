#pragma once

// 代理刚才动了哪一格。
//
// 方案第四节：「产出物一律默认不展开，**只在消息末尾留一个入口按钮**」——
// 那颗按钮和右边图标条是同一个去处，**给两条路是因为它们答的是两个问题**：
// 按钮答「刚说的那件事在哪儿看」，图标答「这部片子现在都有些什么」。
//
// 判据只能是**代理刚才调了哪个工具**：一轮说完话，界面手上有的就是这一串
// 工具名。所以这儿是一张 工具名 → 格名 的表。
//
// ⚠️ **认不出来的要回空串，不许瞎猜一个。** 按错了的那颗按钮比没有更糟：
// 人点开一格空面板，还以为是自己找错地方了。
//
// 表要全：`test_tool_slot.cpp` 直接读 `agent/tools.cpp` 里每一个 `fn("…")`，
// 少分类一个当场红——新加一族工具时会逼着人回答"它做出来的东西在哪一格看"。
// 做法同 `test_enums.cpp`（CLAUDE.md 第八条：收不动就让它会响）。

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace changji::util {

/// 这个工具做出来的东西在哪一格看。没有对应的格就回空串。
inline std::string slot_of_tool(std::string_view name) {
    // 故事
    if (name == "story_read" || name == "story_outline" ||
        name == "story_write_chapters") {
        return "story";
    }
    // 设定（人物场景的参考图）
    if (name == "assets_read" || name == "assets_set_reference" ||
        name == "assets_understand" || name == "refs_make") {
        return "assets";
    }
    // 剧本
    if (name == "script_read" || name == "script_write_all") return "script";
    // 镜头
    if (name == "shots_read" || name == "shot_edit" ||
        name == "storyboard_plan_all") {
        return "shots";
    }
    // 片子。**`render_run` 归这儿**：它出的是片，而片子那一格放的就是片
    //（没接过整片时放这一章的）。
    if (name == "outputs_read" || name == "render_run" || name == "film_join") {
        return "film";
    }
    return {};
}

/// 这一次调用动的是哪一章（`ep01`）。args 里没有、或者根本不是 JSON 就回空串。
///
/// 用处：**画布跟着对话走**——代理刚拆完第 3 章的分镜，而人眼前那面墙还停在
/// 第 1 章。参数名全仓统一是 `episode_id`（`agent/tools.cpp` 里十五处）。
///
/// ⚠️ **args 是模型填的，什么都可能是。** 解析不动、不是对象、那一栏不是
/// 字符串——都当没有，别抛（这一条在工具跑完之后的收尾路上，抛出去就把
/// 一次成功的调用变成了一条报错）。同 `model-args-are-not-trusted`。
inline std::string episode_of_args(std::string_view args) {
    if (args.empty()) return {};
    const auto j = nlohmann::json::parse(args, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return {};
    const auto at = j.find("episode_id");
    if (at == j.end() || !at->is_string()) return {};
    return at->get<std::string>();
}

/// 这个工具**会改掉那一格里的东西**，不是只读一眼。
///
/// 差别落在稿纸上：**场记正在写这一章的时候，那一格要锁住**——人在那儿打的
/// 字，等那一稿落回来 `story.text` 一换就没了，而他自己看不出发生过什么。
/// 而"在读故事"是不用锁的，锁了只是白拦一道。
///
/// 只对**有格**的工具有意义（没格的东西没有"那一格"可改）。
inline bool tool_rewrites(std::string_view name) {
    return name == "story_outline" || name == "story_write_chapters" ||
           name == "script_write_all" || name == "storyboard_plan_all" ||
           name == "shot_edit" || name == "refs_make" ||
           name == "assets_set_reference" || name == "assets_understand" ||
           name == "render_run" || name == "film_join";
}

/// 这个工具**本来就没有可看的产出物**（不是"忘了分类"）。
///
/// 和上面那张表分开写，是为了让守卫分得清"没有"和"漏了"——合成一个函数的话
/// 新加的工具默认落进"没有"，而那正是要抓的那一档。
inline bool tool_shows_nothing(std::string_view name) {
    return name == "project_state" || name == "list_projects" ||
           name == "create_project" || name == "tasks_read" ||
           name == "task_cancel" || name == "ask_user";
}

}  // namespace changji::util
