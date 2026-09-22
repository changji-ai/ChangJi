#pragma once

// 用 sd.cpp 出图。
//
// 这一层是**阶段 5 的核心**，也是模型调度器真正派上用场的地方：
// sd_ctx 的建立和销毁挂在 Scheduler 的槽位上，跨阶段的显存回收由它决定。
//
// ---
//
// 和 sd_backend.hpp 一样，这个头文件**不 include sd.cpp 的头，也不带 #ifdef**。
// 没链 sd.cpp 时 generate() 抛一个说人话的异常，调用方照常写。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <map>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "infer/scheduler.hpp"
#include "models/hardware.hpp"
#include "pipeline/jobs.hpp"

namespace changji::infer {

class SdError : public std::runtime_error {
public:
    explicit SdError(const std::string& what) : std::runtime_error(what) {}
};

/// 一次出图的参数。
/// 一次生成的采样旋钮。**属于这一次请求，不属于加载好的模型。**
///
/// 以前这三个数是在 `SdContext::create` 时从 `[models]` 读出来、
/// 冻在上下文里的，`ImageRequest::cfg` 上还留着一行注释说自己"不再生效"。
/// 那个设计有两个后果，都是 2026-09-13 验 flow_shift 时当场撞到的：
///
///   1. **项目自己的 `changji.toml` 完全不参与。** 建上下文用的是
///      `register_sd_slots` 里那个全局 `provider()`（`Runtime::snapshot()`），
///      而出片每跑一章是拿 `load_settings(项目目录)` 的那一份跑的。
///      于是我在项目里写 `video_flow_shift`，两次出片**字节完全相同**。
///   2. **上下文是 `Residency::Cached`**，改了全局配置也要等它被卸载才生效。
///      于是"改个参数重跑一镜看看"这件事根本不成立，而它不报错。
///
/// 请求里带着走就没有这两个问题——`use_lora` 早就是这么做的，
/// 理由写在 sd_video.cpp 里：上下文是草稿和成片共用的。
struct SamplingKnobs {
    /// 文本 CFG。**H3 要 1.0，Qwen-Image 要 2.5，Wan 要 6.0**，差得很远。
    /// `<= 0` = 没填，`generate*` 会当场抛——悄悄用 sd.cpp 的 7.0 跑 H3
    /// 出来的是一团噪点，而那种错人会先去怀疑提示词。
    double cfg = 0.0;
    /// 0 = 自动，交给 sd.cpp 按模型架构挑。见 `sd_flow_shift`。
    double flow_shift = 0.0;
    /// 双专家模型两个专家交班的 sigma 阈值。`<= 0` = 用 sd.cpp 的默认。
    /// 只有 `video_high_noise` 非空时有意义。
    double moe_boundary = 0.0;
};

struct ImageRequest {
    std::string positive;
    std::string negative;
    int width = 512;
    int height = 512;
    int steps = 20;
    /// 种子。**必须显式给**，不能让它随机——重跑同一个镜头要能得到
    /// 同一张图，否则"重试"和"换一张"就分不清了。
    std::int64_t seed = 0;
    /// 这一次的采样旋钮。**必须填**，见 SamplingKnobs。
    SamplingKnobs knobs;
    /// 参考图的绝对路径。图像编辑模型那条路会用，纯文生图忽略。
    std::vector<std::filesystem::path> reference_images;

    /// VAE 解码分块。**出片那条路早就有，出图这条路以前一个字没填**——
    /// 于是 sd.cpp 走的是整图解码：1280×704 一次要 6.6 GB 计算缓冲，
    /// fp8 扩散模型 19.5 GB 常驻之后 32 GB 的卡挤不出来，报
    /// `vae decode compute failed`（分块那条路的文案是"…while processing
    /// a tile"，看文案就能分辨走的哪条路）。块大小按潜空间算，和出片那边
    /// 一致：16×11 个潜空间格子、重叠 1/4。
    bool vae_tiling = true;
    int vae_tile_x = 16;
    int vae_tile_y = 11;
    double vae_tile_overlap = 0.25;

    /// 这一次生成是给哪一镜的（shot_id）。**只给预览用**：sd.cpp 的预览回调
    /// 是全局的，靠它才知道推上来的小图该挂在墙上哪一格。空就不推预览。
    std::string tag;
};

/// 每一步的进度。
///
/// 采样一步在低配机器上要好几秒，不报的话界面上就是一条几分钟不动的进度条，
/// 用户分不清是在跑还是卡死了。
/// 出图过程中的进度回调。
///
/// sd.cpp 拿同一个回调报三种阶段，总数各不相同：
///   - **Prep**：采样前——分段搬权重、首次从磁盘载权重、腾显存；
///   - **Sample**：真的采样，总数等于请求的步数；
///   - **Decode**：采样后 VAE 分块解码，一块一格。
/// 不分开的话用户会看到"第 1927/1927 步"紧接着"第 1/8 步"再接着
/// "第 78/78 步"，像是跑到头又倒回去两次——而 1927、78 这两个数对他
/// 没有任何意义。**解码不是准备**：它在采样之后，VAE 放显存时只要
/// 两三秒、放内存时二十几秒，牌子上写"准备"用户会以为模型又在重载
/// （2026-09-15 报的）。
///
/// `Wait`：池里每一台工作机都连不上，这一镜**在队列里等它们回来**（不判
/// 失败、不停整轮）。step 传已等的分钟数，steps 传 0。
enum class Phase { Prep, Sample, Decode, Wait };

/// 进 JSON / 事件用的名字："prep" / "sample" / "decode" / "wait"。
const char* phase_name(Phase p);
/// 反过来；认不出的一律按 Sample（老进程不带这个字段）。
Phase phase_from(const std::string& name);
/// 牌子上跟在"出首帧 sh3"后面的那一截："（第 3/8 步）"、"（准备 6/28）"、
/// "（解码 12/78）"；还没进采样也没步数时是"（正在准备模型，可能要先腾出
/// 显存）"——写"准备 0/0"只会让人以为出错了。
std::string phase_note(Phase p, int step, int steps);

using StepCallback =
    std::function<void(int step, int total, double seconds, Phase phase)>;

/// 采样中途的预览图。
///
/// `tag` 是请求里带的 shot_id；`data_url` 是一张 `data:image/png;base64,…`
/// 的小图——潜空间分辨率（704×1280 出来是 88×160），由 sd.cpp 的
/// PREVIEW_PROJ 把潜空间线性投影成 RGB 得来，**不走 VAE**，几乎不花时间。
/// 放大到格子大小自然是糊的，随着步数推进内容逐渐成形。
using PreviewSink =
    std::function<void(const std::string& tag, int step, std::string data_url)>;

/// 挂一个预览落点，回一个牌子；拿牌子摘掉。
///
/// ⚠️ **可以同时挂好几个，每个自己认 tag。** 原来这儿只有一个槽位，
/// 谁装谁覆盖——出片那条在跑的时候，设定页点一张参考图就会把出片的预览
/// 顶掉，而顶掉这件事没有任何提示，表现是镜头墙上的小图突然不动了。
/// 挂多个之后各收各的：每个落点看 tag 是不是自己那件事，不是就不管。
int add_preview_sink(PreviewSink sink);
void remove_preview_sink(int token);

/// 把一张预览发给所有挂着的落点。sd.cpp 的回调（进程内采样）和工作进程
/// 池（活派在别的机器上、预览随轮询带回来）都走这一个口子——
/// 页面上的小图不该知道活是在哪台机器上跑的。
void publish_preview(const std::string& tag, int step, std::string data_url);

/// 挂上、出作用域自动摘。
class PreviewSinkHandle {
public:
    explicit PreviewSinkHandle(PreviewSink sink)
        : token_(add_preview_sink(std::move(sink))) {}
    ~PreviewSinkHandle() { remove_preview_sink(token_); }
    PreviewSinkHandle(const PreviewSinkHandle&) = delete;
    PreviewSinkHandle& operator=(const PreviewSinkHandle&) = delete;

private:
    int token_ = 0;
};

/// 一次出视频的参数。
struct VideoRequest {
    std::string positive;
    std::string negative;
    int width = 448;
    int height = 768;
    int steps = 8;
    int frames = 49;
    int fps = 24;
    std::int64_t seed = 0;
    /// 这一次的采样旋钮。**必须填**，见 SamplingKnobs。
    SamplingKnobs knobs;
    /// 首帧。跨镜头一致性全靠它，没有的话退化成纯文生视频。
    std::optional<std::filesystem::path> start_image;
    /// 尾帧。给了就走首尾帧（FL2VA）：片子从首帧出发、落在这一张上。
    /// 只有首帧没有尾帧是老行为。MiniMax-H3 的 fl2va 权重原生支持，
    /// Wan 那一族不认、sd.cpp 会忽略——所以给了也不会坏，只是没用。
    std::optional<std::filesystem::path> end_image;

    /// VAE 分块解码。**6GB 卡上出视频的必要条件**，不是可选的优化。
    ///
    /// 实测（verify/RESULTS.md 三点八，RTX 2060 6GB，Wan 2.2 TI2V-5B，
    /// 640x352 9 帧）：
    ///
    /// | 配置 | VAE 解码所需显存 | 结果 |
    /// |---|---|---|
    /// | 不分块 | 11747 MB | 失败（可用 5081 MB）|
    /// | 默认 32x32 分块 | 9610 MB | 失败 |
    /// | 再开时间维分块 | 10321 MB | 失败，**反而更高** |
    /// | 16x11、重叠 0.25 | — | 成功，44.7 秒 |
    ///
    /// 大显存的机器为它多付一点解码时间。反过来关掉的话，这台机器上
    /// 一段视频都出不来——而这个项目的立项理由就是"6GB 卡跑 16GB 模型"。
    bool vae_tiling = true;

    /// 分块大小，**单位是潜变量格子不是像素**。
    ///
    /// 用绝对值不用比例，是为了让每块的显存占用不随分辨率变：
    /// 比例分块在换到成片档分辨率时每块也跟着变大，峰值显存照样爆。
    /// 潜变量比这个还小的时候 sd.cpp 自己会夹住（退化成不分块），
    /// 那种情况本来也不需要分块。
    ///
    /// 默认 32x32 为什么不行：碰上 40x22 的潜变量切出来是 2x1 块、
    /// 重叠率 0.75——两块几乎完全重叠，只降了 18%。
    int vae_tile_x = 16;
    int vae_tile_y = 11;
    double vae_tile_overlap = 0.25;

    /// 时间维分块。**默认关**：9 帧只压成 3 个潜变量帧，而分块粒度是 4，
    /// 切不动还引入有状态分块自身的开销，实测显存从 9610 涨到 10321 MB。
    bool vae_temporal_tiling = false;

    /// 这一次要不要挂 [models].video_lora。**按档位分**：Turbo 那类
    /// 蒸馏 LoRA 拿画质换速度，草稿档挂着划算，成片档不该挂。
    /// 上下文是两档共用的，所以只能在每次请求上决定，不能在建上下文时定。
    bool use_lora = true;

    /// 同 ImageRequest::tag。视频取第一帧做预览。
    std::string tag;
};

/// 这个上下文装的是哪个模型。
///
/// 图像和视频是**两个不同的模型**（首帧用图像编辑模型，视频用 Wan），
/// 各自一个 sd_ctx、各自一个调度槽。合成一个的话，跑首帧时视频模型
/// 也占着显存，而 6GB 卡上那意味着两个都装不下。
enum class ModelRole {
    /// 首帧：Edit 权重，要收参考图。
    Image,
    /// 从零出图（角色三视图、空景图）：基础文生图权重，一张参考图都不传。
    /// `[models].image_base` 留空时和 Image 取同一个文件，也就是老行为。
    ImageBase,
    Video,
};

/// 这份配置能不能拿来出图 / 出片。能就返回空串，不能就返回一句人话。
///
/// **纯判断，和有没有链上 sd.cpp 无关**，所以放在 `#ifdef` 外面——
/// 测试目标编的是没链上游那一支，判断逻辑要是写在 `#ifdef` 里面就测不到。
/// 和 CMakeLists 里说的"纯判断拆出来才测得到"是同一个理由。
///
/// 最要紧的一条：**没配 image 时不许拿 video 顶替**。
/// 2026-09-08 实测那么干会让整个进程崩掉（0xc0000094 整数除零），
/// 因为 `sd_img_gen_params_t` 没有 video_frames 字段，
/// 压根没法告诉 generate_image 出几帧。
/// 从设置里把这个角色的采样旋钮取出来。
///
/// **只有这一个地方在做这件事**，三个构造请求的地方（出首帧、出片、
/// 参考图）都调它。角色分开是因为图像和视频这两套数完全不同，
/// 而共用过一次的后果是"出图那条路从来没真跑过，没人发现"。
SamplingKnobs sampling_knobs_for(const config::Settings& settings,
                                 ModelRole role);

std::string sd_model_problem(const config::Settings& settings, ModelRole role);

/// 配置里的 `flow_shift` 翻成传给 sd.cpp 的那个 float。
/// **`<= 0` 就是"没填"，翻成 `INFINITY`**——sd.cpp 看到 INFINITY 会用
/// 它按模型架构定的 `default_flow_shift`（Wan 5、HunyuanVideo 7、
/// MiniMax-H3 12、Qwen-Image / SD3 这一类 3）。
///
/// **这一步存在的理由是一次真出过的错。** `video_flow_shift` 的默认值
/// 原来写死 3.0，那是上游 docs/wan.md 给 Wan2.2 TI2V-5B 的推荐值；
/// 出片模型换成 MiniMax-H3 之后这个数没人跟着改，于是每一镜都在拿
/// Wan 的 time-shift 跑 H3（它要的是 12）。**全程不报错**，
/// 表现只是"片子看着不太对"——人会先去怀疑提示词。
///
/// 和 `sd_model_problem` 一样放在 `#ifdef` 外面，为的是测得到。
float sd_flow_shift(double configured);

/// 中心裁剪矩形，单位像素。
struct CropBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    /// 不用裁：矩形就是整张图。
    bool whole(int src_w, int src_h) const {
        return x == 0 && y == 0 && w == src_w && h == src_h;
    }
};

/// 把 src 按 dst 的**长宽比**中心裁一刀。只裁不缩，尺寸交给下游。
///
/// **为什么要有这一步。** 出片时首帧是当 `init_image` 传给 sd.cpp 的，
/// 而它内部走 `sd_image_to_tensor(img, 目标宽, 目标高)`——那个函数在
/// 尺寸不一致时直接 `interpolate` 到目标，**不管长宽比**。也就是说
/// 喂一张比例不同的首帧进去，出来的片是被拉扁或拉长的，
/// 而且**不报错也不打日志**，只是人脸变宽了一点。
///
/// 这条路是真会走到的：首帧和成片是分开的两个动作，用户完全可以
/// 只重出成片、留着以前的首帧。2026-09-10 把画幅从 704×1280 改成
/// 544×928，两者比例 0.550 对 0.586，差 6%——够看出来了。
///
/// 先中心裁到目标比例，再让 sd.cpp 去缩，出来的就是正的。
/// 裁掉的是长边两头，构图中心不动。
///
/// 比例本来就一样（含尺寸完全相同）时返回整张图，
/// 让调用方能靠 `whole()` 跳过拷贝。
CropBox center_crop_box(int src_w, int src_h, int dst_w, int dst_h);

/// 实测显存的落盘格式：把 `{槽名: 字节数}` 转成一行 JSON，和反过来。
///
/// **单独拆出来是为了能测。** 读写文件那半没法在单元测试里跑，
/// 但"字段名对不对、坏数据会不会让程序崩"是能测也必须测的——
/// 这个文件在两次运行之间保存的是**决定要不要卸模型的依据**，
/// 解析出错的代价是要么白卸（慢），要么不该不卸（OOM）。
/// `fingerprint` 是量这些数时的那套配置（预算、画幅……）。见 parse。
std::string serialize_measured_vram(const std::map<Slot, Scheduler::Measured>& m,
                                    const std::string& fingerprint = "");

/// 坏行、缺字段、负数、不认得的槽名，一律**跳过那一条**，不影响别的。
/// 整个文件解不开就返回空——那等于"没量过"，回到保守那条，安全。
///
/// `want` 非空时还要**对一下配置指纹**：实测值是"在那套配置下见过的峰值"，
/// 换了配置就不作数了。
///
/// **2026-09-13 撞到的**：给出片的预算补上计算缓冲的余量之后，一镜的实际
/// 峰值从 32143 MiB 降到 24563 MiB（省了 7.4 GB），而记录下来的还是
/// 31.39 GB——因为 record_measured_vram 是「只往上记，不往下调」。
/// 那条规则对：同一套配置下不同镜头有出入，取最大值才安全。错的是
/// **换了配置之后它也不肯降**。于是设置页上一直显示 31.39 GB（比实际高
/// 28%），而调度器每次借视频槽都按 31.39 GB 腾地方——整卡才 31.84 GB，
/// 「够就不清理」那条优化永远不触发，每次换阶段白卸一遍模型。
///
/// 指纹对不上就整份当"没量过"。这和上面那条"解不开就返回空"是同一个
/// 取舍：跑一镜自己就补回来，代价只是那一镜先腾一次显存。
std::map<Slot, Scheduler::Measured> parse_measured_vram(
    const std::string& text, const std::string& want = "");

/// sd.cpp 的上下文。**贵**：建一次要解析模型文件、建张量图、分配运行时缓冲。
///
/// 所以它不是每次出图新建一个，而是挂在调度器的槽位上复用。
/// 一章四十个镜头：复用是建一次用四十次，不复用是建四十次，
/// 在 6GB 卡上后者慢到不可用。
class SdContext {
public:
    /// 按配置建一个。模型路径从 [models] 来。
    ///
    /// vram_budget_gb 是给 sd.cpp 的 max_vram：0 表示"用当前空闲显存，
    /// 不设显式预算"。**这个数不是显卡有多少**，是留给推理多少。
    static std::shared_ptr<SdContext> create(const config::Settings& settings,
                                             double vram_budget_gb,
                                             ModelRole role);

    ~SdContext();
    SdContext(const SdContext&) = delete;
    SdContext& operator=(const SdContext&) = delete;

    /// 出一张图，写到 dest（PNG）。
    ///
    /// tok 会在每一步的回调里查。sd.cpp 的采样循环中途打得断
    /// （sd_cancel_generation），所以点了停止不用等这一镜跑完。
    void generate(const ImageRequest& req, const std::filesystem::path& dest,
                  pipeline::CancelToken& tok, const StepCallback& on_step);

    /// 出一段视频，把**裸 RGB24 帧**顺序写到 raw_dest。
    ///
    /// 不在这里编码成 mp4：编码是 ffmpeg 的事，而这一层不该知道
    /// 编码参数从哪儿来。见 sd_video.hpp。
    ///
    /// **模型出了声音的话，同时写一个 wav**，路径是 raw_dest 换成 .wav 后缀
    /// （`audio_path_for(raw_dest)`）。MiniMax-H3 每镜都出一条立体声
    /// （环境声、动效，有时还有人声），2026-09-14 之前它被原地丢掉，成片
    /// 里台词之间是数字静音。没出声音（Wan、没配音频 VAE）就没有这个文件，
    /// 调用方按文件在不在判。
    void generate_video(const VideoRequest& req,
                        const std::filesystem::path& raw_dest,
                        pipeline::CancelToken& tok,
                        const StepCallback& on_step);

private:
    SdContext() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 取配置的回调。**每次加载模型时现取**，不是注册时取一次。
///
/// 这条是有代价学来的：LLM 客户端当初在构造时把 settings.llm 存了下来，
/// 结果用户在设置页改了地址、接口回"已保存"，而请求还是发往老地址。
/// 模型文件这一路同样——改完 [models] 里的文件名之后不重启就不生效，
/// 而"不生效"的表现是加载出来的还是上一个模型，不报任何错。
using SettingsProvider = std::function<config::Settings()>;

/// **这一趟活按哪份配置装模型。**
///
/// 工作进程接活时，槽的 provider 读的是这台的 runtime 配置；而派活那部电影挑的
/// 档位（Edit 还是基础、哪个量化）只盖在任务自己那份 settings 上
/// （task_run.cpp 的 settings_for）。不盖到槽上的话，定妆图要基础权重、
/// 槽却按 runtime 里的 `image` 装了 Edit——2026-09-16 实测：L20 上
/// Qwen_Image-Q8_0 明明在盘上，一键出图跑的还是 Edit，日志里一行
/// "是图像编辑模型，而这一镜一张参考图都没有"。
///
/// **线程局部**：槽的 load 就在借槽的这条线程上同步跑，所以只要在
/// run_task_locally 这一层套一个作用域，load 看到的就是这份。别的线程
/// （比如另一边在借 LLM 槽时估这个槽的占用）看到的仍是 runtime 那份，
/// 那只是估算，无妨。
class ScopedTaskSettings {
public:
    explicit ScopedTaskSettings(const config::Settings& s);
    ~ScopedTaskSettings();
    ScopedTaskSettings(const ScopedTaskSettings&) = delete;
    ScopedTaskSettings& operator=(const ScopedTaskSettings&) = delete;

private:
    const config::Settings* prev_;
};

/// 当前线程上套着的那份；没有就是 nullptr。register_sd_slots 的 provider 先看它。
const config::Settings* task_settings_override();

/// 把 sd.cpp 的上下文注册到调度器的图像槽和视频槽上。
///
/// 注册之后调用方只管 `scheduler().acquire(Slot::Image)`，
/// 什么时候加载、什么时候为了腾地方被卸掉，由调度器决定。
///
/// **一个进程注册一次**，在启动时做。槽已经加载着的时候重新注册会抛异常
/// （新的 unload 会去卸一个不是它加载的东西），所以别放在每次开跑的路径上。
void register_sd_slots(SettingsProvider provider,
                       const models::HardwareProfile& profile);

/// 同上，配置固定不变的那种。测试和命令行用。
void register_sd_slots(const config::Settings& settings,
                       const models::HardwareProfile& profile);

/// 借图像槽，**并保证槽上装的是这个变体**（Edit 还是基础文生图）。
///
/// 直接 `scheduler().acquire(Slot::Image, …)` 的问题是：槽已经加载时调度器
/// 不会再调 load，于是要基础模型的那一步会拿到还挂在槽上的 Edit 权重。
/// 这条先看已装的是哪个，不对就卸掉再借。
///
/// `ModelRole::Video` 传进来按 Image 处理——图像槽上不该出现视频模型。
Lease acquire_image(ModelRole role, const Scheduler::AcquireOptions& opt);

/// 当前挂在图像槽 / 视频槽上的上下文。没加载时返回空。
///
/// 拿它之前要先 acquire 对应的槽，否则可能拿到一个正要被卸掉的。
std::shared_ptr<SdContext> current_image_context();
std::shared_ptr<SdContext> current_video_context();

/// 出片那一步的裸帧文件对应的 wav 路径（换个后缀）。纯路径算术，
/// 没链 sd.cpp 也在——sd_video.cpp 要靠它判"这一镜出没出声音"。
std::filesystem::path audio_path_for(const std::filesystem::path& raw_dest);

}  // namespace changji::infer
