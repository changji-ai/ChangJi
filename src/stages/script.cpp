#include "stages/script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "stages/audio_plan.hpp"
#include "stages/json_extract.hpp"
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

// ---- 解析模型输出用的几张表 ----
//
// **不在 prompts.toml 里**：那份是喂给模型的话，这几张是从模型吐回来的
// 东西里认形式用的，改它们不该动到提示词那一层。原来和提示词放在一个
// 头里，2026-09-14 提示词搬去 prompts.toml 时留在了这儿。

// 机位标签里的景别词。动作行开头挂一个「镜头特写：」时靠它认出来。
//
// **不进提示词，只在解析时用。** 提示词里列一串「不要写镜头/特写/近景」，
// 模型会把这些词原样抄进正文——2026-09-11 在章节那边栽过一次
// （反例句被逐字抄走）。形式是我们定的，削掉就完了。
inline constexpr const char* kCameraWords[] = {
    R"CJ(镜头)CJ",
    R"CJ(特写)CJ",
    R"CJ(近景)CJ",
    R"CJ(中景)CJ",
    R"CJ(远景)CJ",
    R"CJ(全景)CJ",
    R"CJ(空镜)CJ",
    R"CJ(画面)CJ",
    R"CJ(闪回)CJ",
    R"CJ(插入)CJ",
    // 2026-09-13 补的一批**后期/转场**术语。上面那十个都是「怎么拍」，
    // 这一批是「不是拍出来的」——行业写法里这类信息本来就用【】标出来，
    // 意思是"画面里没有，是后期合成的"。模型会把它们写成 `标签：内容`，
    // 而那和一句台词长得一模一样。
    //
    // **实跑撞上的**（预告片那条路）：
    //     黑屏前最后一帧：林浩抬头望向镜头，雨水顺着脸颊滑落……
    // 冒号前七个字，script_dialogue_pairs 认「冒号前 ≤12 字 = 说话人」，
    // 于是整句动作描写变成一个叫「黑屏前最后一帧」的人在说话；名字认不出
    // 就落成旁白，**旁白音会把它念出来**。
    //
    // 只收确定不会被念出口的那些。**不收「字幕」**：「字幕：三年后」削成
    // 「三年后」之后，让旁白念一句"三年后"其实是正当的转场处理，
    // 两种做法都说得通，不该在这一层替人决定。
    R"CJ(黑屏)CJ",
    R"CJ(定格)CJ",
    R"CJ(定场)CJ",
    R"CJ(淡入)CJ",
    R"CJ(淡出)CJ",
    R"CJ(化入)CJ",
    R"CJ(叠化)CJ",
    R"CJ(转场)CJ",
    R"CJ(航拍)CJ",
    R"CJ(慢镜)CJ",
    R"CJ(特效)CJ",
    R"CJ(蒙太奇)CJ",
};

// 说话人为空的各种写法。模型经常无视 schema 填 none、旁白 这类词，
// 原样当名字用的话，成片字幕上会出现「none：寂静」。
inline constexpr const char* kNoSpeaker[] = {
    R"CJ((none))CJ",
    R"CJ(-)CJ",
    R"CJ(n/a)CJ",
    R"CJ(na)CJ",
    R"CJ(narrator)CJ",
    R"CJ(nil)CJ",
    R"CJ(none)CJ",
    R"CJ(none.)CJ",
    R"CJ(null)CJ",
    R"CJ(ost)CJ",
    R"CJ(vo)CJ",
    R"CJ(voiceover)CJ",
    R"CJ(—)CJ",
    R"CJ(旁白)CJ",
    R"CJ(无)CJ",
    R"CJ(画外音)CJ",
    R"CJ(空)CJ",
    R"CJ(（无）)CJ",
};

// 整段外面套的括号和引号。左右相同的（引号）判断规则不一样，
// 见 C++ 侧 wraps_whole 的注释。
inline constexpr const char* kWrappers[][2] = {
    {R"CJ(（)CJ", R"CJ(）)CJ"},
    {R"CJ(()CJ", R"CJ())CJ"},
    {R"CJ(【)CJ", R"CJ(】)CJ"},
    {R"CJ([)CJ", R"CJ(])CJ"},
    {R"CJ(“)CJ", R"CJ(”)CJ"},
    {R"CJ(")CJ", R"CJ(")CJ"},
    {R"CJ(「)CJ", R"CJ(」)CJ"},
    {R"CJ(『)CJ", R"CJ(』)CJ"},
    {R"CJ(')CJ", R"CJ(')CJ"},
};

// 只削开头这几种括号里的时间码。
inline constexpr const char* kLeadBrackets[][2] = {
    {R"CJ([)CJ", R"CJ(])CJ"},
    {R"CJ(【)CJ", R"CJ(】)CJ"},
    {R"CJ(（)CJ", R"CJ(）)CJ"},
    {R"CJ(()CJ", R"CJ())CJ"},
};

// 括号里出现这些才算时间码。光有数字不够——
// 「（他犹豫了3秒）」和「（第3次）」得区分开。
inline constexpr const char* kTimeUnits[] = {
    R"CJ(秒)CJ",
    R"CJ(s)CJ",
    R"CJ(S)CJ",
    R"CJ(:)CJ",
    R"CJ(：)CJ",
    R"CJ(分)CJ",
    R"CJ(帧)CJ",
};

/// 对应 Python 的 f"{x:.0f}"。
///
/// 注意这是**银行家舍入**：f"{0.5:.0f}" 是 "0"，f"{1.5:.0f}" 是 "2"。
/// C 的 %.0f 在默认舍入模式下同样如此，所以直接用。
std::string format_f0(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return std::string(buf);
}

std::string strip_ascii(const std::string& s) { return text::strip_ws(s); }

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

/// 首尾这一对是不是套住整段的那一对。
///
/// 光数左右个数不够。「（甲说）乙答（丙笑）」左右各两个，数目相等，
/// 但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
/// 要从头扫一遍看深度什么时候回到零。
///
/// 按 UTF-8 字符扫而不是按字节：中文括号一个三字节，按字节扫会把
/// 一个汉字的中间字节误当成括号。这里靠"整段前缀比较"来避免——
/// UTF-8 是自同步的，一个完整字符的字节序列不会出现在别的字符中间。
bool wraps_whole(const std::string& s, const std::string& left,
                 const std::string& right) {
    if (left == right) {
        // 引号这类左右一样的，中间不能再出现同一个符号
        const std::string inner =
            s.substr(left.size(), s.size() - left.size() - right.size());
        return inner.find(right) == std::string::npos;
    }
    int depth = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, left.size(), left) == 0) {
            ++depth;
            i += left.size();
        } else if (s.compare(i, right.size(), right) == 0) {
            --depth;
            if (depth == 0) return i + right.size() == s.size();
            if (depth < 0) return false;
            i += right.size();
        } else {
            i += text::utf8_char_len(static_cast<unsigned char>(s[i]));
        }
    }
    return false;
}

const std::set<std::string>& no_speaker_words() {
    static const std::set<std::string> kWords = [] {
        std::set<std::string> s;
        for (const char* w : kNoSpeaker) s.insert(w);
        return s;
    }();
    return kWords;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return s;
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    // 对应 Python 的 str(x)：模型偶尔会填数字
    return it->dump();
}

}  // namespace

std::string strip_wrapper(const std::string& text_in) {
    std::string out = strip_ascii(text_in);
    bool changed = true;
    while (changed && text::utf8_len(out) >= 2) {
        changed = false;
        for (const auto& pair : kWrappers) {
            const std::string left = pair[0], right = pair[1];
            if (!starts_with(out, left) || !ends_with(out, right)) continue;
            if (!wraps_whole(out, left, right)) continue;
            const std::string inner = strip_ascii(
                out.substr(left.size(), out.size() - left.size() - right.size()));
            if (inner.empty()) continue;
            out = inner;
            changed = true;
            break;
        }
    }
    return out;
}

/// 一句话里所有成对引号的位置和内容。
///
/// 只认引号，不认括号：「（小声）」是舞台提示，那是另一回事，
/// 归 strip_wrapper / strip_camera_prefix 管。
struct QuotedSpan {
    std::size_t begin = 0;   ///< 左引号的字节位置
    std::size_t end = 0;     ///< 右引号之后的字节位置
    std::string inner;       ///< 引号之间的内容
};

std::vector<QuotedSpan> quoted_spans(const std::string& s) {
    // 左右一样的（ASCII 的 " 和 '）也收：模型中英文标点混着用。
    static const char* kQuotes[][2] = {
        {R"CJ(“)CJ", R"CJ(”)CJ"},
        {R"CJ(「)CJ", R"CJ(」)CJ"},
        {R"CJ(『)CJ", R"CJ(』)CJ"},
        {R"CJ(")CJ", R"CJ(")CJ"},
        {R"CJ(')CJ", R"CJ(')CJ"},
    };
    std::vector<QuotedSpan> out;
    std::size_t i = 0;
    while (i < s.size()) {
        bool matched = false;
        for (const auto& q : kQuotes) {
            const std::string left = q[0], right = q[1];
            if (s.compare(i, left.size(), left) != 0) continue;
            const std::size_t close = s.find(right, i + left.size());
            if (close == std::string::npos) continue;   // 只开不闭，不算
            out.push_back(QuotedSpan{
                i, close + right.size(),
                s.substr(i + left.size(), close - i - left.size())});
            i = close + right.size();
            matched = true;
            break;
        }
        // 按 UTF-8 字符步进，别踩进汉字中间的字节
        if (!matched) i += text::utf8_char_len(static_cast<unsigned char>(s[i]));
    }
    return out;
}

std::string strip_leading_timecode(const std::string& text_in) {
    const std::string out = strip_ascii(text_in);
    for (const auto& pair : kLeadBrackets) {
        const std::string left = pair[0], right = pair[1];
        if (!starts_with(out, left)) continue;
        const std::size_t end = out.find(right);
        if (end == std::string::npos || end == 0) continue;
        const std::string inside = out.substr(left.size(), end - left.size());

        // 要有数字
        const bool has_digit = std::any_of(
            inside.begin(), inside.end(),
            [](char c) { return c >= '0' && c <= '9'; });
        if (!has_digit) continue;

        // 还要有时间单位。光有数字不够——
        // 「（他犹豫了3秒）」和「（第3次）」得区分开。
        bool has_unit = false;
        for (const char* u : kTimeUnits) {
            if (inside.find(u) != std::string::npos) {
                has_unit = true;
                break;
            }
        }
        if (!has_unit) continue;

        return strip_ascii(out.substr(end + right.size()));
    }
    return out;
}

std::string strip_camera_prefix(const std::string& text_in) {
    const std::string out = strip_ascii(text_in);
    for (const char* sep : {"：", ":"}) {
        const std::size_t at = out.find(sep);
        if (at == std::string::npos || at == 0) continue;
        const std::string head = out.substr(0, at);
        // 机位标签是很短的一截。长的那种是正文里本来就有的冒号
        // （「牌子上写着：营业中」），削掉会丢内容。
        if (text::utf8_len(head) > 8) continue;
        bool camera = false;
        for (const char* w : kCameraWords) {
            if (head.find(w) != std::string::npos) {
                camera = true;
                break;
            }
        }
        if (!camera) continue;
        const std::string rest = strip_ascii(out.substr(at + std::strlen(sep)));
        // 整拍只有一个标签、后面什么都没有时原样留着：削成空串的话
        // 这一拍会被当成空的丢掉，那还不如留着让人看见模型写歪了。
        if (rest.empty()) continue;
        return rest;
    }
    return out;
}

bool is_stage_direction(const std::string& text_in) {
    const std::string t = strip_ascii(text_in);
    if (t.empty()) return false;
    // 只认圆括号。**不认 【】**——那是段头用的，认了会把段头当提示删掉。
    static const char* kRound[][2] = {
        {R"CJ(（)CJ", R"CJ(）)CJ"},
        {"(", ")"},
    };
    for (const auto& pr : kRound) {
        const std::string left = pr[0], right = pr[1];
        if (!starts_with(t, left) || !ends_with(t, right)) continue;
        // 要真的是套住整段的那一对：「（甲）说完（乙）」首尾也各有一个，
        // 但那两个不是一对，整句并没有被包住。
        if (wraps_whole(t, left, right)) return true;
    }
    return false;
}

std::string strip_list_marker(const std::string& text_in) {
    // 只认这几个。**不含 `—`／`——`**：中文里破折号开头是正当写法
    // （话被打断、话外补白），削了是改文意。
    static const char* kMarkers[] = {
        "-", "*", "+", R"CJ(•)CJ", R"CJ(·)CJ", R"CJ(・)CJ", R"CJ(－)CJ",
    };
    std::string out = strip_ascii(text_in);
    // 最多削三层：「- - 」这种见过，但削不完就是内容本身了。
    for (int round = 0; round < 3; ++round) {
        bool hit = false;
        for (const char* m : kMarkers) {
            const std::string mark = m;
            if (!starts_with(out, mark)) continue;
            const std::string rest = strip_ascii(out.substr(mark.size()));
            // 削成空串就不削——「-」自己就是那一拍的全部内容时，
            // 留着比丢掉强（丢掉这一拍会整个消失）。
            if (rest.empty()) return out;
            // **后面紧跟数字的那个减号是符号，不是列表符号。**
            //
            // 2026-09-13 扫一份新项目的设定时看见的场景名：
            // 「-1层停尸间 B区 3号冷柜」——负一层。台词里同理，
            // 「-3度，冻得我手都伸不直」削了就成了「3度」，正好反过来。
            //
            // 注意要看**削空白之前**紧挨着的那个字符：「- 3号出口」是
            // 列表符号后面跟了空格，那个该削。
            const std::string raw_rest = out.substr(mark.size());
            if (!raw_rest.empty() && raw_rest[0] >= '0' && raw_rest[0] <= '9') {
                continue;
            }
            out = rest;
            hit = true;
            break;
        }
        if (!hit) break;
    }
    return out;
}

std::string strip_speech_tags(const std::string& text_in,
                              const std::string& speaker) {
    std::string s = strip_ascii(text_in);

    // 开头重复人名：「苏婉：你来了」→「你来了」。
    // 说话人是单独一个字段，再写一遍只会被念出来。
    if (!speaker.empty()) {
        for (const char* colon : {R"CJ(：)CJ", ":"}) {
            const std::string lead = speaker + colon;
            if (starts_with(s, lead)) {
                s = strip_ascii(s.substr(lead.size()));
                break;
            }
        }
    }

    const std::vector<QuotedSpan> spans = quoted_spans(s);
    if (spans.empty()) return s;   // 正常剧本的台词本来就不带引号

    // **四种中文对话形式里，引号都贴着句子的一头。** 夹在中间的那种
    // （「他说过“再见”，然后走了」）不是对话形式，剥了会只剩两个字。
    const bool at_head = spans.front().begin == 0;
    const bool at_tail = spans.back().end == s.size();
    if (!at_head && !at_tail) return s;

    // 引号外还剩字，才说明裹了旁白。整句就是一对引号的交给 strip_wrapper。
    std::string outside;
    std::size_t at = 0;
    for (const auto& sp : spans) {
        outside += s.substr(at, sp.begin - at);
        at = sp.end;
    }
    outside += s.substr(at);
    if (strip_ascii(outside).empty()) return s;

    // 提示语在中的那种要把两半接起来：「“甲，”他说，“乙。”」→「甲，乙。」
    std::string spoken;
    for (const auto& sp : spans) spoken += sp.inner;
    spoken = strip_ascii(spoken);
    // 剥空了就别剥——宁可多念一句旁白，也不要这一镜彻底没声音。
    return spoken.empty() ? s : spoken;
}

std::string normalize_speaker(const std::string& raw) {
    const std::string name = strip_wrapper(raw);
    const std::string key = lower_ascii(strip_ascii(name));
    return no_speaker_words().count(key) ? std::string() : name;
}

/// **中文语速全仓只能有一个数。**
///
/// 它今天有三份拷贝，三份都是 4.6，而三份各自独立：
///
///   · `cpp/prompts.toml` 的 `chars_per_second`（→ 这儿用的
///     `prompt::script::kCharsPerSecond`）——决定**叫模型写多少字**；
///   · `stages/audio_plan.hpp` 的 `kCharsPerSecond`——决定
///     `estimate_speech_duration()` 算出来**每句实际多长**，而那个数会
///     反推回去锁死镜头时长；
///   · 前端 `ScriptReader` 那个 `charsPerSecond` 默认值——界面上
///     「约 x 秒」和「偏短 / 合适 / 偏长」那个丸子。
///
/// 它们说的是同一件物理事实（一个人一秒念几个字）。谁单独动一个，症状是
/// **剧本长度看着正好、成片却长出两成**，而且一处都不报错：预算按 A 算、
/// 真实时长按 B 算、屏幕上那句话按 C 算。
///
/// 前两份在这儿用 static_assert 钉死（改一个编不过）；第三份由
/// `webapp/client/src/api/speech-rate.test.js` 直接读 prompts.toml 比对。
static_assert(prompt::script::kCharsPerSecond == kCharsPerSecond,
              "prompts.toml 的 chars_per_second 和 stages/audio_plan.hpp 的 "
              "kCharsPerSecond 必须一致：前者决定写多少字，后者决定念多久。"
              "只改一个，剧本会看着正好而成片长出一截，两边都不报错。");

int budget_chars(double duration_s) {
    // Python 的 int() 是**朝零截断**，不是四舍五入
    const double v = duration_s * prompt::script::kCharsPerSecond * prompt::script::kDialogueShare;
    return std::max(20, static_cast<int>(v));
}

// ---- 四段 ----

std::uint32_t random_shape() {
    static thread_local std::mt19937 gen(std::random_device{}());
    const std::uint32_t v = gen();
    return v ? v : 1u;   // 0 表示不浮动，别撞上
}

namespace {

/// 从种子里取第 k 个 0~1 之间的数。
///
/// 混一道 xorshift：`episode_id` 往往只差一个字（ep01 / ep02），不混的话
/// FNV 的低位差不多，形状还是一个样。
double frac(std::uint32_t seed, int k) {
    std::uint32_t x = seed + static_cast<std::uint32_t>(k) * 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<double>(x % 10000u) / 10000.0;
}

/// 在 [lo, hi] 里按种子挑一个。
double pick(std::uint32_t seed, int k, double lo, double hi) {
    return lo + frac(seed, k) * (hi - lo);
}

}  // namespace

// prompts.toml 里那四张表必须互相对得上——下面 `shape * 4 + i` 取的就是
// 它们，对不上就是**越界读一个 constexpr 数组**：表现是提示词里混进一段
// 不相干的字（运气好），或者进程当场没了（运气不好），而两种都指不到这儿。
//
// 这不是假想：act_labels 已经从四个（一组）长到二十个（五组走法）了一次，
// 那次前端那份抄件没跟上，十份剧本里八份的段头认不出来。数据文件改一行、
// 代码一个字不动就能出事的地方，就该让它在**编译期**说话——升级 Crow 那
// 处的注释写的是同一件事：「这里会编译报错，那是好事」。
static_assert(std::size(prompt::script::kActKeys) == 4,
              "act_keys 必须正好四段：下面是按 shape * 4 + i 取的");
static_assert(std::size(prompt::script::kActLabels) % 4 == 0,
              "act_labels 的条数要是 4 的整数倍：一组走法四段");
static_assert(std::size(prompt::script::kActLabels) ==
                  std::size(prompt::script::kActBriefs),
              "act_labels 和 act_briefs 一一对应：加一组走法两张表都要加");
static_assert(std::size(prompt::script::kShapeNames) ==
                  std::size(prompt::script::kActLabels) / 4,
              "shape_names 一组走法一个名字");

std::vector<ActSpec> act_plan(double duration_s, std::uint32_t variation) {
    // 秒数取整了再分。时长再短也按 4 秒排，每段至少 1 秒——
    // 银行家舍入那几条用例会传 0.5 进来，不能在这儿除出负数。
    const int total = std::max(4, static_cast<int>(std::lround(duration_s)));

    // **形状不写死。** 写死的话 60 秒永远是 5/28/21/6，连着看几章一个样。
    // 非零种子就在这几个区间里挑一组：有的章开场慢、后半段炸，有的章从头
    // 压到尾。总拍数还是按时长来，所以「撑不满时长」那个毛病不会回来。
    // **戏的走法也换，不只是秒数。** 只浮动秒数的话，十章看下来是同一出戏
    // 演快一点演慢一点。第 0 组是老的那一套，variation = 0 时就走它。
    const std::size_t shapes = std::size(prompt::script::kActKeys) == 0
                                   ? 1
                                   : std::size(prompt::script::kActLabels) /
                                         std::size(prompt::script::kActKeys);
    const std::size_t shape =
        variation ? static_cast<std::size_t>(frac(variation, 3) *
                                             static_cast<double>(shapes)) % shapes
                  : 0;

    const double open_r = variation ? pick(variation, 0, 0.05, 0.14) : 0.08;
    const double cliff_r = variation ? pick(variation, 1, 0.07, 0.17) : 0.10;
    const double esc_share = variation ? pick(variation, 2, 0.45, 0.68) : 0.57;

    // 开场和留扣是两头的硬件：三分钟的一章开场也不能拖到十几秒，
    // 钩子也不用留一分钟——封顶 8 / 10 秒。
    const int opening =
        std::clamp(static_cast<int>(std::lround(total * open_r)), 1, 8);
    const int cliff =
        std::clamp(static_cast<int>(std::lround(total * cliff_r)), 1, 10);
    const int middle = total - opening - cliff;  // total ≥ 4 时 ≥ 2
    // 中段推进多于回报：压得久，放得才有劲。具体多多少随这一章变。
    int escalation =
        std::max(1, static_cast<int>(std::lround(middle * esc_share)));
    int payoff = middle - escalation;
    if (payoff < 1) {
        payoff = 1;
        escalation = middle - 1;
    }

    const int seconds[4] = {opening, escalation, payoff, cliff};
    std::vector<ActSpec> out;
    int at = 0;
    for (int i = 0; i < 4; ++i) {
        ActSpec a;
        a.key = prompt::script::kActKeys[i];
        a.label = prompt::script::kActLabels[shape * 4 + i];
        a.brief = prompt::script::kActBriefs[shape * 4 + i];
        a.from_s = at;
        a.to_s = at + seconds[i];
        at = a.to_s;
        // 每 4 秒一拍起步（一拍是一个动作或一句 ≤15 字的台词，两三秒），
        // 至少两拍：一段只有一拍就不是一段。天花板每 1.5 秒一拍——
        // 再密画面就没呼吸了，也防模型写个没完。
        a.min_beats = std::max(2, static_cast<int>(std::lround(seconds[i] / 4.0)));
        a.max_beats = std::max(a.min_beats + 2,
                               static_cast<int>(std::lround(seconds[i] / 1.5)));
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<ScenePlan> scene_plan_for_chapter(
    const std::vector<std::pair<std::string, int>>& scene_chars) {
    std::vector<std::pair<std::string, int>> src = scene_chars;
    if (src.empty()) src.emplace_back("", 0);
    std::vector<ScenePlan> out;
    for (std::size_t i = 0; i < src.size(); ++i) {
        ScenePlan p;
        p.key = "s" + std::to_string(i + 1);
        p.where = src[i].first;
        p.chars = std::max(0, src[i].second);
        // 一拍六十字起步、二十字封顶。再短的场也有四拍：场次头、环境、
        // 一个动作、一句话——少于这个就不是一场戏。
        p.min_beats = std::max(4, p.chars / 60);
        p.max_beats = std::max(p.min_beats + 4, p.chars / 20);
        out.push_back(std::move(p));
    }
    return out;
}

std::string render_act_brief(const std::vector<ActSpec>& specs) {
    std::string out = prompt::script::kActBlockHead;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const ActSpec& s = specs[i];
        out += "  " + s.label + "（" + std::to_string(s.from_s) + "–" +
               std::to_string(s.to_s) + " 秒）：" + s.brief +
               "。至少 " + std::to_string(s.min_beats) + " 拍。\n";
    }
    out += prompt::script::kActBlockTail;
    return out;
}

std::string act_header(const std::string& label, int from_s, int to_s) {
    std::string out = "【" + label;
    if (to_s > from_s) {
        out += " " + std::to_string(from_s) + "–" + std::to_string(to_s) + " 秒";
    }
    return out + "】";
}

bool parse_act_header(const std::string& line_in, std::string* label,
                      int* from_s, int* to_s) {
    const std::string line = strip_ascii(line_in);
    const std::string left = "【", right = "】";
    if (!starts_with(line, left) || !ends_with(line, right)) return false;
    if (line.size() <= left.size() + right.size()) return false;
    const std::string inner = strip_ascii(
        line.substr(left.size(), line.size() - left.size() - right.size()));
    // 里面不能再有括号：「【a】b【c】」也是以【开头、以】结尾的
    if (inner.find(left) != std::string::npos ||
        inner.find(right) != std::string::npos) {
        return false;
    }

    int from = 0, to = 0;
    std::string name = inner;
    const std::string unit = " 秒";
    if (ends_with(inner, unit)) {
        // 「开场钩子 0–5 秒」：最后一个空格前是名字，后面是 from–to
        const std::string head = inner.substr(0, inner.size() - unit.size());
        const std::size_t sp = head.rfind(' ');
        if (sp == std::string::npos) return false;
        name = strip_ascii(head.substr(0, sp));
        const std::string range = head.substr(sp + 1);
        // 分隔符认「–」（我们渲染的）和「-」（人手打的）
        std::size_t dash = range.find("–");
        std::size_t dash_len = 3;
        if (dash == std::string::npos) {
            dash = range.find('-');
            dash_len = 1;
        }
        if (dash == std::string::npos || dash == 0) return false;
        const std::string a = range.substr(0, dash);
        const std::string b = range.substr(dash + dash_len);
        const auto all_digits = [](const std::string& s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
                       return c >= '0' && c <= '9';
                   });
        };
        if (!all_digits(a) || !all_digits(b)) return false;
        from = std::stoi(a);
        to = std::stoi(b);
    }

    bool known = false;
    for (const char* l : prompt::script::kActLabels) {
        if (name == l) {
            known = true;
            break;
        }
    }
    if (!known) return false;
    if (label) *label = name;
    if (from_s) *from_s = from;
    if (to_s) *to_s = to;
    return true;
}

bool is_act_header(const std::string& line) {
    return parse_act_header(line, nullptr, nullptr, nullptr);
}

std::string scene_header(int index, const std::string& body) {
    std::string out = "【第" + std::to_string(index) + "场";
    const std::string b = strip_ascii(body);
    if (!b.empty()) out += " · " + b;
    return out + "】";
}

bool parse_scene_header(const std::string& line_in, int* index,
                        std::string* body) {
    const std::string line = strip_ascii(line_in);
    const std::string left = "【第", right = "】";
    if (!starts_with(line, left) || !ends_with(line, right)) return false;
    if (line.size() <= left.size() + right.size()) return false;
    const std::string inner =
        line.substr(left.size(), line.size() - left.size() - right.size());
    if (inner.find("【") != std::string::npos ||
        inner.find("】") != std::string::npos) {
        return false;
    }
    // 「1场 · 夜 · 内 · 天台」：数字到「场」为止
    std::size_t k = 0;
    while (k < inner.size() && inner[k] >= '0' && inner[k] <= '9') ++k;
    if (k == 0) return false;
    const std::string unit = "场";
    if (inner.compare(k, unit.size(), unit) != 0) return false;
    std::string rest = strip_ascii(inner.substr(k + unit.size()));
    // 序号和正文之间那个分隔符：我们渲染的是「 · 」，人手打的可能是
    // 「：」「，」「、」或者只空一格
    for (const char* sep : {"·", "：", "，", "、", ":", ",", "-"}) {
        const std::string s = sep;
        if (starts_with(rest, s)) {
            rest = strip_ascii(rest.substr(s.size()));
            break;
        }
    }
    if (index) *index = std::stoi(inner.substr(0, k));
    if (body) *body = rest;
    return true;
}

bool is_scene_header(const std::string& line) {
    return parse_scene_header(line, nullptr, nullptr);
}

std::string strip_act_headers(const std::string& script) {
    std::vector<std::string> kept;
    std::size_t start = 0;
    while (start <= script.size()) {
        std::size_t end = script.find('\n', start);
        if (end == std::string::npos) end = script.size();
        const std::string line = script.substr(start, end - start);
        if (!is_act_header(line)) kept.push_back(line);
        if (end == script.size()) break;
        start = end + 1;
    }
    return join(kept, "\n");
}

// ---- ScriptDraft ----

std::vector<std::string> ScriptDraft::speakers() const {
    std::vector<std::string> seen;
    for (const Beat& b : beats) {
        const std::string name = strip_ascii(b.speaker);
        if (b.kind != "dialogue" || name.empty()) continue;
        if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
            seen.push_back(name);
        }
    }
    return seen;
}

std::size_t ScriptDraft::dialogue_chars() const {
    std::size_t n = 0;
    for (const Beat& b : beats) {
        if (b.kind == "dialogue") n += text::utf8_len(b.text);
    }
    return n;
}

std::string ScriptDraft::render() const {
    std::vector<std::string> lines;
    int scenes = 0;
    const auto push = [&](const Beat& b) {
        const std::string t = strip_ascii(b.text);
        if (b.kind == "scene") {
            // 场次头：序号是渲染时数出来的，模型不用管编号。空的也要占一行
            // ——「【第2场】」仍然是一个切点，只是没说在哪。
            lines.push_back(scene_header(++scenes, t));
            return;
        }
        if (t.empty()) return;
        // 潜台词和特效挂在拍子末尾的 〔〕 里：分镜认它、配音那条路
        // （script_dialogue_pairs）会把它削掉。
        std::string tail;
        const std::string sub = strip_ascii(b.subtext);
        const std::string fx = strip_ascii(b.fx);
        if (!sub.empty()) tail += "〔" + sub + "〕";
        if (!fx.empty()) tail += "〔特效：" + fx + "〕";
        if (b.kind == "dialogue") {
            const std::string name = strip_ascii(b.speaker);
            const std::string how = strip_ascii(b.delivery);
            std::string head = name;
            // 「林晚（压着嗓子）：台词」。括号里是怎么说的，分镜填进
            // emotion、配音照着念；script_dialogue_pairs 认这个形状。
            if (!name.empty() && !how.empty()) head += "（" + how + "）";
            lines.push_back(head.empty() ? t + tail : head + "：" + t + tail);
        } else {
            lines.push_back(t + tail);
        }
    };
    if (acts.empty()) {
        for (const Beat& b : beats) push(b);
    } else {
        for (const Act& a : acts) {
            // 段头一行。分镜那一步和页面都靠它知道这一段是什么、占几秒。
            lines.push_back(act_header(a.label, a.from_s, a.to_s));
            for (const Beat& b : a.beats) push(b);
        }
    }
    return join(lines, "\n");
}

// ---- 提示词 ----

std::string build_script_prompt(const std::string& premise, double duration_s,
                                StyleLine style_line,
                                const std::string& previous,
                                const std::vector<std::string>& characters,
                                std::uint32_t variation) {
    std::string out;
    out += prompt::script::kSeg0;
    out += format_f0(duration_s);
    out += prompt::script::kSeg1;
    out += style_line == StyleLine::ANIME ? prompt::script::kHintAnime
                                          : prompt::script::kHintRealistic;
    out += prompt::script::kSeg2;
    out += std::to_string(budget_chars(duration_s));
    out += prompt::script::kRules;
    out += render_act_brief(act_plan(duration_s, variation));

    if (!characters.empty()) {
        out += prompt::script::kCharsPre;
        out += join(characters, "、");
        out += prompt::script::kCharsPost;
    }
    const std::string prev = strip_ascii(previous);
    if (!prev.empty()) {
        out += prompt::script::kPrevPre;
        out += text::truncate_utf8(prev, prompt::script::kPrevMaxChars);
        out += prompt::script::kPrevPost;
    }
    out += prompt::script::kTailHead;
    out += strip_ascii(premise);
    out += prompt::script::kTailEnd;
    return out;
}

std::string build_premise_prompt(const std::string& keywords,
                                 StyleLine style_line, int count,
                                 const std::vector<std::string>& existing) {
    std::string out;
    out += prompt::script_premise::kSeg0;
    out += style_line == StyleLine::ANIME ? prompt::script_premise::kHintAnime
                                          : prompt::script_premise::kHintRealistic;
    out += prompt::script_premise::kSeg1;
    out += std::to_string(count);
    out += prompt::script_premise::kRules;

    const std::string kw = strip_ascii(keywords);
    if (!kw.empty()) {
        out += prompt::script_premise::kKeywordsPre;
        out += kw;
        out += prompt::script_premise::kKeywordsPost;
    }
    if (!existing.empty()) {
        // 已经有的方向要避开，否则连点两次「再想几个」会拿到同一批
        std::vector<std::string> trimmed;
        const std::size_t n =
            std::min<std::size_t>(existing.size(), prompt::script_premise::kExistingMaxItems);
        for (std::size_t i = 0; i < n; ++i) {
            trimmed.push_back(text::truncate_utf8(strip_ascii(existing[i]),
                                                  prompt::script_premise::kExistingMaxChars));
        }
        out += prompt::script_premise::kExistingPre;
        out += join(trimmed, "、");
        out += prompt::script_premise::kExistingPost;
    }
    out += prompt::script_premise::kTailEnd;
    return out;
}

std::string build_trailer_prompt(const std::string& premise, double duration_s,
                                 StyleLine style_line,
                                 const std::string& episodes,
                                 const std::vector<std::string>& characters) {
    std::string out;
    out += prompt::script_trailer::kSeg0;
    out += style_line == StyleLine::ANIME ? prompt::script_trailer::kHintAnime
                                          : prompt::script_trailer::kHintRealistic;
    out += prompt::script_trailer::kSeg1;
    out += format_f0(duration_s);
    out += prompt::script_trailer::kSeg2;
    out += std::to_string(budget_chars(duration_s));
    out += prompt::script_trailer::kRules;

    if (!characters.empty()) {
        out += prompt::script_trailer::kCharsPre;
        out += join(characters, "、");
        out += prompt::script_trailer::kCharsPost;
    }
    const std::string eps = strip_ascii(episodes);
    if (!eps.empty()) {
        out += prompt::script_trailer::kEpisodesPre;
        out += text::truncate_utf8(eps, prompt::script_trailer::kEpisodesMaxChars);
        out += prompt::script_trailer::kEpisodesPost;
    }
    out += prompt::script_trailer::kTailHead;
    out += strip_ascii(premise);
    out += prompt::script_trailer::kTailEnd;
    return out;
}

// ---- Schema ----

namespace {

/// 一拍的 schema。四段的和平的共用；四段那份给 text 加个字数地板。
///
/// characters 非空时把 speaker 收紧成枚举——名字必须一字不差，
/// 空串留给动作行。
ordered beat_item_schema(bool with_floor,
                         const std::vector<std::string>& characters = {},
                         bool rich = false) {
    ordered beat_props = ordered::object();
    beat_props["kind"] = {
        {"type", "string"},
        {"enum", ordered::array({"action", "dialogue", "scene"})},
        {"description",
         "action 是动作或环境描写，dialogue 是有人说话，"
         "scene 是换了地方或时间时起的新一场"}};
    beat_props["speaker"] = {
        {"type", "string"},
        {"description", "说话的人。kind 是 action 时填空字符串"}};
    if (!characters.empty()) {
        ordered names = ordered::array();
        for (const std::string& n : characters) names.push_back(n);
        names.push_back("");   // 动作行
        beat_props["speaker"]["enum"] = names;
    }
    beat_props["text"] = {
        {"type", "string"},
        {"description",
         "这一拍的内容。\n"
         "kind=dialogue：只写说出口的话，不带引号，不重复人名。\n"
         "kind=action：写**画面上看得见的东西**——谁在哪、身体在做"
         "什么、碰到什么物件。换了地方或时间就把光线一并交代。\n"
         "kind=scene：只写「日/夜 · 内/外 · 地点」三样，地点用清单里的名字。"
         "第一拍就得是一场的 scene；同一个地方连着几拍不用重复。\n"
         "不要写心里怎么想（「她很生气」画不出来），也不要一拍塞"
         "三个动作（分镜只能挑一个画，剩下的就丢了）。"}};
    // 空拍凑数在语法层就过不去。两个字是「走。」这种最短的台词。
    if (with_floor) beat_props["text"]["minLength"] = 2;

    ordered items = ordered::object();
    items["type"] = "object";
    items["additionalProperties"] = false;
    items["required"] = {"kind", "speaker", "text"};
    if (rich) {
        // 章模式的四栏。**全进 required**：不在里面的字段模型整个略过
        // （storyboard.cpp 那条坐实过的机制），用不上就填空串 / 空数组。
        ordered names = ordered::object();
        names["type"] = "string";
        if (!characters.empty()) {
            ordered en = ordered::array();
            for (const std::string& n : characters) en.push_back(n);
            names["enum"] = en;
        }
        beat_props["characters"] = {
            {"type", "array"},
            {"items", names},
            {"description",
             "这一拍画面里有谁，用人物表里的名字。环境拍、空镜填空数组"}};
        beat_props["delivery"] = {
            {"type", "string"},
            {"maxLength", 30},
            {"description",
             "kind=dialogue 时这句是怎么说的：压着嗓子、笑着说、说到一半停住。"
             "不是台词就填空串"}};
        beat_props["subtext"] = {
            {"type", "string"},
            {"maxLength", 60},
            {"description",
             "潜台词：这个人此刻真正想要什么、在藏什么、怕什么。不进画面、"
             "不进配音，只给分镜挑机位和表情用。场次头填空串"}};
        beat_props["fx"] = {
            {"type", "string"},
            {"maxLength", 40},
            {"description",
             "这一拍需要的特效，写成画面里看得见的样子（火、雨、烟、玻璃碎、"
             "灯忽然灭）。没有就填空串"}};
        items["required"] = {"kind", "speaker", "text", "characters",
                             "delivery", "subtext", "fx"};
    }
    items["properties"] = beat_props;
    return items;
}

void put_title_and_logline(ordered& props) {
    props["title"] = {{"type", "string"},
                      {"description", "这一章的标题，六个字以内"}};
    props["logline"] = {
        {"type", "string"},
        {"description", "一句话说清这一章发生了什么，给人看的，不进成片"}};
}

}  // namespace

const ordered& script_schema() {
    static const ordered s = [] {
        ordered props = ordered::object();
        put_title_and_logline(props);
        props["beats"] = {
            {"type", "array"},
            {"minItems", 4},
            {"description", "按时间顺序排的场次。动作和对白交替，不要连着五句对白"},
            {"items", beat_item_schema(false)}};

        ordered out = ordered::object();
        out["type"] = "object";
        out["additionalProperties"] = false;
        out["required"] = {"title", "logline", "beats"};
        out["properties"] = props;
        return out;
    }();
    return s;
}

ordered script_schema(double duration_s,
                      const std::vector<std::string>& characters,
                      std::uint32_t variation) {
    const std::vector<ActSpec> specs = act_plan(duration_s, variation);

    ordered props = ordered::object();
    put_title_and_logline(props);
    ordered required = ordered::array({"title", "logline"});
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const ActSpec& s = specs[i];
        ordered beats = ordered::object();
        beats["type"] = "array";
        beats["minItems"] = s.min_beats;
        beats["maxItems"] = s.max_beats;
        beats["description"] = s.label + "，" + std::to_string(s.from_s) + "–" +
                               std::to_string(s.to_s) + " 秒。" + s.brief;
        beats["items"] = beat_item_schema(true, characters);

        ordered act = ordered::object();
        act["type"] = "object";
        act["additionalProperties"] = false;
        act["required"] = {"beats"};
        act["properties"] = ordered::object();
        act["properties"]["beats"] = beats;

        props[s.key] = act;
        required.push_back(s.key);
    }

    ordered out = ordered::object();
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = required;
    out["properties"] = props;
    return out;
}

ordered script_schema_for_chapter(const std::vector<ScenePlan>& scenes,
                                  const std::vector<std::string>& characters) {
    ordered props = ordered::object();
    put_title_and_logline(props);

    // 一场一个命名字段，理由和四段那份一样：GBNF 按 properties 的顺序生成，
    // 几场就一定按顺序、一场不少地出来，而且每场的地板各不相同。
    ordered scene_props = ordered::object();
    ordered scene_required = ordered::array();
    for (const ScenePlan& p : scenes) {
        ordered beats = ordered::object();
        beats["type"] = "array";
        beats["minItems"] = p.min_beats;
        beats["maxItems"] = p.max_beats;
        beats["description"] =
            (p.where.empty() ? std::string("这一场") : p.where) +
            "。第一拍是 kind=scene 的场次头";
        // **一份定义，各场 $ref 它。** 每场各塞一份完整的 beat 定义时，
        // 一章三场就把那一千三百字的东西（七个字段、两串枚举、几段描述）
        // 抄三遍贴给模型——2026-09-17 量的。schema 是以文字贴在提示词后面
        // 发的（llm::schema_as_prompt），抄几遍就是几遍的字。
        beats["items"] = ordered{{"$ref", "#/$defs/Beat"}};

        ordered sc = ordered::object();
        sc["type"] = "object";
        sc["additionalProperties"] = false;
        sc["required"] = {"beats"};
        sc["properties"] = ordered::object();
        sc["properties"]["beats"] = beats;
        scene_props[p.key] = sc;
        scene_required.push_back(p.key);
    }
    ordered scenes_obj = ordered::object();
    scenes_obj["type"] = "object";
    scenes_obj["additionalProperties"] = false;
    scenes_obj["required"] = scene_required;
    scenes_obj["properties"] = scene_props;
    props["scenes"] = scenes_obj;

    ordered out = ordered::object();
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = {"title", "logline", "scenes"};
    out["properties"] = props;
    // 各场共用这一份。**$ref 这套分镜那份 schema 一直在用**（CharacterInShot
    // / DialogueLine / ShotSize 都是这么引的），模型认得。
    out["$defs"] = ordered{{"Beat", beat_item_schema(true, characters, /*rich=*/true)}};
    return out;
}

const ordered& premise_schema() {
    static const ordered s = [] {
        ordered idea_props = ordered::object();
        idea_props["title"] = {{"type", "string"},
                               {"description", "片名，八个字以内"}};
        idea_props["premise"] = {
            {"type", "string"},
            {"description",
             "一两句话说清这部电影讲什么。要具体到人物和处境，不要写题材标签"}};
        idea_props["hook"] = {{"type", "string"},
                              {"description", "一句话说清观众为什么会看下去"}};

        ordered items = ordered::object();
        items["type"] = "object";
        items["additionalProperties"] = false;
        items["required"] = {"title", "premise", "hook"};
        items["properties"] = idea_props;

        ordered ideas = ordered::object();
        ideas["type"] = "array";
        ideas["minItems"] = 3;
        ideas["maxItems"] = 5;
        ideas["items"] = items;

        ordered props = ordered::object();
        props["ideas"] = ideas;

        ordered out = ordered::object();
        out["type"] = "object";
        out["additionalProperties"] = false;
        out["required"] = {"ideas"};
        out["properties"] = props;
        return out;
    }();
    return s;
}

// ---- 解析 ----

namespace {

/// 把一个 beats 数组解析成拍子。四段的和平的回包共用这一段。
void parse_beats_into(const json& arr, std::vector<Beat>& out) {
    if (!arr.is_array()) return;
    for (const auto& item : arr) {
        if (!item.is_object()) continue;
        const std::string kind_raw = get_str(item, "kind");
        std::string kind = kind_raw == "dialogue" ? "dialogue"
                           : kind_raw == "scene"  ? "scene"
                                                  : "action";
        // 场次头：只留「日/夜 · 内/外 · 地点」那一截，空的也留——它是切点。
        if (kind == "scene") {
            out.push_back(Beat{kind, "",
                               strip_wrapper(strip_leading_timecode(
                                   strip_list_marker(get_str(item, "text"))))});
            continue;
        }
        const std::string speaker = normalize_speaker(get_str(item, "speaker"));
        if (kind == "dialogue" && speaker.empty()) {
            // 说了话却没说是谁说的，当**动作行**。
            //
            // 原来这儿的注释写的是"当旁白处理"——**那不是这行代码做的事**。
            // 两者在本系统里不是一回事：旁白是 char_id 为空的台词（有旁白音、
            // 有字幕，见 media/assemble.cpp），动作行是画面描述，永远不会被
            // 念出来。
            //
            // 2026-09-13 认真试过改成真旁白，被对拍基线拦下来了，那条数据
            // 说明了为什么：模型给的空说话人台词是
            //
            //     {"kind":"dialogue","speaker":"","text":"远处传来汽笛声。"}
            //
            // 那是音效/场景描述，kind 标错了。当成旁白的话，旁白音会把
            // 「远处传来汽笛声。」念出来，比现在更糟。
            //
            // 另一面也是真的：实跑（walk_c ep02）见过一句
            // 「我要去医院查清楚那张单子是谁的。」落到这儿，那确实是台词，
            // 变成动作行之后不会被念，还成了一句没法画的画面提示。
            //
            // **两种错法各见过一例，证据不够翻案**，所以维持现状并把两面
            // 都记在这儿。真要分开，靠的不是这一层——得让 schema 表达
            // 「dialogue 必须有说话人」，模型就只能二选一了。
            kind = "action";
        }

        const std::string raw = strip_leading_timecode(get_str(item, "text"));
        // **要在 strip_wrapper 之前判。** 它会把「（脚步声）」削成「脚步声」，
        // 括号一没，这一句就和正常台词长得一模一样了。
        // 整句是圆括号提示的，当动作行——那本来就是场面描述，不是台词。
        if (kind == "dialogue" && is_stage_direction(raw)) kind = "action";
        std::string t = strip_wrapper(raw);
        // 机位标签只削动作行。台词里的「你听我说：」不是机位，
        // 而且台词那一行的说话人是单独一个字段，本来就不会认错。
        if (kind == "action") t = strip_camera_prefix(t);
        // markdown 的列表符号漏进字符串字段（「JSON bleed」）。两种拍子
        // 都要削：台词上的会进字幕，动作行上的会进画面描述。
        t = strip_list_marker(t);
        // 台词里裹着的旁白要剥掉，否则配音会把「她说，语气平静……」
        // 一起念出来。见 strip_speech_tags。
        if (kind == "dialogue") t = strip_wrapper(strip_speech_tags(t, speaker));
        if (t.empty()) continue;

        Beat b{kind, speaker, t};
        // 照着章写时多出来的四栏。按秒排那条路的回包里没有，留空。
        if (const auto it = item.find("characters");
            it != item.end() && it->is_array()) {
            for (const auto& n : *it) {
                if (!n.is_string()) continue;
                const std::string name = strip_ascii(n.get<std::string>());
                if (!name.empty()) b.characters.push_back(name);
            }
        }
        b.delivery = strip_wrapper(strip_ascii(get_str(item, "delivery")));
        b.subtext = strip_wrapper(strip_ascii(get_str(item, "subtext")));
        b.fx = strip_wrapper(strip_ascii(get_str(item, "fx")));
        // 〔〕是渲染时给潜台词用的括号，模型自己写进来的先削掉，不然嵌套。
        for (std::string* f : {&b.delivery, &b.subtext, &b.fx}) {
            std::string& v = *f;
            std::string cleaned;
            for (std::size_t i = 0; i < v.size();) {
                if (v.compare(i, 3, "〔") == 0 || v.compare(i, 3, "〕") == 0) {
                    i += 3;
                    continue;
                }
                cleaned += v[i++];
            }
            v = cleaned;
        }
        out.push_back(std::move(b));
    }
}

}  // namespace

ScriptDraft parse_script(const std::string& raw, double duration_s,
                         std::uint32_t variation) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw ScriptError(e.what());
    }
    if (!data.is_object()) throw ScriptError("大模型没有返回对象");

    ScriptDraft draft;
    draft.title = strip_ascii(get_str(data, "title"));
    draft.logline = strip_ascii(get_str(data, "logline"));

    // 四段的回包：四个键都在才算。少一个就退回平的那条路——
    // 模型偶尔会把四段拍成一个 beats 数组，那样解析出来还是一份完整剧本，只是没段头。
    // **种子要和出 schema、拼提示词时用的是同一个**，否则段头上的秒数
    // 和模型看到的对不上。
    const std::vector<ActSpec> specs = act_plan(duration_s, variation);
    const bool four = std::all_of(specs.begin(), specs.end(), [&](const ActSpec& s) {
        return data.contains(s.key);
    });
    if (four) {
        for (const ActSpec& s : specs) {
            const json& node = data.at(s.key);
            // 认 {"beats": [...]} 也认裸数组
            const json& arr = node.is_object() && node.contains("beats")
                                  ? node.at("beats")
                                  : node;
            Act act;
            act.key = s.key;
            act.label = s.label;
            if (duration_s > 0.0) {
                act.from_s = s.from_s;
                act.to_s = s.to_s;
            }
            parse_beats_into(arr, act.beats);
            draft.beats.insert(draft.beats.end(), act.beats.begin(), act.beats.end());
            draft.acts.push_back(std::move(act));
        }
    } else {
        json beats_raw = data.contains("beats") ? data["beats"] : json();
        if (!beats_raw.is_array() || beats_raw.empty()) {
            throw ScriptError("大模型没写出任何内容");
        }
        parse_beats_into(beats_raw, draft.beats);
    }

    if (draft.beats.empty()) throw ScriptError("大模型写的内容全是空的");
    const bool any_dialogue = std::any_of(
        draft.beats.begin(), draft.beats.end(),
        [](const Beat& b) { return b.kind == "dialogue"; });
    if (!any_dialogue) {
        throw ScriptError("整章一句台词都没有，这样出来的是默片");
    }
    return draft;
}

ScriptDraft parse_chapter_script(const std::string& raw,
                                 const std::vector<ScenePlan>& scenes) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw ScriptError(e.what());
    }
    if (!data.is_object()) throw ScriptError("大模型没有返回对象");

    ScriptDraft draft;
    draft.title = strip_ascii(get_str(data, "title"));
    draft.logline = strip_ascii(get_str(data, "logline"));

    const json& sc = data.contains("scenes") ? data.at("scenes") : json();
    if (sc.is_object() && !scenes.empty()) {
        for (const ScenePlan& p : scenes) {
            if (!sc.contains(p.key)) continue;
            const json& node = sc.at(p.key);
            const json& arr = node.is_object() && node.contains("beats")
                                  ? node.at("beats")
                                  : node;
            std::vector<Beat> beats;
            parse_beats_into(arr, beats);
            if (beats.empty()) continue;
            // 一场的第一拍必须是场次头：它是分镜按场拆镜的切点。
            // 模型漏了就补一个空的——「【第N场】」仍是切点，只是没说在哪。
            if (beats.front().kind != "scene") {
                beats.insert(beats.begin(), Beat{"scene", "", ""});
            }
            draft.beats.insert(draft.beats.end(), beats.begin(), beats.end());
        }
    } else {
        // 模型没按场给（或者根本没有场）：认平的 beats，和预告片一样。
        json beats_raw = data.contains("beats") ? data["beats"] : json();
        if (beats_raw.is_array()) parse_beats_into(beats_raw, draft.beats);
    }

    if (draft.beats.empty()) throw ScriptError("大模型没写出任何内容");
    const bool any_dialogue = std::any_of(
        draft.beats.begin(), draft.beats.end(),
        [](const Beat& b) { return b.kind == "dialogue"; });
    if (!any_dialogue) {
        throw ScriptError("整章一句台词都没有，这样出来的是默片");
    }
    return draft;
}

std::vector<PremiseIdea> parse_premises(const std::string& raw) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw ScriptError(e.what());
    }

    json items;
    if (data.is_object()) {
        items = data.contains("ideas") ? data["ideas"] : json();
    } else {
        items = data;
    }
    if (!items.is_array() || items.empty()) {
        throw ScriptError("大模型没给出任何选题");
    }

    std::vector<PremiseIdea> ideas;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        const std::string premise = strip_wrapper(get_str(item, "premise"));
        if (premise.empty()) continue;
        ideas.push_back(PremiseIdea{strip_wrapper(get_str(item, "title")),
                                    premise,
                                    strip_wrapper(get_str(item, "hook"))});
    }
    if (ideas.empty()) throw ScriptError("大模型给的选题全是空的");
    return ideas;
}

}  // namespace changji::stages
