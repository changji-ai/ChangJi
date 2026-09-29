#include "util/net_inward.hpp"

#include "util/say.hpp"

#include <cctype>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace changji::util {

bool ipv4_inward(std::uint32_t ip) {
    const unsigned a = ip >> 24, b = (ip >> 16) & 0xff;
    if (a == 127 || a == 10 || a == 0) return true;               // 本机 / 内网 / 0.0.0.0
    if (a == 172 && b >= 16 && b <= 31) return true;              // 172.16/12
    if (a == 192 && b == 168) return true;                        // 192.168/16
    if (a == 169 && b == 254) return true;                        // 云厂商的元数据口子
    if (a == 100 && b >= 64 && b <= 127) return true;             // 运营商级 NAT
    if (a >= 224) return true;                                    // 组播、保留
    return false;
}

/// 照 inet_aton 的规矩读一个 IPv4：一到四段，每段十进制 / 0 开头八进制 / 0x 十六进制，
/// 最后一段填满剩下的字节（`127.1` = 127.0.0.1，`2130706433` = 127.0.0.1）。
bool parse_ipv4_loose(const std::string& h, std::uint32_t& out) {
    std::vector<std::uint64_t> parts;
    std::size_t i = 0;
    while (i <= h.size()) {
        if (i == h.size() || h[i] == '.') return false;   // 空段
        std::uint64_t v = 0;
        int base = 10;
        if (h[i] == '0' && i + 1 < h.size() && (h[i + 1] == 'x' || h[i + 1] == 'X')) {
            base = 16;
            i += 2;
            if (i >= h.size() || h[i] == '.') return false;
        } else if (h[i] == '0' && i + 1 < h.size() && h[i + 1] != '.') {
            base = 8;
        }
        std::size_t digits = 0;
        while (i < h.size() && h[i] != '.') {
            const char c = h[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else return false;   // 带别的字母：是域名
            if (d >= base) return false;
            v = v * base + d;
            if (v > 0xffffffffULL) return false;
            ++i;
            ++digits;
        }
        if (digits == 0) return false;
        parts.push_back(v);
        if (parts.size() > 4) return false;
        if (i == h.size()) break;
        ++i;   // 跳过 '.'
        if (i == h.size()) return false;   // 末尾一个点
    }
    std::uint32_t ip = 0;
    const std::size_t n = parts.size();
    for (std::size_t k = 0; k + 1 < n; ++k) {
        if (parts[k] > 255) return false;
        ip |= static_cast<std::uint32_t>(parts[k]) << (24 - 8 * k);
    }
    const std::uint64_t last = parts.back();
    const int bits = 32 - 8 * static_cast<int>(n - 1);
    if (bits < 32 && last >= (std::uint64_t(1) << bits)) return false;
    ip |= static_cast<std::uint32_t>(last);
    out = ip;
    return true;
}

/// 读一个 IPv6 字面量（可带 `::` 缩写，末尾可带点分 IPv4）。读不懂回 false。
bool parse_ipv6(const std::string& h, std::uint8_t out[16]) {
    std::string s = h.substr(0, h.find('%'));   // 去掉 zone id
    std::vector<std::uint16_t> head, tail;
    bool gap = false;
    std::vector<std::uint16_t>* cur = &head;
    std::size_t i = 0;
    if (s.rfind("::", 0) == 0) {
        gap = true;
        cur = &tail;
        i = 2;
    }
    while (i < s.size()) {
        std::size_t j = s.find(':', i);
        std::string part = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (part.find('.') != std::string::npos) {   // 末尾的 IPv4
            std::uint32_t v4 = 0;
            if (!parse_ipv4_loose(part, v4) || j != std::string::npos) return false;
            cur->push_back(static_cast<std::uint16_t>(v4 >> 16));
            cur->push_back(static_cast<std::uint16_t>(v4 & 0xffff));
            i = s.size();
            break;
        }
        if (part.empty() || part.size() > 4) return false;
        unsigned v = 0;
        for (char c : part) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            v = v * 16 + (std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10));
        }
        cur->push_back(static_cast<std::uint16_t>(v));
        if (j == std::string::npos) break;
        if (j + 1 < s.size() && s[j + 1] == ':') {
            if (gap) return false;   // 两个 ::
            gap = true;
            cur = &tail;
            i = j + 2;
        } else {
            i = j + 1;
            if (i == s.size()) return false;
        }
    }
    const std::size_t n = head.size() + tail.size();
    if (n > 8 || (!gap && n != 8)) return false;
    std::vector<std::uint16_t> all = head;
    all.resize(8 - tail.size(), 0);
    all.insert(all.end(), tail.begin(), tail.end());
    for (int k = 0; k < 8; ++k) {
        out[2 * k] = static_cast<std::uint8_t>(all[k] >> 8);
        out[2 * k + 1] = static_cast<std::uint8_t>(all[k] & 0xff);
    }
    return true;
}


bool ipv6_inward(const std::uint8_t b[16]) {
    bool all0 = true;
    for (int k = 0; k < 15; ++k) all0 = all0 && b[k] == 0;
    if (all0 && (b[15] == 0 || b[15] == 1)) return true;             // :: / ::1
    if ((b[0] & 0xfe) == 0xfc) return true;                            // fc00::/7
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;            // fe80::/10
    bool mapped = true;                                                // ::ffff:a.b.c.d
    for (int k = 0; k < 10; ++k) mapped = mapped && b[k] == 0;
    if (mapped && b[10] == 0xff && b[11] == 0xff) {
        return ipv4_inward((std::uint32_t(b[12]) << 24) | (std::uint32_t(b[13]) << 16) |
                           (std::uint32_t(b[14]) << 8) | b[15]);
    }
    // NAT64（64:ff9b::/96）里包着的 IPv4 也按那个 IPv4 判。
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
        bool zero = true;
        for (int k = 4; k < 12; ++k) zero = zero && b[k] == 0;
        if (zero) {
            return ipv4_inward((std::uint32_t(b[12]) << 24) | (std::uint32_t(b[13]) << 16) |
                               (std::uint32_t(b[14]) << 8) | b[15]);
        }
    }
    return false;
}

bool literal_inward(const std::string& host, bool& is_literal) {
    is_literal = false;
    if (host.find(':') != std::string::npos) {
        is_literal = true;
        std::uint8_t b[16] = {};
        if (!parse_ipv6(host, b)) return true;   // 读不懂的就当指着里面
        return ipv6_inward(b);
    }
    std::uint32_t v4 = 0;
    if (parse_ipv4_loose(host, v4)) {
        is_literal = true;
        return ipv4_inward(v4);
    }
    return false;
}

Resolved resolve_host(const std::string& host) {
    Resolved out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
#ifdef _WIN32
        out.error = "getaddrinfo " + std::to_string(rc);
#else
        out.error = ::gai_strerror(rc);
#endif
        return out;
    }
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {};
        if (p->ai_family == AF_INET) {
            const auto* a = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
            ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof buf);
            out.inward = out.inward || ipv4_inward(ntohl(a->sin_addr.s_addr));
        } else if (p->ai_family == AF_INET6) {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
            ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof buf);
            out.inward = out.inward || ipv6_inward(reinterpret_cast<const std::uint8_t*>(&a->sin6_addr));
        } else {
            continue;
        }
        if (buf[0] != '\0') out.addrs.emplace_back(buf);
    }
    ::freeaddrinfo(res);
    return out;
}

PublicHost vet_public_host(const std::string& host) {
    PublicHost out;
    const bool local_name = host.empty() || host == "localhost" ||
                            (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0);
    bool literal = false;
    if (local_name || literal_inward(host, literal)) {
        out.refuse = SAYF("这个地址指着本机或者内网（%1），不给开", host);
        return out;
    }
    if (literal) return out;
    const Resolved r = resolve_host(host);
    if (r.addrs.empty()) {
        out.refuse = SAYF("找不到 %1 这个地址（%2）", host, r.error);
        return out;
    }
    if (r.inward) {
        out.refuse = SAYF("这个地址解析到了本机或者内网（%1），不给开", host);
        return out;
    }
    out.pin = r.addrs.front();
    return out;
}

}  // namespace changji::util
