#pragma once

// 循环那几条接口（2026-09-28，见外层 agent/recur.hpp）。
//
//   GET    /api/loops              整张表：全局的、每部片子的都在，外加引擎这会儿的 now
//   POST   /api/loops              {project?, chat?, prompt, every_s?, trigger?}  开一个
//   DELETE /api/loops?id=&work=1   停 = 删。work=1 时它开出来、还在做的那部片子也停
//   POST   /api/loops/run          {id}  现在跑一次（不改节奏）
//
// 推送走 WebSocket 的 "loops" 频道：**有变化就推整张表**（开、删、跑、跳过、跑完）。
// 表很小，推整张最不容易对不上；倒计时由界面拿 `next_at` 和 `now` 自己算。
//
// 实现在外层产品仓库的 `agent/loops_api.cpp`；单独检出这个仓库时编的是
// `loops_api_absent.cpp`：接口回 404，`install_loops` / `loops_tick` 什么都不做。

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "http/run.hpp"   // RunDeps
#include "llm/client.hpp"

namespace changji::http {

ApiResult get_loops();
ApiResult post_loop(const nlohmann::json& body);
ApiResult delete_loop(const std::string& id, bool stop_work);
ApiResult post_loop_run(const nlohmann::json& body);

/// 到点替人开一轮要的客户端和出片后端。`server.cpp` 起服务时装一次；没装之前
/// `loops_tick` 什么都不做。
void install_loops(std::shared_ptr<llm::Client> client, std::function<RunDeps()> run_deps);

/// 看一眼哪几条到点了。搭系统推送那条两秒一拍的车（server.cpp），**不另起线程**。
/// 自己不阻塞：开火是另起线程跑一轮对话。
void loops_tick();

}  // namespace changji::http
