#include "http/run.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <mutex>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "infer/sd_image.hpp"
#include "stages/prompt_compose.hpp"
#include "models/project.hpp"
#include "http/offload.hpp"
#include "media/assemble.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/preview.hpp"
#include "util/fs_time.hpp"
#include "util/human_time.hpp"
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

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 取一个字符串数组。**分得清"没给"和"给了个空的"**——
/// 这两者在这个接口上语义不同，见 RunOptions::only 的注释。
std::optional<std::vector<std::string>> opt_str_list(const json& body,
                                                     const char* key) {
    const auto it = body.find(key);
    if (it == body.end() || it->is_null()) return std::nullopt;
    if (!it->is_array()) {
        throw unprocessable_top(key, "Input should be a valid list", *it,
                                "list_type");
    }
    std::vector<std::string> out;
    for (const auto& v : *it) {
        if (!v.is_string()) {
            throw unprocessable_top(key, "Input should be a valid string", v,
                                    "string_type");
        }
        out.push_back(v.get<std::string>());
    }
    return out;
}

// 分隔符收 `std::string`：它可能是 `SAY("；")` 的结果，那是个临时对象，
// `const char*` 接不住。
std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

/// Python 的 round()：银行家舍入。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 出片那个槽的**等候队列**。
///
/// **点了不该叫人等，该排上。** 2026-09-20 用户定的。原来这条接口在槽忙着
/// 时回 409「已经在跑 ep01 了」——人要做的事变成"记着这件事、过二十分钟
/// 回来再点一次"，而那正是一台机器该替他记的东西。这和参考图那条走的是
/// 同一条路（`ref_gen.cpp` 的 RefQueue，CLAUDE.md 第九条：排队这件事归
/// 引擎记，不归页面）——页面记的话，关掉标签页就散了。
///
/// **队列里存的是请求体本身**，不是拆好的参数：轮到它时照原样再走一遍
/// `post_run`，该校验的（体检、参考图、这一章还在不在）**在真要跑的那一刻
/// 重新验一遍**。存拆好的参数就等于把校验冻在按下去那一刻，而队列里那件
/// 事可能要等一个钟头——这中间项目被删了、模型换了都有可能。
class RunQueue {
public:
    static RunQueue& instance() {
        static RunQueue q;
        return q;
    }

    /// 排上，回第几个（1 起）。
    ///
    /// **一模一样的不重复排**：手快点两下是常事，而两次一模一样的出片跑
    /// 第二遍没有任何意义（第一遍跑完那几镜已经是终态，第二遍什么都不做）。
    /// 回它原来的位置，让页面照实说「已经排在第 2 个了」。
    int enqueue(json body, RunDeps deps) {
        std::lock_guard<std::mutex> lg(mu_);
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].body == body) return static_cast<int>(i) + 1;
        }
        // 上限是防呆不是限量：点上十几下之后，后面那些多半是手滑或者
        // 页面卡住连点，排着也只是让人更看不懂队列里到底有什么。
        if (pending_.size() >= kMax) {
            throw ApiError(429, SAYF("排着的已经有 %1 件了，等这一批跑完再加",
                                     std::to_string(kMax)));
        }
        pending_.push_back({std::move(body), std::move(deps)});
        return static_cast<int>(pending_.size());
    }

    /// 槽空出来了，起下一件。**只在别的线程上叫**（见 JobTable::set_idle_hook）。
    void start_next() {
        Entry next;
        {
            std::lock_guard<std::mutex> lg(mu_);
            if (pending_.empty()) return;
            // 还占着就别动：留在队里，下一次空出来再说。
            if (pipeline::jobs().running(pipeline::JobKind::Run)) return;
            next = std::move(pending_.front());
            pending_.pop_front();
        }
        try {
            post_run(next.body, next.deps);
        } catch (const std::exception& e) {
            // **起不来要留下话。** 这一件没有 HTTP 响应可回（人早就走了），
            // 不记的话它就这么消失了——页面上那句「排着 1 件」下一拍变成
            // 0 件，而什么都没发生。记在这儿，`GET /api/run` 带回去。
            std::lock_guard<std::mutex> lg(mu_);
            last_error_ = e.what();
        }
    }

    /// 给 `GET /api/run`。`project` 非空时顺带标出哪几件是它的。
    json snapshot(const std::string& project) const {
        std::lock_guard<std::mutex> lg(mu_);
        json items = json::array();
        for (const auto& e : pending_) {
            const std::string p = e.body.value("project", std::string{});
            items.push_back(
                {{"episode_id", e.body.value("episode_id", std::string{})},
                 {"project", p},
                 // 这一件是不是当前这部电影的。**在引擎这头判**：
                 // 路径要规范化才比得对（CLAUDE.md 第十一条）。
                 {"mine", project.empty() ? true : paths::same_dir(p, project)},
                 {"preview_s", e.body.value("preview_s", 0.0)},
                 {"all_episodes", e.body.value("all_episodes", false)}});
        }
        json out = {{"items", items}, {"total", items.size()}};
        if (!last_error_.empty()) out["error"] = last_error_;
        return out;
    }

    /// 清空。回清掉了几件。
    int clear() {
        std::lock_guard<std::mutex> lg(mu_);
        const int n = static_cast<int>(pending_.size());
        pending_.clear();
        last_error_.clear();
        return n;
    }

private:
    struct Entry {
        json body;
        RunDeps deps;
    };
    static constexpr std::size_t kMax = 12;
    mutable std::mutex mu_;
    std::deque<Entry> pending_;
    std::string last_error_;
};

/// 正在跑的是哪一章。拼 409 那句话用。
std::string running_episode() {
    const json snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    const auto it = snap.find("episode_id");
    if (it == snap.end() || !it->is_string()) return "None";  // Python f-string 里 None 就是这么印的
    return it->get<std::string>();
}

}  // namespace

std::vector<pipeline::Stage> parse_stages(
    const std::vector<std::string>& names) {
    std::vector<pipeline::Stage> out;
    std::vector<std::string> unknown;
    for (const auto& raw : names) {
        const std::string name = text::strip_ws(raw);
        if (name.empty()) continue;   // Python 那边先 strip 再滤空
        pipeline::Stage s{};
        if (pipeline::stage_from_string(name, s)) {
            out.push_back(s);
        } else {
            unknown.push_back(name);
        }
    }
    if (!unknown.empty()) {
        // 可选项按字典序列出，对齐 Python 的 sorted(known)。
        throw ApiError(400,
                       SAYF("不认识的阶段 %1。可选：assemble、audio、draft、"
                            "final、frames",
                            join(unknown, SAY("、"))));
    }
    return out;
}

ApiResult post_run(const json& body, const RunDeps& deps) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体要是一个对象"));

    // **不查多余的键。** RunRequest 是个普通的 BaseModel，
    // pydantic 默认忽略多余字段。这里 forbid 的话，前端多传一个键就 422，
    // 而那些键现在就在传（前端和后端的版本不一定同步升）。
    const std::string project_path = need_str(body, "project");
    const std::string episode_id = need_str(body, "episode_id");
    const bool skip_final = opt_bool(body, "skip_final", false);
    // **默认跳过。** 挂 Turbo LoRA 之后两档画质拉不开差距，草稿档就是
    // 白跑一遍。想留着的话显式传 skip_draft: false。
    const bool skip_draft = opt_bool(body, "skip_draft", true);
    // 只跑这几个镜头。**空表示整章**——新界面上每个镜头自己有一个
    // "重新生成"，用户看着某一镜不对，想重跑的就是那一个；
    // 没有这一项的话只能整章重跑，而整章是一小时。
    std::set<std::string> only_shots;
    if (const auto it = body.find("shot_ids");
        it != body.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_string()) only_shots.insert(v.get<std::string>());
        }
    }
    const bool force = opt_bool(body, "force", false);
    const bool all_episodes = opt_bool(body, "all_episodes", false);
    const auto stage_names = opt_str_list(body, "stages");
    // 只做这一章的前 n 秒。0 / 没给 = 整章，老行为。
    double preview_s = 0.0;
    if (const auto it = body.find("preview_s");
        it != body.end() && !it->is_null()) {
        if (!it->is_number()) {
            throw unprocessable_top("preview_s",
                                    "Input should be a valid number", *it,
                                    "float_type");
        }
        preview_s = it->get<double>();
        if (preview_s < 0.0) preview_s = 0.0;
    }
    if (preview_s > 0.0) {
        // **这两条组合起来没有一个说得清的意思，就别猜。**
        //
        // 前 n 分钟是"从这一章第一镜起"的一段，而 all_episodes 是"每一章
        // 都跑一遍"——合起来是"每一章的前 n 分钟"还是"整部电影的前 n
        // 分钟"？两种都讲得通，而猜错的代价是跑了一个钟头做出一个没人要的
        // 东西。stages 同理：前 n 分钟本来就是"配音→首帧→成片→装配"这一
        // 整条，只跑其中一段就没有"够不够 n 分钟"可言。
        if (all_episodes) {
            throw ApiError(400,
                           SAY("「只做前 n 分钟」是一章之内的事，不能和 "
                               "all_episodes 一起给"));
        }
        if (stage_names.has_value() && !stage_names->empty()) {
            throw ApiError(400,
                           SAY("「只做前 n 分钟」要走完整条（配音→首帧→成片→"
                               "装配），不能只指定某几个阶段"));
        }
    }
    // **C++ 独有。** "episode"（默认）= 一章跑完再跑下一章，Python 就这样；
    // "stage" = 所有章先配音，再所有章出首帧……多卡时用这个，见任务体里的注释。
    std::string order = "episode";
    if (const auto it = body.find("order"); it != body.end() && it->is_string()) {
        order = it->get<std::string>();
    }
    if (order != "episode" && order != "stage") {
        throw ApiError(400, SAY("order 只能是 episode 或 stage"));
    }

    // **不认识的键要说一声。**
    //
    // 上面那段解释了为什么不 forbid：前端和引擎的版本不一定同步升，
    // 多一个键就 422 会让整个功能挂掉。那个取舍现在还成立，但**默不作声
    // 的代价是真的**：2026-09-13 我自己把 shot_ids 写成 only_shots，
    // 引擎一声不吭地当成"没指定镜头"，于是「重出这一镜」变成了整章重渲
    // 18 镜、跑了二十分钟——而且是在我以为它只渲了一镜的前提下，
    // 连着几轮拿它当证据。
    //
    // 所以：照收不误（兼容性不变），但把不认识的键列回去。真发过来多余键
    // 的调用方看得见，写错字段名的当场就知道。
    //
    // 核过这一版前端发的就是这五个键，没有多余的，所以这条在正常用法下
    // 是静默的（webapp/client/src/composables/useShots.js 那个 api.run）。
    static const std::set<std::string> kKnown = {
        "project", "episode_id",   "skip_final", "skip_draft", "shot_ids",
        "force",   "all_episodes", "stages",     "order",      "preview_s"};
    std::vector<std::string> unknown;
    for (const auto& [key, _] : body.items()) {
        if (kKnown.count(key) == 0) unknown.push_back(key);
    }

    // ---- 已经在跑了：**排上，不是把人赶走** ----
    //
    // 用户 2026-09-20：「如果有新点击不是叫用户等而是加入队列」。原来这儿
    // 回 409「已经在跑 ep01 了」——人要做的事变成"记着这件事、过二十分钟
    // 回来再点一次"，而那正是机器该替他记的。
    //
    // **排在读项目之前判**（原来 409 也在这个位置）：那几步要读盘，而
    // "排上"这件事不需要它们；真要跑的那一刻会从头再走一遍这个函数，
    // 该验的一样不少。
    //
    // 起下一件的钩子在这儿挂：**挂了才有人去起**，而且挂的是这一次带进来
    // 的 deps（测试塞的是假后端，生产是真的）。钩子跑在刚跑完那条工作线程
    // 上，所以它只把活扔给 Offload——在那条线程上直接 start 会 join 自己。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        pipeline::jobs().set_idle_hook([](pipeline::JobKind kind) {
            if (kind != pipeline::JobKind::Run) return;
            Offload::instance().post([] { RunQueue::instance().start_next(); });
        });
        const int at = RunQueue::instance().enqueue(body, deps);
        return {202,
                {{"started", false},
                 {"queued", true},
                 {"position", at},
                 {"running", running_episode()}}};
    }

    // **开工前那趟体检要在这儿判，不能只长在界面上。**
    //
    // 镜头页那两颗按钮按 `doctor.can_run` 决定灰不灰，而这条接口原来
    // 一道闸都没有——于是同一件事，页面上点不动、`curl` 一发就跑起来了。
    // 两边不一致的代价不是"高级用户能绕过"，是**这条规则根本没有一个
    // 权威的地方**：改了界面那份判据，别的客户端（脚本、老版本前端、
    // 另一个界面）照旧我行我素；而界面那份还可能因为读不到体检而放行。
    //
    // 判在引擎里，界面那份就退化成"提前把按钮变灰"的提示，两边同源。
    // 2026-09-15 实测到这个不一致：页面写着「还不能开工：缺 [models].tts」
    // 把按钮锁死，同一时刻 `POST /api/run` 回 200 照跑。
    //
    // 判据本身在 RunDeps::blocked 里（默认那套是体检，认远程机器）。
    if (deps.blocked) {
        const std::string why = deps.blocked();
        if (!why.empty()) throw ApiError(409, SAYF("还不能开工。%1", why));
    }
    const ProjectStore store = open_project(project_path);
    const Project project = load_or_400(store);

    std::vector<std::string> queue;
    if (all_episodes) {
        // 做的就是量产，一章一章手点没有意义。给了这个就忽略 episode_id。
        for (const auto& ep : project.episodes) {
            if (!ep.shots.empty()) queue.push_back(ep.episode_id);
        }
        if (queue.empty()) {
            throw ApiError(400, SAY("这个项目还没有任何一章有分镜表"));
        }
    } else {
        queue.push_back(episode_id);
    }

    // ---- 一张参考图都拿不到的镜头，不许开跑 ----
    //
    // 首帧那一族是图像**编辑**模型，手上没有编辑源时退化成文生图，出来的
    // 是彩色噪点——而闸门拦不住（方差比真图还大，「不是空图」那条一路绿灯）。
    // 一镜两分钟、一章二十几镜，跑完再说就太晚了：那时候人已经等了一个钟头，
    // 拿到的是一章雪花。
    //
    // **只在收参考图的模型上判**：纯文生图的本来就不传参考图，缺不缺一样跑。
    // 判据和出图那头是同一个函数（`accepts_reference_images`，认文件名）。
    {
        const config::Settings s = config::load_settings(store.root());
        if (config::ModelsConfig::accepts_reference_images(s.models.image)) {
            const models::AssetLibrary assets = store.load_assets();
            for (const std::string& id : queue) {
                const models::Episode* ep = project.episode_by_id(id);
                if (ep == nullptr) continue;
                // **只看这一趟真要跑的那几镜。**
                //
                // 抽屉里「重出这一镜」发的是 `shot_ids`。拿整章去判的话，
                // 同一章里另有一镜缺图就把它也挡了——而那一镜自己的参考图
                // 好好的，人想重出的也只有它。挡错的代价比漏挡大：漏挡最多
                // 是那一镜出张噪点，挡错是**这一镜再也重出不了**，而屏幕上
                // 说的还是另一镜的事。
                //
                // 「只做前 n 分钟」同理，而且更要紧：那条路本来就是"先花
                // 二十分钟看看这片子长什么样"，第 40 镜缺参考图不该把它挡
                // 在门外——那一镜这一轮根本不做。**按计划口径挑**（配音还
                // 没跑，时长还可能变），所以是估的那一批；真跑时前缀重挑，
                // 万一多带进来一镜缺图的，那一镜出的是噪点，闸门会说话。
                std::set<std::string> want = only_shots;
                if (preview_s > 0.0 && want.empty()) {
                    want = pipeline::pick_preview_prefix(
                               *ep, preview_s, config::video_limits_for(s),
                               s.assembly.fps)
                               .id_set();
                }
                std::vector<models::Shot> todo;
                for (const auto& sh : ep->shots) {
                    if (want.empty() || want.count(sh.shot_id)) {
                        todo.push_back(sh);
                    }
                }
                const auto bare = stages::shots_without_refs(todo, assets);
                if (bare.empty()) continue;
                // 名字列前几个就够，二十几个 id 糊一屏没人读。
                std::string ids;
                for (std::size_t i = 0; i < bare.size() && i < 5; ++i) {
                    ids += (i ? SAY("、") : std::string{}) + bare[i];
                }
                if (bare.size() > 5) ids += "…";
                throw ApiError(
                    400,
                    SAYN("%1 有 %n 镜一张参考图都拿不到（%2）。当前出图模型是"
                         "图像编辑模型，没有参考图它会退化成文生图、出来是"
                         "噪点，而闸门拦不住。先去设定页把这几镜用到的角色"
                         "定妆、给场景出空景图（「照故事定妆」+「一键出图」），"
                         "再回来跑。",
                         static_cast<long long>(bare.size()), id, ids));
            }
        }
    }

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Run, queue[0],
        [store, queue, skip_final, skip_draft, only_shots, force, stage_names,
         order, preview_s,
         deps](pipeline::JobProgress& p) {
            // 配置和后端在**任务开始时**取一次，不是注册时。
            // 用户改完模型文件不用重启，但一次跑的中途不会换——
            // 中途换的话同一章里前半段和后半段用的是不同的模型。
            config::Settings settings = deps.settings();
            HardwareProfile profile = deps.profile();

            // 采样中途的预览图往界面推。任务开始时挂上、结束时自动摘掉
            // ——`p` 只在这个任务体里有效，摘晚了下一条预览会写进一个已经
            // 不存在的任务。**可以和别人同时挂着**（设定页那边出参考图也
            // 挂一个），各认各的 tag。
            infer::PreviewSinkHandle preview_sink(
                [&p](const std::string& tag, int step, std::string data_url) {
                    pipeline::Event e;
                    e.kind = "preview";
                    e.shot_id = tag;
                    e.current = step;
                    e.preview = std::move(data_url);
                    p.report(e);
                });

            // **项目自己的 changji.toml 盖在全局上。** 画幅和清晰度
            // （[video]）写在那儿：一台机器上可以同时有一部横屏的正片和
            // 一批竖版的物料（竖屏预告、花絮），画幅是这部电影的属性。
            //
            // 只在这一层合，不在 deps.settings() 里——那个函数不知道
            // 当前跑的是哪个项目，而同一个进程会轮流跑好几个。
            settings = config::load_settings(store.root());
            // **画幅、出片步数、首帧步数一次算出来。**
            // 这段判断以前写在这儿，而设置页那边照着档位表自己显示，
            // 两处于是分叉：界面写"成片步数 28"，实际每一镜跑 Turbo 的 6 步。
            // 现在两边都调这一个函数，见 config::effective_spec。
            pipeline::apply_project_spec(settings, profile);
            // 单镜上限也是这部电影的属性（[video].max_shot_s），这条路不经过
            // Runtime，要自己按项目那份设置算一遍。见 config::apply_video_limits。
            config::apply_video_limits(settings);
            const pipeline::Backends backends = deps.backends(settings, store);

            // 阶段名的校验在这里，不在上面的路由里：
            // Python 那边它在 run_stages 内部，错误落进任务状态
            // 而不是变成 400。见 parse_stages 的注释。
            std::optional<std::vector<pipeline::Stage>> only;
            if (stage_names.has_value() && !stage_names->empty()) {
                try {
                    only = parse_stages(*stage_names);
                } catch (const std::exception& e) {
                    p.set_error(e.what());
                    return;
                }
            }

            std::vector<std::string> errors;
            // 跑一章的某几个阶段。出错记下来，不拖垮后面的。
            const auto run_one = [&](const std::string& id,
                                     std::optional<std::vector<pipeline::Stage>> which) {
                p.set_episode_id(id);
                try {
                    pipeline::RunOptions opts;
                    opts.episode_id = id;
                    opts.skip_final = skip_final;
                    opts.skip_draft = skip_draft;
                    opts.only_shots = only_shots;
                    opts.force = force;
                    opts.preview_s = preview_s;
                    opts.only = std::move(which);
                    // 前 n 分钟走自己那条编排（配音之后重挑、跑完量一次、
                    // 不够再来一轮、装成预告），见 pipeline/preview.hpp。
                    const auto report =
                        preview_s > 0.0
                            ? pipeline::run_preview(store, profile, settings,
                                                    opts, backends, p,
                                                    p.token())
                            : pipeline::run_episode(store, profile, settings,
                                                    opts, backends, p,
                                                    p.token());
                    if (!report.errors.empty()) {
                        errors.push_back(
                            SAYF("%1：%2", id, join(report.errors, SAY("；"))));
                    }
                } catch (const std::exception& e) {
                    errors.push_back(SAYF("%1：%2", id, e.what()));
                }
            };

            if (order == "stage") {
                // **按阶段排。** 所有章先配音，再所有章出首帧……
                //
                // 为什么多卡时要这样：按章排的话每一章都要经历一次配音
                // （串行、协调者那张卡）→ 首帧 → 草稿 → 成片 → 装配（CPU），
                // 其间八个工作进程反复空转；每个阶段结尾都有一条"等最慢那镜"
                // 的尾巴；每换一个阶段所有工作进程都要换一次模型。十章就是
                // 十遍。按阶段排，尾巴从十条变一条，模型每个阶段只换一次。
                //
                // 阶段内部的代码一行不动：每个阶段就是 run_episode 带
                // only={那一个阶段} 跑一遍，挑镜头仍然按状态来，断点续跑照旧。
                std::vector<pipeline::Stage> stages = {
                    pipeline::Stage::Audio, pipeline::Stage::Frames,
                    pipeline::Stage::Draft, pipeline::Stage::Final,
                    pipeline::Stage::Assemble};
                if (only.has_value()) {
                    // 用户只要某几个阶段：保留顺序，只留要的
                    std::vector<pipeline::Stage> kept;
                    for (auto st : stages) {
                        if (std::find(only->begin(), only->end(), st) != only->end()) {
                            kept.push_back(st);
                        }
                    }
                    stages = std::move(kept);
                }
                if (skip_final) {
                    stages.erase(std::remove(stages.begin(), stages.end(),
                                             pipeline::Stage::Final),
                                 stages.end());
                }
                if (skip_draft) {
                    stages.erase(std::remove(stages.begin(), stages.end(),
                                             pipeline::Stage::Draft),
                                 stages.end());
                }
                const int total = static_cast<int>(stages.size() * queue.size());
                p.set_queue(0, total);
                int done = 0;
                for (const auto st : stages) {
                    for (const auto& id : queue) {
                        if (p.cancelled()) return;
                        run_one(id, std::vector<pipeline::Stage>{st});
                        p.set_queue(++done, total);
                    }
                }
            } else {
                p.set_queue(0, static_cast<int>(queue.size()));
                int done = 0;
                for (const auto& id : queue) {
                    if (p.cancelled()) return;
                    run_one(id, only);
                    p.set_queue(++done, static_cast<int>(queue.size()));
                }
            }
            if (!errors.empty()) p.set_error(join(errors, SAY("；")));
        },
        /*stop_message=*/"",
        // 顶栏那块"AI 作业中"要靠它说清是哪部电影、点了往哪儿跳。
        paths::to_utf8(store.root()),
        // 任务页面那一行。一次跑几章就说几章。
        // 前 n 分钟那条要说清是预告，不然任务页上它和"出片 · ep01"长得
        // 一模一样，而两者一个是十几分钟、一个是一个钟头。
        preview_s > 0.0
            ? SAYF("预告 · %1 · 前 %2", queue[0], util::human_time(preview_s))
        : queue.size() == 1
            ? SAYF("出片 · %1", queue[0])
            : SAYF("出片 · %1 章", std::to_string(queue.size())));

    // start() 只在同种任务已经在跑时返回 false，而上面刚判过。
    // 还是要判：那两步之间没有锁，两个请求同时进来时后一个要拿到 409，
    // 不能两条线程一起写同一个槽。
    if (!started) {
        throw ApiError(409, SAYF("已经在跑 %1 了", running_episode()));
    }
    json out = {{"started", true}, {"queue", queue}};
    if (!unknown.empty()) {
        // **列回去，不拦。** 见上面收集它的地方：写错字段名的当场看得见，
        // 而版本不同步时多出来的键照旧被忽略、功能不挂。
        out["ignored_fields"] = unknown;
    }
    return {200, out};
}


ApiResult get_run_status(const std::string& project) {
    json out = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    // **没在跑的时候一律算"是你的"**：那时候页面读它只是为了知道"闲着"，
    // 判成别人的会让「停下」那一栏在闲着时还挂着。
    out["mine"] =
        project.empty() || !out.value("running", false)
            ? true
            : paths::same_dir(
                  pipeline::jobs().running_project(pipeline::JobKind::Run),
                  project);
    out["queue"] = RunQueue::instance().snapshot(project);
    return {200, out};
}

ApiResult post_run_queue_clear() {
    return {200, {{"cleared", RunQueue::instance().clear()}}};
}

ApiResult get_run_preview(const std::string& path,
                          const std::string& episode_id, bool all_episodes,
                          bool skip_final, bool skip_draft, bool force,
                          const HardwareProfile& profile, double preview_s) {
    const ProjectStore store = open_project(path);
    const Project project = load_or_400(store);

    std::vector<const Episode*> episodes;
    if (all_episodes) {
        for (const auto& e : project.episodes) {
            if (!e.shots.empty()) episodes.push_back(&e);
        }
    } else if (const Episode* ep = project.episode_by_id(episode_id)) {
        episodes.push_back(ep);
    }
    if (episodes.empty()) throw ApiError(400, SAY("没有可跑的章节，先出分镜"));

    // ---- 前 n 分钟：这一次只算前缀那几镜 ----
    //
    // **格子要用这部电影自己那一份**（`[video].max_shot_s` 是一部电影的
    // 属性）。拿进程里那份全局的会读成"上一次跑的是哪部电影"，于是按钮上
    // 写「前 7 镜」而引擎真跑时挑的是 8 镜——两处说的不是一件事。
    // **和 `POST /api/run` 挡的是同一件事。** 那边这个组合是 400（前 n
    // 分钟是一章之内的事）；这边默不作声地当成整章的话，按钮底下那行预估
    // 报的是另一件事，而人按它安排时间——"预览和真按下去那一下说同一件事"
    // 是这条接口存在的全部理由。
    if (preview_s > 0.0 && all_episodes) {
        throw ApiError(400,
                       SAY("「只做前 n 分钟」是一章之内的事，不能和 "
                           "all_episodes 一起给"));
    }
    const config::Settings settings_for_pick = config::load_settings(store.root());
    pipeline::PreviewPick pick;
    std::set<std::string> only_preview;
    if (preview_s > 0.0) {
        pick = pipeline::pick_preview_prefix(
            *episodes.front(), preview_s,
            config::video_limits_for(settings_for_pick),
            settings_for_pick.assembly.fps);
        only_preview = pick.id_set();
    }

    // 阶段的先后。**一个镜头从它现在的状态开始，会一路走完后面所有阶段。**
    static const char* kOrder[] = {"audio", "frames", "draft", "final"};
    // **这几个词是兜底，不是文案的源头。** 界面按 `stage` 那个键去查自己
    // 那张表（webapp 的 api/labels.js 的 STAGE_LABELS），查不到才用这儿的
    // ——也就是"引擎加了个前端还不认识的阶段"那一种。
    //
    // 2026-09-17 分叉过一次：前端那份把「成片档」改成「出片」（草稿档默认
    // 不跑，「档」字没有对立面了），而预估那一行走的是这儿，还写着旧词。
    // 改文案去改那张表，别改这儿。
    //
    // **表里存中文原话，翻译推迟到用它的那一行**（下面 `SAY(kLabels[i])`）。
    // 静态表就地 `SAY()` 会把语言冻在静态初始化那一刻，见 `util/say.hpp`
    // 里 `SAY_NOOP` 那段。
    static const char* kLabels[] = {SAY_NOOP("配音"), SAY_NOOP("首帧"),
                                    SAY_NOOP("草稿档"), SAY_NOOP("成片档")};
    constexpr int kCount = 4;

    // 每个状态从第几个阶段开始。不在表里的（FINAL_DONE、FALLBACK）这一次不动。
    const auto entry_of = [](ShotStatus s) -> int {
        switch (s) {
            case ShotStatus::PLANNED:        return 0;
            case ShotStatus::AUDIO_DONE:     return 1;
            case ShotStatus::FRAME_DONE:     return 2;
            case ShotStatus::DRAFT_REJECTED: return 2;
            case ShotStatus::DRAFT_DONE:     return 3;
            case ShotStatus::FINAL_REJECTED: return 3;
            default:                         return -1;
        }
    };

    int counts[kCount] = {0, 0, 0, 0};
    int total_shots = 0;
    for (const Episode* ep : episodes) {
        for (const auto& shot : ep->shots) {
            // 前 n 分钟：前缀之外的镜头这一轮根本不碰，一个数都不该算进去。
            if (!only_preview.empty() &&
                only_preview.count(shot.shot_id) == 0) {
                continue;
            }
            ++total_shots;
            // 人工确认过的镜头不重跑，除非明确要求。
            if (shot.status == ShotStatus::LOCKED && !force) continue;
            const int start_at = force ? 0 : entry_of(shot.status);
            if (start_at < 0) continue;
            for (int i = start_at; i < kCount; ++i) {
                if (skip_final && i == 3) continue;
                // **草稿档默认不跑，预览要跟上。** 不跟的话它报的是
                // "草稿 22 镜 + 成片 22 镜，1.8 小时"，而实际只跑成片、
                // 用 Turbo 6 步——**一个和实际不符的预览没有意义**，
                // 比不给还糟：用户按它安排时间。
                if (skip_draft && i == 2) continue;
                ++counts[i];
            }
        }
    }

    // **预览要和真跑的那一条用同一套画幅和步数。**
    //
    // 为什么要缩、怎么缩，全写在 config::workload_scale 头上。这里原来有
    // 一份一模一样的算式，/api/hardware 那边（config/runtime.cpp）另有一份
    // 只缩了步数、没缩画幅的——同一件事三个说法。现在只剩一个。
    // 上面挑前缀时已经读过这个项目的设置了，别再读一遍——同一次请求里读两
    // 份，两份还可能不一样（中间有人存了画面设置）。
    const config::Settings& proj_settings = settings_for_pick;
    double final_scale = 1.0;
    if (const auto it = profile.tiers.find(Tier::FINAL);
        it != profile.tiers.end()) {
        const auto& spec = it->second;
        final_scale = config::workload_scale(
            spec.width, spec.height, spec.steps,
            config::effective_spec(proj_settings, spec.steps));
    }

    double seconds = 0.0;
    for (int i = 2; i < kCount; ++i) {
        if (counts[i] == 0) continue;
        const Tier tier = i == 2 ? Tier::DRAFT : Tier::FINAL;
        if (const auto est = profile.estimate_episode(counts[i], tier)) {
            seconds += tier == Tier::FINAL ? *est * final_scale : *est;
        }
    }
    // 配音和首帧比渲染快得多，按经验各给一点，别报一个明显偏小的数。
    seconds += counts[0] * 8.0 + counts[1] * 12.0;

    json stages = json::array();
    bool any = false;
    for (int i = 0; i < kCount; ++i) {
        if (counts[i] == 0) continue;
        any = true;
        stages.push_back({{"stage", kOrder[i]},
                          {"label", SAY(kLabels[i])},
                          {"shots", counts[i]}});
    }

    json ids = json::array();
    for (const Episode* ep : episodes) ids.push_back(ep->episode_id);

    // 拿不到参考图的那几镜。**预览要和真按下去那一下说同一件事**——
    // `post_run` 见到它就 400，这儿先报出来，界面才能在按之前就把按钮关掉、
    // 把原因写清楚。判据同那边：只在收参考图的模型上算。
    json bare_shots = json::array();
    if (config::ModelsConfig::accepts_reference_images(proj_settings.models.image)) {
        const models::AssetLibrary assets = store.load_assets();
        for (const Episode* ep : episodes) {
            for (const auto& id : stages::shots_without_refs(ep->shots, assets)) {
                bare_shots.push_back(id);
            }
        }
    }

    json out = {
        {"episodes", ids},
        {"shots", total_shots},
        {"stages", stages},
        // 这几镜拿不到参考图，按下去会被 post_run 挡住。空数组 = 没有这回事。
        {"shots_without_refs", bare_shots},
        {"idle", !any},
        // Python 的 round() 回的是 int，不是保留零位小数的浮点。
        // 回成 12.0 的话前端拿到的是 "12" 还是 "12.0" 取决于序列化，
        // 而那个数会直接拼进句子里。
        {"estimate_s", static_cast<long long>(std::nearbyint(seconds))},
        {"estimate_text", seconds != 0.0 ? util::human_time(seconds)
                                         : std::string()},
    };
    if (preview_s > 0.0) {
        // 这一块回的是**引擎挑中的那几镜**。镜头墙照它点亮，不让前端自己
        // 按时长再算一遍——算法在两处各一份的话，漂开的那天页面点亮的和
        // 引擎真做的不是同一批，而两边都不会报错。
        out["preview"] = {
            {"shot_ids", pick.shot_ids},
            {"planned_s", round1(pick.planned_s)},
            {"planned_text", util::human_time_precise(pick.planned_s)},
            {"target_s", preview_s},
            // 整章都算上还不够 n 分钟：这一次做的就是整章。页面要说清，
            // 不然人按「只做前 5 分钟」却看见 22 镜全亮着，以为按错了。
            {"enough", pick.enough},
            {"whole_episode", pick.whole_episode},
        };
    }
    return {200, out};
}

ApiResult get_outputs(const std::string& path) {
    const ProjectStore store = open_project(path);
    const fs::path dir = store.paths().output();

    struct Item {
        json body;
        double mtime = 0.0;
    };

    // ---- 前 n 分钟那几条（output/preview/）----
    //
    // **单开一栏，不混进 files。** `files` 是正片那一栏，页面（EpisodeView
    // 的「出片 N」、EpFilm 那张表）按章号认领它；而预告是同一章的**另一条
    // 片子**，混进去两边的数就打架：引擎这头 `media::episode_of_output` 不
    // 认它（所以「N 章已出片」不算），前端那头 `isFilmOf` 用的是子串加边界，
    // 认。同一个文件两个答案，谁也说不出哪个对。
    //
    // 放进子目录之后两边都看不见它，这一栏是它唯一的出口——页面照这栏
    // 单独摆，标清「前 2 分 03 秒」。
    const auto scan = [&store](const fs::path& where) {
        std::vector<Item> out;
        std::error_code ec;
        if (!fs::is_directory(where, ec)) return out;
        for (const auto& f : fs::directory_iterator(where, ec)) {
            if (!f.is_regular_file(ec)) continue;
            std::string ext = paths::to_utf8(f.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".mp4") continue;
            const double mtime = util::file_mtime_unix(f.path());
            const auto size = static_cast<double>(fs::file_size(f.path(), ec));
            out.push_back({json{
                {"name", paths::to_utf8(f.path().filename())},
                {"rel", store.paths().rel(f.path())},
                {"size_mb", round1(size / (1024.0 * 1024.0))},
                {"mtime", static_cast<long long>(mtime)},
            }, mtime});
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const Item& a, const Item& b) { return a.mtime > b.mtime; });
        return out;
    };

    std::error_code ec;
    json previews = json::array();
    for (auto& it : scan(dir / "preview")) previews.push_back(std::move(it.body));

    if (!fs::is_directory(dir, ec)) {
        return {200, {{"files", json::array()}, {"previews", previews}}};
    }

    std::vector<Item> items;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec)) continue;
        // 扩展名不区分大小写：Python 在 Windows 上用的 pathlib.glob
        // 就是不区分的，同一个目录在两个后端下不该列出不同的文件。
        std::string ext = paths::to_utf8(f.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".mp4") continue;

        const double mtime = util::file_mtime_unix(f.path());
        const auto size = static_cast<double>(fs::file_size(f.path(), ec));
        json entry{
            {"name", paths::to_utf8(f.path().filename())},
            {"rel", store.paths().rel(f.path())},
            {"size_mb", round1(size / (1024.0 * 1024.0))},
            {"mtime", static_cast<long long>(mtime)},
        };
        // **这个文件是哪一章的，由引擎说。**
        //
        // 归属判定只有 `media::episode_of_output` 一份（CLAUDE.md 第八条那张
        // 表上就有它：清理一处、计数一处各判一遍，结果切过的章全漏）。客户端
        // 想知道"ep01 的成片是哪个文件"时，照着文件名自己猜就是第三份实现。
        // 认不出的（人自己扔进来的 mp4）不带这个键——"有没有"靠键在不在表达。
        if (const auto ep = media::episode_of_output(paths::to_utf8(f.path().stem()))) {
            entry["episode"] = *ep;
        }
        items.push_back({std::move(entry), mtime});
    }

    // 新的在前。stable_sort 对齐 Python 的 sorted()——时间相同时保持
    // 目录遍历的顺序，不然同一秒里出的两个成片每次刷新都换位置。
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.mtime > b.mtime; });

    json files = json::array();
    for (auto& it : items) files.push_back(std::move(it.body));
    return {200, {{"files", files}, {"previews", previews}}};
}

}  // namespace changji::http
