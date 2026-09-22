#pragma once

// 模型窗要的四条接口：这一组有哪些档、下到哪儿了、开始下、取消。
//
// ⚠️ **名字里的 "setup" 是历史。** 这几条原来服务于一个"首次运行的初始化
// 页"，那一页 2026-09-14 删了（用户原话「不需要初始化页面」，见 webapp
// 的 ProjectView 开头）。今天调它们的是**模型窗**——项目页「模型」那一行
// 点一下某个模型名弹出来的那个（ModelDialog），而且随时都能开，不只首次
// 运行。体检里那几条出路指的也是它：「去项目页「模型」那一行点一下编剧
// 模型的名字，在弹出来的窗口里填」。
//
// 路径没跟着改名是因为它在前端和配置里都用着，改了是一处破契约；
// 但**注释里别再叫它初始化页**，照着去界面上找是找不到的。
//
// **为什么在 /bff 不在 /api。** `/api/*` 整套在和 Python 的对拍覆盖范围内，
// 而下模型这件事 Python 侧压根没有——那边模型是 ComfyUI 自己管的。
// 加进 /api 就是一处破契约，而这一条完全没必要破。
//
// ---
//
// **为什么要有这一页。** 装好程序打开界面，用户看到的是一个能点的项目页，
// 点到「出片」才发现什么都跑不了，报的是"本地模型一个都没配"。那时候他
// 手上有的只有一句提示和一个配置文件路径，要自己去翻部署手册、找镜像地址、
// 挑量化档、算显存装不装得下。这一页把那段路变成"看一眼推荐、点下载、等"。
//
// **进不去首页是有意的**（只在必需的那几组缺东西时）。可以跳过，
// 但默认拦一下——一个什么都跑不了的首页不比这一页有用。

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "config/model_patch.hpp"   // models_field / apply_setup_patch 搬到 config 层了
#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "models/hardware.hpp"
#include "setup/catalog.hpp"

namespace changji::http {

/// GET /bff/setup/state —— 有哪些模型、这台机器该选哪个、盘上已经有什么。
ApiResult get_setup_state(const config::Settings& settings,
                          const models::HardwareProfile& profile);

/// POST /bff/setup/download —— 开下。body:
///     {"selections": {"video": "h3-q4-turbo", …}, "dir": "可选，模型放哪"}
///
/// 立刻返回，进度去 /bff/setup/progress 拿。已经在下的时候回 409——
/// **不是静默忽略**：用户点了第二次而界面什么都没变的话，
/// 他会以为第一次没点上。
ApiResult post_setup_download(const config::Settings& settings,
                              const nlohmann::json& body);

/// GET /bff/setup/progress —— 下到哪儿了。
ApiResult get_setup_progress();

/// POST /bff/setup/cancel —— 停下。已经下好的留着，下次接着下。
ApiResult post_setup_cancel();

/// 这一组配齐了没有。
///
/// 编剧和配音有另一条出路：接外面的服务。那时候本机一个模型文件都没有
/// 也算配齐了——**不认这一条的话，用云端大模型的人会被永远挡在这一页上**。
/// 这个模型文件还在下载中途吗（旁边有没有 aria2 的控制文件）。
///
/// **光比大小是判不出来的**：aria2c 一开始就把文件按最终大小整个预分配好，
/// 再往里填。所以下到 10% 的时候 `file_size` 返回的正好是清单里那个数，
/// "大小对上了 = 下完了"这条就被骗过去。2026-09-10 真被坑到：61.7 GB 的
/// bf16 才下了 6 GB，模型窗显示已完成，用户选了它，出片直接花屏——
/// 而且**没有任何报错**，safetensors 的头在文件开头早下下来了，解析得
/// 好好的，错的是后面那些还是零的张量。
bool download_in_progress(const std::filesystem::path& model_file);

bool group_satisfied(const setup::Group& g, const config::Settings& settings);

/// 这一组现在配的是哪个选项。认不出来返回空串。
///
/// 判据是**主角色那个文件名对得上**（video 组就是 `[models].video`）。
/// 拿"文件都在盘上"当判据是不行的：两档量化的配套文件可以都下过，
/// 那时候分不出配置里用的是哪一档。大模型走外接服务时按**地址**认，
/// 见实现里那段注释。
///
/// 导出是为了能测：认错的表现不是一句报错，而是下一次保存把用户自己
/// 挑的模型名冲掉（2026-09-14 的那个 bug）。
std::string current_option(const setup::Group& g,
                           const config::Settings& settings);

}  // namespace changji::http
