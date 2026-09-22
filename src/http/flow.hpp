#pragma once

// 引导流程的进度判定：七步走到哪一步了。
//
// **这一层原来在 Node 那个 BFF 里**（`webapp/server/src/routes/flow.js`）。
// 引擎自己发前端之后，走那条路的人拿不到 `/bff/*`——表现是界面能打开、
// 项目列表也在，但**章节下拉框是空的**，「这一章」那一页点不动。
// 也就是说"打开端口就能看处理进度"这件事没真做完。所以把它搬过来。
//
// **判定规则只写在一处。** 写在前端的话，侧边栏、顶部进度条、下一步按钮
// 三个地方各判一遍，迟早对不上——这句是 flow.js 开头的原话，搬过来仍然成立。
//
// **投递那一步整个没有了，不是"恒为未完成"。** 这儿原来写着「`publish` 那
// 一步……引擎这边没有那份记录，所以恒为未完成。投递本身也只有 Node 那侧有。
// 要用投递就仍然起 Node 那一层」——三句话今天都不成立：
//
//   · 步骤表里**没有 publish 这一档**（`flow_steps()` 只有 project / story /
//     assets / episode 四条，2026-09-11 合并时把它并进了「这一章」）。谈不上
//     恒为未完成——`done` 里多一个没有格子对应的键反而是死代码，test_flow 有
//     一条专门盯着。
//   · Node 那一层 2026-09-12 整个删了，`webapp/` 底下没有 `server/`。
//     「起 Node 那一层」是一条走不通的出路，而这类"指向不存在的东西"的话
//     比不说更糟。
//
// 「这一章」这一格的判据取的是**装配出这一章的成片**（见 flow.cpp 里那
// 段），发不发是可选的收尾动作，算进来只会让这一格永远不打勾。

#include <string>

#include <nlohmann/json.hpp>

namespace changji::http {

/// 七步的定义。前端的侧边导航按 `phase` 分段。
///
/// **2026-09-10 从八步变七步**：分镜和制作合成一步「镜头」，
/// 因为那两页也合成一页了（前端今天是「这一章」下面的 `EpShots`）。
///
/// 和 `flow.js` 的 `STEPS` 逐字对齐——前端按 `key` 认人，改一个字
/// 侧边栏就少一格。
nlohmann::json flow_steps();

/// 判定每一步做完了没有。**纯函数，好测。**
///
/// 参数就是三个接口的原样返回：`/api/project`、`/api/shots`、`/api/outputs`。
/// 单独抽出来是因为这里面全是"什么算做完"的判断，而那些判断错了不会报错，
/// 只会让侧边栏停在某一步上，用户不知道为什么走不下去。
///
/// `episode_id` 为空表示还没选章——这时按全片那三步判定，
/// 按章的四步一律未完成。
/// `story` 是 `/api/story` 回的那个 story 对象。**给了默认值**是因为
/// 老项目根本没有 story.json，而判定不该因为少一个文件就挂掉；
/// 默认空对象时「故事」那一格判未完成，别的格子不受影响。
nlohmann::json flow_assess(const nlohmann::json& project,
                           const nlohmann::json& shots,
                           const nlohmann::json& outputs,
                           const std::string& episode_id,
                           const nlohmann::json& story = nlohmann::json(),
                           const nlohmann::json& assets = nlohmann::json(),
                           const nlohmann::json& film = nlohmann::json());

}  // namespace changji::http
