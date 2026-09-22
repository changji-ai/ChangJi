#include "infer/task_run.hpp"

#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "infer/blob.hpp"
#include "infer/capability.hpp"
#include "infer/node_status.hpp"
#include "infer/sd_video.hpp"
#include "llm/client.hpp"
#include "stages/tts_backends.hpp"
#include "media/ffmpeg.hpp"
#include "models/shot.hpp"
#include "setup/catalog.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"
#include "util/say.hpp"
#include "util/paths.hpp"

namespace changji::infer {

std::optional<stages::TTSBackend> make_tts_backend(
    const config::Settings& s, const std::optional<media::FFmpeg>& ff) {
    if (s.tts.backend == "http" && s.tts.base_url.has_value() &&
        !s.tts.base_url->empty()) {
        return stages::http_tts_backend(*s.tts.base_url, 300.0,
                                        llm::default_http_post(), ff);
    }
    if (s.tts.backend == "local") {
        // 模型路径在 [models] 里——那一节本来就是 C++ 侧独有的。
        std::string why;
        const auto ws = s.workspace_path();
        auto local = stages::local_tts_backend(
            s.models.resolve(s.models.tts, ws),
            s.models.resolve(s.models.tts_decoder, ws),
            /*use_gpu=*/true, ff, why);
        if (local.has_value()) return local;
        // 载不起来就退回估算后端。**这儿不写日志**：这个文件没有日志
        // 设施，而用户看得见的地方有两处已经覆盖（配音阶段的 start 事件
        // 会报后端名字，/api/doctor 的「进程内配音」那一项查的就是这两个
        // 模型路径）。
    }
    return std::nullopt;
}

namespace {

/// 这一趟活按哪份配置跑。
///
/// **放在这一层、而不是让调用方各覆各的。** 覆这一层的地方只能有一个：
/// `cannot_do` 按 A 判、`run_task_locally` 按 B 跑的话，表现是"自检说能干、
/// 真跑起来说模型没配"——而那时候用户看到的是一镜失败，他会去查网络和
/// 模型，最后才想到是两处各覆了一份。这和这个文件头上那条
/// "抄一遍的话两边迟早走散"是同一件事。
config::Settings settings_for(const Task& t, const config::Settings& s) {
    const config::Settings picked = setup::with_selections(s, t.pick);

    // **盖不上就用这台自己装的那一档，别把活退回去。**
    //
    // `t.pick` 是派活方那部电影挑的档位 id，这台按自己的模型目录解析。
    // 解析不出来（这台装的是同一族的另一个量化档）时，原来是把这一镜
    // 拒掉，回一句"去设置页给这台补上这一档"——于是整条流水线停在那儿，
    // 而这台机器明明装着一份能用的权重。
    //
    // 一致性的代价是真的（同一族不同量化档，画面会有细微差别），但
    // **一镜用邻近档位渲出来，比一镜空着强得多**；而且不补这一句的话，
    // 用户看到的是一个把失败原因解释得很清楚、却一张图都出不来的程序。
    //
    // 只在"盖了跑不成、不盖跑得成"时退回，其余情况原样用盖过的那份——
    // 无条件退回会让派活方的选择彻底失效。
    for (const Capability cap :
         {Capability::Video, Capability::Frame, Capability::Tts}) {
        if (missing_for(cap, probe_facts(picked)).empty()) continue;
        if (missing_for(cap, probe_facts(s)).empty()) return s;
    }
    return picked;
}

}  // namespace

std::string cannot_do(const Task& t, const config::Settings& base) {
    const config::Settings s = settings_for(t, base);
    // **判据和那张「机器 × 能力」的表共用一份**（infer/capability.hpp）。
    // 两处各写一份的话，迟早出现"表上说能干，派过去却被拒"——而那时候
    // 用户看到的是一镜失败，他会先去查网络和模型，最后才想到是两份规则
    // 走散了。
    const NodeFacts facts = probe_facts(s);
    const Capability cap = t.kind == TaskKind::Video  ? Capability::Video
                           : t.kind == TaskKind::Tts  ? Capability::Tts
                                                      : Capability::Frame;
    if (const auto why = missing_for(cap, facts); !why.empty()) {
        // **这台可能装着模型，只是不是这部电影要的那一档。**
        // 不分开说的话，用户看到的是"这台没配模型"——而他明明在那台上
        // 装过、`/status` 上那一格也是亮的，接着就会去查网络和口令。
        // 判据是「不盖这部电影那一层就干得成」。
        const auto group = cap == Capability::Video  ? "video"
                           : cap == Capability::Tts  ? "tts"
                                                     : "image";
        const auto want = t.pick.find(group);
        if (want != t.pick.end() && !want->second.empty() &&
            missing_for(cap, probe_facts(base)).empty()) {
            return SAYF("%1。这台装的是别的档——这部电影挑的是「%2」，"
                        "去设置页给这台补上这一档，或者给这部电影换一档",
                        why, want->second);
        }
        return why;
    }

    // 产物目录得写得进去。
    //
    // **跨机时不查这个**：那时 dest 是派活方的路径，这台上根本没有那个
    // 目录，去建它是凭空在别人的盘上造目录树。产物落沙箱，沙箱由
    // run_task_locally 现建。
    if (t.return_artifact) return {};
    std::error_code ec;
    const auto dir =
        std::filesystem::path(paths::from_utf8(t.dest)).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return SAYF("产物目录建不出来：%1", paths::to_utf8(dir));
    }
    return {};
}

TaskResult run_task_locally(const Task& t, const config::Settings& base,
                            Origin origin, const std::string& task_id,
                            const StepCallback& on_step,
                            pipeline::CancelToken& tok) {
    // 这部电影挑的档位盖在这台自己的配置上。**盖的是文件名，不是路径**——
    // 模型目录仍然是这台的（见 setup::with_selections）。
    const config::Settings s = settings_for(t, base);
    // 槽装模型时看这份（盖了派活方挑的档位），不看 runtime 那份。
    const ScopedTaskSettings scoped(s);
    TaskResult result;
    const auto cache = cache_root_of(s.workspace_path());
    const auto sandbox = task_sandbox(cache, task_id);
    try {
        // 产物落哪：跨机时落沙箱（dest 是对面的路径，这儿没有），
        // 同机直接写 dest（一个文件系统，省一次搬运）。
        //
        // **文件名要照抄对面那个**：png 和 mp4 是靠扩展名分的，
        // 改了名字下一步 ffmpeg 就不认。
        std::filesystem::path dest = paths::from_utf8(t.dest);
        if (t.return_artifact) {
            std::error_code ec;
            std::filesystem::create_directories(sandbox, ec);
            if (ec) {
                throw std::runtime_error(
                    SAYF("建不出沙箱：%1", paths::to_utf8(sandbox)));
            }
            const auto name = dest.filename();
            dest = sandbox / (name.empty() ? std::filesystem::path("out")
                                           : name);
        }

        models::Shot shot;
        shot.shot_id = t.shot_id;

        if (t.kind == TaskKind::Tts) {
            const media::FFmpeg ff(s.assembly.ffmpeg_path,
                                   s.assembly.ffprobe_path,
                                   media::default_runner());
            auto backend = make_tts_backend(s, ff);
            if (!backend) {
                // **这儿不退回估算后端。** 派活方要的是真声音，
                // 给它一段静音而且说"成功了"，是这条链路上最阴的故障——
                // 整章配完才发现没声音。让它换一台。
                throw std::runtime_error(
                    SAYF("这台的配音后端搭不起来（[tts].backend = %1）",
                         s.tts.backend));
            }
            const auto r = backend->synthesize(
                t.text, dest,
                t.voice_id.empty() ? std::optional<std::string>()
                                   : std::optional<std::string>(t.voice_id),
                t.emotion, t.intensity);
            result.duration_s = r.duration_s;
        } else if (t.kind == TaskKind::Frame) {
            // 参考图可能是 blob: 记法（跨机）也可能是路径（同机），
            // resolve_input 认两种。
            stages::PromptBundle prompts = t.prompts;
            for (auto& r : prompts.reference_images) {
                r = paths::to_utf8(resolve_input(cache, r));
            }
            // **种子是派活方算好的。** 这边算不了：`frame_seed` 要
            // `attempts`，而这儿拿不到那个数。
            stages::sd_renderer_with_seed(s, t.seed, origin)(
                shot, prompts, t.spec, dest, tok, on_step);
        } else {
            stages::RenderPlan plan;
            plan.shot_id = t.shot_id;
            plan.tier = t.tier;
            plan.spec = t.spec;
            // 步数由这台按自己有没有 Turbo LoRA 重定，见 steps_on_node。
            plan.spec.steps =
                config::steps_on_node(s, t.spec.steps, t.spec.steps_pinned);
            plan.frames = t.frames;
            plan.prompts = t.prompts;
            plan.motion = t.motion;
            plan.style_line = t.style_line;
            plan.keep_ambient = t.keep_ambient;
            if (t.end_image) plan.end_image = resolve_input(cache, *t.end_image);
            std::optional<std::filesystem::path> start;
            if (t.start_image) start = resolve_input(cache, *t.start_image);
            sd_video_renderer_with_seed(s, t.seed, origin)(
                shot, plan, start, dest, tok, on_step);
        }
        result.ok = true;
        result.dest = paths::to_utf8(dest);
        if (t.return_artifact) {
            // 收进 blob 仓库，回一个指纹。派活方拿它去 GET /blob/<id>。
            // **产物和输入共用一个仓库**：重跑一镜出来字节相同的话，
            // 第二次连传都不用传。
            result.artifact_id = blob_adopt(cache, dest);
        }
    } catch (const std::exception& e) {
        result.ok = false;
        // 这句会一路变成派活方事件流里的那条 warn，所以要能直接给用户看。
        result.error = e.what();
    }
    return result;
}

}  // namespace changji::infer
