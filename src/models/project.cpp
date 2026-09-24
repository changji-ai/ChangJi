#include "util/say.hpp"
#include "models/project.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

// 只为了新项目那份画幅：`create` 要把内置默认的比例钉进 assets.json，
// 而那个默认只有一处（config::VideoConfig），不在这边抄一份。见 create()。
#include "config/settings.hpp"
#include "models/versions.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"
#include "util/writer.hpp"

namespace fs = std::filesystem;

namespace changji::models {

using json = nlohmann::json;

namespace {

bool is_slug(const std::string& s, bool allow_dash) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [&](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
               (allow_dash && c == '-');
    });
}

std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

/// 读 JSON 文件。
///
/// 模板参数是为了 ordered_json：读资产库必须保留文档里的键顺序，
/// 因为 characters/locations 在接口响应里是**数组**，顺序是值的一部分。
/// 普通的 nlohmann::json 内部是 std::map，parse 时顺序当场就丢了。
template <typename J>
J read_json_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(SAY("读不到文件：") + paths::to_utf8(path));
    }
    J j = J::parse(in, nullptr, false);
    if (j.is_discarded()) {
        throw std::runtime_error(SAY("文件损坏，不是合法 JSON：") + paths::to_utf8(path));
    }
    return j;
}

/// 原子写。先写临时文件再替换，中途断电不会留下半个文件。
///
/// 与 Python 侧一致的关键点是 rename 必须在**同一个文件系统**上，
/// 所以临时文件放在目标文件的同目录，不能用系统临时目录。
void write_json_atomic(const fs::path& path, const json& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // 临时文件名带随机后缀，避免两个进程同时写时互相覆盖对方的临时文件。
    //
    // **一条线程一个随机源，再加一个全进程递增的号。** 原来是函数里一个
    // static 的 mt19937，几条线程同时存盘时一起摇它——那是数据竞争，两条线程
    // 摇出同一个后缀就往同一个临时文件里交叉写，替换上去的是一个坏 JSON。
    // 写作按对话分道之后同时存盘是常事。
    thread_local std::mt19937 rng{std::random_device{}()};
    static std::atomic<unsigned> seq{0};
    std::ostringstream suffix;
    suffix << ".tmp" << std::hex << rng() << "-" << seq.fetch_add(1);
    const fs::path tmp = path.parent_path() /
                         paths::from_utf8(paths::to_utf8(path.filename()) + suffix.str());

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(SAY("写不了临时文件：") + paths::to_utf8(tmp));
        }
        // ensure_ascii=False + indent=2，与 Python 侧的 json.dump 对齐。
        // nlohmann 默认就不转义非 ASCII，中文原样写出。
        out << data.dump(2);
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            throw std::runtime_error(SAY("写临时文件时出错：") + paths::to_utf8(tmp));
        }
    }

    // fs::rename 在标准里要求目标存在时替换掉它（POSIX 语义），
    // MSVC 底层走的是 MoveFileEx 加 MOVEFILE_REPLACE_EXISTING。
    fs::rename(tmp, path, ec);
#ifdef _WIN32
    // ⚠️ **Windows 上有人正读着它，替换就失败（拒绝访问）。** 读的那一头
    //（`load_story` / `load_project`，不拿存盘锁）用 std::ifstream 开文件，MSVC
    // 开的时候不给"允许删除"那一档共享，MoveFileEx 替换就回 ERROR_ACCESS_DENIED
    //（实测：开着是 5，关了就成）。页面每隔几秒就拉一次 /api/project、/api/story，
    // 撞上的话这一次存盘整个抛掉——写一章是一两分钟的模型输出，批量写作是整件活
    // 当场停下。读一个 JSON 是几毫秒的事，等一下再试。
    for (int i = 0; ec && i < 25; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ec.clear();
        fs::rename(tmp, path, ec);
    }
#endif
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error(SAY("替换文件失败：") + paths::to_utf8(path));
    }
}

/// 一部片子一把存盘锁（可重入：`commit_story` 里面还要调 `sync_…`，同一条
/// 线程第二次拿不能把自己锁死）。**只增不删**——锁被引用着，删了就是悬空。
std::recursive_mutex& project_mutex(const fs::path& root) {
    static std::mutex reg_mu;
    static std::map<std::string, std::unique_ptr<std::recursive_mutex>> reg;
    const std::string key = paths::dir_key(paths::to_utf8(root));
    std::lock_guard lg(reg_mu);
    auto& m = reg[key];
    if (!m) m = std::make_unique<std::recursive_mutex>();
    return *m;
}

}  // namespace

const std::vector<std::string>& project_subdirs() {
    static const std::vector<std::string> kDirs = {
        "refs", "audio", "frames", "shots/draft", "shots/final",
        "subtitles", "output", "logs",
    };
    return kDirs;
}

std::string utc_now_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto us = duration_cast<microseconds>(now - secs).count();

    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    // Python 的 isoformat() 在有微秒时输出六位，时区写成 +00:00 不是 Z。
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06lld+00:00",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(us));
    return buf;
}

// ── Episode ────────────────────────────────────────────────────────────

bool Episode::shots_stale() const {
    // 一镜都没有：谈不上"照旧剧本拆的"，那是"还没拆"，界面上另有话说。
    if (shots.empty()) return false;
    // 空串是"说不清"（老项目没有这个键），不冤枉它。理由见头文件那段。
    if (shots_from.empty()) return false;
    return shots_from != chapter_text_fingerprint(script);
}

bool chapter_written(const Story& story, const Episode& ep) {
    if (ep.chapter_refs.empty()) return false;
    // 一条章节记录就是整整一章，挂着的可能不止一个 id（见 chapter_refs），
    // 读的那几处一律取 front()——写剧本、拆分镜照的也是它。
    const Chapter* c = story.chapter_by_id(ep.chapter_refs.front());
    return c != nullptr && !text::strip_ws(c->text).empty();
}

std::vector<Shot> Episode::sorted_shots() const {
    std::vector<Shot> out = shots;
    // 必须是稳定排序。Python 的 sorted() 是稳定的，order 相同的镜头保持
    // 原有先后；std::sort 不保证，order 有重复时顺序会和 Python 不一致，
    // 而这个顺序直接决定成片里镜头的次序。
    std::stable_sort(out.begin(), out.end(),
                     [](const Shot& a, const Shot& b) { return a.order < b.order; });
    return out;
}

const Shot* Episode::shot_by_id(const std::string& shot_id) const {
    for (const auto& s : shots) {
        if (s.shot_id == shot_id) return &s;
    }
    return nullptr;
}

Shot* Episode::shot_by_id(const std::string& shot_id) {
    return const_cast<Shot*>(
        static_cast<const Episode*>(this)->shot_by_id(shot_id));
}

double Episode::planned_duration_s() const {
    double total = 0.0;
    for (const auto& s : shots) total += s.duration_s;
    return total;
}

std::map<std::string, int> Episode::counts_by_status() const {
    std::map<std::string, int> out;
    for (const auto& s : shots) ++out[to_string(s.status)];
    return out;
}

std::vector<Shot> Episode::shots_needing(ShotStatus status) const {
    std::vector<Shot> out;
    for (const auto& s : sorted_shots()) {
        if (s.status == status) out.push_back(s);
    }
    return out;
}

std::vector<std::string> Episode::validate() const {
    std::vector<std::string> errs;
    if (!is_slug(episode_id, false)) {
        errs.push_back(SAY("章 id 只能是小写字母、数字和下划线，当前是 ") + episode_id);
    }
    if (target_duration_s <= 0.0) {
        errs.push_back(episode_id + SAY("：target_duration_s 必须大于 0"));
    }
    for (const auto& s : shots) {
        for (auto& e : s.validate()) {
            errs.push_back(SAYF("%1/%2：%3", episode_id, s.shot_id, e));
        }
    }
    return errs;
}

// ── Project ────────────────────────────────────────────────────────────

const Episode* Project::episode_by_id(const std::string& episode_id) const {
    for (const auto& e : episodes) {
        if (e.episode_id == episode_id) return &e;
    }
    return nullptr;
}

Episode* Project::episode_by_id(const std::string& episode_id) {
    return const_cast<Episode*>(
        static_cast<const Project*>(this)->episode_by_id(episode_id));
}

void Project::touch() { updated_at = utc_now_iso8601(); }

std::vector<std::string> Project::validate() const {
    std::vector<std::string> errs;
    // 项目 id 比镜头 id 多允许连字符
    if (!is_slug(project_id, true)) {
        errs.push_back(SAY("项目 id 只能是小写字母、数字、下划线和连字符，当前是 ") +
                       project_id);
    }
    if (utf8_len(premise) > 2000) {
        errs.push_back(SAYF("premise 超长：%1 字，最多 2000 字",
                            std::to_string(utf8_len(premise))));
    }
    for (const auto& e : episodes) {
        for (auto& msg : e.validate()) errs.push_back(std::move(msg));
    }
    return errs;
}

// ── ProjectPaths ───────────────────────────────────────────────────────

ProjectPaths::ProjectPaths(const fs::path& root) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(root, ec);
    root_ = ec ? root : p;
}

void ProjectPaths::ensure() const {
    std::error_code ec;
    for (const auto& sub : project_subdirs()) {
        fs::create_directories(root_ / paths::from_utf8(sub), ec);
    }
}

fs::path ProjectPaths::project_file() const { return root_ / kProjectFile; }
fs::path ProjectPaths::assets_file() const { return root_ / kAssetsFile; }
fs::path ProjectPaths::story_file() const { return root_ / kStoryFile; }
fs::path ProjectPaths::refs() const { return root_ / "refs"; }
fs::path ProjectPaths::voices() const { return root_ / "voices"; }
fs::path ProjectPaths::audio() const { return root_ / "audio"; }
fs::path ProjectPaths::frames() const { return root_ / "frames"; }
fs::path ProjectPaths::subtitles() const { return root_ / "subtitles"; }
fs::path ProjectPaths::output() const { return root_ / "output"; }
fs::path ProjectPaths::logs() const { return root_ / "logs"; }

fs::path ProjectPaths::shots(const std::string& tier) const {
    return root_ / "shots" / paths::from_utf8(tier);
}

std::string ProjectPaths::rel(const fs::path& p) const {
    std::error_code ec;
    const fs::path abs_p = fs::weakly_canonical(p, ec);
    const fs::path target = ec ? p : abs_p;

    const fs::path r = fs::relative(target, root_, ec);
    // relative 走不出去时返回空，或者结果以 .. 开头都说明不在项目内
    const std::string s = ec ? std::string() : paths::to_utf8(r);
    if (s.empty() || s.rfind("..", 0) == 0) {
        throw std::runtime_error(
            SAYF("路径不在项目目录内，存进项目会破坏可移植性：%1"
                 "\n请先把文件复制进 %2",
                 paths::to_utf8(target), paths::to_utf8(root_)));
    }
    // 正斜杠，保证 Windows 上存的项目拿到 Linux 上也能读
    std::string posix = s;
    std::replace(posix.begin(), posix.end(), '\\', '/');
    return posix;
}

fs::path ProjectPaths::abs(const std::string& rel_path) const {
    std::error_code ec;
    fs::path p = root_ / paths::from_utf8(rel_path);
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

// ── ProjectStore ───────────────────────────────────────────────────────

ProjectStore::ProjectStore(const fs::path& root) : paths_(root) {}

bool ProjectStore::exists() const {
    std::error_code ec;
    return fs::is_regular_file(paths_.project_file(), ec);
}

ProjectStore ProjectStore::create(const fs::path& root,
                                  const std::string& project_id,
                                  const std::string& title,
                                  StyleLine style_line) {
    ProjectStore store(root);
    if (store.exists()) {
        throw AlreadyAProject(
            SAYF("这个目录已经是一个项目了：%1", paths::to_utf8(store.root())));
    }
    store.paths_.ensure();

    Project project;
    project.project_id = project_id;
    project.title = title.empty() ? project_id : title;
    project.style_line = style_line;
    project.created_at = utc_now_iso8601();
    project.updated_at = project.created_at;
    store.save_project(project);

    AssetLibrary assets;
    assets.style.style_line = style_line;
    // **新项目的画幅当场钉在盘上，不留给初值去答。**
    //
    // `assets.json` 里这份比例平时是派生的拷贝（唯一的源是项目
    // changji.toml 的 `[video].orientation`），但**反过来它也是这个项目
    // 的画幅在盘上的记录**：`create` 不写 changji.toml（写的是
    // `http::post_new_project`），而 `config::load_settings` 对没有
    // `[video]` 的项目正是从这一栏把画幅推回来的。这儿不写的话，新建的
    // 项目就靠 `StyleProfile::aspect_ratio` 那个**为老项目留的**初值
    // （9:16）回答"我是什么画幅"——2026-09-18 默认翻成横屏之后，
    // 那个答案是错的。
    //
    // 建项目时显式选了画幅的那条路会紧接着把这一栏改成它选的那一档
    // （`http::post_new_project` 里 write_project_config 后面那几行）。
    assets.style.aspect_ratio = config::VideoConfig{}.aspect_ratio();
    store.save_assets(assets);
    return store;
}

Project ProjectStore::load_project() const {
    if (!exists()) {
        throw std::runtime_error(
            // ⚠️ **别在这句话里点名某一个客户端的做法。**
            //
            // 这儿原来写的是「用 changji new 创建，或者 cd 到正确的目录」
            // ——命令行的说法。而同一句话会原样摆进网页那套和桌面端
            //（2026-09-21 在桌面端的镜头墙上看见的），那两处的人手边没有
            // 终端，这一行是在教他做一件他做不了的事。
            //
            // 说事实和方向，别说按哪个键：三个客户端里"先建一部"和"换一个
            // 目录"都成立。
            SAYF("这里不是一个项目目录：%1\n先建一部，或者换一个目录",
                 paths::to_utf8(root())));
    }
    const json raw = read_json_file<json>(paths_.project_file());
    const int version = raw.value("schema_version", 0);
    if (version > kSchemaVersion) {
        throw std::runtime_error(
            SAYF("项目是用更新版本的场记创建的（格式版本 %1，本机支持到 %2）。"
                 "请升级后再打开",
                 std::to_string(version), std::to_string(kSchemaVersion)));
    }
    return raw.get<Project>();
}

AssetLibrary ProjectStore::load_assets() const {
    std::error_code ec;
    if (!fs::is_regular_file(paths_.assets_file(), ec)) {
        return AssetLibrary{};
    }
    // 用 ordered_json 而不是 json：见 read_json_file 的注释。
    const auto raw = read_json_file<nlohmann::ordered_json>(paths_.assets_file());

    // ⚠️ **形状不对不能当成"这个项目还没有角色"。**
    //
    // `characters` / `locations` 是 **id → 内容** 的对象（OrderedMap），
    // 上面那段注释讲的就是它为什么不是数组。而反序列化用的是
    // `..._WITH_DEFAULT` 那个宏：类型对不上时它**不报错，回默认值**——
    // 一个空库。于是一份手改歪了的 assets.json（比如把它写成数组）读出来
    // 和"新项目"一模一样：界面显示「还没有角色」，人看不出文件里其实有
    // 十几个角色、几十条服装和一堆参考图路径。
    //
    // 更糟的是下一步：这时候点「照故事定妆」，merge_bible 会拿新生成的
    // 那份**盖掉**这个文件——数据就真没了。
    //
    // 手改 assets.json 是这个项目**写在文档里的用法**（见
    // character.cpp 里「唯一的办法是手改 assets.json」那一段），所以这一
    // 处必须说话。抛出去：上层 `load_assets_or_400` 那条本来就是为"文件
    // 本身是坏的"准备的。
    for (const char* key : {"characters", "locations"}) {
        if (!raw.contains(key)) continue;
        const auto& node = raw.at(key);
        if (node.is_null() || node.is_object()) continue;
        throw std::runtime_error(
            SAYF("assets.json 里的 %1 得是一个对象（\"id\": {…} 这种），"
                 "现在是 %2。这份资产库没有装进来——先把它改回对象再打开，"
                 "别在这个状态下重新定妆，那会把文件盖掉：",
                 key, node.type_name()) +
            paths::to_utf8(paths_.assets_file()));
    }

    AssetLibrary lib = raw.get<AssetLibrary>();

    // **没写画风就按这条线补一个。** 空着的后果不是"少一句修饰"：整条
    // 提示词里一个画风词都没有，出图模型每张各自发挥——同一个项目里三个
    // 角色出了皮克斯 3D、半写实、照片三种质感（2026-09-12 实见）。
    //
    // 补在这儿而不是出图那一层：这样它是项目里一条看得见的数据，项目页
    // 那个「画风」框里显示出来、改得动。清空再存的话下次读又会补回来——
    // 那是对的：总得有个底子，"没有画风"不是一种画风。
    if (text::strip_ws(lib.style.global_style).empty()) {
        lib.style.global_style = default_style(lib.style.style_line);
    }
    return lib;
}

Story ProjectStore::load_story() const {
    std::error_code ec;
    if (!fs::is_regular_file(paths_.story_file(), ec)) {
        return Story{};
    }
    return read_json_file<json>(paths_.story_file()).get<Story>();
}

std::unique_lock<std::recursive_mutex> ProjectStore::lock() const {
    return std::unique_lock<std::recursive_mutex>(project_mutex(root()));
}

namespace {

json read_before(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return json();
    try {
        return read_json_file<json>(p);
    } catch (const std::exception&) {
        return json();
    }
}

}  // namespace

void ProjectStore::save_project(Project& project) const {
    std::vector<Overwrite> found;
    {
        const auto g = lock();
        project.touch();
        const json after = project;
        // **先读出盘上那份再写**：比的是"这一下换掉了什么"（models/versions.hpp）。
        const json before = read_before(paths_.project_file());
        write_json_atomic(paths_.project_file(), after);
        found = detail::note_overwrites(paths_, "project", before, after);
    }
    detail::report_overwrites(paths::to_utf8(root()), found);
}

void ProjectStore::save_assets(const AssetLibrary& assets) const {
    const auto g = lock();
    write_json_atomic(paths_.assets_file(), json(assets));
}

void ProjectStore::save_story(const Story& story) const {
    std::vector<Overwrite> found;
    {
        const auto g = lock();
        const json after = story;
        const json before = read_before(paths_.story_file());
        write_json_atomic(paths_.story_file(), after);
        found = detail::note_overwrites(paths_, "story", before, after);
    }
    detail::report_overwrites(paths::to_utf8(root()), found);
}

std::string ProjectStore::copy_into(const fs::path& src,
                                    const std::string& subdir,
                                    const std::string& name) const {
    std::error_code ec;
    const fs::path source = fs::weakly_canonical(src, ec);
    const fs::path from = ec ? src : source;
    if (!fs::is_regular_file(from, ec)) {
        throw std::runtime_error(SAY("文件不存在：") + paths::to_utf8(from));
    }
    const fs::path target_dir = root() / paths::from_utf8(subdir);
    fs::create_directories(target_dir, ec);

    const fs::path target =
        target_dir / (name.empty() ? from.filename() : paths::from_utf8(name));
    if (from != target) {
        fs::copy_file(from, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error(SAY("复制失败：") + paths::to_utf8(from) + " → " +
                                     paths::to_utf8(target));
        }
    }
    return paths_.rel(target);
}

bool is_regular_episode(const std::string& episode_id) {
    if (episode_id.size() <= 2) return false;
    if (episode_id.compare(0, 2, "ep") != 0) return false;
    return std::all_of(episode_id.begin() + 2, episode_id.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

// ---- 这一块是谁写的（models/versions.hpp） ----

namespace {

std::mutex& sink_mu() {
    static std::mutex m;
    return m;
}
OverwriteSink& sink_slot() {
    static OverwriteSink s;
    return s;
}

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// 覆盖的编号。八位十六进制，够一部片子用了；撞上的话留底文件名里还有时刻。
std::string new_overwrite_id() {
    thread_local std::mt19937_64 rng{std::random_device{}() ^
                                     static_cast<std::uint64_t>(now_ms())};
    std::ostringstream os;
    os << std::hex << (rng() & 0xffffffffULL);
    std::string s = os.str();
    while (s.size() < 8) s.insert(s.begin(), '0');
    return s;
}

json read_json_or_null(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return json();
    try {
        return read_json_file<json>(p);
    } catch (const std::exception&) {
        return json();
    }
}

struct UnitView {
    std::string fp;
    json payload;
};
using Units = std::map<std::string, UnitView>;

std::string text_or_empty(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// story.json 里的块：大纲骨架一块，每章正文一块。
///
/// **大纲的指纹只认骨架**（一句话故事、类型、每章标题和梗概）：人物表、
/// 关系、地点「理解故事」会补，那不是"把大纲换了"。留底的那份把它们也带上，
/// 恢复的时候一起回去。**正文空着的章不算一块**——没写的东西谈不上被谁盖掉。
Units story_units(const json& s) {
    Units u;
    if (!s.is_object()) return u;
    json skel = json::object();
    for (const char* k : {"premise", "logline", "genre", "tone"}) {
        skel[k] = text_or_empty(s, k);
    }
    json chapters = json::array();
    if (const auto it = s.find("chapters"); it != s.end() && it->is_array()) {
        for (const json& ch : *it) {
            if (!ch.is_object()) continue;
            const std::string id = text_or_empty(ch, "chapter_id");
            json meta = json::object();
            for (const char* k : {"chapter_id", "title", "summary", "reveal", "plant"}) {
                meta[k] = text_or_empty(ch, k);
            }
            chapters.push_back(std::move(meta));
            const std::string fp = chapter_text_fingerprint(text_or_empty(ch, "text"));
            if (!id.empty() && !fp.empty()) u["chapter/" + id] = UnitView{fp, ch};
        }
    }
    skel["chapters"] = chapters;
    if (!chapters.empty() || !skel.value("logline", std::string()).empty()) {
        json payload = skel;
        for (const char* k : {"characters", "relations", "locations"}) {
            if (const auto it = s.find(k); it != s.end()) payload[k] = *it;
        }
        u["outline"] = UnitView{text::sha1_hex(skel.dump()), std::move(payload)};
    }
    return u;
}

/// 分镜表的指纹。**只认人会当成"分镜改了"的那几栏**——和
/// `agent/outcome.cpp` 的 `board_sig` 同一套：状态、路径、配音回填的实长每出
/// 一次片都变，算进来的话出完片就成了"分镜被出片盖了"。
std::string board_sig(const json& shots) {
    std::string s;
    for (const json& shot : shots) {
        if (!shot.is_object()) continue;
        for (const char* k : {"shot_id", "shot_size", "camera_move", "visual_desc",
                              "first_frame_prompt"}) {
            s += text_or_empty(shot, k);
            s += '|';
        }
        if (const auto it = shot.find("dialogue"); it != shot.end() && it->is_array()) {
            for (const json& d : *it) {
                if (d.is_object()) s += text_or_empty(d, "text");
                s += '|';
            }
        }
        s += ';';
    }
    return s;
}

/// project.json 里的块：每章剧本一块、每章分镜一块。
Units project_units(const json& p) {
    Units u;
    if (!p.is_object()) return u;
    const auto it = p.find("episodes");
    if (it == p.end() || !it->is_array()) return u;
    for (const json& ep : *it) {
        if (!ep.is_object()) continue;
        const std::string id = text_or_empty(ep, "episode_id");
        if (id.empty()) continue;
        const std::string script = text::strip_ws(text_or_empty(ep, "script"));
        if (!script.empty()) {
            u["script/" + id] =
                UnitView{text::sha1_hex(script),
                         json{{"episode_id", id},
                              {"script", text_or_empty(ep, "script")},
                              {"synopsis", text_or_empty(ep, "synopsis")}}};
        }
        const auto sh = ep.find("shots");
        if (sh != ep.end() && sh->is_array() && !sh->empty()) {
            u["board/" + id] =
                UnitView{text::sha1_hex(board_sig(*sh)),
                         json{{"episode_id", id},
                              {"shots", *sh},
                              {"shots_from", text_or_empty(ep, "shots_from")}}};
        }
    }
    return u;
}

Units units_of(const std::string& file, const json& j) {
    return file == "story" ? story_units(j) : project_units(j);
}

/// 块名当目录名用。块名里的章号都是 `ch03` / `ep_01` 这种，这儿再滤一道：
/// 盘上的 json 被人改坏了也不至于写到项目外面去。
std::string safe_unit_path(const std::string& unit) {
    std::string out;
    for (const char c : unit) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/';
        out += ok ? c : '_';
    }
    // `..` 滤掉了点，不会有；开头的斜杠也不许。
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

fs::path versions_dir(const fs::path& root) { return root / "versions"; }

std::vector<Overwrite> read_overwrites(const fs::path& root) {
    std::vector<Overwrite> out;
    const json j = read_json_or_null(versions_dir(root) / "overwrites.json");
    if (!j.is_array()) return out;
    for (const json& e : j) {
        try {
            out.push_back(e.get<Overwrite>());
        } catch (const std::exception&) {
        }
    }
    return out;
}

void write_overwrites(const fs::path& root, const std::vector<Overwrite>& all) {
    json arr = json::array();
    for (const auto& o : all) arr.push_back(o);
    write_json_atomic(versions_dir(root) / "overwrites.json", arr);
}

}  // namespace

void set_overwrite_sink(OverwriteSink sink) {
    std::lock_guard lg(sink_mu());
    sink_slot() = std::move(sink);
}

namespace detail {

std::vector<Overwrite> note_overwrites(const ProjectPaths& paths,
                                       const std::string& file,
                                       const json& before, const json& after) {
    const util::Writer& w = util::current_writer();
    const Units a = units_of(file, before);
    const Units b = units_of(file, after);
    if (a.empty() && b.empty()) return {};

    const fs::path root = paths.root();
    const fs::path owners_file = versions_dir(root) / "owners.json";
    json owners = read_json_or_null(owners_file);
    if (!owners.is_object()) owners = json::object();
    if (!owners.contains("units") || !owners["units"].is_object()) {
        owners["units"] = json::object();
    }
    json& units = owners["units"];

    std::set<std::string> keys;
    for (const auto& [k, v] : a) keys.insert(k);
    for (const auto& [k, v] : b) keys.insert(k);

    const std::int64_t now = now_ms();
    bool dirty = false;
    std::vector<Overwrite> found;
    for (const std::string& key : keys) {
        const auto ia = a.find(key);
        const auto ib = b.find(key);
        const std::string ofp = ia != a.end() ? ia->second.fp : std::string();
        const std::string nfp = ib != b.end() ? ib->second.fp : std::string();
        if (ofp == nfp) continue;

        // 账上记的那一份就是盘上这份吗。**对不上就当不知道是谁的**：中间有
        // 别的路改过它（老版本写的、账还没记上的），硬认一个作者会问错人。
        const auto own = units.find(key);
        const bool own_matches = own != units.end() && own->is_object() &&
                                 !ofp.empty() && own->value("fp", std::string()) == ofp;

        if (w.derived) {
            // 出片这类：不换作者，只把指纹跟上（拆镜、锁时长之后还是那个人的分镜）。
            if (own_matches) {
                if (nfp.empty()) {
                    units.erase(own);
                } else {
                    (*own)["fp"] = nfp;
                }
                dirty = true;
            }
            continue;
        }

        if (own_matches) {
            const std::string owner = own->value("lane", std::string());
            if (owner != w.lane) {
                // **换掉的是别人写的：旧的那份整块留底。** 人改的、按钮派的也
                // 留（回头能找），只是不因此去问谁（见下面那个判据）。
                Overwrite o;
                o.id = new_overwrite_id();
                o.unit = key;
                o.owner_lane = owner;
                o.by_lane = w.lane;
                o.by_title = w.title;
                o.at = now;
                o.stash = "versions/" + safe_unit_path(key) + "/" +
                          std::to_string(now) + "-" + o.id + ".json";
                try {
                    write_json_atomic(root / paths::from_utf8(o.stash),
                                      json{{"unit", key},
                                           {"fp", ofp},
                                           {"owner_lane", owner},
                                           {"by_lane", w.lane},
                                           {"by_title", w.title},
                                           {"at", now},
                                           {"payload", ia->second.payload}});
                    // **两头都是对话、而且不是在按人的决定写回**，才算一件要问的覆盖。
                    // 人手改的、页面按钮派的：人怎么改是人的事（用户原话「用户怎么做
                    // 是用户的事」）。
                    if (w.resolving.empty() && util::is_chat_lane(owner) &&
                        util::is_chat_lane(w.lane)) {
                        found.push_back(std::move(o));
                    }
                } catch (const std::exception&) {
                    // 留底写不下（盘满）：**正文照存**。拦住正文的话丢的是新写的
                    // 那一份，比丢一份底更糟。
                }
            }
        }
        if (nfp.empty()) {
            if (own != units.end()) units.erase(own);
        } else {
            units[key] = json{{"lane", w.lane}, {"fp", nfp}, {"at", now}};
        }
        dirty = true;
    }

    try {
        if (dirty) write_json_atomic(owners_file, owners);
        if (!found.empty()) {
            auto all = read_overwrites(root);
            all.insert(all.end(), found.begin(), found.end());
            write_overwrites(root, all);
        }
    } catch (const std::exception&) {
        // 账记不下同上：正文已经存了，不因为账拖垮这一次写。
    }
    return found;
}

void report_overwrites(const std::string& root, const std::vector<Overwrite>& found) {
    if (found.empty()) return;
    OverwriteSink s;
    {
        std::lock_guard lg(sink_mu());
        s = sink_slot();
    }
    if (!s) return;
    try {
        s(root, found);
    } catch (const std::exception&) {
        // 报不出去不影响存盘。账在盘上，下一轮对话自己会看见。
    }
}

}  // namespace detail

std::vector<Overwrite> list_overwrites(const fs::path& root, bool only_open) {
    auto all = read_overwrites(root);
    if (!only_open) return all;
    std::vector<Overwrite> out;
    for (auto& o : all) {
        if (o.state == "open") out.push_back(std::move(o));
    }
    return out;
}

std::optional<Overwrite> find_overwrite(const fs::path& root, const std::string& id) {
    for (auto& o : read_overwrites(root)) {
        if (o.id == id) return o;
    }
    return std::nullopt;
}

bool settle_overwrite(const fs::path& root, const std::string& id,
                      const std::string& state) {
    const ProjectStore store(root);
    const auto g = store.lock();
    auto all = read_overwrites(root);
    bool hit = false;
    for (auto& o : all) {
        if (o.id == id) {
            o.state = state;
            hit = true;
        }
    }
    if (hit) write_overwrites(root, all);
    return hit;
}

json overwritten_payload(const fs::path& root, const Overwrite& o) {
    // **只认 versions/ 底下、不带 `..` 的**：这一栏是从盘上的账里读的，账被人
    // 改坏了也不能拿它去读项目外面的文件。（别拿 safe_unit_path 洗——它把点也
    // 换掉了，`.json` 洗成 `_json`，留底就读不回来了。）
    const std::string& rel = o.stash;
    if (rel.rfind("versions/", 0) != 0 || rel.find("..") != std::string::npos ||
        rel.find(':') != std::string::npos || rel.find('\\') != std::string::npos) {
        throw std::runtime_error(SAY("留底的位置不对"));
    }
    const json j = read_json_file<json>(root / paths::from_utf8(rel));
    if (!j.is_object() || !j.contains("payload")) {
        throw std::runtime_error(SAY("留底的文件坏了"));
    }
    return j.at("payload");
}

json unit_payload_now(const fs::path& root, const std::string& unit) {
    const ProjectPaths p(root);
    const bool story = unit == "outline" || unit.rfind("chapter/", 0) == 0;
    const json j = read_json_or_null(story ? p.story_file() : p.project_file());
    const Units u = units_of(story ? "story" : "project", j);
    const auto it = u.find(unit);
    return it == u.end() ? json() : it->second.payload;
}

}  // namespace changji::models
