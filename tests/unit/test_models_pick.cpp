// `[models.pick]`：这部电影要哪一档模型。
//
// 用户 2026-09-15 定的两条：**项目优先**（项目里写了就听项目的，全局那份
// 当默认值），以及缺档时**先自动下、下不了才把那台那一格灰掉**。这个文件
// 只钉第一条——配置层。
//
// 为什么要分出这一节：上面那些 `image` / `video` / `tts` 记的是**文件名**，
// 那是机器的属性（每台目录和文件名都不一样），只能放全局。而"要哪一档"是
// **这部电影**的属性，机器无关——档位 id 来自内置目录，每台都认得，各自去自己的
// 模型目录里找对应的文件。所以它能跟着项目目录走。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "config/model_patch.hpp"
#include "config/settings.hpp"
#include "setup/catalog.hpp"
#include "util/paths.hpp"

#include "scoped_env.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 写一份项目配置，回它的目录。
fs::path write_project(const std::string& tag, const std::string& body) {
    const auto dir = fs::temp_directory_path() / "changji_pick" / tag;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::ofstream(dir / "changji.toml") << body;
    return dir;
}

/// 往隔离出来的那份全局配置里写。
void write_global(const changji::test::ScopedUserConfigDir& iso,
                  const std::string& body) {
    std::error_code ec;
    const auto p = config::user_config_path();
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << body;
    (void)iso;
}

}  // namespace

TEST_CASE("项目里挑的那一档盖过全局") {
    changji::test::ScopedUserConfigDir iso("pick_wins");
    write_global(iso, "[models.pick]\nimage = \"全局挑的\"\n");
    const auto proj = write_project("wins", "[models.pick]\nimage = \"项目挑的\"\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.at("image") == "项目挑的");
}

TEST_CASE("按键盖，不整份替换") {
    // ⚠️ **这一条是要害。** 项目多半只写了一两组；整份替换的话，项目里写
    // 一个 image 就把全局挑好的 llm / tts / video 全清空——而那几组清空之后
    // 走的是"从文件名反推"那条老路，表面上还能跑，直到某一组的文件名恰好
    // 不在目录里。
    changji::test::ScopedUserConfigDir iso("pick_merge");
    write_global(iso,
                 "[models.pick]\nllm = \"别动我\"\ntts = \"我也是\"\n");
    const auto proj =
        write_project("merge", "[models.pick]\nimage = \"项目只改了这一组\"\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.at("image") == "项目只改了这一组");
    CHECK(s.models.pick.at("llm") == "别动我");
    CHECK(s.models.pick.at("tts") == "我也是");
    CHECK(s.models.pick.size() == 3);
}

TEST_CASE("不写这一节就什么都不动") {
    // 老项目一个字都不用改：没有 [models.pick] 时，"选了哪一档"照旧从文件名
    // 反推（那部分在 setup_api 的 current_option 里）。
    changji::test::ScopedUserConfigDir iso("pick_absent");
    write_global(iso, "[models.pick]\nllm = \"全局的\"\n");
    const auto proj = write_project("absent", "[video]\norientation = \"portrait\"\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.size() == 1);
    CHECK(s.models.pick.at("llm") == "全局的");
}

TEST_CASE("不是字符串的值跳过，别把整节弄坏") {
    changji::test::ScopedUserConfigDir iso("pick_badtype");
    const auto proj =
        write_project("badtype", "[models.pick]\nimage = \"好的那个\"\nllm = 42\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.at("image") == "好的那个");
    CHECK(s.models.pick.count("llm") == 0);
}

TEST_CASE("默认是空的——没挑过和挑了空串是两回事") {
    config::Settings s;
    CHECK(s.models.pick.empty());
}

// ---- 把档位落到配置上（setup::with_selections）----
//
// 这一段钉的是"派活时按 id 解析"那条路的地基：协调者只把档位 id 发出去，
// 每台照这个 id 去**自己的**模型目录里找文件。所以这个函数改写的必须是
// 文件名、而且**只有**文件名。

TEST_CASE("没挑过就一个字都不动") {
    config::Settings s;
    s.models.image = "本来就写着的.gguf";
    const auto out = setup::with_selections(s, {});
    CHECK(out.models.image == "本来就写着的.gguf");
}

TEST_CASE("认不出的档位 id 原样退回，不是清空") {
    // 老项目里写着一个早就删掉的 id。那时候配置里的文件名还是上次设置页
    // 写的、照旧能跑；界面上 pickProblem 会说这一档认不出来。
    // **清空的话表现是"出片说没配模型"，而配置文件里明明写着。**
    config::Settings s;
    s.models.image = "上次设置页写的.gguf";
    const auto out = setup::with_selections(s, {{"image", "这个档早没了"}});
    CHECK(out.models.image == "上次设置页写的.gguf");
}

TEST_CASE("挑了一档就按那一档的文件名改写 models") {
    // 从内置清单里现取一档，不写死 id：清单是会变的，写死的话这个用例
    // 迟早变成"档位 id 打错了也照样绿"。
    config::Settings probe_fields;
    const setup::Group* group = nullptr;
    const setup::Option* option = nullptr;
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.files.empty()) continue;
            bool named = false;
            for (const auto& f : o.files) {
                if (!f.role.empty() &&
                    config::models_field(probe_fields.models, f.role)) {
                    named = true;
                }
            }
            if (!named) continue;
            group = &g;
            option = &o;
            break;
        }
        if (group != nullptr) break;
    }
    REQUIRE(group != nullptr);
    REQUIRE(option != nullptr);

    config::Settings s;
    s.models.dir = "/我自己的模型目录";
    const auto out = setup::with_selections(s, {{group->key, option->id}});

    for (const auto& f : option->files) {
        if (f.role.empty()) continue;
        config::Settings probe = out;
        const std::string* got = config::models_field(probe.models, f.role);
        if (got == nullptr) continue;  // 不进配置的角色
        CHECK(*got == f.name);
    }
    // **模型目录是这台的，不许动。** 动了的话跨机派活时对面会去找派活
    // 那台的路径——而那个盘符在它上面根本不存在。
    REQUIRE(out.models.dir.has_value());
    CHECK(*out.models.dir == "/我自己的模型目录");
}

TEST_CASE("只落 models 那一节，别的旋钮不碰") {
    // 一个选项的 settings 里还带着 llm.model / tts.backend /
    // tiers.final_steps。那些是设置页写盘那一趟才该动的（它有自己的
    // "别冲掉用户手挑的"规矩）。这儿每跑一个任务覆一遍，照单全收的话，
    // 用户在设置页手改的大模型型号会被每一次出片悄悄改回去。
    const setup::Group* llm = nullptr;
    for (const auto& g : setup::catalog()) {
        if (g.key == "llm") llm = &g;
    }
    REQUIRE(llm != nullptr);
    const setup::Option* cloud = llm->find("zhipu-free");
    REQUIRE(cloud != nullptr);

    config::Settings s;
    s.llm.backend = "openai";
    s.llm.base_url = "https://我自己配的/v1";
    s.llm.model = "我手挑的型号";
    s.tiers.final_steps = 42;
    const auto out = setup::with_selections(s, {{"llm", cloud->id}});

    CHECK(out.llm.backend == "openai");
    CHECK(out.llm.base_url == "https://我自己配的/v1");
    CHECK(out.llm.model == "我手挑的型号");
    CHECK(out.tiers.final_steps == 42);
}
