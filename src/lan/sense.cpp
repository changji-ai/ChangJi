#include "lan/sense.hpp"

#include "llm/client.hpp"
#include "util/say.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <system_error>
#include <thread>
#include <vector>

// ⚠️ **`<unistd.h>` 要挡在平台判断里面。** 它原来摆在最外面，而 Windows
// 上根本没有这个头文件——`lan/sense.cpp` 于是**连"这个平台没接"的兜底那
// 一档都编不过**。2026-09-22 在那台 Windows 上第一次真编才看见：
// 「error C1083: 无法打开包括文件 unistd.h」。
//
// 里头要的三样（`gethostname` / `pipe` / `close`）本来就只有接上了的那两个
// 平台用得着。
#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

// ⚠️ **Linux 上用的是同一套 API。**
//
// Avahi 带一层 Bonjour 兼容（`libavahi-compat-libdnssd`），头文件和函数名
// 和苹果那套**一模一样**——也就是说下面那一整段广播 + 浏览的代码，两个平台
// 用的是同一份，不是各写一遍。装的是 `libavahi-compat-libdnssd-dev`，
// CMake 那头探得到才开（探不到就退回"这个平台还没接"，照实说）。
//
// Windows 还没接：那边要么装 Apple 的 Bonjour SDK（一个额外的安装包），
// 要么走 Win10 自带的 `DnsServiceRegister` 那套——那是另一套 API，
// 得另写一份。做半套比不做更糟，先空着。
#if defined(CHANGJI_HAS_DNSSD)
#include <dns_sd.h>
// ⚠️ **`htons` / `ntohs` 要自己带进来。** macOS 上 `<dns_sd.h>` 顺手就把
// 它们拉进来了，Linux 上不会——2026-09-22 在树莓派上第一次编，四个错里
// 有两个是这个。
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::lan {

namespace {

/// 这一族在网上叫什么。**改它就等于换了一个网络**——老版本和新版本互相
/// 看不见。
constexpr const char* kService = "_changji._tcp";

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

/// 这台机器说给人看的名字。取主机名，取不到就退一个中性的。
std::string host_name() {
    char buf[256] = {0};
#if defined(__APPLE__) || defined(__linux__)
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        std::string s(buf);
        // macOS 上主机名常常是 `xxx.local`，摆在界面上那个后缀是噪声。
        const auto dot = s.find(".local");
        if (dot != std::string::npos) s.resize(dot);
        return s;
    }
#endif
    return SAY("一台机器");
}

}  // namespace

#if defined(CHANGJI_HAS_DNSSD)

// ⚠️ **两个平台的连接模型不一样，而这是真编过才知道的。**
//
// 苹果的 Bonjour 有 `kDNSServiceFlagsShareConnection`：注册、浏览、每一次
// 解析全挂在**一条**连接上，只有一个 fd，收工时把那一条释放掉，挂着的
// 全跟着没。
//
// **Avahi 的兼容层没有这个常量**（它的头文件停在老一档 API）。那边只能
// 各起各的连接、各一个 fd，`select` 要盯一组，而且**每一次解析完得自己
// 把那条收掉**——不收就是一个 fd 漏在那儿，网段上机器进进出出几十回就
// 把 fd 用光了。
//
// 有没有这个常量由 CMake 探（`CHANGJI_DNSSD_SHARE_CONNECTION`，见
// cpp/CMakeLists.txt）——**不按平台写死**，哪天 Avahi 补上了这边自动跟上。
#if defined(CHANGJI_DNSSD_SHARE_CONNECTION)
constexpr DNSServiceFlags kShareFlag = kDNSServiceFlagsShareConnection;
constexpr bool kSharedConn = true;
#else
constexpr DNSServiceFlags kShareFlag = 0;
constexpr bool kSharedConn = false;
#endif

/// 平台那一半。**全是 Bonjour 的东西，只有这个文件看得见。**
struct ResolveCtx;

struct Sense::Guts {
    DNSServiceRef shared = nullptr;   ///< 共用那一条连接
    DNSServiceRef reg = nullptr;      ///< 往外报
    DNSServiceRef browse = nullptr;   ///< 听别人
    std::vector<DNSServiceRef> resolving;   ///< 正在问细节的那几个
    /// 每一次 resolve 的上下文。**得活到那次回调之后**，所以攒在这儿，
    /// 收工时一起放。
    std::vector<std::unique_ptr<ResolveCtx>> ctxs;
    /// 网上那个实例名 → 对面自报的 id。
    ///
    /// ⚠️ **走的那一下只给得出实例名**（browse 的 remove 回调里没有 TXT），
    /// 而表是按 id 记的。这张对照表就是那座桥。
    std::map<std::string, std::string> by_instance;
    /// select 要盯的那几条。**共用那一档只有一条**（shared）；各起各的那
    /// 一档，注册一条、浏览一条、每个正在解析的各一条。
    std::vector<DNSServiceRef> watch;
    /// 解析回来了、等着收的那几条。**只在各起各的那一档用得上。**
    ///
    /// ⚠️ **不在回调里直接释放。** 那会儿正站在这条连接自己的
    /// `DNSServiceProcessResult` 里，把脚下的东西拆了。记一笔，等下一圈
    /// select 之前再收。
    std::vector<DNSServiceRef> finished;
    int wake[2] = {-1, -1};           ///< 自管道：叫停用
    std::thread spin;
};

bool Sense::supported() { return true; }

namespace {

/// 从 TXT 里抠一个键。**抠不到回空串**——对面可能是个更老的版本。
std::string txt_get(const unsigned char* txt, uint16_t len, const char* key) {
    uint8_t vlen = 0;
    const void* v = TXTRecordGetValuePtr(len, txt, key, &vlen);
    if (v == nullptr || vlen == 0) return {};
    return std::string(static_cast<const char*>(v), vlen);
}

}  // namespace

/// 一次 resolve 要带着的东西。
struct ResolveCtx {
    Sense* self = nullptr;
    std::string instance;   ///< 网上那个实例名
};

Sense::~Sense() { stop(); }

Sense& Sense::instance() {
    static Sense one;
    return one;
}

void Sense::start() {
    if (running_) return;
    trouble_.clear();
    guts_ = std::make_unique<Guts>();
    auto* g = guts_.get();

    if (::pipe(g->wake) != 0) { guts_.reset(); return; }

    if (kSharedConn) {
        if (DNSServiceCreateConnection(&g->shared) != kDNSServiceErr_NoError) {
            ::close(g->wake[0]); ::close(g->wake[1]);
            guts_.reset();
            return;
        }
        g->watch.push_back(g->shared);
    }

    // ---- 往外报一声 ----
    //
    // TXT 里带 id 和名字。**id 是认人的凭据**（`peers.hpp` 上那条：
    // 认 id 不认地址），名字只是给人看的。
    TXTRecordRef txt;
    char txtbuf[256];
    TXTRecordCreate(&txt, sizeof(txtbuf), txtbuf);
    TXTRecordSetValue(&txt, "id", static_cast<uint8_t>(id_.size()), id_.c_str());
    TXTRecordSetValue(&txt, "name", static_cast<uint8_t>(name_.size()),
                      name_.c_str());

    // ⚠️ **网上那个实例名必须每个进程唯一，别指望自动改名。**
    //
    // 2026-09-21 实撞：同一台机器上跑两个引擎，两边都拿主机名去注册，
    // 系统 `dns-sd -B` 里**只看得见一条**——两条记录被当成了同一个服务，
    // 于是各自 resolve 到的是同一份，被"自己不算"那一句一挡，两边都是
    // 0 台。名字后面缀上 id 的头四位就分开了。
    //
    // **界面上显示的不是它**：那个走 TXT 里的 `name`，还是干干净净的主机名。
    const std::string instance = name_ + "-" + id_.substr(0, 4);

    // 共用那一档：挂到 shared 上。各起各的那一档：留空，下面那一下自己起。
    g->reg = g->shared;
    DNSServiceErrorType err = DNSServiceRegister(
        &g->reg, kShareFlag, 0,
        instance.c_str(), kService, nullptr, nullptr,
        htons(static_cast<uint16_t>(port_)),
        TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt),
        nullptr, nullptr);
    TXTRecordDeallocate(&txt);
    // **报不出去要说出来。** 咽下去的话界面一直显示"开着"，而整个网段上
    // 没人看得见这一台，人只会以为"对方没开"。
    if (err != kDNSServiceErr_NoError) {
        g->reg = nullptr;
        trouble_ = SAYF("往外报不出去（Bonjour 回 %1）",
                        std::to_string(err));
    } else if (!kSharedConn) {
        g->watch.push_back(g->reg);
    }

    // ---- 听听还有谁 ----
    g->browse = g->shared;
    err = DNSServiceBrowse(
        &g->browse, kShareFlag, 0, kService, nullptr,
        [](DNSServiceRef, DNSServiceFlags flags, uint32_t iface,
           DNSServiceErrorType e, const char* name, const char* type,
           const char* domain, void* bctx) {
            if (e != kDNSServiceErr_NoError) return;
            auto* self = static_cast<Sense*>(bctx);
            auto* gg = self->guts_.get();
            if (gg == nullptr) return;
            if ((flags & kDNSServiceFlagsAdd) == 0) {
                // 走了。**这儿只有实例名，没有 id**——查那张对照表。
                const auto it = gg->by_instance.find(name);
                if (it == gg->by_instance.end()) return;
                std::lock_guard<std::mutex> lk(self->mu_);
                self->book_.lost(it->second);
                return;
            }
            // 来了：再问一次细节（地址、端口、TXT）。
            auto fresh = std::make_unique<ResolveCtx>();
            fresh->self = self;
            fresh->instance = name != nullptr ? name : "";
            auto* raw = fresh.get();
            gg->ctxs.push_back(std::move(fresh));
            DNSServiceRef r = gg->shared;
            if (DNSServiceResolve(
                    &r, kShareFlag, iface, name, type,
                    domain,
                    [](DNSServiceRef self_ref, DNSServiceFlags, uint32_t,
                       DNSServiceErrorType er, const char*, const char* host,
                       uint16_t port, uint16_t tlen, const unsigned char* txt,
                       void* c) {
                        if (er != kDNSServiceErr_NoError) return;
                        auto* cx = static_cast<ResolveCtx*>(c);
                        auto* s = cx->self;
                        Peer p;
                        p.id = txt_get(txt, tlen, "id");
                        p.name = txt_get(txt, tlen, "name");
                        p.host = host != nullptr ? host : "";
                        p.port = ntohs(port);
                        p.seen_at = now_ms();
                        // ⚠️ **自己不算。** 同一台机器广播的那一条自己也
                        // 收得到，不挡的话表上第一行永远是自己。
                        if (p.id.empty() || p.id == s->id_) return;
                        if (s->guts_ != nullptr) {
                            s->guts_->by_instance[cx->instance] = p.id;
                        }
                        {
                            std::lock_guard<std::mutex> lk(s->mu_);
                            s->book_.saw(p);
                        }
                        // 各起各的那一档：这一条问完了，记一笔等着收。
                        // 共用那一档不能收——那是大家共用的那条。
                        if (!kSharedConn && s->guts_ != nullptr) {
                            s->guts_->finished.push_back(self_ref);
                        }
                    },
                    raw) == kDNSServiceErr_NoError) {
                gg->resolving.push_back(r);
                if (!kSharedConn) gg->watch.push_back(r);
            }
        },
        this);
    if (err != kDNSServiceErr_NoError) {
        g->browse = nullptr;
        trouble_ = SAYF("听不见别人（Bonjour 回 %1）", std::to_string(err));
    } else if (!kSharedConn) {
        g->watch.push_back(g->browse);
    }

    // ---- 转起来 ----
    //
    // ⚠️ **这条线程自己收工、自己释放。** `DNSServiceRefDeallocate` 得和
    // 回调在同一条线程上，从外面直接释放会把正在回调里的那一次踩掉。
    running_ = true;
    asking_ = true;
    asker_ = std::thread([this] { ask_around(); });
    g->spin = std::thread([g] {
        auto drop = [g](DNSServiceRef r) {
            auto rm = [r](std::vector<DNSServiceRef>& v) {
                v.erase(std::remove(v.begin(), v.end(), r), v.end());
            };
            rm(g->watch);
            rm(g->resolving);
            DNSServiceRefDeallocate(r);
        };
        while (true) {
            // **先收上一圈问完的那几条。** 站在回调里收等于拆自己脚下的
            // 东西，所以推到这儿——这会儿已经出了 ProcessResult。
            for (DNSServiceRef r : g->finished) drop(r);
            g->finished.clear();

            fd_set set;
            FD_ZERO(&set);
            FD_SET(g->wake[0], &set);
            int top = g->wake[0];
            // 盯着的那几条：共用那一档只有一条，各起各的那一档一堆。
            std::vector<std::pair<int, DNSServiceRef>> fds;
            for (DNSServiceRef r : g->watch) {
                if (r == nullptr) continue;
                const int fd = DNSServiceRefSockFD(r);
                if (fd < 0) continue;
                FD_SET(fd, &set);
                if (fd > top) top = fd;
                fds.emplace_back(fd, r);
            }
            if (::select(top + 1, &set, nullptr, nullptr, nullptr) < 0) break;
            if (FD_ISSET(g->wake[0], &set)) break;
            bool broke = false;
            for (const auto& [fd, r] : fds) {
                if (!FD_ISSET(fd, &set)) continue;
                if (DNSServiceProcessResult(r) != kDNSServiceErr_NoError) {
                    broke = true;
                    break;
                }
            }
            if (broke) break;
        }
        for (auto& r : g->resolving) DNSServiceRefDeallocate(r);
        g->resolving.clear();
        g->finished.clear();
        g->ctxs.clear();
        g->by_instance.clear();
        if (kSharedConn) {
            DNSServiceRefDeallocate(g->shared);   // 共用那条一收，挂着的都跟着没
        } else {
            // 各起各的：注册和浏览各是一条，得各收各的。
            if (g->reg != nullptr) DNSServiceRefDeallocate(g->reg);
            if (g->browse != nullptr) DNSServiceRefDeallocate(g->browse);
        }
        g->reg = nullptr;
        g->browse = nullptr;
        g->shared = nullptr;
        g->watch.clear();
    });
}

void Sense::ask_around() {
    // 每隔几秒问一圈。**在这条线程上发真 HTTP**——一台连不上就要等超时，
    // 卡在那条 select 线程上会把 mDNS 的回调一起堵住。
    const auto get = llm::default_http_get();
    while (asking_) {
        std::vector<Peer> who;
        std::string me;
        {
            std::lock_guard<std::mutex> lk(mu_);
            who = book_.list();
            me = id_;
        }
        for (const auto& p : who) {
            if (!asking_) break;
            if (!p.online || p.host.empty() || p.port <= 0) continue;
            // 主机名末尾那个点去掉——mDNS 报回来的是 `xxx.local.`，
            // 拼进 URL 里多一个点有的库不认。
            std::string host = p.host;
            while (!host.empty() && host.back() == '.') host.pop_back();
            const std::string base =
                "http://" + host + ":" + std::to_string(p.port);

            std::string ticket = p.their_ticket;
            if (ticket.empty()) {
                // ⚠️ **被回绝过的那台，别每三秒再去敲一次门。**
                //
                // 没给就 403，而那是**正常答案**——绝大多数机器本来就不该
                // 给我们用。可要是每一轮都去问，一个二十台机器的网段上就是
                // 每三秒二十个注定 403 的请求，全网都在替我们白跑。
                // 被回绝过就退到一分钟一次：对面什么时候点那一下，
                // 晚一分钟知道没关系。
                const auto now = now_ms();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    const auto it = knocked_.find(p.id);
                    if (it != knocked_.end() && now - it->second < 60'000) {
                        book_.theirs(p.id, {}, -1, 0, 0);
                        continue;
                    }
                    knocked_[p.id] = now;
                }
                const auto r = get(base + "/api/lan/ticket",
                                   {{"X-Changji-Lan", me}}, 3.0);
                if (r.status == 200) {
                    const auto j = nlohmann::json::parse(r.body, nullptr, false);
                    if (!j.is_discarded() && j.is_object()) {
                        ticket = j.value("ticket", std::string());
                    }
                }
            }
            double gpu = -1;
            double vused = 0, vtotal = 0;
            if (!ticket.empty()) {
                // **给了才去读那台的负载。** 那一条接口这会儿本来就没上锁，
                // 可没授权就去读是另一回事——规矩是这头的事。
                const auto r = get(base + "/api/system",
                                   {{"X-Changji-Lan-Ticket", ticket}}, 3.0);
                // ⚠️ **票会作废。** 对面在设置里把那一下取消掉之后，手里
                // 这张就再也换不出东西来了——而我们不会收到任何通知。
                // 不扔的话这头一辈子都以为"还用着呢"：那一台在列表里挂着，
                // 显存那一栏永远是灰的，谁也看不出是被收回了还是刚好没数。
                // 扔掉就退回"没票"那一档，下一轮重新去要（要不着就退避）。
                if (r.status == 401 || r.status == 403) ticket.clear();
                if (r.status == 200) {
                    const auto j = nlohmann::json::parse(r.body, nullptr, false);
                    if (!j.is_discarded() && j.is_object()) {
                        const auto gs = j.value("gpus", nlohmann::json::array());
                        if (!gs.empty() && gs[0].is_object()) {
                            gpu = gs[0].value("util_percent", -1.0);
                            vused = gs[0].value("vram_used_gb", 0.0);
                            vtotal = gs[0].value("vram_total_gb", 0.0);
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lk(mu_);
            // 要着了就把"敲过门"那一笔抹掉：下次要是票失效了
            //（对面收回了权限），要立刻能重新去要，不用再等一分钟。
            if (!ticket.empty()) knocked_.erase(p.id);
            book_.theirs(p.id, ticket, gpu, vused, vtotal);
        }
        // 睡的时候切成小段，**关开关那一下要立刻收工**，不能让人等三秒。
        for (int i = 0; i < 30 && asking_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Sense::stop() {
    if (!running_ || guts_ == nullptr) return;
    running_ = false;
    auto* g = guts_.get();
    const char b = 1;
    if (g->wake[1] >= 0) { [[maybe_unused]] auto n = ::write(g->wake[1], &b, 1); }
    asking_ = false;
    if (asker_.joinable()) asker_.join();
    if (g->spin.joinable()) g->spin.join();
    if (g->wake[0] >= 0) ::close(g->wake[0]);
    if (g->wake[1] >= 0) ::close(g->wake[1]);
    guts_.reset();
    // **表清空**：关掉之后还摆着一屏"在线"的机器是假的。权限不清
    //（那是给出去的承诺，和这会儿谁在线没关系）。
    std::lock_guard<std::mutex> lk(mu_);
    PeerBook fresh;
    fresh.load_grants(book_.grants_json());
    book_ = fresh;
}

#else   // 这个平台上没有那套 API

struct Sense::Guts {};
bool Sense::supported() { return false; }
Sense::~Sense() = default;
Sense& Sense::instance() { static Sense one; return one; }
void Sense::start() {}
void Sense::stop() {}

#endif

// ---- 下面这些两个平台一样 ----

void Sense::boot(const fs::path& data_dir, int port) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        dir_ = data_dir;
        port_ = port;
        id_ = lan::my_id(data_dir);
        name_ = host_name();

        std::error_code ec;
        const fs::path f = data_dir / "lan.json";
        if (fs::exists(f, ec)) {
            std::ifstream in(f);
            const auto j = json::parse(in, nullptr, false);
            if (!j.is_discarded() && j.is_object()) {
                on_ = j.value("on", false);
                book_.load_grants(j.value("grants", json::object()));
            }
        }
    }
    if (on_ && supported()) start();
}

void Sense::set_on(bool on) {
    if (on == on_) return;
    on_ = on;
    if (on && supported()) start();
    else stop();
    save();
}

bool Sense::on() const { return on_; }

std::string Sense::my_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return id_;
}

std::string Sense::my_name() const {
    std::lock_guard<std::mutex> lk(mu_);
    return name_;
}

void Sense::allow(const std::string& id, const Grant& g) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        book_.allow(id, g);
    }
    save();
}

std::vector<Peer> Sense::peers() const {
    std::lock_guard<std::mutex> lk(mu_);
    return book_.list();
}

std::pair<std::string, std::string> Sense::their_door(
    const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : book_.list()) {
        if (p.id != id) continue;
        if (p.their_ticket.empty()) return {};
        std::string host = p.host;
        // mDNS 报回来的主机名末尾带一个点，拼进 URL 有的库不认。
        while (!host.empty() && host.back() == '.') host.pop_back();
        if (host.empty() || p.port <= 0) return {};
        return {"http://" + host + ":" + std::to_string(p.port),
                p.their_ticket};
    }
    return {};
}

Grant Sense::by_ticket(const std::string& ticket) const {
    std::lock_guard<std::mutex> lk(mu_);
    // **感知关着的时候一律不认。** 关掉的意思是"这台机器现在不参与"，
    // 而票是之前发出去的——不挡的话关掉开关也挡不住外来的活，
    // 那个开关就成了摆设。
    if (!on_) return {};
    return book_.by_ticket(ticket);
}

Grant Sense::grant_of(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return book_.grant_of(id);
}

void Sense::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir_.empty()) return;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    std::ofstream out(dir_ / "lan.json");
    out << json{{"on", on_}, {"grants", book_.grants_json()}}.dump(1) << "\n";
}

json Sense::to_json() const {
    json arr = json::array();
    for (const auto& p : peers()) {
        const auto g = grant_of(p.id);
        arr.push_back({{"id", p.id},
                       {"name", p.name},
                       {"host", p.host},
                       {"port", p.port},
                       {"online", p.online},
                       // 我给了他什么
                       {"use", g.use},
                       {"params", g.params},
                       // 他给了我什么。**票不发给界面**——界面不需要它，
                       // 而它是个凭据，少露一处是一处。
                       {"theyLetMe", !p.their_ticket.empty()},
                       {"theirGpu", p.their_gpu},
                       {"vramUsed", p.their_vram_used},
                       {"vramTotal", p.their_vram_total}});
    }
    return {{"supported", supported()},
            {"on", on()},
            {"error", trouble_},
            {"me", {{"id", my_id()}, {"name", my_name()}}},
            {"peers", arr}};
}

}  // namespace changji::lan
