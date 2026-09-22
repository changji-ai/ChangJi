#pragma once

// 分镜表：整套系统的中枢数据结构。
//
// 设计上最重要的一条：这里没有任何描述角色长相、发型、服装的字段。
//
// 保持角色跨镜头一致的常见做法是在提示词里反复强调，那不可靠。这里的做法是让
// 大模型在结构上就没有写错的机会：它只能填 char_id 和本镜可变项，外观描述在
// 渲染时由程序从角色资产库机械拼接，逐字节相同。
//
// 同一份定义既用于运行时校验，也用于导出 JSON Schema 约束大模型输出。
//
// 移植自 src/changji/models/shot.py。校验逻辑与那边的 pydantic 约束一一对应，
// 对拍时才能发现漏移植。

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/json_compat.hpp"

namespace changji::models {

// ── 枚举 ───────────────────────────────────────────────────────────────

/// 景别。取值顺序由近到远。
enum class ShotSize { ECU, CU, MCU, MS, MLS, LS, ELS };

enum class CameraAngle { LOW, EYE_LEVEL, HIGH, OVERHEAD, DUTCH };

enum class CameraMove {
    STATIC, PAN_LEFT, PAN_RIGHT, TILT_UP, TILT_DOWN,
    PUSH_IN, PULL_OUT, HANDHELD, ORBIT
};

/// 焦段。**电影感里最便宜的一个词**（docs/电影质感方案.md）：写实模型对
/// 「85mm 人像，背景压缩，浅景深」这种字面焦段有反应，而我们的镜头层原来
/// 只有景别和机位。AUTO = 没填，拼提示词时一个字不加（老分镜表就是这样）。
enum class Lens { AUTO, WIDE, NORMAL, PORTRAIT, TELE };

/// 角色面部朝向。口型判定用。
enum class FacePose { FRONT, THREE_QUARTER, PROFILE, BACK, OFF_SCREEN };

enum class Transition { CUT, DISSOLVE, FADE_IN, FADE_OUT, WHIP };

/// 镜头在流水线上的位置。断点续跑靠它。
enum class ShotStatus {
    PLANNED,          ///< 分镜已出，未开工
    AUDIO_DONE,       ///< 配音已出，时长已锁
    FRAME_DONE,       ///< 首帧已出
    DRAFT_DONE,       ///< 草稿档视频已出
    DRAFT_REJECTED,   ///< 草稿未过闸门
    FINAL_DONE,       ///< 成片档已出
    FINAL_REJECTED,   ///< 成片未过闸门
    /// 重试超限，**保留最后那一版视频**（闸门没过，但片子在，装配照收）。
    ///
    /// 名字容易误会：全代码库**没有任何地方生成静帧、也没有任何运镜**
    /// （2026-09-13 查过，一处 zoompan 都没有）。原来十几处注释和界面文案
    /// 都写着"降级为静帧加运镜"，那是一句从没兑现过的话。
    FALLBACK,
    LOCKED            ///< 人工确认，不再重跑
};

// 字符串取值必须与 Python 侧逐字相同，两个后端读写同一批项目文件。
NLOHMANN_JSON_SERIALIZE_ENUM(ShotSize, {
    {ShotSize::ECU, "ECU"}, {ShotSize::CU, "CU"}, {ShotSize::MCU, "MCU"},
    {ShotSize::MS, "MS"}, {ShotSize::MLS, "MLS"}, {ShotSize::LS, "LS"},
    {ShotSize::ELS, "ELS"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(CameraAngle, {
    {CameraAngle::LOW, "low"}, {CameraAngle::EYE_LEVEL, "eye_level"},
    {CameraAngle::HIGH, "high"}, {CameraAngle::OVERHEAD, "overhead"},
    {CameraAngle::DUTCH, "dutch"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(CameraMove, {
    {CameraMove::STATIC, "static"}, {CameraMove::PAN_LEFT, "pan_left"},
    {CameraMove::PAN_RIGHT, "pan_right"}, {CameraMove::TILT_UP, "tilt_up"},
    {CameraMove::TILT_DOWN, "tilt_down"}, {CameraMove::PUSH_IN, "push_in"},
    {CameraMove::PULL_OUT, "pull_out"}, {CameraMove::HANDHELD, "handheld"},
    {CameraMove::ORBIT, "orbit"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(Lens, {
    {Lens::AUTO, "auto"}, {Lens::WIDE, "wide"}, {Lens::NORMAL, "normal"},
    {Lens::PORTRAIT, "portrait"}, {Lens::TELE, "tele"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(FacePose, {
    {FacePose::FRONT, "front"}, {FacePose::THREE_QUARTER, "three_quarter"},
    {FacePose::PROFILE, "profile"}, {FacePose::BACK, "back"},
    {FacePose::OFF_SCREEN, "off_screen"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(Transition, {
    {Transition::CUT, "cut"}, {Transition::DISSOLVE, "dissolve"},
    {Transition::FADE_IN, "fade_in"}, {Transition::FADE_OUT, "fade_out"},
    {Transition::WHIP, "whip"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(ShotStatus, {
    {ShotStatus::PLANNED, "planned"}, {ShotStatus::AUDIO_DONE, "audio_done"},
    {ShotStatus::FRAME_DONE, "frame_done"}, {ShotStatus::DRAFT_DONE, "draft_done"},
    {ShotStatus::DRAFT_REJECTED, "draft_rejected"},
    {ShotStatus::FINAL_DONE, "final_done"},
    {ShotStatus::FINAL_REJECTED, "final_rejected"},
    {ShotStatus::FALLBACK, "fallback"}, {ShotStatus::LOCKED, "locked"},
})

/// 这个状态算不算「这一镜有片了」。**只此一处。**
///
/// 判据散开写过一次，代价很大：`agent/tools.cpp` 里那份状态摘要自己列了
/// `final` / `final_ok` / `done` 三个名字——**这三个在上面那张表里一个都没有**
/// （真正的是 `final_done` / `fallback` / `locked` / `draft_done`）。于是它报
/// 的「出片 N/M」里 N **永远是 0**，而那句话每一轮都发给模型看：一章明明
/// 17 镜全出了片，模型读到的是 0/17，接着就会提议把整章重出一遍——一个
/// 钟头的显卡时间。2026-09-21 把那份摘要打出来看才发现。
///
/// `pipeline::assembly_usable` 在这之上再加一条「视频文件真的在」。
bool status_has_film(ShotStatus s);

/// 枚举转回字符串。错误消息里要用，和 Python 的 `.value` 对齐。
const char* to_string(ShotSize v);
const char* to_string(CameraAngle v);
const char* to_string(CameraMove v);
const char* to_string(Lens v);
const char* to_string(FacePose v);
const char* to_string(Transition v);
const char* to_string(ShotStatus v);

/// 状态的人话。**给用户看的消息里不要用枚举名**——引擎报一句
/// 「ep01_sh001 状态是 audio_done」，人得先去查那是什么意思。
/// 界面那边有同一张表（webapp 的 SHOT_STATUS），但引擎发出去的消息
/// 到不了那张表：它是拼好的一整句话。
const std::string& status_zh(ShotStatus v);

/// 能看清嘴部动作的景别。口型判定用，不要改成靠模型判断。
bool is_lipsync_capable(ShotSize v);
/// 俯拍和顶拍看不清嘴，排除在口型之外。
bool is_lipsync_capable(CameraAngle v);
bool is_lipsync_capable(FacePose v);

// ── 结构体 ─────────────────────────────────────────────────────────────

/// 角色在本镜头中的表现。
///
/// 只有可变项。外观描述属于角色资产，不在这里，也不允许大模型在这里写。
struct CharacterInShot {
    std::string char_id;                    ///< 角色 id，必须是已注册角色之一
    std::string expression;                 ///< 表情，如 愕然、隐忍（≤40）
    std::string action;                     ///< 本镜动作，如 后退半步（≤80）
    std::string wardrobe_state = "default"; ///< 服装状态 id，如 suit_torn（≤40）
    FacePose face_pose = FacePose::FRONT;   ///< 面部朝向
    /// 画面位置：left / center / right。
    ///
    /// ⚠️ **只写不读。** `CharacterInShot` 这一层 `llm_shot_schema()` 不裁
    /// （它只把 `char_id` 收成枚举），所以这一栏照样发给模型、模型照样填、
    /// 照样存进 shots.json——而全仓库读它的一处都没有：`prompt_compose`
    /// 拼首帧提示词时不看，`/api/shots` 不回，界面上没有。
    /// 想让它生效，得在 prompt_compose 那边把它拼进去。
    std::string screen_pos = "center";

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        CharacterInShot, char_id, expression, action, wardrobe_state,
        face_pose, screen_pos)

    void validate(const std::string& where, std::vector<std::string>& errs) const;
};

/// 一句台词。时长字段由配音阶段回填，分镜阶段不填。
struct DialogueLine {
    std::optional<std::string> char_id;  ///< 说话角色。为空表示旁白
    std::string text;                    ///< 1..200 字
    std::string emotion = "neutral";     ///< 情绪标签
    double emotion_intensity = 0.5;      ///< 0..1
    std::optional<std::string> voice_id; ///< 音色 id，由角色资产决定

    // 以下由配音阶段回填
    std::optional<std::string> audio_path;   ///< 相对项目根的路径
    std::optional<double> actual_duration_s; ///< ≥0

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        DialogueLine, char_id, text, emotion, emotion_intensity, voice_id,
        audio_path, actual_duration_s)

    void validate(const std::string& where, std::vector<std::string>& errs) const;
};

/// 一个镜头。
struct Shot {
    std::string shot_id;   ///< 全局唯一，如 ep01_s03_sh007；^[a-z0-9_]+$
    std::string scene_id;  ///< ^[a-z0-9_]+$
    int order = 0;         ///< 章内顺序，≥0

    // ---- 画面 ----
    std::string visual_desc;                      ///< 给人看的中文描述（≤300）
    std::string first_frame_prompt;               ///< ≤1200
    std::optional<std::string> last_frame_prompt; ///< 为空则走单帧图生视频（≤1200）
    std::string motion_prompt;                    ///< 运动描述，给视频模型（≤400）
    std::string negative_prompt;

    ShotSize    shot_size    = ShotSize::MS;
    CameraAngle camera_angle = CameraAngle::EYE_LEVEL;
    CameraMove  camera_move  = CameraMove::STATIC;
    /// 焦段。AUTO = 没填。
    Lens        lens         = Lens::AUTO;
    /// 这一镜的光：时段、光源、方向、软硬（≤80）。空 = 没填。
    /// 场景资产里那句 `lighting` 是一场戏的基调，这一句是这一镜的，
    /// 拼在场景层之后、镜头层里——镜头级的光要盖过基调。
    std::string lighting;
    /// 紧接上一镜的动作（同一场景、同一时刻、动作连续）。出片时拿上一镜
    /// 真出来的最后一帧当这一镜的首帧，动作接得上。见 [video].chain_frames。
    bool continuous_with_prev = false;
    /// 复用机位 id。
    ///
    /// ⚠️ **原来这行末尾写着「同场景同机位保证不越轴」——没有谁在保证。**
    /// 提示词那头确实在教模型填（prompts.toml 第 12 条「同一场景内连续镜头
    /// 尽量复用 camera_id，避免越轴」），字段也在 `kLlmShotFields` 里、存得
    /// 下来，但**读它的一处都没有**：没有任何校验比对过同一 camera_id 的两
    /// 镜是否真在轴的同一侧，`/api/shots` 也不回它。今天它能起的作用只有一
    /// 个——模型自己写的时候顺带上了点心。"越轴"这件事没人查。
    std::optional<std::string> camera_id;

    // ---- 引用（只放 id，不放描述）----
    std::vector<CharacterInShot> characters;
    std::optional<std::string> location_id;
    std::vector<std::string> prop_ids;

    // ---- 时间 ----
    double duration_s = 5.0;      ///< 镜头时长，(0, 30]
    bool duration_locked = false; ///< 真表示已由配音时长反推锁定，不可再调

    // ---- 声音 ----
    std::vector<DialogueLine> dialogue;
    /// ⚠️ `sfx` / `bgm_cue`（以及上面的 `prop_ids`）三个是**彻底的死字段**，
    /// 和 screen_pos / camera_id / missing_info 那三个还不一样：它们连模型
    /// 都见不到——`kLlmShotFields` 里没有，`llm_shot_schema()` 裁 schema 时
    /// 就删掉了，`keep_llm_fields()` 解析时再删一遍。于是永远是空的，也永远
    /// 没人读。留着只是因为它们在 `NLOHMANN_DEFINE_TYPE` 那一行里，动了就是
    /// 改盘上格式。
    std::vector<std::string> sfx;
    std::optional<std::string> bgm_cue;
    bool needs_lipsync = false; ///< 由 derive_needs_lipsync 规则推导，不要让模型填

    // ---- 剪辑 ----
    Transition transition_in = Transition::CUT;
    double transition_dur_s = 0.0; ///< 0..2
    std::string subtitle_text;

    // ---- 质控 ----
    std::string beat;             ///< 叙事功能，如 反转、铺垫（≤20）
    /// 需要和前后镜保持一致的细节。
    ///
    /// ⚠️ **2026-09-16（0d2c984）起模型不再被要求写它，所以新数据里永远是
    /// 空的。** prompts.toml 第 172 行那段写着：没人读的字段（camera_id /
    /// transition / continuity_notes / missing_info / wardrobe_state /
    /// screen_pos）连同它们的规则一起从 schema 里删了。
    ///
    /// 但**「没人读」这个判断对这一栏是不成立的**：它一路读到了界面上（见
    /// 下面那几行）。所以现在是半拆状态——引擎照发、界面照显示，而内容只
    /// 可能来自存量数据（hulian-test 的 159 镜里还有 14 镜有）。
    /// 要么把这一栏和它的规则一起放回 schema，要么把下面这条链也拆掉；
    /// 保持现状的代价是界面上一格永远空着的「接戏」。
    ///
    /// 下面这段记的是它当年那条完整的链，留着是因为要恢复的话照它走：
    /// `kLlmShotFields` 里有、`validate()` 查它不超 200 字，存进 shots.json，
    /// `/api/shots` 发出去，
    /// 单章页的镜头抽屉里摆成「接戏」那一行（2026-09-15 补上的——在那之前
    /// 它一路传到浏览器就被丢掉了，而在引擎里 grep 会看到"有人读"）。
    ///
    /// **界面上是只读的。** 它是模型对这一镜的观察，不是参数：改了没有下游
    /// 会读，而一个能改却没人读的框比不摆更糟。
    std::string continuity_notes; ///< ≤200
    /// 模型自报的信息缺口。
    ///
    /// ⚠️ **这一栏现在是彻底死的。** 2026-09-16（0d2c984）起它和它的提示词
    /// 规则一起从 schema 里删了，模型不再写；而在那之前它也没有任何读者——
    /// `/api/shots` 不回它，界面上看不到。模型老老实实说"我不知道她手里拿
    /// 的是什么"，这句话直接埋进 shots.json 就没了下文（存量数据里 159 镜
    /// 还剩 1 镜有）。
    ///
    /// 和 continuity_notes 不一样：那一栏拆了一半（界面还在显示），这一栏
    /// 两头都没有。要删的话直接删，不会碰坏任何东西。
    std::vector<std::string> missing_info;

    // ---- 运行时状态（大模型不填）----
    ShotStatus status = ShotStatus::PLANNED;
    int attempts = 0; ///< ≥0
    std::optional<std::string> frame_path;
    /// 尾帧（`last_frame_prompt` 出的那张），出片时当 end_image 传给
    /// 首尾帧模型。没有就是单帧图生视频。
    std::optional<std::string> end_frame_path;
    std::optional<std::string> video_path;
    std::vector<std::string> gate_notes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Shot, shot_id, scene_id, order,
        visual_desc, first_frame_prompt, last_frame_prompt, motion_prompt,
        negative_prompt, shot_size, camera_angle, camera_move, lens, lighting,
        continuous_with_prev, camera_id,
        characters, location_id, prop_ids,
        duration_s, duration_locked,
        dialogue, sfx, bgm_cue, needs_lipsync,
        transition_in, transition_dur_s, subtitle_text,
        beat, continuity_notes, missing_info,
        status, attempts, frame_path, end_frame_path, video_path, gate_notes)

    /// 校验。返回错误列表而不是抛异常。
    ///
    /// 与 pydantic 的差异要说清楚：那边 model_validator 第一条不过就抛，
    /// 后面的条件看不到；这里把全部问题一次收齐。Web 层要把它们一次性回给
    /// 前端，逐条抛会让用户改一条报一条。对拍时只比较"有没有错"和错误内容，
    /// 不比较条数。
    std::vector<std::string> validate() const;

    bool has_onscreen_dialogue() const;

    /// 全部台词的实际总时长。任一句未配音则返回 nullopt。
    std::optional<double> total_dialogue_duration_s() const;
};

/// 判断一个镜头该不该做口型。
///
/// 这件事必须用规则算，不能交给大模型判断，它在这上面很不稳。
///
/// 四个条件同时成立才做：有出镜台词、景别够近能看清嘴、机位不是顶拍、
/// 并且说话的角色确实是正脸或侧脸对着镜头。
bool derive_needs_lipsync(const Shot& shot);

/// 批量回填 needs_lipsync。分镜生成后、配音之前调用。
void apply_lipsync_rules(std::vector<Shot>& shots);

}  // namespace changji::models
