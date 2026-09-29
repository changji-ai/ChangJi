// 生成参考图的接口。
//
// **真出图这一段测不了**——那要一张显卡、十几 GB 权重和几十秒。这里测的
// 是它前面那一层：参数校验（走不到出图就该拦下）、和种子那条"重出还是那
// 张图"的规矩。
//
// 校验这一层单独测是有理由的：它每一条都该在**借显存之前**就拦下来。漏一
// 条的话表现不是"报错"，而是"转了几十秒然后报一句参数不对"——错的原因，
// 而且是在最贵的地方报的。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include "pipeline/task_board.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "util/cancel_words.hpp"
#include "http/ref_gen.hpp"
#include "http/upload.hpp"
#include "infer/sd_image.hpp"
#include "stages/frames.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

fs::path fresh_copy(const std::string& tag) {
    const json exp = load_golden("project_expectations");
    const fs::path src = paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
                         paths::from_utf8(exp.at("root_name").get<std::string>());
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_出参考图_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

/// 用例里起的那一批，**出作用域一律停下、等它收干净**，再把假后端撤掉。
///
/// ⚠️ 为什么非有不可（2026-09-27 在 Windows 上撞到）：一键出图那条队列跑在一条
/// 分离出去的线程上（`ref_gen.cpp` 的 `run_queue`），假后端又按引用抓着用例里的
/// 局部变量（`painted`）。用例中途一抛（那天是窄字符路径抛「No mapping for the
/// Unicode character」），局部变量没了，队列还在调它——写的是一块已经还回去的栈，
/// 崩在**后面别的用例里**：0xC0000374 堆损坏，或者 test_render 里一个 SIGSEGV，
/// 三次里一次，单跑哪个文件都不复现。下一个用例还会撞上「另一部电影正在出图」。
///
/// 所以这个守卫**声明在那些局部变量之后**：析构倒着来，线程先收干净，局部变量才没。
/// 等的判据是 `active` 落下——那一下在 `run_queue` 把各路线程都 join 完之后，
/// 之后不会再有谁调假后端。
struct QueueDrain {
    std::string project;
    ~QueueDrain() {
        try {
            http::post_references_generate_all_stop({{"project", project}});
            for (int i = 0; i < 1500; ++i) {   // 最多 30 秒
                if (!http::get_references_queue(project).body.value("active", false)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        } catch (...) {
        }
        http::set_ref_renderer({});
    }
};

/// 跑一次，把 ApiError 的状态码取出来。没抛就是 0。
int status_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const http::ApiError& e) {
        return e.status();
    }
    return 0;
}

}  // namespace

TEST_CASE("参数不对的时候，一步都不许走到出图") {
    // 这几条都该在借显存之前就被拦下。走到出图那一步的话，测试机上会是
    // 「等几十秒然后报一句 sd.cpp 没链」——错的原因，错的时间。
    SUBCASE("朝向认不出来") {
        CHECK(status_of([] {
            http::post_character_reference_generate(
                {{"project", "/nowhere"}, {"char_id", "c_x"}, {"slot", "侧躺"}});
        }) == 400);
    }
    SUBCASE("没给项目") {
        CHECK(status_of([] {
            http::post_character_reference_generate(
                {{"project", ""}, {"char_id", "c_x"}});
        }) == 400);
    }
    SUBCASE("缺 char_id") {
        CHECK(status_of([] {
            http::post_character_reference_generate({{"project", "/nowhere"}});
        }) == 400);
    }
    SUBCASE("缺 location_id") {
        CHECK(status_of([] {
            http::post_location_reference_generate({{"project", "/nowhere"}});
        }) == 400);
    }
}

TEST_CASE("库里没有这个人 / 这个地方，回 404 而不是硬画一张") {
    const fs::path root = fresh_copy("找不到");
    CHECK(status_of([&] {
        http::post_character_reference_generate(
            {{"project", paths::to_utf8(root)}, {"char_id", "c_根本没有"}});
    }) == 404);
    CHECK(status_of([&] {
        http::post_location_reference_generate(
            {{"project", paths::to_utf8(root)}, {"location_id", "loc_没有"}});
    }) == 404);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("不给 seed 就按名字算，同一个槽位每次都是同一个") {
    // 这条是"重出还是那张图"的全部依据。用户点刷新往往只是想确认刚才那张
    // 存下来了，每点一次换一张脸的话他会以为自己弄坏了什么。
    const std::int64_t a = http::ref_seed(json::object(), "c_lin_wan_front");
    const std::int64_t b = http::ref_seed(json::object(), "c_lin_wan_front");
    CHECK(a == b);

    // 三个朝向各是各的种子：同一个种子出三张只会得到三张一样的正面。
    CHECK(a != http::ref_seed(json::object(), "c_lin_wan_three_quarter"));
    CHECK(a != http::ref_seed(json::object(), "c_lin_wan_back"));
    // 换个人也得换
    CHECK(a != http::ref_seed(json::object(), "c_other_front"));
}

TEST_CASE("给了 seed 就用它，负数和超范围的折回来") {
    CHECK(http::ref_seed({{"seed", 12345}}, "随便") == 12345);
    // sd.cpp 那边只收非负，直接透传负数会被当成"随机"，而"随机"正是这个
    // 接口不想要的
    CHECK(http::ref_seed({{"seed", -1}}, "随便") == 2147483647);
    CHECK(http::ref_seed({{"seed", 2147483648LL}}, "随便") == 0);
    // 不是整数就当没给：按名字算，而不是把 1.5 截成 1
    CHECK(http::ref_seed({{"seed", 1.5}}, "随便") ==
          http::ref_seed(json::object(), "随便"));
}

TEST_CASE("画参考图走首帧那条后端：要基础权重、种子定死、图落在 refs/") {
    // 2026-09-16 之前参考图只在本机进程内画，本机没出图模型时「一键出图」
    // 整个不可用，而首帧却能派给别的机器。现在两条路是同一个 FrameRenderer。
    const fs::path root = fresh_copy("走后端");
    const models::ProjectStore store{root};
    const auto assets = store.load_assets();
    REQUIRE(!assets.characters.empty());
    const std::string char_id = assets.characters.begin()->first;

    stages::PromptBundle seen;
    std::string seen_shot;
    http::set_ref_renderer([&](const config::Settings&, const models::ProjectStore&) {
        http::RefBackend b;
        b.render = [&](const models::Shot& shot, const stages::PromptBundle& prompts,
                       const models::TierSpec&, const fs::path& dest,
                       pipeline::CancelToken&, const infer::StepCallback&) {
            seen = prompts;
            seen_shot = shot.shot_id;
            std::ofstream(dest, std::ios::binary) << "png";
        };
        return b;
    });

    const auto r = http::post_character_reference_generate(
        {{"project", paths::to_utf8(root)}, {"char_id", char_id}, {"seed", 77}});
    http::set_ref_renderer({});

    CHECK(r.status == 200);
    CHECK(seen.base_model);                 // 不是 Edit 那一份
    REQUIRE(seen.seed_override.has_value());
    CHECK(*seen.seed_override == 77);       // 接口定的种子原样到后端
    CHECK(seen.reference_images.empty());   // 纯文字画
    CHECK(seen_shot == char_id + "_front");
    CHECK(r.body.at("saved").get<std::string>().rfind("refs/", 0) == 0);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("重画参考图：旧图先留底；画砸了原来那张还在") {
    // 2026-09-25 之前 claim_ref_path 在**出图之前**就删掉同名不同扩展名的
    // 旧图：原来传的是 jpg、这次画 png，画失败或者被停，角色那一栏还指着
    // jpg，而 jpg 已经没了。同扩展名的那张则是被新图原地盖掉，没有底。
    const fs::path root = fresh_copy("留底");
    const models::ProjectStore store{root};
    auto assets = store.load_assets();
    REQUIRE(!assets.characters.empty());
    const std::string char_id = assets.characters.begin()->first;
    const std::string stem = char_id + "_front";
    const fs::path jpg = store.paths().refs() / paths::from_utf8(stem + ".jpg");
    fs::create_directories(jpg.parent_path());
    std::ofstream(jpg, std::ios::binary) << "原来传的那张";
    assets.characters.begin()->second.ref_front = store.paths().rel(jpg);
    store.save_assets(assets);

    bool fail = true;
    http::set_ref_renderer([&](const config::Settings&, const models::ProjectStore&) {
        http::RefBackend b;
        b.render = [&](const models::Shot&, const stages::PromptBundle&,
                       const models::TierSpec&, const fs::path& dest,
                       pipeline::CancelToken&, const infer::StepCallback&) {
            if (fail) throw std::runtime_error("假装出图失败");
            std::ofstream(dest, std::ios::binary) << "新画的";
        };
        return b;
    });
    const json body = {{"project", paths::to_utf8(root)}, {"char_id", char_id}};

    // 画砸了：原来那张还在，库里还指着它
    CHECK(status_of([&] { http::post_character_reference_generate(body); }) == 500);
    CHECK(fs::is_regular_file(jpg));
    CHECK(store.load_assets().characters.at(char_id).ref_front ==
          store.paths().rel(jpg));

    // 画成了：旧的 jpg 清掉，但留底里有它
    fail = false;
    CHECK(http::post_character_reference_generate(body).status == 200);
    CHECK_FALSE(fs::is_regular_file(jpg));
    auto hist = http::ref_history(store, stem);
    REQUIRE(hist.size() == 1);
    CHECK(hist.front().extension() == ".jpg");

    // 再画两次，出来都一样：新画的那张只留一份，不把旧版本挤掉
    CHECK(http::post_character_reference_generate(body).status == 200);
    CHECK(http::post_character_reference_generate(body).status == 200);
    hist = http::ref_history(store, stem);
    REQUIRE(hist.size() == 2);
    CHECK(hist.front().extension() == ".png");   // 新的在前
    CHECK(hist.back().extension() == ".jpg");

    http::set_ref_renderer({});
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("「人按的停」那两句话必须还含着界面认的那两个词") {
    // **界面靠正则认它**：stopped-by-hand.js 里是 `/已停下|已取消/`，认出来
    // 才不把它弹成红色报错——「人按的停不是失败」。那边没有别的判据可用
    // （停是从顶栏那块徽标按的，发请求的是另一个页面里的另一个函数，两边
    // 碰不着面；取消也可能来自另一个标签页）。
    //
    // 这条耦合最坏的性质是**会静默失效**：引擎换个说法，界面一声不响地
    // 开始把取消显示成报错。所以两句话收成了常量（util/cancel_words.hpp），
    // 这儿钉住"换词可以，但得还含着那两个词之一"——**第一版这条用例把
    // 字符串硬编在用例里，改源码根本不会红，是自欺**。
    //
    // 真要彻底换：连 stopped-by-hand.js 那个正则一起换，那边的用例也跟着。
    for (const std::string msg : {std::string(changji::util::kStoppedOne),
                                  std::string(changji::util::kCancelled)}) {
        CAPTURE(msg);
        const bool hit =
            msg.find(changji::util::kStopToken1) != std::string::npos ||
            msg.find(changji::util::kStopToken2) != std::string::npos;
        CHECK_MESSAGE(hit, "界面那条正则认不出这句话了");
    }
}

// ---------------------------------------------------------------------------
// 一键出图：队列在引擎这头
// ---------------------------------------------------------------------------

TEST_CASE("一键出图：一次交一整批，排着的那几张报得出来") {
    // 用户 2026-09-17：「应该将所有图片放到队列里，然后一个一个分配才对」，
    // 以及「明明没有开始的，不是应该显示等待中吗，还有后面名字都一样，谁
    // 知道你在出哪个」。**那两句话问的都是同一件事：排队的那几张说不说得
    // 出来。** 页面自己开几条道的那一版说不出——它手里只有"正在画的那几
    // 张"。所以这儿钉住三样：整批画完、每一张的名字带位置、还没派出去的
    // 那几张在快照里报得出来。
    const fs::path root = fresh_copy("一键出图");
    const models::ProjectStore store{root};
    const std::string path = paths::to_utf8(root);

    // 先把已有的参考图全撤掉，好让这一批真有活干。
    {
        auto assets = store.load_assets();
        for (auto& [id, c] : assets.characters) {
            c.ref_front.reset();
            c.ref_three_quarter.reset();
            c.ref_back.reset();
        }
        for (auto& [id, l] : assets.locations) l.ref_empty.reset();
        store.save_assets(assets);
    }
    const auto before = store.load_assets();
    const std::size_t want =
        before.characters.size() * 3 + before.locations.size();
    REQUIRE(want > 1);
    // 中途抛了也要把这一批停下、收干净（见 QueueDrain）。
    const QueueDrain drain{path};

    // **慢一点的假后端**：真跑得太快的话，下面那次快照会落在"已经全画完"
    // 上，而要看的正是"还排着几张"。
    http::set_ref_renderer([&](const config::Settings&, const models::ProjectStore&) {
        http::RefBackend b;
        b.lanes = 1;   // 一条道，好让"排着的"确定地存在
        b.render = [](const models::Shot&, const stages::PromptBundle&,
                      const models::TierSpec&, const fs::path& dest,
                      pipeline::CancelToken&, const infer::StepCallback&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            std::ofstream(dest, std::ios::binary) << "png";
        };
        return b;
    });

    const auto started = http::post_references_generate_all({{"project", path}});
    CHECK(started.status == 202);
    CHECK(started.body.at("total") == want);

    // 跑着的时候问一次：**排着的那几张要报得出名字**。
    bool saw_pending = false;
    bool saw_label_with_slot = false;
    for (int i = 0; i < 200; ++i) {
        const auto snap = http::get_references_queue(path);
        if (!snap.body.value("active", false)) break;
        const auto pending = snap.body.value("pending", json::array());
        if (!pending.empty()) {
            saw_pending = true;
            for (const auto& it : pending) {
                CHECK(!it.at("target").get<std::string>().empty());
                CHECK(!it.at("label").get<std::string>().empty());
            }
        }
        for (const auto& it : snap.body.value("running", json::array())) {
            // 「董平 正面」——**带位置**。只有名字的话，同一个人的三张在
            // 那一行上长得一模一样（用户：「后面名字都一样」）。
            const auto label = it.at("label").get<std::string>();
            if (label.find(' ') != std::string::npos) saw_label_with_slot = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(saw_pending);
    CHECK(saw_label_with_slot);

    // 等它跑完
    for (int i = 0; i < 400; ++i) {
        if (!http::get_references_queue(path).body.value("active", false)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto done = http::get_references_queue(path);
    CHECK(done.body.value("active", true) == false);
    CHECK(done.body.value("done", std::size_t{0}) == want);
    CHECK(done.body.value("failed", std::size_t{1}) == 0);

    // 一张不缺了就该当场说"都齐了"，而不是再排一批空活。
    const auto again = http::post_references_generate_all({{"project", path}});
    CHECK(again.status == 200);
    CHECK(again.body.at("total") == 0);

    const auto after = store.load_assets();
    for (const auto& [id, c] : after.characters) {
        CHECK_MESSAGE(c.ref_front.has_value(), id);
        CHECK_MESSAGE(c.ref_three_quarter.has_value(), id);
        CHECK_MESSAGE(c.ref_back.has_value(), id);
    }
    for (const auto& [id, l] : after.locations) CHECK_MESSAGE(l.ref_empty.has_value(), id);

    http::set_ref_renderer({});
}

TEST_CASE("一键出图：排着的时候人传了一张，那一格不再画、传的那张不被换掉") {
    // 名单是按下去那一刻拍的；排着的这几十分钟里人可能已经传了一张。原来照画不误：
    // 画完 settle 把人传的那张删掉、角色那栏指向新画的，还把这个人的镜头全退回重出。
    const fs::path root = fresh_copy("排着的时候传了一张");
    const models::ProjectStore store{root};
    const std::string path = paths::to_utf8(root);
    std::string last_loc;
    {
        auto assets = store.load_assets();
        for (auto& [id, c] : assets.characters) {
            c.ref_front.reset();
            c.ref_three_quarter.reset();
            c.ref_back.reset();
        }
        for (auto& [id, l] : assets.locations) {
            l.ref_empty.reset();
            last_loc = id;
        }
        store.save_assets(assets);
    }
    REQUIRE(!last_loc.empty());
    const auto before = store.load_assets();
    const std::size_t want = before.characters.size() * 3 + before.locations.size();

    std::atomic<int> painted{0};
    // **声明在 painted 之后**：抛了的话先把队列收干净，painted 才没（见 QueueDrain）。
    const QueueDrain drain{path};
    http::set_ref_renderer([&](const config::Settings&, const models::ProjectStore&) {
        http::RefBackend b;
        b.lanes = 1;
        b.render = [&painted](const models::Shot&, const stages::PromptBundle&,
                              const models::TierSpec&, const fs::path& dest,
                              pipeline::CancelToken&, const infer::StepCallback&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            ++painted;
            std::ofstream(dest, std::ios::binary) << "png";
        };
        return b;
    });
    REQUIRE(http::post_references_generate_all({{"project", path}}).status == 202);
    // 排着的时候，最后一个场景那一格人自己传了一张。
    {
        fs::create_directories(root / "refs");
        std::ofstream(root / "refs" / paths::from_utf8("人传的.jpg"), std::ios::binary) << "jpg";
        auto assets = store.load_assets();
        assets.locations.at(last_loc).ref_empty = "refs/人传的.jpg";
        store.save_assets(assets);
    }
    for (int i = 0; i < 500; ++i) {
        if (!http::get_references_queue(path).body.value("active", false)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(painted.load() == static_cast<int>(want) - 1);
    CHECK(store.load_assets().locations.at(last_loc).ref_empty.value_or("") == "refs/人传的.jpg");
    CHECK(fs::exists(root / "refs" / paths::from_utf8("人传的.jpg")));
    http::set_ref_renderer({});
    fs::remove_all(root);
}

TEST_CASE("一键出图：单独停正在画的那一张，整批接着画") {
    // 原来停一张照「第一件砸了就别接着派」处理，整批都停了——「停一件」和
    // 「停一整批」只剩一个。
    const fs::path root = fresh_copy("单独停一张");
    const models::ProjectStore store{root};
    const std::string path = paths::to_utf8(root);
    {
        auto assets = store.load_assets();
        for (auto& [id, c] : assets.characters) {
            c.ref_front.reset();
            c.ref_three_quarter.reset();
            c.ref_back.reset();
        }
        for (auto& [id, l] : assets.locations) l.ref_empty.reset();
        store.save_assets(assets);
    }
    const auto before = store.load_assets();
    const int want = static_cast<int>(before.characters.size() * 3 + before.locations.size());
    REQUIRE(want > 2);

    std::atomic<int> painted{0};
    // **声明在 painted 之后**：抛了的话先把队列收干净，painted 才没（见 QueueDrain）。
    const QueueDrain drain{path};
    http::set_ref_renderer([&](const config::Settings&, const models::ProjectStore&) {
        http::RefBackend b;
        b.lanes = 1;
        b.render = [&painted](const models::Shot&, const stages::PromptBundle&,
                              const models::TierSpec&, const fs::path& dest,
                              pipeline::CancelToken& tok, const infer::StepCallback&) {
            for (int i = 0; i < 30; ++i) {
                if (tok.cancelled()) throw std::runtime_error(util::kStoppedOne);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ++painted;
            std::ofstream(dest, std::ios::binary) << "png";
        };
        return b;
    });
    REQUIRE(http::post_references_generate_all({{"project", path}}).status == 202);
    // 等第一张开画，停它。
    bool stopped = false;
    for (int i = 0; i < 200 && !stopped; ++i) {
        const auto board = pipeline::task_board(path);
        for (const auto& r : board.value("running", json::array())) {
            stopped = pipeline::cancel_task(r.at("id").get<std::uint64_t>());
            if (stopped) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(stopped);
    for (int i = 0; i < 500; ++i) {
        if (!http::get_references_queue(path).body.value("active", false)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto done = http::get_references_queue(path);
    CHECK(done.body.value("failed", std::size_t{1}) == 0);
    CHECK(painted.load() == want - 1);   // 停掉的那一张之外全画了
    http::set_ref_renderer({});
    fs::remove_all(root);
}
