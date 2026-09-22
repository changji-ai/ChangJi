// /api/media 的测试：越界检查和 HTTP Range。
//
// 这两件事 Python 那边都是白拿的（Starlette 的 FileResponse 自带 206），
// 这里必须自己写，所以也必须自己测。
//
// 越界检查是**安全边界**，不该只在起了服务之后才验证——
// 所以路径解析拆成了纯函数 resolve_media，这个文件直接测它。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "http/media.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path golden_project() {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/project_expectations.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           paths::from_utf8(j.at("root_name").get<std::string>());
}

}  // namespace

TEST_CASE("越界的路径必须被拒绝") {
    const std::string root = paths::to_utf8(golden_project());

    SUBCASE("项目内的真实文件可以取") {
        const auto t = http::resolve_media(root, "project.json");
        CHECK(t.status == 200);
        CHECK(fs::is_regular_file(t.path));
    }

    SUBCASE("用 .. 往上爬要被拦") {
        // 只做字符串前缀比较的话这一条会穿出去
        for (const char* evil : {"../project.json", "../../etc/passwd",
                                 "refs/../../project.json",
                                 "./../../项目_雨夜天台/project.json"}) {
            CAPTURE(evil);
            const auto t = http::resolve_media(root, evil);
            CHECK(t.status == 403);
        }
    }

    SUBCASE("兄弟目录不能靠前缀混进来") {
        // /a/bc 不在 /a/b 之下。用字符串前缀判断会误放行，
        // 所以 resolve_media 走的是 lexically_relative。
        const auto t = http::resolve_media(root + "_别的", "project.json");
        CHECK(t.status != 200);
    }

    SUBCASE("不存在的文件是 404 不是 403") {
        const auto t = http::resolve_media(root, "没有这个文件.png");
        CHECK(t.status == 404);
    }

    SUBCASE("空项目路径是 400") {
        const auto t = http::resolve_media("", "project.json");
        CHECK(t.status == 400);
    }
}

TEST_CASE("Range 头解析") {
    constexpr std::uint64_t kSize = 1000;
    using http::RangeVerdict;

    // 期望值全部来自 Starlette 的 FileResponse._parse_range_header
    // （.venv/Lib/site-packages/starlette/responses.py），不是我推的。
    // 那是对拍要对上的那一侧。

    SUBCASE("普通闭区间") {
        const auto r = http::parse_range("bytes=0-499", kSize);
        REQUIRE(r.verdict == RangeVerdict::Ok);
        CHECK(r.range.first == 0);
        CHECK(r.range.last == 499);
    }

    SUBCASE("开放式结尾：播放器拖进度条发的就是这种") {
        const auto r = http::parse_range("bytes=500-", kSize);
        REQUIRE(r.verdict == RangeVerdict::Ok);
        CHECK(r.range.first == 500);
        CHECK(r.range.last == 999);
    }

    SUBCASE("末尾 N 字节") {
        const auto r = http::parse_range("bytes=-200", kSize);
        REQUIRE(r.verdict == RangeVerdict::Ok);
        CHECK(r.range.first == 800);
        CHECK(r.range.last == 999);
    }

    SUBCASE("末尾 N 超过文件长度就给整个文件") {
        const auto r = http::parse_range("bytes=-5000", kSize);
        REQUIRE(r.verdict == RangeVerdict::Ok);
        CHECK(r.range.first == 0);
        CHECK(r.range.last == 999);
    }

    SUBCASE("末端越界要夹到文件末尾") {
        const auto r = http::parse_range("bytes=900-99999", kSize);
        REQUIRE(r.verdict == RangeVerdict::Ok);
        CHECK(r.range.first == 900);
        CHECK(r.range.last == 999);
    }

    SUBCASE("起点越界回 416，不是 400") {
        // **这一条是对拍抓出来的。** 原来的实现把它和"语法不认识"
        // 混成一种，都回 416；Python 只有这一种回 416。
        CHECK(http::parse_range("bytes=1000-", kSize).verdict ==
              RangeVerdict::NotSatisfiable);
        CHECK(http::parse_range("bytes=5000-6000", kSize).verdict ==
              RangeVerdict::NotSatisfiable);
    }

    SUBCASE("起点大于终点回 400") {
        CHECK(http::parse_range("bytes=500-100", kSize).verdict ==
              RangeVerdict::Malformed);
    }

    SUBCASE("语法不认识回 400") {
        for (const char* bad : {"", "bytes", "bytes=", "bytes=abc-def",
                                "items=0-10", "0-100", "bytes=abc"}) {
            CAPTURE(bad);
            CHECK(http::parse_range(bad, kSize).verdict == RangeVerdict::Malformed);
        }
    }

    SUBCASE("bytes=--5 算出来的起点比文件还大，回 416 不是 400") {
        // Python 的 int("-5") 是合法的，于是起点算成 1000-(-5)=1005，
        // 走到越界那一步才被拦下。看着像语法错，判定却是 416——
        // 这种地方不照抄 Starlette 的顺序就会不一样。
        CHECK(http::parse_range("bytes=--5", kSize).verdict ==
              RangeVerdict::NotSatisfiable);
    }

    SUBCASE("多区间当没看见，回整个文件") {
        // 有意的偏差：Python 回 206 + multipart/byteranges。
        // 不实现的理由见 media.hpp——浏览器的 <video> 不发这种，
        // 而且那个格式的分隔串是随机的，两侧永远逐字节对不上。
        CHECK(http::parse_range("bytes=0-99,200-299", kSize).verdict ==
              RangeVerdict::Ignore);
    }

    SUBCASE("空文件：任何区间的起点都越界") {
        CHECK(http::parse_range("bytes=0-", 0).verdict ==
              RangeVerdict::NotSatisfiable);
    }
}

TEST_CASE("Content-Type 按扩展名给") {
    CHECK(http::content_type_for("a/b/c.mp4") == "video/mp4");
    CHECK(http::content_type_for("x.PNG") == "image/png");   // 大小写不敏感
    CHECK(http::content_type_for("x.jpeg") == "image/jpeg");
    CHECK(http::content_type_for("x.wav") == "audio/wav");
    CHECK(http::content_type_for("x.srt") == "application/x-subrip");
    // sd-cli 会在 -o 给的名字后面再补 .avi，成片实际是这个扩展名
    CHECK(http::content_type_for("out.mp4.avi") == "video/x-msvideo");
    // 注意 from_utf8。直接写 content_type_for("没有扩展名") 会抛
    // "No mapping for the Unicode character"——fs::path 的 const char*
    // 构造按 ANSI 代码页解释，而源码是 UTF-8。
    // 这是这条规矩第三次咬人（前两次在 test_project.cpp 和 proc::which），
    // 纯 ASCII 的字面量没事，带中文的一律要走 from_utf8。
    CHECK(http::content_type_for(paths::from_utf8("没有扩展名")) ==
          "application/octet-stream");
}
