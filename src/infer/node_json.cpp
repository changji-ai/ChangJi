// 那张机器表变成 JSON。
//
// **单独一个文件，因为它不碰网络。**
// 它原来在 node_registry.cpp 里，而那个文件为了去问每一台的 `/status`
// include 了 httplib。测试目标（changji_tests）那一列源码上面写着
// 「一个网络库都不链」——于是 test_node_table.cpp 编得出来、链不起来
// （undefined reference 到 nodes_json）。整个 changji_tests 因为这一个
// 符号起不来，82 个测试文件一个都跑不了。
//
// 拆开之后两边都对：表怎么画是纯逻辑，测试直接拿 NodeState 喂给它；
// 「去问一圈再画」那一半仍然留在 node_registry.cpp。

#include "infer/node_registry.hpp"

#include "infer/node_pick.hpp"

namespace changji::infer {

using nlohmann::json;

json nodes_json(const std::vector<NodeState>& nodes) {
    json rows = json::array();
    for (const NodeState& n : nodes) {
        json caps = json::array();
        for (const Capability c : all_capabilities()) {
            const bool able = n.able.count(c) != 0;
            const bool off = n.off.count(c) != 0;
            // 三态：干不了（灰）／能干但你关了（空心）／参与调度（实心）。
            // 界面照这个画，不自己推。
            const bool locked = n.off_locked.count(c) != 0;
            // 干不了时那句话。整台连不上的时候每一格都是那句连接错误
            // ——那台自己没能开口，问不出更细的。
            std::string why;
            if (!able) {
                const auto it = n.why.find(c);
                why = it != n.why.end() && !it->second.empty() ? it->second
                      : !n.online                             ? n.error
                                                              : std::string();
            }
            caps.push_back({{"cap", to_string(c)},
                            {"label", label_of(c)},
                            {"able", able},
                            {"off", off},
                            // 干不了时为什么。**界面上那个灰格子除了"灰"
                            // 以外什么都不说，这句话是用户唯一的线索。**
                            {"why", why},
                            // 配置文件关的，界面上点不动
                            {"locked", locked},
                            {"on", able && off == false && n.online}});
        }
        rows.push_back({{"url", n.url},
                        {"name", n.name},
                        {"online", n.online},
                        // **这一行是上一次问到的那份，这一拍没答上来。**
                        // 照旧派得出活（多半是它正忙着替我们干活，答不过来
                        // ——见 NodeState::stale），但界面上要说清楚：不说
                        // 的话，一台半死不活的机器会一直显示成健康的。
                        {"stale", n.stale},
                        {"busy", n.busy},
                        {"slots", static_cast<int>(n.slots)},
                        // 在线时是空的；`stale` 时是**这一拍**到底怎么了。
                        {"error", n.error},
                        {"local", n.url == "local"},
                        {"capabilities", caps}});
    }

    // 每个能力现在有几台能接。界面上那句"出片：2 台可用"用它，
    // 派不出去时那句话也在这儿拼好——两边各算一次迟早对不上。
    json summary = json::array();
    for (const Capability c : all_capabilities()) {
        const auto cands = candidates_for(nodes, c);
        summary.push_back(
            {{"cap", to_string(c)},
             {"label", label_of(c)},
             {"count", static_cast<int>(cands.size())},
             {"why", cands.empty() ? why_no_node(nodes, c) : std::string()}});
    }

    return json{{"nodes", rows}, {"summary", summary}};
}
}  // namespace changji::infer
