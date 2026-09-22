#pragma once

// 手上这个是不是最新的。
//
// 用户 2026-09-17：「增加自动更新」。
//
// ⚠️ **这一层只答"是不是最新"，不换二进制。**
//
// 换掉一个正在跑的可执行文件在三个平台上是三种做法（Windows 上根本不让覆盖
// 正在运行的 exe，要先改名再放新的、下次启动才生效），而且换错了的后果是
// **程序起不来**——那时候界面也没了，没有任何地方能说清发生了什么。
// 先把检查做对；替换那一半走装的那条路（install.sh 或者重下一个包）。
//
// **判据不解析发布说明。** 发布那头两个通道各出一份 `version.json`
//（见 .github/workflows/release.yml），字段一样，地址固定：
//
//   https://github.com/<repo>/releases/download/release/version.json
//   https://github.com/<repo>/releases/download/beta/version.json
//
// 地址固定是 2026-09-17 把正式版也改成"永远只有一个 Release"换来的——
// 每版一个 Release 的话这个地址每次都变，程序只能去翻 API 列表再猜。

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"

namespace changji::setup {

/// 查一次的结果。
struct UpdateInfo {
    /// 手上这个（`CHANGJI_VERSION`）。
    std::string current;
    /// 那头最新的那个。问不到时是空串。
    std::string latest;
    /// 有没有比手上这个新的。**问不到一律算没有**——报一个假的"有新版"，
    /// 人点过去发现下不到，比不报更糟。
    bool newer = false;
    /// 去哪儿下。
    std::string url;
    /// 问不到的时候那句原话。空 = 问到了。
    std::string error;
    /// 那头是什么时候编的。界面上摆出来，人能判断这个数新不新。
    std::string built_at;
};

/// 取一段文字（HTTP GET）。**注进来而不是写死**：真实那份要链 httplib，
/// 而这一层的判断（版本比对、字段缺失）值得单独测。
using Fetch = std::function<std::string(const std::string& url)>;

/// `version.json` 的地址。
std::string version_json_url(const config::UpdateConfig& cfg);

/// 查一次。`fetch` 回空串表示没取到。
UpdateInfo check_update(const config::UpdateConfig& cfg,
                        const std::string& current, const Fetch& fetch);

/// 查一次，但**带缓存**。
///
/// ⚠️ **不带缓存的话，每打开一次设置页就是一次跨网请求**，超时 8 秒。
/// GitHub 连不上的时候（公司网、断网、被墙），那一页每次都要多转 8 秒——
/// 而那一页恰恰是"出事了才打开"的那一页（机器表那条 2026-09-17 刚为同一件
/// 事改过：先给旧的、刷新放后台）。
///
/// · `auto_check` 关着时**一次都不问**，除非 `force`；
/// · 上一次问过还不到 `every_hours` 就直接给上一次那份；
/// · `every_hours <= 0` = 只在起服务后问一次，之后不再自己问。
///
/// `force` 是人点了「现在查一次」：那一下必须真去问，不然按钮等于没有。
///
/// **缓存由调用方持有**，不是文件里一个静态的。进程级静态跨不过用例
/// ——上一条用例填进去的东西会让下一条根本不去问（2026-09-17 写这条用例时
/// 当场撞上），而"到底问没问"正是这段唯一要测的东西。真实那一处在路由里
/// 放一个函数级 static，效果一样。
struct UpdateCache {
    std::mutex mu;
    bool has = false;
    UpdateInfo last;
    std::chrono::steady_clock::time_point at{};
};

UpdateInfo cached_update(UpdateCache& cache, const config::UpdateConfig& cfg,
                         const std::string& current, const Fetch& fetch,
                         bool force);

/// 两个版本号，后一个是不是比前一个新。
///
/// ⚠️ **不做语义化版本比较，只比"一不一样"。**
///
/// 这套东西的版本号有两种形状：`v1.2.0` 和
/// `beta-<分支>-<短 sha>`。后一种根本没有先后可言（分支名在中间），而前一种
/// 真要比大小就得处理 `v1.10` 和 `v1.9`——**而这件事在这儿没有价值**：发布那头
/// 永远只有一个 Release，那上面挂的就是最新的，不一样就是该换了。
///
/// 比大小反而会出错：手上是 `beta-xxx` 而那头是 `v1.2.0` 时，任何"谁大"的
/// 规则都会给出一个说不清的答案。
bool is_different_version(const std::string& current, const std::string& latest);

}  // namespace changji::setup
