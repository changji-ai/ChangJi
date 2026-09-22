#pragma once

// 连接设置和运行参数两组接口。
//
// 这三个是阶段 1 漏掉的，路由表核对时找出来的——前端那次探测只发 GET
// 请求，POST-only 的接口整片看不见。
//
// 两组的区别在于**改的东西性质不同**：
//
//   /api/connections —— 换一台干活的机器（推理服务地址、大模型地址和密钥）。
//                       默认写回配置文件：连接是机器级别的事，
//                       改完重启还得再改一遍才是真的难用。
//   /api/settings    —— 调运行参数（帧率、码率、闸门阈值）。
//                       默认只影响本次进程：调画质档位常常是试几次才定，
//                       每次都写进文件反而碍事；但试定了重启就退回默认值
//                       也说不过去。所以做成一个开关。

#include <functional>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "doctor/doctor.hpp"
#include "http/readonly.hpp"

namespace changji::http {

/// 跑一遍体检。注入的，理由和别处一样：真体检会去连推理服务和大模型，
/// 连不上时每一项要等一个超时——测试里那是几十秒，而这个接口的逻辑
/// （改哪些字段、写不写回、报什么标签）和体检结果无关。
using DoctorFn = std::function<doctor::Report(const config::Settings&)>;

/// GET /api/connections —— 当前连的是哪几台机器。
///
/// **api_key 不回传明文**，只回一个"设了没有"和一个掐头去尾的提示。
/// 回明文的话它会进浏览器的网络面板、进前端的状态、进任何一次截图。
ApiResult get_connections();

/// POST /api/connections —— 改连接设置，然后立刻重新体检。
///
/// 体检是同步跑的：改完地址马上告诉用户通不通，比让他自己去点一下体检
/// 按钮有用得多——大部分人改完就走了，不通要到跑流水线时才发现。
ApiResult post_connections(const nlohmann::json& body,
                           const DoctorFn& check);

/// 真体检。
DoctorFn default_doctor();

/// POST /api/settings —— 改运行参数。
///
/// 老写法是把字段直接摊在请求体里，新写法包在 patch 里。**两种都收**，
/// 免得刷新慢一步的页面点一下就报 422。
ApiResult post_settings(const nlohmann::json& body);

}  // namespace changji::http
