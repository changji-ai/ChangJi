#include "pipeline/film_join.hpp"

#include <algorithm>
#include <fstream>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "media/assemble.hpp"
#include "media/product_tags.hpp"
#include "models/story.hpp"
#include "util/human_time.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "telemetry/telemetry.hpp"

namespace changji::pipeline {

namespace fs = std::filesystem;
using json = nlohmann::json;
using models::Episode;
using models::Project;
using models::ProjectStore;

fs::path final_dir(const models::ProjectPaths& paths) { return paths.output() / "final"; }

namespace {

/// 这一章的成片文件，按名字排（切过的章是 ep01_01 这种老命名——见
/// media::episode_of_output）。
std::vector<fs::path> films_of(const fs::path& dir, const std::string& episode_id) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec)) continue;
        if (paths::to_utf8(f.path().extension()) != ".mp4") continue;
        const auto owner = media::episode_of_output(paths::to_utf8(f.path().stem()));
        if (owner && *owner == episode_id) out.push_back(f.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 挂上了章的那几条（`chapter_refs` 非空），按故事里章的先后排。
std::vector<const Episode*> chapter_episodes(const Project& project,
                                             const models::Story& story) {
    std::map<std::string, int> order;
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        order[story.chapters[i].chapter_id] = static_cast<int>(i);
    }
    std::vector<const Episode*> eps;
    for (const auto& e : project.episodes) {
        if (!e.chapter_refs.empty()) eps.push_back(&e);
    }
    const auto rank = [&](const Episode* e) {
        const auto it = order.find(e->chapter_refs.front());
        return it == order.end() ? (1 << 20) : it->second;
    };
    std::stable_sort(eps.begin(), eps.end(),
                     [&](const Episode* a, const Episode* b) { return rank(a) < rank(b); });
    return eps;
}

models::Story story_or_empty(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception&) {
        return models::Story{};
    }
}

/// 各章成片的规格：尺寸 + 帧率（取到 0.01）。
struct FilmSpec {
    int w = 0;
    int h = 0;
    long fps100 = 0;
    bool operator<(const FilmSpec& o) const {
        return std::tie(w, h, fps100) < std::tie(o.w, o.h, o.fps100);
    }
    bool operator==(const FilmSpec& o) const {
        return w == o.w && h == o.h && fps100 == o.fps100;
    }
};

/// 规格不齐的那几章先重编成多数那一种（见 join_film 里那段）。量不到的就当齐了——
/// 为了一次量不到把能拼的拼不成，比花一截强不到哪儿去。
std::vector<fs::path> conform_films(const std::vector<fs::path>& films,
                                    const config::Settings& settings,
                                    const media::FFmpeg& ff, const fs::path& work,
                                    JobProgress& progress) {
    std::vector<std::optional<media::MediaInfo>> infos;
    std::map<FilmSpec, int> votes;
    for (const auto& f : films) {
        try {
            const media::MediaInfo m = ff.probe(f);
            if (m.width > 0 && m.height > 0) {
                infos.push_back(m);
                ++votes[FilmSpec{m.width, m.height, std::lround(m.fps * 100.0)}];
                continue;
            }
        } catch (const std::exception&) {
        }
        infos.push_back(std::nullopt);
    }
    if (votes.size() <= 1) return films;
    FilmSpec target;
    int best = -1;
    for (const auto& [spec, n] : votes) {
        if (n > best) {
            best = n;
            target = spec;
        }
    }
    config::AssemblyConfig cfg = settings.assembly;
    if (target.fps100 > 0) cfg.fps = static_cast<int>(std::lround(target.fps100 / 100.0));
    std::vector<fs::path> out = films;
    for (std::size_t i = 0; i < films.size(); ++i) {
        const auto& m = infos[i];
        if (!m || FilmSpec{m->width, m->height, std::lround(m->fps * 100.0)} == target) continue;
        progress.set_message(SAYF("第 %1 段的尺寸、帧率和别的不一样，先统一一下",
                                  std::to_string(i + 1)));
        const fs::path fixed = work / ("conform_" + std::to_string(i) + ".mp4");
        media::NormalizeOptions opt;
        opt.keep_audio = true;
        opt.source_has_audio = m->has_audio;
        ff.run(media::normalize_args(films[i], target.w, target.h, cfg, fixed, opt));
        out[i] = fixed;
    }
    return out;
}

}  // namespace

std::string film_join_blocker(const ProjectStore& store) {
    const Project project = store.load_project();
    const models::Story story = story_or_empty(store);
    const auto eps = chapter_episodes(project, story);
    if (eps.empty()) return SAY("一章都没有，没什么可接");
    for (const Episode* e : eps) {
        if (!films_of(store.paths().output(), e->episode_id).empty()) return "";
    }
    return SAY("还没有一章出片。这一章出了片就能合成");
}

// `settings` 只在各章规格不齐、要把那几章重编一遍时才读（[assembly] 那套编码参数）。
FilmJoinReport join_film(const ProjectStore& store,
                         const config::Settings& settings,
                         const media::FFmpeg& ff, JobProgress& progress) {
    const std::string blocker = film_join_blocker(store);
    if (!blocker.empty()) throw std::runtime_error(blocker);

    // 取消：合成只有一步（一次 concat），`ff.run` 中途拦不住。开跑前查这一次
    // 就够——在只有一步的流程里假装能中途停，是给人一个按了没反应的按钮。
    if (progress.cancelled()) {
        throw std::runtime_error(SAY(kFilmJoinStoppedMessage));
    }

    const Project project = store.load_project();
    const models::Story story = story_or_empty(store);
    const fs::path out_dir = store.paths().output();
    std::error_code ec;

    // **没片的章跳过**，不是停：用户 2026-09-18「这一章有片就可以成片了」。
    // 跳过的记在清单里，页面上说清这次接的是哪几章。
    std::vector<fs::path> films;
    FilmJoinReport report;
    for (const Episode* e : chapter_episodes(project, story)) {
        auto mine = films_of(out_dir, e->episode_id);
        if (mine.empty()) {
            report.skipped.push_back(e->chapter_refs.front());
            continue;
        }
        report.chapters.push_back(e->chapter_refs.front());
        for (auto& f : mine) films.push_back(f);
        // 镜数问的是「有几镜真的在盘上」，不是 `e->shots.size()`：空壳镜头
        // （shot_id 是空串、没出片）照样占着数组一格，数出来就是虚的。
        for (const models::Shot& s : e->shots) {
            if (!s.video_path.has_value() || s.video_path->empty()) continue;
            if (!fs::is_regular_file(store.paths().abs(*s.video_path), ec)) continue;
            ++report.shots;
        }
    }

    progress.set_total(2);
    progress.set_message(
        SAYF("把 %1 段接成一部电影", std::to_string(films.size())));

    const fs::path dir = final_dir(store.paths());
    // ⚠️ **上一部先别删，等新的拼成了再换。** 原来一进来就 `remove_all(final/)`：
    // ffmpeg 砸了（某一章的片子坏了）、或者最后那一下挪不动（Windows 上旧的那部
    // 正在播放器里开着），成片和 film.json 就都没了——页面上一部电影都不剩。
    fs::create_directories(dir, ec);
    const fs::path work = dir / ".work";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    struct Cleanup {
        const fs::path& d;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(d, e);
        }
    } cleanup{work};

    // **各章规格齐了才零重编码拼。** 每一章按它自己那几镜里最大的尺寸装配——换过机器、
    // 换过画质档、开关过放大、改过 [video] 或帧率之后出的那几章，尺寸帧率就和别的不一样。
    // concat 分离器拿 -c copy 硬拼，不报错，后半截参数全变：播放器花屏、卡住，而合成报
    // 成功。量一遍，不齐的那几章按多数那一种先重编一次（只动不齐的那几章）。
    films = conform_films(films, settings, ff, work, progress);

    const fs::path listing = work / "concat.txt";
    {
        std::ofstream f(listing, std::ios::binary | std::ios::trunc);
        if (!f) {
            throw std::runtime_error(
                SAYF("写不了拼接清单：%1", paths::to_utf8(listing)));
        }
        f << media::concat_listing(films);
    }
    // ⚠️ **这是盘上的文件名，不是话。** `/api/film` 按它回 `name`，
    // `test_film_join.cpp` 逐字钉着，桌面端和播放器也照它找。翻了的话，
    // 换一次界面语言 final/ 里就多一部"另一个名字的成片"，而谁都不报错。
    const std::string film_name = SAY_NEVER("成片.mp4");
    const fs::path staged = work / paths::from_utf8(film_name);
    // 隐式标识。这一步和各章那一步是**同一个函数**——两处内容不同（这儿是
    // 整部电影，填得出项目名），但"标识长什么样"只有一份。
    //
    // 必须在这儿写一次：`-f concat` 不继承输入的元数据，各章成片上那份到
    // 这里一栏都不剩（实测）。
    ff.run(media::concat_args(
        listing, staged,
        media::product_tag_args(models::utc_now_iso8601(), project.title)));
    progress.set_done(1);

    // 拼到 .work 里、成了再挪进来。ffmpeg 中途失败或者人按了停，
    // final/ 里不会躺着一个半截的 成片.mp4——而列表和播放器不会分辨
    // 它是不是完整的，点开就是一部放到一半断掉的成片。
    const fs::path out = dir / paths::from_utf8(film_name);
    // 拼成了：这会儿才把上一部（成片、film.json，以及换过界面语言留下的别名）清掉。
    // 先列再删：边走边删一个目录，迭代器会不会失效标准不保证。
    std::vector<fs::path> old;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path() != work) old.push_back(entry.path());
    }
    for (const auto& p : old) {
        std::error_code rm;
        fs::remove_all(p, rm);
    }
    fs::rename(staged, out, ec);
    if (ec) throw std::runtime_error(SAYF("挪不动合成好的电影：%1", ec.message()));
    report.path = out;
    progress.add_output(paths::to_utf8(out));

    // 时长量成片自己，不拿各章时间线加：时间线是排期的估算，成片是最后
    // 出来的那个文件，页面上报的秒数要和人拖进度条看到的一样。
    //
    // **量不到就空着，不留 0**：`ff.probe` 会抛（ffprobe 不在配置的那个路径
    // 上），也会不抛而回 0（容器里没写 duration，`media::to_double` 照文档
    // 回 0）。留 0 的话 film.json 里就写下一个 0，而 GET /api/film 认的是
    // 「是不是个数」，照抄给页面——于是一次**成功的合成**在页面上写成
    // 「成片 · 0 秒」，成片文件却好好地在 output/final 里。
    //
    // 量不到**也不抛**：为了一个给人看的秒数把整次合成报成失败，用户会以为
    // 成片没出来而重跑一遍。
    try {
        const double measured = ff.probe(out).duration_s;
        if (measured > 0.0) report.total_s = measured;
    } catch (const std::exception&) {
        // 抛了也是"没量到"，和量出 0 一样：total_s 空着。
    }

    const json manifest{
        {"name", film_name},
        // 没量到写 null，不写 0。读的那头（`http::get_film`）只认数字，
        // 所以 null 和这一栏干脆不写是同一个意思：不知道。
        {"total_s", report.total_s ? json(*report.total_s) : json(nullptr)},
        {"shots", report.shots},
        {"chapters", report.chapters},
        {"skipped", report.skipped}};
    {
        std::ofstream f(dir / "film.json", std::ios::binary | std::ios::trunc);
        f << manifest.dump(2);
    }
    // 没量到的时候这句话里**不塞一个秒数**。`human_time(0)` 是「0 秒」，
    // 贴在「合成好了：」后面读起来就是合出了一部空的成片。
    progress.set_message(
        report.total_s
            ? SAYF("合成好了：%1", util::human_time(*report.total_s))
            : SAY("合成好了。时长没量出来（ffprobe 没给）"));
    progress.set_done(2);
    // 匿名使用统计：接成一部片子（带成片分钟，地图上那座城市闪一下）
    telemetry::count("film_joined");
    if (report.total_s) {
        telemetry::count("film_minutes", *report.total_s / 60.0);
        telemetry::event("film_joined", *report.total_s / 60.0);
    } else {
        telemetry::event("film_joined");
    }
    return report;
}

}  // namespace changji::pipeline
