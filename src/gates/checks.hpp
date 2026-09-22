#pragma once

// 质量闸门。
//
// **无人值守模式下，闸门是唯一阻止废片流入成片的机制。**
//
// 三条纪律：
//   判定必须程序可算，不能依赖人看。
//   失败必须给出可操作的下一步，而不只是说不合格。
//   绝不静默放行，重试超限就明确降级并留下记录。
//
// 移植自 src/changji/gates/checks.py。

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "util/say.hpp"
#include "media/ffmpeg.hpp"
#include "models/shot.hpp"

namespace changji::gates {

enum class Verdict {
    Pass,
    Retry,     ///< 换种子重跑可能就好了
    Regress,   ///< 重跑也没用，得退回上一阶段
    Fallback,  ///< 重试超限，降级处理
};

const char* to_string(Verdict v);

/// 一次闸门判定的结果。
struct GateResult {
    std::string shot_id;
    Verdict verdict = Verdict::Pass;
    std::string gate;
    std::vector<std::string> reasons;
    /// 判定依据的数字。**失败时人要看的就是它**——只说"画面近乎纯色"
    /// 不够，得知道展布是 3 还是 7.9，才判断得出是模型的问题还是阈值太严。
    std::map<std::string, double> metrics;

    bool ok() const { return verdict == Verdict::Pass; }
    std::string describe() const;
};

/// 取样点在片子的哪个位置。**只说"画面近乎纯色"不够**——
/// 片头纯色多半是模型没起来，片尾纯色多半是帧数超了模型的上限，
/// 两者的下一步完全不同。
std::string position_name(int index, int total);

/// 检查一个镜头的视频。草稿档和成片档用同一套检查，只是期望值不同。
GateResult gate_video(const models::Shot& shot,
                      const std::filesystem::path& video_path,
                      const media::FFmpeg& ff, const config::GateConfig& cfg,
                      std::optional<double> expected_duration_s = std::nullopt,
                      std::optional<std::pair<int, int>> expected_size = std::nullopt,
                      const std::string& gate_name = SAY("画面闸门"));

/// 检查镜头时长能不能装下配音。
///
/// **这个检查放在装配之前。** 装完再发现装不下就得重做整章。
GateResult gate_audio_sync(const models::Shot& shot,
                           const std::filesystem::path& video_path,
                           const media::FFmpeg& ff,
                           const config::GateConfig& cfg);

/// 整章装配后的检查。
struct EpisodeGateResult {
    Verdict verdict = Verdict::Pass;
    std::vector<std::string> reasons;
    std::map<std::string, double> metrics;

    bool ok() const { return verdict == Verdict::Pass; }
};

/// 成片检查：时长、响度、音轨。
EpisodeGateResult gate_episode(const std::filesystem::path& video_path,
                               const media::FFmpeg& ff,
                               const config::GateConfig& cfg,
                               std::optional<double> expected_duration_s = std::nullopt);

/// 闸门失败后决定怎么办。
///
/// **这是无人值守能不能不卡死的关键。** 重试超限时降级而不是停下来，
/// 保证整章能出片，同时把问题记录下来供人事后查。
Verdict decide_next(const GateResult& result, const models::Shot& shot,
                    const config::GateConfig& cfg);

/// 闸门结果概览。无人值守时这是人唯一要看的东西。
std::string summarize(const std::vector<GateResult>& results);

/// 「通过闸门」那句话后面挂的运动量：`（运动 3.5，最大 45）`。
///
/// 闸门对运动量只报数不判（见 gate_video 里那段），所以过了闸门的镜头
/// 这个数得有地方露出来，不然攒不出定阈值的样本。没量到就回空串。
std::string motion_note(const GateResult& result);

}  // namespace changji::gates
