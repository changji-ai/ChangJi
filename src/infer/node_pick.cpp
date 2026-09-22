#include "util/say.hpp"

#include "infer/node_pick.hpp"

#include <algorithm>

#include "infer/worker_pool.hpp"   // kLocalEndpoint

namespace changji::infer {

bool coast_on_last_ok(ProbeMiss miss, bool had_ok,
                      std::chrono::seconds since_last_ok) {
    if (!had_ok) return false;
    if (miss != ProbeMiss::Slow) return false;
    return since_last_ok <= std::chrono::minutes(2);
}

std::vector<const NodeState*> candidates_for(const std::vector<NodeState>& nodes,
                                             Capability c) {
    std::vector<const NodeState*> out;
    for (const NodeState& n : nodes) {
        if (!n.online) continue;
        if (n.off.count(c) != 0) continue;
        if (n.able.count(c) == 0) continue;
        out.push_back(&n);
    }
    return out;
}

std::vector<std::string> remote_slots_for(const std::vector<NodeState>& nodes,
                                          Capability c) {
    std::vector<std::string> out;
    for (const NodeState* n : candidates_for(nodes, c)) {
        if (n->url == kLocalEndpoint) continue;
        // 报 0 按 1：一台在线的机器不能因为少报一个数就从池里消失
        const std::size_t k = std::max<std::size_t>(1, n->slots);
        for (std::size_t i = 0; i < k; ++i) out.push_back(n->url);
    }
    return out;
}

std::optional<std::string> pick_for(const std::vector<NodeState>& nodes,
                                    Capability c) {
    const auto cands = candidates_for(nodes, c);
    if (cands.empty()) return std::nullopt;
    for (const NodeState* n : cands) {
        if (!n->busy) return n->url;
    }
    // 全忙。**回第一个去排队，不是失败**——一镜一两分钟等得起，
    // 当场失败的话用户得到的是一章里随机几镜没了。
    return cands.front()->url;
}

std::string why_no_node(const std::vector<NodeState>& nodes, Capability c) {
    const std::string what = label_of(c);
    if (nodes.empty()) {
        return SAYF("没有任何一台机器登记在案，%1 这一步没地方派", what);
    }

    int offline = 0, turned_off = 0, cannot = 0;
    for (const NodeState& n : nodes) {
        if (!n.online) {
            ++offline;
        } else if (n.off.count(c) != 0) {
            ++turned_off;
        } else if (n.able.count(c) == 0) {
            ++cannot;
        }
    }

    // **要说清是哪一层拦的。** 只说"没有可用节点"的话，用户唯一能做的
    // 就是挨个去翻配置——而这三种情况要做的事完全不同：等一等／去表上
    // 打开／去装模型。
    std::string out = SAYF("%1 这一步一台都派不出去：", what);
    bool first = true;
    const auto add = [&](int n, const std::string& one) {
        if (n <= 0) return;
        if (!first) out += SAY("；");
        out += one;
        first = false;
    };
    // ⚠️ **三句各是一整句，不是"数 + 一个碎片"。**
    //
    // 原来是 `SAYN("%n 台%1", n, 那半句)`，而那半句在别的语言里**自己带着
    // 动词**——英语下 1 台时出来的是「1 machine say they can’t do it」，
    // 数换成了单数，动词还是复数。实拍 `/api/nodes` 时看见的就是这个样子。
    //
    // 这正是 `util/say.hpp` 上那条"别拿 + 拼"：碎片翻不了，也改不动语序，
    // 更管不了跟着数变的那几个词。一句话一个键。
    add(offline, SAYN("%n 台连不上", offline));
    add(turned_off, SAYN("%n 台被你在表上关掉了", turned_off));
    add(cannot,
        SAYN("%n 台自己说干不了（缺模型或者没编进去，看那台的状态）", cannot));
    return out;
}

}  // namespace changji::infer
