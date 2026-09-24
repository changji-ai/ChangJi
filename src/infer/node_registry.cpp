#include "infer/node_registry.hpp"

#include <thread>

#include "config/runtime.hpp"
#include "infer/local_exec.hpp"
#include "infer/node_prefs.hpp"
#include "infer/node_status.hpp"
#include "util/httplib.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

#include <algorithm>

namespace changji::infer {

namespace {

using nlohmann::json;

/// 问一台的 `/status` 要等多久。
///
/// **短一点。** 这几个请求是串着发的，而它们挡在页面前面：三台不通、
/// 每台等十秒，用户看到的是一个转了半分钟的圈。一台机器答不出一个
/// 自我介绍，多半也接不了活。
constexpr int kProbeTimeoutS = 3;

/// 这段回包是不是场记自己的网页界面。
///
/// 只认嵌进二进制的那张 index.html 的两个固定标记，不做宽松匹配：
/// 宽松了就会把别人家的 HTML 也说成"你起错模式了"，那比原来那句还糟。
bool looks_like_webapp(const std::string& body) {
    return body.find("<div id=\"app\">") != std::string::npos &&
           body.find(SAY_NEVER("场记")) != std::string::npos;
}

std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto pos = url.find("://");
    const std::string rest =
        pos == std::string::npos ? url : url.substr(pos + 3);
    const auto slash = rest.find('/');
    if (slash == std::string::npos) return {url, ""};
    return {url.substr(0, pos == std::string::npos ? slash : pos + 3 + slash),
            rest.substr(slash)};
}

/// 把配置里那几个 off 翻成能力。
///
/// **认不出的名字要说出来**，不能当没看见——那会变成"我明明关了出片，
/// 它还是派过去了"，而用户完全不知道是自己拼错了。
std::set<Capability> parse_off(const std::vector<std::string>& off,
                               std::string& complaint) {
    std::set<Capability> out;
    for (const std::string& s : off) {
        if (const auto c = capability_from(s)) {
            out.insert(*c);
        } else {
            if (!complaint.empty()) complaint += SAY("；");
            complaint += SAYF("配置里的 off 有个认不出的能力名「%1」", s);
        }
    }
    return out;
}

/// 本机这一行。
NodeState local_node(const config::Settings& s) {
    NodeState n;
    n.url = "local";
    const auto facts = probe_facts(s);
    for (const auto& r : capabilities_of(facts)) {
        if (r.able) {
            n.able.insert(r.cap);
        } else {
            n.why[r.cap] = r.why;
        }
    }
    n.online = true;   // 自己总是在线的
    n.busy = local_exec().busy();
    // 名字从自我介绍里取，和别的机器显示成同一种东西
    const auto js = node_status_json(s, config::runtime().profile());
    n.name = js.value("name", SAY("本机"));
    return n;
}

}  // namespace

std::vector<NodeState> NodeRegistry::snapshot(const config::Settings& s,
                                              std::chrono::seconds max_age) {
    std::vector<NodeState> out;
    bool stale = false;
    {
        std::lock_guard lg(mu_);
        const auto age = std::chrono::steady_clock::now() - fetched_at_;
        out = nodes_;
        stale = nodes_.empty() || age >= max_age;
    }
    if (stale) {
        if (out.empty()) {
            // 手上一份都没有，只能等这一趟。
            refresh(s);
            std::lock_guard lg(mu_);
            out = nodes_;
        } else if (!refreshing_.exchange(true)) {
            // **有旧的就先给旧的，刷新放后台。** 理由见头文件：关着的机器
            // 一定会走满探活超时，而那是每一次请求都要付的。
            std::thread([this, s] {
                try {
                    refresh(s);
                } catch (...) {
                    // 刷不动就保持上一份，下一拍再试。别让一条后台线程
                    // 把整个进程带走。
                }
                refreshing_.store(false);
            }).detach();
        }
    }

    // **开关不进缓存，每次现算。** 缓存的是"问出来的事实"（在线没有、
    // 能干什么），那要发 HTTP 所以值得缓；开关是本地读一个小文件，
    // 而且点一下就该立刻生效——进了缓存的话，用户点完要等五秒才看得到。
    const NodePrefs prefs = load_node_prefs(s.workspace_path());
    for (NodeState& n : out) {
        const auto it = prefs.find(n.url);
        n.off = n.off_locked;
        if (it != prefs.end()) n.off.insert(it->second.begin(), it->second.end());
    }
    return out;
}

namespace {

/// 问一台机器的 /status，填出它那一行。**纯粹一台，不碰别人**——
/// 下面要拿它一台开一条线程。
NodeState probe_peer(const config::Settings& s,
                     const config::PeerNodeConfig& cfg) {
    NodeState n;
    n.url = cfg.url;
    n.name = cfg.url;   // 连上了再换成它自报的名字
    std::string complaint;
    // 配置里那份是锁着的：改它要动配置文件。界面上点的那份在
    // snapshot 里合进来。
    n.off_locked = parse_off(cfg.off, complaint);
    n.off = n.off_locked;

    const auto [origin, prefix] = split_url(cfg.url);
    httplib::Client cli(origin);
    cli.set_connection_timeout(kProbeTimeoutS, 0);
    cli.set_read_timeout(kProbeTimeoutS, 0);
    const std::string token = cfg.token.empty() ? s.peer.token : cfg.token;
    if (!token.empty()) cli.set_bearer_token_auth(token);

    auto res = cli.Get(prefix + "/status");
    if (!res) {
        n.online = false;
        n.error = SAYF("连不上：%1", httplib::to_string(res.error()));
        // **「接得上但答得慢」和「根本没人听」要分开**（见 ProbeMiss）。
        //
        // 一台满负荷的机器 TCP 握得上手（内核把连接收进 backlog），只是
        // 那条线程腾不出手来答——落到 Read 超时上。而进程没了的是
        // Connection（被拒）。两者显示成同一句"连不上"的话，**一台正忙着
        // 替我们干活的机器会被判成死的**。
        switch (res.error()) {
            case httplib::Error::Read:
            case httplib::Error::Write:
            case httplib::Error::ConnectionTimeout:
                n.miss = ProbeMiss::Slow;
                break;
            default:
                n.miss = ProbeMiss::Refused;
                break;
        }
    } else if (res->status == 401) {
        // **单独认这一种。** 「口令不对」和「连不上」要做的事完全不同，
        // 而两边显示成同一句话的话，用户会去查网络。
        n.online = false;
        n.error = SAY("口令不对。这台的 [peer].token 和你这边填的对不上");
        n.miss = ProbeMiss::Answered;
    } else if (res->status != 200) {
        n.online = false;
        n.error = SAYF("答的不是 200：%1", std::to_string(res->status));
        n.miss = ProbeMiss::Answered;
    } else {
        const auto js = json::parse(res->body, nullptr, false);
        if (js.is_discarded()) {
            n.online = false;
            // **最常犯的那个错要单独认出来。** 理由同上面 401 那一条。
            //
            // 用户手上刚装好、刚在浏览器里打开的那一个，就是完整服务
            // （`changji --port 8080`）。把它的地址填到这张表里是第一
            // 反应——而完整服务的 `/status` 落在前端的兜底路由上，
            // 回的是 200 + 那张 index.html。于是这里解析失败，原来一律
            // 说「那头多半不是 changji」：**结论正好说反了**，对面正是
            // changji，只是起错了模式。用户照这句话去查地址、查端口、
            // 查防火墙，而要改的是那台的起法。
            if (looks_like_webapp(res->body)) {
                n.error = SAY(
                    "这台起的是完整服务，不是工作进程。派活要的是 "
                    "`changji --worker --port 9101`（只算不发界面）；"
                    "现在这个端口上是网页界面，填它没用");
            } else {
                n.error = SAY("答的不是 JSON，那头多半不是 changji");
            }
        } else try {
            // ⚠️ **回包的形状由对面说了算，一栏类型不对就是 type_error。**
            // `"name": null`、`capabilities: ["frame"]` 这种都会让 `value()` 抛，
            // 而这儿跑在一条裸线程上（`refresh` 里那几条）——漏出去就是
            // std::terminate，整个引擎没了。当它答的不是 changji。
            n.online = true;
            n.name = js.value("name", cfg.url);
            n.busy = js.value("busy", false);
            // 那台同时收得下几件。没报（老版本）按 1。
            //
            // ⚠️ **夹在 1~64**：报一个 -1 的话按 size_t 读出来是天文数字，
            // `remote_slots_for` 照着往表里推那么多个地址，内存当场耗尽。
            const auto sl = js.find("slots");
            const long long want =
                sl != js.end() && sl->is_number_integer() ? sl->get<long long>() : 1;
            n.slots = static_cast<std::size_t>(std::clamp<long long>(want, 1, 64));
            if (js.contains("capabilities") &&
                js["capabilities"].is_array()) {
                for (const auto& item : js["capabilities"]) {
                    const auto c = capability_from(item.value("cap", ""));
                    if (!c) continue;
                    if (item.value("able", false)) {
                        n.able.insert(*c);
                    } else {
                        // **那句话得跟着一起过来。** 它是那台自己算的
                        // （缺哪个文件、编没编进去，只有它知道），这边
                        // 除了原样传没有别的办法补出来。
                        n.why[*c] = item.value("why", std::string());
                    }
                }
            }
        } catch (const nlohmann::json::exception&) {
            // 只收回从回包里读出来的那几样；配置里来的（锁着的开关之类）照留。
            n.name = cfg.url;
            n.online = false;
            n.busy = false;
            n.slots = 1;
            n.able.clear();
            n.why.clear();
            n.error = SAY("答的不是 JSON，那头多半不是 changji");
        }
    }
    if (!complaint.empty()) {
        n.error = n.error.empty() ? complaint
                                  : n.error + SAY("；") + complaint;
    }
    return n;
}

}  // namespace

void NodeRegistry::refresh(const config::Settings& s) {
    std::vector<NodeState> fresh;
    fresh.push_back(local_node(s));

    // **并行问，不要一台一台排队。**
    //
    // 每台都是一次跨网 HTTP，探活超时 kProbeTimeoutS 秒。串着问的话总时间是
    // **加起来**：2026-09-17 实测一台本机加一台跨境远程，`/api/nodes` 要
    // 3.2 秒，而设置页开着就等它——那一页恰恰是"出事了才打开"的那一页。
    // 再加一台机器就再加一份（用户 2026-09-16 问过「再加一个电脑怎么分配
    // 任务」，加机器是常态）。
    //
    // 并行之后总时间是**最慢那一台**。顺序照配置里的顺序，不按谁先回来
    // ——那张表上的行不该每次刷新都跳。
    const auto& peers = s.peer.nodes;
    std::vector<NodeState> got(peers.size());
    {
        std::vector<std::thread> pool;
        pool.reserve(peers.size());
        for (std::size_t i = 0; i < peers.size(); ++i) {
            pool.emplace_back([&s, &peers, &got, i] {
                got[i] = probe_peer(s, peers[i]);
            });
        }
        for (auto& t : pool) t.join();
    }
    // **答得慢的那台，先按上一次问到的算。**
    //
    // 一台正忙着替我们干活的机器会答得比探活超时（3 秒）还慢——2026-09-20
    // 实测那台 L20：一边渲一边答 `/status` 要 2.4 秒，读权重那一分钟干脆
    // 答不上来。判成离线的代价是连锁的：机器表那一行变灰 → 体检说"出图
    // 后端没编进来"（本机确实没有）→ 镜头页三颗按钮全灰 → 出片被拒。
    // **人什么都没干，只是那台正在替他干活。**
    //
    // 判据在 `coast_on_last_ok`（那儿能测）：只有"接得上但答得慢"那一种、
    // 而且上一次问到还不久，才按上一次的算；被拒和"答了但答得不对"（口令
    // 不对、起错模式）一律照实报——那些等多久都不会自己好。
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lg(last_ok_mu_);
        for (auto& n : got) {
            const auto it = last_ok_.find(n.url);
            const bool had = it != last_ok_.end();
            if (n.online) {
                // 问到了：记下这一份，给下一次当"上一次"。
                if (had) {
                    it->second = {n, now};
                } else {
                    last_ok_.emplace(n.url, LastOk{n, now});
                }
                continue;
            }
            const auto since = had ? std::chrono::duration_cast<std::chrono::seconds>(
                                         now - it->second.at)
                                   : std::chrono::seconds::max();
            if (!coast_on_last_ok(n.miss, had, since)) continue;
            // 按上一次那份算，但**把这一格立起来**：照旧派得出活，
            // 而界面上要说清这是上一次的答案——不说的话，一台半死不活的
            // 机器会一直显示成健康的。
            NodeState coasted = it->second.state;
            coasted.off = n.off;             // 开关是本地的，用这一拍的
            coasted.off_locked = n.off_locked;
            coasted.stale = true;
            coasted.error = n.error;         // 这一拍到底怎么了，留着
            n = std::move(coasted);
        }
    }

    for (auto& n : got) fresh.push_back(std::move(n));

    std::lock_guard lg(mu_);
    nodes_ = std::move(fresh);
    fetched_at_ = std::chrono::steady_clock::now();
}
void NodeRegistry::warm(const config::Settings& s) {
    if (refreshing_.exchange(true)) return;
    std::thread([this, s] {
        try {
            refresh(s);
        } catch (...) {
        }
        refreshing_.store(false);
    }).detach();
}

NodeRegistry& node_registry() {
    static NodeRegistry one;
    return one;
}

json nodes_json(const config::Settings& s) {
    return nodes_json(node_registry().snapshot(s));
}

// `nodes_json(const std::vector<NodeState>&)` **搬到 node_json.cpp 去了**。
// 它是纯的（NodeState → JSON，不碰网络），而这个文件因为 node_registry()
// 要去问每一台的 /status 而 include 了 httplib——测试目标那一列上面写着
// 「一个网络库都不链」，所以只要它留在这儿，test_node_table.cpp 就永远
// 链不起来（实测：undefined reference 到 nodes_json）。


}  // namespace changji::infer
