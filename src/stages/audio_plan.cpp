#include "stages/audio_plan.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "stages/render.hpp"
#include "stages/storyboard.hpp"   // split_motion
#include "util/text.hpp"

namespace changji::stages {

namespace {

/// 标点不发音，但产生停顿。逗号短一点，句号问号感叹号长一点。
///
/// 把标点当成字算的话，一句"什么？！"会被估成四个字的长度，
/// 而它实际上要停顿快一秒。
const std::map<std::string, double>& pauses() {
    static const std::map<std::string, double> m = {
        {"，", 0.18}, {"、", 0.12}, {"；", 0.22}, {"：", 0.18},
        {"。", 0.32}, {"？", 0.35}, {"！", 0.35}, {"…", 0.40}, {"—", 0.25},
    };
    return m;
}

const std::vector<std::string>& sentence_ends() {
    static const std::vector<std::string> v = {"。", "！", "？", "…"};
    return v;
}

const std::vector<std::string>& clause_ends() {
    static const std::vector<std::string> v = {"；", "，", "、"};
    return v;
}

bool in(const std::vector<std::string>& set, const std::string& ch) {
    return std::find(set.begin(), set.end(), ch) != set.end();
}

/// 去掉全部空白（不只是首尾）。对齐 Python 的 re.sub(r"[\s]", "", text)。
std::string strip_all_space(const std::string& s) {
    std::string out;
    for (const auto& ch : text::utf8_chars(s)) {
        if (ch.size() == 1) {
            const unsigned char c = static_cast<unsigned char>(ch[0]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
                c == '\f') {
                continue;
            }
        }
        out += ch;
    }
    return out;
}

std::string join_chars(const std::vector<std::string>& v, std::size_t from,
                       std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < v.size(); ++i) out += v[i];
    return out;
}

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

}  // namespace

double estimate_speech_duration(const std::string& text_in) {
    const std::string stripped = strip_all_space(text_in);
    if (stripped.empty()) return 0.0;

    double pause_total = 0.0;
    int spoken = 0;
    for (const auto& ch : text::utf8_chars(stripped)) {
        const auto it = pauses().find(ch);
        if (it != pauses().end()) {
            pause_total += it->second;
        } else {
            ++spoken;
        }
    }
    return spoken / kCharsPerSecond + pause_total + kLeadInS + kTailS;
}

double max_line_seconds(int fps) {
    // 跟着视频模型的上限走：换上能出长镜头的模型，一句台词也就能更长，
    // 不用再被切成一句一镜。
    return max_shot_duration_s(fps) - kTailS;
}


/// 这一段除了标点和空白，还有没有能念出声的字。声明在 audio_plan.hpp
/// （导出来是为了让 test_audio_plan 用同一份判据，见那儿的说明）。
///
/// **不能用 text::rstrip_punct 代替。** 那个表只有八个字符
/// （。．；；，，、和空格），是给"清理句尾"用的，不含 ！？…—
/// 之类——实测就是栽在这儿：修完之后「！」和「…」照旧被切成独立片段。
///
/// 这里要判的是另一件事：**这一段送给 TTS 会不会念出东西来**。所以列的是
/// 中英文常见的标点和引号括号，宁可多列几个：漏掉一个的代价是一段纯标点
/// 进了合成器，而那东西配出来多长完全不可预期（实测一个「！」出过 41 秒）。
bool has_speakable(const std::string& s) {
    static const std::set<std::string> kMarks = {
        "。", "，", "、", "；", "：", "！", "？", "…", "—", "－", "～",
        "「", "」", "『", "』", "“", "”", "‘", "’", "（", "）", "《", "》",
        "〈", "〉", "【", "】", "·", "‥", "﹏", "　",
        ".", ",", ";", ":", "!", "?", "-", "~", "\"", "'", "(", ")",
        "[", "]", "{", "}", "<", ">", "/", "\\", "|", "*", "_", "+", "=",
        " ", "\t", "\n", "\r",
    };
    for (const auto& ch : text::utf8_chars(s)) {
        if (kMarks.count(ch) == 0) return true;
    }
    return false;
}


std::vector<std::string> split_long_text(const std::string& text_in,
                                         double max_seconds) {
    if (estimate_speech_duration(text_in) <= max_seconds) return {text_in};

    // 在给定的一组标点处切。**只有攒够 60% 的预算才切**——
    // 见一个标点就切的话，"你好，我来了。"会被切成三段，
    // 每段一秒不到，配出来是三段各自带头尾留白的碎音频。
    const auto chunks_by = [max_seconds](const std::vector<std::string>& marks,
                                         const std::vector<std::string>& src) {
        std::vector<std::string> out;
        for (const auto& part : src) {
            if (estimate_speech_duration(part) <= max_seconds) {
                out.push_back(part);
                continue;
            }
            std::string buf;
            for (const auto& ch : text::utf8_chars(part)) {
                buf += ch;
                if (in(marks, ch) &&
                    estimate_speech_duration(buf) >= max_seconds * 0.6) {
                    out.push_back(buf);
                    buf.clear();
                }
            }
            if (!buf.empty()) out.push_back(buf);
        }
        return out;
    };

    std::vector<std::string> pieces = chunks_by(sentence_ends(), {text_in});
    pieces = chunks_by(clause_ends(), pieces);

    // 还有过长的就只能按字数硬切。**每一段都要自带头尾的呼吸留白**，
    // 算字数时先把这部分扣掉，否则切出来的每一段都刚好超一点。
    const double speakable = std::max(0.5, max_seconds - kLeadInS - kTailS);
    const std::size_t take =
        std::max<std::size_t>(1, static_cast<std::size_t>(speakable * kCharsPerSecond));

    std::vector<std::string> final_pieces;
    for (const auto& piece : pieces) {
        std::vector<std::string> chars = text::utf8_chars(piece);
        while (estimate_speech_duration(join_chars(chars, 0, chars.size())) >
               max_seconds) {
            final_pieces.push_back(join_chars(chars, 0, take));
            if (chars.size() <= take) {
                chars.clear();
                break;
            }
            chars.erase(chars.begin(),
                        chars.begin() + static_cast<std::ptrdiff_t>(take));
        }
        if (!chars.empty()) {
            final_pieces.push_back(join_chars(chars, 0, chars.size()));
        }
    }

    std::vector<std::string> out;
    for (const auto& p : final_pieces) {
        const std::string t = text::strip_ws(p);
        if (t.empty()) continue;

        // **不能留下只有标点的碎片。**
        //
        // 硬切是按字数切的，最后很容易剩一个「！」之类的尾巴。而 TTS 拿到
        // 孤零零一个标点，合成出来的东西不可预期——实测出过一句
        // 「…我得赔光这个月的房租！」被切成三段，最后那段就是一个「！」，
        // **配出来 40.96 秒**，而那一镜只有 5 秒，声音盖住后面好几镜。
        // 估算那边完全看不出来：标点不发音，estimate_speech_duration 给的
        // 是零点几秒，所以它一路过了所有检查。
        //
        // 标点要**并回前一段**，不是丢掉：它是前一句的语气，去掉之后
        // 那句话的停顿和情绪都变了。前面没有段可并（整句就是个标点）时
        // 原样留着——那种输入本来就不该出现，真出现了让它显出来，
        // 比悄悄吞掉强。
        if (!has_speakable(t) && !out.empty()) {
            out.back() += t;
            continue;
        }
        out.push_back(t);
    }
    return out;
}

std::string free_shot_id(const std::string& base,
                         const std::set<std::string>& used) {
    for (const char suffix : std::string("bcdefghijklmnopqrstuvwxyz")) {
        const std::string candidate = base + "_" + suffix;
        if (used.count(candidate) == 0) return candidate;
    }
    // 二十五个后缀都用光了（一个镜头拆出二十六段），退回数字。
    int n = 2;
    while (used.count(base + "_" + std::to_string(n)) != 0) ++n;
    return base + "_" + std::to_string(n);
}

/// 已经配过音的用真实时长，没配过的用估算。混着用是对的：
/// 拆分发生在配音之后，但重跑时可能有几句还没配。
double dur_of(const models::DialogueLine& line) {
    if (line.actual_duration_s.has_value()) return *line.actual_duration_s;
    return estimate_speech_duration(line.text);
}

std::vector<std::vector<models::DialogueLine>> group_lines(
    const models::Shot& shot, double max_seconds) {
    if (shot.dialogue.size() <= 1) return {shot.dialogue};

    const auto& dur = dur_of;

    const double budget = std::max(0.5, max_seconds - kTailS);
    std::vector<std::vector<models::DialogueLine>> groups;
    std::vector<models::DialogueLine> current;
    double total = 0.0;

    for (const auto& line : shot.dialogue) {
        const double d = dur(line);
        // **一句话就超预算时也要单独成组**，不能因为装不下就丢掉。
        // 所以先看 current 非空——空的时候无条件收下。
        if (!current.empty() && total + d > budget) {
            groups.push_back(current);
            current.clear();
            total = 0.0;
        }
        current.push_back(line);
        total += d;
    }
    if (!current.empty()) groups.push_back(current);
    return groups;
}

std::vector<models::Shot> split_overlong_shots(std::vector<models::Shot> shots,
                                               double max_seconds) {
    // 编号要跟**整章**比对着发。同一章重跑一次配音会再拆一次，
    // 只按本次的序号取名的话第二次又会取出一个 sh001_b，
    // 于是一章里出现两个同名镜头：按 id 找镜头只能找到头一个，
    // 音频和首帧的文件名也会互相覆盖。
    std::set<std::string> used;
    for (const auto& s : shots) used.insert(s.shot_id);

    std::stable_sort(shots.begin(), shots.end(),
                     [](const models::Shot& a, const models::Shot& b) {
                         return a.order < b.order;
                     });

    std::vector<models::Shot> out;
    for (auto& shot : shots) {
        auto groups = group_lines(shot, max_seconds);
        if (groups.size() <= 1) {
            out.push_back(shot);
            continue;
        }
        // **运动描述要跟着一起拆。**
        //
        // 深拷贝把整条时间轴原样带给了每一份：ep05 那一镜三段
        // （`[0-5秒] / [5-10秒] / [10-15秒]`）拆成两镜之后，两镜挂着
        // 一模一样的十五秒时间轴，出来的就是两条几乎一样的视频接在一起，
        // 而且每一镜的时间轴都比它自己的时长长一倍。2026-09-16 实测：
        // ep05_sh019 和 ep05_sh019_b 的 motion_prompt 一字不差。
        //
        // 按各份台词的长短分段，分完每份的时间轴重新从 0 起算。
        std::vector<double> weights;
        weights.reserve(groups.size());
        for (const auto& g : groups) {
            double d = 0.0;
            for (const auto& line : g) d += dur_of(line);
            weights.push_back(d);
        }
        const std::vector<std::string> motions =
            split_motion(shot.motion_prompt, weights);

        shot.dialogue = groups[0];
        if (!motions.empty()) shot.motion_prompt = motions[0];
        out.push_back(shot);

        for (std::size_t g = 1; g < groups.size(); ++g) {
            models::Shot extra = shot;   // 深拷贝
            extra.shot_id = free_shot_id(shot.shot_id, used);
            used.insert(extra.shot_id);
            extra.dialogue = groups[g];
            if (g < motions.size()) extra.motion_prompt = motions[g];
            // **新镜是全新的画面，之前那一镜的产物一概不能继承。**
            // 继承的话，新镜会带着原镜的 frame_path 和 video_path，
            // 流水线看到"已经有产物"就跳过它，成片里那一段是重复的画面。
            extra.frame_path.reset();
            extra.video_path.reset();
            extra.attempts = 0;
            extra.gate_notes.clear();
            extra.status = models::ShotStatus::PLANNED;
            extra.duration_locked = false;
            out.push_back(extra);
        }
    }

    // order 是整数，拆完统一重排一遍。不重排的话新镜和原镜同号，
    // 排序不稳定时成片里两镜的先后是随机的。
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].order = static_cast<int>(i);
    }
    return out;
}

std::string summarize(const std::vector<ShotAudioPlan>& plans) {
    if (plans.empty()) return "没有需要配音的镜头";

    double total_speech = 0.0;
    // **只数有台词的那几镜。** 见下面为什么。
    double locked_by_lines = 0.0;
    int shots_with_lines = 0;
    int total_lines = 0;
    std::vector<std::string> tight;
    for (const auto& p : plans) {
        total_speech += p.speech_duration_s;
        total_lines += p.lines;
        if (p.lines) {
            locked_by_lines += p.locked_duration_s;
            ++shots_with_lines;
        }
        // 没有台词的镜头留白当然充足，不该出现在"留白不足"的名单里。
        if (p.is_tight() && p.lines) tight.push_back(p.shot_id);
    }

    // **这句话必须在 rebalance 之后依然成立。**
    //
    // 原来写的是「锁定后镜头总长 X 秒」，是 plans 里所有镜头的和。两处错：
    //
    //   * 听上去像整章，其实不是（没台词的过渡镜不一定在 plans 里）；
    //   * 它是**配音刚锁完那一刻**的值，而 rebalance 在它之后才跑。
    //
    // 实测 walk_c ep01：这句报 71.0 秒，而同一屏上一行刚说整章重排到了
    // 61.8 秒——同样是这 18 个镜头，两个数，人只能当其中一个是错的。
    //
    // 现在只报**有台词那几镜**的时长：rebalance 明确不动它们
    // （storyboard.cpp 里只挑 `dialogue.empty() && !duration_locked`），
    // 所以这个数跑完之后还是对的。而且它正好解释了这一章为什么压不更短。
    // 整章多长由 pipeline/episode.cpp 在 rebalance 之后单独报。
    std::string out =
        "配音完成 " + std::to_string(total_lines) + " 句，覆盖 " +
        std::to_string(plans.size()) + " 个镜头\n语音总长 " +
        fmt("%.1f", total_speech) + " 秒，台词把 " +
        std::to_string(shots_with_lines) + " 个镜头钉死在 " +
        fmt("%.1f", locked_by_lines) + " 秒，这部分压不动";

    if (!tight.empty()) {
        out += "\n其中 " + std::to_string(tight.size()) +
               " 个镜头留白不足半秒，装配时不能再压缩：";
        // 只列前五个。四十镜里有二十个紧的时候，列全了没人会读。
        for (std::size_t i = 0; i < tight.size() && i < 5; ++i) {
            if (i) out += "、";
            out += tight[i];
        }
    }
    return out;
}

}  // namespace changji::stages
