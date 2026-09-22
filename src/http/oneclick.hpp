#pragma once

// 一键成片：从一个空项目到一条能看的片子，一颗按钮跑到底。
//
// 六步：**大纲（只要 1 章）→ 写这一章正文 → 理解（人物/场景/长相/剧本）
// → 拆这一章分镜 → 把缺的参考图画齐 → 出前 n 分钟**。
//
// ---
//
// **为什么在引擎里，不在页面里。**
//
// 这条链四十分钟起。页面里那两条（StoryView 的「一键处理」、EpShots 的
// 「跑完整部电影」）是在浏览器的闭包里一步步发请求的——关掉标签页、刷新
// 一下，链条就断在半路：手里那一件跑完了，下一件永远不会发出去，而屏幕上
// 什么都不会说。CLAUDE.md 第九条记的正是这件事（「队列活在标签页的闭包里，
// 关掉就散」），那一条是参考图队列收进引擎时写下的。
//
// 收在这儿之后：跑在 `JobKind::Write` 那个槽上，进度走
// `GET /api/script/series`（和别的批量活同一条），能停、能刷新、能换台
// 机器看。
//
// **第五步（参考图）用户没提，但少不了。** 出图模型是图像编辑模型时，
// 没有参考图 `POST /api/run` 直接 400，那句话是"去设定页把角色定妆、给
// 场景出空景图，再回来跑"——一颗一键把人支到别的页面按两下，正是
// CLAUDE.md 第一条（「能自动解决的就别报错」）要治的。
//
// **最后一步交给出片那个槽就收手。** 出片是 `JobKind::Run`，和这个槽
// 互不相干；这件活把它起起来就算跑完了，不在这儿等一个钟头——等的话
// 「写」那个槽被占一个钟头，期间任何写作都 409。页面跟着看出片那条进度。

#include <memory>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "http/run.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// POST /api/oneclick —— body:
/// `{project, premise?, keywords?, preview_s?, overwrite?}`
///
/// `premise` 不给就用项目里存着的那句；一个字都没有也照跑（模型自己想选题）。
/// `preview_s` 不给就用这部电影的设置（项目 changji.toml 的
/// `[preview].seconds`，和分镜页那颗按钮是同一个值）。
///
/// **已经写过正文的项目要显式 `overwrite`**，否则 409：这条链第一步就会
/// 把故事整个换掉，而那是人几个钟头的东西。
ApiResult post_oneclick(const nlohmann::json& body,
                        std::shared_ptr<llm::Client> client,
                        const RunDeps& deps);

}  // namespace changji::http
