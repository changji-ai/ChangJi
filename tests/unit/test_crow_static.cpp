// Crow 发静态文件，路径里带中文（cmake/patch_crow_utf8_path.cmake，2026-09-25）。
//
// Crow 1.2.0 那两处走窄字符 API：Windows 上窄字符串按系统代码页解，UTF-8 的中文
// 目录名一个都 stat 不到——`/api/media` 回 500，设定、镜头墙、播放器全是空的。
// 默认起的片名全是中文，所以这不是边角情形。
//
// 这条用例直接调打过补丁的那个函数，也把它读文件那一半（`u8path` 开的流）走一遍。
// 在 macOS / Linux 上它本来就绿；要它红得起来的是 Windows 那一份构建。

#include <doctest/doctest.h>

#include <crow.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

TEST_CASE("Crow 发静态文件：带中文的路径照样 stat 得到、长度对、读得出来") {
    const fs::path dir = fs::temp_directory_path() / paths::from_utf8("changji_走路带风_静态");
    std::error_code ec;
    fs::create_directories(dir / paths::from_utf8("参考图"), ec);
    const fs::path file = dir / paths::from_utf8("参考图") / paths::from_utf8("林知夏_正面.png");
    const std::string bytes = "\x89PNG\r\n\x1a\nnot really a picture";
    {
        std::ofstream(file, std::ios::binary) << bytes;
    }

    crow::response res;
    res.set_static_file_info_unsafe(paths::to_utf8(file));
    CHECK(res.code == 200);
    CHECK(res.get_header_value("Content-Length") == std::to_string(bytes.size()));
    CHECK(res.is_static_type());

    // 发文件那一下（`do_write_static`）开流的方式和补丁里一样：Windows 上走 u8path。
#ifdef _WIN32
    std::ifstream is(fs::u8path(paths::to_utf8(file)), std::ios::binary);
#else
    std::ifstream is(paths::to_utf8(file), std::ios::binary);
#endif
    const std::string back((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    CHECK(back == bytes);

    SUBCASE("不在的文件照旧 404（补丁没把判据放宽）") {
        crow::response gone;
        gone.set_static_file_info_unsafe(paths::to_utf8(dir / paths::from_utf8("没有这张.png")));
        CHECK(gone.code == 404);
    }
    SUBCASE("目录不算文件") {
        crow::response d;
        d.set_static_file_info_unsafe(paths::to_utf8(dir));
        CHECK(d.code == 404);
    }
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 按区间发（cmake/patch_crow_static_range.cmake，2026-09-25）
// ---------------------------------------------------------------------------
//
// `/api/media` 的分段请求原来读进内存再回，拖一下整部电影就是一两个 G。
// 补丁让 crow 的分块写从中间开始、只发那么长。这条起一个真的 crow 服务，
// 拿 httplib 去要，**逐字节比**：偏移没人理的话回的是文件开头那一段，
// 长度对、内容错——画面花掉而不报错，只有比字节看得出来。

namespace {

int free_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor acc(io, {asio::ip::make_address("127.0.0.1"), 0});
    return acc.local_endpoint().port();
}

struct Reply {
    int status = 0;
    std::string content_length;
    std::string body;
};

/// 在一条已经连上的 socket 上发一个 GET、按 Content-Length 收完回包。
/// 测试程序不链 httplib（见 CMakeLists 里 changji_tests 那段），手写这一小段。
Reply get(asio::ip::tcp::socket& sock, const std::string& path) {
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    asio::write(sock, asio::buffer(req));
    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n");
    std::istream in(&buf);
    Reply r;
    std::string line;
    std::getline(in, line);
    r.status = std::stoi(line.substr(9, 3));
    std::size_t len = 0;
    while (std::getline(in, line) && line != "\r") {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "content-length") {
            r.content_length = line.substr(colon + 2, line.size() - colon - 3);
            len = std::stoul(r.content_length);
        }
    }
    // read_until 可能多读进来一截正文，先把 streambuf 里剩下的拿走
    r.body.assign(std::istreambuf_iterator<char>(in), {});
    if (r.body.size() < len) {
        std::string rest(len - r.body.size(), '\0');
        asio::read(sock, asio::buffer(rest));
        r.body += rest;
    }
    return r;
}

}  // namespace

TEST_CASE("Crow 按区间发静态文件：从中间开始、只发那么长、不多一个字节") {
    const fs::path file = fs::temp_directory_path() / "changji_static_range.bin";
    std::string bytes(100000, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
    { std::ofstream(file, std::ios::binary) << bytes; }

    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    std::uint64_t off = 0, len = 0;
    CROW_ROUTE(app, "/f")([&] {
        crow::response res;
        res.set_static_file_info_unsafe(paths::to_utf8(file));
        res.code = 206;
        res.changji_static_range(off, len);
        return res;
    });
    CROW_ROUTE(app, "/whole")([&] {
        crow::response res;
        res.set_static_file_info_unsafe(paths::to_utf8(file));
        return res;
    });
    const int port = free_port();
    REQUIRE(port > 0);
    auto running = app.bindaddr("127.0.0.1").port(static_cast<std::uint16_t>(port)).run_async();
    app.wait_for_server_start();

    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
    SUBCASE("中间一段，跨好几个 16 KB 的块") {
        off = 40000;
        len = 30000;
        const auto r = get(sock, "/f");
        CHECK(r.status == 206);
        CHECK(r.content_length == "30000");
        CHECK(r.body == bytes.substr(40000, 30000));
    }
    SUBCASE("到文件尾") {
        off = 99990;
        len = 10;
        const auto r = get(sock, "/f");
        CHECK(r.body == bytes.substr(99990));
    }
    SUBCASE("同一条连接上接着要整个文件：上一次的偏移不带过来") {
        off = 5;
        len = 5;
        auto r = get(sock, "/f");
        CHECK(r.body == bytes.substr(5, 5));
        r = get(sock, "/whole");
        CHECK(r.status == 200);
        CHECK(r.body == bytes);
    }
    std::error_code sec;
    sock.close(sec);

    app.stop();
    running.wait();
    std::error_code ec;
    fs::remove(file, ec);
}
