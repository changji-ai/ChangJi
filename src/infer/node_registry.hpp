#pragma once

// 那张「机器 × 能力」的表，后端这一半。
//
// 表里有谁：**本机自己（叫 `local`）**，加上 `[[peer.nodes]]` 里配的那些。
// 本机也是一行，不是特例——不这样的话"本地 vs 远程"永远是两条代码路径，
// 界面和调度都统一不了。
//
// 每台的「能不能」是问出来的（`GET /status`），「准不准」是配置里那几个
// `off`，「该不该」交给 `node_pick.hpp`。三层分得很干净，这一层只负责
// **把事实收齐**。
//
// 缓存 + 按需刷新：问一遍要发几个 HTTP，页面每秒刷一次的话纯属给对面
// 添乱；而缓存太久的话，一台刚上线的机器要等半天才出现在表上。

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "infer/node_pick.hpp"

namespace changji::infer {

class NodeRegistry {
public:
    /// 默认缓存多久。5 秒：页面点开时基本是新的，连着刷也不会把对面问烦。
    static constexpr std::chrono::seconds kDefaultMaxAge{5};

    /// 拿一份快照。
    ///
    /// **过期了先把旧的给出去，刷新放后台。** 只有手上一份都没有时才等。
    ///
    /// ⚠️ 等一趟的代价是"最慢那台的超时"，而**关着的机器一定会走满超时**：
    /// 2026-09-17 实测远端那台关着，`/api/nodes` 每次 3.2 秒——而设置页开着
    /// 就等它，那一页恰恰是"出事了才打开"的那一页。旧数据最多旧五秒，
    /// 而且下一拍就新了；等三秒是每一次都要付。
    std::vector<NodeState> snapshot(
        const config::Settings& s,
        std::chrono::seconds max_age = kDefaultMaxAge);

    /// 不管缓存，现在就问一遍。配置改了之后要调它。
    void refresh(const config::Settings& s);

    /// 起服务时在后台先问一遍，别让第一个开页面的人等。
    void warm(const config::Settings& s);

private:
    mutable std::mutex mu_;
    /// 每台**上一次问到**的那份，和问到的时刻。
    ///
    /// 「答得慢」的那一拍先按它算（见 refresh 里那段和
    /// `coast_on_last_ok`）。单独一把锁：refresh 是并行探活之后才合，
    /// 而它跑在 `mu_` 之外。
    struct LastOk {
        NodeState state;
        std::chrono::steady_clock::time_point at;
    };
    mutable std::mutex last_ok_mu_;
    std::map<std::string, LastOk> last_ok_;
    std::vector<NodeState> nodes_;
    std::chrono::steady_clock::time_point fetched_at_{};
    /// 后台已经有一趟在刷了。**不加这个的话每次请求都开一条线程**——
    /// 页面那几处两秒一拍，关着的那台又要三秒，线程会越堆越多。
    std::atomic<bool> refreshing_{false};
};

/// 进程内那一份。
NodeRegistry& node_registry();

/// 那张表，给界面的形状。
///
/// 每台一行，每个能力一格，格子里带"为什么干不了"——**那句话是用户唯一
/// 的线索**，界面上悬停就能看见。
nlohmann::json nodes_json(const config::Settings& s);

/// 同上，但**不问任何机器**：拼那张表这一段是纯的，单独拎出来好撞。
/// 上面那个 = `snapshot()` 去问一圈 + 这个。
nlohmann::json nodes_json(const std::vector<NodeState>& nodes);

}  // namespace changji::infer
