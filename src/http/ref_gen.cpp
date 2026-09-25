#include "http/ref_gen.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <filesystem>
#include <set>
#include <string>

#include "config/runtime.hpp"
#include "http/job_stream.hpp"
#include "http/offload.hpp"
#include "http/reset.hpp"
#include "http/upload.hpp"
#include "infer/sd_image.hpp"
#include "models/project.hpp"
#include "pipeline/episode.hpp"  // frame_spec
#include "pipeline/activity.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/task_board.hpp"
#include "stages/frames.hpp"
#include "stages/ref_images.hpp"
#include "util/cancel_words.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, SAYF("请求里缺少字符串字段 %1", key));
    }
    return body.at(key).get<std::string>();
}

std::string opt_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        return {};
    }
    return text::strip_ws(body.at(key).get<std::string>());
}

bool opt_bool(const json& body, const char* key) {
    return body.is_object() && body.contains(key) && body.at(key).is_boolean() &&
           body.at(key).get<bool>();
}

RefRendererProvider& ref_renderer() {
    static RefRendererProvider provider;
    return provider;
}

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}


struct Rendered {
    std::string rel;
    std::int64_t seed = 0;
    int width = 0;
    int height = 0;
    int steps = 0;
    double seconds = 0.0;
};

/// 真出一张图，写进 refs/。
///
/// 画幅和步数**跟这部电影的首帧完全一样**（项目的 `[video]` 盖在档位表上，
/// 见 pipeline::apply_project_spec）。刻意不另设一档：参考图是拿去喂首帧
/// 的，比首帧小等于先把细节丢掉再让模型照着画，比首帧大只是白花时间——
/// 实测 5090 上多花一倍（544×928 约一分钟，1088×1920 要两分钟）。
Rendered render_ref(const ProjectStore& store, const std::string& stem,
                    const std::string& positive, const std::string& negative,
                    std::int64_t seed, const std::string& stream_id,
                    const stages::FrameRenderer* shared = nullptr,
                    pipeline::Task* enrolled = nullptr) {
    // **项目自己的 changji.toml 盖在全局上**，和跑流水线走同一条路。
    config::Settings settings = config::load_settings(store.root());
    HardwareProfile profile = config::runtime().profile();
    pipeline::apply_project_spec(settings, profile);
    // **比例从画幅来，不从资产库来。** 这儿手里就有 Settings，没有理由
    // 再去读那份派生出来的拷贝——2026-09-14 之前读的是后者，而它能被单独
    // 改成和画幅相反，于是参考图竖的、成片横的，参考图还正是每一镜的底子。
    const TierSpec spec = pipeline::frame_spec(profile, settings)
                              .scaled_to(settings.video.aspect_ratio());
    if (spec.width <= 0 || spec.height <= 0 || spec.steps <= 0) {
        throw ApiError(500,
                       SAY("档位表里没有首帧那一档，出不了图。先去设置页体检一下"));
    }

    const fs::path dest = claim_ref_path(store, stem, ".png");

    // **登记到"在干的活"里去，登记在出图之前。** 这个接口是同步的，
    // 没有任务表那一套，所以它以前在界面上整个不可见——2026-09-11 撞上过：
    // 用户这边正出着参考图（占着图像槽），另一头的批量写作四章全挂在
    // 「显存不够加载 LLM：「图像」正用着」，而顶栏一片安静、GPU 占用 0%，
    // 挡路的那件事只能登服务器翻日志才查得到。
    //
    // 登记在出图之前，是因为**等显存也是在忙**：头一张要先把出图模型
    // 读进显存（十几秒到一分钟），这段时间 sd.cpp 的回调一次都不触发，
    // 界面上就是一个不动的转圈——那正是最需要顶栏说句话的时候。
    // 批量那条**排进队的时候就登记过一行**（`enroll`），这儿接管它就好；
    // 再登记一行的话同一件活在任务页面上会出现两次。单张那条照旧自己开。
    std::optional<pipeline::Activity> own_act;
    if (enrolled == nullptr) {
        // `image` 一词两用，这儿是参考图（见 `util/task_slot.hpp`）。
        own_act.emplace("image", paths::to_utf8(store.root()), std::string{},
                        SAY("正在画参考图"), "assets");
    }
    pipeline::Activity adopt_act = pipeline::Activity(
        enrolled != nullptr ? *enrolled : own_act->task());
    pipeline::Activity& act = adopt_act;
    // **说清楚画的是哪一格。** `stem` 就是那条固定频道上用的 target
    // （下面 ref_progress 发的也是它）。没有 WebSocket 的时候设定页只能从
    // `/api/system` 那份表里认这一格，见 Activity::set_target。
    act.set_target(stem);

    // **和出首帧走同一条后端**（见 RefRendererProvider）：进程内的话，
    // 排执行位、借显存槽、排队等都在 frames.cpp 那一份里；派出去的话，
    // 工作进程那头照样。这一路要的是基础文生图权重，不是 Edit：三视图和
    // 空景图是从纯文字生成的，一张 reference_images 都没有，而 Edit 权重
    // 没有编辑源会退化成文生图（catalog.cpp 上写着「那时候它出的东西
    // 不能看」）。`[models].image_base` 没配时退回 Edit，不会跑不起来。
    stages::PromptBundle prompts;
    prompts.positive = positive;
    prompts.negative = negative;
    prompts.base_model = true;
    prompts.seed_override = seed;
    // 首帧那条路认的是"一镜"；这儿没有镜头，拿 stem 当它的名字——
    // 预览小图也靠这个 tag 挂到设定页对的那一格。
    Shot fake;
    fake.shot_id = stem;
    // **整批那条把后端搭一次就传下来。** 搭一次要建池、探活、连每台机器，
    // 一张一张各搭一次的话，十六张图就是十六次——而它们本来就该跑在同一个
    // 池的同一批位置上。
    const stages::FrameRenderer render =
        shared ? *shared
        : ref_renderer() ? ref_renderer()(settings, store).render
                         : stages::sd_renderer(settings);

    // 只认自己那件活的预览。**同时可以有别人挂着**（出片那条就挂着一个），
    // 不认 tag 的话镜头墙的小图会飘到参考图这边来。
    infer::PreviewSinkHandle preview_sink(
        [&stream_id, &stem](const std::string& tag, int step, std::string url) {
            if (tag != stream_id && tag != stem) return;
            ref_preview(stem, step, url);       // 固定频道那份
            job_preview(stream_id, step, std::move(url));
        });

    const auto t0 = std::chrono::steady_clock::now();
    // **拿这条 stream 自己的令牌。**
    //
    // 这儿原来是就地建一个，谁也够不着它——而 sd_image.cpp 一路上查了五处
    // `tok.cancelled()`，也就是说"停一张图"这件事引擎本来就做得到，只是没
    // 人接上。表现是：顶栏那块「正在思考」上按「停下」，`/api/job/cancel`
    // 在表里找不到这条 stream，回 {stopped:false}，界面照着说一句「这一步
    // 已经结束了」——而它明明还在跑，跑完那几十秒照样把图写进去。
    //
    // `current_cancel()` 没有 JobScope 时回的是哑元（见 job_stream.hpp），
    // 所以同步那条路（老客户端、curl、对拍）一个字没变。
    //
    // 批量那条没有 JobScope（没人点过某一张），`current_cancel()` 回的是
    // 哑元——那样的话任务页面上那个叉按下去什么也不会发生。**那一族的令牌
    // 挂在账本那件活上**，按 id 取消立起来的就是它。
    pipeline::CancelToken& tok =
        enrolled != nullptr ? enrolled->token() : current_cancel();
    try {
        render(fake, prompts, spec, dest, tok,
               [&act, &stream_id, &stem](int step, int steps, double,
                                         infer::Phase phase) {
                   // 读权重、解码和采样不是一个量级（1927 个张量、78 块
                   // vs 8 步），画在同一条进度条上会像"跑到头又倒回去了"。
                   // 顶栏只有一行，就只画采样那一段。
                   if (phase != infer::Phase::Sample) return;
                   act.set_progress(step, steps);
                   // 异步那条路上，点了按钮的人也在等这个数。
                   job_progress(stream_id, step, steps);
                   // 再往固定频道播一份：**刷新过页面的人只剩这条路**。
                   ref_progress(stem, step, steps);
               });
    } catch (const std::exception& e) {
        // **人按的停不是失败。** 报成「出图失败：已取消」的话，人会去找哪
        // 儿出错了。和大模型那一族一致：取消回 400（见 planning.cpp 里
        // stage_guard 那段，LlmError 的「已取消」也是 400）。
        // 派出去那条路抛的是 runtime_error，进程内是 SdError，都在这儿接。
        if (tok.cancelled()) throw ApiError(400, util::kStoppedOne);
        throw ApiError(500, SAYF("出图失败：%1", e.what()));
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();

    std::error_code ec;
    if (!fs::is_regular_file(dest, ec)) {
        throw ApiError(500, SAYF("出图那一步没报错，但文件没落地：%1",
                                 paths::to_utf8(dest)));
    }
    settle_ref_path(store, stem, dest);

    return {store.paths().rel(dest), seed, spec.width, spec.height, spec.steps,
            seconds};
}

}  // namespace

void set_ref_renderer(RefRendererProvider provider) {
    ref_renderer() = std::move(provider);
}

std::int64_t ref_seed(const json& body, const std::string& stem) {
    if (body.is_object() && body.contains("seed") &&
        body.at("seed").is_number_integer()) {
        const std::int64_t given = body.at("seed").get<std::int64_t>();
        // 负数和超范围的都折回正数区间，和 frame_seed 一个规矩
        const std::int64_t wrapped = given % 2147483648LL;
        return wrapped < 0 ? wrapped + 2147483648LL : wrapped;
    }
    const std::string hex = text::sha1_hex("ref:" + stem);
    const auto base =
        static_cast<std::uint32_t>(std::stoul(hex.substr(0, 8), nullptr, 16));
    return static_cast<std::int64_t>(base % 2147483648u);
}

namespace {

/// 真画一张角色参考图并存盘。回的就是这个接口同步跑时那份 body。
///
/// **从接口里抽出来，因为它有两条调用路**（同步那条、后台那条），而两条
/// 必须跑同一段代码——抄一份的话两边迟早只改一边。
///
/// 自己重开一遍 store：后台那条跑起来时，接口那一帧的栈早没了，而重开
/// 就是读两个 json 文件，比起几十秒的出图不值一提。
json character_ref_job(const std::string& project_path,
                       const std::string& char_id, const std::string& slot,
                       std::int64_t seed, const std::string& stream_id,
                       const stages::FrameRenderer* shared = nullptr,
                       pipeline::Task* enrolled = nullptr) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, SAYF("没有角色 %1", char_id));

    Character& c = it->second;
    const Rendered out = render_ref(
        store, char_id + "_" + slot,
        stages::build_character_ref_prompt(c, assets.style, slot),
        stages::ref_negative(assets.style), seed, stream_id, shared, enrolled);

    // **先出图再存盘。** 反过来的话出图失败会留下一条指向不存在的文件的
    // 路径，而界面上那个位置会显示成"已有参考图"——比没有更糟。
    //
    // ⚠️ **存的是"重新读一份、只把这一格的路径填上去"，不是手里这份整份
    // 写回。** 上面那张图要几十秒（头一张还要先把出图模型读进显存，可能是
    // 几分钟），而 `assets` 是那之前读的。整份写回去的话，这期间在设定页
    // 改的外观、提示词、别的格子刚画好的图全被静默盖掉——而"一边出图一边
    // 在抽屉里改字"正是这一页的日常：界面那边专门为它写过"只刷新没改过的
    // 那几条"，结果引擎这头把人改的东西吞了。
    //
    // **读、填、存一把锁**（ProjectStore::lock）：两张卡同时出完两格的话，
    // 各读各存，后存的那份里前一格还是空的——图在盘上、库里说没有。
    std::unique_lock<std::recursive_mutex> store_guard = store.lock();
    AssetLibrary latest = store.load_assets();
    const auto dst = latest.characters.find(char_id);
    if (dst == latest.characters.end()) {
        // 这一格的主人在出图的这几十秒里被删了（重新定妆会收掉重名的）。
        // 图还在磁盘上，但没有地方记它了。
        throw ApiError(409, SAYF("角色 %1 在出图的这几十秒里没了，"
                                 "这张图没记进资产库",
                                 char_id));
    }
    if (slot == "front")              dst->second.ref_front = out.rel;
    else if (slot == "three_quarter") dst->second.ref_three_quarter = out.rel;
    else                              dst->second.ref_back = out.rel;
    store.save_assets(latest);
    store_guard.unlock();

    // 和上传那条一样**无条件重跑**：参考图直接决定画面长什么样。
    return {
        {"saved", out.rel},
        {"slot", slot},
        {"seed", out.seed},
        {"width", out.width},
        {"height", out.height},
        {"steps", out.steps},
        {"seconds", out.seconds},
        // 只退画面里有他的那几镜（reset.hpp 上那段）。
        {"reset_shots", reset_shots_with_character(store, char_id)},
    };
}

}  // namespace

ApiResult post_character_reference_generate(const json& body) {
    const std::string slot =
        body.is_object() && body.contains("slot") && body.at("slot").is_string()
            ? body.at("slot").get<std::string>()
            : std::string("front");
    static const std::set<std::string> kSlots = {"front", "three_quarter",
                                                 "back"};
    if (kSlots.count(slot) == 0) {
        throw ApiError(400, SAY("只有正面、四分之三侧面、背面三个位置"));
    }

    // **能当场判的先当场判。** 项目打不开、没这个角色，这两条要立刻回
    // 400/404——扔到后台去的话，用户点完看到的是"开始了"，几秒后才从
    // 另一条路上飘回来一句报错。
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string char_id = need_str(body, "char_id");
    if (assets.characters.count(char_id) == 0) {
        throw ApiError(404, SAYF("没有角色 %1", char_id));
    }

    const std::string project_path = paths::to_utf8(store.root());
    const std::int64_t seed = ref_seed(body, char_id + "_" + slot);
    const std::string stream_id = opt_str(body, "stream");

    // **异步那条：当场回一句"开始了"，活在后台线程上干。** 理由见
    // http/job_stream.hpp 开头那段——一张几十秒，而这几十秒里落在同一条
    // I/O 线程上的连接全都干等，顶栏那块表更是会直接冻住。
    //
    // 没有 stream 就照旧同步跑到底：结果没地方送回去。老客户端、curl、
    // 对拍脚本走的都是那条，一个字没变。
    if (opt_bool(body, "async") && !stream_id.empty()) {
        const std::string target = char_id + "_" + slot;
        Offload::instance().post([project_path, char_id, slot, seed, stream_id,
                                  target] {
            // **挂上 JobScope。** 这条后台线程要能被 /api/job/cancel 找到
            // ——render_ref 里那个令牌就是从这儿拿的（current_cancel）。
            // 别的异步接口走的 start_async 里挂的也是它。
            const JobScope scope{stream_id};
            try {
                job_done(stream_id, character_ref_job(project_path, char_id,
                                                      slot, seed, stream_id));
                // 固定频道那份：刷新过页面的人靠它知道该重新拉这张图了。
                ref_done(target);
            } catch (const std::exception& e) {
                job_error(stream_id, e.what());
                ref_error(target, e.what());
            }
        });
        return {202, {{"started", true}, {"stream", stream_id}}};
    }

    return {200, character_ref_job(project_path, char_id, slot, seed, stream_id)};
}

namespace {

/// 真画一张空景图并存盘。见 character_ref_job 上面那段。
json location_ref_job(const std::string& project_path,
                      const std::string& location_id, std::int64_t seed,
                      const std::string& stream_id,
                      const stages::FrameRenderer* shared = nullptr,
                      pipeline::Task* enrolled = nullptr) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, SAYF("没有场景 %1", location_id));
    }

    Location& l = it->second;
    const Rendered out = render_ref(
        store, location_id + "_empty",
        stages::build_location_ref_prompt(l, assets.style),
        stages::ref_negative(assets.style), seed, stream_id, shared, enrolled);

    // 同 character_ref_job：重新读一份，只填这一格，一把锁。理由见那儿。
    std::unique_lock<std::recursive_mutex> store_guard = store.lock();
    AssetLibrary latest = store.load_assets();
    const auto dst = latest.locations.find(location_id);
    if (dst == latest.locations.end()) {
        throw ApiError(409, SAYF("场景 %1 在出图的这几十秒里没了，"
                                 "这张图没记进资产库",
                                 location_id));
    }
    dst->second.ref_empty = out.rel;
    store.save_assets(latest);
    store_guard.unlock();

    return {
        {"saved", out.rel},
        {"seed", out.seed},
        {"width", out.width},
        {"height", out.height},
        {"steps", out.steps},
        {"seconds", out.seconds},
        {"reset_shots", reset_shots_at_location(store, location_id)},
    };
}

}  // namespace

ApiResult post_location_reference_generate(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string location_id = need_str(body, "location_id");
    if (assets.locations.count(location_id) == 0) {
        throw ApiError(404, SAYF("没有场景 %1", location_id));
    }

    const std::string project_path = paths::to_utf8(store.root());
    const std::int64_t seed = ref_seed(body, location_id + "_empty");
    const std::string stream_id = opt_str(body, "stream");

    if (opt_bool(body, "async") && !stream_id.empty()) {
        const std::string target = location_id + "_empty";
        Offload::instance().post([project_path, location_id, seed, stream_id,
                                  target] {
            const JobScope scope{stream_id};   // 同角色那条，见上面
            try {
                job_done(stream_id, location_ref_job(project_path, location_id,
                                                     seed, stream_id));
                ref_done(target);
            } catch (const std::exception& e) {
                job_error(stream_id, e.what());
                ref_error(target, e.what());
            }
        });
        return {202, {{"started", true}, {"stream", stream_id}}};
    }

    return {200, location_ref_job(project_path, location_id, seed, stream_id)};
}

// ---------------------------------------------------------------------------
// 「一键出图」：队列在这儿
// ---------------------------------------------------------------------------

namespace {

struct QueueItem {
    bool character = false;
    std::string id;
    std::string slot;    ///< 角色才有
    std::string label;   ///< 「唐海 正面」，给界面那行用
    std::string target;  ///< `char_id_slot` / `location_id_empty`
    /// 这一件在任务账本上的那一行。**排进队的那一刻就登记**，任务页面
    /// 于是能把还没开始的那几件也列出来（用户 2026-09-17：「排队中的
    /// （预计什么时候开始，取消图标按钮）」）。轮到了 `begin()`。
    std::shared_ptr<pipeline::Task> task;
};

/// 全进程一条队列。
///
/// **一条就够，而且必须只有一条**：同时跑两批等于同一批位置上挤两倍的活，
/// 而位置是按显卡数开的。第二个项目按下去时回 409，界面照着说一句
/// 「另一部电影正在出图」——比让两批互相抢着变慢、谁也说不清好。
struct RefQueue {
    std::mutex mu;
    std::condition_variable cv;

    bool active = false;
    std::string project;          ///< 正在跑的那个项目目录
    /// 谁起的这一批：空 = 页面上那颗按钮，`chat:<编号>` = 那条对话
    ///（`pipeline::chat_lane`）。**对话说「停」只停自己起的**——同出片、写作
    /// 那两样按道分（CLAUDE.md 第十四条）。
    std::string lane;
    std::vector<QueueItem> items;
    std::size_t next = 0;         ///< 下一件派谁
    std::size_t done = 0;
    std::size_t failed = 0;
    std::map<std::string, std::string> running;  ///< target → label
    std::string error;            ///< 第一句报错，原话
    bool by_hand = false;         ///< 是人按的停，不是砸了
    pipeline::CancelToken tok;

    /// 最后一次广播出去的那份，留着给刷新过页面的人补一份。
    json last_state;

    static RefQueue& instance() {
        static RefQueue q;
        return q;
    }

    /// 已经上锁时用。
    json snapshot_locked() const {
        json running_arr = json::array();
        for (const auto& [t, l] : running) {
            running_arr.push_back({{"target", t}, {"label", l}});
        }
        // **还没派出去的那几张也要报名字。**
        //
        // 不报的话，界面上那几格和"根本没排上"长得一模一样——用户
        // 2026-09-17：「明明没有开始的，不是应该显示等待中吗」。
        // 一批至多几十条，每条两个短字符串，播一份不值得心疼。
        //
        // **按了停就一条都不报。** 那几张不会再派出去了，还挂着「等待中」
        // 的话人会一直等它们动。
        json pending_arr = json::array();
        const bool stopping = tok.cancelled();
        if (!stopping) {
            for (std::size_t i = next; i < items.size(); ++i) {
                pending_arr.push_back(
                    {{"target", items[i].target}, {"label", items[i].label}});
            }
        }
        const std::size_t total = items.size();
        const std::size_t settled = done + failed;
        return {
            {"project", project},
            {"active", active},
            {"total", total},
            {"done", done},
            {"failed", failed},
            // **排队的那几张自己算，别让界面去减。** 界面减出来的数在
            // "正在画的那几张还没落进 done" 那一瞬间会是负的。
            {"queued", stopping || total <= settled + running.size()
                           ? std::size_t{0}
                           : total - settled - running.size()},
            // 按了停，手上这几张画完就收——那一行得当场改口，不能等下一张
            // 画完（几十秒）才变。
            {"stopping", stopping},
            {"running", running_arr},
            {"pending", pending_arr},
            {"error", error},
            {"by_hand", by_hand},
        };
    }
};

/// 两个路径指的是不是同一个项目。
///
/// **不能拿字符串直接比。** 队列里存的是 `ProjectStore::root()`（规范化
/// 过的），而问的那一头给的是界面手里那串——macOS 上临时目录
/// `/var/folders/…` 是 `/private/var/folders/…` 的软链，两串字不一样而指
/// 的是同一个地方。比错的后果是"问了等于没问"：这一批明明在跑，快照回的
/// 是「没在跑」，界面于是让人再按一次，回来一句 409。
/// 收到 `paths::same_dir` 上了（出片队列和「正在跑的是不是这部电影」也要
/// 同一条判据）。留个名字是因为这一头的调用点全是「同不同一个项目」的语气。
bool same_project(const std::string& a, const std::string& b) {
    return paths::same_dir(a, b);
}

/// 广播一份，并留底。
void publish(RefQueue& q) {
    json state;
    {
        std::lock_guard<std::mutex> lg(q.mu);
        state = q.snapshot_locked();
        q.last_state = state;
    }
    ref_queue(state);
}

/// 这一批要画哪些。**顺序是固定的**：角色按资产库的顺序、每人三格，
/// 然后是场景。固定顺序才谈得上"接着上次跑"。
std::vector<QueueItem> missing_refs(const AssetLibrary& assets, bool force) {
    static const char* kSlots[] = {"front", "three_quarter", "back"};
    static const char* kSlotNames[] = {SAY_NOOP("正面"),
                                       SAY_NOOP("四分之三侧面"),
                                       SAY_NOOP("背面")};
    std::vector<QueueItem> out;
    for (const auto& [id, c] : assets.characters) {
        for (int i = 0; i < 3; ++i) {
            const std::string slot = kSlots[i];
            const std::optional<std::string>& have =
                slot == "front"          ? c.ref_front
                : slot == "three_quarter" ? c.ref_three_quarter
                                          : c.ref_back;
            if (!force && have.has_value() && !have->empty()) continue;
            QueueItem it;
            it.character = true;
            it.id = id;
            it.slot = slot;
            it.label =
                (c.name.empty() ? id : c.name) + " " + SAY(kSlotNames[i]);
            it.target = id + "_" + slot;
            out.push_back(std::move(it));
        }
    }
    for (const auto& [id, l] : assets.locations) {
        if (!force && l.ref_empty.has_value() && !l.ref_empty->empty()) continue;
        QueueItem it;
        it.character = false;
        it.id = id;
        it.label = l.name.empty() ? id : l.name;
        it.target = id + "_empty";
        out.push_back(std::move(it));
    }
    return out;
}

/// 给这一批每一件登记一行「排队中」。
///
/// **在接口那一帧里登记，不是等派出去才登记**：任务页面要回答的正是
/// 「还排着哪几件」，派出去才登记的话那一栏永远是空的。
void enroll(std::vector<QueueItem>& items, const std::string& project) {
    for (auto& it : items) {
        // ⚠️ **`image` 一词两用**：这儿是参考图（设定那一格），`frames.cpp`
        // 那儿是出首帧（镜头那一格）。`kind` 网页那套在读，改不得，所以
        // 哪一格由起任务的人自己说（见 `util/task_slot.hpp`）。
        it.task = std::make_shared<pipeline::Task>(
            "image", SAYF("画参考图 · %1", it.label), project, std::string{},
            "assets");
        it.task->set_target(it.target);
    }
}

/// 整批跑完为止。**在自己的线程上**，接口早就回去了。
void run_queue(std::string project_path) {
    RefQueue& q = RefQueue::instance();

    // 后端搭一次，整批共用——见 render_ref 里那段。搭不起来（没配模型）
    // 时下面每一张都会自己退回 sd_renderer，报错也是一张一张报。
    int lanes = 1;
    std::shared_ptr<stages::FrameRenderer> shared;
    try {
        ProjectStore store = open_project(project_path);
        config::Settings settings = config::load_settings(store.root());
        if (ref_renderer()) {
            RefBackend b = ref_renderer()(settings, store);
            shared = std::make_shared<stages::FrameRenderer>(std::move(b.render));
            lanes = std::max(1, b.lanes);
        }
    } catch (const std::exception&) {
        // 搭后端这一步失败不该把整批判死：让第一张图去报那句真正的原因。
    }

    const auto worker = [&] {
        for (;;) {
            QueueItem item;
            std::size_t idx = 0;
            {
                std::unique_lock<std::mutex> lk(q.mu);
                if (q.tok.cancelled() || q.next >= q.items.size()) return;
                idx = q.next++;
                item = q.items[idx];
                // **领到一件已经被单独取消的，就地划掉，接着领下一件。**
                // 放手即结账（记成"取消了"），不占位置也不算失败。
                if (item.task && item.task->cancelled()) {
                    q.items[idx].task.reset();
                    item.task.reset();
                    continue;
                }
                q.running[item.target] = item.label;
            }
            // 轮到它了，开始计时。
            if (item.task) item.task->begin();
            publish(q);

            std::string err;
            try {
                // **每一张自己的种子还是按 stem 算**，和单张那条一样：
                // 整批重跑一次得到的还是同一批脸。
                const std::int64_t seed = ref_seed(json::object(), item.target);
                if (item.character) {
                    character_ref_job(project_path, item.id, item.slot, seed, "",
                                      shared.get(), item.task.get());
                } else {
                    location_ref_job(project_path, item.id, seed, "",
                                     shared.get(), item.task.get());
                }
                ref_done(item.target);
            } catch (const std::exception& e) {
                // ApiError 也是 runtime_error，what() 就是那句原话。
                err = e.what();
            }

            if (item.task && !err.empty()) item.task->fail(err);
            {
                std::lock_guard<std::mutex> lg(q.mu);
                q.running.erase(item.target);
                // ⚠️ **这一张干完就结账，别拖到整批收尾。**
                //
                // `Task` 是析构的时候记耗时的。留到整批收尾一起放手的话，
                // 六张图的"耗时"全等于"从自己开工到整批结束"——实测六张
                // 一模一样的 96.9 秒，而它们真实是四五十秒到一分半不等。
                //
                // 两个持有者都要放：队列里那份和手上这份。
                q.items[idx].task.reset();
                if (err.empty()) {
                    q.done += 1;
                } else {
                    q.failed += 1;
                    if (q.error.empty()) {
                        q.error = err;
                        // **人按的停不是失败**，说法不一样：引擎给取消回的
                        // 是「已停下这一张」，报成红的会让人去找哪儿出错了。
                        q.by_hand = util::is_cancel_word(err);
                    }
                    // 第一件砸了就别接着派：后面那些多半栽在同一件事上
                    // （模型没配、显存不够），接着画只是让人多等十几分钟
                    // 再看到同一句话。
                    q.tok.request();
                }
            }
            // 手上这份最后放。放完这一件就进"做完的"了。
            item.task.reset();
            if (!err.empty()) ref_error(item.target, err);
            publish(q);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(lanes));
    for (int i = 0; i < lanes; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    {
        std::lock_guard<std::mutex> lg(q.mu);
        q.active = false;
        q.running.clear();
        // 到这儿还留着 task 的，是**一次都没轮到**的那几件（按了停、或者
        // 前面砸了）。放手即结账，它们记成"取消了"。
        q.items.clear();
    }
    publish(q);
}

}  // namespace

ApiResult post_references_generate_all(const json& body) {
    // 能当场判的先当场判，见 post_character_reference_generate 那段。
    ProjectStore store = open_project(need_str(body, "project"));
    const AssetLibrary assets = store.load_assets();
    const bool force = opt_bool(body, "force");
    const std::string project_path = paths::to_utf8(store.root());

    std::vector<QueueItem> items = missing_refs(assets, force);

    RefQueue& q = RefQueue::instance();
    {
        std::lock_guard<std::mutex> lg(q.mu);
        if (q.active) {
            throw ApiError(409,
                           same_project(q.project, project_path)
                               ? SAY("这一批已经在画了")
                               : SAY("另一部电影正在出图，等它画完再来"));
        }
        if (items.empty()) {
            return {200, {{"total", 0}, {"started", false}}};
        }
        q.project = project_path;
        q.lane = opt_str(body, "lane");
        enroll(items, project_path);
        q.items = std::move(items);
        q.next = 0;
        q.done = 0;
        q.failed = 0;
        q.running.clear();
        q.error.clear();
        q.by_hand = false;
        q.tok.reset();
        q.active = true;
    }
    const std::size_t total = q.items.size();
    publish(q);

    std::thread(run_queue, project_path).detach();
    return {202, {{"total", total}, {"started", true}}};
}

ApiResult post_references_generate_all_stop(const json& body) {
    RefQueue& q = RefQueue::instance();
    const std::string project = opt_str(body, "project");
    {
        std::lock_guard<std::mutex> lg(q.mu);
        if (!q.active) return {200, {{"stopped", false}}};
        if (!project.empty() && !same_project(q.project, project)) {
            return {200, {{"stopped", false}}};
        }
        // 带了道的（对话那头）只停自己那一道起的；不带的是人按的按钮，照停。
        if (body.contains("lane") && opt_str(body, "lane") != q.lane) {
            return {200, {{"stopped", false}}};
        }
        q.tok.request();
        q.by_hand = true;
    }
    // **当场重播一份。** 队列本来是每settle一件才播一次，而一件几十秒——
    // 不播的话按完「停下」那一行还写着「还排着 4 张」，看着像没按上。
    publish(q);
    // 正在画的那几张也要停：它们各自在 render_ref 里查的是自己那条令牌。
    // 这儿只拦住"还没派出去的"，已经在画的那一两张画完就收——一张几十秒，
    // 比留下半张写坏的图强。
    return {200, {{"stopped", true}}};
}

ApiResult get_references_queue(const std::string& project) {
    RefQueue& q = RefQueue::instance();
    std::lock_guard<std::mutex> lg(q.mu);
    if (!q.active && q.last_state.is_null()) {
        return {200, {{"active", false}, {"total", 0}}};
    }
    json state = q.active ? q.snapshot_locked() : q.last_state;
    // 问的是别的项目就别把这一批的数报过去。
    if (!project.empty() &&
        !same_project(state.value("project", std::string{}), project)) {
        return {200, {{"active", false}, {"total", 0}}};
    }
    return {200, state};
}

}  // namespace changji::http
