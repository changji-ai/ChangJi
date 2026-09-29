#include "http/listen_addr.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include <asio.hpp>

#include "infer/peer_auth.hpp"
#include "util/say.hpp"

#if !defined(_WIN32)
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace changji::http {

namespace {

/// 回环上 `port` 有没有人在听：连一下，接了就是有。
///
/// ⚠️ **别用阻塞的 connect。** Windows 上连一个没人听的回环端口要等两秒上下（被拒之后
/// 系统还要重试几次 SYN），桌面端每次起来就白白多等两秒。真有人在听的话回环上一毫秒都
/// 用不了，给三百毫秒绰绰有余。
bool someone_listens_on_loopback(int port) {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    bool connected = false;
    sock.async_connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)},
                       [&connected](const asio::error_code& ec) { connected = !ec; });
    io.run_for(std::chrono::milliseconds(300));
    return connected;
}

/// 192.168 → 0，10 → 1，172.16 → 2。排序用。
int lan_rank(const std::string& ip) {
    if (ip.rfind("192.168.", 0) == 0) return 0;
    if (ip.rfind("10.", 0) == 0) return 1;
    return 2;
}

}  // namespace

bool port_usable(const std::string& host, int port) {
    if (port <= 0 || port > 65535) return false;
    if (someone_listens_on_loopback(port)) return false;
    try {
        asio::io_context io;
        asio::ip::tcp::acceptor acc(io);
        const asio::ip::tcp::endpoint ep(asio::ip::make_address(host), static_cast<unsigned short>(port));
        acc.open(ep.protocol());
#if !defined(_WIN32)
        // POSIX 上 SO_REUSEADDR 只放过 TIME_WAIT、不放过正在听的，Crow 自己也设它。不设的话
        // 桌面端刚关掉又打开（上一轮的连接还在 TIME_WAIT）就被当成占着、换了端口——地址一换，
        // 别的机器表上那一行就连不上了。
        acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
#endif
        // Windows 上**不设**：那边 SO_REUSEADDR 的意思是"别人正听着也让我绑上"。
        acc.bind(ep);
        acc.close();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool is_lan_ipv4(unsigned char a, unsigned char b) {
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

std::vector<std::string> lan_ipv4s() {
    std::vector<std::string> out;
    const auto keep = [&out](unsigned char a, unsigned char b, const std::string& s) {
        if (!is_lan_ipv4(a, b)) return;
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
#if defined(_WIN32)
    // Windows 上解析自己的机器名，系统就地回每张网卡的地址，不出网、不等。
    try {
        asio::io_context io;
        asio::ip::tcp::resolver r(io);
        for (const auto& e : r.resolve(asio::ip::tcp::v4(), asio::ip::host_name(), "")) {
            const auto addr = e.endpoint().address();
            if (!addr.is_v4()) continue;
            const auto bytes = addr.to_v4().to_bytes();
            keep(bytes[0], bytes[1], addr.to_string());
        }
    } catch (...) {
    }
#else
    // POSIX 上**别解析机器名**：Linux 多半只回 /etc/hosts 里那个 127.0.1.1，macOS 上可能
    // 去问 mDNS、一等好几秒。直接数网卡。
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* p = list; p != nullptr; p = p->ifa_next) {
            if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) continue;
            const auto* in = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
            std::array<unsigned char, 4> bytes{};
            std::copy_n(reinterpret_cast<const unsigned char*>(&in->sin_addr.s_addr), 4, bytes.begin());
            keep(bytes[0], bytes[1], asio::ip::address_v4(bytes).to_string());
        }
        ::freeifaddrs(list);
    }
#endif
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& x, const std::string& y) { return lan_rank(x) < lan_rank(y); });
    if (out.size() > 3) out.resize(3);
    return out;
}

std::string ready_banner(const std::string& bind_host, int port, const std::string& token,
                         const std::vector<std::string>& lan_ips) {
    const std::string p = std::to_string(port);
    const bool pub = infer::is_public_bind(bind_host);
    // 听的是某一个具体地址（`--host 192.168.1.5`）就只摆它；0.0.0.0 / :: / 空 = 所有网卡，
    // 本机那一条写回环，别的电脑那几条写局域网地址。
    const bool any = bind_host.empty() || bind_host == "0.0.0.0" || bind_host == "::" || bind_host == "[::]";
    std::vector<std::string> hosts;
    if (!pub) {
        hosts.push_back(bind_host);   // 回环：照它听的那个写（127.0.0.1 / localhost）
    } else if (any) {
        hosts.push_back("127.0.0.1");
        hosts.insert(hosts.end(), lan_ips.begin(), lan_ips.end());
    } else {
        hosts.push_back(bind_host);
    }
    const bool tokened = pub && !token.empty();

    std::string out = "\n  ";
    // 对外监听而问不出局域网地址（云主机只有公网地址时就这样）：本机那条之外说一句怎么换。
    out += pub && any && lan_ips.empty() ? SAY("场记起来了。在浏览器里打开（别的电脑把 127.0.0.1 换成这台的地址）：")
                                         : SAY("场记起来了。在浏览器里打开：");
    out += "\n\n";
    for (const auto& h : hosts) {
        out += "    http://" + h + ":" + p + "/";
        if (tokened) out += "?token=" + token;
        out += "\n";
    }
    out += "\n  ";
    // 类名跟着界面走：2026-09-28 起口令摆在「互联」那一类的「本机地址与口令」里，
    // 原来那一类叫「机器表」。照旧名字找的人在导航里找不到它。
    out += tokened ? SAYF("口令：%1（可在网页「设置 ▸ 互联」中查看和修改）", token)
                   : SAY("只听本机，不用口令。要让别的电脑也能打开，起的时候加 --host 0.0.0.0");
    out += "\n\n";
    return out;
}

}  // namespace changji::http
