#pragma once

// 成片：GET /api/film 看合成出来的那部电影，POST /api/film/join 合成。
// 算法和落盘在 pipeline/film_join。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

/// GET /api/film?path= —— output/final 里那部电影，带上一次合成的清单。
///
/// ```
/// {name: "成片.mp4"|null, total_s: number|null, shots: number|null,
///  chapters: [chapter_id], skipped: [chapter_id], legacy_cut: bool,
///  files: [{name, rel, size_mb, mtime, duration_s?, shots?, from_chapter?}]}
/// ```
///
/// 键一个都不会少，`output/final` 还不存在时也带齐。**页面这么读：**
///
/// · `total_s` / `shots` 是 **null 时显示「—」，不要当 0**。null 的意思是
///   没量到，而「0 秒 0 镜」是在替引擎说一句它没量过的话。
/// · null **不等于「还没合成过」**：清单没读到是一支，合成成功而 ffprobe
///   没给出时长是另一支（那一栏在 film.json 里就是 null，见
///   `pipeline::FilmJoinReport::total_s`）。合没合成过看 `files` 里有没有成片。
/// · 那部电影是 `files` 里 `name` 那一条，**不是 `files[0]`**。`name` 是
///   null 就是「不知道哪个是成片」——`output/final` 是个目录，人往里放别的
///   mp4 不该让这一页打不开，所以 `files` 回的是整个目录。
/// · `legacy_cut` 为 true：盘上这几个文件是**上一版按时长切出来的那几
///   段**（盘上叫「第01集.mp4」这种名字），不是这一版合成的那一部电影。
///   这时 `name` 是 null、`total_s` 是这几段**加起来**多长、每一段自己的
///   时长和镜数在 `files[].duration_s` / `files[].shots` 上。页面上要说清
///   这是上一版切出来的，重新合成一次就变成一部完整的电影。
/// · `chapters` / `skipped` 是上一次合成（或上一版切）接了哪几章、哪几章
///   还没出片，两种情况下都是实情。
ApiResult get_film(const std::string& path);

/// POST /api/film/join —— body: {project}。
/// 跑在 JobKind::Run 那个槽上（出片那边忙着就 409），进度走 GET /api/run。
/// 一章都没出片当场 400，不起活；没片的章跳过。
ApiResult post_film_join(const nlohmann::json& body);

}  // namespace changji::http
