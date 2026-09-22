#include "util/say.hpp"
#include "models/shot.hpp"

#include <algorithm>

namespace changji::models {

namespace {

/// UTF-8 字符数，不是字节数。
///
/// 这是移植 pydantic `max_length` 时必须处理的一处：Python 的 str 长度数的是
/// 字符，一个汉字算 1；C++ 的 std::string::size() 数字节，一个汉字算 3。
/// 直接用 size() 会把 "visual_desc 最多 300 字" 变成 "最多 100 字"，
/// 而且只在中文内容上出错——英文语料测不出来。对拍语料里必须有中文，
/// 原因之一就是这个。
///
/// 只数首字节不是延续字节（10xxxxxx）的字节数即可，不需要完整解码。
std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

/// 对应 pydantic 的 pattern=r"^[a-z0-9_]+$"。
/// 不引 <regex>：这条规则简单到不值得付那个编译期和运行时开销。
bool is_slug(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

/// 数字转字符串，去掉 std::to_string 那一串没用的尾零。
///
/// 默认行为是 99.000000，出现在给用户看的错误消息里很难受。
std::string num(double v) {
    std::string s = std::to_string(v);
    if (s.find('.') == std::string::npos) return s;
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

void check_len(const std::string& value, std::size_t max_chars,
               const std::string& field, std::vector<std::string>& errs) {
    const std::size_t n = utf8_len(value);
    if (n > max_chars) {
        errs.push_back(SAYF("%1 超长：%2 字，最多 %3 字", field,
                            std::to_string(n), std::to_string(max_chars)));
    }
}

}  // namespace

// ── 枚举转字符串 ───────────────────────────────────────────────────────

bool status_has_film(ShotStatus s) {
    // DRAFT_DONE 也算：只跑草稿档验叙事时，那一档就是成品
    // （和 `pipeline::assembly_usable` 同一句话）。
    switch (s) {
        case ShotStatus::FINAL_DONE:
        case ShotStatus::DRAFT_DONE:
        case ShotStatus::FALLBACK:
        case ShotStatus::LOCKED:
            return true;
        default:
            return false;
    }
}

const char* to_string(ShotSize v) {
    switch (v) {
        case ShotSize::ECU: return "ECU";
        case ShotSize::CU:  return "CU";
        case ShotSize::MCU: return "MCU";
        case ShotSize::MS:  return "MS";
        case ShotSize::MLS: return "MLS";
        case ShotSize::LS:  return "LS";
        case ShotSize::ELS: return "ELS";
    }
    return "?";
}

const char* to_string(CameraAngle v) {
    switch (v) {
        case CameraAngle::LOW:       return "low";
        case CameraAngle::EYE_LEVEL: return "eye_level";
        case CameraAngle::HIGH:      return "high";
        case CameraAngle::OVERHEAD:  return "overhead";
        case CameraAngle::DUTCH:     return "dutch";
    }
    return "?";
}

const char* to_string(CameraMove v) {
    switch (v) {
        case CameraMove::STATIC:    return "static";
        case CameraMove::PAN_LEFT:  return "pan_left";
        case CameraMove::PAN_RIGHT: return "pan_right";
        case CameraMove::TILT_UP:   return "tilt_up";
        case CameraMove::TILT_DOWN: return "tilt_down";
        case CameraMove::PUSH_IN:   return "push_in";
        case CameraMove::PULL_OUT:  return "pull_out";
        case CameraMove::HANDHELD:  return "handheld";
        case CameraMove::ORBIT:     return "orbit";
    }
    return "?";
}

const char* to_string(Lens v) {
    switch (v) {
        case Lens::AUTO:     return "auto";
        case Lens::WIDE:     return "wide";
        case Lens::NORMAL:   return "normal";
        case Lens::PORTRAIT: return "portrait";
        case Lens::TELE:     return "tele";
    }
    return "?";
}

const char* to_string(FacePose v) {
    switch (v) {
        case FacePose::FRONT:         return "front";
        case FacePose::THREE_QUARTER: return "three_quarter";
        case FacePose::PROFILE:       return "profile";
        case FacePose::BACK:          return "back";
        case FacePose::OFF_SCREEN:    return "off_screen";
    }
    return "?";
}

const char* to_string(Transition v) {
    switch (v) {
        case Transition::CUT:      return "cut";
        case Transition::DISSOLVE: return "dissolve";
        case Transition::FADE_IN:  return "fade_in";
        case Transition::FADE_OUT: return "fade_out";
        case Transition::WHIP:     return "whip";
    }
    return "?";
}

/// 这一格在界面上怎么说。**给人看的，跟着界面语言换。**
///
/// ⚠️ 判"是哪个状态"一律用 `to_string()`——那是存盘和上线的名字，不翻。
const std::string& status_zh(ShotStatus v) {
    switch (v) {
        case ShotStatus::PLANNED:         return SAY("未开工");
        case ShotStatus::AUDIO_DONE:      return SAY("配音完成，还没出片");
        case ShotStatus::FRAME_DONE:      return SAY("首帧完成，还没出片");
        case ShotStatus::DRAFT_DONE:      return SAY("草稿完成");
        case ShotStatus::DRAFT_REJECTED:  return SAY("草稿未过闸门");
        case ShotStatus::FINAL_DONE:      return SAY("成片完成");
        case ShotStatus::FINAL_REJECTED:  return SAY("成片未过闸门");
        case ShotStatus::FALLBACK:        return SAY("重试超限，已降级");
        case ShotStatus::LOCKED:          return SAY("已锁定");
    }
    return SAY("状态不明");
}

const char* to_string(ShotStatus v) {
    switch (v) {
        case ShotStatus::PLANNED:         return "planned";
        case ShotStatus::AUDIO_DONE:      return "audio_done";
        case ShotStatus::FRAME_DONE:      return "frame_done";
        case ShotStatus::DRAFT_DONE:      return "draft_done";
        case ShotStatus::DRAFT_REJECTED:  return "draft_rejected";
        case ShotStatus::FINAL_DONE:      return "final_done";
        case ShotStatus::FINAL_REJECTED:  return "final_rejected";
        case ShotStatus::FALLBACK:        return "fallback";
        case ShotStatus::LOCKED:          return "locked";
    }
    return "?";
}

// ── 口型能力判定 ───────────────────────────────────────────────────────
//
// 对应 Python 侧的三个 frozenset。做成函数而不是 set，因为枚举值连续，
// switch 比查表快，而且漏掉新增枚举值时编译器会警告。

bool is_lipsync_capable(ShotSize v) {
    switch (v) {
        case ShotSize::ECU:
        case ShotSize::CU:
        case ShotSize::MCU:
        case ShotSize::MS:
            return true;
        case ShotSize::MLS:
        case ShotSize::LS:
        case ShotSize::ELS:
            return false;
    }
    return false;
}

bool is_lipsync_capable(CameraAngle v) {
    // 只有顶拍被排除。俯拍（high）仍然能看到嘴。
    return v != CameraAngle::OVERHEAD;
}

bool is_lipsync_capable(FacePose v) {
    switch (v) {
        case FacePose::FRONT:
        case FacePose::THREE_QUARTER:
        case FacePose::PROFILE:
            return true;
        case FacePose::BACK:
        case FacePose::OFF_SCREEN:
            return false;
    }
    return false;
}

// ── 校验 ───────────────────────────────────────────────────────────────

void CharacterInShot::validate(const std::string& where,
                               std::vector<std::string>& errs) const {
    if (char_id.empty()) {
        errs.push_back(SAYF("%1：char_id 不能为空", where));
    }
    check_len(expression, 40, SAYF("%1 的 expression", where), errs);
    check_len(action, 80, SAYF("%1 的 action", where), errs);
    check_len(wardrobe_state, 40, SAYF("%1 的 wardrobe_state", where), errs);
}

void DialogueLine::validate(const std::string& where,
                            std::vector<std::string>& errs) const {
    const std::size_t n = utf8_len(text);
    if (n < 1) {
        errs.push_back(SAYF("%1：台词不能为空", where));
    }
    check_len(text, 200, SAYF("%1 的台词", where), errs);

    if (emotion_intensity < 0.0 || emotion_intensity > 1.0) {
        errs.push_back(SAYF("%1：emotion_intensity 要在 0 到 1 之间，当前 %2",
                            where, num(emotion_intensity)));
    }
    if (actual_duration_s.has_value() && *actual_duration_s < 0.0) {
        errs.push_back(SAYF("%1：actual_duration_s 不能为负", where));
    }
}

std::vector<std::string> Shot::validate() const {
    std::vector<std::string> errs;

    if (!is_slug(shot_id)) {
        errs.push_back(SAY("shot_id 只能是小写字母、数字和下划线，当前是 ") + shot_id);
    }
    if (!is_slug(scene_id)) {
        errs.push_back(SAY("scene_id 只能是小写字母、数字和下划线，当前是 ") + scene_id);
    }
    if (order < 0) {
        errs.push_back(SAY("order 不能为负"));
    }

    check_len(visual_desc, 300, "visual_desc", errs);
    check_len(first_frame_prompt, 1200, "first_frame_prompt", errs);
    if (last_frame_prompt.has_value()) {
        check_len(*last_frame_prompt, 1200, "last_frame_prompt", errs);
    }
    check_len(motion_prompt, 400, "motion_prompt", errs);
    check_len(lighting, 80, "lighting", errs);
    check_len(beat, 20, "beat", errs);
    check_len(continuity_notes, 200, "continuity_notes", errs);

    if (duration_s <= 0.0 || duration_s > 30.0) {
        errs.push_back(SAYF("duration_s 要在 0 到 30 秒之间，当前 %1",
                            num(duration_s)));
    }
    if (transition_dur_s < 0.0 || transition_dur_s > 2.0) {
        errs.push_back(SAYF("transition_dur_s 要在 0 到 2 秒之间，当前 %1",
                            num(transition_dur_s)));
    }
    if (attempts < 0) {
        errs.push_back(SAY("attempts 不能为负"));
    }

    // 对应 Python 的 _check_transition
    if (transition_in == Transition::CUT && transition_dur_s != 0.0) {
        errs.push_back(SAY("硬切的转场时长必须为 0"));
    }
    if (transition_in != Transition::CUT && transition_dur_s <= 0.0) {
        // `to_string(Transition)` 是接口和存盘的取值，不翻——翻了就是
        // 改了接口（同 `to_string(Slot)`）。
        errs.push_back(SAYF("%1 需要一个大于 0 的转场时长",
                            to_string(transition_in)));
    }

    for (std::size_t i = 0; i < characters.size(); ++i) {
        characters[i].validate(SAYF("第 %1 个角色", std::to_string(i + 1)),
                               errs);
    }
    for (std::size_t i = 0; i < dialogue.size(); ++i) {
        dialogue[i].validate(SAYF("第 %1 句台词", std::to_string(i + 1)), errs);
    }

    // 对应 Python 的 _check_dialogue_chars：
    // 台词里出现的角色必须也在 characters 里，否则是分镜自相矛盾。
    for (const auto& line : dialogue) {
        if (!line.char_id.has_value()) continue;
        const bool present = std::any_of(
            characters.begin(), characters.end(),
            [&](const CharacterInShot& c) { return c.char_id == *line.char_id; });
        if (!present) {
            errs.push_back(SAYF("台词说话人 %1 不在本镜角色列表中。"
                                "旁白请把 char_id 留空", *line.char_id));
        }
    }

    return errs;
}

bool Shot::has_onscreen_dialogue() const {
    return std::any_of(dialogue.begin(), dialogue.end(),
                       [](const DialogueLine& l) { return l.char_id.has_value(); });
}

std::optional<double> Shot::total_dialogue_duration_s() const {
    if (dialogue.empty()) return 0.0;
    double total = 0.0;
    for (const auto& line : dialogue) {
        if (!line.actual_duration_s.has_value()) return std::nullopt;
        total += *line.actual_duration_s;
    }
    return total;
}

// ── 口型规则 ───────────────────────────────────────────────────────────

bool derive_needs_lipsync(const Shot& shot) {
    if (!shot.has_onscreen_dialogue()) return false;
    if (!is_lipsync_capable(shot.shot_size)) return false;
    if (!is_lipsync_capable(shot.camera_angle)) return false;

    // 说话的角色里，只要有一个是正脸或侧脸对着镜头就做口型。
    for (const auto& line : shot.dialogue) {
        if (!line.char_id.has_value()) continue;
        for (const auto& c : shot.characters) {
            if (c.char_id == *line.char_id && is_lipsync_capable(c.face_pose)) {
                return true;
            }
        }
    }
    return false;
}

void apply_lipsync_rules(std::vector<Shot>& shots) {
    for (auto& shot : shots) {
        shot.needs_lipsync = derive_needs_lipsync(shot);
    }
}

}  // namespace changji::models
