#include "media/subtitles.hpp"

#include <algorithm>
// **MSVC 会顺带把 <cmath> 带进来，GCC 不会。** 这一处 std::fmod 在
// Windows 上编了几个月都没事，第一次在 Linux 上编就挂：
// "'fmod' is not a member of 'std'"。方案里"交叉编译到树莓派"那条要的
// 就是这种东西早点冒出来。
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "util/say.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::media {

namespace {

#include "media/east_asian_width.inc.hpp"

/// 断在这些标点之后是自然的。
///
/// ⚠️ **这三张表是排版规则，不是界面。** 比的是字幕正文里的中文标点，
/// 翻了之后中文字幕就再也断不好行——而它不报错，只在成片上表现成
/// 「标点跑到行首去了」。所以整张表 `SAY_NEVER`。
const std::vector<std::string>& break_after() {
    static const std::vector<std::string> v = {SAY_NEVER("，"), SAY_NEVER("。"), SAY_NEVER("？"), SAY_NEVER("！"), SAY_NEVER("；"),
                                               SAY_NEVER("："), SAY_NEVER("、"), SAY_NEVER("…"), SAY_NEVER("—")};
    return v;
}

/// 这些不能出现在行首。
const std::vector<std::string>& no_line_start() {
    static const std::vector<std::string> v = {SAY_NEVER("，"), SAY_NEVER("。"), SAY_NEVER("？"), SAY_NEVER("！"), SAY_NEVER("；"),
                                               SAY_NEVER("："), SAY_NEVER("、"), SAY_NEVER("）"), SAY_NEVER("】"), SAY_NEVER("》"),
                                               SAY_NEVER("」"), SAY_NEVER("』"), SAY_NEVER("…"), SAY_NEVER("—"), SAY_NEVER("·"), SAY_NEVER("%")};
    return v;
}

/// 这些不能出现在行尾。
const std::vector<std::string>& no_line_end() {
    static const std::vector<std::string> v = {SAY_NEVER("（"), SAY_NEVER("【"), SAY_NEVER("《"), SAY_NEVER("「"), SAY_NEVER("『")};
    return v;
}

/// 把 UTF-8 串切成一个个字符（**不是字节**）。
///
/// 断行的所有下标都以"第几个字符"计。按字节算的话，一个中文字会被
/// 劈成三段，产出非法 UTF-8——而那个串会进 ASS 文件，播放器直接不显示。
std::vector<std::string> chars_of(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t n =
            text::utf8_char_len(static_cast<unsigned char>(s[i]));
        const std::size_t take = std::min(n, s.size() - i);
        out.push_back(s.substr(i, take));
        i += take;
    }
    return out;
}

char32_t codepoint_of(const std::string& ch) {
    if (ch.empty()) return 0;
    const auto b0 = static_cast<unsigned char>(ch[0]);
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && ch.size() >= 2) {
        return static_cast<char32_t>(((b0 & 0x1F) << 6) |
                                     (static_cast<unsigned char>(ch[1]) & 0x3F));
    }
    if ((b0 & 0xF0) == 0xE0 && ch.size() >= 3) {
        return static_cast<char32_t>(((b0 & 0x0F) << 12) |
                                     ((static_cast<unsigned char>(ch[1]) & 0x3F) << 6) |
                                     (static_cast<unsigned char>(ch[2]) & 0x3F));
    }
    if ((b0 & 0xF8) == 0xF0 && ch.size() >= 4) {
        return static_cast<char32_t>(((b0 & 0x07) << 18) |
                                     ((static_cast<unsigned char>(ch[1]) & 0x3F) << 12) |
                                     ((static_cast<unsigned char>(ch[2]) & 0x3F) << 6) |
                                     (static_cast<unsigned char>(ch[3]) & 0x3F));
    }
    return 0;
}

bool in(const std::vector<std::string>& set, const std::string& ch) {
    return std::find(set.begin(), set.end(), ch) != set.end();
}

double width_of(const std::string& ch) { return is_wide(codepoint_of(ch)) ? 1.0 : 0.5; }

std::string join(const std::vector<std::string>& v, std::size_t from,
                 std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < v.size(); ++i) out += v[i];
    return out;
}

/// 找一个断点，返回**第几个字符**处切。
std::size_t find_break(const std::vector<std::string>& chars, double max_width) {
    double width = 0.0;
    std::size_t limit = chars.size();
    for (std::size_t i = 0; i < chars.size(); ++i) {
        width += width_of(chars[i]);
        if (width > max_width) {
            limit = i;
            break;
        }
    }

    // 在宽度范围内从后往前找标点。只往回找 8 个字符：找太远的话，
    // 一行会短得很难看，那还不如在宽度处硬断。
    //
    // **边界要和 Python 的 range(limit-1, max(0, limit-8), -1) 逐个对齐。**
    // 多查一格或少查一格，某些句子的断点就和 Python 差一个字——
    // 那不会报错，只会让两边出的字幕文件不一样。
    if (limit >= 2) {
        const std::size_t stop = limit > 8 ? limit - 8 : 0;
        for (std::size_t i = limit - 1; i > stop; --i) {
            if (in(break_after(), chars[i])) return i + 1;
        }
    }

    // 没有标点就在宽度上限处断，但避开非法位置。
    std::size_t cut = limit;
    int guard = 0;
    while (cut > 1 && guard < 6) {
        ++guard;
        if (cut < chars.size() && in(no_line_start(), chars[cut])) {
            --cut;
            continue;
        }
        if (in(no_line_end(), chars[cut - 1])) {
            --cut;
            continue;
        }
        break;
    }
    return std::max<std::size_t>(1, cut);
}

}  // namespace

bool is_wide(char32_t cp) {
    // 区间表是有序的，二分。121 个区间线性扫也不慢，但断行要对每个字符
    // 调一次，一章几千字就是几十万次。
    std::size_t lo = 0;
    std::size_t hi = sizeof(kWideRanges) / sizeof(kWideRanges[0]);
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (cp < kWideRanges[mid].lo) {
            hi = mid;
        } else if (cp > kWideRanges[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

double display_width(const std::string& utf8) {
    double total = 0.0;
    for (const auto& ch : chars_of(utf8)) total += width_of(ch);
    return total;
}

std::vector<std::string> wrap_chinese(const std::string& text, double max_width,
                                      int max_lines) {
    const std::string trimmed = text::strip_ws(text);
    if (trimmed.empty()) return {};
    if (display_width(trimmed) <= max_width) return {trimmed};

    std::vector<std::string> lines;
    std::vector<std::string> rest = chars_of(trimmed);

    while (!rest.empty() && static_cast<int>(lines.size()) < max_lines) {
        const std::size_t cut = find_break(rest, max_width);
        if (cut == 0 || cut >= rest.size()) {
            lines.push_back(text::strip_ws(join(rest, 0, rest.size())));
            rest.clear();
            break;
        }
        lines.push_back(text::strip_ws(join(rest, 0, cut)));
        rest = chars_of(text::strip_ws(join(rest, cut, rest.size())));
    }

    if (!rest.empty()) {
        // 塞不下的部分并进最后一行。**宁可最后一行长一点也不丢字**——
        // 丢字的表现是观众看到一句没说完的话。
        if (lines.empty()) lines.push_back("");
        lines.back() = text::strip_ws(lines.back() + join(rest, 0, rest.size()));
    }

    std::vector<std::string> out;
    for (auto& ln : lines) {
        if (!ln.empty()) out.push_back(ln);
    }
    return out;
}

/// 台词里的字进 Dialogue 行之前要过一遍。
///
/// **花括号是硬伤。** ASS 里 `{` 开始一个特效覆盖块、到 `}` 为止整段被
/// 吞掉。台词里出现一个 `{`，那几个字在成片里就没了——**而且不报错**，
/// 要盯着成片看才发现。剧本是大模型写的，它偶尔会吐出 ASCII 花括号。
/// libass 认 `\\{` 这种写法，渲染成一个字面的大括号。
///
/// **裸换行会把这一行拆断。** Dialogue 是一行一条记录，文本里混进 CR/LF
/// 之后半条记录变成下一行，渲染器多半直接忽略——又是一处静默丢字。
/// 换成空格：真要换行，上面 wrap 出来的那几段之间已经有 \\N 了。
///
/// 反斜杠不动：ASS 没有通用的反斜杠转义，而中文台词里出现裸反斜杠
/// 比出现花括号少得多，动它反而可能改坏本来对的输出。
std::string ass_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '{' || c == '}') {
            out += '\\';
            out += c;
        } else if (c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string ass_time(double seconds) {
    seconds = std::max(0.0, seconds);
    const int h = static_cast<int>(seconds / 3600.0);
    const int m = static_cast<int>(std::fmod(seconds, 3600.0) / 60.0);
    const double s = std::fmod(seconds, 60.0);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%05.2f", h, m, s);
    return buf;
}

/// 把参考画布上的数缩到真实画布上。
///
/// 横向的按宽比缩，纵向的按高比缩——和 libass 从 PlayResX/PlayResY 往
/// 真实分辨率缩的口径一致。**字号按宽比缩不是按高比**：要装得下的是
/// 一行字的宽度，而竖屏时两个比一样，横屏时按高缩会把字压成十几像素。
AssStyleMetrics ass_metrics(const AssOptions& opt) {
    AssStyleMetrics m;
    const double sx = opt.width > 0
                          ? static_cast<double>(opt.width) / kAssRefWidth
                          : 1.0;
    const double sy = opt.height > 0
                          ? static_cast<double>(opt.height) / kAssRefHeight
                          : 1.0;
    m.font_size = std::max(12, static_cast<int>(std::lround(opt.font_size * sx)));
    m.margin_h = std::max(8, static_cast<int>(std::lround(opt.margin_h * sx)));
    m.margin_v = std::max(8, static_cast<int>(std::lround(opt.margin_v * sy)));

    // **最后再按真实宽度兜一次底。**
    //
    // 上面那两个数只要有一个被人单独改过，"多少字装得下"就和
    // max_chars_per_line 对不上了，而对不上的表现是字**被切掉**，
    // 不是折行——WrapStyle: 2 只认我们自己插进去的硬换行。所以这里
    // 不信任那个配置值，按算出来的宽度取小的那个。
    //
    // 满宽中文字的宽度约等于字号（1 em），display_width 里 ASCII 记 0.5，
    // 所以"多少个单位"直接就是 可用宽度 / 字号。
    m.max_chars_per_line = opt.max_chars_per_line;
    if (opt.width > 0 && m.font_size > 0) {
        const int usable = opt.width - 2 * m.margin_h;
        const int fits = usable / m.font_size;
        // 低于 6 个字就不是"断行"了，是把每个字单独一行。那种时候宁可
        // 让它略微超一点，也别切成这样——超出的部分至少还看得见一半。
        if (fits >= 6) m.max_chars_per_line = std::min(m.max_chars_per_line, fits);
    }
    return m;
}

std::string build_ass(const std::vector<SubtitleCue>& cues,
                      const AssOptions& opt) {
    const AssStyleMetrics m = ass_metrics(opt);
    std::ostringstream os;
    // WrapStyle 2 = 只在显式换行符处断行。断点由上面的 wrap_chinese 算好，
    // 不让渲染库自作主张——libass 对中文只按字符断，会在词中间折断。
    os << "[Script Info]\n"
       << "ScriptType: v4.00+\n"
       << "PlayResX: " << opt.width << "\n"
       << "PlayResY: " << opt.height << "\n"
       << "WrapStyle: 2\n"
       << "ScaledBorderAndShadow: yes\n"
       << "\n"
       << "[V4+ Styles]\n"
       << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
          "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
          "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
          "Alignment, MarginL, MarginR, MarginV, Encoding\n"
       << "Style: dialogue," << opt.font << "," << m.font_size
       << ",&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,3,"
          "1,2,"
       << m.margin_h << "," << m.margin_h << "," << m.margin_v << ",1\n"
       << "Style: narration," << opt.font << ","
       << static_cast<int>(m.font_size * 0.92)
       << ",&H00E8E8E8,&H000000FF,&H00000000,&H64000000,0,1,0,0,100,100,0,0,1,3,"
          "1,2,"
       << m.margin_h << "," << m.margin_h << "," << m.margin_v << ",1\n"
       << "Style: title," << opt.font << ","
       << static_cast<int>(m.font_size * 1.4)
       << ",&H00FFFFFF,&H000000FF,&H00000000,&H96000000,1,0,0,0,100,100,2,0,1,4,"
          "2,5,"
       << m.margin_h << "," << m.margin_h << ",0,1\n"
       << "\n"
       << "[Events]\n"
       << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
          "Effect, Text\n";

    std::string out = os.str();
    for (const auto& cue : cues) {
        if (text::strip_ws(cue.text).empty()) continue;
        const auto wrapped = wrap_chinese(
            cue.text, static_cast<double>(m.max_chars_per_line), opt.max_lines);
        std::string body;
        for (std::size_t i = 0; i < wrapped.size(); ++i) {
            if (i) body += "\\N";   // ASS 的硬换行
            // **先转义再拼。** 反过来的话会把我们自己刚写的 \\N 也转掉。
            body += ass_escape(wrapped[i]);
        }
        const std::string style =
            (cue.style == "dialogue" || cue.style == "narration" ||
             cue.style == "title")
                ? cue.style
                : "dialogue";
        out += "\nDialogue: 0," + ass_time(cue.start_s) + "," +
               ass_time(cue.end_s) + "," + style + ",,0,0,0,," + body;
    }
    return out + "\n";
}

fs::path write_ass(const fs::path& path, const std::vector<SubtitleCue>& cues,
                   const AssOptions& opt) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error(SAYF("写不了字幕文件：%1",
                                      paths::to_utf8(path)));
    }
    // UTF-8 BOM。一些播放器靠它才认出中文，没有的话按本地代码页解，
    // 出来的是一屏乱码。
    f << "\xEF\xBB\xBF" << build_ass(cues, opt);
    return path;
}

std::vector<std::string> validate_cues(const std::vector<SubtitleCue>& cues,
                                       int max_chars_per_line) {
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < cues.size(); ++i) {
        const auto& cue = cues[i];
        const std::string n = std::to_string(i + 1);
        if (cue.end_s <= cue.start_s) {
            problems.push_back(SAYF("第 %1 条字幕时间倒挂", n));
        }
        if (text::strip_ws(cue.text).empty()) {
            problems.push_back(SAYF("第 %1 条字幕是空的", n));
        }
        if (cue.duration_s() < 0.4) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", cue.duration_s());
            problems.push_back(
                SAYF("第 %1 条字幕只显示 %2 秒，看不清", n, buf));
        }
        const auto wrapped =
            wrap_chinese(cue.text, static_cast<double>(max_chars_per_line));
        for (const auto& ln : wrapped) {
            if (display_width(ln) > max_chars_per_line * 1.6) {
                problems.push_back(SAYF("第 %1 条字幕断行后仍然超长", n));
                break;
            }
        }
    }
    for (std::size_t i = 0; i + 1 < cues.size(); ++i) {
        const auto& a = cues[i];
        const auto& b = cues[i + 1];
        if (b.start_s < a.end_s - 0.01) {
            problems.push_back(SAYF("字幕重叠：%1 与 %2",
                                    text::truncate_utf8(a.text, 10),
                                    text::truncate_utf8(b.text, 10)));
        }
    }
    return problems;
}

}  // namespace changji::media
