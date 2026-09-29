#pragma once

// 一个地址是不是**这台机器自己、或者这张内网**（2026-09-26 从 stages/web_tools 搬出来）。
//
// 上网那几个工具（读网页、跟跳转）要拿它判：网址是模型填的，而模型看的页面是从外面来
// 的——页面里写一句「打开 http://169.254.169.254/…」，租来的 GPU 机上那是云厂商的元数据
// 口子；`http://127.0.0.1:8080/api/…` 是引擎自己。
//
// 原来只判**字面**（`points_inward`），域名不解析：可一个解析到 127.0.0.1 的域名（
// `127.0.0.1.nip.io` 这种现成的，或者自己注册一个）就绕过去了。所以现在两层：
//   · 字面那一层照旧（`literal_inward`，inet_aton 那套宽松写法都认）；
//   · 取网页那一层（`llm::default_http_get(..., public_only=true)`）**先解析、每个地址都
//     判，再钉住判过的那个地址去连**（httplib 的 hostname→addr 映射）——判完再让它自己
//     解析一次的话，DNS 在两次之间换个答案（重绑定）就又进来了。

#include <cstdint>
#include <string>
#include <vector>

namespace changji::util {

/// 一个 IPv4 地址（主机序）落不落在本机 / 内网 / 元数据口子 / 组播保留这几段里。
bool ipv4_inward(std::uint32_t ip);

/// 一个 IPv6 地址（16 字节）是不是 ::、::1、fc00::/7、fe80::/10，或者映射进来的内网 IPv4。
bool ipv6_inward(const std::uint8_t b[16]);

/// 照 inet_aton 的规矩读一个 IPv4（`127.1`、`2130706433`、`0x7f.1`、`0177.0.0.1` 都认）。
bool parse_ipv4_loose(const std::string& h, std::uint32_t& out);

/// 读一个 IPv6 字面量（可带 `::` 缩写、末尾点分 IPv4、`%zone`），不带方括号。
bool parse_ipv6(const std::string& h, std::uint8_t out[16]);

/// `host`（小写、不带方括号和端口）是字面地址时 `is_literal = true`，回它指不指着里面。
/// 读不懂的 IPv6 字面量当指着里面。域名回 false、`is_literal = false`。
bool literal_inward(const std::string& host, bool& is_literal);

struct Resolved {
    std::vector<std::string> addrs;   ///< 解析出来的地址（数字串）
    bool inward = false;               ///< 其中**任何一个**指着里面
    std::string error;                 ///< 解析不了时那一句（getaddrinfo 的原话）
};

/// 解析一个主机名（`getaddrinfo`，v4 + v6 都要）。
Resolved resolve_host(const std::string& host);

/// 取网页之前那一道（`llm::default_http_get(..., public_only=true)` 调它）：
/// `host` 小写、不带方括号和端口。
struct PublicHost {
    /// 空 = 可以连；不空 = 不连，这一句给人看（SAY）。
    std::string refuse;
    /// 要钉住去连的地址（域名解析出来、判过的那一个）。字面地址不用钉，空。
    std::string pin;
};
PublicHost vet_public_host(const std::string& host);

}  // namespace changji::util
