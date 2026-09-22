// 引擎自己发前端那一层。
//
// 这一层没有 Python 对应物（Python 那边前端是 Node 发的，它自己只有一页
// 内置界面），所以不是对拍，是纯粹的新逻辑。
//
// 盯三件事：
//   **路径规整**——`..` 要挡住。现在文件都在内存里翻不出去，
//   但这段逻辑哪天改成读磁盘，`..` 就是目录穿越；
//   **谁接管**——`/api` 开头的绝不能被前端截走，否则整套接口全变成 HTML；
//   **Content-Type 带 charset**——界面全是中文，少了它简体 Windows 上
//   浏览器按本地代码页猜，一片乱码。

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>

#include "http/bff_routes.hpp"
#include "http/bundled_webapp.inc.hpp"
#include "http/webapp.hpp"

using namespace changji;

TEST_CASE("请求路径规整成 dist 里的相对路径") {
    CHECK(http::normalize_webapp_path("/") == "index.html");
    CHECK(http::normalize_webapp_path("") == "index.html");
    CHECK(http::normalize_webapp_path("/index.html") == "index.html");
    CHECK(http::normalize_webapp_path("/assets/index-abc.js") ==
          "assets/index-abc.js");
    // 多个前导斜杠也该收拾干净
    CHECK(http::normalize_webapp_path("///assets/a.css") == "assets/a.css");
    // 查询串不算路径
    CHECK(http::normalize_webapp_path("/index.html?x=1") == "index.html");
    CHECK(http::normalize_webapp_path("/?a=b") == "index.html");
}

TEST_CASE("`..` 和反斜杠一律挡掉") {
    // **现在挡住，不是因为现在会出事。** 文件都在内存里，`..` 翻不出去。
    // 是因为这段规整逻辑哪天改成读磁盘，`..` 就是目录穿越，
    // 而那时候没人会想起来这里少了一道判断。
    CHECK(http::normalize_webapp_path("/../etc/passwd").empty());
    CHECK(http::normalize_webapp_path("/assets/../../secret").empty());
    CHECK(http::normalize_webapp_path("/..").empty());
    // Windows 上反斜杠同样是分隔符
    CHECK(http::normalize_webapp_path("/assets\\..\\x").empty());
    CHECK(http::normalize_webapp_path("/a\\b").empty());
}

TEST_CASE("接口路径不能被前端截走") {
    // 截走了的话整套接口全变成 HTML，而前端拿到 HTML 去 JSON.parse，
    // 报的是"意外的 <"——离真正的原因隔着十万八千里。
    CHECK_FALSE(http::webapp_owns("/api/health"));
    CHECK_FALSE(http::webapp_owns("/api/run"));
    CHECK_FALSE(http::webapp_owns("/api"));
    CHECK_FALSE(http::webapp_owns("/ws"));

    // 前端自己的路由归它管
    CHECK(http::webapp_owns("/"));
    CHECK(http::webapp_owns("/production"));
    CHECK(http::webapp_owns("/assets/index-abc.js"));
    // **前缀判断要连着斜杠**：以后真有个 /apidoc 页面，
    // 用 "/api" 裸前缀会把它误判成接口。
    CHECK(http::webapp_owns("/apidoc"));
    CHECK(http::webapp_owns("/api-guide"));
}

TEST_CASE("Content-Type 认得出常见类型，而且都带 charset") {
    CHECK(http::webapp_content_type("index.html") == "text/html; charset=utf-8");
    CHECK(http::webapp_content_type("assets/a.js") ==
          "text/javascript; charset=utf-8");
    CHECK(http::webapp_content_type("assets/a.css") == "text/css; charset=utf-8");
    CHECK(http::webapp_content_type("x.json") == "application/json; charset=utf-8");

    // 二进制的不该带 charset——带了有些代理会拿它当文本改编码
    CHECK(http::webapp_content_type("a.png") == "image/png");
    CHECK(http::webapp_content_type("a.woff2") == "font/woff2");

    // 不认识的别瞎猜
    CHECK(http::webapp_content_type("a.unknown") == "application/octet-stream");
}

TEST_CASE("前端真的嵌进来了") {
    // **这一条钉的是"打包过"这件事本身。** 忘了跑 gen_webapp.py 的话，
    // 编出来的二进制照样能起、接口照样好使，只是打开端口一片空白——
    // 比报错难查得多，因为服务是活的。
    const auto* index = http::find_webapp_file("index.html");
    // 这句话和 server.cpp 那条 500 是同一份跑法的两份拷贝，两边都要能照着
    // 敲通：`cd` 完要回到 changji/（不然 cpp/tools 不在手边），解释器写
    // python3（macOS 和多数 Linux 里没有 `python`）。
    REQUIRE_MESSAGE(index != nullptr,
                    "index.html 没嵌进来。在 changji/ 目录下跑一下："
                    "cd webapp/client && npm run build && cd ../.. ，然后 "
                    "python3 cpp/tools/gen_webapp.py（Windows 上是 python）");
    const std::string html(*index);
    CHECK(html.find("<script") != std::string::npos);
    // Vite 打出来的入口一定引 /assets/ 下的东西；没有就是空壳
    CHECK(html.find("/assets/") != std::string::npos);

    CHECK(http::find_webapp_file("不存在的文件.js") == nullptr);
}

TEST_CASE("webapp 要的每一条 /bff 接口，C++ 这边都得有") {
    // **这条用例是被一个真 bug 逼出来的。** 设置页调 /bff/settings/overview，
    // 而 C++ 那时只实现了 health / status / config / flow——那个请求 404，
    // 整页当引擎离线处理：地址空、配置文件空、右上角写"连不上"。
    // 而顶栏读的是实现了的 /bff/settings/status，显示"引擎已连接"。
    // **同一个页面上两个相反的结论**，用户先看到的是后者，于是来问
    // "为什么我这里引擎显示是连不上的"。
    //
    // 单元测试看不见这种错：两边各自都是对的，错的是**中间少了一条**。
    // 所以这里拿真正打包进去的前端代码里出现的 /bff 路径，去查 kBffRoutes。
    std::set<std::string> wanted;
    for (const auto& [name, content] : http::kBundledWebappChunks) {
        const std::string body(content);
        for (std::size_t i = body.find("/bff/"); i != std::string::npos;
             i = body.find("/bff/", i + 1)) {
            std::size_t j = i;
            while (j < body.size() &&
                   (std::isalnum(static_cast<unsigned char>(body[j])) != 0 ||
                    body[j] == '/' || body[j] == '_' || body[j] == '-')) {
                ++j;
            }
            wanted.insert(body.substr(i, j - i));
        }
    }
    REQUIRE_MESSAGE(!wanted.empty(),
                    "打包进来的前端里一条 /bff 都没找到，这个用例失效了");

    for (const auto& path : wanted) {
        CAPTURE(path);
        const bool have =
            std::find(std::begin(http::kBffRoutes), std::end(http::kBffRoutes),
                      std::string_view(path)) != std::end(http::kBffRoutes);
        CHECK(have);
    }
}
TEST_CASE("webapp 连的那个 WebSocket 地址，C++ 这边得有") {
    // **同 /bff 那条，同一个形状，又栽了一次。**
    //
    // 引擎自己一直是 /ws，而前端拼的是 /api/ws——它原来跑在 Node 那层
    // 后面，那层把 /api/* 整个转给引擎。webapp 嵌进二进制之后转发没了，
    // 前端连的地址 404。
    //
    // **这次的症状特别难往这儿想**：连不上时 run store 退回 1.2 秒轮询，
    // 顶上的总进度、阶段名、事件流全都照常走。只有镜头墙上每张牌的进度
    // 和状态是**只**吃 WebSocket 的。于是用户点了「重新生成」，那一格
    // 一动不动，而页面别处一切正常——看着像那一格坏了。
    std::set<std::string> wanted;
    for (const auto& [name, content] : http::kBundledWebappChunks) {
        const std::string body(content);
        // 前端拼的是 `${proto}//${host}/api/ws`，打包之后那一段字面量
        // 就是 "/api/ws"。按同样的办法找出所有 ws 结尾的路径。
        for (std::size_t i = body.find("/ws"); i != std::string::npos;
             i = body.find("/ws", i + 1)) {
            // 往前吃到路径开头（可能是 /api/ws 这种带前缀的）
            std::size_t b = i;
            while (b > 0) {
                const char c = body[b - 1];
                if (std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                    c == '/' || c == '_' || c == '-') {
                    --b;
                } else {
                    break;
                }
            }
            // 后面紧跟字母数字的不算（比如 "/wsdl"、"/ws_foo"）
            const std::size_t after = i + 3;
            if (after < body.size() &&
                (std::isalnum(static_cast<unsigned char>(body[after])) != 0 ||
                 body[after] == '_' || body[after] == '-')) {
                continue;
            }
            const std::string path = body.substr(b, after - b);
            if (!path.empty() && path[0] == '/') wanted.insert(path);
        }
    }
    REQUIRE_MESSAGE(!wanted.empty(),
                    "打包进来的前端里一个 ws 地址都没找到，这个用例失效了");

    for (const auto& path : wanted) {
        CAPTURE(path);
        const bool have =
            std::find(std::begin(http::kWsRoutes), std::end(http::kWsRoutes),
                      std::string_view(path)) != std::end(http::kWsRoutes);
        CHECK(have);
    }
}

TEST_CASE("要文件还是要路由，得分得开") {
    // 分不开的话「找不到就回 index.html」会把缺失的资源也变成一页 HTML，
    // 而且是 200——浏览器按 <script> 取回一页 HTML，解析当场失败、页面全白，
    // 可每一个请求都是 200，什么都提示不出来。2026-09-10 排查白屏卡在这。
    SUBCASE("前端路由：没有扩展名") {
        CHECK_FALSE(http::wants_file("shots"));
        CHECK_FALSE(http::wants_file("project"));
        CHECK_FALSE(http::wants_file(""));
        CHECK_FALSE(http::wants_file("settings/"));
    }
    SUBCASE("具体文件：带扩展名") {
        CHECK(http::wants_file("index.html"));
        CHECK(http::wants_file("assets/index-Abc123.js"));
        CHECK(http::wants_file("assets/ShotsView-x9.css"));
        CHECK(http::wants_file("favicon.ico"));
        CHECK(http::wants_file("fonts/x.woff2"));
    }
    SUBCASE("以点开头的不当资源") {
        CHECK_FALSE(http::wants_file(".gitkeep"));
    }
    SUBCASE("点在结尾也不算") {
        CHECK_FALSE(http::wants_file("weird."));
    }
}

TEST_CASE("缓存头：index.html 每次核，带哈希的资源随便缓存") {
    // index.html 里写死了带哈希的资源名。不发 no-cache 的话浏览器会按
    // 启发式规则自己缓存它，换一版之后照着旧 html 去取已经不存在的资源，
    // **刷新也没用**——刷新拿到的还是缓存里那份 html。
    CHECK(http::webapp_cache_control("index.html") == "no-cache");
    CHECK(http::webapp_cache_control("") == "no-cache");

    const std::string a = http::webapp_cache_control("assets/index-Abc123.js");
    CHECK(a.find("immutable") != std::string::npos);
    CHECK(a.find("max-age=") != std::string::npos);

    // assets 底下的目录形式不是资源，别给长缓存
    CHECK(http::webapp_cache_control("assets/") == "no-cache");
}
