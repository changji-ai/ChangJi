#include "infer/sd_image.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <vector>

#include <nlohmann/json.hpp>

#include "infer/lora_names.hpp"
#include "infer/scheduler.hpp"
#include "models/hardware.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

#ifdef CHANGJI_HAVE_SD
#include <stable-diffusion.h>
// **两个 STATIC 不能删。** 它们让 stb 的所有函数变成内部链接，
// 只在这个 .cpp 里可见。
//
// 不加的话：开了 CHANGJI_LLAMA 之后 mtmd 也带一份 stb（vendor::stb），
// 链接时几十个 stbi_* 符号重复定义，直接 LNK1169。两份 stb 同时存在
// 是常态而不是意外——sd.cpp 和 llama.cpp 各自 vendor 了一份，
// 这正是方案风险二说的那类问题，只是这次撞在第三方小库上。
//
// 用 STATIC 而不是"改成用 mtmd 那份"：两份 stb 的版本不一定一样，
// 借用别人的实现等于把自己的行为绑在对方的升级上。
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <thirdparty/stb_image_write.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <thirdparty/stb_image.h>
#endif

namespace fs = std::filesystem;

namespace changji::infer {

namespace {
thread_local const config::Settings* t_task_settings = nullptr;
}

ScopedTaskSettings::ScopedTaskSettings(const config::Settings& s)
    : prev_(t_task_settings) {
    t_task_settings = &s;
}

ScopedTaskSettings::~ScopedTaskSettings() { t_task_settings = prev_; }

const config::Settings* task_settings_override() { return t_task_settings; }

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Prep: return "prep";
        case Phase::Decode: return "decode";
        case Phase::Wait: return "wait";
        case Phase::Sample: break;
    }
    return "sample";
}

Phase phase_from(const std::string& name) {
    if (name == "prep") return Phase::Prep;
    if (name == "decode") return Phase::Decode;
    if (name == "wait") return Phase::Wait;
    return Phase::Sample;
}

std::string phase_note(Phase p, int step, int steps) {
    const std::string n = std::to_string(step) + "/" + std::to_string(steps);
    switch (p) {
        case Phase::Prep:
            // steps == 0：还没开始采样，正在腾显存 / 装模型。
            return steps == 0 ? SAY("（正在准备模型，可能要先腾出显存）")
                              : SAYF("（准备 %1）", n);
        case Phase::Decode: return SAYF("（解码 %1）", n);
        case Phase::Wait:
            // 不是这一镜的错，也不是停了：说清"在等谁"，人才知道该去
            // 看机器而不是看镜头。
            return step > 0
                       ? SAYF("（工作机都连不上，排着队等它们回来，已等 %1 分钟）",
                              std::to_string(step))
                       : SAY("（工作机都连不上，排着队等它们回来）");
        case Phase::Sample: break;
    }
    return SAYF("（第 %1 步）", n);
}

// **这一段在 #ifdef 外面**：它只看配置，和有没有链上 sd.cpp 无关，
// 而测试目标编的是没链上游那一支。写在 #ifdef 里面就测不到了。
std::string sd_model_problem(const config::Settings& settings, ModelRole role) {
    const auto& m = settings.models;
    if (role == ModelRole::Video) {
        if (m.video.empty()) {
            return SAY("没配视频模型。在 changji.toml 的 [models] 里填 video，"
                       "或者把出片交给推理服务");
        }
        return {};
    }
    if (!m.image.empty()) return {};
    if (!m.video.empty()) {
        // 拿视频模型走 generate_image 会整个进程崩，见头文件里的说明。
        return SAY("没配出图模型（[models].image）。\n"
                   "**不能用视频模型顶替**：sd.cpp 的 generate_image 没有帧数"
                   "参数，拿 Wan 这类视频模型进去会让进程直接崩掉（实测）。\n"
                   "要么在 [models] 里填一个图像模型（比如 "
                   "Qwen-Image-Edit-Q4_K_M.gguf），\n"
                   "要么把出图交给推理服务（[models].engine = \"comfy\"）");
    }
    return SAY("没配出图模型。在 changji.toml 的 [models] 里填 image，"
               "或者把出图交给推理服务");
}

/// 这个角色要哪一份扩散权重。
///
/// **基础那一档留空就退回 Edit 那一份**，也就是 2026-09-15 之前的行为：
/// 定妆和空景照旧拿 Edit 权重做文生图。老配置不会因为这次改动跑不起来，
/// 只是仍然落在 catalog.cpp 那段注释说的"不能看"的退化路径上——体检里
/// 会单独说这一条。
const std::string& diffusion_for(const config::ModelsConfig& m,
                                 ModelRole role) {
    if (role == ModelRole::Video) return m.video;
    if (role == ModelRole::ImageBase && !m.image_base.empty()) {
        return m.image_base;
    }
    return m.image;
}

// 同样在 #ifdef 外面，理由见头文件。
SamplingKnobs sampling_knobs_for(const config::Settings& settings,
                                 ModelRole role) {
    const auto& m = settings.models;
    SamplingKnobs k;
    if (role == ModelRole::Video) {
        k.cfg = m.video_cfg;
        k.flow_shift = m.video_flow_shift;
        // 高噪声那份为空 = 不是双专家，交班阈值没有意义，留 0。
        k.moe_boundary = m.video_high_noise.empty() ? 0.0 : m.video_moe_boundary;
    } else {
        k.cfg = m.image_cfg;
        k.flow_shift = m.image_flow_shift;
    }
    return k;
}

// 同样在 #ifdef 外面，理由见头文件。
float sd_flow_shift(double configured) {
    if (!(configured > 0.0)) {  // 0、负数、NaN 都算"没填"
        return std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(configured);
}

namespace {

/// 预览的落点。**可以同时有好几个**，见 add_preview_sink 上面那段。
std::mutex& preview_mu() {
    static std::mutex m;
    return m;
}
std::map<int, PreviewSink>& preview_sinks() {
    static std::map<int, PreviewSink> m;
    return m;
}

/// cfg 没填就当场抛。
///
/// **不给它一个"兜底默认值"是故意的。** sd.cpp 自己的默认是 7.0，
/// 而 H3 要 1.0、Qwen-Image 要 2.5。拿 7.0 跑 H3 出来的是一团噪点，
/// 而那种错人会先去怀疑提示词、再怀疑首帧，最后才想到旋钮——
/// 这一路正是 2026-09-13 flow_shift 那次走过的。
/// 少填一个字段就当场停，比跑完两分钟拿到一团噪点便宜得多。
[[maybe_unused]] void require_knobs(const SamplingKnobs& k,
                                   const std::string& what) {
    if (!(k.cfg > 0.0)) {
        throw SdError(SAYF("%1的请求没填 cfg。构造请求的地方要调 "
                           "sampling_knobs_for(settings, role) 把 [models] "
                           "里那套旋钮带上——这是调用方的 bug，不是配置问题。",
                           what));
    }
}

}  // namespace

int add_preview_sink(PreviewSink sink) {
    static int next = 1;
    std::lock_guard lg(preview_mu());
    const int token = next++;
    preview_sinks()[token] = std::move(sink);
    return token;
}

void remove_preview_sink(int token) {
    std::lock_guard lg(preview_mu());
    preview_sinks().erase(token);
}

void publish_preview(const std::string& tag, int step, std::string data_url) {
    std::vector<PreviewSink> sinks;
    {
        std::lock_guard lg(preview_mu());
        for (const auto& [token, s] : preview_sinks()) sinks.push_back(s);
    }
    if (sinks.empty() || tag.empty()) return;
    for (const auto& sink : sinks) sink(tag, step, data_url);
}

fs::path audio_path_for(const fs::path& raw_dest) {
    fs::path p = raw_dest;
    p.replace_extension(".wav");
    return p;
}

// 这个也在 #ifdef 外面，理由同 sd_model_problem：纯算术，没它测不到。
CropBox center_crop_box(int src_w, int src_h, int dst_w, int dst_h) {
    CropBox all{0, 0, src_w, src_h};
    // 参数不成样子就别裁——宁可把原图整张递下去，也别在这儿算出一个
    // 负宽度让下游去崩。
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return all;

    // 比较 src_w/src_h 和 dst_w/dst_h，交叉相乘避开浮点。
    const long long lhs = static_cast<long long>(src_w) * dst_h;
    const long long rhs = static_cast<long long>(dst_w) * src_h;
    if (lhs == rhs) return all;  // 比例已经对上，一刀都不用裁

    CropBox box;
    if (lhs > rhs) {
        // 源比目标宽，裁两侧。
        box.h = src_h;
        box.w = static_cast<int>(rhs / dst_h);
    } else {
        // 源比目标高，裁上下。
        box.w = src_w;
        box.h = static_cast<int>(lhs / dst_w);
    }
    // 整除会往下取，取到 0 的话下游拿到一张空图。夹到至少 1 像素。
    box.w = std::max(1, std::min(box.w, src_w));
    box.h = std::max(1, std::min(box.h, src_h));
    box.x = (src_w - box.w) / 2;
    box.y = (src_h - box.h) / 2;
    return box;
}

// 这两个在 #ifdef 外面，理由同 center_crop_box：纯转换，写在里面就测不到。

std::string serialize_measured_vram(const std::map<Slot, Scheduler::Measured>& m,
                                    const std::string& fingerprint) {
    nlohmann::json j = nlohmann::json::object();
    // 量这些数时的那套配置。键名带下划线，和槽名（LLM/图像/视频/配音）
    // 不会撞——解析那边也是按槽名逐个找的，多一个键不影响老逻辑。
    if (!fingerprint.empty()) j["_config"] = fingerprint;
    for (const auto& [slot, v] : m) {
        if (v.bytes == 0) continue;
        // **形状换了：数 -> 对象。** 光记字节数不够——那个数只在"活不比
        // 当时大"的前提下算数，见 Scheduler::record_measured_vram。
        j[to_string(slot)] = nlohmann::json{{"bytes", v.bytes},
                                            {"work", v.work}};
    }
    return j.dump();
}

std::map<Slot, Scheduler::Measured> parse_measured_vram(
    const std::string& text, const std::string& want) {
    std::map<Slot, Scheduler::Measured> out;
    if (text.empty()) return out;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        // 整个文件坏掉就当没量过。**不能瞎猜**：这个数决定要不要卸模型，
        // 猜小了是 OOM。回到保守那条只是慢一点。
        return out;
    }
    if (!j.is_object()) return out;
    // **配置指纹对不上就整份作废。** 见头文件：实测值是"在那套配置下见过的
    // 峰值"，换了预算或画幅它就不再是上限，而 record_measured_vram 只升不降，
    // 自己降不回来。
    //
    // **没有这个键的老文件同样作废。** 第一版特意放过了它们，想护住升级前
    // 攒下的数——结果是那一改**对已经存在的文件完全无效**：新峰值比存着的
    // 低，record_measured_vram 在"没长高"时直接 return、不写文件，于是永远
    // 补不上指纹，那条陈旧值永久留存（实测偏高 28%）。
    //
    // 放过它们换来的是"第一镜不用先腾显存"，而代价是一个永远纠不正的数。
    // 不划算。代码里本来就有"老格式 → 第一镜重新量"这条先例（work == 0
    // 那支），一视同仁。
    if (!want.empty()) {
        const auto cfg = j.find("_config");
        const bool same = cfg != j.end() && cfg->is_string() &&
                          cfg->get<std::string>() == want;
        if (!same) return out;
    }
    const Slot kAll[] = {Slot::LLM, Slot::Image, Slot::Video, Slot::TTS};
    for (const Slot s : kAll) {
        const auto it = j.find(to_string(s));
        if (it == j.end()) continue;
        // **老文件里是个光秃秃的数**，那时候还没记"量的是多大的活"。
        // 读回来 work = 0，调度器见到 0 就知道这个数不能拿来给
        // 指定了大小的那一镜背书——跑一镜自己就补上了。
        if (it->is_number_unsigned()) {
            const auto v = it->get<std::uint64_t>();
            if (v > 0) out[s] = Scheduler::Measured{static_cast<std::size_t>(v), 0};
            continue;
        }
        if (!it->is_object()) continue;
        const auto b = it->find("bytes");
        if (b == it->end() || !b->is_number_unsigned()) continue;
        const auto bv = b->get<std::uint64_t>();
        if (bv == 0) continue;
        std::size_t work = 0;
        if (const auto w = it->find("work");
            w != it->end() && w->is_number_unsigned()) {
            work = static_cast<std::size_t>(w->get<std::uint64_t>());
        }
        out[s] = Scheduler::Measured{static_cast<std::size_t>(bv), work};
    }
    return out;
}

#ifdef CHANGJI_HAVE_SD

namespace {

/// 正在跑的那一次生成。
///
/// sd.cpp 的进度回调是**全局的**（sd_set_progress_callback 不带 ctx），
/// 所以这里只能用一个全局的当前状态。这意味着**同一时刻只能跑一次生成**——
/// 但那本来就是事实：显卡只有一张，并发只会更慢。
struct ActiveGeneration {
    std::mutex mu;
    sd_ctx_t* ctx = nullptr;
    const StepCallback* on_step = nullptr;
    pipeline::CancelToken* tok = nullptr;
    std::atomic<bool> cancel_sent{false};
    /// 这一次生成**要采样几步**。sd.cpp 的进度回调加载权重和采样共用一个，
    /// 靠"报上来的总数是不是等于我们要的步数"把两者分开。
    int want_steps = 0;
    /// 给哪一镜的。预览回调也是全局的，靠它把小图挂到墙上对的那一格。
    std::string tag;
    /// 这一次占的是哪个槽。量到的显存要记到它名下。
    Slot slot = Slot::Image;
    /// 这一次干的活有多大：像素 × 帧数。量到的显存要连它一起记——
    /// 光记字节数的话，在 720p 量到的数会被拿去给 2K 那一镜背书。
    /// 见 Scheduler::record_measured_vram。
    std::size_t work = 0;
    /// 这一轮量过了没有。**一次生成只量一次**：问一次 nvidia-smi 要
    /// 一百毫秒上下，每一步都问的话出图那种几十步的会明显变慢。
    bool sampled = false;
    /// 最后一个采样步报过了没有。报过之后再来的、总数不等于步数的
    /// 回调就是 VAE 解码，不是准备。见 progress_trampoline。
    bool sample_done = false;
    /// 这一次生成期间**整张卡被占掉的峰值**（GB），每次回调都刷一遍，
    /// 生成结束再记给调度器。只在 NVML 那条路可用时这么量（进程内一次
    /// 查询不到一毫秒）；退回 nvidia-smi 那条路时还是老办法量一次。
    ///
    /// 为什么不能只量第一步：H3 权重全放内存、分段跑，第一步时显存只有
    /// 权重的头几段和一小块缓冲（约 10 GB），峰值在后面的段和 VAE 解码
    /// （24～29 GB）。按第一步记的话调度器以为出片只要 10 GB，首帧那 16 GB
    /// 的图像模型就留在卡上不卸，下一镜出片当场"cannot make enough memory"
    /// （2026-09-13 q4_full sh004 实见，三次全败降级）。
    bool measure_alone = false;
    double peak_used_gb = 0.0;
};

ActiveGeneration& active() {
    static ActiveGeneration a;
    return a;
}

std::string base64(const std::vector<unsigned char>& in) {
    static const char* k =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i < in.size()) {
        unsigned v = in[i] << 16;
        if (i + 1 < in.size()) v |= in[i + 1] << 8;
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

/// 采样中途 sd.cpp 给的预览。**只取第一帧**：视频的潜空间投影每帧一张，
/// 而牌子上只放得下一张。编成 PNG 再 base64 交给落点，由它推给界面。
/// 没装落点或者这次生成没带 tag，就什么都不做——一张不要的 PNG 也别编。
void preview_trampoline(int step, int frame_count, sd_image_t* frames,
                        bool /*is_noisy*/, void* /*data*/) {
    if (frame_count <= 0 || frames == nullptr || frames[0].data == nullptr) return;
    std::string tag;
    {
        std::lock_guard lg(active().mu);
        tag = active().tag;
    }
    if (tag.empty()) return;
    {
        std::lock_guard lg(preview_mu());
        if (preview_sinks().empty()) return;   // 没人看就别编码
    }

    const sd_image_t& img = frames[0];
    std::vector<unsigned char> buf;
    const auto append = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* p = static_cast<const unsigned char*>(data);
        out->insert(out->end(), p, p + size);
    };
    if (!::stbi_write_png_to_func(append, &buf, static_cast<int>(img.width),
                                  static_cast<int>(img.height),
                                  static_cast<int>(img.channel), img.data,
                                  static_cast<int>(img.width * img.channel))) {
        return;
    }
    // **编一次，发给所有人。** 编码是这条路上唯一花时间的一步。
    publish_preview(tag, step, "data:image/png;base64," + base64(buf));
}

/// 生成结束把这一轮的峰值记给调度器。失败的那一轮也记：OOM 之前占到
/// 多少正是下次该腾出多少。
void record_peak_vram(bool /*ok*/) {
    ActiveGeneration& a = active();
    double peak = 0.0;
    Slot slot = Slot::Image;
    std::size_t work = 0;
    {
        std::lock_guard lg(a.mu);
        if (!a.measure_alone) return;
        peak = a.peak_used_gb;
        slot = a.slot;
        work = a.work;
    }
    if (peak <= 0.0) return;
    scheduler().record_measured_vram(
        slot, static_cast<std::size_t>(peak * 1024) * 1024 * 1024, work);
}

void progress_trampoline(int step, int steps, float time, void* /*data*/) {
    ActiveGeneration& a = active();
    StepCallback cb;
    sd_ctx_t* ctx = nullptr;
    pipeline::CancelToken* tok = nullptr;
    {
        std::lock_guard lg(a.mu);
        if (a.on_step) cb = *a.on_step;
        ctx = a.ctx;
        tok = a.tok;
    }
    int want = 0;
    {
        std::lock_guard lg(a.mu);
        want = a.want_steps;
    }
    // 总数对不上我们要的步数，就说明这一轮回调不是采样。
    //
    // 采样之前的那些（分段搬权重、首次从磁盘载权重）上层只看得到
    // (step, steps)，分不出是哪一种，所以只叫 Prep，文案不能写死成
    // "加载模型"——写死之后每镜都冒出来，看着像"模型被重载了 22 次"，
    // 而实际整轮只从磁盘载过一次。
    //
    // **采样跑到最后一步之后再来的，就是 VAE 分块解码。** 它和准备
    // 是两回事：VAE 在显存里两三秒、在内存里二十几秒，牌子上写"准备"
    // 用户会以为模型又在重载。靠"最后一个采样步见过没有"分开。
    Phase phase = Phase::Sample;
    {
        std::lock_guard lg(a.mu);
        if (want > 0 && steps != want) {
            phase = a.sample_done ? Phase::Decode : Phase::Prep;
        } else if (step >= steps && steps > 0) {
            a.sample_done = true;
        }
    }
    const bool loading = phase != Phase::Sample;
    if (cb) cb(step, steps, static_cast<double>(time), phase);

    // **在这儿量一次真实占用。**
    //
    // 挑第一个采样步：这时候权重和计算缓冲都已经分配好，正是峰值附近
    // （96 GB 卡上实测，第 1 步就到 75.8 GB，一直保持到最后一步）。
    // 再早量到的是加载中途的数，再晚就错过了。
    //
    // 量它是因为**静态估算靠不住**：同一路 weights="cpu"，我算 14.6 GB、
    // 实测 74 GB。见 Scheduler::record_measured_vram。
    // **每次回调刷一遍峰值**（NVML 可用时）。为什么不只量第一步，见
    // ActiveGeneration::peak_used_gb。量的是"总量 − 空闲"，也就是整张卡
    // 被占掉的，所以只在这个槽是唯一装着的时候才算数（measure_alone 在
    // 生成开始时定，见 generate / generate_video）。
    {
        bool alone = false;
        {
            std::lock_guard lg(a.mu);
            alone = a.measure_alone;
        }
        if (alone) {
            if (const auto t = models::vram_totals_gb(); t.has_value()) {
                const double used_gb = t->total_gb - t->free_gb;
                std::lock_guard lg(a.mu);
                if (used_gb > a.peak_used_gb) a.peak_used_gb = used_gb;
            } else if (!loading && step >= 1) {
                // 没有 NVML：退回老办法，第一个采样步量一次
                bool first = false;
                Slot slot = Slot::Image;
                std::size_t work = 0;
                {
                    std::lock_guard lg(a.mu);
                    if (!a.sampled) {
                        a.sampled = true;
                        first = true;
                        slot = a.slot;
                        work = a.work;
                    }
                }
                if (first) {
                    if (const auto f = models::free_vram_gb(); f.has_value()) {
                        const auto prof = models::HardwareProfile::detect(std::nullopt);
                        const double total_gb = prof.gpu.has_value() ? prof.gpu->vram_gb() : 0.0;
                        const double used_gb = total_gb - *f;
                        if (total_gb > 0.0 && used_gb > 0.0) {
                            scheduler().record_measured_vram(
                                slot,
                                static_cast<std::size_t>(used_gb * 1024) * 1024 * 1024,
                                work);
                        }
                    }
                }
            }
        }
    }

    // 取消是**在这里**发出去的。sd.cpp 的采样循环没有别的插手点，
    // 不在回调里发的话，点了停止要等这一镜跑完——低配机器上那是好几分钟。
    if (tok && tok->cancelled() && ctx &&
        !a.cancel_sent.exchange(true, std::memory_order_relaxed)) {
        ::sd_cancel_generation(ctx, SD_CANCEL_ALL);
    }
}

/// 把 PNG 写到磁盘。
///
/// **不能用 stbi_write_png**：它内部走 fopen(const char*)，Windows 上
/// 那个路径按 ANSI 代码页解释，中文项目名直接写不进去，而且报的错是
/// "写文件失败"，看不出是编码问题。所以先编码到内存再自己落盘。
void write_png(const fs::path& dest, const sd_image_t& img) {
    std::vector<unsigned char> buf;
    const auto sink = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* p = static_cast<const unsigned char*>(data);
        out->insert(out->end(), p, p + size);
    };
    const int ok = ::stbi_write_png_to_func(
        sink, &buf, static_cast<int>(img.width), static_cast<int>(img.height),
        static_cast<int>(img.channel), img.data,
        static_cast<int>(img.width * img.channel));
    if (!ok || buf.empty()) throw SdError(SAY("PNG 编码失败"));

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) throw SdError(SAYF("写不了 %1", paths::to_utf8(dest)));
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    f.close();
    if (!f) throw SdError(SAYF("写 %1 时出错", paths::to_utf8(dest)));
}

/// 读一张参考图。同样不能让 stb 自己开文件，理由同上。
sd_image_t load_image(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw SdError(SAYF("读不了参考图 %1", paths::to_utf8(p)));
    const std::string bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    int w = 0, h = 0, c = 0;
    unsigned char* data = ::stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(bytes.data()),
        static_cast<int>(bytes.size()), &w, &h, &c, 3);
    if (!data) throw SdError(SAYF("参考图解不开：%1", paths::to_utf8(p)));
    sd_image_t img{};
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    img.channel = 3;
    img.data = data;
    return img;
}

/// 按 box 把 img 裁掉，**就地**改，不重新分配。
///
/// 就地是有意的：`img.data` 是 stbi_load 给的，调用方拿 stbi_image_free 去还。
/// 换成自己 malloc 的buffer就把所有权绑在"stbi_image_free 正好是 free"
/// 这个实现细节上了。裁剪只会变小，原地挪得开。
///
/// 挪的方向也安全：目标下标 3*(y*新宽+x) 恒不大于源下标
/// 3*((y+y0)*宽+x+x0)（因为 新宽≤宽、x0≥0、y0≥0），所以从头往后拷不会
/// 覆盖还没读的像素。
void crop_in_place(sd_image_t& img, const CropBox& box) {
    if (box.whole(static_cast<int>(img.width), static_cast<int>(img.height))) {
        return;
    }
    const auto ch = static_cast<std::size_t>(img.channel);
    const auto src_w = static_cast<std::size_t>(img.width);
    for (int y = 0; y < box.h; ++y) {
        const std::size_t dst = static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(box.w) * ch;
        const std::size_t src =
            (static_cast<std::size_t>(y + box.y) * src_w +
             static_cast<std::size_t>(box.x)) * ch;
        std::memmove(img.data + dst, img.data + src,
                     static_cast<std::size_t>(box.w) * ch);
    }
    img.width = static_cast<uint32_t>(box.w);
    img.height = static_cast<uint32_t>(box.h);
}

std::string vram_arg(double gb) {
    if (gb <= 0.0) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", gb);
    return std::string(buf);
}

}  // namespace

struct SdContext::Impl {
    sd_ctx_t* ctx = nullptr;

    /// **一个 sd_ctx 同时只许一件活在跑。**
    ///
    /// 2026-09-11 这条把服务干掉过一次：设定页上连点三个「重画」，三条线程
    /// 同时进 sd.cpp，撞在
    ///     conditioning/conditioner.hpp:1980: GGML_ASSERT(!hidden_states.empty())
    /// 上——abort()，**不是异常，兜不住**，在跑的活全没了。
    ///
    /// sd_ctx 里面那一堆（conditioner 的中间缓冲、ggml 的计算图和分配器）
    /// 是按"一次一件"写的，两条线程进去就是互相踩。
    ///
    /// **调度器挡不住这个。** 它管的是"哪个模型在显存里"，同一个槽是可以
    /// 被借好几次的（多个租约共用一份已加载的权重，大模型那边正是靠这个
    /// 并行跑好几路）。所以"一次一件"这件事只能由用它的人自己管——而这个
    /// 上下文是共享的，那就该它自己管，谁调都不会漏。
    ///
    /// 代价是第二件活在这儿干等（出一张图几十秒）。那是对的：它本来也
    /// 抢不到卡，等在这儿至少界面上那个百分比停在 0，而不是整个服务没了。
    std::mutex run_mu;
    // 路径要活到 sd_ctx 建完：sd_ctx_params_t 存的是 const char*，
    // 不拷贝。传临时 string 的 c_str() 的话，new_sd_ctx 读到的是野指针。
    std::string diffusion, vae, text_encoder, max_vram, params_backend;
    /// 双专家模型的高噪声那一份（Wan 2.2 A14B）。空 = 单模型。
    /// 和上面几个一样，路径要活到 sd_ctx 建完。
    std::string high_noise;
    /// 音频 VAE（MiniMax-H3 那类音画一起出的模型）。
    std::string audio_vae;
    /// 出片挂的 LoRA。sd_lora_t 存的是 const char*，不拷贝，所以路径
    /// 要活到 generate_video 调完，不能用临时 string 的 c_str()。
    std::string lora;
    float lora_strength = 1.0f;
    /// VAE 分块大小的覆盖值，0 = 用请求里带的默认。
    int vae_tile = 0;
    /// Qwen-Image 那一路的文本编码器（sd.cpp 的 llm_path）和它的视觉塔。
    /// **和 text_encoder 互斥**：一次只填其中一边——Wan 走 t5xxl，
    /// Qwen-Image 走 llm，两个参数位不是一回事。
    std::string llm, llm_vision;
    // **采样旋钮不在这儿了。** cfg / flow_shift / moe_boundary 以前是建
    // 上下文时从全局 [models] 冻下来的，于是项目自己的 changji.toml 不
    // 参与、改了配置也要等上下文被卸载才生效——两条都不报错。现在跟着
    // 每次请求走，见 SamplingKnobs。

    ~Impl() {
        if (ctx) ::free_sd_ctx(ctx);
    }
};

std::shared_ptr<SdContext> SdContext::create(const config::Settings& settings,
                                             double vram_budget_gb,
                                             ModelRole role) {
    const fs::path ws = settings.workspace_path();
    const auto& m = settings.models;
    if (const std::string why = sd_model_problem(settings, role); !why.empty()) {
        throw SdError(why);
    }

    auto self = std::shared_ptr<SdContext>(new SdContext());
    self->impl_ = std::make_unique<Impl>();
    Impl& impl = *self->impl_;

    // 出视频用视频模型，出首帧用图像模型。
    //
    // **原来这里写着"没配就退回视频模型出单帧"，那条退路是坏的**，
    // 上面那个 throw 里记了为什么（sd.cpp 的 generate_image 没有帧数参数，
    // 拿视频模型进去整个进程崩）。走到这里时 image 一定非空。
    const bool is_video = role == ModelRole::Video;
    const std::string& which = diffusion_for(m, role);
    impl.diffusion = paths::to_utf8(m.resolve(which, ws));
    // 双专家的高噪声那一份。只有视频那条路有——Qwen-Image 不是 MoE。
    if (is_video && !m.video_high_noise.empty()) {
        impl.high_noise = paths::to_utf8(m.resolve(m.video_high_noise, ws));
    }

    // **VAE 和文本编码器要按角色挑，不能两边共用一套。**
    // 之前这里写死了 video_vae + video_text_encoder，图像那条路也拿它俩用
    // ——因为图像那条路从来没真跑过，没人发现。
    // Wan 的 VAE 和 Qwen-Image 的不是一回事；UMT5-XXL 和 Qwen2.5-VL 更不是，
    // 而且在 sd.cpp 里根本不是同一个参数位（t5xxl_path vs llm_path，
    // 见上游 docs/qwen_image_edit.md）。
    // **喂错了不报错**：照常加载，然后出一张和提示词没关系的图。
    // 图像那几项留空就退回视频那套，只用 Wan 的人不必填两遍。
    const std::string& vae_key =
        (!is_video && !m.image_vae.empty()) ? m.image_vae : m.video_vae;
    impl.vae = vae_key.empty() ? "" : paths::to_utf8(m.resolve(vae_key, ws));

    // 出片的 LoRA（Turbo 那类蒸馏适配器）。
    if (is_video && !m.video_lora.empty()) {
        const auto p = m.resolve(m.video_lora, ws);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            // **先核张量名。** H3 的 Turbo LoRA 是裸名，sd.cpp 对不上会悄悄
            // 不挂——挂着和摘掉出的片字节相同，跑了四天才发现。裸名就用
            // 旁边那份加了前缀的副本，见 infer/lora_names.hpp。核不了
            // （文件坏了、目录只读）就照原来的传，让 sd.cpp 自己报。
            fs::path use = p;
            try {
                use = ensure_sdcpp_lora_names(p);
            } catch (const std::exception& e) {
                // 这一族 `[出片]` / `[出图]` / `[vram]` 走 stderr，是日志不是
                // 界面——桌面端没有日志格，它们只落在控制台。所以不包 `SAY()`，
                // 和 `server.cpp` 里那两条 `CROW_LOG_*` 一个道理。
                std::fprintf(stderr, SAY_NEVER("[出片] 核 LoRA 张量名失败（%s），照原文件传\n"),
                             e.what());
            }
            impl.lora = paths::to_utf8(use);
            impl.lora_strength = static_cast<float>(m.video_lora_strength);
        } else {
            // **默认就指着 Turbo 那份，所以"文件不在"是常态，不是错误**
            // ——没下过 LoRA 的机器照样能出片，只是慢四倍。
            // 但要说一声：不说的话用户以为 Turbo 在跑，
            // 而实际每镜多花两分钟，他只会觉得"这机器怎么这么慢"。
            std::fprintf(
                stderr,
                SAY_NEVER("[出片] 没找到加速 LoRA（%s），这一轮不挂它。"
                          "出片会慢几倍。下载见 tools/fetch_models.sh\n"),
                paths::to_utf8(p).c_str());
        }
    }
    if (is_video && m.video_vae_tile > 0) impl.vae_tile = m.video_vae_tile;

    // 音频 VAE：MiniMax-H3 那类画面和声音一起出的模型才有。
    if (is_video && !m.video_audio_vae.empty()) {
        impl.audio_vae = paths::to_utf8(m.resolve(m.video_audio_vae, ws));
    }

    if (is_video && !m.video_llm.empty()) {
        // **视频模型也可能用 llm_path 而不是 t5xxl_path。**
        // Wan 那一路是 UMT5-XXL → t5xxl；MiniMax-H3 用裁过的 Qwen3-VL-32B
        // → llm。校验那边保证这两项不会同时填。
        impl.llm = paths::to_utf8(m.resolve(m.video_llm, ws));
        if (!m.video_llm_vision.empty()) {
            impl.llm_vision = paths::to_utf8(m.resolve(m.video_llm_vision, ws));
        }
    } else if (!is_video && !m.image_text_encoder.empty()) {
        impl.llm = paths::to_utf8(m.resolve(m.image_text_encoder, ws));
        // **视觉塔只给 Edit 那一路挂。** 它的用处是让编码器"看见"参考图
        // （2509 起要它，不挂的话 sd.cpp 只在日志里说一句 vision disabled
        // 就照常出图）。基础那一路一张参考图都不传，挂上去没有输入可看，
        // 白占一份显存。
        if (role != ModelRole::ImageBase &&
            !m.image_text_encoder_vision.empty()) {
            impl.llm_vision =
                paths::to_utf8(m.resolve(m.image_text_encoder_vision, ws));
        }
    } else {
        impl.text_encoder = m.video_text_encoder.empty()
                                ? ""
                                : paths::to_utf8(
                                      m.resolve(m.video_text_encoder, ws));
    }
    impl.max_vram = vram_arg(vram_budget_gb);
    // 权重放哪，见 ModelsConfig::weights。
    //
    // cpu：权重放系统内存，用到才搬进显存。**这是 6GB 卡上能跑的关键**——
    // 见方案里"跨模型调度"那一节。不管的话（权重直接进显存）在那台机器上
    // 加载阶段就 OOM。
    //
    // auto：params_backend 留空 + auto_fit。sd.cpp 只在两个后端 spec 都为空时
    // 才启用 auto_fit（stable-diffusion.cpp 里那一行 `&& params_backend_spec.empty()`），
    // 所以这里**不能**填 "" 之外的任何东西。它按这张卡真实的空闲显存逐组件放。
    // auto 时留空（sd.cpp 只在两个 spec 都为空时才启用 auto_fit）；
    // cpu 就是 "cpu"；别的原样传下去当组件规格。
    // **图像和视频各用各的。** `weights` 是为视频模型定的（18 GB 只能放内存），
    // 图像模型跟着放内存就是每一步从内存搬 7 GB 权重——见
    // ModelsConfig::image_weights 上面那组数字。
    const std::string& w = is_video ? m.weights : m.image_weights;
    const bool auto_fit = w == "auto";
    // gpu：一个组件都不指定后端，全在默认后端（也就是显卡）上跑。
    // 传给 sd.cpp 的同样是空串，但 **auto_fit 是假**——它只在
    // `auto_fit && 两个 spec 都空` 时才启用，所以这两条不会打架。
    // 这个取值是给统一内存的机器准备的，见 ModelsConfig::weights_for。
    impl.params_backend = (auto_fit || w == "gpu") ? "" : w;

    sd_ctx_params_t p{};
    ::sd_ctx_params_init(&p);
    p.auto_fit = auto_fit;
    p.diffusion_model_path = impl.diffusion.c_str();
    if (!impl.high_noise.empty()) {
        p.high_noise_diffusion_model_path = impl.high_noise.c_str();
    }
    if (!impl.vae.empty()) p.vae_path = impl.vae.c_str();
    if (!impl.audio_vae.empty()) p.audio_vae_path = impl.audio_vae.c_str();
    // 随机数发生器。**sd.cpp 的默认是 cuda**，而上游给 MiniMax-H3 的命令行
    // 是 --rng cpu。发生器不同则初始噪声不同，同一个种子出的画面就不一样，
    // 而且没有任何报错。只在出片这条路上认这一项。
    // effective_*：读设置那两条入口已经把 auto 落成具体值，这里再问一遍
    // 是给没经过它们的 Settings（单元测试、直接构造的）兜底。
    const std::string rng = m.effective_video_rng();
    if (is_video && rng != "cuda") {
        p.rng_type = ::str_to_rng_type(rng.c_str());
    }
    if (!impl.text_encoder.empty()) {
        // **是 t5xxl_path，不是 embeddings_connectors_path。**
        //
        // 原来写的是后者。sd.cpp 里那是另一条加载路径（日志写的是
        // "loading embeddings connectors from ..."），而且**加载失败只 warn
        // 不报错**——于是 umt5-xxl 被静默忽略、t5xxl_path 是空的，
        // Wan 拿不到任何文本条件。症状会是"出的片和提示词没关系"，
        // 而日志里只有一行 warn，指不到这儿。
        //
        // 依据是 sd.cpp 自己的 docs/wan.md，那上面的命令行是：
        //   --t5xxl ...\models\text_encoders\umt5-xxl-encoder-Q8_0.gguf
        p.t5xxl_path = impl.text_encoder.c_str();
    }
    // 上游 docs/wan.md 给 Wan 的命令行带着 --diffusion-fa，而
    if (!impl.llm.empty()) p.llm_path = impl.llm.c_str();
    if (!impl.llm_vision.empty()) p.llm_vision_path = impl.llm_vision.c_str();

    // sd_ctx_params_init 的默认是 false。6 GB 卡上这一项直接影响塞不塞得下。
    p.diffusion_flash_attn = m.diffusion_flash_attn;
    // Qwen-Image-Edit 2511 要这一项，上游 docs/qwen_image_edit.md 原话：
    // "must be enabled; otherwise, image editing quality will degrade
    // significantly"。按文件名认，别让人记着填。
    if (!is_video && which.find("2511") != std::string::npos &&
        config::ModelsConfig::accepts_reference_images(which)) {
        p.model_args = "qwen_image_zero_cond_t=true";
    }
    p.max_vram = impl.max_vram.c_str();
    p.params_backend = impl.params_backend.c_str();
    p.n_threads = -1;   // -1 = 物理核数

    impl.ctx = ::new_sd_ctx(&p);
    if (impl.ctx == nullptr) {
        throw SdError(
            SAYF("出图模型加载失败：%1\n看一眼上面 sd.cpp 打的日志，"
                 "多半是文件不对或者显存不够",
                 impl.diffusion));
    }
    return self;
}

SdContext::~SdContext() = default;

void SdContext::generate(const ImageRequest& req, const fs::path& dest,
                         pipeline::CancelToken& tok,
                         const StepCallback& on_step) {
    // 见 Impl::run_mu。**锁在最外面**：中间那些 sd.cpp 调用没有一处是
    // 可重入的，只锁一段等于没锁。
    std::lock_guard<std::mutex> only_one(impl_->run_mu);
    // 取消要在拿到锁之后再查一次：排在前面那件跑了几十秒，这期间用户
    // 完全可能已经点了停止。
    if (tok.cancelled()) throw SdError(SAY("已取消"));

    std::vector<sd_image_t> refs;
    // **基础版文生图模型不收参考图。** sd.cpp 看到 ref_images 就走 EDIT
    // mode，基础模型出来的是参考图的翻版，然后出片那边拿到一张和提示词
    // 矛盾的首帧只能硬切。来历见 ModelsConfig::accepts_reference_images。
    const bool take_refs =
        config::ModelsConfig::accepts_reference_images(impl_->diffusion);
    if (!take_refs && !req.reference_images.empty()) {
        static bool said = false;   // 每镜都喊一遍是噪声，说一次够了
        if (!said) {
            said = true;
            std::fprintf(
                stderr,
                SAY_NEVER("[出图] %s 是文生图模型，这一轮不传参考图：sd.cpp "
                          "会把参考图当编辑源，出来的是它的翻版。角色一致性靠"
                          "文字描述；要用参考图就把 [models].image 换成 "
                          "Qwen-Image-Edit\n"),
                impl_->diffusion.c_str());
        }
    }
    // **反过来那一头也要说：编辑模型手上一张参考图都没有。**
    //
    // 首帧那一族 2026-09-15 整族换成了 Qwen-Image-Edit 2509，于是这条路
    // 从"几乎碰不上"变成了默认路径上的一种：这一镜在场的角色还没定妆
    // 三视图、这个场景也没有空景图，`prompt_compose` 就一张 ref 都推不出来
    // （那两处都是 `has_value() && !empty()` 才推），Edit 手上没有编辑源，
    // 退化成文生图。
    //
    // **而那个东西不能看，还骗得过闸门。** 设计文档里那一段记着实测：
    // 「39 GB bf16 编辑模型出 22 帧要 4 分钟以上、出来是雪花」；
    // tools/fetch_models.sh 那条记着为什么闸门拦不住——「方差比真图还大，
    // 靠"方差不为零"判"不是空图"会一路绿灯」。
    //
    // 所以这一句非说不可。说一次就够，理由同上面那条。
    if (take_refs && req.reference_images.empty()) {
        static bool said_bare = false;
        if (!said_bare) {
            said_bare = true;
            std::fprintf(
                stderr,
                SAY_NEVER("[出图] %s 是图像编辑模型，而这一镜一张参考图都没有："
                          "没有编辑源它会退化成文生图，出来多半是彩色噪点，"
                          "而且闸门那条「不是空图」的判据拦不住它。先去设定页"
                          "把角色定妆、给场景出空景图（「照故事定妆」+"
                          "「一键出图」），参考图有了这一族才是它该干的活\n"),
                impl_->diffusion.c_str());
        }
    }
    if (take_refs) {
        for (const auto& p : req.reference_images) refs.push_back(load_image(p));
    }
    // 上面任何一张读失败都会抛，此时前面几张的内存还没释放。
    // 用一个哨兵在退出时收拾。
    struct RefGuard {
        std::vector<sd_image_t>& v;
        ~RefGuard() {
            for (auto& i : v) ::stbi_image_free(i.data);
        }
    } guard{refs};

    sd_img_gen_params_t g{};
    ::sd_img_gen_params_init(&g);
    g.prompt = req.positive.c_str();
    g.negative_prompt = req.negative.c_str();
    g.width = req.width;
    g.height = req.height;
    g.seed = req.seed;
    g.batch_count = 1;
    g.sample_params.sample_steps = req.steps;
    // cfg / flow_shift 跟着请求走，不是建上下文时冻下来的那一份。
    // 为什么，见 SamplingKnobs。
    require_knobs(req.knobs, SAY("出图"));
    g.sample_params.guidance.txt_cfg = static_cast<float>(req.knobs.cfg);
    g.sample_params.flow_shift = sd_flow_shift(req.knobs.flow_shift);
    // VAE 分块解码——和 generate_video 那边同一套写法。不填的话 sd.cpp
    // 整图解码，1280×704 要 6.6 GB 缓冲，fp8 常驻的 32 GB 卡上出不来图。
    g.vae_tiling_params.enabled = req.vae_tiling;
    g.vae_tiling_params.tile_size_x = req.vae_tile_x;
    g.vae_tiling_params.tile_size_y = req.vae_tile_y;
    g.vae_tiling_params.target_overlap = static_cast<float>(req.vae_tile_overlap);
    g.vae_tiling_params.rel_size_x = 0.0f;
    g.vae_tiling_params.rel_size_y = 0.0f;
    if (!refs.empty()) {
        g.ref_images = refs.data();
        g.ref_images_count = static_cast<int>(refs.size());
    }

    ActiveGeneration& a = active();
    {
        std::lock_guard lg(a.mu);
        a.ctx = impl_->ctx;
        a.on_step = &on_step;
        a.tok = &tok;
        a.want_steps = req.steps;
        a.tag = req.tag;
        a.slot = Slot::Image;
        a.work = static_cast<std::size_t>(req.width) * req.height;
        a.sampled = false;   // 每次生成重新量一遍，见 progress_trampoline
        a.sample_done = false;
        a.peak_used_gb = 0.0;
        // 只在这个槽是唯一装着的时候量，理由见 peak_used_gb。
        const auto loaded = scheduler().loaded_slots();
        a.measure_alone = loaded.size() == 1 && loaded.front() == Slot::Image;
    }
    a.cancel_sent.store(false, std::memory_order_relaxed);
    ::sd_set_progress_callback(progress_trampoline, nullptr);
    // PREVIEW_PROJ：潜空间线性投影成 RGB，不走 VAE，每步一张几乎不花时间。
    // denoised=true 要的是"预测出来的干净图"，那才是逐渐成形的那个；
    // 带噪那份对人没有意义。间隔 1 = 每步都给。
    ::sd_set_preview_callback(preview_trampoline, PREVIEW_PROJ, 1, true, false,
                              nullptr);

    sd_image_t* out = nullptr;
    int count = 0;
    const bool ok = ::generate_image(impl_->ctx, &g, &out, &count);
    record_peak_vram(ok);

    {
        std::lock_guard lg(a.mu);
        a.ctx = nullptr;
        a.on_step = nullptr;
        a.tok = nullptr;
        a.want_steps = 0;
        a.tag.clear();
    }
    ::sd_set_preview_callback(nullptr, PREVIEW_NONE, 0, false, false, nullptr);

    if (tok.cancelled()) {
        if (out) ::free_sd_images(out, count);
        throw SdError(SAY("已取消"));
    }
    if (!ok || out == nullptr || count <= 0) {
        if (out) ::free_sd_images(out, count);
        throw SdError(SAY("出图失败，看一眼上面 sd.cpp 打的日志"));
    }

    try {
        write_png(dest, out[0]);
    } catch (...) {
        ::free_sd_images(out, count);
        throw;
    }
    ::free_sd_images(out, count);
}

void SdContext::generate_video(const VideoRequest& req, const fs::path& raw_dest,
                               pipeline::CancelToken& tok,
                               const StepCallback& on_step) {
    std::lock_guard<std::mutex> only_one(impl_->run_mu);   // 见 Impl::run_mu
    if (tok.cancelled()) throw SdError(SAY("已取消"));
    if (!::sd_ctx_supports_video_generation(impl_->ctx)) {
        throw SdError(SAY("这个模型不支持出视频。[models].video 要填一个视频"
                          "模型（比如 Wan），填成图像模型是不行的"));
    }

    sd_image_t start{};
    bool has_start = false;
    if (req.start_image.has_value()) {
        start = load_image(*req.start_image);
        has_start = true;
        crop_in_place(start, center_crop_box(static_cast<int>(start.width),
                                             static_cast<int>(start.height),
                                             req.width, req.height));
    }
    struct StartGuard {
        sd_image_t& img;
        bool& has;
        ~StartGuard() {
            if (has) ::stbi_image_free(img.data);
        }
    } sguard{start, has_start};

    // 尾帧同首帧：裁到画幅。**只有首帧时才认尾帧**——H3 的 FL2VA 权重
    // 把两张都当条件，没有首帧只给尾帧不是它训练过的用法。
    sd_image_t end{};
    bool has_end = false;
    if (has_start && req.end_image.has_value()) {
        end = load_image(*req.end_image);
        has_end = true;
        crop_in_place(end, center_crop_box(static_cast<int>(end.width),
                                           static_cast<int>(end.height),
                                           req.width, req.height));
    }
    StartGuard eguard{end, has_end};

    sd_vid_gen_params_t g{};
    ::sd_vid_gen_params_init(&g);
    g.prompt = req.positive.c_str();
    g.negative_prompt = req.negative.c_str();
    g.width = req.width;
    g.height = req.height;
    g.video_frames = req.frames;
    g.fps = req.fps;
    g.seed = req.seed;
    g.sample_params.sample_steps = req.steps;
    // cfg / flow_shift 跟着请求走，不是建上下文时冻下来的那一份。
    // 为什么，见 SamplingKnobs。
    require_knobs(req.knobs, SAY("出片"));
    g.sample_params.guidance.txt_cfg = static_cast<float>(req.knobs.cfg);
    g.sample_params.flow_shift = sd_flow_shift(req.knobs.flow_shift);
    // **高噪声专家的旋钮要单独填一遍。** sd_vid_gen_params_init 给它的是
    // 另一套默认值（cfg 7.0、flow_shift 无穷），不填的话前几步会在一个
    // 和低噪声那份完全不同的 cfg 上跑——而这**不会报错**，只是出来的片
    // 前后不搭。上游 docs/wan.md 的 A14B 命令行两边给的就是同一个 cfg。
    //
    // 步数保持 init 给的 -1：那是"按 moe_boundary 自动分"的意思
    // （sd.cpp 扫 sigma 序列，第一个小于阈值的下标就是交班点）。
    // 填成具体数字的话两段步数是**相加**的，总步数会翻倍。
    g.high_noise_sample_params.guidance.txt_cfg =
        static_cast<float>(req.knobs.cfg);
    g.high_noise_sample_params.flow_shift = sd_flow_shift(req.knobs.flow_shift);
    // 0 = 没填，沿用 sd_vid_gen_params_init 给的 0.875。单专家时本来
    // 就没有意义（sampling_knobs_for 在那种情况下留 0）。
    if (req.knobs.moe_boundary > 0.0) {
        g.moe_boundary = static_cast<float>(req.knobs.moe_boundary);
    }

    // LoRA。**这个数组要活到 generate_video 返回**——sd_vid_gen_params_t
    // 存的是指针，不拷贝。放在这一层的局部变量里正好（下面就调用了）。
    sd_lora_t lora{};
    if (!impl_->lora.empty() && req.use_lora) {
        lora.path = impl_->lora.c_str();
        lora.multiplier = impl_->lora_strength;
        // H3 不是混合专家，高噪声那份不存在；A14B 挂 LoRA 的话这里要分两条。
        lora.is_high_noise = false;
        g.loras = &lora;
        g.lora_count = 1;
    }

    if (has_start) g.init_image = start;
    if (has_end) g.end_image = end;

    // VAE 分块。**不设的话默认是关的**，而关着在 6GB 卡上解码要 11.7GB，
    // 直接失败。见 VideoRequest 里那张实测表。
    g.vae_tiling_params.enabled = req.vae_tiling;
    g.vae_tiling_params.temporal_tiling = req.vae_temporal_tiling;
    // 配置里给了就盖掉请求带的默认：调小分块换显存，好让 VAE 权重常驻。
    g.vae_tiling_params.tile_size_x =
        impl_->vae_tile > 0 ? impl_->vae_tile : req.vae_tile_x;
    g.vae_tiling_params.tile_size_y =
        impl_->vae_tile > 0 ? impl_->vae_tile : req.vae_tile_y;
    g.vae_tiling_params.target_overlap = static_cast<float>(req.vae_tile_overlap);
    // 显式清零：rel_size 非零时 sd.cpp 优先用比例、忽略上面的绝对块大小
    // （见 vae.hpp 的 get_tile_size）。依赖 init 把它清成 0 的话，
    // 上游哪天改了默认值，这里会静默地换成另一套分块策略。
    g.vae_tiling_params.rel_size_x = 0.0f;
    g.vae_tiling_params.rel_size_y = 0.0f;

    ActiveGeneration& a = active();
    {
        std::lock_guard lg(a.mu);
        a.ctx = impl_->ctx;
        a.on_step = &on_step;
        a.tok = &tok;
        a.want_steps = req.steps;
        a.tag = req.tag;
        a.slot = Slot::Video;
        a.work = static_cast<std::size_t>(req.width) * req.height *
                 std::max(1, req.frames);
        a.sampled = false;
        a.sample_done = false;
        a.peak_used_gb = 0.0;
        const auto loaded = scheduler().loaded_slots();
        a.measure_alone = loaded.size() == 1 && loaded.front() == Slot::Video;
    }
    a.cancel_sent.store(false, std::memory_order_relaxed);
    ::sd_set_progress_callback(progress_trampoline, nullptr);
    // PREVIEW_PROJ：潜空间线性投影成 RGB，不走 VAE，每步一张几乎不花时间。
    // denoised=true 要的是"预测出来的干净图"，那才是逐渐成形的那个；
    // 带噪那份对人没有意义。间隔 1 = 每步都给。
    ::sd_set_preview_callback(preview_trampoline, PREVIEW_PROJ, 1, true, false,
                              nullptr);

    sd_image_t* frames = nullptr;
    int count = 0;
    sd_audio_t* audio = nullptr;
    const bool ok = ::generate_video(impl_->ctx, &g, &frames, &count, &audio);
    record_peak_vram(ok);

    {
        std::lock_guard lg(a.mu);
        a.ctx = nullptr;
        a.on_step = nullptr;
        a.tok = nullptr;
        a.want_steps = 0;
        a.tag.clear();
    }
    ::sd_set_preview_callback(nullptr, PREVIEW_NONE, 0, false, false, nullptr);

    struct FrameGuard {
        sd_image_t*& f;
        int& n;
        ~FrameGuard() {
            if (f) ::free_sd_images(f, n);
        }
    } fguard{frames, count};
    // 声音那份也要还。2026-09-13 记过：以前这里从不 free，每镜约 1.3 MB。
    struct AudioGuard {
        sd_audio_t*& a;
        ~AudioGuard() {
            if (a) ::free_sd_audio(a);
        }
    } aguard{audio};

    if (tok.cancelled()) throw SdError(SAY("已取消"));
    if (!ok || frames == nullptr || count <= 0) {
        throw SdError(SAY("出视频失败，看一眼上面 sd.cpp 打的日志"));
    }

    // 裸 RGB24 顺序写出去。**逐帧写不是一次性拼成一个大 buffer**：
    // 121 帧 448x768 是 125 MB，先攒在内存里等于在出视频最吃内存的
    // 那一刻再要 125 MB。
    std::error_code ec;
    fs::create_directories(raw_dest.parent_path(), ec);
    std::ofstream f(raw_dest, std::ios::binary | std::ios::trunc);
    if (!f) throw SdError(SAYF("写不了 %1", paths::to_utf8(raw_dest)));
    for (int i = 0; i < count; ++i) {
        const sd_image_t& img = frames[i];
        if (img.channel != 3) {
            throw SdError(SAYF("sd.cpp 吐的帧不是 RGB24（channel=%1），"
                               "编码参数对不上",
                               std::to_string(img.channel)));
        }
        const std::size_t n =
            static_cast<std::size_t>(img.width) * img.height * img.channel;
        f.write(reinterpret_cast<const char*>(img.data),
                static_cast<std::streamsize>(n));
        if (!f) throw SdError(SAY("写裸帧时出错，多半是磁盘满了"));
    }
    f.close();
    if (!f) throw SdError(SAYF("写 %1 时出错", paths::to_utf8(raw_dest)));

    // ---- 声音 ----
    //
    // 模型出了就写成 16 位 PCM 的 wav 放在裸帧旁边（sd.cpp 给的是交错的
    // float，和它自己 CLI 的 write_wav_to_file 同一套换算）。**没出就不写**：
    // 上层按文件在不在判，别写一个空 wav 让 ffmpeg 去猜。
    const fs::path wav = audio_path_for(raw_dest);
    fs::remove(wav, ec);
    if (audio != nullptr && audio->data != nullptr && audio->sample_count > 0 &&
        audio->channels > 0 && audio->sample_rate > 0) {
        const std::uint64_t n = audio->sample_count * audio->channels;
        const std::uint32_t data_bytes = static_cast<std::uint32_t>(n * 2);
        std::ofstream w(wav, std::ios::binary | std::ios::trunc);
        if (!w) throw SdError(SAYF("写不了 %1", paths::to_utf8(wav)));
        const auto u32 = [&w](std::uint32_t v) {
            const unsigned char b[4] = {
                static_cast<unsigned char>(v & 0xFF),
                static_cast<unsigned char>((v >> 8) & 0xFF),
                static_cast<unsigned char>((v >> 16) & 0xFF),
                static_cast<unsigned char>((v >> 24) & 0xFF)};
            w.write(reinterpret_cast<const char*>(b), 4);
        };
        const auto u16 = [&w](std::uint16_t v) {
            const unsigned char b[2] = {
                static_cast<unsigned char>(v & 0xFF),
                static_cast<unsigned char>((v >> 8) & 0xFF)};
            w.write(reinterpret_cast<const char*>(b), 2);
        };
        const std::uint16_t ch = static_cast<std::uint16_t>(audio->channels);
        w.write("RIFF", 4);
        u32(36 + data_bytes);
        w.write("WAVE", 4);
        w.write("fmt ", 4);
        u32(16);
        u16(1);  // PCM
        u16(ch);
        u32(audio->sample_rate);
        u32(audio->sample_rate * ch * 2);
        u16(static_cast<std::uint16_t>(ch * 2));
        u16(16);
        w.write("data", 4);
        u32(data_bytes);
        std::vector<std::int16_t> pcm(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            const float s = std::max(-1.0f, std::min(1.0f, audio->data[i]));
            pcm[i] = static_cast<std::int16_t>(std::lrint(s * 32767.0f));
        }
        w.write(reinterpret_cast<const char*>(pcm.data()),
                static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
        w.close();
        if (!w) throw SdError(SAYF("写 %1 时出错", paths::to_utf8(wav)));
    }
}

#else   // 没链 sd.cpp

struct SdContext::Impl {};

std::shared_ptr<SdContext> SdContext::create(const config::Settings&, double,
                                             ModelRole) {
    throw SdError(SAY("这个二进制没有编进出图后端。出图请走推理服务，"
                      "或者用带 sd.cpp 的构建"));
}
SdContext::~SdContext() = default;
void SdContext::generate(const ImageRequest&, const fs::path&,
                         pipeline::CancelToken&, const StepCallback&) {
    throw SdError(SAY("这个二进制没有编进出图后端"));
}
void SdContext::generate_video(const VideoRequest&, const fs::path&,
                               pipeline::CancelToken&, const StepCallback&) {
    throw SdError(SAY("这个二进制没有编进出图后端"));
}

#endif

// ---- 挂到调度器上 ----
//
// 这一段两种构建都要有：没链 sd.cpp 时槽照样注册，
// acquire 的时候 create() 抛异常，调度器会把账退干净。
// 不注册的话调用方拿到的是"槽还没注册"，那句话对用户没意义。

namespace {

std::mutex g_ctx_mu;
std::shared_ptr<SdContext> g_image_ctx;
std::shared_ptr<SdContext> g_video_ctx;

/// 图像槽这一刻**要装**哪个变体，和**已经装着**哪个。
///
/// 两个图像模型（Edit 和基础）各 20 GB 上下，同一张卡上不该并存——而且
/// 它们也从不同时用：先把三视图和空景图铺开（基础），再一镜一镜出首帧
/// （Edit）。所以共用 `Slot::Image` 这一个槽，由调度器换进换出，而不是
/// 再开一个槽把显存预算劈成两半。
///
/// `spec.load` 是注册时定下的闭包、一个进程只跑这一份，所以要装哪一个
/// 得从外面递进去：`acquire_image()` 先把 want 摆好，必要时把装错的那个
/// 卸掉，再去借槽。
std::atomic<ModelRole> g_image_want{ModelRole::Image};
std::atomic<bool> g_image_loaded_valid{false};
std::atomic<ModelRole> g_image_loaded{ModelRole::Image};

}  // namespace

void register_sd_slots(const config::Settings& settings,
                       const models::HardwareProfile& profile) {
    register_sd_slots([settings] { return settings; }, profile);
}

void register_sd_slots(SettingsProvider raw_provider,
                       const models::HardwareProfile& profile) {
    // **weights = "smart" 在这里展开成 sd.cpp 认的组件规格。**
    // 只有这一层拿得到这张卡真实的显存（profile），而 SdContext::create
    // 只看 Settings。包一层 provider，下面所有地方看到的都是展开后的值——
    // 包括 budget_for 和 set_budget，它们都按 weights 的取值分支。
    const double card_gb = profile.gpu.has_value() ? profile.gpu->vram_gb()
                                                   : profile.vram_gb;
    // 统一内存的机器上 smart 展开成另一套（"放内存"在那里省不出地方）。
    // 见 ModelsConfig::weights_for。
    const bool unified = profile.gpu.has_value() && profile.gpu->unified();
    const SettingsProvider provider = [raw_provider, card_gb, unified] {
        // 两个模型放哪都按文件大小算。视频那份 smart 以前只看卡，
        // 换了卡还要人去改 cpu——用户的原话："都应该让程序自己算。"
        // 图像那份还要看它**有多大**：fp8 的 Qwen-Image 20 GB、Q6_K 16 GB、
        // Q4 12 GB，同一张 32 GB 的卡，前者常驻不下、后两者可以。
        //
        // **展开逻辑不写在这儿**：设置页也要拿同一份结果显示给用户看，
        // 各写一遍就会分叉。见 config::expand_placement。
        // 正在接活的线程上有派活方那份（盖了档位的）就用它，见 ScopedTaskSettings。
        const config::Settings* task = task_settings_override();
        return config::expand_placement(task ? *task : raw_provider(), card_gb,
                                        unified);
    };
    // 预算取探测到的显存，留一成给驱动上下文和别的程序。
    //
    // 估高了是 OOM 直接崩，估低了只是多分段（慢）。所以往低了取——
    // 这条和 Scheduler 里那个 vram_estimate 的取舍是同一个道理。
    // **预算一律按物理显存，不按 vram_gb_override。** 那个数是拿来挑档位的。
    // 2026-09-10 配置里写着 override = 12，这一行就算出 10.8 GB 的上限——
    // 20 GB 的图像模型在第 34/62 段 OOM。卡真有多少显存，探到了就用探到的。
    const double budget = profile.gpu.has_value()
                              ? profile.gpu->vram_gb() * 0.9
                              : (profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0);
    // **auto 模式的预算按物理显存算，不按 vram_gb_override。**
    // override 是拿来挑档位的（44 GB 的卡想要 1280×704 的成片档就填 20），
    // 拿它当显存预算的话 auto_fit 会把本来装得下的编码器赶去内存。
    // 探测不到卡（没有 nvidia-smi）就退回上面那个数。
    // **先减计算缓冲的余量，再乘系数。**
    //
    // 0.9 那一成是给驱动上下文和别的程序留的，**不是给计算缓冲留的**——
    // 缓冲比它大一个量级：1280×704 的 VAE 解码实测 6576 MB。
    // 不减的话权重会把显存占满，然后 decode_first_stage 失败，
    // 而在接上 sd.cpp 的日志之前，上层只看得到一句"出图失败"。
    const double reserve = provider().models.vram_reserve_gb;
    const double physical =
        profile.gpu.has_value()
            ? std::max(1.0, profile.gpu->vram_gb() - reserve) * 0.9
            : budget;
    // 显式给了组件规格（比如 te=cpu,vae=cpu）的时候，权重放哪已经由用户
    // 定了，余量那 6 GB 就别再扣——扣了预算只剩 23 GB，扩散模型 19.5 GB
    // 加上它自己的计算缓冲就超了，5090 上表现是
    // "segment 56/62 failed during weight preparation"。整卡按 0.9 给。
    const double whole =
        profile.gpu.has_value() ? profile.gpu->vram_gb() * 0.9 : budget;
    // **按角色取自己那一项。** 2026-09-10 这里只看全局的 weights：视频那项
    // 是 "cpu"，走 `budget`（按 vram_gb_override = 20 算出 18 GB），而图像
    // 上下文拿的是常驻放置——20 GB 权重配 18 GB 上限，sd.cpp 在第 34/62 段
    // 报 "failed during weight preparation"，六镜首帧全废。
    // 捕获里**没有 `budget`**：下面三条分支全都返回 physical 或 whole 了
    // （"cpu" 那条 2026-09-13 从 budget 改成 physical，理由就在它自己那段
    // 注释里）。捕获了不用，-Wall 每次编译报一条 unused-lambda-capture。
    // `budget` 本身还在用——physical / whole 探不到卡时退回的就是它。
    const auto budget_for = [physical, whole](const config::Settings& s,
                                              ModelRole role) {
        const std::string& w =
            role == ModelRole::Video ? s.models.weights : s.models.image_weights;
        if (w == "auto") return physical;
        // **"cpu" 也要减计算缓冲的余量。**
        //
        // 这儿原来返回 `budget`（整卡 × 0.9，没减余量），撞的是上面那条注释
        // 描述得一字不差的墙，只是隔了一条分支：
        //
        //   auto 那次（2026-09-10，出首帧）  权重塞满后只剩 3447 MB
        //                                   → decode_first_stage failed，22 镜全废
        //   cpu 这次（2026-09-13，出片）     available 3797.06 MB device
        //                                   → weight preparation failed
        //
        // "cpu" 的字面意思是权重放系统内存，但 sd.cpp 的 --offload-to-cpu 是
        // **按需搬进显存、能缓存多少由预算说了算**（官方文档：weights cached
        // on RAM, moving them to VRAM on demand）。所以预算给多大它就占多大，
        // 计算缓冲照样没地方——和 auto 一个道理。
        //
        // 实测（walk_c，544×928）：出片这一路估 14.6 GB、**实占 31.39 GB**，
        // 整卡 31.84 GB，只剩 0.45 GB。一章十几镜里总有一两镜撞上，重试超限
        // 就降级（留着最后那一版没过闸门的），而成片照出——最难发现的那种。
        //
        // **注意余量默认 6 GB 是按出图的 VAE 解码定的**（6576 MB），而出片的
        // 计算缓冲更大（stages/limits.hpp 那边按 14.6 GB 记，还随画布涨）。
        // 所以这一改是"从没有余量变成有一点余量"，不是"从此够用"。真要顶到
        // 大画布，把 [models].vram_reserve_gb 调大——那一项的注释里写了
        // "出更大的图要调大它"。
        if (w == "cpu") return physical;
        return whole;
    };
    const std::size_t estimate =
        static_cast<std::size_t>(budget * 1024) * 1024 * 1024;

    // 老实数：这一路跑起来真正要占的显存（常驻权重 + 计算缓冲）。
    // 只有问到了卡上的空闲显存时才拿它比，见 SlotSpec::live_vram_estimate。
    // 这里能算是因为 provider 已经把 smart 展开成具体规格了。
    const auto live_bytes = [](double gb) {
        return gb > 0 ? static_cast<std::size_t>(gb * 1024) * 1024 * 1024
                      : static_cast<std::size_t>(0);
    };

    // **预算要真的设上。** 不设的话 Scheduler::make_room 第一行就
    // `budget_ == 0 → return true`，谁也不驱逐谁——下面"同时只装得下一个"
    // 那句注释描述的行为从来没生效过。
    //
    // 权重放系统内存时看不出来（显存里只有计算缓冲）；换成 weights = "auto"
    // 之后两套权重都常驻显存，出完首帧切去出片时一张 45 GB 的卡上是
    // 图像模型 + 它的编码器 + 视频模型 + umt5，直接 CUDA OOM，进程 abort。
    //
    // 预算按 auto 那条路的数取：它才是真占显存的那个。
    // **把实时显存的问法装上。** 不装的话调度器只信静态估算，
    // 每次切阶段都卸一个再装一个——一次重装几十秒到几分钟，
    // 而卡上可能一直空着一大半（权重放内存时显存里只有计算缓冲）。
    scheduler().set_free_vram_probe([] { return models::free_vram_gb(); });
    // 整卡显存，给"问不到卡"时的推算用。见 Scheduler::set_total_vram。
    //
    // **优先问 NVML 要"自己这张卡"的总量。** card_gb 来自
    // HardwareProfile::detect，那条取的是 nvidia-smi 输出的第一行，也就是
    // 物理 0 号；而工作进程可能绑在别的卡上（CUDA_VISIBLE_DEVICES）。
    // 同型号的多卡上两者一样，混插不同型号时 card_gb 可能偏大——
    // 而这个数偏大，"总量 − 别人占的 = 空闲"就偏乐观，正是会 OOM 的方向。
    // 见 models::visible_device_index。
    {
        double total = card_gb;
        if (const auto t = models::vram_totals_gb();
            t.has_value() && t->total_gb > 0.0) {
            total = t->total_gb;
        }
        if (total > 0.0) {
            scheduler().set_total_vram(
                static_cast<std::size_t>(total * 1024) * 1024 * 1024);
        }
    }

    // **把上次量到的读回来，并且以后量到新的就写下去。**
    //
    // 实测值只活在进程里的话，每次重启后的第一次出片都会白白卸掉大模型
    // ——那时候还没量到，走的是保守那条。而这个进程一天可能重启好几次
    // （改配置、换模型、崩了被拉起来）。
    //
    // 落在数据目录而不是配置目录：这是程序自己量出来的运行时事实，
    // 不是用户填的东西，不该混进他手写的 changji.toml 边上。
    {
        const fs::path store =
            paths::user_data_dir("changji") / "vram_measured.json";
        // **这套实测值属于哪一套配置。** 只收真正会改变占用、而且没有
        // 别的地方记着的那几项：两条路各自的预算（预算决定 sd.cpp 能在
        // 显存里囤多少权重）。模型换了的话文件大小会让预算那一项跟着变，
        // 不必单列。
        //
        // **画布不进指纹**（2026-09-13 起）。以前也收，但收的是**全局**
        // 设置里的画布，而画布是项目的属性：全局是 544×928、项目是
        // 704×1280，指纹一样、实测照收；反过来全局改一下，项目没变，
        // 整份作废。画布该走的门是 record_measured_vram 记下的 work
        // （像素 × 帧），量过的活罩不住这次的活时按比例放大
        // （Scheduler::make_room），那才是按每一镜真实的画幅判。
        const std::string fingerprint = [&] {
            const config::Settings s0 = provider();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "v%.2f/i%.2f",
                          budget_for(s0, ModelRole::Video),
                          budget_for(s0, ModelRole::Image));
            return std::string(buf);
        }();
        std::error_code ec;
        std::ifstream in(store, std::ios::binary);
        if (in) {
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            int no_work = 0;
            const auto loaded = parse_measured_vram(text, fingerprint);
            if (loaded.empty() && text.find("\"bytes\"") != std::string::npos) {
                // **说一声为什么攒下的数不作数了。** 不说的话用户看到的是
                // "设置页上那个实测值怎么没了"，以及第一镜又卸了一次模型。
                std::fprintf(stderr,
                             SAY_NEVER("[vram] 上次量到的峰值是在另一套配置下"
                                       "量的（预算或画布变了，或者是升级前的"
                                       "老文件），不再是上限，整份作废。"
                                       "第一镜会重新量一次。\n"));
            }
            for (const auto& [slot, v] : loaded) {
                scheduler().record_measured_vram(slot, v.bytes, v.work);
                if (v.work == 0) ++no_work;
            }
            // **说一声第一镜为什么会卸模型。**
            // 老版本存下来的实测值里没有"当时量的是多大的活"，按画幅门
            // 的规矩就不能拿来背书（见 Scheduler::record_measured_vram）。
            // 于是升级之后第一镜必定先卸一次、重新量。不说的话用户看到的
            // 是"又卸了"，会以为根本没修好。
            if (no_work > 0) {
                std::fprintf(stderr,
                             SAY_NEVER("[vram] 读到 %d 条老格式的实测值"
                                       "（没记画幅）。第一镜会重新量一次，"
                                       "那一镜会先腾显存；量完之后"
                                       "「够就不清理」照常。\n"),
                             no_work);
            }
        }
        scheduler().set_measured_sink([store, fingerprint](Slot, std::size_t) {
            // 整份重写，不是追加——就四个槽，文件几十字节。
            //
            // **先写临时文件再改名，不要原地 truncate。**
            // 原地写有两种撕裂法，两种都真实存在：
            //   1. 多卡时一张卡一个工作进程，**八个进程写同一个文件**
            //      （这个路径按用户数据目录算，和卡无关）。两个进程的
            //      truncate + write 交错，读回来就是半截 JSON。
            //   2. 写到一半被 Ctrl+C 或者 OOM 的 abort() 打断。出片那一步
            //      正是最容易 abort 的时候，而这次写入往往就跟在它后面。
            // 撕裂的后果不是崩——parse 会当成"没量过"而返回空——但那等于
            // 把攒下来的实测值全丢了，下一次出片又要白卸一遍模型。
            // rename 是同目录内的原子替换，读的人要么看到旧的要么看到新的。
            std::error_code e;
            fs::create_directories(store.parent_path(), e);
            // 临时名带进程号：八个进程各写各的，不会互相覆盖到一半。
            // 一个进程一个后缀。**不用进程号**：那要平台分支
            // （getpid / _getpid），而这里只要"别撞车"，随机数就够。
            static const std::string kTag = [] {
                std::random_device rd;
                return std::to_string(rd()) + "-" + std::to_string(rd());
            }();
            fs::path tmp = store;
            tmp += ".tmp." + kTag;
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) return;   // 写不了就算了，下次重新量
                out << serialize_measured_vram(scheduler().all_measured(),
                                               fingerprint);
                if (!out) {         // 磁盘满之类
                    out.close();
                    fs::remove(tmp, e);
                    return;
                }
            }
            fs::rename(tmp, store, e);
            if (e) fs::remove(tmp, e);   // 改不过去就别留一地临时文件
        });
    }

    // **调度器的预算和 sd.cpp 的 max_vram 不是一回事，别去"统一"它们。**
    //
    // sd.cpp 的 max_vram：这一路的**权重**能在显存里囤多少。
    // 调度器的 budget：整个规划的信封——各槽的 vram_estimate 要装得进去。
    //
    // 2026-09-13 我把这里改成 budget_for(Video) 想"一处定义"，结果是
    // 信封（23.26 GB）比视频槽自己的估算（28.66 GB）还小，make_room 永远
    // 满足不了，**每一镜都报「显存不够加载 视频。腾不出空间」然后降级**。
    // 一整章十八镜全废。
    //
    // 所以这里保持按整卡算：估算和信封都用 `budget`，两者一致；
    // 给 sd.cpp 的权重预算另算（budget_for 那边减了计算缓冲的余量），
    // 那是**信封里给权重划的一块**，本来就该更小。
    scheduler().set_budget(
        static_cast<std::size_t>(
            (provider().models.weights == "auto" ? physical : budget) * 1024) *
        1024 * 1024);

    // 两个槽的估值都按整个预算算，也就是**同时只装得下一个**。
    // 这不是保守，是事实：6GB 卡上图像模型和视频模型任意一个都要占满，
    // 让调度器知道这件事，它才会在切阶段时先卸掉另一个。
    {
        SlotSpec spec;
        spec.slot = Slot::Image;
        spec.residency = Residency::Cached;   // 每个镜头都要，别反复卸
        spec.vram_estimate = estimate;
        // 每次借槽时现算：模型可能已经被换掉了（模型窗就能换），
        // 而槽一个进程只注册一次。见 SlotSpec::live_vram。
        spec.live_vram = [provider, live_bytes] {
            const config::Settings s = provider();
            // **按这一刻要装的那一份算**：图像槽上换着两个模型
            // （Edit 出首帧、基础出定妆和空景），一律按 image 算的话，
            // 装基础那一份时估的是另一个文件。
            return live_bytes(
                config::image_placement_of(s, diffusion_for(s.models,
                                                            g_image_want.load()))
                    .live_vram_gb);
        };
        // 视频模型重新加载更贵（文件大得多），所以图像的优先级更低，
        // 腾地方时先卸它。
        spec.evict_priority = 5;
        spec.load = [provider, budget_for] {
            const config::Settings s = provider();
            const ModelRole want = g_image_want.load();
            auto ctx = SdContext::create(s, budget_for(s, want), want);
            std::lock_guard lg(g_ctx_mu);
            g_image_ctx = std::move(ctx);
            g_image_loaded.store(want);
            g_image_loaded_valid.store(true);
        };
        spec.unload = [] {
            std::lock_guard lg(g_ctx_mu);
            g_image_ctx.reset();   // 析构里 free_sd_ctx
            g_image_loaded_valid.store(false);
        };
        scheduler().register_slot(std::move(spec));
    }
    {
        SlotSpec spec;
        spec.slot = Slot::Video;
        spec.residency = Residency::Cached;
        spec.vram_estimate = estimate;
        // **出片这个槽故意不给 live_vram。**
        //
        // 别的槽给了估算，是因为那个估算被实测对上过：图像走 "cpu" 那一路
        // 算 10.6 GB、实测也是 10.6；大模型 9 GB 的文件算 15.25、实测 15.4。
        // 出片这一路两条分支都被实测**推翻**过，而且是往小了错：
        //   weights="cpu"    算 14.6 GB，实测 74 GB
        //   weights="te=cpu" 算 81.8 GB，实际超过 95.6 GB，当场 CUDA OOM
        // 原因写在 Scheduler::record_measured_vram 上面：权重放不放显存
        // 决定不了占用，ggml 照样按层往显存搬、分配器还留着大池子，
        // 而这些随模型大小、画幅、帧数剧烈变化。
        //
        // 拿一个往小了错五倍的数去判"够，不卸"，下一步就是 CUDA OOM——
        // 走 GGML_ASSERT 直接 abort()，整个服务没了。不给这个数，
        // 没量过时就退回整份预算那条保守路：第一镜该卸就卸（慢几十秒），
        // 跑完这一镜就量到了，之后每一镜都按真数判。这正是
        // scheduler.hpp 上写的那句"没量到之前一律走保守那条"。
        //
        // 估算本身没删——设置页还照样显示它，和实测值并排摆着，
        // 差多少一眼看得见。只是不再拿它去做驱逐判断。
        spec.evict_priority = 9;
        spec.load = [provider, budget_for] {
            const config::Settings s = provider();
            auto ctx = SdContext::create(s, budget_for(s, ModelRole::Video),
                                         ModelRole::Video);
            std::lock_guard lg(g_ctx_mu);
            g_video_ctx = std::move(ctx);
        };
        spec.unload = [] {
            std::lock_guard lg(g_ctx_mu);
            g_video_ctx.reset();
        };
        scheduler().register_slot(std::move(spec));
    }
}

Lease acquire_image(ModelRole role, const Scheduler::AcquireOptions& opt) {
    // 出片那一档不该走这条路，走了也按 Edit 处理，别让它把图像槽换成视频。
    const ModelRole want =
        role == ModelRole::ImageBase ? ModelRole::ImageBase : ModelRole::Image;
    // **装错了就先卸。** 调度器看到槽"已经加载"就不会再调 load，
    // 于是不卸的话，要基础模型的那一步会拿到还挂在槽上的 Edit 权重——
    // 而那正是这次要治的病（定妆拿 Edit 做文生图）。
    if (g_image_loaded_valid.load() && g_image_loaded.load() != want) {
        scheduler().evict(Slot::Image);
    }
    g_image_want.store(want);
    return scheduler().acquire(Slot::Image, opt);
}

std::shared_ptr<SdContext> current_image_context() {
    std::lock_guard lg(g_ctx_mu);
    return g_image_ctx;
}

std::shared_ptr<SdContext> current_video_context() {
    std::lock_guard lg(g_ctx_mu);
    return g_video_ctx;
}

}  // namespace changji::infer
