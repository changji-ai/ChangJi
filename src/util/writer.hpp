#pragma once

// 此刻这条线程上**是谁在写**。
//
// 2026-09-24 起写作按对话分道（`pipeline::JobTable::start` 的 `lane`）：同一
// 部片子可以两条对话同时在写，一条的东西可能被另一条盖掉。**盖掉不能是静悄悄
// 的**——被盖的那条对话要来问人：恢复我的、保留盖上来的、还是合并（用户原话：
// 「被覆盖的话你要询问是恢复你的还是保留被覆盖的」「还要再加一个是否合并」）。
//
// 要认出"谁盖了谁"，存盘那一刻就得知道写的人是谁。存盘在很深的地方
// （`models::ProjectStore::save_*`），一路把"谁"当参数传下去要改几十个函数，
// 所以挂在线程上：派活的那条线程（任务表的工作线程、对话那一轮的线程）开工
// 时立一个 `WriterScope`，底下不管调到哪儿存盘，问 `current_writer()` 就知道。
//
// **没立就是"人"**：页面上的按钮、手改的稿子。人怎么改是人自己的事，存盘照记
// 账、照留底，但不因为人改了什么去问谁。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace changji::util {

struct Writer {
    /// 谁：空 = 人（页面、手改）；`chat:<对话编号>` = 那一条对话（`pipeline::chat_lane`）。
    std::string lane;
    /// **派生出来的**那一类写（出片：回填状态、路径、配音锁时长、拆镜）。
    /// 它改的不是谁的作品，不记作者、不算盖了谁；只把账上的指纹跟上，免得
    /// 出完片之后"这一块是谁写的"对不上号。
    bool derived = false;
    /// 这一件是干什么的（「写正文 · 还缺的 3 章」）。问人的时候说清是什么盖的。
    std::string title;
    /// 正在按人的决定处理哪一件覆盖（`models::Overwrite::id`）。
    /// 非空时照样留底，但**不再记成一件新的覆盖**——人刚拍了板，再去问对面
    /// 那条对话「你的被盖了」就是来回踢皮球。
    std::string resolving;
};

namespace detail {
inline std::vector<Writer>& writer_stack() {
    thread_local std::vector<Writer> s;
    return s;
}
}  // namespace detail

/// 这条线程上此刻是谁在写。没立过就是"人"（lane 空）。
inline const Writer& current_writer() {
    static const Writer kHuman{};
    const auto& s = detail::writer_stack();
    return s.empty() ? kHuman : s.back();
}

/// 在这个作用域里，"写的人"是 `w`。可以嵌套，出了作用域恢复外面那个。
class WriterScope {
public:
    explicit WriterScope(Writer w) { detail::writer_stack().push_back(std::move(w)); }
    ~WriterScope() { detail::writer_stack().pop_back(); }
    WriterScope(const WriterScope&) = delete;
    WriterScope& operator=(const WriterScope&) = delete;
};

/// 这一道是不是一条对话（`chat:` 开头）。
inline bool is_chat_lane(const std::string& lane) {
    return lane.rfind("chat:", 0) == 0;
}

/// `chat:<编号>` 里的编号。不是对话的道回空串。
inline std::string chat_of_lane(const std::string& lane) {
    return is_chat_lane(lane) ? lane.substr(5) : std::string();
}

}  // namespace changji::util
