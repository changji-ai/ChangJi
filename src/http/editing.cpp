#include "http/editing.hpp"

#include <set>
#include <string>
#include <vector>

#include "models/project.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// 取一个必填的字符串字段。缺了或类型不对都是 400 而不是 500。
std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, SAYF("请求里缺少字符串字段 %1", key));
    }
    return body.at(key).get<std::string>();
}

/// ShotPatch 允许出现的字段。
///
/// 对应 Python 那边 model_config = {"extra": "forbid"}。不做这个检查的话，
/// 前端把字段名拼错了会静默不生效——用户改了半天看不到变化，
/// 而后端一句话都没说。
const std::set<std::string>& patch_allowed() {
    static const std::set<std::string> kAllowed = {
        "first_frame_prompt", "motion_prompt", "negative_prompt", "visual_desc",
        "shot_size", "camera_angle", "camera_move", "duration_s",
        "transition_in", "transition_dur_s", "subtitle_text", "beat",
        "needs_lipsync", "status", "dialogue_texts",
    };
    return kAllowed;
}

/// 改了这些就得重出画面。
const std::set<std::string>& visual_keys() {
    static const std::set<std::string> kVisual = {
        "first_frame_prompt", "motion_prompt", "negative_prompt",
        "shot_size", "camera_angle", "camera_move", "duration_s",
    };
    return kVisual;
}

/// 把 JSON 里的枚举字符串转成枚举。认不出来是 400。
///
/// Python 那边靠 pydantic 的 model_validate 做这件事，转不了会抛
/// ValidationError，被外层翻成 400 "改动不合法：..."。这里要给出同样的效果。
template <typename E>
E parse_enum(const json& v, const char* field) {
    if (!v.is_string()) {
        throw ApiError(400, SAYF("改动不合法：%1 要是字符串", field));
    }
    // nlohmann 的枚举反序列化在认不出取值时会**静默回落到表里第一项**，
    // 不会报错。所以要自己回头验一次：转出来再转回字符串，对不上就是非法值。
    const E e = v.get<E>();
    if (std::string(to_string(e)) != v.get<std::string>()) {
        throw ApiError(400, SAYF("改动不合法：%1 不认识这个取值 %2", field,
                                 v.get<std::string>()));
    }
    return e;
}

double need_num(const json& v, const char* field) {
    if (!v.is_number()) {
        throw ApiError(400, SAYF("改动不合法：%1 要是数字", field));
    }
    return v.get<double>();
}

std::string need_string(const json& v, const char* field) {
    if (!v.is_string()) {
        throw ApiError(400, SAYF("改动不合法：%1 要是字符串", field));
    }
    return v.get<std::string>();
}

/// 把 patch 应用到镜头的副本上。抛异常时调用方手里的原对象一个字段没动。
void apply_patch(Shot& s, const json& patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const std::string& k = it.key();
        const json& v = it.value();

        // Python 那边 patch 是 model_dump(exclude_none=True)，
        // 值为 null 的字段压根不会出现。这里显式跳过，效果相同。
        if (v.is_null()) continue;

        if (patch_allowed().count(k) == 0) {
            // 注意是 422 不是 400：这是 pydantic 的 extra="forbid" 违规，
            // FastAPI 走的是校验错误那条路。对拍语料抓到过我这里写成 400。
            throw unprocessable("patch", k, "Extra inputs are not permitted",
                                v, "extra_forbidden");
        }

        if (k == "first_frame_prompt")      s.first_frame_prompt = need_string(v, "first_frame_prompt");
        else if (k == "motion_prompt")      s.motion_prompt = need_string(v, "motion_prompt");
        else if (k == "negative_prompt")    s.negative_prompt = need_string(v, "negative_prompt");
        else if (k == "visual_desc")        s.visual_desc = need_string(v, "visual_desc");
        else if (k == "subtitle_text")      s.subtitle_text = need_string(v, "subtitle_text");
        else if (k == "beat")               s.beat = need_string(v, "beat");
        else if (k == "shot_size")          s.shot_size = parse_enum<ShotSize>(v, "shot_size");
        else if (k == "camera_angle")       s.camera_angle = parse_enum<CameraAngle>(v, "camera_angle");
        else if (k == "camera_move")        s.camera_move = parse_enum<CameraMove>(v, "camera_move");
        else if (k == "transition_in")      s.transition_in = parse_enum<Transition>(v, "transition_in");
        else if (k == "status")             s.status = parse_enum<ShotStatus>(v, "status");
        else if (k == "duration_s")         s.duration_s = need_num(v, "duration_s");
        else if (k == "transition_dur_s")   s.transition_dur_s = need_num(v, "transition_dur_s");
        else if (k == "needs_lipsync") {
            if (!v.is_boolean()) throw ApiError(400, SAY("改动不合法：needs_lipsync 要是真假值"));
            s.needs_lipsync = v.get<bool>();
        }
        // dialogue_texts 不在这里处理，它有自己的连带效果，见下面
    }
}

}  // namespace

ApiResult post_shot(const json& body) {
    const std::string project_path = need_str(body, "project");
    const std::string episode_id = need_str(body, "episode_id");
    const std::string shot_id = need_str(body, "shot_id");

    if (project_path.empty()) throw ApiError(400, SAY("没有指定项目目录"));

    json patch = body.contains("patch") ? body.at("patch") : json::object();
    if (!patch.is_object()) throw ApiError(400, SAY("patch 要是一个对象"));

    ProjectStore store(paths::from_utf8(project_path));
    Project project = store.load_project();

    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, SAYF("没有章节 %1", episode_id));
    Shot* shot = ep->shot_by_id(shot_id);
    if (shot == nullptr) throw ApiError(404, SAYF("没有镜头 %1", shot_id));

    // 台词单独拎出来，它不是普通字段
    std::vector<std::string> texts;
    bool has_texts = false;
    if (patch.contains("dialogue_texts") && !patch.at("dialogue_texts").is_null()) {
        const json& t = patch.at("dialogue_texts");
        if (!t.is_array()) throw ApiError(400, SAY("改动不合法：dialogue_texts 要是数组"));
        for (const auto& x : t) {
            if (!x.is_string()) throw ApiError(400, SAY("改动不合法：台词要是字符串"));
            texts.push_back(x.get<std::string>());
        }
        has_texts = true;
    }
    patch.erase("dialogue_texts");

    // 改了这些就得重出画面。
    //
    // ⚠️ **这是「调用方设过这个键」的语义，不是「值变了」**：下面只看键在
    // 不在、值非 null，不和当前值比。Python 那边 patch 是
    // `model_dump(exclude_none=True)`，只有真设过的字段才会出现，所以两者
    // 等价——照抄的是那套语义，别顺手改成比值（对拍语料按现在这样钉着）。
    //
    // 代价是调用方必须守约。界面 2026-09-15 之前把十三个字段整份发过来，
    // 于是每一次保存都必然 touched_visual：改个字幕错别字也把这一镜退回
    // PLANNED、解掉锁、下一轮整个重渲染。现在它只送真改过的那几项
    // （EpShots 的 saveShot）。
    //
    // 资产那两条（角色、场景）不一样，它们是**比值**的——见
    // editing_assets.cpp 的 changed()。两边不同不是疏忽：那边一份 patch
    // 里塞的是整份设定，而这边是一次编辑。
    bool touched_visual = false;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (!it.value().is_null() && visual_keys().count(it.key()) != 0) {
            touched_visual = true;
            break;
        }
    }
    const bool patch_has_status =
        patch.contains("status") && !patch.at("status").is_null();

    // 先改副本。校验不过就整个丢掉，原对象一个字段都没动——
    // Python 那边 model_validate 出新对象再赋回，也是这个性质。
    Shot draft = *shot;
    apply_patch(draft, patch);

    if (has_texts) {
        if (texts.size() != draft.dialogue.size()) {
            throw ApiError(400,
                           SAYF("台词条数对不上：给了 %1 条，这个镜头有 %2 条",
                                std::to_string(texts.size()),
                                std::to_string(draft.dialogue.size())));
        }
        for (std::size_t i = 0; i < texts.size(); ++i) {
            if (draft.dialogue[i].text == texts[i]) continue;
            draft.dialogue[i].text = texts[i];
            // 台词变了，配音和时长都要重做
            draft.dialogue[i].actual_duration_s = std::nullopt;
            draft.dialogue[i].audio_path = std::nullopt;
            draft.duration_locked = false;
            touched_visual = true;
        }
    }

    const auto errs = draft.validate();
    if (!errs.empty()) {
        throw ApiError(400, SAYF("改动不合法：%1", errs.front()));
    }

    const bool reset = touched_visual && !patch_has_status;
    if (reset) {
        draft.status = ShotStatus::PLANNED;
        draft.attempts = 0;
        draft.gate_notes.clear();
    }

    *shot = std::move(draft);
    store.save_project(project);

    return {200, {
        {"saved", true},
        {"reset_to_planned", reset},
        {"status", to_string(shot->status)},
    }};
}

}  // namespace changji::http
