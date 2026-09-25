// 项目新建、删除、改梗概的测试。
//
// 删项目那个是整套接口里**唯一不可逆**的操作，所以三道闸每一道都单独测，
// 而且每道都测"闸挡住了"和"闸放行了"两边——只测放行的话，闸失效了
// 测试照样绿，而那时候一次误点就能把跑了一夜的成片全删了。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/projects.hpp"
#include "http/readonly.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 取规范形式再比路径。
///
/// macOS 上 /var 是 /private/var 的软链，而 ProjectPaths 构造时会
/// weakly_canonical——于是"传进去的路径"和"回来的路径"字面上不相等，
/// 尽管指的是同一个目录。两边都规范化，比的才是同一件事。
fs::path canon(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

/// 一个临时的项目库。settings.workspace 指到这里。
struct Workspace {
    fs::path root;
    config::Settings settings;

    explicit Workspace(const std::string& tag) {
        root = fs::temp_directory_path() / paths::from_utf8("changji_库_" + tag);
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        settings.workspace = paths::to_utf8(root);
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
};

/// 在库里建一个真项目，返回它的目录。
fs::path make_project(const Workspace& ws, const std::string& name) {
    const fs::path dir = ws.root / paths::from_utf8(name);
    models::ProjectStore::create(dir, text::project_slug(name), name);
    return dir;
}

}  // namespace

TEST_CASE("新建项目：只填名字就落在项目库里") {
    // 让用户去猜容器里的绝对路径是没道理的，他也不知道项目库挂在哪。
    Workspace ws("新建");
    const auto r = http::guard([&] {
        return http::post_new_project(
            json{{"path", "雨夜天台"}, {"title", "雨夜天台"}}, ws.settings);
    });
    REQUIRE(r.status == 200);

    const fs::path made = paths::from_utf8(r.body.at("root").get<std::string>());
    CHECK(fs::is_directory(made));
    // **两边都取规范形式再比。** ProjectPaths 的构造函数会
    // weakly_canonical 一下（那是有意的，projects.cpp 的"在不在库里"
    // 靠它），而 macOS 上 /var 是 /private/var 的软链——
    // 临时目录拿到的是 /var/folders/...，规范化之后变成 /private/var/...，
    // 直接比字符串就永远不相等。Windows 和 Linux 上两者本来就一样。
    CHECK(canon(made.parent_path()) == canon(ws.root));
    CHECK(models::ProjectStore(made).exists());

    const models::Project p = models::ProjectStore(made).load_project();
    CHECK(p.title == "雨夜天台");
    // 纯中文目录名要走 sha1 退路，不能是空的 id
    CHECK_FALSE(p.project_id.empty());
    CHECK(p.project_id.rfind("p-", 0) == 0);
}

TEST_CASE("新建横屏项目：assets 里那份比例拷贝也要是横的") {
    // **画幅是唯一的源**（config::VideoConfig::aspect_ratio），assets.json
    // 里 StyleProfile.aspect_ratio 只是派生出来的一份拷贝，给出参考图用。
    // 派生原来只在「存画面」那条路上做，而建项目不经过它——于是直接建一个
    // 横屏项目，配出来就是「横屏 + 9:16」：成片横的、参考图竖的，而参考图
    // 正是每一镜的底子。2026-09-17 照用户的话建横屏项目时当场撞上。
    Workspace ws("横屏");
    const auto r = http::guard([&] {
        return http::post_new_project(
            json{{"path", "横屏剧"}, {"orientation", "landscape"}}, ws.settings);
    });
    REQUIRE(r.status == 200);
    const fs::path made = paths::from_utf8(r.body.at("root").get<std::string>());
    const auto assets = models::ProjectStore(made).load_assets();
    CHECK(assets.style.aspect_ratio == "16:9");

    SUBCASE("竖屏要显式选：aspect_ratio 跟着变 9:16") {
        // 2026-09-18 默认翻成横屏之后，竖屏不再是「不说话就有」的那一档。
        const auto r2 = http::guard([&] {
            return http::post_new_project(
                json{{"path", "竖屏片"}, {"orientation", "portrait"}}, ws.settings);
        });
        REQUIRE(r2.status == 200);
        const fs::path m2 = paths::from_utf8(r2.body.at("root").get<std::string>());
        CHECK(models::ProjectStore(m2).load_assets().style.aspect_ratio == "9:16");
    }

    SUBCASE("不给画幅就是横屏 16:9") {
        // 用户 2026-09-18 把产品定位改成电影制作平台，默认从 portrait 翻成
        // landscape。**源和那份派生的拷贝要一起翻**：只翻 [video].orientation
        // 而 assets.json 里还写着 9:16 的话，成片横的、参考图竖的——而参考图
        // 正是每一镜的底子（就是外层这条用例当年撞上的那件事）。
        const auto r3 = http::guard([&] {
            return http::post_new_project(json{{"path", "不说画幅"}}, ws.settings);
        });
        REQUIRE(r3.status == 200);
        const fs::path m3 = paths::from_utf8(r3.body.at("root").get<std::string>());
        CHECK(config::load_settings(m3).video.orientation == "landscape");
        CHECK(models::ProjectStore(m3).load_assets().style.aspect_ratio == "16:9");

        // **而且是钉在盘上的**，不是靠内置默认答出来的：新建那一下就把
        // [video].orientation 写进了项目自己的 changji.toml。这样读它的人
        // 走的是「项目自己说了」那一支，以后内置默认再翻也不动这个项目。
        std::ifstream in(m3 / "changji.toml", std::ios::binary);
        const std::string t3((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        CHECK(t3.find("orientation = \"landscape\"") != std::string::npos);
    }
}

TEST_CASE("老项目的画幅：读出来是它自己那一档，在设置页存一次也不会被翻横") {
    // **老项目目录里没有 changji.toml。** 写那份文件的是
    // http::post_new_project，`ProjectStore::create` 不写——手工建的、
    // 早于项目模板那一批，画幅在盘上的记录只有 assets.json 里那份比例
    // （仓库自己的夹具 tests/golden/项目_雨夜天台/ 就是这一类）。
    //
    // 2026-09-18 内置默认翻成 landscape 之后，不从那儿推的话两件事会坏，
    // 而且都不报错：
    //   · 一个已经出了两百镜 544×928 的项目再补一镜出的是 928×544，
    //     同一章两种画幅，装配时要么上下黑边要么直接拼坏；
    //   · 用户在设置页碰一下「后期 / 画面」，引擎就把 landscape 永久写进
    //     这个竖屏项目——不可逆，也没有任何提示。
    Workspace ws("老项目画幅");
    const fs::path root = ws.root / paths::from_utf8("老竖屏剧");
    auto store = models::ProjectStore::create(root, "lao-shupin", "老竖屏剧");
    {
        // 这个项目所有参考图和已出镜头都是 9:16
        auto assets = store.load_assets();
        assets.style.aspect_ratio = "9:16";
        store.save_assets(assets);
    }
    REQUIRE_FALSE(fs::exists(root / "changji.toml"));

    // 设置页那两条路（GET /bff/project/video、GET /bff/project/finish）
    // 报的都是这个值，用户看到的于是是竖屏。
    const config::Settings s = config::load_settings(root);
    CHECK(s.video.orientation == "portrait");

    // 装配和出图那条路上对的那笔账（pipeline::run_episode 里那句
    // `assets.style.aspect_ratio = settings.video.aspect_ratio()`）
    // 对老项目**是恒等的**：两头相等，所以它翻不动这个项目。
    CHECK(s.video.aspect_ratio() == store.load_assets().style.aspect_ratio);

    SUBCASE("在设置页按一下保存，盘上落下的是竖屏那一档") {
        // 下面这一句就是 /bff/project/finish 起底那一段真跑的东西
        // （server.cpp 里 `write_project_config(root, s.video)`）——
        // 用户改的是后期和声音，根本没碰画幅。
        REQUIRE(config::write_project_config(root, s.video));

        std::ifstream in(root / "changji.toml", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        // 模板的注释里写着 `orientation = "portrait" | "landscape"`，所以
        // 只能找**赋值那一行**的原样；带 = 号的这一串在注释里不成立。
        CHECK(text.find("orientation = \"landscape\"") == std::string::npos);
        // 再读一遍——这次答话的是文件本身，不是推出来的。
        CHECK(config::load_settings(root).video.orientation == "portrait");
        CHECK(config::load_settings(root).video.size() ==
              std::pair<int, int>{544, 928});
    }
}

TEST_CASE("新建项目：带路径分隔符的当绝对/相对路径用") {
    Workspace ws("带路径");
    const fs::path elsewhere =
        fs::temp_directory_path() / paths::from_utf8("changji_别处");
    std::error_code ec;
    fs::remove_all(elsewhere, ec);

    const auto r = http::guard([&] {
        return http::post_new_project(
            json{{"path", paths::to_utf8(elsewhere)}}, ws.settings);
    });
    REQUIRE(r.status == 200);
    // 没被塞进项目库
    CHECK(canon(paths::from_utf8(r.body.at("root").get<std::string>())) ==
          canon(elsewhere));
    CHECK(models::ProjectStore(elsewhere).exists());
    fs::remove_all(elsewhere, ec);
}

// ---- 片名里带 `..` 的，拦在门外 ----
//
// 名字**是模型填的**（`create_project` 那个工具的 `name`），而模型读得到
// 网页和热榜（`stages/web_tools`）——一段带路径的字从那儿进来，就能往项目库
// 外面写东西。
//
// 2026-09-21 实撞：拿假模型让代理调 `create_project`，名字写 `../逃出去`。
// 工具回了一句「建好了：../逃出去」，而项目库里什么都没有——真东西建在了
// **仓库旁边**（相对路径是按进程的工作目录解析的），连那一轮的对话记录都
// 写了进去。项目库列表里看不见它，人只会觉得"建好的片子丢了"。
TEST_CASE("新建项目：片名里带 `..` 的一律拒绝，别往项目库外面写") {
    Workspace ws("越界");
    const auto try_new = [&](const json& body) {
        return http::guard([&] {
            return http::post_new_project(body, ws.settings);
        });
    };

    for (const char* bad : {"../逃出去", "../../更远", "子目录/../../外面",
                            ".."}) {
        CAPTURE(bad);
        const auto r = try_new(json{{"path", bad}});
        CHECK(r.status == 400);
    }

    // 相对路径**一律落在项目库里**，不按进程的工作目录算。带一层子目录也
    // 照样在里面。
    const auto ok = try_new(json{{"path", "子目录/片子"}});
    REQUIRE(ok.status == 200);
    const fs::path root = paths::from_utf8(ok.body.at("root").get<std::string>());
    CAPTURE(paths::to_utf8(root));
    const fs::path lib = canon(ws.settings.workspace_path());
    const fs::path rel = canon(root).lexically_relative(lib);
    CHECK(!rel.empty());
    CHECK(paths::to_utf8(*rel.begin()) != "..");
}

TEST_CASE("新建项目的几种拒绝") {
    Workspace ws("拒绝");
    const auto try_new = [&](const json& body) {
        return http::guard([&] {
            return http::post_new_project(body, ws.settings);
        });
    };

    CHECK(try_new(json{{"path", ""}}).status == 400);
    CHECK(try_new(json{{"path", "   "}}).status == 400);
    CHECK(try_new(json{{"path", "x"}, {"style_line", "别的"}}).status == 400);
    CHECK(try_new(json{{"title", "缺 path"}}).status == 422);

    SUBCASE("同名的要 409 不是 400") {
        // 前端据此提示"换个名字"，400 会被当成参数错。
        CHECK(try_new(json{{"path", "重名"}}).status == 200);
        CHECK(try_new(json{{"path", "重名"}}).status == 409);
    }
}

TEST_CASE("删项目：三道闸") {
    Workspace ws("删除");
    const fs::path proj = make_project(ws, "要删的");

    const auto try_del = [&](const std::string& path,
                             const std::string& confirm) {
        return http::guard([&] {
            return http::post_delete_project(
                json{{"path", path}, {"confirm_name", confirm}}, ws.settings);
        });
    };

    SUBCASE("闸一：不在项目库里的不给删") {
        const fs::path outside =
            fs::temp_directory_path() / paths::from_utf8("changji_库外项目");
        std::error_code ec;
        fs::remove_all(outside, ec);
        models::ProjectStore::create(outside, "outside", "库外");

        const auto r = try_del(paths::to_utf8(outside), "changji_库外项目");
        CHECK(r.status == 403);
        // 一定还在
        CHECK(fs::is_directory(outside));
        fs::remove_all(outside, ec);
    }

    SUBCASE("闸一：项目库自己也不给删") {
        // 等于的话这个接口就能删掉整个项目库，
        // 那是"一次误点删掉所有项目"，比删错一个严重得多。
        const auto r = try_del(paths::to_utf8(ws.root), ws.root.filename().string());
        CHECK(r.status == 403);
        CHECK(fs::is_directory(ws.root));
    }

    SUBCASE("闸一：拿 .. 绕出去也不行") {
        // 字面上看着在库里面，实际指向外面。
        const std::string sneaky =
            paths::to_utf8(ws.root / paths::from_utf8("要删的") / "..") + "/..";
        const auto r = try_del(sneaky, "x");
        CHECK(r.status == 403);
        CHECK(fs::is_directory(ws.root));
    }

    SUBCASE("闸二：不是项目的目录不给删") {
        // 指到一个普通目录上的话，删掉的可能是用户放素材的地方。
        const fs::path plain = ws.root / paths::from_utf8("只是个目录");
        std::error_code ec;
        fs::create_directories(plain, ec);
        {
            std::ofstream out(plain / paths::from_utf8("重要资料.txt"), std::ios::binary);
            out << "别删我";
        }
        const auto r = try_del(paths::to_utf8(plain), "只是个目录");
        CHECK(r.status == 404);
        CHECK(fs::is_regular_file(plain / paths::from_utf8("重要资料.txt")));
    }

    SUBCASE("闸三：名字对不上不给删") {
        const auto r = try_del(paths::to_utf8(proj), "要删的x");
        CHECK(r.status == 400);
        CHECK(r.body.at("detail").get<std::string>().find("要删的") !=
              std::string::npos);
        CHECK(fs::is_directory(proj));

        // 空的确认名也不行
        CHECK(try_del(paths::to_utf8(proj), "").status == 400);
        CHECK(fs::is_directory(proj));
    }

    SUBCASE("三道都过才真删") {
        const auto r = try_del(paths::to_utf8(proj), "要删的");
        REQUIRE(r.status == 200);
        CHECK_FALSE(fs::exists(proj));
        // 项目库本身还在
        CHECK(fs::is_directory(ws.root));
    }
}

TEST_CASE("新建和删除对「只填名字」的理解必须一致") {
    // **2026-09-11 实测撞到的。** 新建那边有条规则："只填名字（不带路径
    // 分隔符）就落在项目库根目录下"——用户不知道项目库挂在哪，让他猜
    // 绝对路径没道理。删除那边以前没有这条，相对路径直接交给
    // weakly_canonical，那是按**进程的工作目录**解析的。
    //
    // 于是同一个 "要删的" 传给两个接口指的是两个地方：新建成功了，拿同样
    // 的字符串去删就是 403「只能删项目库里面的」，而用户看不出自己哪里
    // 错了——他填的就是新建时填的那个名字。
    Workspace ws("同一条规则");
    const fs::path proj = make_project(ws, "只填名字的");
    REQUIRE(fs::is_directory(proj));

    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", "只填名字的"}, {"confirm_name", "只填名字的"}},
            ws.settings);
    });
    CHECK(r.status == 200);
    CHECK_FALSE(fs::exists(proj));
}

TEST_CASE("删项目会连素材和成片一起删") {
    // 这一条是在确认"不可逆"的范围有多大——文档里写了，测试里也要有，
    // 否则将来有人改成"只删 project.json"，测试照样绿。
    Workspace ws("删干净");
    const fs::path proj = make_project(ws, "带素材的");
    std::error_code ec;
    fs::create_directories(proj / "output", ec);
    {
        std::ofstream out(proj / "output" / paths::from_utf8("成片.mp4"), std::ios::binary);
        out << "假的";
    }

    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(proj)}, {"confirm_name", "带素材的"}},
            ws.settings);
    });
    REQUIRE(r.status == 200);
    CHECK_FALSE(fs::exists(proj / "output" / paths::from_utf8("成片.mp4")));
    CHECK_FALSE(fs::exists(proj));
}

TEST_CASE("正在跑的时候不让删项目") {
    // 跑到一半删项目，工作线程下一次写盘会写到一个不存在的目录上，
    // 报的错和"删项目"八竿子打不着。
    Workspace ws("跑着删");
    const fs::path proj = make_project(ws, "在跑的");

    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress& p) {
                               while (!release.load() && !p.cancelled()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(1));
                               }
                           });

    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(proj)}, {"confirm_name", "在跑的"}},
            ws.settings);
    });
    CHECK(r.status == 409);
    CHECK(fs::is_directory(proj));

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("确认名字：目录名和片名都认") {
    // 2026-09-14 之前只认目录名，而界面上到处显示的是片名——一个目录叫
    // convenience-store、片名叫「深夜便利店」的项目，确认框要你打片名才
    // 解锁，打完提交引擎回 400 要目录名，这条路彻底堵死。
    Workspace ws("双名");
    const fs::path proj = ws.settings.workspace_path() / "convenience-store";
    models::ProjectStore::create(proj, "convenience-store", "深夜便利店");

    // 片名认
    auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(proj)}, {"confirm_name", "深夜便利店"}},
            ws.settings);
    });
    CHECK(r.status == 200);
    CHECK_FALSE(fs::exists(proj));

    // 目录名也认
    const fs::path again = ws.settings.workspace_path() / "corner-shop";
    models::ProjectStore::create(again, "corner-shop", "拐角小店");
    r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(again)}, {"confirm_name", "corner-shop"}},
            ws.settings);
    });
    CHECK(r.status == 200);

    // 两个都不是的还是拦住，而且报的是目录名——那是磁盘上的身份
    const fs::path third = ws.settings.workspace_path() / "third-shop";
    models::ProjectStore::create(third, "third-shop", "第三家");
    r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(third)}, {"confirm_name", "随便打的"}},
            ws.settings);
    });
    CHECK(r.status == 400);
    CHECK(fs::is_directory(third));
}

TEST_CASE("跑片的闸只挡正在跑的那个项目") {
    // 单卡上一章要跑很久，而「趁着在跑顺手把测试残留清了」恰恰是这段时间
    // 最想干的事。原来这道闸不看路径，任何项目都删不掉。
    Workspace ws("跑着删别的");
    const fs::path busy = make_project(ws, "在跑的");
    const fs::path idle = make_project(ws, "闲着的");

    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
    std::atomic<bool> release{false};
    pipeline::jobs().start(
        pipeline::JobKind::Run, "ep01",
        [&release](pipeline::JobProgress& p) {
            while (!release.load() && !p.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        },
        "", paths::to_utf8(busy));

    // 正在跑的那个：挡
    auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(busy)}, {"confirm_name", "在跑的"}},
            ws.settings);
    });
    CHECK(r.status == 409);
    CHECK(fs::is_directory(busy));

    // 别的项目：放行
    r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(idle)}, {"confirm_name", "闲着的"}},
            ws.settings);
    });
    CHECK(r.status == 200);
    CHECK_FALSE(fs::exists(idle));

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("不知道在跑哪个项目时一律挡住") {
    // start 的 project 是尾参、默认空串，留空表示"不知道"。放行等于可能
    // 删掉正在跑的那个——比误挡严重得多，所以 fail-closed。
    Workspace ws("不知道跑哪个");
    const fs::path proj = make_project(ws, "无辜的");

    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress& p) {
                               while (!release.load() && !p.cancelled()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(1));
                               }
                           });

    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(proj)}, {"confirm_name", "无辜的"}},
            ws.settings);
    });
    CHECK(r.status == 409);
    CHECK(fs::is_directory(proj));

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("改片名：只动 title，目录一个字不改") {
    Workspace ws("改名");
    const fs::path proj = make_project(ws, "原来的名字");

    auto r = http::guard([&] {
        return http::post_project_rename(
            json{{"project", paths::to_utf8(proj)}, {"title", "  深夜便利店  "}});
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("title") == "深夜便利店");  // 两端空白去掉
    CHECK(fs::is_directory(proj));              // 目录没搬
    CHECK(models::ProjectStore(proj).load_project().title == "深夜便利店");

    // 空名字拦住：放过去的话列表会回落到目录名，看着像"改名没生效"
    r = http::guard([&] {
        return http::post_project_rename(
            json{{"project", paths::to_utf8(proj)}, {"title", "   "}});
    });
    CHECK(r.status == 400);
    CHECK(models::ProjectStore(proj).load_project().title == "深夜便利店");
}

TEST_CASE("正在跑时改名也挡住：跑的那一头会把 title 整份盖回去") {
    Workspace ws("跑着改名");
    const fs::path busy = make_project(ws, "在跑的");
    const fs::path idle = make_project(ws, "闲着的");

    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
    std::atomic<bool> release{false};
    pipeline::jobs().start(
        pipeline::JobKind::Run, "ep01",
        [&release](pipeline::JobProgress& p) {
            while (!release.load() && !p.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        },
        "", paths::to_utf8(busy));

    auto r = http::guard([&] {
        return http::post_project_rename(
            json{{"project", paths::to_utf8(busy)}, {"title", "新名字"}});
    });
    CHECK(r.status == 409);
    CHECK(models::ProjectStore(busy).load_project().title == "在跑的");

    // 别的项目照样能改
    r = http::guard([&] {
        return http::post_project_rename(
            json{{"project", paths::to_utf8(idle)}, {"title", "改成功了"}});
    });
    CHECK(r.status == 200);

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("正在跑的项目在删除目标底下也要挡：remove_all 是递归的") {
    Workspace ws("套娃");
    const fs::path outer = make_project(ws, "外层");
    const fs::path inner = outer / paths::from_utf8("试拍");
    models::ProjectStore::create(inner, "shipai", "试拍");

    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
    std::atomic<bool> release{false};
    pipeline::jobs().start(
        pipeline::JobKind::Run, "ep01",
        [&release](pipeline::JobProgress& p) {
            while (!release.load() && !p.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        },
        "", paths::to_utf8(inner));

    // 删外层会把正在跑的内层一起 remove_all 掉
    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(outer)}, {"confirm_name", "外层"}},
            ws.settings);
    });
    CHECK(r.status == 409);
    CHECK(fs::is_directory(inner));

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("改梗概") {
    Workspace ws("梗概");
    const fs::path proj = make_project(ws, "改梗概的");

    const auto r = http::guard([&] {
        return http::post_project_premise(
            json{{"project", paths::to_utf8(proj)},
                 {"premise", "  林晚在天台等一个七年没出现的人。  "}});
    });
    REQUIRE(r.status == 200);
    // 两端空白要去掉
    CHECK(r.body.at("premise") == "林晚在天台等一个七年没出现的人。");
    CHECK(models::ProjectStore(proj).load_project().premise ==
          "林晚在天台等一个七年没出现的人。");

    SUBCASE("超长的按字符截到 2000") {
        // 按字节截的话中文会被切出半个字，那半个字节进 JSON 时
        // nlohmann 会抛异常——存梗概这件事本身又炸一次。
        std::string long_premise;
        for (int i = 0; i < 3000; ++i) long_premise += "很";
        const auto r2 = http::guard([&] {
            return http::post_project_premise(
                json{{"project", paths::to_utf8(proj)},
                     {"premise", long_premise}});
        });
        REQUIRE(r2.status == 200);
        const std::string got = r2.body.at("premise").get<std::string>();
        CHECK(text::utf8_len(got) == 2000);
        CHECK(got.size() == 6000);   // 三字节一个，没切half
    }

    SUBCASE("项目不存在是 400") {
        const auto r2 = http::guard([&] {
            return http::post_project_premise(
                json{{"project", "Z:/没有这个目录"}, {"premise", "x"}});
        });
        CHECK(r2.status == 400);
    }

    SUBCASE("有故事时两份梗概一起改；没有故事不凭空建一份") {
        // 原来只改 project.json：故事页上还是旧那句，下次从故事页一存又把旧的
        // 写回 project.json。
        models::ProjectStore store(proj);
        CHECK_FALSE(fs::exists(store.paths().story_file()));
        models::Story s;
        s.premise = "旧梗概";
        store.save_story(s);
        const auto r2 = http::guard([&] {
            return http::post_project_premise(
                json{{"project", paths::to_utf8(proj)}, {"premise", "新梗概"}});
        });
        REQUIRE(r2.status == 200);
        CHECK(store.load_story().premise == "新梗概");
        CHECK(store.load_project().premise == "新梗概");
    }
}

TEST_CASE("项目 id 的 slug 规则和角色的不一样") {
    // 分隔符是连字符不是下划线，退路前缀是 "p-" 且取 8 位。
    // 合并的话其中一边的 id 会悄悄变形，而 id 变形意味着老项目打不开。
    CHECK(text::project_slug("Rainy Night") == "rainy-night");
    CHECK(text::project_slug("雨夜天台").rfind("p-", 0) == 0);
    CHECK(text::project_slug("雨夜天台").size() == 10);   // "p-" + 8 位
    CHECK(text::project_slug("a_b-c") == "a_b-c");
    CHECK(text::project_slug("--x--") == "x");

    // 角色那个用下划线，前缀是 "x" 且 6 位
    CHECK(text::slug("Lin Wan") == "lin_wan");
    CHECK(text::slug("林晚").rfind("x", 0) == 0);
    CHECK(text::slug("林晚").size() == 7);
}

TEST_CASE("新建项目时落一份标准的项目配置") {
    // 以前项目目录里没有 changji.toml，只有用户在界面上改过画幅才冒出一个
    // 两行的；而那时 save_user_config 拿全局模板起底，项目配置里会出现
    // [llm]、[workers] 这些机器的属性。现在建项目就写一份只含这部电影自己的属性的。
    Workspace ws("项目配置");
    const auto r = http::guard([&] {
        return http::post_new_project(json{{"path", "配置剧"}}, ws.settings);
    });
    REQUIRE(r.status == 200);
    const fs::path made = paths::from_utf8(r.body.at("root").get<std::string>());
    const fs::path toml = made / "changji.toml";
    REQUIRE(fs::is_regular_file(toml));

    // 解析得动，画幅是内置默认，别的节解析出来就是默认值（模板没有夹带
    // 别的东西），而且机器的属性一个都不在里面。
    const config::Settings s = config::load_settings(made);
    CHECK(s.video.orientation == "landscape");
    CHECK(s.video.quality == "720p");

    // 2026-09-16 从 5.0 抬到 15.0：原来那个数的理由是「8 秒的镜头会中途
    // 硬切」，而同一天查出来中途硬切的根因是运动描述没盖满时长、以及写了
    // 进画出画的动作。上限不该替内容做决定。见 settings.cpp 里那份模板。
    CHECK(s.video.max_shot_s == doctest::Approx(15.0));
    CHECK(s.assembly.crf == config::AssemblyConfig{}.crf);
    CHECK(s.gates.max_attempts_per_shot ==
          config::GateConfig{}.max_attempts_per_shot);
    std::ifstream in(toml, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(text.find("[llm]") == std::string::npos);
    CHECK(text.find("[workers]") == std::string::npos);
    CHECK(text.find("dir =") == std::string::npos);
    // 模板里那两个占位符都要换掉
    CHECK(text.find("@ORIENTATION@") == std::string::npos);
    CHECK(text.find("@QUALITY@") == std::string::npos);

    SUBCASE("建的时候就能定画幅") {
        const auto r2 = http::guard([&] {
            return http::post_new_project(
                json{{"path", "横屏剧"}, {"orientation", "landscape"},
                     {"quality", "hd"}},
                ws.settings);
        });
        REQUIRE(r2.status == 200);
        const fs::path made2 =
            paths::from_utf8(r2.body.at("root").get<std::string>());
        const config::Settings s2 = config::load_settings(made2);
        CHECK(s2.video.orientation == "landscape");
        CHECK(s2.video.quality == "hd");
        CHECK(s2.video.size() == std::pair<int, int>{1280, 704});
    }

    SUBCASE("画幅写错了在建目录之前就拒") {
        const auto r3 = http::guard([&] {
            return http::post_new_project(
                json{{"path", "错画幅"}, {"quality", "4k"}}, ws.settings);
        });
        CHECK(r3.status == 400);
        CHECK_FALSE(fs::exists(ws.root / paths::from_utf8("错画幅")));
    }

    SUBCASE("已经有的那份一个字节都不动") {
        const std::string before = text;
        CHECK_FALSE(config::write_project_config(made, config::VideoConfig{}));
        std::ifstream in2(toml, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(in2)),
                                std::istreambuf_iterator<char>());
        CHECK(after == before);
    }
}

TEST_CASE("每一章报的「几镜」数的是能用的，不是数组长度") {
    // 这个数是三处的判据：顶栏那几个步骤对勾（http/flow.cpp 的 has_shots）、
    // 镜头页按钮上的「还差 N 章分镜」、下拉里的「（17 镜）」。空壳镜头
    // （shot_id 是空串）指不到任何文件、进不了任何一步——报数组长度的话，
    // 一章空壳在这三处都显示成"已经有分镜了"。2026-09-17 实见 ep07。
    Workspace ws("数镜头");
    auto store = models::ProjectStore::create(ws.root / paths::from_utf8("剧"), "p-1", "剧",
                                              models::StyleLine::REALISTIC);
    models::Project pj = store.load_project();
    models::Episode ep;
    ep.episode_id = "ep01";
    models::Shot good;
    good.shot_id = "ep01_sh001";
    ep.shots.push_back(good);
    ep.shots.push_back(models::Shot{});   // 空壳
    ep.shots.push_back(models::Shot{});
    pj.episodes.push_back(ep);
    store.save_project(pj);

    const auto r = http::guard([&] {
        return http::get_project(paths::to_utf8(store.root()));
    });
    REQUIRE(r.status == 200);
    const auto& eps = r.body.at("episodes");
    REQUIRE(eps.size() == 1);
    CHECK(eps[0].at("shots") == 1);   // 三镜里只有一镜能用

    // **状态计数要和它数同一批镜头。** 界面拿这两个数相减算「还差几镜
    // 出片」（EpShots 的 projectPending）——一个数全部、一个数能用的，
    // 就会漏算或者算出负数。
    const auto& st = eps[0].at("status");
    int counted = 0;
    for (auto it = st.begin(); it != st.end(); ++it) counted += it.value().get<int>();
    CHECK(counted == eps[0].at("shots").get<int>());
}

