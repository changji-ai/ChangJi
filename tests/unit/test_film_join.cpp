// 成片：把出了片的章按章序接成一部完整的电影，一个文件。
//
// 2026-09-18 之前这儿是 test_series_cut.cpp，钉的是 plan_series_cut——
// 「接成一条再按每集时长切成几集」那套纯算法。用户当天把产品定位改成电影
// 制作平台，切段整条拔掉，那个函数和它的四条用例一起没了。
//
// join_film 里已经没有纯算法了，剩下的全是文件和 ffmpeg。ffmpeg 那一层
// 换成桩（media::Runner，和 test_gates / test_finish 一个路子）就跑得起来，
// 于是这儿钉的是：
//   · 成片落在哪个目录；
//   · 什么时候**还不能**合成——判词是给人看的，说错了人就去错地方找原因；
//   · GET /api/film 读老项目那份 cut.json 时说的话对不对（那一页上什么都
//     没跑，全是读盘，照样测得死）；
//   · 合成走完一整趟之后清单里写下了什么——尤其是 **ffprobe 没给出时长**
//     那一支，它在真机上是"ffprobe 不在那个路径上"和"容器里没写时长"。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/film.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/film_join.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;

namespace {

namespace fs = std::filesystem;

fs::path fresh_project(const std::string& tag) {
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_成片_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    models::ProjectStore::create(root, "chengpian", "成片测试");
    return root;
}

/// 在 output/ 下摆一个这一章的成片。内容无所谓——`films_of` 只按名字认。
void put_film(const models::ProjectStore& store, const std::string& stem) {
    std::error_code ec;
    fs::create_directories(store.paths().output(), ec);
    std::ofstream f(store.paths().output() / paths::from_utf8(stem + ".mp4"),
                    std::ios::binary | std::ios::trunc);
    f << "not really a movie";
}

/// 一条挂着章的记录（`Episode` / `episode_id` 是历史留下的名字，一条就是一章）。
models::Episode chapter_episode(const std::string& episode_id,
                                const std::string& chapter_id) {
    models::Episode e;
    e.episode_id = episode_id;
    e.title = chapter_id;
    e.chapter_refs.push_back(chapter_id);
    return e;
}

models::Story story_with(const std::vector<std::string>& chapter_ids) {
    models::Story s;
    for (const auto& id : chapter_ids) {
        models::Chapter c;
        c.chapter_id = id;
        c.title = id;
        s.chapters.push_back(c);
    }
    return s;
}

}  // namespace

TEST_CASE("成片落在 output/final") {
    // 这个目录每次合成都 remove_all 重写，所以它是什么**必须钉住**：
    // 指歪一格删掉的就是别人的东西。
    const fs::path root = fresh_project("目录");
    const models::ProjectStore store(root);
    CHECK(pipeline::final_dir(store.paths()) == store.paths().output() / "final");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("还不能合成的时候，说得出是为什么") {
    const fs::path root = fresh_project("判词");
    models::ProjectStore store(root);

    SUBCASE("一章都没有") {
        CHECK(pipeline::film_join_blocker(store) == "一章都没有，没什么可接");
    }

    SUBCASE("有章，但一章都还没出片") {
        models::Project p = store.load_project();
        p.episodes.push_back(chapter_episode("ep01", "ch01"));
        p.episodes.push_back(chapter_episode("ep02", "ch02"));
        store.save_project(p);
        store.save_story(story_with({"ch01", "ch02"}));
        CHECK(pipeline::film_join_blocker(store) ==
              "还没有一章出片。这一章出了片就能合成");
    }

    SUBCASE("有一章出了片就能合成：不等所有章") {
        // 用户 2026-09-18：「这一章有片就可以成片了」。**没片的章跳过，
        // 不是停**——挡在这儿的话，一部电影要全部拍完才看得到一版粗剪。
        models::Project p = store.load_project();
        p.episodes.push_back(chapter_episode("ep01", "ch01"));
        p.episodes.push_back(chapter_episode("ep02", "ch02"));
        store.save_project(p);
        store.save_story(story_with({"ch01", "ch02"}));
        put_film(store, "ep02");  // 后面那一章先出片，照样能合成
        CHECK(pipeline::film_join_blocker(store).empty());
    }

    SUBCASE("老项目切过的章也算出了片") {
        // 2026-09-18 之前一章会切成 ep01_01 / ep01_02。今天不再产出这种
        // 名字，但老项目磁盘上还躺着——认不出来的话，一部拍完的老电影会
        // 被报成「还没有一章出片」。见 media::episode_of_output。
        models::Project p = store.load_project();
        p.episodes.push_back(chapter_episode("ep01", "ch01"));
        store.save_project(p);
        store.save_story(story_with({"ch01"}));
        put_film(store, "ep01_02");
        CHECK(pipeline::film_join_blocker(store).empty());
    }

    SUBCASE("不挂章的那条不算数：预告片、手动加的那一条不该顶替正片") {
        models::Episode loose;
        loose.episode_id = "trailer";
        models::Project p = store.load_project();
        p.episodes.push_back(loose);
        store.save_project(p);
        put_film(store, "trailer");
        CHECK(pipeline::film_join_blocker(store) == "一章都没有，没什么可接");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}


// ---------------------------------------------------------------------------
// GET /api/film：老项目切出来的那几段
// ---------------------------------------------------------------------------

namespace {

/// 往 output/final 里写一份清单。
void put_manifest(const models::ProjectStore& store, const std::string& name,
                  const nlohmann::json& body) {
    const fs::path dir = pipeline::final_dir(store.paths());
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / paths::from_utf8(name), std::ios::binary | std::ios::trunc);
    f << body.dump(2);
}

/// 往 output/final 里摆一个成片文件。
void put_final_mp4(const models::ProjectStore& store, const std::string& name) {
    const fs::path dir = pipeline::final_dir(store.paths());
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / paths::from_utf8(name), std::ios::binary | std::ios::trunc);
    f << "not really a movie";
}

}  // namespace

TEST_CASE("成片页：读不到清单就说不知道，不许报 0") {
    // 「0 秒 · 0 镜」是在信誓旦旦地报一个没读到的值。人看见 0 会以为成片
    // 合坏了，而盘上那个文件好好的——只是没人量过它。
    const fs::path root = fresh_project("没清单");
    const models::ProjectStore store(root);
    put_final_mp4(store, "成片.mp4");

    const auto r = http::get_film(paths::to_utf8(root));
    CHECK(r.status == 200);
    CHECK(r.body.at("total_s").is_null());
    CHECK(r.body.at("shots").is_null());
    CHECK(r.body.at("name").is_null());  // 不知道哪个是成片，别拿 files[0] 冒充
    CHECK(r.body.at("files").size() == 1);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("成片页：老项目切出来的那几段，一段都不能少") {
    // 2026-09-17 切过 6 段的老项目打开成片页——盘上是 cut.json 加六个
    // 第0N集.mp4。只读 film.json 的那一版：六个文件都列得出来，而 total_s /
    // shots 读不到回落成 0，页面拿 files[0] 显示「第01集 · 0 秒 · 0 镜」，
    // 另外五段在页面上彻底消失。
    const fs::path root = fresh_project("老清单");
    const models::ProjectStore store(root);
    put_final_mp4(store, "第01集.mp4");
    put_final_mp4(store, "第02集.mp4");
    put_manifest(store, "cut.json",
                 {{"per_episode_s", 180.0},
                  {"total_s", 300.0},
                  {"chapters", {"ch01", "ch02"}},
                  {"skipped", {"ch03"}},
                  {"parts",
                   {{{"name", "第01集.mp4"}, {"duration_s", 180.0}, {"shots", 7}},
                    {{"name", "第02集.mp4"}, {"duration_s", 120.0}, {"shots", 5}}}}});

    const auto r = http::get_film(paths::to_utf8(root));
    CHECK(r.status == 200);
    // **页面得说得出这是上一版切出来的几段**，不是这一版合成的那部电影。
    CHECK(r.body.at("legacy_cut").get<bool>());
    CHECK(r.body.at("name").is_null());
    CHECK(r.body.at("total_s").get<double>() == doctest::Approx(300.0));
    CHECK(r.body.at("shots").get<int>() == 12);  // 老清单只有每一段的，加起来
    CHECK(r.body.at("chapters").size() == 2);
    CHECK(r.body.at("skipped").size() == 1);
    REQUIRE(r.body.at("files").size() == 2);  // 六段里只剩一段的那个 bug
    CHECK(r.body.at("files")[0].at("name").get<std::string>() == "第01集.mp4");
    // 每一段自己多长，挂回它自己那一条——总长是这几段加起来的，按在第一段
    // 头上就又成了"报一个不是它的数"。
    CHECK(r.body.at("files")[0].at("duration_s").get<double>() == doctest::Approx(180.0));
    CHECK(r.body.at("files")[1].at("shots").get<int>() == 5);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("成片页：新清单在的时候读的是它，不是老的") {
    // 老项目重新合成过一次：film.json 和 cut.json 会同时在（remove_all 之后
    // cut.json 其实没了，但盘上的事说不准）。**新的说了算**，而且不能再报
    // legacy_cut——那句「这是上一版切出来的」会把人劝去重合成一次已经合好的。
    const fs::path root = fresh_project("新清单");
    const models::ProjectStore store(root);
    put_final_mp4(store, "成片.mp4");
    put_manifest(store, "cut.json", {{"total_s", 300.0}});
    put_manifest(store, "film.json",
                 {{"name", "成片.mp4"},
                  {"total_s", 612.5},
                  {"shots", 34},
                  {"chapters", {"ch01"}},
                  {"skipped", nlohmann::json::array()}});

    const auto r = http::get_film(paths::to_utf8(root));
    CHECK_FALSE(r.body.at("legacy_cut").get<bool>());
    CHECK(r.body.at("name").get<std::string>() == "成片.mp4");
    CHECK(r.body.at("total_s").get<double>() == doctest::Approx(612.5));
    CHECK(r.body.at("shots").get<int>() == 34);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// 合成走完一整趟：清单里写下的"不知道"
// ---------------------------------------------------------------------------

namespace {

/// 假的 ffmpeg / ffprobe。
///
/// 拼接那一趟只做一件事：**把输出文件创建出来**（`concat_args` 的最后一个
/// 参数就是它）——`join_film` 随后要把它从 .work 挪进 final/，文件不在那一
/// 步就抛，整趟走不到量时长那儿。
/// ffprobe 那一趟照 `probe_out` 答；`probe_ok` 是 false 就当作"机器上根本
/// 没有 ffprobe"（`run_exe` 抛 FFmpegMissing）。
media::Runner fake_ff(std::string probe_out, bool probe_ok = true) {
    return [probe_out = std::move(probe_out), probe_ok](
               const std::string& exe, const std::vector<std::string>& args, double) {
        media::ProcResult r;
        r.launched = true;
        if (exe == "ffprobe") {
            r.launched = probe_ok;
            r.out = probe_out;
            return r;
        }
        std::ofstream f(paths::from_utf8(args.back()), std::ios::binary | std::ios::trunc);
        f << "joined";
        return r;
    };
}

/// 跑一趟合成。JobProgress 只能由 JobTable 交出来，所以借一张本地的表。
pipeline::FilmJoinReport join_with(const models::ProjectStore& store,
                                   const media::FFmpeg& ff) {
    pipeline::JobTable table;
    pipeline::FilmJoinReport report;
    const config::Settings settings;
    table.start(pipeline::JobKind::Run, "", [&](pipeline::JobProgress& p) {
        report = pipeline::join_film(store, settings, ff, p);
    });
    table.wait_idle();
    return report;
}

/// 摆好一个"有一章、那一章出了片"的项目。
models::ProjectStore ready_project(const std::string& tag) {
    const fs::path root = fresh_project(tag);
    models::ProjectStore store(root);
    models::Project p = store.load_project();
    p.episodes.push_back(chapter_episode("ep01", "ch01"));
    store.save_project(p);
    store.save_story(story_with({"ch01"}));
    put_film(store, "ep01");
    return store;
}

nlohmann::json read_manifest(const models::ProjectStore& store) {
    std::ifstream in(pipeline::final_dir(store.paths()) / "film.json", std::ios::binary);
    nlohmann::json m;
    in >> m;
    return m;
}

}  // namespace

TEST_CASE("合成成功而时长没量到：清单里是 null，不是 0") {
    // 这一支在真机上很常见：ffprobe 不在 `assembly.ffprobe_path` 指的地方，
    // 或者拼出来的容器里压根没写 duration。成片**是好的**，就躺在
    // output/final 里——只是没人量过它。
    //
    // 留 0 的话，0 会顺着 film.json 流进 GET /api/film（它认的是"是不是个
    // 数"），成片页照着说「成片 · 0 秒」：一次成功的合成被报成了一段空的成片。
    const std::string probe_none =
        R"({"format":{},"streams":[{"codec_type":"video"}]})";

    SUBCASE("ffprobe 跑了，但没给时长") {
        models::ProjectStore store = ready_project("没时长");
        const media::FFmpeg ff("ffmpeg", "ffprobe", fake_ff(probe_none));
        const auto report = join_with(store, ff);

        CHECK_FALSE(report.total_s.has_value());
        CHECK(read_manifest(store).at("total_s").is_null());

        const auto r = http::get_film(paths::to_utf8(store.root()));
        CHECK(r.body.at("total_s").is_null());
        // 名字和镜数是这一趟自己数出来的，照常有——"不知道"只该盖住量不到
        // 的那一栏，不是整份回包。
        CHECK(r.body.at("name").get<std::string>() == "成片.mp4");
        CHECK(r.body.at("shots").is_number_integer());
        CHECK_FALSE(r.body.at("legacy_cut").get<bool>());

        std::error_code ec;
        fs::remove_all(store.root(), ec);
    }

    SUBCASE("机器上根本没有 ffprobe") {
        // 抛了也**不能把整趟合成报成失败**：成片已经在盘上了，报失败的话
        // 用户会以为没出来，再合一遍——而再合一遍还是量不到。
        models::ProjectStore store = ready_project("没ffprobe");
        const media::FFmpeg ff("ffmpeg", "ffprobe", fake_ff("", false));
        const auto report = join_with(store, ff);

        CHECK_FALSE(report.total_s.has_value());
        CHECK(fs::is_regular_file(report.path));
        CHECK(read_manifest(store).at("total_s").is_null());

        std::error_code ec;
        fs::remove_all(store.root(), ec);
    }

    SUBCASE("量到了就照实写") {
        models::ProjectStore store = ready_project("量到了");
        const media::FFmpeg ff(
            "ffmpeg", "ffprobe",
            fake_ff(R"({"format":{"duration":"612.5"},"streams":[]})"));
        const auto report = join_with(store, ff);

        REQUIRE(report.total_s.has_value());
        CHECK(*report.total_s == doctest::Approx(612.5));
        const auto r = http::get_film(paths::to_utf8(store.root()));
        CHECK(r.body.at("total_s").get<double>() == doctest::Approx(612.5));

        std::error_code ec;
        fs::remove_all(store.root(), ec);
    }
}
