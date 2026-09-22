#include "config/runtime.hpp"
#include "util/say.hpp"

#include <algorithm>
#include <cstdio>

#include "stages/limits.hpp"

namespace {

/// 这张卡有多少显存。**探测一次就记住**：探显卡要跑 nvidia-smi，而
/// Runtime::replace 在一次运行里会被调好几次（起服务、改设置、换机器）。
/// 一次运行里显卡不会变。
double detected_vram_gb() {
    static const double v = [] {
        const auto p = changji::models::HardwareProfile::detect(std::nullopt);
        return p.vram_gb;
    }();
    return v;
}

}  // namespace

namespace changji::config {

Settings Runtime::snapshot() const {
    std::lock_guard lg(mu_);
    return settings_;
}

stages::VideoLimits video_limits_for(const Settings& s) {
    // **先按模型自己认，配置里填了才覆盖。** 换模型时人改的是 [models].video
    // 那一行，不会想起来还有帧数格子要跟着改——所以默认让它自己认，
    // 那三项留 0 就是"你看着办"。
    stages::VideoLimits limits = stages::guess_video_limits(
        s.models.video, !s.models.video_llm.empty());
    // 再按这张卡夹一道：**模型能出 15 秒不等于这张卡能出 15 秒**。
    // 探测不到显卡（单元测试、没装驱动）时 cap_by_vram 原样返回，不放开。
    //
    // 常驻权重按"全放内存"算（0），也就是显存全给计算缓冲——5090 上
    // 1280×704 就是这么配的（weights = "cpu"）。真常驻一部分权重时可用的
    // 更少，但那种配置本来也跑不了长镜头，夹得更短没坏处。
    limits = stages::cap_by_vram(limits, detected_vram_gb(), 0.0);
    // 再按画布夹一道**内核跑得动**的上限。这一条不是显存：大卡上 cap_by_vram
    // 会放开到 12 秒，而 704×1280 跑到 294 帧是整个引擎当场死掉
    // （见 cap_by_kernel_limit 的实测表）。画布取的是这份设置里的 [video]，
    // 也就是 --project 那个项目的；别的项目按各自的画布出片时，这里用的
    // 仍是这一份——和上面帧率那条一样的两条入口问题，先按最常见的一路堵住。
    {
        const auto [cw, ch] = s.video.size();
        limits = stages::cap_by_kernel_limit(limits, cw, ch);
    }
    if (s.models.video_max_frames > 0) limits.max_frames = s.models.video_max_frames;
    if (s.models.video_frame_step > 0) limits.frame_step = s.models.video_frame_step;
    if (s.models.video_frame_base >= 0 && s.models.video_frame_step > 0) {
        limits.frame_base = s.models.video_frame_base;
    }
    // 最后是这部电影自己要的。**放在手填覆盖之后**：video_max_frames 是
    // "这张卡/这个模型最多能出多少"，max_shot_s 是"这部电影要多短"，
    // 这部电影的要求只能往下夹，不能借它把机器的上限抬上去。
    if (s.video.max_shot_s > 0.0) {
        const int fps = stages::effective_fps(limits, s.assembly.fps);
        // frames_for 已经夹在上限之内，所以要的秒数比机器上限还长时它回的
        // 就是上限本身（对齐到格子），这时什么都别动。
        const int want = limits.frames_for(s.video.max_shot_s, fps);
        if (want < limits.max_frames_on_grid()) limits.max_frames = want;
    }
    return limits;
}

void apply_video_limits(const Settings& s) {
    stages::set_video_limits(video_limits_for(s));
}

void Runtime::replace(Settings s) {
    // 视频模型的限制跟着配置走。**放在这一处而不是每个调用方**：能改配置的
    // 入口有五个（起服务、/api/settings、/api/connections、模型窗……），
    // 漏掉任何一个都是"配置改了但分镜还按老上限排"，而且不报错。
    // 怎么算的见 video_limits_for；按项目重读设置的那几条路也调它。
    apply_video_limits(s);

    // **帧率也得跟着模型走。** MiniMax-H3 只出 24fps，传别的值 sd.cpp
    // 自己覆盖掉（只打一句 LOG_WARN，淹在 CUDA Graph 刷屏里）。而我们这边
    // `[assembly].fps` 还按人填的那个数去算帧数、算每镜时长、编码——
    // 填 30 的话整片快 25%，人走路变小跑，字幕跟着漂。**不报错。**
    //
    // **这是两条入口里的第二条。** 另一条是 `load_settings`：出片那条路
    // 每跑一章都从项目的 changji.toml 重读一遍设置（http/run.cpp 里那句
    // `load_settings(store.root())`），**根本不经过 Runtime**。两条互不
    // 相通，只堵一条等于没堵，所以那边在 migrate_legacy 里调同一个函数。
    if (const std::string note = normalize_fps_for_model(s); !note.empty()) {
        std::fprintf(stderr, SAY_NEVER("[配置] %s\n"), note.c_str());
    }
    resolve_model_family_defaults(s);

    std::lock_guard lg(mu_);
    settings_ = std::move(s);
}

void Runtime::set_tier_override(models::Tier tier,
                                const models::TierSpec& spec) {
    std::lock_guard lg(mu_);
    tier_overrides_[tier] = spec;
}

void Runtime::clear_tier_overrides() {
    std::lock_guard lg(mu_);
    tier_overrides_.clear();
}

models::HardwareProfile Runtime::profile() const {
    std::optional<double> vram;
    std::map<models::Tier, models::TierSpec> overrides;
    TiersConfig from_file;
    {
        std::lock_guard lg(mu_);
        vram = settings_.vram_gb_override;
        overrides = tier_overrides_;
        from_file = settings_.tiers;
    }
    // detect() 会跑 nvidia-smi，别拿着锁做。
    models::HardwareProfile p = models::HardwareProfile::detect(vram);

    // **配置文件里的 [tiers] 先落，进程内的覆盖后落。**
    // 顺序不能反：进程内那份是用户刚在设置页改的，比文件新。
    // 0 表示"这一项没填"，照旧用按显存推出来的值。
    const auto apply = [&p](models::Tier t, int w, int h, int st) {
        const auto it = p.tiers.find(t);
        if (it == p.tiers.end()) return;
        if (w > 0) it->second.width = w;
        if (h > 0) it->second.height = h;
        if (st > 0) it->second.steps = st;
    };
    apply(models::Tier::DRAFT, from_file.draft_width, from_file.draft_height,
          from_file.draft_steps);
    apply(models::Tier::FINAL, from_file.final_width, from_file.final_height,
          from_file.final_steps);

    for (const auto& kv : overrides) {
        const auto it = p.tiers.find(kv.first);
        if (it == p.tiers.end()) continue;
        // 只覆盖三个字段，measured_seconds 保留——那是标定出来的，
        // 换个分辨率不代表标定结果作废，而重新标定要跑一遍真实渲染。
        it->second.width = kv.second.width;
        it->second.height = kv.second.height;
        it->second.steps = kv.second.steps;
    }

    // **最后把成片档换成真正会跑的那一档。**
    //
    // 档位表里的步数假设的是不挂蒸馏 LoRA 的模型，画幅是按这张卡推出来的；
    // 挂着 Turbo 时 effective_spec 会把步数改成 6、把画幅换成项目 [video]
    // 里那个，而**那一步在出片的路上，不在这儿**。于是同一台机器两个说法：
    // 这个 profile 说 1920×1088 / 30 步 / 908 秒，实跑 544×928 / 6 步 /
    // 210 秒。而 profile 正是 /api/hardware 和 /api/run/preview 的唯一来源
    // ——界面上的规格和"要等多久"两个数都是错的，人还按它安排时间。
    //
    // **上一版只换了步数，没换画幅。** 于是 /api/hardware 一直报
    // 1920×1088，而磁盘上的成片是 544×928（ffprobe 量的）——同一个函数里
    // 同一个毛病，修了一半。画幅差 4.13 倍像素，比步数那一项还大。
    Settings snap;
    {
        std::lock_guard lg(mu_);
        snap = settings_;
    }
    const auto fin = p.tiers.find(models::Tier::FINAL);
    if (fin != p.tiers.end() && fin->second.steps > 0) {
        models::TierSpec& spec = fin->second;
        // **压扁之前先把这个数存下来。**
        //
        // 这里存而不是只靠 detect() 存，是因为上面 [tiers] 和进程内覆盖
        // 可能已经改过它——人显式填了 final_steps 的话，首帧该跟着那个
        // 数走，不是跟着表里的原始值走。见 HardwareProfile::table_final_steps。
        p.table_final_steps = spec.steps;
        const auto eff = effective_spec(snap, spec.steps);
        // 耗时的缩法（为什么不是纯步数比）写在 workload_scale 头上。
        // 必须在改 spec 之前算：它拿的是**表里**那一档当基准。
        const double scale =
            workload_scale(spec.width, spec.height, spec.steps, eff);
        if (spec.measured_seconds.has_value()) *spec.measured_seconds *= scale;
        if (eff.final_steps > 0) spec.steps = eff.final_steps;
        spec.steps_pinned = eff.steps_pinned;
        if (eff.width > 0 && eff.height > 0) {
            spec.width = eff.width;
            spec.height = eff.height;
        }
    }
    return p;
}

Runtime& runtime() {
    static Runtime r;
    return r;
}

}  // namespace changji::config
