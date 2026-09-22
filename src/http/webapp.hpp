#pragma once

// 打包进来的前端，由引擎自己发。
//
// **为什么引擎要发前端。** 原来 `GET /` 回一句 text/plain 指路
// （"界面在 Node 那一层，默认 5174"），那是迁移期的临时状态：前端由一个
// Node 的 BFF 发，engine 只管接口。可它带来两件事——用户得起两个进程，
// 而且**打开引擎的端口看不到任何东西**，只有一句让他去别处的话。
//
// 现在把打包好的 dist 嵌进二进制（`bundled_webapp.inc.hpp`，28 个文件
// 329 KB），访问引擎的端口就能直接打开界面。
//
// **`/bff/*` 也由这个进程答了**（2026-09-10）。这一段以前写的是"设置页和
// 上传页要 /bff/*，得起 Node 那一层"——那是嵌进来的头一版，只发静态文件。
// 之后把那几条补齐了，清单在 `bff_routes.hpp`，`test_webapp.cpp` 拿打包
// 进来的前端代码里出现的 `/bff/` 路径去核对它。
//
// 所以现在**一个进程就是全部**：静态文件、`/api/*`、`/bff/*`、WebSocket。
//
// 这儿原来还跟着一句「Node 那一层只剩开发时用（`npm run dev` 的转发目标）
// 和投递那一套」——**那一层 2026-09-12 整个删了**，`webapp/` 底下已经没有
// `server/`（`/bff/publish/*` 现在回的是一句说明，原话就是「别去找
// webapp/server，那个目录已经不在仓库里了」）。开发时 `npm run dev` 也不
// 经过它了，vite.config.js 里那两条 proxy 直接转给引擎的 8080，那个文件上
// 还记着改这一行的理由：「忘了改的话打开是一片空白加满屏 502」。
//
// 留着那句话，等于让读的人去找一个不存在的目录——而它紧挨在「一个进程就是
// 全部」后面，自己打自己。

#include <string>
#include <string_view>

namespace changji::http {

/// 按扩展名给 Content-Type。
///
/// **必须带 charset=utf-8**：界面里全是中文，少了它浏览器按本地代码页猜，
/// 简体 Windows 上就是一片乱码。
std::string webapp_content_type(std::string_view path);

/// 找一个打包进来的文件。找不到回 nullptr。
///
/// `path` 是相对 dist 的，不带前导斜杠。
const std::string* find_webapp_file(std::string_view path);

/// 把请求路径规整成 dist 里的相对路径。
///
/// 干三件事：去掉前导 `/`、空路径当 `index.html`、**挡住 `..`**。
/// 最后一件是重点：这些文件虽然在内存里、翻不出去，但规整逻辑以后要是
/// 改成读磁盘，`..` 就是目录穿越。现在就挡住，省得那天忘了。
/// 挡下来的返回空串。
std::string normalize_webapp_path(std::string_view request_path);

/// 这个路径该不该由前端接管。
///
/// `/api` 开头的一律不接管——那是接口。其余的交给前端，
/// 找不到对应文件就回 `index.html`（单页应用的深链接要靠它，
/// 直接打开 /production 这种地址才不会 404）。
bool webapp_owns(std::string_view request_path);

/// 这个路径要的是一个**具体文件**，还是一条前端路由。
///
/// 分这一刀是因为「找不到就回 index.html」不能一视同仁：
///   `/shots`            —— 路由，回 index.html 是对的（深链接靠它）
///   `/assets/index-A.js` —— 文件，找不到就该 404
///
/// **回 index.html 的代价在第二种上是致命的**：浏览器按 `<script>` 去取
/// 那个 js，拿回来的却是一整页 HTML，还是 200。解析当场失败，页面一片空白，
/// 而**没有任何一个请求是失败的**——F12 里全是 200，日志里也全是 200。
/// 2026-09-10 排查"刷新就白屏"时就卡在这上面。
///
/// 判据是**有没有扩展名**。前端路由都是 /shots、/project 这种光秃秃的路径；
/// 带 .js/.css/.png/.woff2 的一律是文件。
bool wants_file(std::string_view rel_path);

/// 这个文件该怎么缓存。返回值直接当 `Cache-Control` 发出去。
///
/// **index.html 必须每次回源核一遍**（no-cache）。不发这个头的话浏览器
/// 会按启发式规则自己缓存，而 index.html 里写死了带哈希的资源名——
/// 换一版之后旧的那些文件已经不在包里了，浏览器却还照着旧 html 去取，
/// 刷新也没用（刷新拿到的还是缓存里那份 html）。用户看到的就是白屏。
///
/// 带哈希的资源正相反：内容变了名字就变了，可以放心长期缓存。
std::string webapp_cache_control(std::string_view rel_path);

}  // namespace changji::http
