#include "media/assemble.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

// 时间轴按真正生成得出来的帧数算，不按分镜表里的名义时长
#include "media/product_tags.hpp"
#include "models/project.hpp"
#include "stages/limits.hpp"
#include "util/say.hpp"
#include "util/cmdline.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::media {

namespace {

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

/// 路径转成 UTF-8 的正斜杠形式。ffmpeg 的清单和滤镜都只认正斜杠。
std::string fwd(const fs::path& p) {
    std::string s = paths::to_utf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

}  // namespace

double Timeline::total_duration_s() const {
    double total = 0.0;
    for (const auto& e : entries) total += e.duration_s;
    return total;
}

std::vector<SubtitleCue> Timeline::cues() const {
    std::vector<SubtitleCue> out;
    for (const auto& e : entries) {
        out.insert(out.end(), e.cues.begin(), e.cues.end());
    }
    return out;
}

Timeline build_timeline(const std::vector<models::Shot>& shots,
                        const models::ProjectPaths& paths,
                        const config::AssemblyConfig& config,
                        const std::function<double(const std::filesystem::path&)>& probe) {
    Timeline timeline;
    double cursor = 0.0;

    for (const auto& shot : shots) {
        if (!shot.video_path.has_value() || shot.video_path->empty()) {
            throw AssemblyError(
                SAYF("镜头 %1 还没有视频，不能装配", shot.shot_id));
        }
        const fs::path video = paths.abs(*shot.video_path);
        std::error_code ec;
        if (!fs::is_regular_file(video, ec)) {
            throw AssemblyError(SAYF("镜头 %1 的视频文件不见了：%2",
                                     shot.shot_id, paths::to_utf8(video)));
        }

        // **不减重叠——因为根本没有重叠。**
        //
        // 这里原来写的是"溶解会让两镜重叠，起点要往回挪"，把非硬切镜头的
        // 起点往前拉 transition_dur_s。可拼接走的是
        // `-f concat -c copy`（下面 concat_args），**纯硬切，全树一处
        // xfade / acrossfade / fade= 都没有**——转场从来没被渲染过。
        //
        // 于是时间线比成片短、字幕比画面早，而且**逐个 dissolve 累积**。
        // 2026-09-13 在 walk_c ep01 上量到的（两镜 dissolve，各 0.4 秒）：
        //
        //     ep01_sh001    画面 0.00   字幕 0.00   差 +0.00
        //     ep01_sh004    画面 13.58  字幕 13.18  差 -0.40
        //     ep01_sh009    画面 31.13  字幕 30.33  差 -0.80
        //     ep01_sh014_b  画面 59.50  字幕 58.70  差 -0.80
        //
        // 每一条字幕单看都"差不多对"，连起来到片尾差了将近一秒。
        //
        // **真要做转场是另一件事**：xfade 要把整条片子重编码一遍
        // （`-c copy` 就没了），画质和时间都要付代价。在那之前，
        // shot.transition_in / transition_dur_s 只是记下来的意图，
        // 时间线不能按它算——**模拟一个不渲染的东西，错的是两处**。
        const double start = cursor;

        // **时间轴按这一镜真正生成了多少帧算，不按分镜表里那个名义值。**
        //
        // 帧数要落在模型的格子上（Wan 4n+1、MiniMax-H3 17k+5），所以名义
        // 4 秒的镜头出来可能是 107 帧 = 4.458 秒。按名义值排的话，每一镜
        // 差的那几百毫秒会**逐镜累积**——十几镜之后字幕和画面能错开好几秒，
        // 而每一段单独看都是对的，最难查的那种。
        double real =
            stages::video_limits().real_duration_s(shot.duration_s, config.fps);
        // 能量到文件就按文件的：名义值对齐到的是**现在**的帧格，而这一镜
        // 可能是在另一份 limits 下出的。
        if (probe) {
            try {
                const double measured = probe(video);
                if (measured > 0.0) real = measured;
            } catch (const std::exception&) {
                // 探不到就按名义值
            }
        }

        TimelineEntry entry;
        entry.shot_id = shot.shot_id;
        entry.video_path = video;
        entry.start_s = start;
        entry.duration_s = real;
        entry.transition_in = shot.transition_in;
        entry.transition_dur_s = shot.transition_dur_s;

        double speech_cursor = start;
        for (const auto& line : shot.dialogue) {
            const double dur = line.actual_duration_s.value_or(0.0);
            if (line.audio_path.has_value() && !line.audio_path->empty()) {
                entry.audio_paths.push_back(paths.abs(*line.audio_path));
            }
            const std::string t = text::strip_ws(line.text);
            if (!t.empty() && dur > 0.0) {
                SubtitleCue cue;
                cue.start_s = speech_cursor;
                cue.end_s = speech_cursor + dur;
                cue.text = t;
                // 没有 char_id 就是旁白，用另一套样式。
                cue.style = line.char_id.has_value() ? "dialogue" : "narration";
                entry.cues.push_back(cue);
            }
            speech_cursor += dur;
        }

        cursor = start + real;
        timeline.entries.push_back(std::move(entry));
    }
    return timeline;
}

std::optional<std::string> episode_of_output(const std::string& stem) {
    if (stem.empty()) return std::nullopt;
    // `ep01_02` → `ep01`；`ep01` → `ep01`
    if (stem.size() >= 4 && stem[stem.size() - 3] == '_' &&
        std::isdigit(static_cast<unsigned char>(stem[stem.size() - 2])) != 0 &&
        std::isdigit(static_cast<unsigned char>(stem.back())) != 0) {
        return stem.substr(0, stem.size() - 3);
    }
    return stem;
}

std::vector<std::string> normalize_args(const fs::path& src, int target_w,
                                        int target_h,
                                        const config::AssemblyConfig& config,
                                        const fs::path& dest) {
    return normalize_args(src, target_w, target_h, config, dest,
                          NormalizeOptions{});
}

std::vector<std::string> normalize_args(const fs::path& src, int target_w,
                                        int target_h,
                                        const config::AssemblyConfig& config,
                                        const fs::path& dest,
                                        const NormalizeOptions& opt) {
    const std::string w = std::to_string(target_w);
    const std::string h = std::to_string(target_h);
    // 先按比例缩到框内，再补边到目标尺寸。直接 scale 到目标尺寸会拉伸，
    // 而竖屏的电影里混进一个横屏镜头时，拉伸出来的人脸一眼就不对。
    std::string vf = "scale=" + w + ":" + h +
                     ":force_original_aspect_ratio=decrease,"
                     "pad=" + w + ":" + h + ":(ow-iw)/2:(oh-ih)/2,"
                     "setsar=1,fps=" + std::to_string(config.fps);
    // 后期链接在缩放补边**后面**：遮幅、颗粒都要按最终尺寸算。
    if (!opt.extra_vf.empty()) vf += "," + opt.extra_vf;
    // 补那一层（绝大多数情况是空的，见 NormalizeOptions::watermark）。
    // 接在后期链**后面**：补的理由之一就是遮幅会裁掉水印，排在它前面等于白补。
    vf = with_watermark(vf, opt.watermark);

    std::vector<std::string> args = {"-y", "-i", paths::to_utf8(src)};
    if (opt.keep_audio && !opt.source_has_audio) {
        // 源里没声音就铺一条静音，让每一镜都有同样规格的音轨。
        // concat 碰到一镜有音轨一镜没有会丢同步，而且不报错。
        args.insert(args.end(),
                    {"-f", "lavfi", "-i",
                     "anullsrc=r=" + std::to_string(config.audio_sample_rate) +
                         ":cl=stereo"});
    }
    args.insert(args.end(), {"-vf", vf});
    if (opt.keep_audio) {
        args.insert(args.end(), {"-map", "0:v", "-map",
                                 opt.source_has_audio ? "0:a" : "1:a",
                                 "-c:a", config.audio_codec,
                                 "-b:a", config.audio_bitrate,
                                 "-ar", std::to_string(config.audio_sample_rate),
                                 "-ac", std::to_string(config.audio_channels)});
        // 静音源是无限长的，截到视频长度正是要的。
        if (!opt.source_has_audio) args.push_back("-shortest");
    } else {
        args.push_back("-an");   // 音频统一在后面处理
    }
    args.insert(args.end(), {
        "-c:v", config.video_codec,
        "-crf", std::to_string(config.crf),
        "-pix_fmt", config.pix_fmt,
        "-preset", "medium",
    });
    if (opt.tune_grain) args.insert(args.end(), {"-tune", "grain"});
    args.insert(args.end(), {
        // 时基也要统一。不统一的话 concat 会在拼接处丢帧，
        // 表现是每一镜的开头卡一下。
        "-video_track_timescale", "90000",
        paths::to_utf8(dest),
    });
    return args;
}

std::string look_filters(const config::LookConfig& look, int target_w,
                         int target_h, const fs::path& project_root) {
    if (!look.enabled()) return {};

    // 调色前的线性一段：遮幅、柔化。
    std::vector<std::string> pre;
    if (look.letterbox > 0.0 && target_w > target_h) {
        // 横屏才遮。裁到比例再补回原高，容器还是 16:9，上下是黑边。
        int lb = static_cast<int>(std::lround(target_w / look.letterbox));
        lb -= lb % 2;
        if (lb > 0 && lb < target_h) {
            const std::string W = std::to_string(target_w);
            const std::string H = std::to_string(target_h);
            const std::string L = std::to_string(lb);
            const std::string y = std::to_string((target_h - lb) / 2);
            pre.push_back("crop=" + W + ":" + L + ":0:" + y);
            pre.push_back("pad=" + W + ":" + H + ":0:" + y);
        }
    }
    if (look.soften > 0.0) {
        pre.push_back("gblur=sigma=" + fmt("%.3g", look.soften));
    }

    // 颗粒最后加：加在柔化前会被柔化抹掉，加在调色前会被曲线改形。
    // 只在亮度通道（c0），逐帧随机（t）、均匀分布（u）。
    std::vector<std::string> post;
    if (look.grain > 0.0) {
        post.push_back("noise=c0s=" + fmt("%.3g", look.grain) + ":c0f=t+u");
    }

    const auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& x : v) {
            if (!out.empty()) out += ",";
            out += x;
        }
        return out;
    };

    if (!look.color() || look.lut_strength <= 0.0) {
        std::vector<std::string> all = pre;
        all.insert(all.end(), post.begin(), post.end());
        return join(all);
    }

    // 调色本体：有 LUT 用 LUT，没有用内置曲线。
    std::string grade;
    if (!look.lut.empty()) {
        fs::path lut = paths::from_utf8(look.lut);
        if (lut.is_relative()) lut = project_root / lut;
        grade = "lut3d=file='" + escape_filter_path(lut) + "':interp=tetrahedral";
    } else {
        // 内置的「胶片」：S 形曲线（暗部压一点、亮部柔和滚落到 0.97，
        // 不顶到 1）、暗部往青、亮部往暖、饱和略降。这不是哪一款胶片，
        // 是行业说的那几样共性——AI 画面缺的就是这几样。
        grade =
            "curves=m='0/0 0.25/0.22 0.5/0.5 0.75/0.78 1/0.97',"
            "colorbalance=rs=-0.05:gs=0:bs=0.06:rm=0.02:gm=0:bm=-0.02:"
            "rh=0.05:gh=0.02:bh=-0.06,"
            "eq=saturation=0.92";
    }

    std::string out = join(pre);
    if (look.lut_strength >= 1.0) {
        if (!out.empty()) out += ",";
        out += grade;
    } else {
        // 分两路：一路调色，再按强度混回原图。blend 的第一路是上层。
        if (!out.empty()) out += ",";
        out += "split[o][g];[g]" + grade + "[g2];[g2][o]blend=all_mode=normal:all_opacity=" +
               fmt("%.3g", look.lut_strength);
    }
    if (!post.empty()) out += "," + join(post);
    return out;
}

/// 清单里那条路径要按 ffmpeg concat 解析器的规矩转义。
///
/// **单引号是硬伤。** 我们写的是 `file '<路径>'`，而在 concat 解析器里
/// 单引号内再遇到 ' 就结束引用——项目名里带一个撇号（don't、
/// Rock'n'Roll），这一行就断在半截，后面那截被当成别的记号。
///
/// 表现是最后装配那一步报一句 ffmpeg 的解析错，而它指的位置离真正的原因
/// （项目叫什么名）十万八千里——**而且是在整章都跑完之后才炸**。
///
/// 规矩：收掉引用、贴一个转义的单引号、再开回引用。反斜杠不用管，
/// fwd() 已经把它全换成正斜杠了。
std::string concat_quote(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (const char c : path) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out;
}

std::string concat_listing(const std::vector<fs::path>& clips) {
    std::string out;
    for (const auto& p : clips) {
        out += "file '" + concat_quote(fwd(p)) + "'\n";
    }
    return out;
}

std::vector<std::string> concat_args(const fs::path& listing,
                                     const fs::path& dest,
                                     const std::vector<std::string>& tags) {
    // -safe 0 是必需的：清单里是绝对路径，默认的安全检查会拒绝。
    std::vector<std::string> args = {"-y",   "-f",  "concat",
                                     "-safe", "0",  "-i",
                                     paths::to_utf8(listing), "-c", "copy"};
    // 标识摆在输出文件名**之前**：-metadata 是输出选项，摆到后面 ffmpeg 会
    // 把它当成下一个输出的选项，而这条命令没有下一个输出。
    args.insert(args.end(), tags.begin(), tags.end());
    args.push_back(paths::to_utf8(dest));
    return args;
}

std::vector<std::string> silent_audio_args(const fs::path& video,
                                           const config::AssemblyConfig& config,
                                           const fs::path& dest) {
    // 这里用 -shortest 是安全的：静音源是无限长的，截到视频长度
    // 正是想要的结果。**下面 mix_args 那条路不能用它**，理由见那边。
    return {
        "-y",
        "-i", paths::to_utf8(video),
        "-f", "lavfi", "-i",
        "anullsrc=r=" + std::to_string(config.audio_sample_rate) + ":cl=stereo",
        "-c:v", "copy",
        "-c:a", config.audio_codec,
        "-b:a", config.audio_bitrate,
        "-ar", std::to_string(config.audio_sample_rate),
        "-ac", std::to_string(config.audio_channels),
        "-shortest",
        paths::to_utf8(dest),
    };
}

std::vector<std::string> mix_args(const fs::path& video,
                                  const std::vector<AudioSegment>& segments,
                                  const config::AssemblyConfig& config,
                                  double target_lufs, double max_true_peak_db,
                                  double video_duration_s, const fs::path& dest) {
    return mix_args(video, segments, config, target_lufs, max_true_peak_db,
                    video_duration_s, dest, MixOptions{});
}

std::vector<std::string> mix_args(const fs::path& video,
                                  const std::vector<AudioSegment>& segments,
                                  const config::AssemblyConfig& config,
                                  double target_lufs, double max_true_peak_db,
                                  double video_duration_s, const fs::path& dest,
                                  const MixOptions& opt) {
    std::vector<std::string> args = {"-y", "-i", paths::to_utf8(video)};
    for (const auto& s : segments) args.insert(args.end(), {"-i", paths::to_utf8(s.path)});
    // 配乐排在台词后面，下标 = 1 + 台词数。
    const std::size_t music_index = 1 + segments.size();
    if (opt.music.has_value()) {
        args.insert(args.end(), {"-i", paths::to_utf8(*opt.music)});
    }
    const std::string rate = std::to_string(config.audio_sample_rate);

    // 每条配音延迟到自己的位置，然后混在一起。
    std::vector<std::string> filters;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        const std::string delay_ms =
            std::to_string(static_cast<long long>(segments[i].at_s * 1000.0));
        // adelay 要给每个声道各一个值，所以是 "ms|ms"。只给一个的话
        // 右声道不延迟，听起来是回声。
        filters.push_back("[" + n + ":a]aresample=" +
                          std::to_string(config.audio_sample_rate) +
                          ",adelay=" + delay_ms + "|" + delay_ms + "[a" + n + "]");
    }

    std::string mix_inputs;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        mix_inputs += "[a" + std::to_string(i + 1) + "]";
    }
    // loudnorm 之后必须再 aresample 一次：这个滤镜内部按 192k 工作，
    // 不收回来的话编码器会挑个 96k 之类的采样率，文件白白变大。
    //
    // %.12g 而不是 %g。**这里和 storyboard.cpp 的 format_g 不是一回事**：
    // 那边对的是 Python 的 f"{x:g}"（也是 6 位有效数字，两边同一套规则），
    // 这边对的是 Python 的 f"{self.target_lufs}"，也就是 str(float)，
    // 不截位。%g 默认 6 位有效数字，target_lufs 填 -16.123456 时
    // C++ 会写出 -16.1235——**响度目标真的变了**，不是显示问题。
    //
    // 剩下一处对不齐是有意留着的：整数值 Python 写 "-16.0"，
    // 这里写 "-16"。ffmpeg 两个都当 -16 解析，成片一模一样；
    // 要逐字节一样得照搬 Python 的 repr 规则（整数浮点补 ".0"），
    // 为一个解析结果相同的字符串背那套算法不值。
    const std::string finish =
        "loudnorm=I=" + fmt("%.12g", target_lufs) +
        ":TP=" + fmt("%.12g", max_true_peak_db) + ":LRA=11,aresample=" +
        rate + "[amixed]";
    const std::string amix_tail = ":dropout_transition=0:normalize=0";

    const bool has_bg = opt.bed || opt.music.has_value();
    if (!has_bg) {
        // 只有台词：和以前逐字节一样的那条链。
        filters.push_back(mix_inputs + "amix=inputs=" +
                          std::to_string(segments.size()) + amix_tail + "," +
                          finish);
    } else {
        // ---- 台词之外的几层 ----
        //
        // 环境声（各镜原生音轨 concat 出来的那条）和配乐先各自压到台词
        // 底下，合成一条底子；有台词时底子再被台词侧链压一道（人一开口
        // 环境声和配乐往下让，说完回来），最后和台词混、归一响度。
        std::vector<std::string> bg_labels;
        if (opt.bed) {
            filters.push_back("[0:a]aresample=" + rate + ",volume=" +
                              fmt("%.12g", opt.bed_db) + "dB[bed]");
            bg_labels.push_back("[bed]");
        }
        if (opt.music.has_value()) {
            filters.push_back("[" + std::to_string(music_index) +
                              ":a]aresample=" + rate + ",volume=" +
                              fmt("%.12g", opt.music_db) + "dB[mus]");
            bg_labels.push_back("[mus]");
        }
        std::string bg = bg_labels.front();
        if (bg_labels.size() == 2) {
            filters.push_back("[bed][mus]amix=inputs=2" + amix_tail + "[bg]");
            bg = "[bg]";
        }
        if (segments.empty()) {
            filters.push_back(bg + finish);
        } else {
            filters.push_back(mix_inputs + "amix=inputs=" +
                              std::to_string(segments.size()) + amix_tail +
                              "[dlg]");
            if (opt.duck) {
                // sidechaincompress 会吃掉侧链那一路，所以台词要分两份：
                // 一份当侧链，一份进最后的混音。
                filters.push_back("[dlg]asplit[dlg1][dlg2]");
                filters.push_back(bg + "[dlg2]sidechaincompress=threshold=0.03:"
                                       "ratio=8:attack=20:release=400[bgd]");
                filters.push_back("[bgd][dlg1]amix=inputs=2" + amix_tail + "," +
                                  finish);
            } else {
                filters.push_back(bg + "[dlg]amix=inputs=2" + amix_tail + "," +
                                  finish);
            }
        }
    }
    // 音轨补静音到视频长度。
    filters.push_back("[amixed]apad[aout]");

    std::string chain;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) chain += ";";
        chain += filters[i];
    }

    args.insert(args.end(), {
        "-filter_complex", chain,
        "-map", "0:v", "-map", "[aout]",
        "-c:v", "copy",
        "-c:a", config.audio_codec, "-b:a", config.audio_bitrate,
        "-ar", std::to_string(config.audio_sample_rate),
        "-ac", std::to_string(config.audio_channels),
        // **以视频为准截断补出来的静音尾巴，而不是 -shortest。**
        // 配音总长几乎总是短于画面（无对白的镜头没有音频），
        // -shortest 会把成片截到配音那么长，后面的画面直接丢掉。
        "-t", fmt("%.3f", video_duration_s),
        paths::to_utf8(dest),
    });
    return args;
}

std::vector<std::string> burn_args(const fs::path& video,
                                   const fs::path& ass_path,
                                   const config::AssemblyConfig& config,
                                   const fs::path& dest) {
    return {
        "-y",
        "-i", paths::to_utf8(video),
        "-vf", "subtitles='" + escape_filter_path(ass_path) + "'",
        "-c:v", config.video_codec,
        "-crf", std::to_string(config.crf),
        "-pix_fmt", config.pix_fmt,
        "-preset", "medium",
        // 音频原样拷贝。重编码一次是白白多一次有损压缩，
        // 而烧字幕根本不碰音轨。
        "-c:a", "copy",
        paths::to_utf8(dest),
    };
}

int assembly_watermark_upscale(const config::LookConfig& look,
                               const config::UpscaleConfig& upscale,
                               int target_w, int target_h) {
    if (look.enabled() && look.letterbox > 0.0 && target_w > target_h) return 1;
    if (upscale.enabled()) return std::max(1, upscale.scale);
    return 0;
}

std::string escape_filter_path(const fs::path& path) {
    std::string text = fwd(path);
    std::string out;
    for (const char c : text) {
        // 冒号是滤镜的参数分隔符，要转义。反斜杠上面已经换成正斜杠了。
        if (c == ':') {
            out += '\\';
            out += c;
            continue;
        }
        // **单引号 2026-09-11 才补上。**
        //
        // 调用方写的是 -vf subtitles='<路径>'，路径里再出现一个单引号
        // 就提前收尾，后面的字变成滤镜的其它参数，ffmpeg 报一句语法错。
        //
        // 以前不修是有理由的：Python 侧那个 _escape_filter_path 也只做了
        // \\ -> / 和 : -> \\:，单方面改会让对拍多一条"不同"。当时写的是
        // "真要修就两边一起修（阶段 8 删掉 Python 之后就只剩一侧）"——
        // **Python 已经删了，那个前提到了**。而且 concat 清单那边刚补过
        // 同样的转义（见 concat_quote），两处不一致本身也是个坑。
        //
        // 规矩和 concat 那边一样：收掉引用、贴一个转义的单引号、再开回引用。
        if (c == '\'') {
            out += "'\\''";
            continue;
        }
        out += c;
    }
    return out;
}

std::vector<std::string> subtitle_problems(const Timeline& timeline,
                                           const config::AssemblyConfig& config) {
    return validate_cues(timeline.cues(), config.subtitle_max_chars_per_line);
}

// ---- Assembler ----

Assembler::Assembler(const FFmpeg& ff, config::AssemblyConfig config,
                     models::ProjectPaths paths, double target_lufs,
                     double max_true_peak_db)
    : ff_(ff),
      config_(std::move(config)),
      paths_(std::move(paths)),
      target_lufs_(target_lufs),
      max_true_peak_db_(max_true_peak_db) {}

std::pair<int, int> Assembler::target_size(const Timeline& timeline) const {
    int bw = 0, bh = 0;
    for (const auto& e : timeline.entries) {
        try {
            const MediaInfo info = ff_.probe(e.video_path);
            if (info.width * info.height > bw * bh) {
                bw = info.width;
                bh = info.height;
            }
        } catch (const FFmpegError&) {
            // 一镜读不出来不该让整章装不了：后面还有几十镜，
            // 取最大值这件事少一个样本没关系。
            continue;
        }
    }
    if (bw == 0 && bh == 0) throw AssemblyError(SAY("所有镜头都读不出分辨率"));
    // 必须是偶数，否则 yuv420p 编不了——而报错是编码器内部的，很难懂。
    return {bw - bw % 2, bh - bh % 2};
}

fs::path Assembler::assemble(const Timeline& timeline,
                             const std::string& out_name, bool burn_subtitles) {
    if (timeline.entries.empty()) {
        throw AssemblyError(SAY("时间线是空的，没有可装配的镜头"));
    }

    const fs::path work = paths_.output() / ".work";
    std::error_code ec;
    fs::create_directories(work, ec);

    // 中间文件不管成功失败都要清掉：一章的中间产物是几百兆，
    // 留在 output/.work 里几次之后磁盘就满了，而用户看不到那个目录。
    struct Cleanup {
        const fs::path& dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{work};

    auto [tw, th] = target_size(timeline);

    const auto warn = [this](const std::string& msg) {
        if (finish_ && finish_->warn) finish_->warn(msg);
    };

    // ---- 放大 ----
    //
    // 放大在调色和颗粒**之前**（行业顺序：放大 → 校正 → LUT → 颗粒），
    // 所以目标尺寸先按倍数放大，每一镜先过放大命令再进 normalize。
    const bool upscaling = finish_ && finish_->upscale.enabled();
    if (upscaling) {
        tw *= finish_->upscale.scale;
        th *= finish_->upscale.scale;
        tw -= tw % 2;
        th -= th % 2;
    }

    // ---- 后期链 ----
    std::string extra_vf;
    if (finish_) {
        extra_vf = look_filters(finish_->look, tw, th, finish_->project_root);
    }

    // ---- 补水印 ----
    //
    // 平时不补（回 0）：水印在单镜出片那一步就烧好了。判据和理由都在
    // `assembly_watermark_upscale` 上。
    WatermarkPlan watermark;
    if (finish_) {
        const int up = assembly_watermark_upscale(finish_->look,
                                                  finish_->upscale, tw, th);
        if (up > 0) {
            watermark = stage_watermark(work / "cj_watermark.png", tw, th, up);
        }
    }

    std::vector<fs::path> normalized;
    for (std::size_t i = 0; i < timeline.entries.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "norm_%04zu.mp4", i);
        const fs::path out = work / name;
        const TimelineEntry& entry = timeline.entries[i];

        fs::path src = entry.video_path;
        if (upscaling) {
            char up_name[32];
            std::snprintf(up_name, sizeof(up_name), "up_%04zu.mp4", i);
            const fs::path up = work / up_name;
            const std::map<std::string, std::string> vars = {
                {"in", paths::to_utf8(src)},
                {"out", paths::to_utf8(up)},
                {"width", std::to_string(tw)},
                {"height", std::to_string(th)},
                {"short", std::to_string(std::min(tw, th))},
                {"scale", std::to_string(finish_->upscale.scale)},
            };
            const auto argv = util::expand_command(finish_->upscale.command, vars);
            const auto r = util::run_command(argv, finish_->upscale.timeout_s);
            std::error_code up_ec;
            if (r.ok && fs::is_regular_file(up, up_ec)) {
                src = up;
            } else {
                // 放大失败不拦装配：这一镜用原片，normalize 会把它拉到
                // 目标尺寸（糊一点，但片子在）。说一声是必须的。
                warn(SAYF("镜头 %1 放大失败，用原片顶上：%2", entry.shot_id,
                          r.ok ? SAY("命令跑完了但没有产出文件") : r.error));
            }
        }

        NormalizeOptions opt;
        opt.watermark = watermark;      // 平时是空的，见上
        if (finish_) {
            opt.extra_vf = extra_vf;
            opt.keep_audio = finish_->sound.ambient;
            opt.tune_grain = finish_->look.enabled() && finish_->look.grain > 0.0;
            if (opt.keep_audio) {
                try {
                    opt.source_has_audio = ff_.probe(src).has_audio;
                } catch (const FFmpegError&) {
                    opt.source_has_audio = false;
                }
            }
        }
        ff_.run(normalize_args(src, tw, th, config_, out, opt));
        normalized.push_back(out);
    }

    const fs::path listing = work / "concat.txt";
    {
        std::ofstream f(listing, std::ios::binary | std::ios::trunc);
        if (!f) {
            throw AssemblyError(
                SAYF("写不了拼接清单：%1", paths::to_utf8(listing)));
        }
        f << concat_listing(normalized);
    }
    const fs::path silent = work / "joined.mp4";
    // 隐式标识就写在这一步——从这儿往下游三步都带着，见 concat_args 上那段。
    // 章的片名这一层不知道（只有一个 out_name 那样的文件名），空着；
    // 整部电影那头（pipeline/film_join）拿得到项目名，会填上。
    ff_.run(concat_args(listing, silent,
                        product_tag_args(models::utc_now_iso8601())));

    // 每条配音落在时间线上的位置。
    std::vector<AudioSegment> segments;
    for (const auto& entry : timeline.entries) {
        double cursor = entry.start_s;
        for (std::size_t i = 0; i < entry.audio_paths.size(); ++i) {
            segments.push_back({entry.audio_paths[i], cursor});
            cursor += i < entry.cues.size() ? entry.cues[i].duration_s() : 0.0;
        }
    }

    // ---- 环境声和配乐 ----
    MixOptions mo;
    if (finish_) {
        const MediaInfo joined = ff_.probe(silent);
        // 每一镜 normalize 时都带了音轨（没有的铺了静音），所以拼出来的
        // 那条就是环境声底子。探一下是为了保险：探不到就当没有。
        mo.bed = finish_->sound.ambient && joined.has_audio;
        mo.bed_db = finish_->sound.ambient_db;
        mo.duck = finish_->sound.duck;
        if (finish_->sound.music && finish_->music.has_value()) {
            std::error_code m_ec;
            if (fs::is_regular_file(*finish_->music, m_ec)) {
                mo.music = *finish_->music;
                mo.music_db = finish_->sound.music_db;
            } else {
                warn(SAYF("配乐文件不在，这一章没有配乐：%1",
                          paths::to_utf8(*finish_->music)));
            }
        }
    }

    const fs::path with_audio = work / "with_audio.mp4";
    if (segments.empty() && !mo.bed && !mo.music.has_value()) {
        ff_.run(silent_audio_args(silent, config_, with_audio));
    } else {
        const double dur = ff_.probe(silent).duration_s;
        ff_.run(mix_args(silent, segments, config_, target_lufs_,
                         max_true_peak_db_, dur, with_audio, mo));
    }

    const fs::path final_path = paths_.output() / paths::from_utf8(out_name);
    fs::create_directories(final_path.parent_path(), ec);

    const auto cues = timeline.cues();
    // **有字幕就先写出 .ass，不管烧不烧。**
    //
    // 烧不了的时候（这台的 ffmpeg 没编 libass，见 run_assemble 里那一段）
    // 外挂字幕是唯一的出路——那时候更需要这个文件。原来它写在「要烧」
    // 这个分支里，不烧就连文件都没有，等于字幕整个丢了。
    bool burn = false;
    if (!cues.empty()) {
        const MediaInfo info = ff_.probe(with_audio);
        AssOptions opt;
        opt.width = info.width > 0 ? info.width : 1080;
        opt.height = info.height > 0 ? info.height : 1920;
        opt.font = config_.subtitle_font;
        opt.max_chars_per_line = config_.subtitle_max_chars_per_line;
        opt.max_lines = config_.subtitle_max_lines;

        // **字幕跟着成片的相对位置走。**
        //
        // `out_name` 可以带子目录（预告是 `preview/ep01.mp4`，见
        // pipeline::AssembleOptions），字幕就落在 `subtitles/preview/` 下。
        // 不跟着走的话两条片子的字幕在 `subtitles/` 里同名，后写的那份把
        // 先写的盖掉——播放器挂上去的是**另一条片子**的字幕，而时间戳看着
        // 都挺像，没有任何一处会报错。
        const fs::path ass =
            paths_.subtitles() / fs::path(paths::from_utf8(out_name)).parent_path() /
            paths::from_utf8(paths::to_utf8(final_path.stem()) + ".ass");
        fs::create_directories(ass.parent_path(), ec);
        write_ass(ass, cues, opt);
        if (burn_subtitles) {
            ff_.run(burn_args(with_audio, ass, config_, final_path));
            burn = true;
        }
    }
    if (!burn) {
        // 没字幕就直接搬过去，不重编码一遍——那是白白多一次有损压缩。
        fs::remove(final_path, ec);
        fs::rename(with_audio, final_path, ec);
        if (ec) {
            // 跨盘 rename 会失败（工作目录和输出目录理论上同在项目里，
            // 但用户可能把 output 做成了符号链接）。退回复制。
            fs::copy_file(with_audio, final_path,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                throw AssemblyError(SAYF("成片挪不到 %1：%2",
                                         paths::to_utf8(final_path),
                                         ec.message()));
            }
        }
    }
    return final_path;
}

}  // namespace changji::media
