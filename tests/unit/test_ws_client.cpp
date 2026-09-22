// WebSocket 协议部分的测试。
//
// 这些错了**不会崩**，只会"连不上"或者"收到的消息是乱码"。
// 而在真实服务端上试错一次要几十秒，还得先起一个后端。
//
// **被测的那份现在没有调用方**，留着是为了多机互联（一个 changji 连
// 另一个的 `/ws`），见 net/ws_client.hpp 开头。所以这些用例也别跟着删——
// 真接上那条路的时候，它们是唯一能提前告诉你握手串写没写对的东西。

#include <doctest/doctest.h>

#include <string>

#include "net/ws_client.hpp"
#include "util/text.hpp"

using namespace changji;
namespace ws = changji::ws;

TEST_CASE("accept 是 base64(sha1(key+GUID))，中间不经过十六进制") {
    // RFC 6455 里那个例子。拿 sha1 的十六进制串去 base64 的话会得到
    // 56 个字符，而服务端算出来是 28 个——握手失败，报错只是"连不上"。
    CHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("base64 的补齐位不能少") {
    // 少补的话 Sec-WebSocket-Key 长度不对，服务端直接拒绝连接。
    CHECK(text::base64_encode("") == "");
    CHECK(text::base64_encode("f") == "Zg==");
    CHECK(text::base64_encode("fo") == "Zm8=");
    CHECK(text::base64_encode("foo") == "Zm9v");
    CHECK(text::base64_encode("foob") == "Zm9vYg==");
    CHECK(text::base64_encode("fooba") == "Zm9vYmE=");
    CHECK(text::base64_encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("sha1 的原始字节和十六进制版是同一个摘要") {
    const std::string raw = text::sha1_raw("abc");
    REQUIRE(raw.size() == 20);
    CHECK(text::sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");

    std::string hex;
    for (const char c : raw) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
        hex += buf;
    }
    CHECK(hex == text::sha1_hex("abc"));
}

TEST_CASE("随机 key 每次都不同") {
    // 固定的话，中间有缓存的代理可能把上一次的响应当成这一次的。
    const std::string a = ws::random_key();
    const std::string b = ws::random_key();
    CHECK(a != b);
    CHECK(a.size() == 24);   // 16 字节 base64 出来是 24 个字符
    CHECK(a.back() == '=');
}

TEST_CASE("地址拆解") {
    SUBCASE("带端口和查询串") {
        const auto u = ws::parse_url("ws://192.168.1.9:8188/ws?clientId=abc");
        REQUIRE(u.has_value());
        CHECK(u->host == "192.168.1.9");
        CHECK(u->port == "8188");
        // 查询串要留在请求行里，丢了的话 ComfyUI 不知道这条连接是谁的，
        // 于是一条进度消息都不会推过来
        CHECK(u->target == "/ws?clientId=abc");
        CHECK_FALSE(u->tls);
    }

    SUBCASE("不带端口时按协议取默认值") {
        const auto u = ws::parse_url("ws://comfy.local/ws");
        REQUIRE(u.has_value());
        CHECK(u->port == "80");
        const auto s = ws::parse_url("wss://comfy.local/ws");
        REQUIRE(s.has_value());
        CHECK(s->port == "443");
        CHECK(s->tls);
    }

    SUBCASE("不带路径时补一个斜杠") {
        const auto u = ws::parse_url("ws://127.0.0.1:8188");
        REQUIRE(u.has_value());
        CHECK(u->target == "/");
    }

    SUBCASE("认不出的返回空") {
        CHECK_FALSE(ws::parse_url("127.0.0.1:8188/ws").has_value());
        CHECK_FALSE(ws::parse_url("ws://").has_value());
        CHECK_FALSE(ws::parse_url("").has_value());
    }
}

TEST_CASE("握手请求的必备头一个都不能少") {
    const auto u = ws::parse_url("ws://127.0.0.1:8188/ws?clientId=x");
    REQUIRE(u.has_value());
    const std::string req = ws::handshake_request(*u, "dGhlIHNhbXBsZSBub25jZQ==");
    CAPTURE(req);

    CHECK(req.rfind("GET /ws?clientId=x HTTP/1.1", 0) == 0);
    CHECK(req.find("Host: 127.0.0.1:8188") != std::string::npos);
    CHECK(req.find("Upgrade: websocket") != std::string::npos);
    CHECK(req.find("Connection: Upgrade") != std::string::npos);
    CHECK(req.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==") !=
          std::string::npos);
    // 版本必须是 13。不写或者写别的，服务端回 426 而不是 101
    CHECK(req.find("Sec-WebSocket-Version: 13") != std::string::npos);
    // 头部以空行结束
    CHECK(req.size() >= 4);
    CHECK(req.substr(req.size() - 4) == "\r\n\r\n");

    SUBCASE("默认端口不写进 Host") {
        // 写了的话有的服务端和中间的代理会认为 Host 和它自己的配置不符。
        const auto d = ws::parse_url("ws://comfy.local/ws");
        REQUIRE(d.has_value());
        const std::string r = ws::handshake_request(*d, "k");
        CHECK(r.find("Host: comfy.local\r\n") != std::string::npos);
    }
}

TEST_CASE("帧解码：三种长度编码都要认") {
    // 只实现前两种是最常见的错误：一段几十 KB 的预览图就会超过 65535，
    // 然后整条连接的解析全乱套——而表现是"收到乱码"，不是"解析失败"。
    SUBCASE("7 位长度") {
        const std::string wire = std::string("\x81\x05", 2) + "hello";
        std::size_t used = 0;
        const auto f = ws::decode_frame(wire, used);
        REQUIRE(f.has_value());
        CHECK(f->opcode == ws::Opcode::Text);
        CHECK(f->fin);
        CHECK(f->payload == "hello");
        CHECK(used == wire.size());
    }

    SUBCASE("16 位长度") {
        const std::string body(300, 'x');
        std::string wire;
        wire.push_back(static_cast<char>(0x81));
        wire.push_back(static_cast<char>(126));
        wire.push_back(static_cast<char>((300 >> 8) & 0xFF));
        wire.push_back(static_cast<char>(300 & 0xFF));
        wire += body;

        std::size_t used = 0;
        const auto f = ws::decode_frame(wire, used);
        REQUIRE(f.has_value());
        CHECK(f->payload == body);
        CHECK(used == wire.size());
    }

    SUBCASE("64 位长度") {
        const std::string body(70000, 'y');
        std::string wire;
        wire.push_back(static_cast<char>(0x82));   // 二进制帧，预览图就是这个
        wire.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            wire.push_back(static_cast<char>(
                (static_cast<std::uint64_t>(70000) >> (i * 8)) & 0xFF));
        }
        wire += body;

        std::size_t used = 0;
        const auto f = ws::decode_frame(wire, used);
        REQUIRE(f.has_value());
        CHECK(f->opcode == ws::Opcode::Binary);
        CHECK(f->payload.size() == 70000);
        CHECK(used == wire.size());
    }
}

TEST_CASE("数据不够时返回空，不是报错") {
    // TCP 是流，一帧被切成两半到达是常态。当成错误的话，
    // 连接会在第一个大消息上断掉，而那正是进度消息最多的时候。
    const std::string full = std::string("\x81\x05", 2) + "hello";
    for (std::size_t n = 0; n < full.size(); ++n) {
        CAPTURE(n);
        std::size_t used = 0;
        CHECK_FALSE(ws::decode_frame(full.substr(0, n), used).has_value());
    }
    std::size_t used = 0;
    CHECK(ws::decode_frame(full, used).has_value());
}

TEST_CASE("一个缓冲区里连着多帧要能逐个取出") {
    // 服务端在一次 write 里塞几条进度消息是常见的。
    const std::string a = std::string("\x81\x03", 2) + "aaa";
    const std::string b = std::string("\x81\x04", 2) + "bbbb";
    std::string buf = a + b;

    std::size_t used = 0;
    auto f1 = ws::decode_frame(buf, used);
    REQUIRE(f1.has_value());
    CHECK(f1->payload == "aaa");
    buf.erase(0, used);

    auto f2 = ws::decode_frame(buf, used);
    REQUIRE(f2.has_value());
    CHECK(f2->payload == "bbbb");
    buf.erase(0, used);
    CHECK(buf.empty());
}

TEST_CASE("服务端带了掩码也要能解") {
    // 按协议服务端发的帧不该带掩码，但直接拒绝的话，
    // 一个不守规矩的代理能让整条路都用不了。
    const std::string masked = ws::encode_frame(ws::Opcode::Text, "hello",
                                                0x12345678);
    std::size_t used = 0;
    const auto f = ws::decode_frame(masked, used);
    REQUIRE(f.has_value());
    CHECK(f->payload == "hello");
}

TEST_CASE("客户端发的帧必须打掩码") {
    // 不打的话服务端按协议要断开连接。这一条最容易漏，
    // 因为服务端发来的帧是不打的，照着抄就错了。
    const std::string wire = ws::encode_frame(ws::Opcode::Ping, "", 0xDEADBEEF);
    REQUIRE(wire.size() >= 2);
    CHECK((static_cast<unsigned char>(wire[1]) & 0x80) != 0);   // MASK 位

    SUBCASE("掩码之后的载荷不等于原文") {
        const std::string s = ws::encode_frame(ws::Opcode::Text, "AAAA", 0x01020304);
        // 头 2 字节 + 4 字节掩码 + 4 字节载荷
        REQUIRE(s.size() == 10);
        CHECK(s.substr(6) != "AAAA");
    }
}

TEST_CASE("编出来的帧能被自己解回去") {
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{125},
                                std::size_t{126}, std::size_t{65535},
                                std::size_t{65536}}) {
        CAPTURE(n);
        const std::string body(n, 'z');
        const std::string wire =
            ws::encode_frame(ws::Opcode::Text, body, 0xA1B2C3D4);
        std::size_t used = 0;
        const auto f = ws::decode_frame(wire, used);
        REQUIRE(f.has_value());
        CHECK(f->payload == body);
        CHECK(used == wire.size());
    }
}
