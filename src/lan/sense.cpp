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
// **Windows 走的是另一套**（2026-09-23 接上）：Win10 1703 起系统自带的
// `DnsService*`（`windns.h` / `dnsapi.lib`），不是 Apple 的 Bonjour SDK。
// 那一族函数名和这边完全不同，所以是**另一份实现**（本文件底下
// `CHANGJI_HAS_WINDNS` 那一段），不是同一份代码两个平台共用。
#if defined(CHANGJI_HAS_DNSSD)
#include <dns_sd.h>
// ⚠️ **`htons` / `ntohs` 要自己带进来。** macOS 上 `<dns_sd.h>` 顺手就把
// 它们拉进来了，Linux 上不会——2026-09-22 在树莓派上第一次编，四个错里
// 有两个是这个。
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#endif

#if defined(CHANGJI_HAS_WINDNS)
#ifndef NOMINMAX
#define NOMINMAX   // windows.h 里的 min/max 宏会把 std::min 一族打烂
#endif
#include <windows.h>
#include <windns.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <set>
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
#if defined(__APPLE__) || defined(__linux__)
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        std::string s(buf);
        // macOS 上主机名常常是 `xxx.local`，摆在界面上那个后缀是噪声。
        const auto dot = s.find(".local");
        if (dot != std::string::npos) s.resize(dot);
        return s;
    }
#elif defined(CHANGJI_HAS_WINDNS)
    // Windows 上取这台机器的名字。
    //
    // ⚠️ **不走 `gethostname`。** 那一个在 winsock 里，要先 `WSAStartup`
    // 才回得出东西——而这儿是进程刚起来、谁都还没初始化网络的时候。
    // `GetComputerNameEx` 不挑这些，而且直接给 DNS 那一档的名字（不是
    // NetBIOS 那个全大写的）。
    wchar_t wide[256] = {0};
    DWORD n = static_cast<DWORD>(sizeof(wide) / sizeof(wide[0]));
    if (::GetComputerNameExW(ComputerNameDnsHostname, wide, &n) != 0 &&
        wide[0] != L'\0') {
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                              nullptr, nullptr);
        if (len > 1) {
            std::string out(static_cast<size_t>(len - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr,
                                  nullptr);
            return out;
        }
    }
#endif
    return SAY("一台机器");
}

/// 一台在机器表上会长什么地址。**只此一处**——`their_door`（拿它来算那
/// 一下）和 `their_url`（"是不是已经在表上了"）都走它。两处各拼一遍的话，
/// 哪天端口或者主机名的处理改了，两句话就对不上。
///
/// ⚠️ **mDNS 报回来的主机名末尾带一个点**（`macbook.local.`），拼进 URL
/// 有的库不认。
std::string url_of(const Peer& p) {
    std::string host = p.host;
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty() || p.port <= 0) return {};
    return "http://" + host + ":" + std::to_string(p.port);
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

#elif defined(CHANGJI_HAS_WINDNS)

// ---- Windows 那一份：系统自带的 DNS-SD ----
//
// 走 `windns.h` 里 `DnsService*` 那一族（Win10 1703 起，`dnsapi.lib`），
// **不是 Apple 的 Bonjour SDK**——那条路要人另装一个安装包、还要那个服务
// 一直开着，而"拷过去就能跑"是这个包的立项理由之一。理由全文在
// cpp/CMakeLists.txt 那一段探测上头。
//
// ⚠️ **和上面苹果 / Avahi 那一份不是一套模型，没法共用代码。**
// 那头是"给我一个 fd，有事我通知你"；这头是**回调式**：发一次请求，系统在
// 它自己的线程池上回调。三处跟着不一样：
//
//   一、没有 fd 可 `select`，叫停靠一个事件（`quit`）。
//   二、回调在**别人的线程**上跑，碰 `book_` 一律上锁。
//   三、**浏览报不出"走了"这件事。** 苹果那头 remove 是一条回调；这头
//       `DnsServiceBrowse` 只管报"现在有这么几条"。所以这儿是**一轮一轮
//       地照相**：开一次浏览收几秒、停掉、和上一轮比——上一轮在、这一轮
//       不在的，就是走了。
//
// ⚠️ **端口是主机序，不是网络序。** 苹果那套收发都要自己 `htons`/`ntohs`，
// 这套不用：`wPort` 进出都是主机序。照着上面那份抄一个 `ntohs` 过来的话，
// 8080 会变成 36895——而那个数看着也挺像个端口号。

namespace {

/// UTF-8 → UTF-16。这套 API 从头到尾只认宽字符。
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          w.data(), n);
    return w;
}

/// UTF-16 → UTF-8。
std::string narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr,
                                        nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

/// 记录里那个名字转成宽字符。
///
/// ⚠️ **两个重载都要。** `DNS_RECORD` 是跟着 `UNICODE` 这个宏走的别名：
/// 定义了是 `DNS_RECORDW`（名字是 `wchar_t*`），没定义是 `DNS_RECORDA`
/// （`char*`）。而这个目标定没定 `UNICODE` 不归这个文件管——上游哪天
/// 加一句就换了一个类型，而换过去之后这儿是**编译错**，不是悄悄坏掉。
/// 两个重载摆在这儿，两种都接得住。
std::wstring to_w(const wchar_t* s) { return s != nullptr ? std::wstring(s) : std::wstring(); }
std::wstring to_w(const char* s) { return widen(s != nullptr ? std::string(s) : std::string()); }

/// 一次「问细节」要带着的东西。
struct WinAsk {
    std::wstring name;              ///< 网上那个实例全名（**要可写**，见下）
    std::uint64_t gen = 0;          ///< 哪一轮开出去的
    DNS_SERVICE_CANCEL cancel{};
};

/// 回调那一头要用到的东西。**整个进程只有这一份，而且从不释放。**
///
/// ⚠️ **这是有意的，不是懒。** 那几个回调跑在系统的线程池上，而
/// `DnsServiceBrowseCancel` / `DnsServiceResolveCancel` 之后**还可能再回来
/// 一次**（文档没说不会）。把上下文挂在 `Guts` 上的话，关掉那一下
/// `Guts` 一析构，晚到的那一次回调写的就是已经还回去的内存——而那种崩溃
/// 是随机的、隔几天才出一次。所以：上下文活在这儿，关掉只是把 `live`
/// 放倒；回调先看这一面旗，倒了就什么都不做。
///
/// `asks` 同理**只进不出**。一次会话里网段上有几台就几条，几十个字节一条。
struct WinShare {
    std::mutex lk;
    std::atomic<bool> live{false};
    std::atomic<std::uint64_t> gen{0};
    /// 这一轮照下来看见了哪几个实例（全名）。
    std::set<std::wstring> shot;
    /// 实例名 → 对面自报的 id。**走的那一下只给得出实例名**，这张表是桥。
    std::map<std::wstring, std::string> by_instance;
    /// 问出去还没回来的那几条。**从不清空**，见上面那段。
    std::deque<WinAsk> asks;
};

WinShare& win_share() {
    static WinShare one;
    return one;
}

/// 问细节最多同时挂这么多条。挂到这个数就不再往外问了——正常网段上几十
/// 台顶天，到这个数只能是有人在刷，再问下去就是拿内存换噪声。
constexpr size_t kAskCap = 2048;

}  // namespace

struct Sense::Guts {
    /// 往外报那一条。**撤的时候要拿同一份 request 去撤**，所以整份留着。
    DNS_SERVICE_REGISTER_REQUEST reg{};
    DNS_SERVICE_INSTANCE* reg_inst = nullptr;
    /// 报出去那一下是异步的，等它回话。
    HANDLE reg_done = nullptr;
    std::atomic<DWORD> reg_status{ERROR_SUCCESS};
    bool reg_sent = false;
    /// 叫停。**手动复位**——一置上，所有在等的地方一起醒。
    HANDLE quit = nullptr;
    std::thread spin;
};

bool Sense::supported() { return true; }

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

    g->quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g->reg_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g->quit == nullptr || g->reg_done == nullptr) {
        if (g->quit != nullptr) ::CloseHandle(g->quit);
        if (g->reg_done != nullptr) ::CloseHandle(g->reg_done);
        guts_.reset();
        return;
    }

    // ⚠️ **这三样在开跑之前抄一份。** 底下那条线程活得比这个函数长，而
    // 它们是 `boot()` 写进去的——不抄的话就是跨线程读，而且中间隔着一把
    // 别人的锁。
    const std::string my_id = id_;
    const std::string my_name = name_;
    const int my_port = port_;

    auto& sh = win_share();
    sh.live.store(true);
    const std::uint64_t gen = sh.gen.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lk(sh.lk);
        sh.shot.clear();
        sh.by_instance.clear();
    }

    running_ = true;
    asking_ = true;
    asker_ = std::thread([this] { ask_around(); });

    g->spin = std::thread([this, g, gen, my_id, my_name, my_port] {
        auto& share = win_share();
        // `_changji._tcp` → `_changji._tcp.local`
        const std::wstring family = widen(std::string(kService) + ".local");

        // ---- 往外报一声 ----
        //
        // TXT 里带 id 和名字。**id 是认人的凭据**（`peers.hpp` 上那条：
        // 认 id 不认地址），名字只是给人看的。
        //
        // ⚠️ **实例名必须每个进程唯一。** 同一台机器上跑两个引擎，两边都拿
        // 主机名去注册的话，网上只看得见一条——两条记录被当成同一个服务。
        // 名字后面缀上 id 的头四位就分开了（苹果那一份上写着同一件事，
        // 2026-09-21 实撞过）。**界面上显示的不是它**：那个走 TXT 里的
        // `name`，还是干干净净的主机名。
        const std::wstring inst =
            widen(my_name + "-" + my_id.substr(0, 4)) + L"." + family;
        // 主机名那一份系统自己会出 A 记录，我们只要指对。
        const std::wstring host = widen(my_name) + L".local";
        const std::wstring k_id = L"id";
        const std::wstring k_name = L"name";
        const std::wstring v_id = widen(my_id);
        const std::wstring v_name = widen(my_name);
        PCWSTR keys[2] = {k_id.c_str(), k_name.c_str()};
        PCWSTR vals[2] = {v_id.c_str(), v_name.c_str()};

        g->reg_inst = ::DnsServiceConstructInstance(
            inst.c_str(), host.c_str(), nullptr, nullptr,
            static_cast<WORD>(my_port), 0, 0, 2, keys, vals);
        if (g->reg_inst != nullptr) {
            g->reg.Version = DNS_QUERY_REQUEST_VERSION1;
            g->reg.InterfaceIndex = 0;
            g->reg.pServiceInstance = g->reg_inst;
            g->reg.pQueryContext = g;
            g->reg.hCredentials = nullptr;
            g->reg.unicastEnabled = FALSE;
            // ⚠️ **无捕获的 lambda 当函数指针使。** 这几个回调声明成
            // `WINAPI`（= `__stdcall`），而 lambda 转出来的是默认调用约定
            // ——x64 和 arm64 上只有一种调用约定，两者是同一个类型，编得过。
            // 这个程序不出 32 位的 Windows 包；真要出，这几处得换成写死
            // `__stdcall` 的自由函数。
            g->reg.pRegisterCompletionCallback =
                [](DWORD status, void* ctx, DNS_SERVICE_INSTANCE* si) {
                    auto* gg = static_cast<Sense::Guts*>(ctx);
                    if (gg != nullptr) {
                        gg->reg_status.store(status);
                        ::SetEvent(gg->reg_done);
                    }
                    // **回调给的这一份要自己放**，不是我们构造的那一份。
                    if (si != nullptr) ::DnsServiceFreeInstance(si);
                };
            const DWORD r = ::DnsServiceRegister(&g->reg, nullptr);
            if (r == DNS_REQUEST_PENDING) {
                g->reg_sent = true;
                // 等它回话，但别死等——系统那头不吭声的话，浏览那一半
                // 还是该转起来。
                ::WaitForSingleObject(g->reg_done, 5000);
            }
            const DWORD st = (r == DNS_REQUEST_PENDING) ? g->reg_status.load() : r;
            // **报不出去要说出来。** 咽下去的话界面一直显示"开着"，而整个
            // 网段上没人看得见这一台，人只会以为"对方没开"。
            if (st != ERROR_SUCCESS) {
                std::lock_guard<std::mutex> lk(mu_);
                trouble_ = SAYF("往外报不出去（Windows 回 %1）",
                                std::to_string(static_cast<int>(st)));
            }
        } else {
            // 连实例都没构出来（名字拼错、内存不够）。**也走同一句话**
            // ——多一句要多翻十一种语言，而人看见的信息是一样的。
            std::lock_guard<std::mutex> lk(mu_);
            trouble_ = SAYF("往外报不出去（Windows 回 %1）",
                            std::to_string(static_cast<int>(::GetLastError())));
        }

        // ---- 一轮一轮地照相 ----
        //
        // 开一次浏览、收几秒、停掉，然后和上一轮比。**"走了"这件事只能
        // 这么看出来**（见文件上头那段第三条）。
        bool said_deaf = false;
        while (::WaitForSingleObject(g->quit, 0) != WAIT_OBJECT_0) {
            {
                std::lock_guard<std::mutex> lk(share.lk);
                share.shot.clear();
            }

            DNS_SERVICE_CANCEL browse_cancel{};
            DNS_SERVICE_BROWSE_REQUEST br{};
            br.Version = DNS_QUERY_REQUEST_VERSION1;
            br.InterfaceIndex = 0;
            br.QueryName = family.c_str();
            br.pQueryContext = nullptr;   // 上下文在 `win_share()` 里，见那儿
            br.pBrowseCallback = [](DWORD, void*, DNS_RECORD* rec) {
                auto& sh2 = win_share();
                if (rec != nullptr && sh2.live.load()) {
                    std::lock_guard<std::mutex> lk(sh2.lk);
                    for (DNS_RECORD* r = rec; r != nullptr; r = r->pNext) {
                        // 这一族回来的是 PTR，里头那个名字就是实例全名。
                        if (r->wType != DNS_TYPE_PTR) continue;
                        if (r->Data.PTR.pNameHost == nullptr) continue;
                        sh2.shot.insert(to_w(r->Data.PTR.pNameHost));
                    }
                }
                // **这串记录归我们放。**
                if (rec != nullptr) ::DnsRecordListFree(rec, DnsFreeRecordList);
            };
            const DNS_STATUS bs = ::DnsServiceBrowse(&br, &browse_cancel);
            if (bs != DNS_REQUEST_PENDING) {
                if (!said_deaf) {
                    said_deaf = true;
                    std::lock_guard<std::mutex> lk(mu_);
                    trouble_ = SAYF("听不见别人（Windows 回 %1）",
                                    std::to_string(static_cast<int>(bs)));
                }
                if (::WaitForSingleObject(g->quit, 5000) == WAIT_OBJECT_0) break;
                continue;
            }

            // 收一会儿。**这几秒就是"多久才看得见一台新开的机器"**，
            // 短了收不全（mDNS 的应答是散着回来的），长了人等得着急。
            const bool bye = ::WaitForSingleObject(g->quit, 4000) == WAIT_OBJECT_0;
            ::DnsServiceBrowseCancel(&browse_cancel);
            if (bye) break;

            // ---- 和上一轮比 ----
            std::vector<std::wstring> fresh;   // 这一轮新看见、还没问过细节的
            std::vector<std::string> gone;     // 上一轮在、这一轮不在的
            {
                std::lock_guard<std::mutex> lk(share.lk);
                for (const auto& one : share.shot) {
                    if (share.by_instance.find(one) == share.by_instance.end()) {
                        fresh.push_back(one);
                    }
                }
                for (auto it = share.by_instance.begin();
                     it != share.by_instance.end();) {
                    if (share.shot.count(it->first) == 0) {
                        gone.push_back(it->second);
                        it = share.by_instance.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (!gone.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                for (const auto& id : gone) book_.lost(id);
            }

            // ---- 新来的：问一次细节（地址、端口、TXT）----
            for (const auto& one : fresh) {
                if (::WaitForSingleObject(g->quit, 0) == WAIT_OBJECT_0) break;
                WinAsk* ask = nullptr;
                {
                    std::lock_guard<std::mutex> lk(share.lk);
                    if (share.asks.size() >= kAskCap) break;
                    share.asks.push_back(WinAsk{one, gen, {}});
                    ask = &share.asks.back();   // deque：往后加不搬家
                }
                DNS_SERVICE_RESOLVE_REQUEST rr{};
                rr.Version = DNS_QUERY_REQUEST_VERSION1;
                rr.InterfaceIndex = 0;
                // ⚠️ **`QueryName` 是 `PWSTR`，不是 `PCWSTR`。** 给一份自己
                // 留着的可写缓冲（`ask->name` 活到进程结束），别给临时对象。
                rr.QueryName = ask->name.data();
                rr.pQueryContext = ask;
                rr.pResolveCompletionCallback =
                    [](DWORD status, void* ctx, DNS_SERVICE_INSTANCE* si) {
                        auto* a = static_cast<WinAsk*>(ctx);
                        auto& sh2 = win_share();
                        const bool fresh_enough =
                            a != nullptr && sh2.live.load() &&
                            a->gen == sh2.gen.load();
                        if (si != nullptr && status == ERROR_SUCCESS &&
                            fresh_enough) {
                            Peer p;
                            for (DWORD i = 0; i < si->dwPropertyCount; ++i) {
                                const std::string k = narrow(si->keys[i]);
                                if (k == "id") p.id = narrow(si->values[i]);
                                else if (k == "name") p.name = narrow(si->values[i]);
                            }
                            p.host = narrow(si->pszHostName);
                            p.port = si->wPort;   // **主机序**，见文件上头
                            p.seen_at = now_ms();
                            Sense& s = Sense::instance();
                            // ⚠️ **自己不算。** 自己广播的那一条自己也收得
                            // 到，不挡的话表上第一行永远是自己。
                            if (!p.id.empty() && p.id != s.my_id()) {
                                {
                                    std::lock_guard<std::mutex> lk(sh2.lk);
                                    sh2.by_instance[a->name] = p.id;
                                }
                                std::lock_guard<std::mutex> lk(s.mu_);
                                s.book_.saw(p);
                            }
                        }
                        // **回调给的这一份要自己放。**
                        if (si != nullptr) ::DnsServiceFreeInstance(si);
                    };
                ::DnsServiceResolve(&rr, &ask->cancel);
            }

            // 喘一口再照下一轮。加上上面收的那几秒，一台机器开起来之后
            // 最多七八秒就看得见。
            if (::WaitForSingleObject(g->quit, 3000) == WAIT_OBJECT_0) break;
        }

        // ---- 收工 ----
        //
        // **撤回那一条广播**：不撤的话我们已经关掉了，网段上别人还看得见
        // 一台"在线"的机器，点进去是连不上的。
        if (g->reg_sent) {
            ::ResetEvent(g->reg_done);
            if (::DnsServiceDeRegister(&g->reg, nullptr) == DNS_REQUEST_PENDING) {
                ::WaitForSingleObject(g->reg_done, 3000);
            }
            g->reg_sent = false;
        }
        if (g->reg_inst != nullptr) {
            ::DnsServiceFreeInstance(g->reg_inst);
            g->reg_inst = nullptr;
        }
    });
}

void Sense::stop() {
    if (!running_ || guts_ == nullptr) return;
    running_ = false;
    // 先把旗放倒：晚到的回调从这一下起什么都不做（见 `WinShare` 上那段）。
    win_share().live.store(false);
    auto* g = guts_.get();
    if (g->quit != nullptr) ::SetEvent(g->quit);
    asking_ = false;
    if (asker_.joinable()) asker_.join();
    if (g->spin.joinable()) g->spin.join();
    if (g->quit != nullptr) ::CloseHandle(g->quit);
    if (g->reg_done != nullptr) ::CloseHandle(g->reg_done);
    guts_.reset();
    {
        std::lock_guard<std::mutex> lk(win_share().lk);
        win_share().shot.clear();
        win_share().by_instance.clear();
    }
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

// ⚠️ **这一条原来关在苹果那一档里，2026-09-23 挪出来的。** 它一行平台代码
// 都没有（发 HTTP、读回包、记一笔），关在里头的后果是 Windows 那一档一接上
// 就链不动：`Sense::ask_around` 未解析，而那句报错指的是 `start()` 里那条
// lambda，看不出是"这个函数根本没被编进来"。

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
        const std::string url = url_of(p);
        if (url.empty()) return {};
        return {url, p.their_ticket};
    }
    return {};
}

std::string Sense::their_url(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : book_.list()) {
        if (p.id == id) return url_of(p);
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
