#include "infer/worker_proto.hpp"

#include <stdexcept>

#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::infer {

using nlohmann::json;

const char* to_string(TaskKind k) {
    switch (k) {
        case TaskKind::Video: return "video";
        case TaskKind::Tts: return "tts";
        case TaskKind::Frame: break;
    }
    return "frame";
}

std::optional<TaskKind> task_kind_from(const std::string& s) {
    if (s == "frame") return TaskKind::Frame;
    if (s == "video") return TaskKind::Video;
    if (s == "tts") return TaskKind::Tts;
    return std::nullopt;
}

namespace {

/// 取必填字段。**缺了就抛，别用默认值糊过去**——
/// 一个没带 dest 的任务默默写到当前目录去，比当场报错难查得多。
const json& need(const json& j, const char* key) {
    if (!j.is_object()) throw std::runtime_error(SAY("任务不是一个对象"));
    const auto it = j.find(key);
    if (it == j.end()) {
        throw std::runtime_error(SAYF("任务里缺 %1", key));
    }
    return *it;
}

}  // namespace

json to_json(const Task& t) {
    json j{
        {"kind", to_string(t.kind)},
        {"shot_id", t.shot_id},
        {"prompts",
         json{{"positive", t.prompts.positive},
              {"negative", t.prompts.negative},
              // 视频那份负向词和画面/风格层：2026-09-16 之前不在协议里，
              // 派出去的出片任务负向词是空的、正向词退回整段外观。
              {"negative_video", t.prompts.negative_video},
              {"video_scene", t.prompts.video_scene},
              {"style_layer", t.prompts.style_layer},
              {"reference_images", t.prompts.reference_images},
              {"base_model", t.prompts.base_model}}},
        {"spec", t.spec},
        {"frames", t.frames},
        {"motion", t.motion},
        {"style_line", t.style_line},
        {"tier", t.tier},
        {"dest", t.dest},
        {"seed", t.seed},
        {"return_artifact", t.return_artifact},
        {"text", t.text},
        {"voice_id", t.voice_id},
        {"emotion", t.emotion},
        {"intensity", t.intensity},
    };
    if (t.start_image) j["start_image"] = *t.start_image;
    if (t.end_image) j["end_image"] = *t.end_image;
    if (t.keep_ambient) j["keep_ambient"] = *t.keep_ambient;
    if (t.prompts.seed_override) j["prompts"]["seed_override"] = *t.prompts.seed_override;
    // **空的就不发。** 老版本的工作进程会把认不得的键原样忽略，
    // 但请求体里多一个空对象这件事在日志里读着像"挑了、挑空了"。
    if (!t.pick.empty()) j["pick"] = t.pick;
    return j;
}

Task task_from_json(const json& j) {
    Task t;
    const auto kind = task_kind_from(need(j, "kind").get<std::string>());
    if (!kind) {
        throw std::runtime_error(SAY("任务的 kind 只能是 frame / video / tts"));
    }
    t.kind = *kind;
    t.shot_id = need(j, "shot_id").get<std::string>();
    t.dest = need(j, "dest").get<std::string>();

    const auto& p = need(j, "prompts");
    t.prompts.positive = p.value("positive", std::string());
    t.prompts.negative = p.value("negative", std::string());
    t.prompts.negative_video = p.value("negative_video", std::string());
    t.prompts.video_scene = p.value("video_scene", std::string());
    t.prompts.style_layer = p.value("style_layer", std::string());
    if (p.contains("reference_images") && p["reference_images"].is_array()) {
        t.prompts.reference_images =
            p["reference_images"].get<std::vector<std::string>>();
    }
    // 老版本的派活方不带：那时候没有"画参考图派出去"这回事，按首帧走
    t.prompts.base_model = p.value("base_model", false);
    if (p.contains("seed_override") && p["seed_override"].is_number_integer()) {
        t.prompts.seed_override = p["seed_override"].get<std::int64_t>();
    }

    t.spec = need(j, "spec").get<models::TierSpec>();
    t.frames = j.value("frames", 0);
    t.motion = j.value("motion", std::string());
    if (j.contains("style_line")) {
        t.style_line = j["style_line"].get<models::StyleLine>();
    }
    if (j.contains("tier")) t.tier = j["tier"].get<models::Tier>();
    if (j.contains("start_image") && j["start_image"].is_string()) {
        t.start_image = j["start_image"].get<std::string>();
    }
    if (j.contains("end_image") && j["end_image"].is_string()) {
        t.end_image = j["end_image"].get<std::string>();
    }
    if (j.contains("keep_ambient") && j["keep_ambient"].is_boolean()) {
        t.keep_ambient = j["keep_ambient"].get<bool>();
    }
    // **种子必须带**。让工作进程自己算的话它不知道 attempts，
    // 算出来的图和串行跑的不一样——那样并行就不是"更快"，是"结果变了"。
    t.seed = need(j, "seed").get<std::int64_t>();
    // **默认假**：老版本的派活方不带这个字段，那时候它要的就是老行为
    // （直接写 dest，两头共享文件系统）。
    t.return_artifact = j.value("return_artifact", false);
    // **没有、或者形状不对，都当"没挑"。** 派活那头版本比这台老时就没有
    // 这个键，那时候它要的就是老行为：这台按自己 `[models]` 里的文件名跑。
    if (const auto it = j.find("pick"); it != j.end() && it->is_object()) {
        for (const auto& [group, id] : it->items()) {
            if (id.is_string()) t.pick[group] = id.get<std::string>();
        }
    }
    t.text = j.value("text", std::string());
    t.voice_id = j.value("voice_id", std::string());
    t.emotion = j.value("emotion", std::string());
    t.intensity = j.value("intensity", 0.0);

    // **在入口处判掉非法 UTF-8。** nlohmann 解析时照单全收，dump 时才抛
    // type_error.316——那时候已经出了这儿的 try 块，派活方拿到的是一个
    // 空白的 500。跨机时两头编码不一样是迟早的事（2026-09-12 实撞：
    // 请求体是 GBK 的中文台词，日志里只有一行 invalid UTF-8 byte）。
    for (const auto& [what, s] : std::vector<std::pair<const char*, const std::string*>>{
             {"shot_id", &t.shot_id},
             {"dest", &t.dest},
             {"text", &t.text},
             {"voice_id", &t.voice_id},
             {"emotion", &t.emotion},
             {"motion", &t.motion},
             {"prompts.positive", &t.prompts.positive},
             {"prompts.negative", &t.prompts.negative},
             {"prompts.negative_video", &t.prompts.negative_video},
             {"prompts.video_scene", &t.prompts.video_scene},
             {"prompts.style_layer", &t.prompts.style_layer},
         }) {
        if (!text::is_valid_utf8(*s)) {
            throw std::runtime_error(
                SAYF("%1 不是合法的 UTF-8。派活那头多半没按 UTF-8 编码"
                     "（Windows 上用 GBK 发中文就会这样）",
                     what));
        }
    }
    return t;
}

json to_json(const TaskResult& r) {
    return json{{"ok", r.ok},
                {"error", r.error},
                {"dest", r.dest},
                {"artifact_id", r.artifact_id},
                {"duration_s", r.duration_s}};
}

TaskResult task_result_from_json(const json& j) {
    TaskResult r;
    r.ok = j.value("ok", false);
    r.error = j.value("error", std::string());
    r.dest = j.value("dest", std::string());
    r.artifact_id = j.value("artifact_id", std::string());
    r.duration_s = j.value("duration_s", 0.0);
    return r;
}

json to_json(const TaskProgress& p) {
    json j{{"state", p.state},
           {"step", p.step},
           {"steps", p.steps},
           {"phase", p.phase}};
    if (p.preview_step >= 0) j["preview_step"] = p.preview_step;
    if (!p.preview.empty()) j["preview"] = p.preview;
    if (p.result) j["result"] = to_json(*p.result);
    return j;
}

TaskProgress task_progress_from_json(const json& j) {
    TaskProgress p;
    p.state = j.value("state", std::string("queued"));
    p.step = j.value("step", 0);
    p.steps = j.value("steps", 0);
    p.phase = j.value("phase", std::string("sample"));
    p.preview_step = j.value("preview_step", -1);
    p.preview = j.value("preview", std::string());
    if (j.contains("result") && j["result"].is_object()) {
        p.result = task_result_from_json(j["result"]);
    }
    return p;
}

std::string worker_rejected_message(int status, const std::string& body) {
    // 截 200 字符。**按字符，不按字节**——见头文件里那段。
    return SAYF("工作进程拒了这个任务（%1）：%2", std::to_string(status),
                text::truncate_utf8(body, 200));
}

std::string worker_bad_accept_message(const std::string& body) {
    return SAYF("工作进程回的不是 {\"id\":...}：%1",
                text::truncate_utf8(body, 200));
}


std::uint64_t room_seq_of(const std::string& reject_body) {
    const auto js = nlohmann::json::parse(reject_body, nullptr, false);
    if (js.is_discarded() || !js.is_object()) return 0;
    const auto it = js.find("seq");
    if (it == js.end() || !it->is_number_unsigned()) return 0;
    return it->get<std::uint64_t>();
}

bool room_freed(const std::string& wait_body, std::uint64_t asked_seq,
                std::uint64_t* now_seq) {
    const auto js = nlohmann::json::parse(wait_body, nullptr, false);
    if (js.is_discarded() || !js.is_object()) return false;
    std::uint64_t seq = asked_seq;
    if (const auto it = js.find("seq");
        it != js.end() && it->is_number_unsigned()) {
        seq = it->get<std::uint64_t>();
    }
    if (now_seq != nullptr) *now_seq = seq;
    int free_now = 0;
    if (const auto it = js.find("free"); it != js.end() && it->is_number()) {
        free_now = it->get<int>();
    }
    return free_now > 0 || seq != asked_seq;
}

std::string task_key(const Task& t) {
    nlohmann::json j = to_json(t);
    // 派活方本机的落盘路径，和内容无关。见头文件。
    j.erase("dest");
    // 要不要把产物传回来，是"怎么取"不是"是什么"。
    j.erase("return_artifact");
    return text::sha1_hex(j.dump());
}

Orphan orphan_check(bool settled, std::chrono::seconds since_seen,
                    std::chrono::seconds running_for) {
    using namespace std::chrono;
    if (settled) {
        return since_seen > hours(1) ? Orphan::DropSettled : Orphan::Keep;
    }
    return running_for > minutes(30) ? Orphan::CancelStuck : Orphan::Keep;
}

}  // namespace changji::infer
