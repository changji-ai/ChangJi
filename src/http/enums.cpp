#include "util/say.hpp"
#include "http/enums.hpp"

#include "models/shot.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

/// 一条：值走引擎自己的 to_string，中文写在这儿。
template <typename E>
json row(E v, const std::string& zh) {
    return json{{"value", models::to_string(v)}, {"label", zh}};
}

}  // namespace

json enums_json() {
    using models::CameraAngle;
    using models::CameraMove;
    using models::ShotSize;
    using models::ShotStatus;

    json out;

    // 景别。**顺序是从近到远**，界面上那个下拉照这个顺序摆——乱序的下拉
    // 人要一个一个读过去才挑得到。
    out["shot_size"] = json::array({
        row(ShotSize::ECU, SAY("大特写")), row(ShotSize::CU, SAY("特写")),
        row(ShotSize::MCU, SAY("近景")),   row(ShotSize::MS, SAY("中景")),
        row(ShotSize::MLS, SAY("中远景")), row(ShotSize::LS, SAY("远景")),
        row(ShotSize::ELS, SAY("大远景")),
    });

    out["camera_angle"] = json::array({
        row(CameraAngle::LOW, SAY("仰拍")), row(CameraAngle::EYE_LEVEL, SAY("平视")),
        row(CameraAngle::HIGH, SAY("俯拍")), row(CameraAngle::OVERHEAD, SAY("顶拍")),
        row(CameraAngle::DUTCH, SAY("斜角")),
    });

    out["camera_move"] = json::array({
        row(CameraMove::STATIC, SAY("固定")),    row(CameraMove::PAN_LEFT, SAY("左摇")),
        row(CameraMove::PAN_RIGHT, SAY("右摇")), row(CameraMove::TILT_UP, SAY("上摇")),
        row(CameraMove::TILT_DOWN, SAY("下摇")), row(CameraMove::PUSH_IN, SAY("推进")),
        row(CameraMove::PULL_OUT, SAY("拉远")),  row(CameraMove::HANDHELD, SAY("手持")),
        row(CameraMove::ORBIT, SAY("环绕")),
    });

    // 状态那一族**不在这儿写中文**：引擎里早就有一份（`models::status_zh`），
    // 它进的是命令行摘要和报错消息。再写一遍就是这个文件要治的病。
    json status = json::array();
    for (auto v : {ShotStatus::PLANNED, ShotStatus::AUDIO_DONE, ShotStatus::FRAME_DONE,
                   ShotStatus::DRAFT_DONE, ShotStatus::DRAFT_REJECTED,
                   ShotStatus::FINAL_DONE, ShotStatus::FINAL_REJECTED,
                   ShotStatus::FALLBACK, ShotStatus::LOCKED}) {
        status.push_back({{"value", models::to_string(v)}, {"label", models::status_zh(v)}});
    }
    out["shot_status"] = std::move(status);

    return out;
}

ApiResult get_enums() { return {200, enums_json()}; }

}  // namespace changji::http
