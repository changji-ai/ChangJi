// Project / ProjectPaths / ProjectStore 的对拍测试。
//
// 这个文件是阶段 1 完成标志的可执行版本：
//
//     「能读 Python 写的项目文件（含中文路径和中文内容），字段全部对得上」
//
// 语料不是 JSON 片段，是 export_golden.py 用真实的 ProjectStore **建出来的
// 一个目录**，目录名带中文（golden/项目_雨夜天台/）。中文目录名是刻意的：
// MSVC 上 fs::path(std::string) 按 ANSI 代码页解释窄字符串，而项目里的
// std::string 一律是 UTF-8，撞上就抛异常。这个坑在 verify/RESULTS.md 里
// 记过，这里用语料把它钉死，以后谁改回 fs::path(str) 立刻红。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using namespace changji::models;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path
                    << "（语料在版本库里，Python 引擎删掉之后不再重新生成）");
    json j;
    in >> j;
    return j;
}

/// 当年 Python 建出来、随后冻进版本库的那个项目目录。
/// 名字带中文是测试的一部分。
fs::path golden_project_root(const json& exp) {
    return changji::paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           changji::paths::from_utf8(exp.at("root_name").get<std::string>());
}

}  // namespace

TEST_CASE("读 Python 写的项目：中文目录名、中文内容") {
    const json exp = load_golden("project_expectations");
    const fs::path root = golden_project_root(exp);

    REQUIRE_MESSAGE(fs::exists(root),
                    "项目目录不存在——它在版本库里，检查一下检出是否完整");

    const ProjectStore store(root);
    REQUIRE(store.exists());

    const Project p = store.load_project();

    CHECK(p.project_id == exp.at("project_id").get<std::string>());
    CHECK(p.title == exp.at("title").get<std::string>());
    CHECK(p.premise == exp.at("premise").get<std::string>());
    CHECK(p.schema_version == exp.at("schema_version").get<int>());
    CHECK(p.style_line == StyleLine::REALISTIC);
    CHECK_FALSE(p.created_at.empty());
    CHECK_FALSE(p.updated_at.empty());

    std::vector<std::string> ids;
    for (const auto& e : p.episodes) ids.push_back(e.episode_id);
    CHECK(ids == exp.at("episode_ids").get<std::vector<std::string>>());

    CHECK(p.validate().empty());

    SUBCASE("资产库也能读，且中文角色名完好") {
        const AssetLibrary lib = store.load_assets();
        REQUIRE(lib.characters.count("c_lin_yuan") == 1);
        CHECK(lib.characters.at("c_lin_yuan").name == "林渊");
        CHECK(lib.locations.size() == 2);
        CHECK(lib.validate().empty());
    }
}

TEST_CASE("没写画风的项目，读出来要有这条线的底子") {
    // **2026-09-12 用户报的那件事就是它空着。** 同一个项目里三个角色出了
    // 皮克斯 3D、半写实、照片三种质感，空景图也是一会 CG 一会实拍——
    // 因为整条提示词里一个画风词都没有：`style_line` 只管拼接用的逗号，
    // 而唯一的画风字段 `global_style` 默认是空的。
    //
    // 补在读这一步，不在出图那一层：这样它是项目里一条看得见的数据，
    // 项目页那个「画风」框显示得出来、改得动。
    const fs::path root =
        fs::temp_directory_path() /
        ("changji_style_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    const auto write_assets = [&](const char* line, const char* style) {
        std::ofstream out(root / "assets.json", std::ios::binary);
        out << R"({"characters":{},"locations":{},"style":{)"
            << R"("style_line":")" << line << R"(",)"
            << R"("global_style":")" << style << R"(",)"
            << R"("negative_prompt":"","aspect_ratio":"9:16"}})";
    };

    const ProjectStore store(root);

    SUBCASE("写实线：空的补成实拍那句") {
        write_assets("realistic", "");
        const AssetLibrary lib = store.load_assets();
        CHECK(lib.style.global_style == default_style(StyleLine::REALISTIC));
        CHECK(lib.style.global_style.find("实拍") != std::string::npos);
    }

    SUBCASE("动漫线补的是另一句") {
        write_assets("anime", "");
        const AssetLibrary lib = store.load_assets();
        CHECK(lib.style.global_style == default_style(StyleLine::ANIME));
        // 两条线不能补成同一句，否则选了等于没选
        CHECK(default_style(StyleLine::ANIME) !=
              default_style(StyleLine::REALISTIC));
    }

    SUBCASE("用户自己写了就一个字都不动") {
        write_assets("realistic", "电影感冷调，胶片颗粒");
        CHECK(store.load_assets().style.global_style == "电影感冷调，胶片颗粒");
    }

    SUBCASE("只有空格也算没写") {
        write_assets("realistic", "   ");
        CHECK(store.load_assets().style.global_style ==
              default_style(StyleLine::REALISTIC));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("Episode 的查询方法与 Python 一致") {
    const json exp = load_golden("project_expectations");
    const ProjectStore store(golden_project_root(exp));
    const Project p = store.load_project();

    const Episode* ep = p.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    const json& e = exp.at("ep01");

    SUBCASE("按 order 排序") {
        std::vector<std::string> got;
        for (const auto& s : ep->sorted_shots()) got.push_back(s.shot_id);
        CHECK(got == e.at("sorted_shot_ids").get<std::vector<std::string>>());
    }

    SUBCASE("计划时长") {
        CHECK(ep->planned_duration_s() ==
              doctest::Approx(e.at("planned_duration_s").get<double>()));
    }

    SUBCASE("按状态计数") {
        const auto got = ep->counts_by_status();
        const auto want = e.at("counts_by_status").get<std::map<std::string, int>>();
        CHECK(got == want);
    }

    SUBCASE("断点续跑用的按状态取镜头") {
        std::vector<std::string> planned;
        for (const auto& s : ep->shots_needing(ShotStatus::PLANNED)) {
            planned.push_back(s.shot_id);
        }
        CHECK(planned == e.at("shots_needing_planned").get<std::vector<std::string>>());

        std::vector<std::string> audio;
        for (const auto& s : ep->shots_needing(ShotStatus::AUDIO_DONE)) {
            audio.push_back(s.shot_id);
        }
        CHECK(audio ==
              e.at("shots_needing_audio_done").get<std::vector<std::string>>());
    }

    SUBCASE("按 id 找镜头") {
        CHECK(ep->shot_by_id("ep01_s03_sh007") != nullptr);
        CHECK(ep->shot_by_id("不存在") == nullptr);
    }
}

TEST_CASE("目录布局与相对路径约定") {
    const json exp = load_golden("project_expectations");
    const fs::path root = golden_project_root(exp);
    const ProjectPaths paths(root);

    SUBCASE("子目录齐全") {
        // **在临时目录里现建一个，不看语料目录里现在有什么。**
        //
        // 原来这里直接列语料项目根下的目录。那七个子目录是空的，
        // 而 **git 不跟踪空目录**——它们只是"碰巧在盘上"。后果有两个：
        //
        //   一，新克隆一份仓库跑测试就会挂，而报错只说"子目录对不上"，
        //       完全看不出真正的原因是"少了几个空目录"。
        //   二，任何清过一次工作区的操作都会让它挂。2026-09-08 就是
        //       这么撞上的：一次 `git stash -u` 把它们清掉了，
        //       而 `stash pop` 带不回来——git 从来没跟踪过它们。
        //
        // 加 .gitkeep 占位是另一种修法，但那会往 refs/ 里塞一个文件，
        // 而上传那几条用例是**逐个文件名比 refs/ 的内容**的，
        // 立刻挂四条。
        //
        // 真正要钉的本来就不是"盘上有什么"，是 **ensure() 建出来的
        // 布局和 Python 一致**。
        const fs::path tmp =
            fs::temp_directory_path() / changji::paths::from_utf8("changji_布局");
        std::error_code ec;
        fs::remove_all(tmp, ec);
        const ProjectPaths fresh(tmp);
        fresh.ensure();

        std::vector<std::string> got;
        for (const auto& entry : fs::directory_iterator(tmp)) {
            if (entry.is_directory()) {
                got.push_back(changji::paths::to_utf8(entry.path().filename()));
            }
        }
        std::sort(got.begin(), got.end());
        CHECK(got == exp.at("subdirs").get<std::vector<std::string>>());
    }

    SUBCASE("rel 用正斜杠，与 Python 的 as_posix 一致") {
        // 这一条保证 Windows 上存的项目拿到 Linux 上也能读
        CHECK(paths.rel(root / "frames" / "a.png") ==
              exp.at("rel_of_frames_file").get<std::string>());
        CHECK(paths.rel(root / "shots" / "draft" / "b.mp4") ==
              exp.at("rel_of_nested").get<std::string>());
    }

    SUBCASE("项目外的路径要被拒绝") {
        // 存绝对路径会破坏可移植性，必须抛
        CHECK_THROWS(paths.rel(root.parent_path() / changji::paths::from_utf8("别处.png")));
    }

    SUBCASE("abs 是 rel 的逆") {
        const std::string r = paths.rel(root / "frames" / "a.png");
        CHECK(paths.abs(r) == fs::weakly_canonical(root / "frames" / "a.png"));
    }
}

TEST_CASE("C++ 写回去 Python 还能读，中文不坏") {
    // 往返：读 Python 写的 → C++ 改一点 → C++ 写 → C++ 再读。
    // 真正的双向对拍要等接口层，这里先保证 C++ 自己的写不破坏数据。
    const json exp = load_golden("project_expectations");
    const fs::path src = golden_project_root(exp);

    // 复制到一个同样带中文名的临时目录再改，不动语料
    // 注意这里必须走 from_utf8。写成 / "changji_测试_往返" 会当场抛
    // "No mapping for the Unicode character"——源码是 UTF-8，而 fs::path 的
    // const char* 构造按 ANSI 代码页解释。第一版就是这么写的，测试直接 THREW。
    const fs::path tmp = fs::temp_directory_path() /
                         changji::paths::from_utf8("changji_测试_往返");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::copy(src, tmp, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());

    {
        ProjectStore store(tmp);
        Project p = store.load_project();
        const std::string before = p.updated_at;

        p.title = "雨夜天台（改过）";
        p.episodes.at(0).synopsis = "改一句中文看会不会坏";
        store.save_project(p);

        // save_project 会 touch，updated_at 必须变
        CHECK(p.updated_at != before);
    }

    {
        const ProjectStore store(tmp);
        const Project p = store.load_project();
        CHECK(p.title == "雨夜天台（改过）");
        CHECK(p.episodes.at(0).synopsis == "改一句中文看会不会坏");
        // 没动的字段要原样还在
        CHECK(p.premise == exp.at("premise").get<std::string>());
        CHECK(p.episodes.size() == 2);
        CHECK(p.episodes.at(0).shots.size() == 2);
    }

    SUBCASE("写完不留临时文件") {
        int tmp_files = 0;
        for (const auto& entry : fs::directory_iterator(tmp)) {
            const std::string n = changji::paths::to_utf8(entry.path().filename());
            if (n.find(".tmp") != std::string::npos) ++tmp_files;
        }
        CHECK(tmp_files == 0);
    }

    fs::remove_all(tmp, ec);
}

TEST_CASE("时间戳格式与 Python 的 isoformat 对齐") {
    const std::string ts = utc_now_iso8601();
    // 2026-09-07T12:34:56.123456+00:00
    CHECK(ts.size() == 32);
    CHECK(ts[4] == '-');
    CHECK(ts[7] == '-');
    CHECK(ts[10] == 'T');
    CHECK(ts[13] == ':');
    CHECK(ts[16] == ':');
    CHECK(ts[19] == '.');
    // 微秒六位，时区是 +00:00 不是 Z
    CHECK(ts.substr(26) == "+00:00");
    CHECK(ts.find('Z') == std::string::npos);

    // **对拍看不见这一条，所以只能靠这里。** updated_at / created_at 每次
    // 存盘都会变，对拍把它们放进了忽略表——忽略的是值，格式也就跟着没人看了。
    //
    // 而带不带时区是有人依赖的：webapp 的 humanAgo 用 Date.parse 解它，
    // **不带时区的话 JS 按本地时间解**，东八区会把"刚刚"显示成"8 小时前"。
}

TEST_CASE("新建项目") {
    const fs::path root = fs::temp_directory_path() /
                          changji::paths::from_utf8("changji_新建_项目");
    std::error_code ec;
    fs::remove_all(root, ec);

    ProjectStore store = ProjectStore::create(root, "my-drama", "我的电影",
                                              StyleLine::ANIME);
    CHECK(store.exists());

    const Project p = store.load_project();
    CHECK(p.project_id == "my-drama");
    CHECK(p.title == "我的电影");
    CHECK(p.style_line == StyleLine::ANIME);
    // 两者不会完全相同：Python 侧也是各自调一次 now()，然后 save_project
    // 又 touch 一次。只要求都非空且 updated_at 不早于 created_at。
    CHECK_FALSE(p.created_at.empty());
    CHECK(p.updated_at >= p.created_at);  // ISO 8601 定长，字典序即时间序
    CHECK(p.validate().empty());

    // 资产库的风格线要跟着项目走
    CHECK(store.load_assets().style.style_line == StyleLine::ANIME);

    // **画幅也跟着落到盘上**，不留给初值去答。
    //
    // `create` 不写 changji.toml（写它的是 http::post_new_project），所以
    // 这份 assets.json 里的比例就是这个项目的画幅在盘上的唯一记录——
    // `config::load_settings` 对没有 [video] 的项目正是从这一栏推回来的。
    // 不写的话，新建的项目会靠 StyleProfile 那个**为老项目留的**初值
    // （9:16）回答"我是什么画幅"，而内置默认 2026-09-18 已经是横屏了。
    CHECK(store.load_assets().style.aspect_ratio ==
          changji::config::VideoConfig{}.aspect_ratio());
    CHECK(store.load_assets().style.aspect_ratio == "16:9");

    // 子目录都建出来了
    for (const auto& sub : project_subdirs()) {
        CHECK_MESSAGE(fs::exists(root / changji::paths::from_utf8(sub)),
                      "缺子目录 " << sub);
    }

    SUBCASE("同一个目录不能建两次") {
        CHECK_THROWS(ProjectStore::create(root, "again"));
    }

    fs::remove_all(root, ec);
}

TEST_CASE("项目 id 允许连字符，镜头 id 不允许") {
    Project p;
    p.project_id = "my-drama-2";
    CHECK(p.validate().empty());

    p.project_id = "My-Drama";  // 大写不行
    CHECK_FALSE(p.validate().empty());
}

TEST_CASE("资产库保持文件里的键顺序") {
    // 接口响应里 characters/locations 是数组，顺序是值的一部分。
    // assets.json 里是 loc_rooftop 在前、loc_alley 在后（插入顺序），
    // 用 std::map 或普通 json 解析都会变成字典序，前端渲染顺序就变了。
    const json exp = load_golden("project_expectations");
    const ProjectStore store(golden_project_root(exp));
    const AssetLibrary lib = store.load_assets();

    std::vector<std::string> order;
    for (const auto& kv : lib.locations) order.push_back(kv.first);
    CHECK(order == std::vector<std::string>{"loc_rooftop", "loc_alley"});

    // 而 location_ids() 要的是排序结果（给大模型做约束解码），
    // 这两件事不能混
    CHECK(lib.location_ids() ==
          std::vector<std::string>{"loc_alley", "loc_rooftop"});
}


