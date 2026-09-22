#pragma once

// ffmpeg 与 ffprobe 的封装。
//
// 装配、闸门、抽帧都要用。移植自 src/changji/assembly/ffmpeg.py。
//
// ---
//
// **解析和执行是分开的。** 跑子进程要真有 ffmpeg 才行，而这一层真正容易
// 错的是**解析**：ffprobe 的 JSON 里帧率写成 "24000/1001" 这种分数，
// signalstats 的输出是一堆 key=value 的行，loudnorm 把 JSON 打在 stderr 里
// 混在日志中间。这几个解析器全是纯函数，喂真实输出的样本就能测死。
//
// 解析错了的表现都很隐蔽：帧率算成 0 会让时长校验永远通过；
// 展布取错字段会让所有画面都被判成纯色，于是整章重跑还是全被拒。

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace changji::media {

class FFmpegError : public std::runtime_error {
public:
    explicit FFmpegError(const std::string& what) : std::runtime_error(what) {}
};

/// 找不到 ffmpeg。装配环节的硬依赖。
class FFmpegMissing : public FFmpegError {
public:
    explicit FFmpegMissing(const std::string& what) : FFmpegError(what) {}
};

/// 一个媒体文件的关键信息。
struct MediaInfo {
    std::filesystem::path path;
    double duration_s = 0.0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int frames = 0;
    bool has_video = false;
    bool has_audio = false;
    std::string pix_fmt;
    std::string sar;
};

/// 一帧的亮度统计。用来判断画面是不是纯色或全黑。
///
/// **展布用 YHIGH 减 YLOW**，也就是第 90 和第 10 百分位之差，不是极差。
/// 极差会被单个亮点或暗点带偏：一张几乎全黑但有一个高光点的废图，
/// 极差能到 250，看起来很正常。百分位之差不吃这一套。
struct PixelStats {
    double mean = 0.0;
    double spread = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double low = 0.0;
    double high = 0.0;

    /// 近乎纯色。生成失败最常见的表现。
    ///
    /// 阈值取 8。实测正常画面的展布在 40 以上，纯色的在个位数。
    ///
    /// **它分不开"纯色"和"很暗"，这是已知的。** 2026-09-13 实测
    /// （walk_c ep01_sh004，提示词是「车内光线昏暗，只有手机屏幕发出的
    /// 蓝光映照在脸上」）：
    ///
    ///     首帧          YMIN 16  YLOW 16  YAVG 23.8  YHIGH 33  YMAX 197   展布 17（过）
    ///     出的视频      YMIN  7  YLOW 16  YAVG 19.3  YHIGH 21  YMAX 184   展布 5（判纯色）
    ///
    /// 图是对的——暗车厢里蓝光打在侧脸上，车窗有雨痕，YMAX 184 就是那块
    /// 屏幕。只是九成像素挤在暗部，p10~p90 自然窄。重试三次都这样，
    /// 最后降级——那一版视频留下来了，也进了成片。
    ///
    /// **试过用极差分开，不行。** 语料里 blank_with_hotspot 那条是
    /// mean 127.2 / 展布 4 / 16~250 的中灰均匀废图，极差和上面那张合法
    /// 暗场一样大；而它均值 127，looks_clipped（mean < 6）也抓不到。
    /// 低端距离、高端尾巴、两侧尾巴几种判据都能分开这两个例子，但那是
    /// 拿两个点拟合规则——而误放一个真废图（成片里一段纯色）比误杀一个
    /// 暗镜头（降级，画面还在）代价大。
    ///
    /// 所以判据不动，改成把均值一并报给用户（见 gates/checks.cpp）：
    /// 展布小 + 均值也低 = 大概率是暗场而不是废图，人一眼能判。
    /// 真要拍大量暗戏，这个 8 该变成配置项——但那个数应该按素材定，
    /// 不该按一次观察拍。
    bool looks_blank() const { return spread < 8.0; }

    /// 整体过曝或全黑。
    bool looks_clipped() const { return mean < 6.0 || mean > 249.0; }
};

// ---- 纯函数。喂真实输出就能测。 ----

/// 解析成浮点。解析不了返回 0，不抛——ffprobe 对某些容器会给
/// "N/A" 或者干脆不给这个字段，那不是错误。
double to_double(const std::string& v);

/// 帧率。ffprobe 给的是 "24/1"、"24000/1001" 这种分数。
///
/// **不能直接 stod**："24000/1001" 会被解析成 24000，也就是把
/// NTSC 的 23.976 当成 24000 fps。那之后所有按帧率算的时长全错。
double parse_fps(const std::string& rate);

/// 取样位置（占总时长的比例）。**避开首尾各 10%**，那里常有编码伪影。
std::vector<double> sample_points(int samples);

/// 解析 ffprobe 的 -show_format -show_streams JSON。
MediaInfo parse_probe(const std::string& json_text,
                      const std::filesystem::path& path);

/// 解析 signalstats 的输出。
///
/// **signalstats 不输出标准差。** YDIF 是相邻帧差异，单帧模式下根本不产生，
/// 拿它当标准差会让所有画面都被判成纯色——于是整章重跑，还是全被拒。
PixelStats parse_signalstats(const std::string& text);

/// 从 loudnorm 的输出里抠出那段 JSON。
///
/// ffmpeg 把它打在 **stderr** 上，混在一堆日志中间，所以要从最后一个
/// 大括号对里取。只留 input_/output_/target_ 开头的键。
std::map<std::string, double> parse_loudnorm(const std::string& stderr_text);

// ---- 执行 ----

/// 跑一个子进程。
///
/// **stdout 和 stderr 是合并的**，这不是偷懒：ffmpeg 把有用的东西
/// 两边都放——signalstats 走 stdout，loudnorm 的 JSON 走 stderr，
/// 版本信息也在 stderr。分开读要为每种调用记住"该看哪边"，
/// 而记错的表现是"解析不到结果"，看不出是读错了流。
struct ProcResult {
    int exit_code = 0;
    std::string out;
    /// 进程有没有起来。区分"跑了但失败"和"根本没这个程序"——
    /// 后者要说"去装 ffmpeg"，前者要把 ffmpeg 自己的报错贴出来。
    bool launched = false;

    /// 超时被杀掉了。
    ///
    /// 从 2026-09-08 起 `proc::run` 真的会超时杀进程（原来 popen 没这个
    /// 能力，超时参数是摆设）。**能力一旦生效，就得能报出来**——
    /// 不然一次跑了半小时被杀掉的编码，看到的是"ffmpeg 退出码 1"
    /// 加一段被截断的日志，人会以为是编码参数不对。
    bool timed_out = false;
};

/// 一段片子的运动量，见 FFmpeg::motion_stats。frames = 0 表示没量到。
struct MotionStats {
    int frames = 0;
    double mean = 0.0;
    double median = 0.0;
    double max = 0.0;
};

using Runner = std::function<ProcResult(const std::string& exe,
                                        const std::vector<std::string>& args,
                                        double timeout_s)>;

/// 真正起子进程的那个。实现走 util/proc。
Runner default_runner();

class FFmpeg {
public:
    FFmpeg(std::string ffmpeg_exe, std::string ffprobe_exe, Runner runner);

    /// 缺了要在跑之前就说清楚，而不是跑到装配才炸。
    void check() const;
    bool available() const;

    /// 这个 ffmpeg 有没有编进某个滤镜。
    ///
    /// **2026-09-16 实测撞到：`brew install ffmpeg` 出来的 9.0.1 没有 libass**，
    /// 也就没有 `subtitles` 滤镜。而整章装配的最后一步是烧字幕，于是一章画面
    /// 全出好了、装配却整个失败，输出目录空的。报的还是一句误导的
    /// 「No option name near '/Users/…'」——人会以为是路径里的空格。
    ///
    /// 有就烧，没有就把字幕留成外挂 .ass，片子照出。查一次缓存住：
    /// `-h filter=xxx` 一次几十毫秒，而装配里每一章都要问。
    ///
    /// **缓存跟着这个实例走，不是全局静态。** 全局的话「换一个 ffmpeg 路径
    /// 再问」拿到的是上一个的答案——单元测试里两个桩 runner 就撞上了，
    /// 而真实场景是用户在设置页改了 assembly.ffmpeg_path。拷贝共享同一份
    /// （拷的是同一个二进制），所以放在 shared_ptr 里。
    bool has_filter(const std::string& name) const;

    MediaInfo probe(const std::filesystem::path& p) const;

    /// 取一帧的亮度统计。
    ///
    /// 用 ffmpeg 的 signalstats 滤镜算，不在进程里解码——
    /// 省掉一个图像库依赖，而交叉编译到树莓派时那是一笔实在的成本。
    PixelStats pixel_stats(const std::filesystem::path& p,
                           std::optional<double> at_second = std::nullopt) const;

    /// 在片段的几个位置取样。**只看一帧会漏掉中途崩坏的镜头。**
    std::vector<PixelStats> sample_pixel_stats(const std::filesystem::path& p,
                                               int samples = 3) const;

    /// 整段片子的运动量：相邻帧的平均绝对差（缩到 256 宽、灰度，0~255）。
    ///
    /// **亮度取样看不出这两种废片**（2026-09-13 实测）：
    ///   * 几乎不动的——项目 321 的四镜成片，画面很清楚（展布正常），
    ///     相邻帧差均值只有 0.27～0.66；能动的镜头在 2.7～3.6；
    ///   * 中途硬切成另一场戏的——8 秒那镜均值 3.5、中位 2.2、**最大 45**。
    /// 一个数分开第一种（均值），一个数分开第二种（最大值对中位数）。
    ///
    /// 走 ffmpeg 的 tblend=difference + signalstats，不在进程里解码，
    /// 和 pixel_stats 一个路数；192 帧的片子 0.4 秒。老 ffmpeg 没有
    /// tblend 的话抛 FFmpegError，调用方按"量不到"处理，别让闸门因此失败。
    MotionStats motion_stats(const std::filesystem::path& p) const;

    /// 测响度。两遍归一法的第一遍。
    std::map<std::string, double> measure_loudness(
        const std::filesystem::path& p) const;

    /// 抽一帧存成图片。首尾帧比对和"视频模型出单帧"那条退路都要用。
    std::filesystem::path extract_frame(const std::filesystem::path& p,
                                        const std::filesystem::path& dest,
                                        double at_second = 0.0) const;

    /// 直接跑一串参数。装配那边要拼很长的滤镜链，包不完。
    std::string run(const std::vector<std::string>& args,
                    double timeout_s = 1800.0) const;

private:
    std::string run_exe(const std::string& exe,
                        const std::vector<std::string>& args,
                        double timeout_s) const;

    std::string ffmpeg_;
    std::string ffprobe_;
    /// 见 has_filter。shared_ptr 是为了让拷贝共享同一份，同时保住可拷贝性
    /// （mutex 不可拷贝，直接当成员会让整个 FFmpeg 不能拷）。
    struct FilterCache {
        std::mutex mu;
        std::map<std::string, bool> known;
    };
    std::shared_ptr<FilterCache> filters_;
    Runner runner_;
};

}  // namespace changji::media
