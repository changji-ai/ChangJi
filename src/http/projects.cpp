#include "http/projects.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        // **input 是整个请求体，不是 null。** FastAPI 报缺字段时把父对象
        // 放进 input，前端拿它回显"你提交的是这些"。写 null 的话那一栏是空的。
        // 实时对拍抓出来的（写接口那一轮）。
        throw unprocessable_top(key, "Field required", body, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

std::string opt_str(const json& body, const char* key, const std::string& def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

/// target 是不是**严格**在 root 里面。
///
/// 严格：等于 root 本身不算。等于的话这个接口就能删掉整个项目库，
/// 那是"一次误点删掉所有项目"，比删错一个项目严重得多。
///
/// 两边都先规范化。不规范化的话 `项目库/a/../../别处` 这种路径
/// 字面上看着在里面，实际指向外面。
bool strictly_inside(const fs::path& target, const fs::path& root) {
    std::error_code ec;
    const fs::path t = fs::weakly_canonical(target, ec);
    if (ec) return false;
    const fs::path r = fs::weakly_canonical(root, ec);
    if (ec) return false;
    if (t == r) return false;
    const fs::path rel = t.lexically_relative(r);
    if (rel.empty()) return false;
    // lexically_relative 会产出 ".." 开头的路径表示"在外面"
    const std::string first = paths::to_utf8(*rel.begin());
    return first != "..";
}

/// 把请求里那个 path 解析成真正的目录。
///
/// **只填名字（不带路径分隔符）就落在项目库根目录下。** 用户不知道项目库
/// 挂在哪（容器里、用户数据目录里），让他猜绝对路径没道理。
///
/// 这条规则以前只写在新建那一支里，删除那边直接把原样的字符串交给
/// weakly_canonical——而那是按**进程的工作目录**解析的。于是同一个
/// "我的项目" 传给两个接口指的是两个地方：新建成功了，拿同样的字符串去
/// 删就是 403「只能删项目库里面的」，用户看不出自己哪里错了。
/// 2026-09-11 实测撞到。抽成一处，两边不会再分叉。
fs::path resolve_project_path(const std::string& raw,
                              const config::Settings& settings) {
    fs::path path = paths::expand_user(raw);
    if (path.is_absolute()) return path;

    // ⚠️ **不是绝对路径就一律落在项目库里，而且不许往上走。**
    //
    // 这儿原来只把**一段**的名字接到项目库上（`"我的项目"`），带斜杠的原样
    // 返回——而"原样"是按**进程的工作目录**解析的。桌面端的工作目录是谁启
    // 动它谁说了算（Finder 里双击就是 `/`，命令行里就是当时那个目录），
    // 对用户毫无意义。
    //
    // 2026-09-21 实撞：拿假模型让代理调 `create_project`，名字写
    // `../逃出去`——工具回了一句「建好了：../逃出去」，而项目库里什么都
    // 没有，真东西建在了**仓库旁边**（`/Volumes/…/changji/逃出去`），
    // 连那一轮的对话记录都写进去了。项目库列表里看不见它，人只会觉得
    // "建好的片子丢了"。
    //
    // **名字是模型填的**，而模型读得到网页和热榜（`stages/web_tools`）——
    // 一段带路径的字从那儿进来，就能往项目库外面写东西。
    for (const auto& part : path) {
        if (paths::to_utf8(part) == "..") {
            throw ApiError(400, SAYF("片名里不能有 `..`：%1", raw));
        }
    }
    return settings.workspace_path() / path;
}

}  // namespace

ApiResult post_new_project(const json& body, const config::Settings& settings) {
    const std::string style = opt_str(body, "style_line", "realistic");
    if (style != "realistic" && style != "anime") {
        throw ApiError(400, SAY("风格线只能是 realistic 或 anime"));
    }
    const StyleLine line =
        style == "anime" ? StyleLine::ANIME : StyleLine::REALISTIC;

    const std::string raw = text::strip_ws(need_str(body, "path"));
    if (raw.empty()) throw ApiError(400, SAY("得给项目起个名字"));

    // 只填名字就落在项目库根目录下。见 resolve_project_path。
    const fs::path path = resolve_project_path(raw, settings);

    const std::string name = paths::to_utf8(path.filename());
    const std::string title = opt_str(body, "title", "");

    // 画幅可以在建项目时就定；没给就按内置默认（横屏 720p）。
    // **先校验再建目录**：目录建了再报 400，下次同名就是 409，用户会以为
    // 名字被占了。
    config::VideoConfig video;
    video.orientation = opt_str(body, "orientation", video.orientation);
    video.quality = opt_str(body, "quality", video.quality);
    if (const auto errs = video.validate(); !errs.empty()) {
        throw ApiError(400, errs.front());
    }

    try {
        ProjectStore store = ProjectStore::create(
            path, text::project_slug(name), title.empty() ? name : title, line);
        // 一部电影一份标准参数，建的时候就落下来。见 project_config_template。
        //
        // **这一句也是新项目的画幅被钉在盘上的那一下。** 有了它，
        // `config::load_settings` 读这个项目走的是「项目自己写了
        // [video]」那一支，永远落不到内置默认上，也用不着去 assets.json
        // 里推——推那条路是给没有这份文件的老项目留的。
        config::write_project_config(store.root(), video);
        // **派生那份拷贝也得在这儿写对。**
        //
        // 画幅是唯一的源（config::VideoConfig::aspect_ratio），assets.json
        // 里 StyleProfile.aspect_ratio 只是它派生出来的一份拷贝，给出参考图
        // 用。派生原来只在「存画面」那条路上做（server.cpp 的 /api/video），
        // 而建项目这条路不经过它——于是**直接建一个横屏项目，配出来就是
        // 「横屏 + 9:16」**：成片横的、参考图竖的，而参考图正是每一镜的底子。
        // 2026-09-17 照用户的话建 hengping-test 时当场撞上。
        if (auto assets = store.load_assets();
            assets.style.aspect_ratio != video.aspect_ratio()) {
            assets.style.aspect_ratio = video.aspect_ratio();
            store.save_assets(assets);
        }
        return {200, {{"root", paths::to_utf8(store.root())}}};
    } catch (const fs::filesystem_error& e) {
        throw ApiError(400, SAYF("创建目录失败：%1", e.what()));
    } catch (const ProjectStore::AlreadyAProject& e) {
        // **按类型认，不按那句话认。** 409 不是 400——前端据此提示"换个
        // 名字"，而 400 会被当成参数错。原来这儿是 `msg.find("已经")`，
        // 而那句话 2026-09-22 翻了：德语下找不到「已经」，于是重名项目
        // 回的是 400，前端那条提示再也不出现。见 `AlreadyAProject` 上那段。
        throw ApiError(409, e.what());
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

namespace {

/// 正在跑就别动这个项目的 project.json。
///
/// 跑到一半删项目，工作线程下一次写盘会写到一个不存在的目录上，报的错和
/// "删项目"八竿子打不着；改名更阴——rename 读一份快照改一个字段写回去，
/// 而工作线程手里那份整份快照几十秒后照样落盘，**新名字被静默盖回旧的**，
/// 全程 200，界面上看着像"改名没生效"。
///
/// 2026-09-14 之前这道闸只在删除那条路上，而且不看路径、只问「有没有 Run
/// 在跑」：单卡上一章要跑很久，而「趁着在跑顺手把测试残留清了」恰恰是这段
/// 时间最想干的事，人会收到一句和自己的操作对不上的 409；同时它**漏了
/// Write**（写全片、批量排分镜），那类任务照样往项目目录里写盘。
///
/// ⚠️ **三种"不确定"一律按挡处理**（fail-closed）：
///   · 发起方没说在跑哪个（start 的 project 是尾参，默认空串）；
///   · 那条路径规范化失败（权限、盘符掉线、超长路径）；
///   · 正在跑的项目在目标**底下**——删除是 remove_all，递归的。
/// 放行的代价是弄坏正在跑的那一趟，比误挡严重得多。
/// 要干的是哪一件。**不传动词，传这个**：见下面那段。
enum class What { Delete, Rename };

void guard_not_running(const fs::path& canon, What what) {
    // **整句一条，不是往句子中间塞一个动词。** 原来是 `"正在跑，" + verb +
    // "了会把跑到一半的东西弄坏"`——塞进去的那个「删」「改」在别的语言里
    // 位置全不一样（德语动词跑到最后），碎片翻不了也排不了序。
    const auto whole = [what](bool this_one) {
        if (what == What::Delete) {
            return this_one ? SAY("这个项目正在跑，删了会把跑到一半的东西弄坏")
                            : SAY("正在跑，删了会把跑到一半的东西弄坏");
        }
        return this_one ? SAY("这个项目正在跑，改了会把跑到一半的东西弄坏")
                        : SAY("正在跑，改了会把跑到一半的东西弄坏");
    };
    // **每一件在跑的都要看**：写作按「片子 + 谁派的」分道，同时可能有好几件，
    // 只看"最近那一件"的话，删 B 时恰好 A 是最近的那件，B 的就漏过去了。
    for (const auto kind : {pipeline::JobKind::Run, pipeline::JobKind::Write}) {
        for (const std::string& busy : pipeline::jobs().running_projects(kind)) {
            if (busy.empty()) {
                throw ApiError(409, whole(false));
            }
            std::error_code bec;
            const fs::path busy_path =
                fs::weakly_canonical(paths::from_utf8(busy), bec);
            const bool same_or_inside =
                bec || busy_path == canon || strictly_inside(busy_path, canon);
            if (same_or_inside) {
                throw ApiError(409, whole(true));
            }
        }
    }
}

}  // namespace

ApiResult post_delete_project(const json& body,
                              const config::Settings& settings) {
    const std::string raw = need_str(body, "path");
    const std::string confirm = need_str(body, "confirm_name");

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(settings.workspace_path(), ec);
    if (ec) throw ApiError(400, SAYF("项目库路径不对：%1", ec.message()));

    // **和新建用同一条解析规则。** 以前这里是 expand_user 之后直接交给
    // weakly_canonical，相对路径按进程的工作目录算——新建时填 "我的项目"
    // 建在项目库里，删除时填同一个字符串却指到别处，报 403。
    const fs::path target = resolve_project_path(raw, settings);

    // 闸一：只让删项目库里面的。别的路径可能是用户自己放在别处的项目，
    // 也可能是手滑填的系统目录，不该由这个接口负责。
    if (!strictly_inside(target, root)) {
        throw ApiError(403, SAYF("只能删项目库 %1 里面的项目。别处的请自己删",
                                 paths::to_utf8(root)));
    }

    // 闸二：必须确实是一个项目。指到一个普通目录上的话，
    // 删掉的可能是用户放素材的地方。
    const ProjectStore store(target);
    if (!store.exists()) throw ApiError(404, SAY("这个目录不是一个项目"));

    // 闸二点五：**正在跑的是不是这一个**。
    const fs::path canon = fs::weakly_canonical(target, ec);
    if (ec) throw ApiError(400, SAYF("路径不对：%1", ec.message()));
    guard_not_running(canon, What::Delete);

    // 闸三：名字一字不差。防的是"选错了一行然后顺手点了确认"。
    //
    // **目录名和片名都认。** 2026-09-14 之前只认目录名，而界面上到处显示
    // 的是片名（/api/projects 的 name 就是 `title.empty() ? dir : title`）：
    // 一个目录叫 convenience-store、片名叫「深夜便利店」的项目，确认框要
    // 你打「深夜便利店」才解锁，打完提交引擎回 400 要目录名——**界面上
    // 唯一给你的那个名字正是引擎唯一不收的那个**，这条路彻底堵死。
    const std::string dir_name = paths::to_utf8(canon.filename());
    std::string title;
    try {
        title = store.load_project().title;
    } catch (const std::exception&) {
        // 读不了就只认目录名。坏项目照样要能删掉。
    }
    if (confirm != dir_name && (title.empty() || confirm != title)) {
        throw ApiError(400,
                       SAYF("确认名字对不上，要一字不差地填 %1", dir_name));
    }

    fs::remove_all(canon, ec);
    if (ec) throw ApiError(500, SAYF("删不掉：%1", ec.message()));
    return {200, {{"deleted", paths::to_utf8(canon)}}};
}

ApiResult post_project_premise(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    const std::string premise = need_str(body, "premise");

    ProjectStore store(paths::from_utf8(path));
    // 读→改→存在一把锁里（CLAUDE.md 第十四条）。
    const auto guard = store.lock();
    Project project;
    try {
        project = store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
    project.premise = text::truncate_utf8(text::strip_ws(premise), 2000);
    store.save_project(project);
    // **梗概在盘上有两份，两份一起改。** story.json 里那份是写大纲、故事页读的，
    // project.json 里那份是写剧本的提示词读的。原来这儿只改后一份——故事页上
    // 还是旧那句，下一次从故事页存一下又把旧的写回 project.json，一声不响
    //（post_story 上那段说的是同一件事的另一头）。还没有故事就不凭空建一份。
    if (fs::is_regular_file(store.paths().story_file())) {
        models::Story story = store.load_story();
        if (story.premise != project.premise) {
            story.premise = project.premise;
            store.save_story(story);
        }
    }
    return {200, {{"premise", project.premise}}};
}

ApiResult post_project_rename(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    const std::string raw = need_str(body, "title");

    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(paths::from_utf8(path), ec);
    if (ec) throw ApiError(400, SAYF("路径不对：%1", ec.message()));
    // 正在跑的话改了也白改：工作线程手里那份整份快照会把 title 盖回去。
    // 详见 guard_not_running。
    guard_not_running(canon, What::Rename);

    ProjectStore store(canon);
    // 读→改→存在一把锁里（CLAUDE.md 第十四条）：不拿锁的话，读完、存之前别的
    // 活正好存了一回 project.json，这边整份存回去就把那一回冲掉。
    const auto guard = store.lock();
    Project project;
    try {
        project = store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }

    // 空名字会让列表回落到目录名（readonly.cpp 的 name 字段），看起来像
    // "改名没生效"。直接拦住，让人知道这一步没做成。
    const std::string title = text::truncate_utf8(text::strip_ws(raw), 200);
    if (title.empty()) throw ApiError(400, SAY("片名不能是空的"));

    project.title = title;
    store.save_project(project);
    return {200, {{"title", project.title},
                  {"project_id", project.project_id}}};
}

}  // namespace changji::http
