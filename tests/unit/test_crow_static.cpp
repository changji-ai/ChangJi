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
