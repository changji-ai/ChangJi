#include "http/film.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "config/settings.hpp"
#include "http/job_stream.hpp"
#include "media/ffmpeg.hpp"
#include "pipeline/film_join.hpp"
#include "pipeline/jobs.hpp"
#include "util/fs_time.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::http {

namespace fs = std::filesystem;
using json = nlohmann::json;
using models::ProjectStore;

namespace {

// 这几个小工具每个接口文件各抄一份，是这一层的老规矩（planning.cpp、
// story_api.cpp 里都有一份）。

void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(422, SAY("请求体要是个对象"));
    for (const auto& kv : body.items()) {
        if (!allowed.count(kv.key())) {
            throw ApiError(422, SAYF("多了一个字段：%1", kv.key()));
        }
    }
}

ProjectStore open_project(const std::string& path) {
    const fs::path root = paths::from_utf8(path);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        throw ApiError(404, SAYF("项目目录不存在：%1", path));
    }
    return ProjectStore(root);
}

ProjectStore open_project(const json& body) {
    const auto it = body.is_object() ? body.find("project") : body.end();
    if (it == body.end() || !it->is_string()) {
        throw ApiError(422, SAY("缺 project"));
    }
    return open_project(it->get<std::string>());
}

void load_or_400(const ProjectStore& store) {
    try {
        (void)store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, SAYF("项目读不了：%1", e.what()));
    }
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

json read_json_object(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.good()) return json();
    try {
        json m;
        in >> m;
        return m.is_object() ? m : json();
    } catch (const std::exception&) {
        return json();
    }
}

/// final/ 里那份清单，和它是哪一版写的。
struct Manifest {
    json data = json::object();
    /// 读到了一份（film.json 或者老的 cut.json）。
    bool found = false;
    /// 读到的是 2026-09-18 之前那版「按每集 N 秒切」写的 cut.json。
    bool legacy = false;
};

// 这一版合成写的是 film.json（一部电影、一个文件）；2026-09-18 之前那版
// 「按每集 N 秒切成几集」写的是 cut.json，老项目盘上躺的是它。
//
// **老项目的 cut.json 要读。** 只读 film.json 的那一版，一个 2026-09-17 切过
// 六集的老项目打开成片页是这样：目录里六个 第0N集.mp4 都列得出来，清单读不到
// 于是 total_s / shots 回落成 0——页面照着说「第01集 · 0 秒 · 0 镜」。那个 0
// 不是量出来的，是没读到；**没读到就得说不知道**（回 null），不然人会以为成片
// 切坏了，而盘上六段好好的。
//
// 读了也**不迁**（不把 cut.json 改写成 film.json）：一份 GET 不该往盘上写，
// 而且两份清单说的根本不是一件事——一个描述盘上那几段，一个描述一部电影。
Manifest read_manifest(const fs::path& dir) {
    Manifest mf;
    mf.data = read_json_object(dir / "film.json");
    if (!mf.data.is_null()) {
        mf.found = true;
        return mf;
    }
    mf.data = read_json_object(dir / "cut.json");
    if (!mf.data.is_null()) {
        mf.found = true;
        mf.legacy = true;
        return mf;
    }
    mf.data = json::object();
    return mf;
}

}  // namespace

ApiResult get_film(const std::string& path) {
    const ProjectStore store = open_project(path);
    const fs::path dir = pipeline::final_dir(store.paths());
    // 这几个键**一个都不能少**，目录还不存在时也要带齐：页面拿
    // `film.chapters?.length` 这种写法读它，缺一个就是「读出 undefined 之后
    // 悄悄按 0 走」，而那和「真的是 0 章」长得一模一样。
    //
    // 而量不到的那几个**默认是 null，不是 0**：`0 秒 · 0 镜` 是在信誓旦旦地
    // 报一个没读到的值（同 ShowDialog 那条规矩）。null = 不知道，页面照着
    // 显示「—」。
    json out{{"name", nullptr},
             {"total_s", nullptr},
             {"shots", nullptr},
             {"chapters", json::array()},
             {"skipped", json::array()},
             {"legacy_cut", false},
             {"files", json::array()}};

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {200, out};
    const Manifest mf = read_manifest(dir);
    out["legacy_cut"] = mf.legacy;
    // 这次接了哪几章、跳过哪几章（还没出片的）。页面上要说清。
    // 两版清单都记着这两栏，都是这几个文件的实情。
    out["chapters"] = mf.data.value("chapters", json::array());
    out["skipped"] = mf.data.value("skipped", json::array());

    // 老清单描述的是切出来的**几段**，没有「那部电影」这个东西：`name` 留
    // null（页面别拿 files[0] 冒充），`total_s` 是这几段**加起来**多长，每一
    // 段自己的时长挂回它自己那一条 files[]。
    std::map<std::string, json> parts;
    if (mf.legacy) {
        const json arr = mf.data.value("parts", json::array());
        long long shots = 0;
        bool counted = false;
        if (arr.is_array()) {
            for (const auto& part : arr) {
                if (!part.is_object()) continue;
                if (part.contains("name") && part["name"].is_string()) {
                    parts[part["name"].get<std::string>()] = part;
                }
                // 老清单没有整份的镜数，只有每一段的。切点落在镜头边界上、
                // 一镜只属于一段，加起来就是这几段一共多少镜。
                if (part.contains("shots") && part["shots"].is_number_integer()) {
                    shots += part["shots"].get<long long>();
                    counted = true;
                }
            }
        }
        if (counted) out["shots"] = shots;
        if (mf.data.contains("total_s") && mf.data["total_s"].is_number()) {
            out["total_s"] = mf.data["total_s"];
        }
    } else if (mf.found) {
        if (mf.data.contains("name") && mf.data["name"].is_string()) {
            out["name"] = mf.data["name"];
        }
        if (mf.data.contains("total_s") && mf.data["total_s"].is_number()) {
            out["total_s"] = mf.data["total_s"];
        }
        if (mf.data.contains("shots") && mf.data["shots"].is_number_integer()) {
            out["shots"] = mf.data["shots"];
        }
    }

    std::vector<json> files;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec)) continue;
        if (paths::to_utf8(f.path().extension()) != ".mp4") continue;
        const std::string name = paths::to_utf8(f.path().filename());
        json item{{"name", name},
                  {"rel", store.paths().rel(f.path())},
                  {"size_mb", round1(static_cast<double>(fs::file_size(f.path(), ec)) /
                                     (1024.0 * 1024.0))},
                  {"mtime", static_cast<long long>(util::file_mtime_unix(f.path()))}};
        // 上一版按时长切出来的那几段（盘上叫「第01集.mp4」这种名字）：
        // 每一段自己多长、几镜、从哪一章起（老清单里有）。
        // 这一版合成出来的那一部没有这几栏——它的时长就是上面的 total_s。
        if (const auto it = parts.find(name); it != parts.end()) {
            for (const char* k : {"duration_s", "shots", "from_chapter"}) {
                if (it->second.contains(k)) item[k] = it->second[k];
            }
        }
        files.push_back(std::move(item));
    }
    // 按名字排，顺序稳定就行。
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) {
        return a.at("name").get<std::string>() < b.at("name").get<std::string>();
    });
    for (auto& f : files) out["files"].push_back(std::move(f));
    return {200, out};
}

// 2026-09-18 定成电影平台之前这条叫 POST /api/film/cut，body 里带
// `per_episode_s`（0 = 整部一集），引擎照它把整条时间线切成几集。成片既然
// 就是一部完整的电影、一个文件，这条路由和那个参数一起拔掉了。
// **不留 /api/film/cut 做兼容**：前端打包进这个二进制，没有外部调用方。
ApiResult post_film_join(const json& body) {
    forbid_extra(body, {"project"});
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, SAY("出片那边还在忙，等它完了再合成"));
    }
    ProjectStore store = open_project(body);
    load_or_400(store);
    const std::string blocker = pipeline::film_join_blocker(store);
    if (!blocker.empty()) throw ApiError(400, blocker);

    const std::string root = paths::to_utf8(store.root());
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Run, "",
        [root](pipeline::JobProgress& p) {
            const JobScope scope{p.job_id(), p.token()};
            const ProjectStore st(paths::from_utf8(root));
            const config::Settings s = config::load_settings(st.root());
            const media::FFmpeg ff(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                   media::default_runner());
            pipeline::join_film(st, s, ff, p);
        },
        pipeline::kFilmJoinStoppedMessage, root, SAY("成片 · 合成整部电影"));
    if (!started) {
        throw ApiError(409, SAY("出片那边还在忙，等它完了再合成"));
    }
    return {202, {{"started", true}}};
}

}  // namespace changji::http
