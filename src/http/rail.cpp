#include "http/rail.hpp"

#include <algorithm>
#include <cmath>
#include <system_error>

#include "agent/tools.hpp"
#include "config/runtime.hpp"
#include "http/readonly.hpp"
#include "http/run.hpp"
#include "http/story_api.hpp"
#include "util/fs_time.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace changji::http {

namespace {

/// 有没有一章写了正文。**不是"有几章"**：大纲写完那一刻就有十二章了，而它们
/// 一个字正文都没有——那时候点进「故事」只有一张空表。
///
/// ⚠️ **`/api/story` 的回包是包了一层的**：
/// `{story: {...}, chapters: N, written: N, episodes: N, empty: bool}`，
/// 顶层那个 `chapters` 是**数目不是数组**。2026-09-21 就栽在这儿：故事明明
/// 有两章正文，图标条一格都不亮，而且**不报任何错**——`chapters` 取出来是
/// 个整数，`is_array()` 一判就 false，整条判据静悄悄变成"永远没有"。
///
/// `written` 是引擎自己数的"有正文的章数"（`Story::written_chapters()`），
/// 用它就不用在这儿再数一遍。
bool any_chapter_written(const json& story_resp) {
    if (!story_resp.is_object()) return false;
    return story_resp.value("written", 0) > 0;
}

/// 有没有一个角色或场景。有名字才算——空壳条目点进去是一片空白。
bool any_asset(const json& project) {
    const auto has = [&](const char* key) {
        if (!project.contains(key) || !project.at(key).is_array()) return false;
        for (const auto& it : project.at(key)) {
            if (!it.value("name", std::string()).empty()) return true;
        }
        return false;
    };
    return has("characters") || has("locations");
}

/// 有没有一章有剧本。
bool any_script(const json& project) {
    if (!project.contains("episodes") || !project.at("episodes").is_array()) return false;
    for (const auto& e : project.at("episodes")) {
        if (e.value("script_chars", 0) > 0) return true;
    }
    return false;
}

/// 有没有一镜能用。
///
/// 回包里的 `shots` 这个数**已经是"能用的镜头数"**（`readonly.cpp` 里那段
/// 注释：空壳镜头不算），所以这儿照用它，不去数数组长度。
bool any_shot(const json& project) {
    if (!project.contains("episodes") || !project.at("episodes").is_array()) return false;
    for (const auto& e : project.at("episodes")) {
        if (e.value("shots", 0) > 0) return true;
    }
    return false;
}

/// 有没有出片。
bool any_film(const json& outputs) {
    if (!outputs.is_object()) return false;
    for (const char* key : {"files", "previews"}) {
        if (outputs.contains(key) && outputs.at(key).is_array() &&
            !outputs.at(key).empty()) {
            return true;
        }
    }
    return false;
}

/// 一个文件的改动时间，秒。不在回 0。
std::int64_t stamp_of(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return 0;
    return static_cast<std::int64_t>(util::file_mtime_unix(p));
}

/// 一个目录里最新那个文件的改动时间。空目录回 0。
///
/// **不递归到底**：`output/` 底下还有 preview 子目录，`frames/` 是平的。
/// 一层就够，而且一层是有界的——一部片子两百多镜，递归扫是白花时间。
std::int64_t newest_in(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    std::int64_t best = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        best = std::max(best, static_cast<std::int64_t>(util::file_mtime_unix(e.path())));
    }
    return best;
}

}  // namespace

std::vector<RailSlot> rail_present(const json& project, const json& story,
                                   const json& outputs) {
    // **顺序就是流水线的顺序**，所以这条图标条本身就是进度。但它不是导航：
    // 不带对勾、不带"下一步在这儿"的点，人也不必按顺序点——下一步是代理的事。
    return {
        {"story", SAY("故事"), any_chapter_written(story), 0},
        {"assets", SAY("设定"), any_asset(project), 0},
        {"script", SAY("剧本"), any_script(project), 0},
        {"shots", SAY("镜头"), any_shot(project), 0},
        {"film", SAY("片子"), any_film(outputs), 0},
    };
}

std::map<std::string, std::int64_t> rail_stamps(const fs::path& root) {
    std::map<std::string, std::int64_t> out;
    out["story"] = stamp_of(root / "story.json");
    // 设定这一档要连参考图一起看：改了外观词和重画了一张图，人都要再看一眼。
    out["assets"] = std::max(stamp_of(root / "assets.json"), newest_in(root / "refs"));
    // 剧本和分镜都存在 project.json 里，所以它俩的指纹一起动——
    // **这是实话，不是偷懒**：改剧本本来就会让分镜过期（`shots_stale`）。
    out["script"] = stamp_of(root / "project.json");
    out["shots"] = std::max(stamp_of(root / "project.json"), newest_in(root / "frames"));
    out["film"] = newest_in(root / "output");
    return out;
}

ApiResult get_rail(const std::string& project) {
    json story;
    json proj;
    json outs;

    if (!project.empty()) {
        // **一个读砸了不该让整条算不出来。** 老项目没有 story.json 是常事，
        // 成片目录还没建也是常事——那几档按"还没有"算就对了。
        try {
            const auto r = get_project(project);
            if (r.status == 200) proj = r.body;
        } catch (const std::exception&) {}
        try {
            const auto r = get_story(project);
            if (r.status == 200) story = r.body;
        } catch (const std::exception&) {}
        try {
            const auto r = get_outputs(project);
            if (r.status == 200) outs = r.body;
        } catch (const std::exception&) {}
    }

    auto slots = rail_present(proj, story, outs);
    if (!project.empty()) {
        const auto stamps = rail_stamps(paths::from_utf8(project));
        for (auto& s : slots) {
            const auto it = stamps.find(s.key);
            if (it != stamps.end()) s.stamp = it->second;
        }
    }

    json arr = json::array();
    for (const auto& s : slots) {
        arr.push_back({{"key", s.key},
                       {"label", s.label},
                       {"present", s.present},
                       {"stamp", s.stamp}});
    }
    return {200, {{"slots", arr}, {"project", project}}};
}

namespace {

/// 这一格该问哪个工具。
std::string tool_for(const std::string& key) {
    if (key == "story") return "story_read";
    if (key == "assets") return "assets_read";
    if (key == "script") return "script_read";
    if (key == "shots") return "shots_read";
    if (key == "film") return "outputs_read";
    return {};
}

/// 挑第一章有东西的。`want` 是剧本还是镜头，判据不一样。
std::string first_episode_with(const json& project, const std::string& key) {
    if (!project.contains("episodes") || !project.at("episodes").is_array()) return {};
    for (const auto& e : project.at("episodes")) {
        const bool ok = key == "script" ? e.value("script_chars", 0) > 0
                                        : e.value("shots", 0) > 0;
        if (ok) return e.value("episode_id", std::string());
    }
    // 一章都没有东西时回第一章：点进去看到「还没有剧本」也是个答案，
    // 比一句"没挑到章"强。
    if (!project.at("episodes").empty()) {
        return project.at("episodes")[0].value("episode_id", std::string());
    }
    return {};
}

}  // namespace

ApiResult get_peek(const std::string& project, const std::string& key,
                   const std::string& episode_id) {
    const std::string tool = tool_for(key);
    if (tool.empty()) throw ApiError(400, SAYF("没有这一格：%1", key));
    if (project.empty()) {
        return {200, {{"text", SAY("还没有项目。")}, {"key", key}}};
    }

    std::string ep = episode_id;
    if (ep.empty() && (key == "script" || key == "shots")) {
        // ⚠️ **读不动项目要照实说，不能落进下面「一章都没有」那一句。**
        //
        // 原来这儿读砸了就吞掉，`ep` 空着往下走，于是 project.json 写坏了的
        // 项目点开「剧本」，看见的是灰色的「还没有剧本。先得有正文——说一句
        // 『写第一章』」——**坏的装成了空的**，还在教人去重写一遍（2026-09-23
        // 拿一个写坏的项目挨格点开时撞见）。
        std::string why;
        try {
            const auto r = get_project(project);
            if (r.status == 200) {
                ep = first_episode_with(r.body, key);
            } else {
                why = agent::readable_detail(r.body);
            }
        } catch (const ApiError& e) {
            why = e.detail().is_string() ? std::string(e.what())
                                         : agent::readable_detail(e.detail());
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!why.empty()) {
            return {200, {{"text", why}, {"key", key}, {"episode_id", ""}, {"failed", true}}};
        }
    }

    // 这两格是**按章看**的。一章都没有的时候别去问工具——它回的是一句
    // 接口参数的报错（「要说是哪一章（episode_id）。」），而这句话会**原样
    // 摆到界面上**：人点开「剧本」，看见的是一个英文参数名。
    // 2026-09-21 拿一个刚建的空项目挨个点开时撞见。
    //
    // 说的话要带上下一步：这一格空着的时候，人要的正是"那我该干什么"。
    if (ep.empty() && (key == "script" || key == "shots")) {
        std::string text = SAY("还没有分镜。有了剧本才好拆镜头。");
        if (key == "script") {
            // ⚠️ **「没有剧本」不等于「没有正文」。** 正文写完、还没理解过
            // 的项目也是一章剧本都没有，而对它说「先得有正文——说一句
            // 『写第一章』」是把人支回已经做完的那一步。2026-09-21 拿一个
            // 写完八章正文、还没理解的项目点开「剧本」撞见。
            //
            // 判据用和图标条同一个（`any_chapter_written`），别再数一遍。
            bool written = false;
            // 读砸了同上：照实说，别落成「先得有正文」。
            std::string why;
            try {
                const auto r = get_story(project);
                if (r.status == 200) written = any_chapter_written(r.body);
                else why = agent::readable_detail(r.body);
            } catch (const ApiError& e) {
                why = e.detail().is_string() ? std::string(e.what())
                                             : agent::readable_detail(e.detail());
            } catch (const std::exception& e) {
                why = e.what();
            }
            if (!why.empty()) {
                return {200, {{"text", why}, {"key", key}, {"episode_id", ""}, {"failed", true}}};
            }
            text = written
                       ? SAY("还没有剧本。正文有了——说一句「理解故事」就往下走。")
                       : SAY("还没有剧本。先得有正文——说一句「写第一章」。");
        }
        return {200, {{"text", text}, {"key", key}, {"episode_id", ""}}};
    }

    agent::ToolContext ctx;
    ctx.project = project;
    ctx.settings = config::runtime().snapshot();

    json args = json::object();
    if (!ep.empty()) args["episode_id"] = ep;

    // ⚠️ **这段话是摆给人看的，不是给模型的。** 同一个 `*_read` 工具，
    // 两个读者：模型那一遍要中文原话，这一遍要人自己的语言。所以带上
    // `Audience::human()`（见 `util/say.hpp`）。不带的话，整条图标条和
    // 格子名都翻好了，点开一格却是一句中文——2026-09-22 在德语和阿拉伯语
    // 下实地撞见的就是这个。
    const std::string text = agent::run_tool(ctx, tool, args.dump(), i18n::Audience::human());
    // **没读成要说出来**（`failed`，2026-09-23）：那一格摆的是一段话，读成了是
    // 摘要、没读成是一句报错，两者在界面上原来长得一样。判据是工具自己记的
    // 那一笔（`ToolContext::failed`），不是去认那句话。
    return {200, {{"text", text},
                  {"key", key},
                  {"episode_id", ep},
                  {"failed", ctx.failed}}};
}

}  // namespace changji::http
