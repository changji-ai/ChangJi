#pragma once

// 一个够用的 WebSocket 客户端（协议那一半）。
//
// ⚠️ **它现在一个调用方都没有，别按"死代码"删。**
//
// 留着是为了**多机互联：一个 changji 连另一个 changji 的 `/ws`**。
// 那条路上要当客户端的是我们自己，而 `/ws` 是我们自己的服务端，
// 所以这份编解码是照着那一头写的、也只需要伺候那一头。
// 2026-09-10 判过一次删留，结论是留——判断和理由记在
// 方案文档「ws_client 为什么留着」那一节。
//
// **为什么自己写。** Crow 只有服务端（前端连过来看进度），
// 没有客户端。为一个客户端再拉一个库（websocketpp、Boost.Beast）代价不小：
// Beast 要整个 Boost，websocketpp 停更多年且 API 依赖旧版 asio。
// 而我们要的功能是**收文本帧**——发只发一次握手，连 ping 都可以只回不发。
//
// **传输那一半不在这儿，而且已经没了。** 连 socket 的那段代码原来住在
// `tests/compat/main.cpp` 里（对拍工具直接用 asio 连），阶段 8 删对拍时
// 一起走了。所以真要用起来的人**还得自己写一层传输**——那是几十行 asio，
// 想省事的话去 git 历史里抄那个 `WsPeek` 类（连上、握手、订阅、读几条）：
//     git show 17514f1:changji/cpp/tests/compat/main.cpp | sed -n '1550,1650p'
// 写在这儿是因为"以为拿来就能连"会浪费掉半天。
//
// 协议部分（握手串、帧的编解码）是纯函数，单独暴露出来测。
// 那部分错了不会崩，只会"连不上"或者"收到的消息是乱码"，
// 而在真实服务端上试错一次要几十秒。
//
// 不支持的：wss（TLS）、分片续帧的**跨帧文本拼接**除外的扩展、压缩扩展。
// 我们自己的 /ws 是明文，要 TLS 的部署放个反向代理。
//
// **原来放在 src/comfy/ 底下**，那是历史位置——它一直是通用的，
// 只是最早的用途是连 ComfyUI。2026-09-10 拆 ComfyUI 时挪到 src/net/。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace changji::ws {

/// RFC 6455 那个固定 GUID。拼在客户端 key 后面算 accept。
inline constexpr const char* kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// 拆一个 ws:// 地址。
struct Url {
    std::string host;
    std::string port = "80";
    /// 路径带查询串，直接写进请求行。
    std::string target = "/";
    bool tls = false;
};

/// 认不出返回 nullopt。
std::optional<Url> parse_url(const std::string& url);

/// 握手请求。key 是 16 字节随机数的 base64。
std::string handshake_request(const Url& u, const std::string& key);

/// 服务端该回的 Sec-WebSocket-Accept。
///
/// base64(sha1(key + GUID))，**中间不经过十六进制**。
/// 拿 sha1 的十六进制串去 base64 会得到 56 个字符，服务端算的是 28 个。
std::string accept_key(const std::string& client_key);

/// 16 字节随机数的 base64。每次连接都要不同——固定的话，
/// 中间有缓存的代理可能把上一次的响应当成这一次的。
std::string random_key();

/// 帧的操作码。只列用得上的。
enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/// 解出来的一帧。
struct Frame {
    Opcode opcode = Opcode::Text;
    bool fin = true;
    std::string payload;
};

/// 从缓冲区头部解一帧。
///
/// 返回 nullopt 表示**数据还不够**，调用方再读一点再来（不是错误）。
/// consumed 是这一帧占了多少字节，只有返回有值时才有意义。
///
/// 长度有三种编码（7 位 / 16 位 / 64 位），只实现前两种是最常见的错误：
/// 一段几十 KB 的预览图就会超过 65535，然后整条连接的解析全乱套。
std::optional<Frame> decode_frame(const std::string& buf, std::size_t& consumed);

/// 编一帧。**客户端发出的帧必须打掩码**，不打的话服务端按协议要断开连接。
std::string encode_frame(Opcode op, const std::string& payload,
                         std::uint32_t mask_key);

}  // namespace changji::ws
