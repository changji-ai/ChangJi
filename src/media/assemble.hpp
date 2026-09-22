#pragma once

// 把一章的镜头拼成这一章的成片。
//
// 移植自 src/changji/assembly/assemble.py。
//
// ---
//
// **ffmpeg 的参数拼成什么样，这里当成契约来测。**
//
// 和 sd_video 的 encode_args 是同一个理由：参数错了**不会当场报错**，
// 只会让成片在某些播放器上打不开、音画错位、或者中间丢掉一整个镜头。
// 那要等到成片出来、有人从头看一遍才发现，而一章是三分钟。
//
// 所以每一步的参数都是一个纯函数，Assembler 只负责按顺序调它们。

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "media/ffmpeg.hpp"
#include "media/subtitles.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"

namespace changji::media {

class AssemblyError : public std::runtime_error {
public:
    explicit AssemblyError(const std::string& what) : std::runtime_error(what) {}
};

/// 时间线上的一个镜头。
struct TimelineEntry {
    std::string shot_id;
    std::filesystem::path video_path;
    double start_s = 0.0;
    double duration_s = 0.0;
    models::Transition transition_in = models::Transition::CUT;
    double transition_dur_s = 0.0;
    std::vector<std::filesystem::path> audio_paths;
    std::vector<SubtitleCue> cues;
};

/// 一章的时间线。分镜表加配音时长推出来的排期。
struct Timeline {
    std::vector<TimelineEntry> entries;

    double total_duration_s() const;
    std::vector<SubtitleCue> cues() const;
};

/// 由分镜表推出时间线。
///
/// **字幕的时间戳来自配音的真实时长，不是估算。** 这是音画对齐的最后一环——
/// 用估算时长的话，一章下来字幕会越飘越远，而每一条单看都"差不多对"。
///
/// `probe` 给了就按 mp4 **实测**长度排时间轴；没给按名义值对齐到帧格算。
/// 后者是进程全局的 limits 算的，跑完之后可能被别的项目重算过（改了
/// max_shot_s、换了画幅），时间轴就和文件长度分叉，字幕逐镜累积漂移。
Timeline build_timeline(
    const std::vector<models::Shot>& shots, const models::ProjectPaths& paths,
    const config::AssemblyConfig& config,
    const std::function<double(const std::filesystem::path&)>& probe = {});

/// 一个成片文件名（不带扩展名）是哪一章的。不是成片就回空。
///
/// **`_NN` 是历史留下的命名。** 2026-09-18 之前一章会按「每集 N 秒」在章内
/// 切成几段，切出来的叫 `ep01_01` / `ep01_02`；不切的那次叫 `ep01`。今天
/// 一章只出一个 `<episode_id>.mp4`，整部电影也只出一个 `成片.mp4`，**再也
/// 不产出 `_NN`**。留着认它，是因为老项目磁盘上还躺着那种名字，而三处都要
/// 认得出 `ep01_02.mp4` 是 ep01 的：
///
/// · `pipeline/film_join.cpp` 的 `films_of`——**最要紧的一处**。
///   `film_join_blocker` 和 `join_film` 靠它答「这一章出片了没有」。改成
///   精确匹配的话，老项目里切过的章当场变成没出片：要么整部电影被判
///   「还没有一章出片」合不了，要么被静静塞进 `report.skipped`，人看着
///   合成成功、电影里少了几章。（`tests/unit/test_film_join.cpp` 那条
///   「老项目切过的章也算出了片」钉的就是这个。）
/// · `pipeline/episode.cpp`：装配前清掉这一章的老成片，孤儿留在盘上会被
///   上面那两处数进去。
/// · `http/readonly.cpp`：一章出了几个成片文件，页面上那个数。
///
/// 判法**不用子串**：短 id（`ep`、`e` 这种，建章接口不拦长度）拿子串会
/// 命中所有 epNN。要么整个名字就是那个 id，要么是 id + '_' + 两位数字。
std::optional<std::string> episode_of_output(const std::string& stem);

// ---- 各步的参数。纯函数，测得死。 ----

/// 统一编码规格。
///
/// **拼接环节最容易踩的坑。** 分辨率、帧率、像素格式、采样宽高比、时基
/// 任何一项不齐都会导致花屏或丢帧，而 concat 分离器不会为此报错。
std::vector<std::string> normalize_args(const std::filesystem::path& src,
                                        int target_w, int target_h,
                                        const config::AssemblyConfig& config,
                                        const std::filesystem::path& dest);

/// 后期链的滤镜串（柔化 → 调色 → 颗粒，横屏还有遮幅），接在缩放补边后面。
///
/// 空串 = 什么都不加（preset = off）。`target_w/h` 是补边后的尺寸，遮幅
/// 按它算；`project_root` 用来解析相对的 LUT 路径。
///
/// 内置调色是一组 ffmpeg 原生滤镜（S 形曲线、暗部偏青亮部偏暖、略去饱和），
/// 不是哪个牌子的胶片——想要 Kodak 2383 那种就填 `lut`。两条路都按
/// `lut_strength` 和原图混（split → 调色 → blend）。
std::string look_filters(const config::LookConfig& look, int target_w,
                         int target_h,
                         const std::filesystem::path& project_root);

/// 统一规格那一步的可选项。默认值 = 2026-09-14 之前的行为，逐字节一样。
struct NormalizeOptions {
    /// look_filters 的结果。
    std::string extra_vf;
    /// 带上源里的音轨（模型自己出的环境声）。源里没有就铺一条静音——
    /// 每一镜都得有音轨，不然 concat 拼到没有的那镜会丢同步。
    bool keep_audio = false;
    bool source_has_audio = false;
    /// 编码时 `-tune grain`：不加的话 x264 把颗粒当噪声抹掉，白做。
    bool tune_grain = false;
};

std::vector<std::string> normalize_args(const std::filesystem::path& src,
                                        int target_w, int target_h,
                                        const config::AssemblyConfig& config,
                                        const std::filesystem::path& dest,
                                        const NormalizeOptions& opt);

/// concat 清单文件的内容。路径一律正斜杠。
std::string concat_listing(const std::vector<std::filesystem::path>& clips);

/// 拼接。**零重编码**——上一步已经把规格统一了。
///
/// `tags` 是产物的隐式标识（`media::product_tags.hpp`），空 = 不写，参数和
/// 加它之前逐字节一样。
///
/// **这一步是写标识的唯一入口。** `-f concat` 不继承输入文件的元数据（实测：
/// 拼完一栏不剩），所以这里写的是新的；而从这儿往下游——混音那两条路
/// （`silent_audio_args` / `mix_args`，`-c:v copy`）、烧字幕（重编码）、
/// 不烧字幕时的直接改名——**三步都原样带着**，实测过。
///
/// 反过来说：别在下游补写。下游每一层都是两条路，写一处就漏一条，
/// 而漏掉的表现是「某些片子没有标识」，谁都不报错。
std::vector<std::string> concat_args(const std::filesystem::path& listing,
                                     const std::filesystem::path& dest,
                                     const std::vector<std::string>& tags = {});

/// 没有配音时也要有音轨，否则有些平台会认为文件损坏。
std::vector<std::string> silent_audio_args(const std::filesystem::path& video,
                                           const config::AssemblyConfig& config,
                                           const std::filesystem::path& dest);

/// 一段配音在时间线上的位置。
struct AudioSegment {
    std::filesystem::path path;
    double at_s = 0.0;
};

/// 混音并做响度归一。
///
/// **绝不能用 -shortest**：配音总长几乎总是短于画面（无对白的镜头没有音频），
/// 用它会把成片截到配音那么长，后面的画面直接丢掉。
/// 实测一次四镜头的片子丢了整整一个镜头。
std::vector<std::string> mix_args(const std::filesystem::path& video,
                                  const std::vector<AudioSegment>& segments,
                                  const config::AssemblyConfig& config,
                                  double target_lufs, double max_true_peak_db,
                                  double video_duration_s,
                                  const std::filesystem::path& dest);

/// 台词之外的几层。默认值 = 只混台词，滤镜链和以前逐字节一样。
struct MixOptions {
    /// 视频自带的音轨当环境声底子（各镜的原生音轨 concat 起来的那条）。
    bool bed = false;
    double bed_db = -12.0;
    /// 一条配乐。
    std::optional<std::filesystem::path> music;
    double music_db = -20.0;
    /// 台词处把底子再压一道（sidechaincompress）。
    bool duck = true;
};

/// 同上，多了环境声和配乐两层。台词那几路的处理一个字没变。
std::vector<std::string> mix_args(const std::filesystem::path& video,
                                  const std::vector<AudioSegment>& segments,
                                  const config::AssemblyConfig& config,
                                  double target_lufs, double max_true_peak_db,
                                  double video_duration_s,
                                  const std::filesystem::path& dest,
                                  const MixOptions& opt);

/// 烧字幕。
std::vector<std::string> burn_args(const std::filesystem::path& video,
                                   const std::filesystem::path& ass_path,
                                   const config::AssemblyConfig& config,
                                   const std::filesystem::path& dest);

/// 把路径转成 ffmpeg 滤镜能接受的形式。
///
/// Windows 上 `C:\x` 里的冒号是滤镜的参数分隔符，反斜杠是转义符，
/// 直接传进去滤镜解析失败——而报错信息是滤镜语法错误，
/// 看不出根因是路径。
std::string escape_filter_path(const std::filesystem::path& path);

/// 装配前先查一遍字幕。
std::vector<std::string> subtitle_problems(const Timeline& timeline,
                                           const config::AssemblyConfig& config);

/// 成片那几道「让它像电影」的工序：放大、后期链、声音几层。
///
/// **不给就是老行为**（`concat -c copy`、只混台词）。给了按各自的开关来。
struct FinishOptions {
    config::LookConfig look;
    config::SoundConfig sound;
    config::UpscaleConfig upscale;
    /// 解析相对路径（LUT）用。
    std::filesystem::path project_root;
    /// 这一章的配乐文件，没有就不混。
    std::optional<std::filesystem::path> music;
    /// 放大失败、配乐文件不在这类**不该拦装配**的事从这儿说出去。
    std::function<void(const std::string&)> warn;
};

/// 把一章的镜头拼成这一章的成片。
class Assembler {
public:
    Assembler(const FFmpeg& ff, config::AssemblyConfig config,
              models::ProjectPaths paths, double target_lufs = -16.0,
              double max_true_peak_db = -1.5);

    /// 装配时做后期链、放大、环境声和配乐。见 FinishOptions。
    void set_finish(FinishOptions finish) { finish_ = std::move(finish); }

    /// 装配成片，返回成片路径。
    ///
    /// `out_name` 相对 `output/`，**可以带子目录**（预告走
    /// `preview/ep01.mp4`）。外挂字幕落在 `subtitles/` 下**同样的子目录**里，
    /// 同名换后缀——两条片子的字幕才不会在一个目录里撞名字。
    std::filesystem::path assemble(const Timeline& timeline,
                                   const std::string& out_name = "episode.mp4",
                                   bool burn_subtitles = true);

    /// 取最大的那个分辨率作为统一规格，**避免放大模糊**。
    std::pair<int, int> target_size(const Timeline& timeline) const;

private:
    const FFmpeg& ff_;
    config::AssemblyConfig config_;
    models::ProjectPaths paths_;
    /// 响度目标从闸门配置传进来。
    ///
    /// 以前这里写死 -16，界面上改了响度目标其实一点用没有，
    /// 成片还是老样子——一个只写不读的开关，和 _TIER_OVERRIDES 同一类问题。
    double target_lufs_;
    double max_true_peak_db_;
    std::optional<FinishOptions> finish_;
};

}  // namespace changji::media
