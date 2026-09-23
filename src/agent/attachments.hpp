#pragma once

// 人跟着一句话带进来的文件：图、片子、音频、文字稿、Word 稿。
//
// 用户 2026-09-23：「在输入框的左下角显示 + 号图标点击插入图片/视频/文件等
// 可以被解析的任何文件……也可以通过拖拽文件放到输入框里」。
//
// ---
//
// **模型只读得懂字。** 所以一件附件落到对话里是两份：
//
//   · 给人看的（`media_of`）：一张缩略图、一条片子、一张文字卡——和场记做出
//     来的东西同一个样子（`agent/outcome.hpp` 的 `media_item`），界面一套
//     画法；
//   · 给模型看的（`describe_for_model`）：「【附件】董平.png（图片，1.2 MB）
//     路径：D:\…」。字稿和 Word 稿**把正文读出来贴上**——人丢一份小说进来，
//     要的就是"照这个写"，只给一个路径模型什么都做不了。
//
// **存的是原路径，不拷**：一段片子几百兆，拷一份进项目目录就是白占一倍盘。
// 代理要用的时候（设成参考图）那几个工具自己会拷（`assets_set_reference`
// → `copy_into`）。代价是引擎在别的机器上时路径那头读不到——那时候照实说，
// 和原来拖图进来说一句是同一个前提。
//
// 哪些算「读得懂」由 `kind_of` 一处说了算，桌面端的选文件框、拖进来那一层
// 都问它（同一件事别在两处各写一遍，CLAUDE.md 第八条）。

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::agent {

/// 这个文件归哪一类：`image` / `video` / `audio` / `text` / `doc`；读不懂回空串。
///
/// **按扩展名判**（不分大小写）。
std::string attachment_kind(const std::string& path);

/// 选文件框用的扩展名表（不带点）。和 `attachment_kind` 是同一张表。
std::vector<std::string> attachment_extensions();

/// 一件附件给人看的那一份（`{kind, path, title, text?}`）。
///
/// `kind` 落成界面认的那几种：图是 `image`、片子是 `video`、字稿和 Word 稿是
/// `text`（带一段开头）、音频是 `file`。**`path` 是绝对路径**（不是 `rel`）：
/// 附件不在项目目录里。
nlohmann::json attachment_media(const std::string& path);

/// 一件附件给模型看的那一段。读不到（文件不在这台机器上）也照实写进去。
std::string attachment_for_model(const std::string& path);

/// Word 稿（.docx）的正文，一段一行。读不出来回空串。
///
/// docx 是个 zip 包，正文在 `word/document.xml` 里；只认 zip 的两种存法
///（原样存、deflate）——Word 自己写出来的都是这两种。
std::string docx_text(const std::string& path);

}  // namespace changji::agent
