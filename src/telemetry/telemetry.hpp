#pragma once

// 匿名使用统计：这台场记在不在、在干什么、按天干了多少，发给 changji.xyz（server/src/appstats.js）。
//
// 2026-09-29 用户：「在软件里增加软件使用统计显示到 changji.xyz 里访问中」「也要在地图上实时显示」，
// 定的是「默认开 + 首次启动明示 + 一键关」。三种上报：
//
//   /t/v1/beat    程序开着时每 5 分钟一条（刚起来、状态变了各补一条）：空闲 / 在写 / 在出片
//   /t/v1/event   发生那一刻一条：新装上、写完一章、出完一镜、接成一部片子（带成片分钟）
//   /t/v1/report  一天一条：按天的计数（每一步成败和耗时、闸门打回）、硬件档位、用的哪一类大模型和哪几个模型
//
// **只发白名单上的东西**，而且白名单写死在这一个文件里（和服务器那头 appstats.js 那几张表对得上）：
// 提示词、故事和剧本的字、片名和文件路径、图片视频、密钥和服务地址一个字都不经过这里——
// 钩子只交进来「哪一类活、成没成、多久」，不交项目、不交标题。
//
// 关着的时候（设置里关了、CHANGJI_TELEMETRY=0、DO_NOT_TRACK、本机编的版本）一个字节都不发，
// 也不在本机记账。
//
// ⚠️ 这个头会被桌面端的 .cpp 在 Qt 之后 include（desktop/engine.cpp 设 shell）：别在这里用
// `emit`、`signals`、`slots` 这几个词，也别往里拖 pipeline/jobs.hpp 这种重头文件。

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace changji::telemetry {

// ---- 钩子（任务账本、成片、新建片子、闸门、大模型调用） ----

enum class End { ok, fail, cancel };

/// 一件活开工（账本 `Task::begin`）：心跳里的「在干什么」按开着的活算。
void task_begun(std::string_view kind);
/// 一件活结账（账本 `~Task`）。`begun`：开过工没有（没开过工的只算取消，不算「在干」）。
/// `slot` 只用来分参考图和首帧（`image` 一词两用），不往外发。
void task_ended(std::string_view kind, std::string_view slot, bool begun, End end, double ms);
/// 按天的计数加一笔（`COUNTS` 那张表上的键；表外的扔掉）。
void count(std::string_view key, double n = 1.0);
/// 闸门打回一次（代号要像 `too_short` 这样，别的扔掉）。
void gate(std::string_view code);
/// 出了一件事（`installed` / `chapter_written` / `shot_rendered` / `film_joined`）。
/// `minutes` 只跟 `film_joined` 走，小于 0 = 不带。
void event(std::string_view kind, double minutes = -1.0);
/// 这个进程是桌面端里的引擎还是命令行引擎（桌面端在起引擎之前设一次）。
void set_shell(std::string shell);

// ---- 纯函数（用例直接测） ----

/// 像不像正式发出去的版本号：`v2.5.9`、`v2.5.12-v2.5`（beta 带分支名）。本机编的 `dev`、
/// `v2.2-local-cuda` 不是。
bool release_version(std::string_view v);

/// 开关之外、为什么发不了。
enum class Block { none, env, dnt, local_build };
std::string to_string(Block b);
/// `env` 取环境变量（没有回空串）。`CHANGJI_TELEMETRY_URL` 设了的话本机编的也放行（用来对着测试网关验）。
Block blocked(const std::function<std::string(const char*)>& env, std::string_view version);

/// 远程大模型是哪一家：按地址认，只回白名单上的名字（`zhipu`、`openai`……、`lan`、`other`）。**不回地址本身。**
std::string host_class(std::string_view base_url);
/// 显卡厂商：`nvidia` / `amd` / `intel` / `apple` / `other` / `none`。
std::string gpu_vendor_of(std::string_view gpu_name);
/// 显卡型号：去掉括号里的附注（「（统一内存）」）、只留字母数字和几个标点、80 个字以内。
std::string clean_gpu_name(std::string_view gpu_name);
/// 界面语言：`zh_CN` / 空 → `zh`，`en_US` → `en`，`pt_BR`、`zh_TW` 原样。
std::string lang_code(std::string_view spoken);
/// 模型文件名：只取文件名、只认 `[A-Za-z0-9._+-]`（人自己起的怪名字回空，不往外发）。
std::string model_name(std::string_view path);
/// 开着的活 → `rendering`（有在出图出片配音的）/ `writing`（有在写、在想的）/ `idle`。
std::string state_of(const std::map<std::string, int>& running);
/// 本地日期 `2026-09-29`。
std::string day_of(std::chrono::system_clock::time_point t);

// ---- 发 ----

/// 发送器要的东西，全部注进来（用例里换成桩）。
struct Io {
    /// POST 一段 JSON，回 {状态码, 回包}；没连上回 {0, ""}。**不许抛。**
    std::function<std::pair<int, std::string>(const std::string& url, const std::string& body)> post;
    std::function<std::chrono::system_clock::time_point()> now;
    /// 设置里开着没有（`[telemetry] enabled`）。
    std::function<bool()> configured;
    /// 发到哪儿（`https://changji.xyz`，末尾不带斜杠）。
    std::function<std::string()> endpoint;
    /// 硬件档位：{gpu_vendor, gpu_name, vram_gb, ram_gb}。一天问一次（问卡可能慢）。
    std::function<nlohmann::json()> hw;
    /// 用的什么：{llm, llm_host, video_model, image_model, tts_model, peers}。
    std::function<nlohmann::json()> setup;
    /// 环境变量（`blocked` 用）。
    std::function<std::string(const char*)> env;
};

struct Options {
    Io io;
    /// 安装编号、按天的账落在这儿（`<数据目录>`）。
    std::filesystem::path dir;
    /// `CHANGJI_VERSION`。
    std::string version;
    /// `cuda` / `vulkan` / `metal` / `hip` / `sycl` / `cpu`：编的时候定的（CMake 的 `CHANGJI_GPU_BUILD`）。
    std::string gpu_build = "cpu";
    /// 后台那条线程多久看一眼。**0 = 不起线程**（用例自己一拍一拍调 `tick_once`）。
    std::chrono::milliseconds tick{5000};
};

/// 起后台那条线程（引擎主进程一个；工作进程不起）。再调一次是先停再起。
void start(Options opt);
/// 停线程、把账落盘。
void stop();
/// 眼下能不能发（开关开着、没被环境变量 / DO_NOT_TRACK / 本机编的挡住）。
bool enabled();

/// `/api/telemetry`：{enabled, configured, blocked, notice_shown, endpoint, preview: {beat, event, report}}。
/// `notice_shown` 由调用方填（在配置里）。preview 是按眼下的数据现拼的原文。
nlohmann::json status();

// ---- 用例用：不起线程，一拍一拍手动推 ----

/// 走一拍：该发心跳就发、有事件就发、到点了就发汇总、该落盘就落盘。
void tick_once();
/// 清空内存里的账、在线表、事件队列（用例之间不串味）。
void reset_for_tests();

}  // namespace changji::telemetry
