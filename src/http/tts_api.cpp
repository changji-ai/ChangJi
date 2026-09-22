#include "http/tts_api.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include "config/runtime.hpp"
#include "http/job_stream.hpp"
#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "stages/audio.hpp"
#include "stages/tts_backends.hpp"
#include "pipeline/activity.hpp"
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
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// 按 `[tts].backend` 搭一个后端。和出片那一步同一套选法。
///
/// 搭不起来就退回估算后端——那会写出一段等长静音。在出片那条路上这是
/// 刻意的（画面那几步照样能验），但在"念给我听"这件事上没有意义，
/// 所以响应里把后端名字回出去，界面照实说。
}  // namespace

ApiResult post_tts_say(const json& body) {
    const std::string project_path = need_str(body, "project");
    if (project_path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    ProjectStore store(paths::from_utf8(project_path));

    const std::string raw = text::strip_ws(need_str(body, "text"));
    if (raw.empty()) throw ApiError(400, SAY("没有要念的字"));

    // **超了就念前面那一段，并且明说。** 静默截断的话，用户以为听完了整段，
    // 而实际后半截根本没念。
    const std::string said = text::truncate_utf8(raw, kSayMaxChars);
    const bool truncated = said.size() != raw.size();

    const config::Settings s = config::runtime().snapshot();
    const auto ff = media::FFmpeg(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                  media::default_runner());
    const stages::TTSBackend backend =
        stages::pick_tts_backend(s, ff, llm::default_http_post());
    if (!backend.synthesize) throw ApiError(503, SAY("配音后端没准备好"));

    // 落点：项目的 audio/ 下面一个固定名字。**固定名字是刻意的**——朗读是
    // 随手点的，一天点几十次；按内容起名的话 audio/ 里会堆满再也用不上的
    // wav，而它们和镜头配音混在同一个目录里，看着像是出了一堆废文件。
    std::error_code ec;
    const fs::path dir = store.paths().audio();
    fs::create_directories(dir, ec);
    const fs::path out = dir / "say.wav";

    // 念一段几秒钟，但它要借配音那一槽——而那一槽和大模型抢同一张卡。
    // 顶栏那本账上要看得见，否则别的活被它挡住时没人知道是谁挡的。
    pipeline::Activity act{"say", paths::to_utf8(store.root()), "",
                           SAY("正在朗读")};

    // **按了停就真的别再念出来。**
    //
    // 朗读走 start_async（server.cpp 里 /api/tts/say 那条），于是顶栏那块
    // 「正在思考」上有一个「停下」——按下去 `/api/job/cancel` 在表里**找得
    // 到**这条 stream（JobScope 是 start_async 挂的），回 {stopped:true}，
    // 按钮跟着变成「停着…」。而这儿原来一处都没查过令牌：几十秒后那段音频
    // 照样冒出来、还自动播。**按了停还响，比压根没有那个按钮更糟。**
    //
    // 合成是一次阻塞调用（后端可能是本机模型，也可能是一条 HTTP 请求），
    // 中途插不进检查点，所以只能卡两头：
    //   · 进来先看一眼——排在 Offload 队列里、以及下面借槽读权重那一段，
    //     可能几十秒，而人正是在这一段里按的停；
    //   · 出来再看一眼——念完了也别把结果送回去，不然前一句白查。文件已经
    //     落盘不要紧：say.wav 是个固定名字（见上面那段），下次朗读就盖掉。
    //
    // 同步那条路（老客户端、curl、对拍）一个字没变：没有 JobScope 时
    // `current_cancel()` 回的是哑元，永远不是 cancelled。
    pipeline::CancelToken& tok = current_cancel();
    // 取消回 400，和出图、大模型那两族一致（「已停下这一张」）——人按的停
    // 不是失败，报成 5xx 会让人去找哪儿出错了。
    if (tok.cancelled()) throw ApiError(400, SAY("已停下，这一段没念"));

    stages::SynthesisResult res;
    try {
        const std::string voice = opt_str(body, "voice");
        // **和出片那条走同一个还原。** 角色上传的参考音色存成
        // `voices/c_xxx.wav`（相对项目根），直接喂给合成器会解析到引擎
        // 进程的当前目录。两处各写一份的话迟早只改一边，那种 bug 的表现
        // 是"镜头里能用、试听不出声"。见 stages/audio.hpp 里 resolve_voice。
        const auto voice_arg = stages::resolve_voice(
            voice.empty() ? std::nullopt : std::optional(voice), store.paths());
        res = backend.synthesize(said, out, voice_arg, "", 1.0);
    } catch (const std::exception& e) {
        throw ApiError(502, SAYF("念不出来：%1", e.what()));
    }
    if (tok.cancelled()) throw ApiError(400, SAY("已停下，这一段没念"));

    return {200, {
        {"rel", store.paths().rel(out)},
        {"seconds", res.duration_s},
        {"chars", static_cast<int>(text::utf8_len(said))},
        {"truncated", truncated},
        // estimate 那条出来的是静音。界面照实说，别让人对着一段没声音的
        // 音频以为是自己音箱坏了。
        {"backend", backend.name},
    }};
}

}  // namespace changji::http
