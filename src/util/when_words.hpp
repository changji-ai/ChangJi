#pragma once

// 「多久之前」那几个字。
//
// 一条消息底下写一句「3 分钟前」，人一眼就知道这轮对话是刚才的还是昨天的
// ——而绝对时间（14:32）要他自己去减。
//
// **越近说得越细，越远说得越粗**：刚说完的那几秒不该写「0 分钟前」，
// 而三天前的东西写「4320 分钟前」也没人读得懂。
//
// 收成纯函数是为了能验：这一族全是边界（差一秒、差一分、跨天），而界面上
// 只看得见一句话——错了也就是"时间有点怪"，没人会去查。
//
// ⚠️ **别的语言里这几句用缩写**（`%1 min ago` / `%1 h ago`）。
// 理由同 `util/human_time.cpp`：一句一个 key 记不下复数，而这儿每一档的
// 下限都是 1（60~119 秒就是「1 分钟前」），"1 minutes ago" 是真会出现的。
// 缩写在"多久之前"这种地方本来就是通行写法。中日韩照旧写整词。
//
// ⚠️ 这份**曾经**被 `cpp/tools/say_scan.py` 的 `NOT_UI` 整份排掉，理由写的是
// 「解析用的词表」——**判错了**：它一个字都不解析，是把毫秒数说成人话，
// 挂在桌面端每条消息底下（`desktop/chat_model.cpp`）。2026-09-22 改回来。

#include <cstdint>
#include <string>

#include "util/say.hpp"

namespace changji::util {

/// `at` 和 `now` 都是毫秒。**未来的时间当"刚刚"**：机器时钟往回调过、
/// 或者两台机器差几秒，都会让 `at` 比 `now` 大——那时候说「-1 分钟前」
/// 比说「刚刚」糟得多。
inline std::string when_word(std::int64_t at, std::int64_t now) {
    if (at <= 0) return {};                    // 没有时间就什么都不说
    const std::int64_t d = now - at;
    if (d < 60'000) return SAY("刚刚");
    if (d < 3600'000) return SAYF("%1 分钟前", std::to_string(d / 60'000));
    if (d < 86400'000) {
        return SAYF("%1 小时前", std::to_string(d / 3600'000));
    }
    const std::int64_t days = d / 86400'000;
    if (days == 1) return SAY("昨天");
    if (days < 30) return SAYF("%1 天前", std::to_string(days));
    const std::int64_t months = days / 30;
    if (months < 12) return SAYF("%1 个月前", std::to_string(months));
    return SAYF("%1 年前", std::to_string(days / 365));
}

}  // namespace changji::util
