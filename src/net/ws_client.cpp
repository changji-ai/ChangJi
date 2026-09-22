#include "net/ws_client.hpp"

#include <cstring>
#include <random>

#include "util/text.hpp"

namespace changji::ws {

namespace {

std::uint64_t be_read(const std::string& s, std::size_t off, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = (v << 8) | static_cast<unsigned char>(s[off + i]);
    }
    return v;
}

}  // namespace

std::optional<Url> parse_url(const std::string& url) {
    Url u;
    std::string rest;
    if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
    } else if (url.rfind("wss://", 0) == 0) {
        rest = url.substr(6);
        u.tls = true;
        u.port = "443";
    } else if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else if (url.rfind("https://", 0) == 0) {
        rest = url.substr(8);
        u.tls = true;
        u.port = "443";
    } else {
        return std::nullopt;
    }

    const std::size_t slash = rest.find('/');
    std::string authority = rest;
    if (slash != std::string::npos) {
        authority = rest.substr(0, slash);
        u.target = rest.substr(slash);
    }
    if (authority.empty()) return std::nullopt;

    // 只处理 host:port。IPv6 字面量（[::1]:8188）留给以后——
    // ComfyUI 的默认地址是 127.0.0.1，撞上的概率很低，
    // 而把它写错比不支持更糟。
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = authority.substr(colon + 1);
    } else {
        u.host = authority;
    }
    if (u.host.empty() || u.port.empty()) return std::nullopt;
    return u;
}

std::string handshake_request(const Url& u, const std::string& key) {
    std::string host = u.host;
    // 默认端口不写进 Host 头。写了的话有的服务端（和中间的代理）
    // 会认为 Host 和它自己的配置不符。
    const bool default_port = (!u.tls && u.port == "80") ||
                              (u.tls && u.port == "443");
    if (!default_port) host += ":" + u.port;

    return "GET " + u.target + " HTTP/1.1\r\n"
           "Host: " + host + "\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: " + key + "\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "\r\n";
}

std::string accept_key(const std::string& client_key) {
    return text::base64_encode(text::sha1_raw(client_key + kMagic));
}

std::string random_key() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::string raw(16, '\0');
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t v = rng();
        for (int b = 0; b < 8; ++b) {
            raw[i + b] = static_cast<char>((v >> (b * 8)) & 0xFF);
        }
    }
    return text::base64_encode(raw);
}

std::optional<Frame> decode_frame(const std::string& buf, std::size_t& consumed) {
    if (buf.size() < 2) return std::nullopt;

    const auto b0 = static_cast<unsigned char>(buf[0]);
    const auto b1 = static_cast<unsigned char>(buf[1]);

    Frame f;
    f.fin = (b0 & 0x80) != 0;
    f.opcode = static_cast<Opcode>(b0 & 0x0F);

    const bool masked = (b1 & 0x80) != 0;
    std::uint64_t len = b1 & 0x7F;
    std::size_t off = 2;

    // 三种长度编码。只实现前两种是最常见的错误：一段几十 KB 的预览图
    // 就会超过 65535，然后整条连接的解析全乱套——而表现是"收到乱码"，
    // 不是"解析失败"。
    if (len == 126) {
        if (buf.size() < off + 2) return std::nullopt;
        len = be_read(buf, off, 2);
        off += 2;
    } else if (len == 127) {
        if (buf.size() < off + 8) return std::nullopt;
        len = be_read(buf, off, 8);
        off += 8;
    }

    std::uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        // 服务端发来的帧按协议不该带掩码，但带了也照样解——
        // 直接拒绝的话，一个不守规矩的代理能让整条路都用不了。
        if (buf.size() < off + 4) return std::nullopt;
        for (int i = 0; i < 4; ++i) {
            mask[i] = static_cast<std::uint8_t>(buf[off + i]);
        }
        off += 4;
    }

    if (buf.size() < off + len) return std::nullopt;

    f.payload.assign(buf, off, static_cast<std::size_t>(len));
    if (masked) {
        for (std::size_t i = 0; i < f.payload.size(); ++i) {
            f.payload[i] = static_cast<char>(
                static_cast<std::uint8_t>(f.payload[i]) ^ mask[i % 4]);
        }
    }
    consumed = off + static_cast<std::size_t>(len);
    return f;
}

std::string encode_frame(Opcode op, const std::string& payload,
                         std::uint32_t mask_key) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(op)));

    const std::size_t n = payload.size();
    // **客户端发出的帧必须打掩码**（MASK 位置 1），不打的话服务端按协议
    // 要断开连接。这一条最容易漏，因为服务端发来的帧是不打的。
    if (n < 126) {
        out.push_back(static_cast<char>(0x80 | n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>(
                (static_cast<std::uint64_t>(n) >> (i * 8)) & 0xFF));
        }
    }

    std::uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = static_cast<std::uint8_t>((mask_key >> ((3 - i) * 8)) & 0xFF);
        out.push_back(static_cast<char>(mask[i]));
    }
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(
            static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return out;
}

}  // namespace changji::ws
