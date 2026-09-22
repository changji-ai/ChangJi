#pragma once

#include <string_view>

namespace changji::http {

/// 这个二进制自己答的那几条 `/bff/*`。
///
/// **为什么要有这份清单。** webapp 原来跑在 Node 那层后面，`/bff/*` 是那层
/// 的接口；把 webapp 嵌进二进制之后，这几条要由 C++ 自己答。漏一条的后果
/// 不是"少个功能"——2026-09-10 漏了 `/bff/settings/overview`，表现是
/// **同一个页面上两个相反的结论**：顶栏读 `/bff/settings/status`（实现了）
/// 显示"引擎已连接"，设置页读 overview（404）当引擎离线处理，
/// 地址空、配置文件空、右上角写"连不上"。
///
/// 单元测试看不见这种错：两边各自都是对的，错的是中间少了一条。
/// 所以 `test_webapp.cpp` 拿**打包进来的前端代码**里出现的 `/bff/` 路径
/// 来查这份清单。
///
/// **在 server.cpp 里加一条 `/bff` 路由时，这里也要加。** 不加的话那条
/// 路由照样能用，只是这份清单不再是全集——而清单的意义就是"前端要的
/// 都在这儿"，少了它就只是个摆设。
inline constexpr std::string_view kBffRoutes[] = {
    "/bff/health",
    "/bff/settings/status",
    "/bff/settings/config",
    "/bff/settings/overview",
    "/bff/flow",
    "/bff/project/video",
    "/bff/project/finish",
    "/bff/settings/llm",
    "/bff/run/pending",
    // 首次运行那一页。漏一条的表现和当年漏 overview 一样难看：
    // 界面停在"正在读取模型清单"，而顶栏写着引擎已连接。
    "/bff/setup/state",
    "/bff/setup/download",
    "/bff/setup/progress",
    "/bff/setup/cancel",
    // 投递那一套 **2026-09-12 随 Node 那层一起删了**（不是"还没搬进来"：
    // webapp/server 整个目录已经不在仓库里，见 webapp/README.md）。但这几条
    // 仍然要答——回空形状加一句说明，而不是 404。前端 Promise.all 一挂
    // 整页就是个红框，用户分不清是"没做"还是"坏了"。
    "/bff/publish/platforms",
    "/bff/publish/targets",
    "/bff/publish/records",
    // 前端拼的是 /bff/publish/targets/<id>，扫出来就是带尾斜杠这一条。
    "/bff/publish/targets/",
    "/bff/publish/deliver",
    "/bff/publish/batch",
};

/// 这个二进制上能连 WebSocket 的地址。
///
/// **为什么有两个。** 引擎自己一直是 `/ws`。而前端拼的是 `/api/ws`——
/// 它原来跑在 Node 那层后面，那层把 `/api/*` 整个转给引擎，`/api/ws`
/// 落到引擎就成了 `/ws`。webapp 嵌进二进制之后转发那一层没了，前端
/// 连的地址就再也没人应答。
///
/// **症状不是"没有进度"，而是"进度看着是对的、只有单镜那一块不动"。**
/// 连不上时 run store 会退回 1.2 秒一次的轮询，顶上的总进度、阶段名、
/// 事件流全都照常走；只有镜头墙上每张牌的进度和状态是**只**吃 WebSocket
/// 的（`trackInflight`）。于是表现是：点了「重新生成」，那一格没有任何
/// 反应，而页面别处一切正常。
///
/// 两个地址都留着：`/ws` 是引擎自己的，对拍那套 WebSocket 验收连的是它；
/// `/api/ws` 是前端连的。**在 server.cpp 里加一个 WS 地址时这里也要加**——
/// 下面那条测试拿打包进来的前端代码里出现的 ws 地址来核对这份清单。
inline constexpr std::string_view kWsRoutes[] = {
    "/ws",
    "/api/ws",
};

}  // namespace changji::http
