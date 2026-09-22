#include "media/ffmpeg.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <array>
#include <cstdio>
#include <sstream>

#include <nlohmann/json.hpp>

#include "util/paths.hpp"
#include "util/proc.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::media {

namespace {

/// signalstats 的键 -> 我们的字段名。
const std::array<std::pair<const char*, const char*>, 5>& stat_keys() {
    static const std::array<std::pair<const char*, const char*>, 5> a = {{
        {"lavfi.signalstats.YAVG", "mean"},
        {"lavfi.signalstats.YMIN", "minimum"},
        {"lavfi.signalstats.YMAX", "maximum"},
        {"lavfi.signalstats.YLOW", "low"},
        {"lavfi.signalstats.YHIGH", "high"},
    }};
    return a;
}

std::string fmt3(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

/// JSON 里可能是字符串也可能是数字，两种都要认。
double num_of(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return 0.0;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) return to_double(it->get<std::string>());
    return 0.0;
}

std::string str_of(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

double to_double(const std::string& v) {
    try {
        std::size_t used = 0;
        const double d = std::stod(v, &used);
        return used == 0 ? 0.0 : d;
    } catch (const std::exception&) {
        // ffprobe 对某些容器给 "N/A"，那不是错误，是"这个字段没有"。
        return 0.0;
    }
}

double parse_fps(const std::string& rate) {
    const std::size_t slash = rate.find('/');
    if (slash == std::string::npos) return to_double(rate);
    const double num = to_double(rate.substr(0, slash));
    const std::string den_s = rate.substr(slash + 1);
    const double den = den_s.empty() ? 1.0 : to_double(den_s);
    // 分母是 0 时返回 0 而不是 inf。ffprobe 对没有视频轨的文件给 "0/0"，
    // 而 inf 会一路传进时长计算，最后表现成"时长是 nan"。
    return den == 0.0 ? 0.0 : num / den;
}

std::vector<double> sample_points(int samples) {
    if (samples <= 1) return {0.5};
    // 避开首尾各 10%：那里常有编码伪影，按它判画面会误伤正常镜头。
    constexpr double lo = 0.1;
    constexpr double hi = 0.9;
    const double step = (hi - lo) / (samples - 1);
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) out.push_back(lo + step * i);
    return out;
}

MediaInfo parse_probe(const std::string& json_text, const fs::path& path) {
    const json data = json::parse(json_text, nullptr, false);
    if (data.is_discarded()) {
        throw FFmpegError(SAYF("ffprobe 输出无法解析：%1", paths::to_utf8(path)));
    }

    MediaInfo info;
    info.path = path;

    const json* video = nullptr;
    const json* audio = nullptr;
    const auto streams = data.find("streams");
    if (streams != data.end() && streams->is_array()) {
        for (const auto& s : *streams) {
            const std::string type = str_of(s, "codec_type");
            if (type == "video" && video == nullptr) video = &s;
            if (type == "audio" && audio == nullptr) audio = &s;
        }
    }
    info.has_video = video != nullptr;
    info.has_audio = audio != nullptr;

    const auto format = data.find("format");
    if (format != data.end() && format->is_object()) {
        info.duration_s = num_of(*format, "duration");
    }
    // 有些容器（裸流、部分 mkv）在 format 里没有时长，只在视频轨上有。
    // 不回落的话时长永远是 0，而按时长做的校验会全部通过。
    if (info.duration_s == 0.0 && video != nullptr) {
        info.duration_s = num_of(*video, "duration");
    }

    if (video != nullptr) {
        info.width = static_cast<int>(num_of(*video, "width"));
        info.height = static_cast<int>(num_of(*video, "height"));
        const std::string rate = str_of(*video, "r_frame_rate");
        info.fps = parse_fps(rate.empty() ? "0/1" : rate);
        info.frames = static_cast<int>(num_of(*video, "nb_frames"));
        info.pix_fmt = str_of(*video, "pix_fmt");
        info.sar = str_of(*video, "sample_aspect_ratio");
    }
    return info;
}

PixelStats parse_signalstats(const std::string& text) {
    std::map<std::string, double> found;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = text::strip_ws(line);
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = text::strip_ws(trimmed.substr(0, eq));
        for (const auto& [ffkey, name] : stat_keys()) {
            if (key == ffkey) {
                found[name] = to_double(trimmed.substr(eq + 1));
                break;
            }
        }
    }

    if (found.count("mean") == 0) {
        throw FFmpegError(SAY("signalstats 没有返回亮度统计，"
                              "可能是文件损坏或没有视频轨"));
    }

    const auto get = [&](const char* k, double def) {
        const auto it = found.find(k);
        return it == found.end() ? def : it->second;
    };

    PixelStats s;
    s.mean = get("mean", 0.0);
    s.minimum = get("minimum", 0.0);
    s.maximum = get("maximum", 0.0);
    // YLOW/YHIGH 缺了才退回极差。老版本 ffmpeg 的 signalstats 没有这两个
    // 百分位字段，那时候极差是唯一能用的，虽然会被单个亮点带偏。
    s.low = get("low", s.minimum);
    s.high = get("high", s.maximum);
    s.spread = std::max(0.0, s.high - s.low);
    return s;
}

std::map<std::string, double> parse_loudnorm(const std::string& stderr_text) {
    // 从**最后**一对大括号里取。ffmpeg 前面还会打一堆日志，
    // 其中可能带大括号（滤镜图的描述就有），从第一个开始找会捞到错的。
    const std::size_t start = stderr_text.rfind('{');
    const std::size_t end = stderr_text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        throw FFmpegError(
            SAY("loudnorm 没有返回测量结果，可能是文件里没有音轨"));
    }
    const json data =
        json::parse(stderr_text.substr(start, end - start + 1), nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
        throw FFmpegError(SAY("loudnorm 输出无法解析"));
    }

    std::map<std::string, double> out;
    for (const auto& kv : data.items()) {
        const std::string& k = kv.key();
        if (k.rfind("input_", 0) == 0 || k.rfind("output_", 0) == 0 ||
            k.rfind("target_", 0) == 0) {
            out[k] = kv.value().is_string()
                         ? to_double(kv.value().get<std::string>())
                         : (kv.value().is_number() ? kv.value().get<double>() : 0.0);
        }
    }
    return out;
}

// ---- 执行 ----

Runner default_runner() {
    return [](const std::string& exe, const std::vector<std::string>& args,
              double timeout_s) {
        const proc::Result r =
            proc::run(exe, args, static_cast<int>(timeout_s * 1000));
        ProcResult out;
        out.exit_code = r.exit_code;
        out.out = r.out;
        out.launched = r.launched;
        out.timed_out = r.timed_out;
        return out;
    };
}

FFmpeg::FFmpeg(std::string ffmpeg_exe, std::string ffprobe_exe, Runner runner)
    : ffmpeg_(std::move(ffmpeg_exe)),
      ffprobe_(std::move(ffprobe_exe)),
      runner_(std::move(runner)),
      filters_(std::make_shared<FilterCache>()) {}

void FFmpeg::check() const {
    std::vector<std::string> missing;
    for (const auto& [name, exe] : {std::pair{"ffmpeg", ffmpeg_},
                                    std::pair{"ffprobe", ffprobe_}}) {
        const ProcResult r = runner_(exe, {"-version"}, 15.0);
        if (!r.launched) missing.push_back(name);
    }
    if (missing.empty()) return;

    std::string names;
    // **指向缺的那一个的配置项，不是永远指 ffmpeg_path。**
    //
    // 原来无论缺谁都写"填 assembly.ffmpeg_path"。缺的是 ffprobe 时那句话
    // 是错的——ffprobe 有自己的 `assembly.ffprobe_path`，填 ffmpeg_path
    // 一点用没有。2026-09-08 跑装配判据时撞上：ffmpeg 明明配好了，
    // 报的还是"填 assembly.ffmpeg_path"，照做当然没用。
    //
    // 这类错误比没有提示更糟：它让人**照着做，然后发现没用**，
    // 于是开始怀疑别的地方。
    std::string keys;
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i) {
            names += SAY("和");
            // 这一串是配置键名（`assembly.ffmpeg_path`），拿顿号串起来给人
            // 看的，所以分隔符跟着语言换。
            keys += SAY("、");
        }
        names += missing[i];
        keys += std::string("assembly.") + missing[i] + "_path";
    }
    // 说清怎么办。只说"找不到"的话，用户下一步不知道该干什么。
    throw FFmpegMissing(
        SAYF("找不到 %1。\n"
             "Windows 可以用 winget install Gyan.FFmpeg，\n"
             "macOS 用 brew install ffmpeg，\n"
             "Linux 用包管理器安装。\n"
             "装好后如果仍然找不到，在配置里填 %2 指定完整路径。",
             names, keys));
}

bool FFmpeg::has_filter(const std::string& name) const {
    if (!filters_) return false;
    {
        std::lock_guard lg(filters_->mu);
        const auto it = filters_->known.find(name);
        if (it != filters_->known.end()) return it->second;
    }
    bool ok = false;
    try {
        // 认不得的滤镜 ffmpeg 回 "Unknown filter 'xxx'."，**退出码却是 0**，
        // 所以看的是输出而不是退出码。
        const std::string out = run_exe(ffmpeg_, {"-hide_banner", "-h",
                                                  "filter=" + name}, 20.0);
        ok = out.find("Unknown filter") == std::string::npos &&
             out.find("Filter " + name) != std::string::npos;
    } catch (const std::exception&) {
        ok = false;   // 问不出来就当没有：退回外挂字幕，片子照出
    }
    {
        std::lock_guard lg(filters_->mu);
        filters_->known[name] = ok;
    }
    return ok;
}

bool FFmpeg::available() const {
    try {
        check();
        return true;
    } catch (const FFmpegMissing&) {
        return false;
    }
}

std::string FFmpeg::run_exe(const std::string& exe,
                            const std::vector<std::string>& args,
                            double timeout_s) const {
    const ProcResult r = runner_(exe, args, timeout_s);
    if (!r.launched) {
        throw FFmpegMissing(
            SAYF("找不到 %1。在配置里填 assembly.ffmpeg_path 指定完整路径",
                 exe));
    }
    if (r.timed_out) {
        // **超时要单独说。** 混在"退出码 N"里的话，一次跑了半小时被杀掉的
        // 编码看起来就像编码参数写错了，人会去翻滤镜串。
        throw FFmpegError(
            SAYF("%1 超时（%2 秒）被中止。\n"
                 "要么这一段太长，要么它卡住了。最后的输出：\n%3",
                 exe, std::to_string(static_cast<int>(timeout_s)),
                 r.out.size() > 800 ? r.out.substr(r.out.size() - 800) : r.out));
    }
    if (r.exit_code != 0) {
        // 把 ffmpeg 自己的报错贴出来，截断到 800 字——它的日志很长，
        // 而有用的那一行通常在最后。
        const std::string tail =
            r.out.size() > 800 ? r.out.substr(r.out.size() - 800) : r.out;
        throw FFmpegError(SAYF("%1 退出码 %2：\n%3", exe,
                               std::to_string(r.exit_code), tail));
    }
    return r.out;
}

std::string FFmpeg::run(const std::vector<std::string>& args,
                        double timeout_s) const {
    // -hide_banner -nostdin 每次都带：不带 nostdin 的话，ffmpeg 遇到
    // "文件已存在，覆盖吗"会去读 stdin，而这个进程的 stdin 是关着的，
    // 它会一直等到超时。
    std::vector<std::string> full = {"-hide_banner", "-nostdin"};
    full.insert(full.end(), args.begin(), args.end());
    return run_exe(ffmpeg_, full, timeout_s);
}

MediaInfo FFmpeg::probe(const fs::path& p) const {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        throw FFmpegError(SAYF("文件不存在：%1", paths::to_utf8(p)));
    }
    const std::string out = run_exe(
        ffprobe_,
        {"-v", "error", "-print_format", "json", "-show_format", "-show_streams",
         paths::to_utf8(p)},
        120.0);
    return parse_probe(out, p);
}

PixelStats FFmpeg::pixel_stats(const fs::path& p,
                               std::optional<double> at_second) const {
    std::vector<std::string> args;
    if (at_second.has_value()) {
        // -ss 放在 -i 之前是关键帧定位，快得多。放后面是精确定位但要
        // 从头解码到那一点，一段几分钟的片子会慢几十倍。
        args.push_back("-ss");
        args.push_back(fmt3(*at_second));
    }
    args.push_back("-i");
    args.push_back(paths::to_utf8(p));
    args.push_back("-frames:v");
    args.push_back("1");
    args.push_back("-vf");
    args.push_back("signalstats,metadata=print:file=-");
    args.push_back("-f");
    args.push_back("null");
    args.push_back("-");
    return parse_signalstats(run(args, 120.0));
}

std::vector<PixelStats> FFmpeg::sample_pixel_stats(const fs::path& p,
                                                   int samples) const {
    const MediaInfo info = probe(p);
    if (info.duration_s <= 0.0) return {};

    std::vector<PixelStats> out;
    for (const double frac : sample_points(samples)) {
        try {
            out.push_back(pixel_stats(p, info.duration_s * frac));
        } catch (const FFmpegError&) {
            // 某一个取样点取不到不该让整次取样失败：片尾那一帧取不到
            // 是常事（时长有零点几秒的误差），而前面几个点已经够判断了。
            continue;
        }
    }
    return out;
}

MotionStats parse_motion(const std::string& text) {
    // metadata=print 每帧打一行 "lavfi.signalstats.YAVG=2.345"，
    // 这里 YAVG 是**差分图**的均值，也就是相邻两帧的平均绝对差。
    std::vector<double> diffs;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = text::strip_ws(line);
        const std::string key = "lavfi.signalstats.YAVG=";
        if (trimmed.rfind(key, 0) != 0) continue;
        diffs.push_back(to_double(trimmed.substr(key.size())));
    }
    MotionStats m;
    if (diffs.empty()) return m;
    m.frames = static_cast<int>(diffs.size());
    double sum = 0.0;
    for (const double d : diffs) {
        sum += d;
        m.max = std::max(m.max, d);
    }
    m.mean = sum / static_cast<double>(diffs.size());
    std::vector<double> sorted = diffs;
    std::sort(sorted.begin(), sorted.end());
    m.median = sorted[sorted.size() / 2];
    return m;
}

MotionStats FFmpeg::motion_stats(const fs::path& p) const {
    // scale 到 256 宽再算：一是快（整段解码的大头在这儿），二是让不同画幅
    // 的数能横着比。tblend=difference 出的是相邻帧的差分图，signalstats
    // 的 YAVG 就是它的均值。
    return parse_motion(run(
        {"-i", paths::to_utf8(p), "-vf",
         "scale=256:-2,format=gray,tblend=all_mode=difference,signalstats,"
         "metadata=print:key=lavfi.signalstats.YAVG:file=-",
         "-f", "null", "-"},
        300.0));
}

std::map<std::string, double> FFmpeg::measure_loudness(const fs::path& p) const {
    const std::string out = run(
        {"-i", paths::to_utf8(p), "-af",
         "loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"},
        600.0);
    return parse_loudnorm(out);
}

fs::path FFmpeg::extract_frame(const fs::path& p, const fs::path& dest,
                               double at_second) const {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    run({"-y", "-ss", fmt3(at_second), "-i", paths::to_utf8(p), "-frames:v", "1",
         "-q:v", "2", paths::to_utf8(dest)},
        120.0);
    return dest;
}

}  // namespace changji::media
