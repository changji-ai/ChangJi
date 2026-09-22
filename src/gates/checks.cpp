#include "util/say.hpp"
#include "gates/checks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::gates {

namespace {

/// 保留 n 位小数。**对齐 Python 的 round()，也就是银行家舍入**——
/// 这些数会进 metrics 然后被对拍逐个比，差一个末位就是不通过。
double round_to(double v, int digits) {
    const double f = std::pow(10.0, digits);
    return std::nearbyint(v * f) / f;
}

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

GateResult fail(const std::string& shot_id, Verdict v, const std::string& gate,
                std::vector<std::string> reasons,
                std::map<std::string, double> metrics = {}) {
    GateResult r;
    r.shot_id = shot_id;
    r.verdict = v;
    r.gate = gate;
    r.reasons = std::move(reasons);
    r.metrics = std::move(metrics);
    return r;
}

}  // namespace

const char* to_string(Verdict v) {
    switch (v) {
        case Verdict::Pass:     return "pass";
        case Verdict::Retry:    return "retry";
        case Verdict::Regress:  return "regress";
        case Verdict::Fallback: return "fallback";
    }
    return "?";
}

std::string GateResult::describe() const {
    // ⚠️ **闸门说的话会落进 `gates.jsonl` 和 `shot.gate_notes`。** 落下去的
    // 是**当时**那一国话——换了界面语言，旧记录不会跟着变，那是对的：那一行
    // 记的是"当时说了什么"，不是"现在该怎么说"。
    if (ok()) return SAYF("%1 通过%2", shot_id, gate);
    return SAYF("%1 未过%2：%3", shot_id, gate, join(reasons, SAY("；")));
}

std::string position_name(int index, int total) {
    if (total <= 1) return SAY("画面");
    if (index == 0) return SAY("片头");
    if (index == total - 1) return SAY("片尾");
    return SAY("片中");
}

std::string motion_note(const GateResult& result) {
    const auto mean = result.metrics.find("motion_mean");
    const auto mx = result.metrics.find("motion_max");
    if (mean == result.metrics.end() || mx == result.metrics.end()) return "";
    return SAYF("（运动 %1，最大 %2）", fmt("%.1f", mean->second),
                fmt("%.0f", mx->second));
}


GateResult gate_video(const models::Shot& shot, const fs::path& video_path,
                      const media::FFmpeg& ff, const config::GateConfig& cfg,
                      std::optional<double> expected_duration_s,
                      std::optional<std::pair<int, int>> expected_size,
                      const std::string& gate_name) {
    std::vector<std::string> reasons;
    std::map<std::string, double> metrics;

    std::error_code ec;
    if (!fs::is_regular_file(video_path, ec)) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {SAY("视频文件不存在")});
    }
    // 1KB 以下不可能是一段视频。先看大小再去 probe，省掉一次子进程——
    // 而且 ffprobe 对一个零字节文件的报错很难懂。
    if (fs::file_size(video_path, ec) < 1024) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {SAY("视频文件几乎是空的")});
    }

    media::MediaInfo info;
    try {
        info = ff.probe(video_path);
    } catch (const media::FFmpegError& e) {
        return fail(shot.shot_id, Verdict::Retry, gate_name,
                    {SAYF("视频文件读不出来：%1", e.what())});
    }

    if (!info.has_video) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {SAY("文件里没有视频轨")});
    }

    metrics["duration_s"] = info.duration_s;
    metrics["width"] = static_cast<double>(info.width);
    metrics["height"] = static_cast<double>(info.height);

    // 时长。**差太多是 REGRESS 不是 RETRY**：帧数算错了，换个种子重跑
    // 还是一样的帧数。要退回上一阶段重新算。
    if (expected_duration_s.has_value() && *expected_duration_s != 0.0) {
        const double drift = std::abs(info.duration_s - *expected_duration_s);
        metrics["duration_drift_s"] = round_to(drift, 3);
        if (drift > std::max(0.5, *expected_duration_s * 0.2)) {
            return fail(shot.shot_id, Verdict::Regress, gate_name,
                        {SAYF("时长 %1 秒，期望 %2 秒，差 %3 秒，"
                              "多半是帧数算错了",
                              fmt("%.2f", info.duration_s),
                              fmt("%.2f", *expected_duration_s),
                              fmt("%.2f", drift))},
                        metrics);
        }
    }

    // 分辨率不符说明档位参数没生效，同样重跑无用。
    if (expected_size.has_value() &&
        (info.width != expected_size->first || info.height != expected_size->second)) {
        return fail(shot.shot_id, Verdict::Regress, gate_name,
                    {SAYF("分辨率 %1x%2，期望 %3x%4，档位参数没生效",
                          std::to_string(info.width),
                          std::to_string(info.height),
                          std::to_string(expected_size->first),
                          std::to_string(expected_size->second))},
                    metrics);
    }

    // 画面内容。**多点取样**，只看一帧会漏掉中途崩坏。
    std::vector<media::PixelStats> samples;
    try {
        samples = ff.sample_pixel_stats(video_path, 3);
    } catch (const media::FFmpegError& e) {
        return fail(shot.shot_id, Verdict::Retry, gate_name,
                    {SAYF("读不出画面统计：%1", e.what())}, metrics);
    }
    if (samples.empty()) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {SAY("取不到任何画面")},
                    metrics);
    }

    std::vector<double> spreads, means;
    for (const auto& s : samples) {
        spreads.push_back(s.spread);
        means.push_back(s.mean);
    }
    const double spread_min = *std::min_element(spreads.begin(), spreads.end());
    double mean_sum = 0.0;
    for (const double m : means) mean_sum += m;
    metrics["spread_min"] = round_to(spread_min, 2);
    metrics["mean_avg"] = round_to(mean_sum / static_cast<double>(means.size()), 2);

    std::vector<std::string> blank_where;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].looks_blank()) {
            blank_where.push_back(
                position_name(static_cast<int>(i), static_cast<int>(samples.size())));
        }
    }
    if (!blank_where.empty()) {
        // **把均值一并报出来。** 展布这个判据分不开"纯色"和"很暗"
        // （见 media::PixelStats::looks_blank 里那段实测），而均值分得开：
        // 展布小 + 均值也低 = 大概率是按要求压暗的镜头，不是废图；
        // 展布小 + 均值在中间 = 一片均匀的灰，那才是生成失败。
        // 判据不动，但别让人只看见一句断言。
        const double mean_avg = mean_sum / static_cast<double>(means.size());
        std::string why =
            SAYF("%1的画面近乎纯色，展布只有 %2（均值 %3）",
                 join(blank_where, SAY("、")), fmt("%.1f", spread_min),
                 fmt("%.0f", mean_avg));
        if (mean_avg < 40.0) {
            why += SAY("。均值这么低，多半是这一镜本来就该很暗——"
                       "这个检查分不开「暗」和「空」，要留就单独重出它");
        }
        reasons.push_back(why);
    }

    std::vector<std::string> clipped_where;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].looks_clipped()) {
            clipped_where.push_back(
                position_name(static_cast<int>(i), static_cast<int>(samples.size())));
        }
    }
    if (!clipped_where.empty()) {
        reasons.push_back(SAYF("%1的画面整体过暗或过曝",
                               join(clipped_where, SAY("、"))));
    }

    // 已经报过"近乎纯色"就不再报"细节偏少"——同一件事说两遍，
    // 而用户要从两句里判断是不是两个问题。
    if (spread_min < cfg.min_pixel_std && blank_where.empty()) {
        reasons.push_back(SAYF("画面细节偏少，展布 %1 低于阈值 %2",
                               fmt("%.1f", spread_min),
                               fmt("%.0f", cfg.min_pixel_std)));
    }

    // 相邻取样点之间画面差异过大，说明中途崩坏。
    if (means.size() >= 2) {
        const double swing = *std::max_element(means.begin(), means.end()) -
                             *std::min_element(means.begin(), means.end());
        metrics["mean_swing"] = round_to(swing, 2);
        if (swing > 60.0) {
            reasons.push_back(SAYF("片中亮度剧烈跳变 %1，可能中途崩坏",
                                   fmt("%.0f", swing)));
        }
    }

    // 运动量。**只报数，不判**（2026-09-13 起）：几乎不动的片子（均值
    // 0.27～0.66）和中途硬切的片子（最大 45 / 中位 2.2）现在都能过闸门，
    // 但手上各只有几个样本，阈值先别拍——数进 metrics，出片那边把它写进
    // 「通过闸门」那句话里，攒够了再定判据。见 media::FFmpeg::motion_stats。
    // 量不到（老 ffmpeg、文件坏了）不算失败：上面几道已经把坏文件拦住了。
    try {
        const media::MotionStats motion = ff.motion_stats(video_path);
        if (motion.frames > 0) {
            metrics["motion_mean"] = round_to(motion.mean, 2);
            metrics["motion_median"] = round_to(motion.median, 2);
            metrics["motion_max"] = round_to(motion.max, 2);

            // **片中硬切这一种判。** 2026-09-13 三次实见，都是首帧和提示词
            // 对不上、模型只好在半路切到提示词要的画面：参考图翻版当首帧
            // （最大 89 / 中位 3.3）、8 秒长镜头（45 / 2.2）、手机特写画成
            // 拿手机的人（77 / 约 2）。正常镜头最大不过 27（那镜均值 8.3，
            // 是真的在动）。所以判据是"最大值又高又孤"：≥ 40 且 ≥ 中位的
            // 10 倍。甩镜那种快速运动是整段都大，中位跟着高，不会撞上。
            //
            // **判成 REGRESS 不是 RETRY**：首帧没变的话换种子重出视频还是
            // 那张首帧，还是得切。要回首帧阶段换个种子重出（render.cpp 里
            // 认 cut_inside 这个标记把状态退回去）。
            //
            // **2026-09-16 调了一次：40 太高，一条真硬切从底下漏过去了。**
            // ep04_sh010 是一段几乎不动的空镜，中途整个切黑：最大 34、
            // 中位 0.13——差了 261 倍，孤得不能再孤，却因为 34 < 40 判了
            // 通过，带着那一刀进了成片（attempts 跑到 3，gate_notes 全空）。
            //
            // 把地板降到 25、倍数从 10 提到 13，分辨的活交给"孤不孤"。
            // 在手上这 26 个有人眼核对过的样本上：5 条真硬切全抓到
            // （81/4.8、34/0.13，加上原来那三条 89/3.3、45/2.2、77/2），
            // 21 条正常镜头一条不误报——包括下面用例里钉着的 27/2.5
            // （倍数只有 10.8）、一只手带着纸快速划过的 51/9.7、
            // 和整段在动的 29/3.5。
            constexpr double kCutMaxDiff = 25.0;
            constexpr double kCutRatio = 13.0;
            if (motion.max >= kCutMaxDiff &&
                motion.max >= kCutRatio * std::max(motion.median, 0.1)) {
                metrics["cut_inside"] = 1.0;
                return fail(shot.shot_id, Verdict::Regress, gate_name,
                            {SAYF("片中硬切：相邻帧差最大 %1，中位只有 %2。"
                                  "多半是首帧和提示词对不上，模型半路切到了"
                                  "提示词要的画面。换种子重出视频没用，已退回"
                                  "首帧阶段，再跑一次首帧和出片",
                                  fmt("%.0f", motion.max),
                                  fmt("%.1f", motion.median))},
                            metrics);
            }
        }
    } catch (const media::FFmpegError&) {
    }

    if (!reasons.empty()) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, reasons, metrics);
    }
    GateResult r;
    r.shot_id = shot.shot_id;
    r.verdict = Verdict::Pass;
    r.gate = gate_name;
    r.metrics = metrics;
    return r;
}

GateResult gate_audio_sync(const models::Shot& shot, const fs::path& video_path,
                           const media::FFmpeg& ff,
                           const config::GateConfig& cfg) {
    const std::string gate = SAY("音画闸门");
    const auto speech = shot.total_dialogue_duration_s();
    if (!speech.has_value()) {
        // 有台词但没有配音时长，说明配音阶段没跑完。重跑视频没用。
        return fail(shot.shot_id, Verdict::Regress, gate,
                    {SAY("有台词但没有配音时长，配音阶段没跑完")});
    }
    if (*speech == 0.0) {
        GateResult r;
        r.shot_id = shot.shot_id;
        r.verdict = Verdict::Pass;
        r.gate = gate;
        return r;
    }

    media::MediaInfo info;
    try {
        info = ff.probe(video_path);
    } catch (const media::FFmpegError& e) {
        return fail(shot.shot_id, Verdict::Retry, gate,
                    {SAYF("读不出视频时长：%1", e.what())});
    }

    const double slack = info.duration_s - *speech;
    std::map<std::string, double> metrics{
        {"speech_s", round_to(*speech, 3)},
        {"video_s", round_to(info.duration_s, 3)},
        {"slack_s", round_to(slack, 3)},
    };

    if (slack < -cfg.max_audio_drift_s) {
        return fail(shot.shot_id, Verdict::Regress, gate,
                    {SAYF("配音 %1 秒装不进 %2 秒的镜头，超出 %3 秒。"
                          "需要重新锁定时长后重出这一镜",
                          fmt("%.2f", *speech), fmt("%.2f", info.duration_s),
                          fmt("%.2f", -slack))},
                    metrics);
    }
    GateResult r;
    r.shot_id = shot.shot_id;
    r.verdict = Verdict::Pass;
    r.gate = gate;
    r.metrics = metrics;
    return r;
}

EpisodeGateResult gate_episode(const fs::path& video_path,
                               const media::FFmpeg& ff,
                               const config::GateConfig& cfg,
                               std::optional<double> expected_duration_s) {
    EpisodeGateResult out;
    std::error_code ec;
    if (!fs::is_regular_file(video_path, ec)) {
        return {Verdict::Regress, {SAY("成片文件不存在")}, {}};
    }

    media::MediaInfo info;
    try {
        info = ff.probe(video_path);
    } catch (const media::FFmpegError& e) {
        return {Verdict::Regress, {SAYF("成片读不出来：%1", e.what())}, {}};
    }

    out.metrics["duration_s"] = round_to(info.duration_s, 2);
    if (!info.has_video) out.reasons.push_back(SAY("成片里没有视频轨"));
    if (!info.has_audio) out.reasons.push_back(SAY("成片里没有音轨"));

    if (expected_duration_s.has_value() && *expected_duration_s != 0.0) {
        const double drift = std::abs(info.duration_s - *expected_duration_s);
        out.metrics["duration_drift_s"] = round_to(drift, 2);
        // 整章的容差比单镜宽（5% 对 20%，下限 2 秒对 0.5 秒）：
        // 几十镜拼起来，每镜零点几秒的误差累积是正常的。
        if (drift > std::max(2.0, *expected_duration_s * 0.05)) {
            out.reasons.push_back(
                SAYF("成片时长 %1 秒，期望 %2 秒", fmt("%.1f", info.duration_s),
                     fmt("%.1f", *expected_duration_s)));
        }
    }

    if (info.has_audio) {
        try {
            const auto loud = ff.measure_loudness(video_path);
            const auto get = [&loud](const char* k) {
                const auto it = loud.find(k);
                return it == loud.end() ? 0.0 : it->second;
            };
            const double measured = get("input_i");
            const double peak = get("input_tp");
            out.metrics["lufs"] = round_to(measured, 2);
            out.metrics["true_peak_db"] = round_to(peak, 2);
            if (std::abs(measured - cfg.target_lufs) > 2.0) {
                out.reasons.push_back(
                    SAYF("响度 %1 LUFS，目标 %2，平台可能会自动调整",
                         fmt("%.1f", measured), fmt("%.0f", cfg.target_lufs)));
            }
            if (peak > cfg.max_true_peak_db + 0.5) {
                out.reasons.push_back(SAYF("真峰值 %1 dBTP 偏高，可能削波",
                                           fmt("%.1f", peak)));
            }
        } catch (const media::FFmpegError&) {
            out.reasons.push_back(SAY("测不出响度"));
        }
    }

    out.verdict = out.reasons.empty() ? Verdict::Pass : Verdict::Retry;
    return out;
}

Verdict decide_next(const GateResult& result, const models::Shot& shot,
                    const config::GateConfig& cfg) {
    if (result.ok()) return Verdict::Pass;
    // REGRESS 不看重试次数：重跑也没用，问题在上一阶段。
    if (result.verdict == Verdict::Regress) return Verdict::Regress;
    if (shot.attempts + 1 >= cfg.max_attempts_per_shot) {
        // **降级而不是停下来。** 无人值守跑一晚上，为一镜停住等于
        // 整晚白熬；留着最后那一版至少整章能出片，问题记录下来事后查。
        return cfg.fallback_on_exhausted ? Verdict::Fallback : Verdict::Regress;
    }
    return Verdict::Retry;
}

std::string summarize(const std::vector<GateResult>& results) {
    if (results.empty()) return SAY("没有需要检查的镜头");
    int passed = 0;
    for (const auto& r : results) {
        if (r.ok()) ++passed;
    }
    std::string out = SAYN("闸门检查 %n 个镜头，通过 %1 个",
                           static_cast<long long>(results.size()),
                           std::to_string(passed));
    // 只列没过的。全过时人不需要读四十行"通过"。
    for (const auto& r : results) {
        if (!r.ok()) out += "\n  " + r.describe();
    }
    return out;
}

}  // namespace changji::gates
