#pragma once

// 一部片子可以有好几条对话，每条一个 id。**这个 id 会变成一个文件名**
//（`<项目目录>/chats/<id>.jsonl`），所以它得先过一道。
//
// ⚠️ **不是模型填的，也一样要查。** 这个 id 从界面来、从接口来，而接口是
// 开着的（引擎可以在别的机器上）。一个带 `..` 或者斜杠的 id 就能把文件写到
// 项目目录外面去——和片名那条是同一族（`model-args-are-not-trusted`）。
//
// 规矩：**只认 `[A-Za-z0-9_-]`，长度 1~64**。不改写、不清洗——清洗过的 id
// 和原来那个不是同一条对话，而人以为是。不合规就当场拒，照实说。

#include <string>
#include <string_view>

namespace changji::util {

/// 这个对话 id 能不能用。
inline bool chat_id_ok(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace changji::util
