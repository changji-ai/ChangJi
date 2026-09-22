#include "stages/audio.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <cmath>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <fstream>

#include "stages/storyboard.hpp"
#include "pipeline/task_board.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::stages {

namespace {

double round3(double v) { return std::nearbyint(v * 1000.0) / 1000.0; }


void put_u32(std::ostream& o, std::uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF),
                       static_cast<char>((v >> 8) & 0xFF),
                       static_cast<char>((v >> 16) & 0xFF),
                       static_cast<char>((v >> 24) & 0xFF)};
    o.write(b, 4);
}

void put_u16(std::ostream& o, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF),
                       static_cast<char>((v >> 8) & 0xFF)};
    o.write(b, 2);
}

std::uint32_t get_u32(const std::string& s, std::size_t off) {
    if (off + 4 > s.size()) return 0;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3])) << 24);
}

std::uint16_t get_u16(const std::string& s, std::size_t off) {
    if (off + 2 > s.size()) return 0;
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(s[off]) |
        (static_cast<unsigned char>(s[off + 1]) << 8));
}

}  // namespace

void write_silence(const fs::path& path, double seconds, int sample_rate) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // 至少一帧。零帧的 wav 有些播放器和 ffmpeg 的 concat 会当成损坏文件。
    const std::uint32_t frames = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(seconds * sample_rate));
    const std::uint16_t channels = 1;
    const std::uint16_t bits = 16;
    const std::uint32_t byte_rate =
        static_cast<std::uint32_t>(sample_rate) * channels * (bits / 8);
    const std::uint32_t data_bytes = frames * channels * (bits / 8);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw AudioError("写不了音频文件：" + paths::to_utf8(path));

    f.write("RIFF", 4);
    put_u32(f, 36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put_u32(f, 16);
    put_u16(f, 1);            // PCM
    put_u16(f, channels);
    put_u32(f, static_cast<std::uint32_t>(sample_rate));
    put_u32(f, byte_rate);
    put_u16(f, static_cast<std::uint16_t>(channels * (bits / 8)));
    put_u16(f, bits);
    f.write("data", 4);
    put_u32(f, data_bytes);

    const std::string zeros(4096, '\0');
    std::uint32_t left = data_bytes;
    while (left > 0) {
        const std::uint32_t take = std::min<std::uint32_t>(left, 4096);
        f.write(zeros.data(), take);
        left -= take;
    }
}

std::optional<double> wav_peak_ratio(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    // 整个读进来。这条只在时长短过绝对下限时才会被调，文件也就几十 KB。
    std::string all((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (all.size() < 12 || all.compare(0, 4, "RIFF") != 0 ||
        all.compare(8, 4, "WAVE") != 0) {
        return std::nullopt;
    }
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    std::size_t data_off = 0;
    std::size_t data_len = 0;
    std::size_t off = 12;
    while (off + 8 <= all.size()) {
        const std::string id = all.substr(off, 4);
        const std::uint32_t size = get_u32(all, off + 4);
        if (id == "fmt ") {
            format = get_u16(all, off + 8);
            bits = get_u16(all, off + 22);
        } else if (id == "data") {
            data_off = off + 8;
            data_len = std::min<std::size_t>(size, all.size() - data_off);
            break;
        }
        off += 8 + size + (size % 2);
    }
    if (format != 1 || bits != 16 || data_len < 2) return std::nullopt;

    int peak = 0;
    for (std::size_t i = data_off; i + 1 < data_off + data_len; i += 2) {
        const int v = static_cast<std::int16_t>(get_u16(all, i));
        peak = std::max(peak, v < 0 ? -v : v);
    }
    return peak / 32768.0;
}

namespace {

/// 把 16 位单声道 PCM 读出来，顺带给出采样率。读不了返回空。
///
/// 和 wav_peak_ratio 走同一套块遍历：**不能假定 fmt 在 12、data 在 36**，
/// 很多引擎会插一个 LIST 块写元数据。
struct WavPcm {
    std::vector<double> x;
    int rate = 0;
};

std::optional<WavPcm> read_wav_mono(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string all((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (all.size() < 12 || all.compare(0, 4, "RIFF") != 0 ||
        all.compare(8, 4, "WAVE") != 0) {
        return std::nullopt;
    }
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    std::size_t data_off = 0, data_len = 0;
    std::size_t off = 12;
    while (off + 8 <= all.size()) {
        const std::string id = all.substr(off, 4);
        const std::uint32_t size = get_u32(all, off + 4);
        if (id == "fmt ") {
            format = get_u16(all, off + 8);
            channels = get_u16(all, off + 10);
            rate = get_u32(all, off + 12);
            bits = get_u16(all, off + 22);
        } else if (id == "data") {
            data_off = off + 8;
            data_len = std::min<std::size_t>(size, all.size() - data_off);
            break;
        }
        off += 8 + size + (size % 2);
    }
    if (format != 1 || bits != 16 || channels == 0 || rate == 0 || data_len < 2) {
        return std::nullopt;
    }
    WavPcm out;
    out.rate = static_cast<int>(rate);
    const std::size_t stride = static_cast<std::size_t>(channels) * 2;
    out.x.reserve(data_len / stride);
    // 多声道只取第一路：TTS 出来的是单声道，用户传进来的可能不是，
    // 而混下来对基频没有好处（两路相位差会削掉周期性）。
    for (std::size_t i = data_off; i + stride <= data_off + data_len; i += stride) {
        out.x.push_back(static_cast<std::int16_t>(get_u16(all, i)) / 32768.0);
    }
    return out;
}

/// 一帧里的基频。测不出来返回 0。
///
/// 归一化自相关：`r(lag) = Σx[i]x[i+lag] / sqrt(Σx[i]² · Σx[i+lag]²)`。
/// 归一化这一步不能省——不归一的话 r 随能量单调增，峰值总落在最小的
/// lag（也就是最高的频率）上。
double frame_f0(const std::vector<double>& x, std::size_t from, std::size_t n,
                int rate) {
    // 人声基频取 60~400 Hz。低于 60 的是低频噪声，高于 400 的在中文里
    // 基本只会是倍频误判。
    const std::size_t min_lag = static_cast<std::size_t>(rate / 400);
    const std::size_t max_lag = static_cast<std::size_t>(rate / 60);
    if (min_lag < 2 || n < max_lag * 2) return 0.0;

    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) mean += x[from + i];
    mean /= static_cast<double>(n);

    double e0 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[from + i] - mean;
        e0 += v * v;
    }
    // 静音帧不参与。**判据是能量，不是峰值**：一段削顶的噪声峰值很高，
    // 但它没有周期性，下面那道 0.35 的门槛会把它挡掉。
    if (e0 < 1e-6) return 0.0;

    double best_r = 0.0;
    std::size_t best_lag = 0;
    for (std::size_t lag = min_lag; lag <= max_lag; ++lag) {
        // 右边界：比到 x 的尾巴就停。帧起点越靠后，能比的 lag 越少。
        if (from + n + lag > x.size()) break;
        double num = 0.0, e1 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = x[from + i] - mean;
            const double b = x[from + i + lag] - mean;
            num += a * b;
            e1 += b * b;
        }
        if (e1 < 1e-9) continue;
        const double r = num / std::sqrt(e0 * e1);
        if (r > best_r) {
            best_r = r;
            best_lag = lag;
        }
    }
    // 0.35 是"这一帧确实有周期性"的门槛。清音（s、sh、f）本来就没有
    // 基频，它们该被挡在外面，而不是贡献一个随机数。
    if (best_lag == 0 || best_r < 0.35) return 0.0;
    return static_cast<double>(rate) / static_cast<double>(best_lag);
}

}  // namespace

std::optional<double> estimate_wav_f0(const fs::path& path) {
    const auto pcm = read_wav_mono(path);
    if (!pcm.has_value() || pcm->rate <= 0) return std::nullopt;

    // 40 毫秒一帧、20 毫秒一跳。40 毫秒在 60 Hz 上也有两个多周期，
    // 自相关才站得住。
    const std::size_t frame = static_cast<std::size_t>(pcm->rate * 0.04);
    const std::size_t hop = static_cast<std::size_t>(pcm->rate * 0.02);
    if (frame == 0 || hop == 0 || pcm->x.size() < frame * 2) return std::nullopt;

    std::vector<double> hits;
    for (std::size_t i = 0; i + frame < pcm->x.size(); i += hop) {
        const double f = frame_f0(pcm->x, i, frame, pcm->rate);
        if (f > 0.0) hits.push_back(f);
    }
    // 太少就别给数。一两帧撞对了不代表测出了这段话的基频。
    if (hits.size() < 5) return std::nullopt;

    // **中位数，不是均值。** 浊音段里偶尔有一帧落到倍频或半频上，
    // 均值会被拽走，中位数不会。
    std::sort(hits.begin(), hits.end());
    return hits[hits.size() / 2];
}

double probe_wav_duration(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw AudioError("音频文件读不出时长：" + paths::to_utf8(path));

    // 只读头部。一段几十秒的 wav 是几兆，为了读时长全读进来是浪费，
    // 而配音一章要读几十次。
    std::string head(1024, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    if (head.size() < 12 || head.compare(0, 4, "RIFF") != 0 ||
        head.compare(8, 4, "WAVE") != 0) {
        throw AudioError("不是 WAV 文件：" + paths::to_utf8(path));
    }

    // **块要逐个走，不能假定 fmt 在 12、data 在 36。** 很多 TTS 引擎
    // 会插一个 LIST 块写元数据，按固定偏移读的话拿到的是垃圾数——
    // 而垃圾数会变成一个荒唐的时长，然后镜头按它锁定。
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t data_bytes = 0;

    std::size_t off = 12;
    while (off + 8 <= head.size()) {
        const std::string id = head.substr(off, 4);
        const std::uint32_t size = get_u32(head, off + 4);
        if (id == "fmt ") {
            channels = get_u16(head, off + 10);
            sample_rate = get_u32(head, off + 12);
            bits = get_u16(head, off + 22);
        } else if (id == "data") {
            data_bytes = size;
            break;
        }
        // 块大小是奇数时后面有一个填充字节。
        off += 8 + size + (size % 2);
    }

    if (sample_rate == 0 || channels == 0 || bits == 0) {
        throw AudioError("音频文件读不出时长：" + paths::to_utf8(path));
    }
    if (data_bytes == 0) {
        // data 块在头 1KB 之外（元数据很长时会这样）。退回按文件大小估：
        // 比抛异常强——那会让整条流水线断在一句台词上。
        std::error_code ec;
        const auto total = fs::file_size(path, ec);
        if (ec || total <= off) {
            throw AudioError("音频文件读不出时长：" + paths::to_utf8(path));
        }
        data_bytes = static_cast<std::uint32_t>(total - off - 8);
    }

    const double frames =
        static_cast<double>(data_bytes) / (channels * (bits / 8.0));
    return frames / sample_rate;
}

TTSBackend estimate_backend(int sample_rate) {
    TTSBackend b;
    b.name = "estimate";
    b.synthesize = [sample_rate](const std::string& text, const fs::path& out,
                                 const std::optional<std::string>&,
                                 const std::string&, double) {
        SynthesisResult r;
        r.duration_s = estimate_speech_duration(text);
        write_silence(out, r.duration_s, sample_rate);
        r.audio_path = out;
        return r;
    };
    // 刻意不给 list_voices：估算后端没有音色的概念，
    // 给一个空列表的话会被当成"问到了但一条都没有"，
    // 而那会让 voice_for 去自动挑选，挑出个空的。
    return b;
}

// ---- AudioStage ----

AudioStage::AudioStage(TTSBackend backend, config::TTSConfig config,
                       models::ProjectPaths paths)
    : backend_(std::move(backend)),
      config_(std::move(config)),
      paths_(std::move(paths)) {}

void AudioStage::split_long_lines(models::Shot& shot) const {
    const double limit = max_line_seconds();
    std::vector<models::DialogueLine> out;
    for (const auto& line : shot.dialogue) {
        const auto pieces = split_long_text(line.text, limit);
        if (pieces.size() <= 1) {
            out.push_back(line);
            continue;
        }
        for (const auto& piece : pieces) {
            models::DialogueLine clone = line;
            clone.text = piece;
            // 切开之后原来那份音频对不上了，清掉重新合成。
            // 不清的话新的一段会指向整句的音频，混音时那一句会被念两遍。
            clone.audio_path.reset();
            clone.actual_duration_s.reset();
            out.push_back(clone);
        }
    }
    shot.dialogue = out;
}

std::optional<std::string> resolve_voice(const std::optional<std::string>& voice,
                                         const models::ProjectPaths& paths) {
    if (!voice.has_value() || voice->empty()) return voice;
    const fs::path raw = paths::from_utf8(*voice);
    if (raw.is_absolute()) return voice;
    std::error_code ec;
    const fs::path abs_p = paths.abs(*voice);
    if (!fs::is_regular_file(abs_p, ec)) return voice;
    return paths::to_utf8(abs_p);
}

const std::vector<std::string>& AudioStage::available_voices() {
    if (!voices_.has_value()) {
        // 查一次就够，缓存住。一章四十镜、每镜两句，不缓存就是八十次请求。
        voices_ = backend_.list_voices ? backend_.list_voices()
                                       : std::vector<std::string>{};
    }
    return *voices_;
}

std::optional<std::string> AudioStage::voice_for(
    const models::DialogueLine& line, const models::AssetLibrary& assets) {
    // **音色由角色资产决定，不由分镜决定。** 分镜是大模型写的，
    // 它没有理由知道这个项目里有哪些音色。
    const models::Character* ch = nullptr;
    if (line.char_id.has_value()) {
        const auto it = assets.characters.find(*line.char_id);
        if (it != assets.characters.end()) ch = &it->second;
    }
    const std::optional<std::string> wanted =
        ch != nullptr ? ch->voice_id : line.voice_id;

    const auto& available = available_voices();
    if (available.empty()) {
        // 问不到列表就别自作主张，照填的来。
        return wanted;
    }

    if (wanted.has_value() && !wanted->empty()) {
        if (std::find(available.begin(), available.end(), *wanted) !=
            available.end()) {
            // 人工指定的优先，不该被自动挑选覆盖。
            return wanted;
        }
        // 老项目里存的可能是 v_角色名 这种早年自造的 id，服务端不认。
        // 原样提交上去节点会拒绝，整条流水线断在配音这一步。
        // 退回自动挑选，让老项目还能跑，而不是让人先去挨个改一遍音色。
        {
            std::lock_guard<std::mutex> lg(unknown_mu_);
            unknown_.insert(*wanted);
        }
    }

    if (ch == nullptr) {
        return models::pick_voice(available, "", 0);   // 旁白
    }
    return models::pick_voice(available, ch->voice_gender, ch->voice_order);
}

/// 分镜排到这个长度以上的，当它是有意的长镜头：配音不许压短。
static constexpr double kLongShotS = 8.0;

double AudioStage::lock_duration(models::Shot& shot, double speech_s) const {
    // 没有台词的镜头保持分镜给的时长不动，它们是节奏调节的余量。
    if (shot.dialogue.empty()) return shot.duration_s;
    // 有台词的向上吸附到可生成档位并锁定。**宁长勿短**：
    // 短了会截断台词，长了尾巴上留一点表演余韵反而自然。
    //
    double locked = ceil_duration(speech_s + kTailS);
    // **分镜明确规划了多段动作的，不许压短。**
    //
    // 上面那句「宁长勿短」说的是吸附方向（ceil 不是 floor），原来没拿结果
    // 和分镜计划的那个数比——于是一句两秒的台词能把一个计划 15 秒的长镜头
    // 压成 3 秒，而分镜里为那 15 秒写好的三段动作
    // （`[0-5秒] / [5-10秒] / [10-15秒]`）连同镜头本身一起没了。
    // 2026-09-16 实测：ep05_sh007 计划 15 秒、配音锁成 3 秒。
    //
    // **判据是「运动描述分了几段」，不是一律不许压。** 一段的镜头（绝大
    // 多数对话镜）照旧跟着台词收紧，节奏不变；分了两段以上的，说明分镜
    // 为这一镜安排了好几件事要演，那个时长是它的判断，配音只知道"话有
    // 多长"，不该替它决定。这样改动只落在真正的长镜头上，别的一个不碰。
    //
    // 整章时长超了由配音之后那次 rebalance 去收，而它动的正是没有台词的
    // 那些镜头。
    // **计划得明显长的镜头同样不压。** 只看分了几段的话，一个计划 12 秒、
    // 运动描述只写了一段的长镜头照样被一句两秒的台词压成 3 秒
    // （2026-09-16 查出）。8 秒以上的镜头一定是分镜有意排的长镜头。
    if (count_motion_segments(shot.motion_prompt) >= 2 ||
        shot.duration_s >= kLongShotS) {
        locked = std::max(locked, shot.duration_s);
    }
    shot.duration_s = locked;
    shot.duration_locked = true;
    // **改了时长就把运动描述跟着改。** 和 rebalance_durations 末尾那段同一
    // 件事：分镜写的是 `[0-3秒]`，配音把这一镜锁到 5 秒，多出来那两秒没人
    // 描述，出片模型自由发挥——而它发挥的方式是把主体丢掉。
    if (!shot.motion_prompt.empty()) {
        shot.motion_prompt = motion_covering(shot.motion_prompt, locked);
    }
    return locked;
}

ShotAudioPlan AudioStage::process_shot(models::Shot& shot,
                                       const models::AssetLibrary& assets,
                                       pipeline::CancelToken& tok) {
    // 过长的台词先按字数估一遍切开。估算只是第一道——
    // 合成出来的真实时长常常比估的长两成，所以合成之后还要再验一次。
    split_long_lines(shot);
    const double limit = max_line_seconds();

    std::vector<models::DialogueLine> done;
    std::deque<models::DialogueLine> queue(shot.dialogue.begin(),
                                           shot.dialogue.end());
    int idx = 0;
    int splits = 0;

    while (!queue.empty()) {
        if (tok.cancelled()) throw AudioError("已取消");
        models::DialogueLine line = queue.front();
        queue.pop_front();

        const auto voice = voice_for(line, assets);
        // **参考音色是项目里的相对路径，喂给合成器之前要还原成绝对的。**
        //
        // 进程内配音把 voice_id 当一段音频的路径用（tts_backends.cpp 里
        // `req.speaker_ref`），而资产库里存的是相对项目根的
        // `voices/c_xxx.wav`——那是有意的，项目整个拷到别的机器上还能读。
        // 不还原的话它解析到**引擎进程的当前目录**，服务器上是 /root，
        // 必然打不开，而报的错只是一句"参考音色读不了"。
        // 和 2026-09-13 早些时候参考图那个坑是同一个形状。
        //
        // **只还原真的落在项目里的那种。** 外部配音服务那条路上 voice_id
        // 是个音色名（"zh-CN-XiaoxiaoNeural" 之类），不是路径，碰都不能碰。
        const auto voice_arg = resolve_voice(voice, paths_);
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%02d.wav", shot.shot_id.c_str(), idx);
        const fs::path out = paths_.audio() / paths::from_utf8(name);

        const SynthesisResult result = backend_.synthesize(
            line.text, out, voice_arg, line.emotion, line.emotion_intensity);

        // **合成出来还是装不下，按实测语速重切一次再合成。**
        //
        // 光靠估算不行：估的是每秒 4.6 个字，不同引擎不同音色差得很远。
        // 实测有过估 4.8 秒、出来 5.8 秒的，那 1 秒就盖到下一镜上了。
        if (result.duration_s > limit && splits < kMaxResplits &&
            text::utf8_len(line.text) > 6) {
            // 估算偏了多少就把预算缩多少：估 E 秒实际 D 秒，
            // 想让实际落到 limit，就得按 limit * E / D 去估。
            // 再留一成余量，免得来回重切。
            const double est = estimate_speech_duration(line.text);
            const double scaled = limit * est / result.duration_s * 0.9;
            const auto pieces = split_long_text(line.text, std::max(0.8, scaled));
            if (pieces.size() > 1) {
                ++splits;
                // 倒着插回队首，保持原来的先后。
                for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
                    models::DialogueLine clone = line;
                    clone.text = *it;
                    clone.audio_path.reset();
                    clone.actual_duration_s.reset();
                    queue.push_front(clone);
                }
                continue;
            }
        }

        line.actual_duration_s = round3(result.duration_s);
        if (result.audio_path.has_value()) {
            line.audio_path = paths_.rel(*result.audio_path);
        }
        line.voice_id = voice;
        done.push_back(line);
        ++idx;
    }

    shot.dialogue = done;
    double total = 0.0;
    for (const auto& l : done) total += l.actual_duration_s.value_or(0.0);
    const double locked = lock_duration(shot, total);

    ShotAudioPlan plan;
    plan.shot_id = shot.shot_id;
    plan.speech_duration_s = round3(total);
    plan.locked_duration_s = locked;
    plan.slack_s = round3(locked - total);
    plan.lines = static_cast<int>(done.size());
    return plan;
}

std::vector<ShotAudioPlan> AudioStage::run(std::vector<models::Shot*>& shots,
                                           const models::AssetLibrary& assets,
                                           pipeline::JobProgress& progress,
                                           pipeline::CancelToken& tok,
                                           int concurrency) {
    const int total = static_cast<int>(shots.size());

    /// 一镜的结果。**只装数据，不碰 Shot**——写回统一放到后面在调用线程上做。
    struct Done {
        /// 这一格真跑过没有。**写回的唯一凭据。**
        /// 同 render.cpp / frames.cpp 的 `Done::ran`：那两处 2026-09-17
        /// 丢过一章的数据（没跑过的一格装的是默认构造的空壳，照样被写回去）。
        /// 这一层从一开始就用正面判据。
        bool ran = false;
        bool ok = false;
        ShotAudioPlan plan;
        /// 跑完的那份副本。process_shot 就地改的是它，不是原镜头。
        models::Shot shot;
    };
    std::vector<Done> done(shots.size());

    // **音色表开跑前先查一次。** 它是懒加载的（available_voices），
    // 几路同时撞上去就是几次重复请求，还得给 voices_ 加锁；
    // 预热之后那一份只读，锁一把都不用。
    available_voices();

    // 同时跑几镜。上限是镜头数——池里两个位置而只有一镜时，起两条线程只是白占。
    const int lanes = std::max(1, std::min(concurrency, total));

    // 每一镜在任务账本上占一行，**开工之前就全部登记**。理由见 frames.cpp
    // 那处。标题里带上这一镜第一句台词——配音那一族光报镜头号，页面上
    // 二十行长得一模一样（用户 2026-09-17：「任务名要显示清楚干什么的」）。
    const std::string project_path = paths::to_utf8(paths_.root());
    std::vector<std::unique_ptr<pipeline::Task>> tasks;
    tasks.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        std::string line;
        if (!shots[i]->dialogue.empty()) {
            line = shots[i]->dialogue.front().text;
            // 一行放得下的长度。截了要说一声，不然读起来像台词本来就断了。
            if (line.size() > 30) line = line.substr(0, 30) + "…";
        }
        tasks.push_back(std::make_unique<pipeline::Task>(
            "tts",
            line.empty() ? "配音 · " + shots[i]->shot_id
                         : "配音 · " + shots[i]->shot_id + "「" + line + "」",
            project_path));
        tasks.back()->token().link(&tok);
    }

    std::atomic<int> next{0};

    const auto lane = [&] {
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= total) return;
            if (tok.cancelled()) continue;   // 剩下的都不跑，ran 留假

            models::Shot* shot = shots[i];
            const int index = i + 1;
            pipeline::Task& task = *tasks[i];
            if (task.cancelled()) continue;   // 单独取消了这一镜，ran 留假
            task.begin();
            // **本地副本。** process_shot 会就地改台词（重切）和时长，
            // 并行那一段一个字节都不往 shots 里写。
            models::Shot local = *shot;
            done[i].ran = true;

            {
                pipeline::Event e;
                e.stage = "audio";
                e.kind = "progress";
                e.current = index;
                e.total = total;
                e.shot_id = local.shot_id;
                e.message = "配音 " + local.shot_id;
                progress.report(e);
            }

            try {
                done[i].plan = process_shot(local, assets, task.token());
                done[i].ok = true;
                local.status = models::ShotStatus::AUDIO_DONE;
            // **这一镜完了要说一声。** 界面把"带 shot_id 的 progress"当成
            // "这一镜正在跑"，靠一条非 progress 的事件把它移出去。
            // 只报 progress 不报完成的话，这一镜会永远挂在"正在配音"上——
            // 一章二十二镜跑完配音之后，整面墙都写着「配音」，包括那些
            // 其实只是在等的。用户看到的就是这个（2026-09-10 报的）。
                pipeline::Event fin;
                fin.stage = "audio";
                fin.kind = "shot_done";
                fin.current = index;
                fin.total = total;
                fin.shot_id = local.shot_id;
                fin.message = local.shot_id + " 配音完成";
                progress.report(fin);
            } catch (const std::exception& e) {
                // 一镜配音失败不拖垮后面几镜。这一镜的状态不推进，
                // 后面的闸门会看出"有台词但没有配音时长"。
                done[i].ok = false;
                task.fail(e.what());
                pipeline::Event ev;
                ev.stage = "audio";
                ev.kind = "warn";
                ev.shot_id = local.shot_id;
                ev.message = local.shot_id + " 配音失败：" + e.what();
                progress.report(ev);
            }
            done[i].shot = std::move(local);
            // 边跑边结账，见 frames.cpp 同一处。
            tasks[i].reset();
        }
    };

    if (lanes == 1) {
        lane();   // 串行那条路一个线程都不起，行为和以前逐字节一样
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(lanes));
        for (int k = 0; k < lanes; ++k) pool.emplace_back(lane);
        for (auto& t : pool) t.join();
    }

    // ---- 收。**在调用线程上按镜头原顺序改 Shot** ----
    std::vector<ShotAudioPlan> plans;
    for (int i = 0; i < total; ++i) {
        if (!done[i].ran) continue;   // 没跑过的一格都不碰
        *shots[i] = std::move(done[i].shot);
        if (done[i].ok) plans.push_back(std::move(done[i].plan));
    }

    if (!unknown_.empty()) {
        std::string names;
        for (const auto& v : unknown_) {
            if (!names.empty()) names += "、";
            names += v;
        }
        pipeline::Event e;
        e.stage = "audio";
        e.kind = "warn";
        // 指路要指到今天真有的那个地方：「角色场景页」2026-09-11 就合成
        // 「设定」了，而且那一格从来不是下拉框——音色是一个带候选清单的
        // 输入框（进程内配音要一段参考音频的路径，外部服务要它自己认的
        // 音色名，两种都得能手填，见 AssetCharacters 里那段注释）。
        e.message = "这些音色服务端上没有，已自动换成可用的：" + names +
                    "。想指定的话去设定页的「人物」，点开这个角色，"
                    "在「音色」那一格填或者挑一段";
        progress.report(e);
    }
    return plans;
}

}  // namespace changji::stages
