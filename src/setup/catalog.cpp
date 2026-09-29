#include "setup/catalog.hpp"

#include "util/say.hpp"

#include <algorithm>
#include <cmath>

#include "config/model_patch.hpp"
#include "config/settings.hpp"

namespace changji::setup {

using json = nlohmann::json;

namespace {

double gb(std::uint64_t bytes) { return static_cast<double>(bytes) / 1e9; }

/// 权重要多大的卡才**常驻得下**（GB）。
///
/// **不自己算，去问引擎真正在用的那个函数。** `ModelsConfig::weights_for` /
/// `image_weights_for` 里那几个常数（视频的 14.6 GB 计算缓冲、图像的
/// 6.6 + 4 GB 再乘 0.9）都是 5090 上量出来的，而且改过好几轮。这张表要是
/// 照抄一份，公式一调它就开始骗人——而"界面说装得下、实际 OOM"这种错
/// 不会有任何报错，只会在跑到第 34 段时炸。
///
/// 反推的办法是扫：从 4 GB 往上按 0.5 GB 一档试，找它从 "cpu"
/// （＝权重全放内存）翻成组件规格的那一点。整张表就扫这么几十次，一次性的。
///
/// ⚠️ **这里问的是"要多大的卡"，所以不传 unified，将来也别传。**
/// 这张表是给人看的静态门槛（"这个模型要 24 GB 才常驻得下"），和跑它的
/// 是哪台机器无关。而且统一内存那一支里 `image_weights_for` 装不下时
/// 返回的是 "te=cpu,vae=cpu" 而不是 "cpu"——扫描的终止条件是"不等于 cpu"，
/// 传了 unified 的话第一档 4 GB 就命中，整张表的门槛全变成 4 GB。
double resident_vram(bool image, double model_gb) {
    config::ModelsConfig m;  // weights / image_weights 默认都是 "smart"
    for (double v = 4.0; v <= 200.0; v += 0.5) {
        const std::string w =
            image ? m.image_weights_for(v, model_gb) : m.weights_for(v, model_gb);
        if (w != "cpu") return v;
    }
    return 200.0;
}

/// 大模型（llama.cpp）要多大的卡。
///
/// 这条没有对应的引擎函数可问——llama.cpp 是整个载进显存的，没有
/// "放内存流式跑"那一档。所以只能估，锚点是这台机器上量到的一个数：
/// Qwen3-14B Q4_K_M 文件 9.0 GB，载进去占 15.4 GB（多出来的是 KV 缓存
/// 和上下文）。1.25 倍加 4 GB 对得上那个点（9×1.25+4 = 15.25）。
///
/// **估偏了的代价不对称**：估低了用户挑一个装不下的，llama.cpp 直接报错
/// 载不进去；估高了只是推荐保守一档。所以宁可往高了估。
double llm_vram(double model_gb) {
    // **式子只有一份**，在 ModelsConfig::llm_live_vram_gb——调度器判断
    // "显存够不够不用卸"用的也是它。抄两份迟早只改一处，然后模型窗说
    // 装得下、调度器说装不下，或者反过来。
    // 这里往上取到 0.5 GB 是给人看的：清单上写 15.5 比 15.25 干净。
    return std::ceil(config::ModelsConfig{}.llm_live_vram_gb(model_gb) * 2.0) / 2.0;
}

/// 这一档量化本身怎么样。**同一把尺子量所有家族**，省得每处各写一句
/// 而且互相打架。
std::string quant_note(const std::string& q) {
    if (q == "bf16" || q == "fp16" || q == "BF16" || q == "F16") {
        return "原始精度，无任何损失，体积最大。";
    }
    if (q == "fp8") return "8 位浮点，画质接近原始精度。";
    if (q == "int8_convrot") {
        return "8 位整数（ComfyUI 的 int8_tensorwise + convrot 格式），"
               "体积与 Q8_0 相当，画质接近原始精度。";
    }
    if (q == "fp8_scaled") {
        return "带缩放的 8 位浮点（ComfyUI 格式），体积与 Q8_0 相当。";
    }
    if (q == "Q8_0") return "最接近原始精度的量化版本，差异几乎无法察觉。";
    if (q == "Q6_K") return "画质与体积最均衡的版本，适合大多数设备。";
    if (q.rfind("Q5", 0) == 0) return "比 Q6_K 略小，差异需仔细对比才能察觉。";
    if (q == "Q4_K_M") return "常用的折中版本，细节有所减少，构图和动作基本保持。";
    if (q.rfind("Q4", 0) == 0) return "与 Q4_K_M 同级，体积略有差异。";
    if (q.rfind("Q3", 0) == 0) return "画质明显下降，仅建议在显存较小时使用。";
    if (q.rfind("Q2", 0) == 0) return "画质严重下降，仅建议用于流程测试。";
    return "";
}

/// id 用的小写形式。`Q4_K_M` → `q4_k_m`，点换成横杠。
std::string lower_id(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '.') c = '-';
    }
    return s;
}

/// 「这一组先不下」。每组都有一个，理由见 catalog.hpp。
Option none_option(const std::string& label, const std::string& note,
                   std::vector<std::pair<std::string, json>> settings = {}) {
    Option o;
    o.id = kNoneOption;
    o.label = label;
    o.family_note = note;
    // rank 为负 = 永远不会被挑成默认值。挑默认值那个函数还会额外跳过它，
    // 见 recommend()——小卡上它的门槛是 0，不跳的话会成为唯一"够得上"的一项。
    o.rank = -1;
    o.settings = std::move(settings);
    return o;
}

// ---------------------------------------------------------------------------
// 编剧模型：Qwen3 四个尺寸 × 五档量化
// ---------------------------------------------------------------------------
//
// 2026-09-14 随进程内后端一起摘掉、2026-09-19 一起接回来。字节数是 09-10
// 用 ?blobs=true 核过的那份，原样搬回。**换家族要重新核字节数**（文件头
// 第一条规则），别照着别处的表抄。

struct LlmSpec {
    const char* size;  // "14B"
    const char* quant;
    std::uint64_t bytes;
};

// 全部来自 Qwen 官方的 GGUF 仓库，2026-09-10 用 ?blobs=true 核过。
constexpr LlmSpec kLlm[] = {
    {"32B", "Q8_0", 34817718912ULL},   {"32B", "Q6_K", 26883306112ULL},
    {"32B", "Q5_K_M", 23214831232ULL}, {"32B", "Q5_0", 22635493024ULL},
    {"32B", "Q4_K_M", 19762149024ULL},
    {"14B", "Q8_0", 15698533728ULL},   {"14B", "Q6_K", 12121937248ULL},
    {"14B", "Q5_K_M", 10514569568ULL}, {"14B", "Q5_0", 10263894400ULL},
    {"14B", "Q4_K_M", 9001752960ULL},
    {"8B", "Q8_0", 8709518112ULL},     {"8B", "Q6_K", 6725899040ULL},
    {"8B", "Q5_K_M", 5851112224ULL},   {"8B", "Q5_0", 5720761152ULL},
    {"8B", "Q4_K_M", 5027783488ULL},
    {"4B", "Q8_0", 4280404704ULL},     {"4B", "Q6_K", 3306260704ULL},
    {"4B", "Q5_K_M", 2889513184ULL},   {"4B", "Q5_0", 2823710976ULL},
    {"4B", "Q4_K_M", 2497280256ULL},
};

/// 参数量越大写得越好，同尺寸里量化越高越好。rank 按这个排。
///
/// **尺寸的权重压过量化**：14B 的 Q4_K_M 写得比 8B 的 Q8_0 好。
/// 反过来排的话，24 GB 的卡会被推荐一个 8B Q8_0，而那台机器装得下 14B。
int llm_rank(const std::string& size, const std::string& quant) {
    const int by_size = size == "32B" ? 400 : size == "14B" ? 300
                        : size == "8B" ? 200 : 100;
    const int by_quant = quant == "Q8_0" ? 5 : quant == "Q6_K" ? 4
                         : quant == "Q5_K_M" ? 3 : quant == "Q5_0" ? 2 : 1;
    return by_size + by_quant;
}

std::string llm_family_note(const std::string& family) {
    if (family == "Qwen3-32B") {
        return "本机可运行的版本中写作质量最高。它与出图模型共用显存："
               "显存低于 40 GB 时，每次写正文都需要先卸载出图模型，"
               "完成后再重新加载。";
    }
    if (family == "Qwen3-14B") {
        return "已在 RTX 5090 上实测：Q4_K_M 占用显存 15.4 GB；出片时会被卸载，"
               "重新加载约需 4.6 秒。写正文时被质量检查退回的次数多于云端模型，"
               "多数可在修改稿中修正。";
    }
    if (family == "Qwen3-8B") {
        return "适合显存较小的设备。正文篇幅偏短，人物关系刻画较弱。";
    }
    return "对显卡几乎没有要求。仅建议用于流程测试，无法写出可用的正文。";
}

// ---------------------------------------------------------------------------
// 出片：MiniMax-H3
//
// **Wan 2.2 TI2V-5B 2026-09-17 从清单里去掉了**（用户要求）。它曾是最省显存
// 的一条路（8 GB 的卡也跑得动），但用户看过它出的成片，原话是「图生视频还是
// 太差了」——留在选择器里只会让人下三个 G 再得出同一个结论。
//
// **引擎那头没动**：sd_image / settings 里认 Wan 权重的代码都留着，已经配了
// Wan 的机器照跑不误，只是不再从这儿推荐和下载。
// ---------------------------------------------------------------------------

constexpr const char* kH3GgufRepo = "leejet/MiniMax-H3-GGUF";
/// unsloth 那份把**裁过的 fl2va** 量到了更低的几档（leejet 只到 Q4_K_M）。
/// 16 GB 的卡上要的就是这几档，见下面 kH3 表里那两条。
constexpr const char* kH3SmallRepo = "unsloth/MiniMax-H3-GGUF";
constexpr const char* kH3ComfyRepo = "Comfy-Org/MiniMax-H3";
constexpr const char* kH3LoraRepo = "larryvrh/MiniMax-H3-Turbo-Lora";
/// 完整版（33B）那一支的量化。leejet 只出了 Q4_K_M 一档，别的档只有这家有。
constexpr const char* kH3FullQuantRepo = "Abiray/MiniMax-H3-GGUF";

constexpr const char* kH3FamilyNote =
    "画面与立体声同时生成，动作真实感在同级模型中最佳。注意："
    "无论选择哪个版本，整套文件都至少需要下载 31 GB；"
    "许可协议禁止美国、欧盟、英国、韩国的创作者分发用它生成的视频。";

// **「完整」和「精简」不是高配低配，是"能不能拿去继续训练"。**
//
// 光看名字谁都会挑「完整」——用户 2026-09-17 就在问"是不是还有个低配版"，
// 而他找的那个就是「精简」，只是名字没告诉他这一点。他那台机器上跑的正是
// 「完整」，实测 124 秒一镜，白扛 26 GB。
//
// 官方 README 写得很清楚：H3-Omni-Transformer 是 33B，其中约 13B 在
// AdaLN 分支上，而"AdaLN 的调制输出可以预先算好缓存，**只做推理的部署
// 不需要载入这些参数**"。「精简」就是把那 13B 去掉的那一版。
//
// 对得上：完整 bf16 66.28 GB、精简 bf16 40.23 GB，比值 0.607；
// 20B/33B = 0.606。
constexpr const char* kH3FullNote =
    "⚠️ 出片不需要此版本。「完整」比「精简」"
    "多出的 13B 参数位于 AdaLN 分支，"
    "官方说明指出仅用于推理的部署无需加载这部分参数，"
    "选择此版本只会多占用二十多 GB。仅在继续训练该模型（微调、"
    "训练 LoRA）时才需要完整权重。";

constexpr const char* kH3PrunedNote =
    "出片推荐此版本：官方的仅推理版本，"
    "从 33B 中移除了仅在微调时使用的 13B AdaLN 分支，剩余 20B，"
    "体积减少约四成，出片所需的功能完整。";

struct H3Spec {
    const char* id;
    const char* family;  // "MiniMax-H3 完整" / "MiniMax-H3 精简"
    const char* quant;
    const char* path;    // 相对下面那个仓库前缀
    const char* repo;
    std::uint64_t bytes;
    int rank;
};

// **只收 leejet 的 GGUF 和 Comfy 的纯 bf16。**
//
// int8_convrot 和 fp8_scaled 那几份没收：前者是个专门的格式，后者带 scale
// 张量——sd.cpp 的加载器里没有一行处理 scaled，会按普通 fp8 读，不报错，
// 只是出来的东西全是垃圾（Qwen2.5-VL 的 fp8_scaled 上已经栽过一次）。
// 清单里只放能确定读得对的：读不对这件事没有任何报错。
// **rank 按"出片出来什么样"排，不按"哪一支更完整"。**
//
// 原来完整那一支的 rank 整段高于精简，2026-09-17 给完整补上量化档之后
// 当场出事：5090 上推荐落到了 `h3-full-q3_k_m`——一个严重退化的完整版，
// 压过高精度的 `h3-pruned-q6_k`。测试抓住了。
//
// 按上面 kH3FullNote 里那段官方说明，完整多出来的 13B 在推理时根本不加载，
// 所以**同一个量化级别上，完整并不更好，只是更大**。于是排法是：先按量化
// 级别（bf16 > Q8 > Q6 > Q5 > Q4 > Q3 > Q2），同一级别里精简在前。
constexpr H3Spec kH3[] = {
    // ---- 完整（33B）----
    {"h3-full-bf16", "MiniMax-H3 完整", "bf16",
     "diffusion_models/minimax_h3_fl2va_bf16.safetensors", kH3ComfyRepo,
     66280487368ULL, 98},
    {"h3-full-q8_0", "MiniMax-H3 完整", "Q8_0",
     "unet/MiniMax-H3-FL2VA-Q8_0.gguf", kH3FullQuantRepo, 36035216640ULL, 92},
    {"h3-full-int8_convrot", "MiniMax-H3 完整", "int8_convrot",
     "diffusion_models/minimax_h3_fl2va_int8_convrot.safetensors", kH3ComfyRepo,
     34038892334ULL, 91},
    {"h3-full-q6_k", "MiniMax-H3 完整", "Q6_K",
     "unet/MiniMax-H3-FL2VA-Q6_K.gguf", kH3FullQuantRepo, 28219050240ULL, 87},
    {"h3-full-q5_k_m", "MiniMax-H3 完整", "Q5_K_M",
     "unet/MiniMax-H3-FL2VA-Q5_K_M.gguf", kH3FullQuantRepo, 23887484192ULL, 83},
    {"h3-full-q5_0", "MiniMax-H3 完整", "Q5_0",
     "unet/MiniMax-H3-FL2VA-Q5_0.gguf", kH3FullQuantRepo, 22779297056ULL, 82},
    // leejet 的那一份比 Abiray 的 Q4_K_M（19864208217）小一个 G，是 sd.cpp
    // 作者自己出的，留着它当这一级，不收另一份同名的。
    {"h3-full-q4_k_m", "MiniMax-H3 完整", "Q4_K_M",
     "minimax_h3_fl2va-Q4_K_M.gguf", kH3GgufRepo, 18779848448ULL, 79},
    {"h3-full-q4_0", "MiniMax-H3 完整", "Q4_0",
     "unet/MiniMax-H3-FL2VA-Q4_0.gguf", kH3FullQuantRepo, 18639605024ULL, 78},
    {"h3-full-q3_k_m", "MiniMax-H3 完整", "Q3_K_M",
     "unet/MiniMax-H3-FL2VA-Q3_K_M.gguf", kH3FullQuantRepo, 15567048992ULL, 75},

    // ---- 精简（20B，只做推理）----
    {"h3-pruned-bf16", "MiniMax-H3 精简", "bf16",
     "diffusion_models/minimax_h3_fl2va_pruned_bf16.safetensors", kH3ComfyRepo,
     40225724176ULL, 100},
    {"h3-pruned-q8_0", "MiniMax-H3 精简", "Q8_0",
     "minimax_h3_fl2va_pruned-Q8_0.gguf", kH3SmallRepo, 21437786208ULL, 96},
    {"h3-pruned-int8_convrot", "MiniMax-H3 精简", "int8_convrot",
     "diffusion_models/minimax_h3_fl2va_pruned_int8_convrot.safetensors",
     kH3ComfyRepo, 20970379616ULL, 95},
    {"h3-pruned-fp8_scaled", "MiniMax-H3 精简", "fp8_scaled",
     "diffusion_models/minimax_h3_fl2va_pruned_fp8_scaled.safetensors",
     kH3ComfyRepo, 20958205608ULL, 94},
    {"h3-pruned-q6_k", "MiniMax-H3 精简", "Q6_K",
     "minimax_h3_fl2va_pruned-Q6_K.gguf", kH3SmallRepo, 16586784864ULL, 88},
    {"h3-pruned-q5_0", "MiniMax-H3 精简", "Q5_0",
     "minimax_h3_fl2va_pruned-Q5_0.gguf", kH3SmallRepo, 13923170400ULL, 84},
    {"h3-pruned-q4_k_m", "MiniMax-H3 精简", "Q4_K_M",
     "minimax_h3_fl2va_pruned-Q4_K_M.gguf", kH3GgufRepo, 11420663904ULL, 80},
    // ---- 16 GB 的卡 ----
    //
    // ⚠️ **H3 没有任何一档能在 16 GB 上常驻显存**：`resident_vram` 算出来的
    // 门槛是模型大小 + 15 GB 上下（那 15 GB 是视频解码和采样缓冲）。16 GB
    // 上走的一定是"权重放内存"那条，每一步从内存往显卡搬权重。
    //
    // 这两档的意义是**搬的东西少一半**：6.3 GiB 比 10.6 GiB 每步少搬四成多，
    // 而 PCIe 正是那条路的瓶颈（settings.hpp 里那段实测：权重放内存时 GPU
    // 利用率 18%，常驻是 82%）。
    //
    // **不收 unsloth 的 UD-*_XL**：那是它自己的动态混合精度方案，没法确认
    // sd.cpp 读得对——而读不对这件事没有任何报错。
    {"h3-pruned-q3_k", "MiniMax-H3 精简", "Q3_K",
     "minimax_h3_fl2va_pruned-Q3_K.gguf", kH3SmallRepo, 8759328864ULL, 76},
    {"h3-pruned-q2_k", "MiniMax-H3 精简", "Q2_K",
     "minimax_h3_fl2va_pruned-Q2_K.gguf", kH3SmallRepo, 6724190304ULL, 72},
};


// ---------------------------------------------------------------------------
// 出首帧：Qwen-Image
// ---------------------------------------------------------------------------

constexpr const char* kQwenImageFamilyNote =
    "首帧是跨镜头一致性的基准，也是出片的起始画面："
    "首帧模糊会导致后续每一镜都模糊，且不会报错。"
    "因此建议选择较高的版本：在 RTX 5090 上实测，"
    "Q6_K 权重常驻显存时每张约 35 秒，放在内存中则慢约五倍。"
    "此系列为图像编辑模型：角色三视图和空景图会作为参考图一同输入，"
    "以保证同一角色在各镜头中外貌一致。"
    "没有任何参考图的镜头会退化为文生图，效果无法使用，"
    "因此需要先生成定妆照和参考图。";

/// 首帧那一族的仓库。**2509 版**：多参考图是它加的，而这套流水线一镜要喂
/// 好几张（在场的每个角色一张三视图 + 这个场景的空景图，见
/// stages/prompt_compose.cpp 里那个 refs）。初版 Edit 只收一张。
constexpr const char* kImageRepo = "QuantStack/Qwen-Image-Edit-2509-GGUF";

struct ImageSpec {
    const char* quant;
    const char* file;
    std::uint64_t bytes;
    int rank;
};

/// 字节数是 2026-09-15 在 HuggingFace 和魔搭上各查一遍对过的，两边一字不差
/// （FileSpec::bytes 上那条规矩：两个源必须一致，否则"下完了没有"的判据就废了）。
/// **这一族没有 BF16 也没有 fp8**——上游只放了 GGUF 这一梯队。
constexpr ImageSpec kImage[] = {
    {"Q8_0", "Qwen-Image-Edit-2509-Q8_0.gguf", 21761817120ULL, 15},
    {"Q6_K", "Qwen-Image-Edit-2509-Q6_K.gguf", 16824990240ULL, 13},
    {"Q5_1", "Qwen-Image-Edit-2509-Q5_1.gguf", 15391717920ULL, 12},
    {"Q5_K_M", "Qwen-Image-Edit-2509-Q5_K_M.gguf", 14934899232ULL, 11},
    {"Q5_0", "Qwen-Image-Edit-2509-Q5_0.gguf", 14400813600ULL, 10},
    {"Q5_K_S", "Qwen-Image-Edit-2509-Q5_K_S.gguf", 14117698080ULL, 9},
    {"Q4_K_M", "Qwen-Image-Edit-2509-Q4_K_M.gguf", 13065746976ULL, 8},
    {"Q4_1", "Qwen-Image-Edit-2509-Q4_1.gguf", 12886145568ULL, 7},
    {"Q4_K_S", "Qwen-Image-Edit-2509-Q4_K_S.gguf", 12204309024ULL, 6},
    {"Q4_0", "Qwen-Image-Edit-2509-Q4_0.gguf", 11928271392ULL, 5},
    {"Q3_K_M", "Qwen-Image-Edit-2509-Q3_K_M.gguf", 9764502048ULL, 4},
    {"Q3_K_S", "Qwen-Image-Edit-2509-Q3_K_S.gguf", 9037543968ULL, 3},
    {"Q2_K", "Qwen-Image-Edit-2509-Q2_K.gguf", 7147452960ULL, 2},
};

/// 定妆和空景那一族的仓库：**基础版 Qwen-Image，不带 edit**。
///
/// 为什么要单独一族，见 config/settings.hpp 上 `image_base` 那段：三视图和
/// 空景图是从纯文字生成的（`ref_gen.cpp` 一张参考图都不传），而上面那一族
/// 是图像**编辑**模型，自己的说明里就写着「没有任何参考图的镜头会退化成
/// 文生图，那时候它出的东西不能看」。
///
/// **和 Edit 族同一个发布者、同样 13 档**，落盘名是 `Qwen_Image-*.gguf`
/// ——名字里没有 "edit"，所以 `accepts_reference_images()` 会正确判定它
/// 不收参考图（那条判据认的就是文件名里那四个字母）。
constexpr const char* kImageBaseRepo = "QuantStack/Qwen-Image-GGUF";

/// 字节数 2026-09-15 在 HuggingFace 和魔搭上各查一遍对过，两边一字不差。
/// **不要照抄上面 Edit 那张表**：同架构同量化多数档位确实一样，但 Q4_0
/// （11852773920 vs 11928271392）和 Q3_K_M（9679567392 vs 9764502048）
/// 这几档不一样，抄了就会让"下完了没有"的判据永远不成立。
constexpr ImageSpec kImageBase[] = {
    {"Q8_0", "Qwen_Image-Q8_0.gguf", 21761817120ULL, 15},
    {"Q6_K", "Qwen_Image-Q6_K.gguf", 16824990240ULL, 13},
    {"Q5_1", "Qwen_Image-Q5_1.gguf", 15391717920ULL, 12},
    {"Q5_K_M", "Qwen_Image-Q5_K_M.gguf", 14934899232ULL, 11},
    {"Q5_0", "Qwen_Image-Q5_0.gguf", 14400813600ULL, 10},
    {"Q5_K_S", "Qwen_Image-Q5_K_S.gguf", 14117698080ULL, 9},
    {"Q4_K_M", "Qwen_Image-Q4_K_M.gguf", 13065746976ULL, 8},
    {"Q4_1", "Qwen_Image-Q4_1.gguf", 12843678240ULL, 7},
    {"Q4_K_S", "Qwen_Image-Q4_K_S.gguf", 12140608032ULL, 6},
    {"Q4_0", "Qwen_Image-Q4_0.gguf", 11852773920ULL, 5},
    {"Q3_K_M", "Qwen_Image-Q3_K_M.gguf", 9679567392ULL, 4},
    {"Q3_K_S", "Qwen_Image-Q3_K_S.gguf", 8952609312ULL, 3},
    {"Q2_K", "Qwen_Image-Q2_K.gguf", 7062518304ULL, 2},
};

constexpr const char* kQwenImageBaseFamilyNote =
    "角色三视图和空景图由文字描述直接生成，没有可供编辑的参考图，"
    "属于文生图，需要基础权重。首帧系列为图像编辑模型，"
    "用它执行此步骤会出现其说明中所述的退化情况。"
    "此系列与首帧系列共用 VAE 和文本编码器（同为 Qwen2.5-VL），"
    "只需额外下载一份扩散权重。不下载也可以运行："
    "定妆和空景将沿用首帧模型。";

/// Qwen-Image 的文本编码器（Qwen2.5-VL-7B）按扩散模型那一档配。
///
/// **别用 Comfy 那份 fp8_scaled**：那种格式带 scale 张量，sd.cpp 的加载器里
/// 没有一行处理 scaled，会按普通 fp8 读——不报错，只是文本条件全是垃圾。
/// 上游 docs/qwen_image.md 用的就是下面这份 Q8_0。
FileSpec image_encoder(double diff_gb) {
    if (diff_gb >= 20.0) {
        return {"qwen_2.5_vl_7b_bf16.safetensors", "Comfy-Org/Qwen-Image_ComfyUI",
                "split_files/text_encoders/qwen_2.5_vl_7b.safetensors",
                16584415576ULL, "image_text_encoder",
                "文本编码器 Qwen2.5-VL-7B（bf16 原版）。权重常驻内存"};
    }
    if (diff_gb >= 10.0) {
        return {"Qwen2.5-VL-7B-Instruct-Q8_0.gguf",
                "unsloth/Qwen2.5-VL-7B-Instruct-GGUF",
                "Qwen2.5-VL-7B-Instruct-Q8_0.gguf",
                8098524032ULL, "image_text_encoder",
                "文本编码器 Qwen2.5-VL-7B（Q8_0）。权重常驻内存"};
    }
    return {"Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
            "unsloth/Qwen2.5-VL-7B-Instruct-GGUF",
            "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
            4683072384ULL, "image_text_encoder",
            "文本编码器 Qwen2.5-VL-7B（Q4_K_M）。权重常驻内存"};
}

// ---------------------------------------------------------------------------
// 配音：Qwen3-TTS
// ---------------------------------------------------------------------------

constexpr const char* kTtsRepo = "ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF";
constexpr const char* kTtsFamilyNote =
    "在引擎进程内运行，无需另外启动服务。此组是四组中体积最小的，"
    "几乎不占用显存（1.7B 模型，任何显卡均可容纳）。";

struct TtsSpec {
    const char* quant;
    const char* backbone;
    std::uint64_t backbone_bytes;
    const char* mmproj;
    std::uint64_t mmproj_bytes;
    int rank;
};

constexpr TtsSpec kTts[] = {
    {"bf16", "Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf", 3472593760ULL,
     "mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf", 669081472ULL, 30},
    {"Q8_0", "Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf", 1847874400ULL,
     "mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf", 446422912ULL, 20},
    // Q4_K_M 那一档没有配套的 mmproj，配 Q8_0 那份——解码器本身很小，
    // 省那 200 MB 没有意义，而**拿错了 mtmd 会报"这份 mmproj 不支持音频生成"**。
    {"Q4_K_M", "Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf", 1035965280ULL,
     "mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf", 446422912ULL, 10},
};

// ---------------------------------------------------------------------------

std::vector<Group> build() {
    std::vector<Group> gs;

    // ---------------- 编剧 ----------------
    {
        Group g;
        g.key = "llm";
        g.title = "编剧模型";
        g.purpose = "用于编写剧本大纲、拆分分镜和提取角色，是整条流水线的第一步。";
        g.required = true;
        g.owned_roles = {"llm"};

        // **本地这一族 2026-09-19 接回来了。** 2026-09-14 摘掉的理由是进程内
        // 后端删了、下了也没东西用；现在后端回来了（llm::LocalClient），
        // 而且这是产品要给别人用的——默认那档得是装上就能跑的，不能是一个
        // 要人自己去领密钥的云端。
        for (const auto& spec : kLlm) {
            const std::string size = spec.size;
            const std::string quant = spec.quant;
            const std::string file = "Qwen3-" + size + "-" + quant + ".gguf";
            Option o;
            o.id = lower_id("qwen3-" + size + "-" + quant);
            o.family = "Qwen3-" + size;
            o.label = "Qwen3-" + size + " · " + quant;
            o.quant = quant;
            o.family_note = llm_family_note(o.family);
            o.note = quant_note(quant);
            // **和出图出片不一样：这条路没有"权重放内存"那一档。**
            // llama.cpp 是整个载进显存的，装不下就是载不进去
            // （register_llm_slot 那头会退回 CPU 跑，慢但出得来东西）。
            o.min_vram_gb = llm_vram(gb(spec.bytes));
            o.rank = llm_rank(size, quant);
            o.files.push_back({"llm/" + file, "Qwen/Qwen3-" + size + "-GGUF", file,
                               spec.bytes, "llm", "编剧模型权重"});
            // 下了模型就该用进程内那条路，否则下完还得自己去模型窗里切一下，
            // 而不切的表现是「写正文」按钮报连不上 127.0.0.1:11434。
            o.settings.push_back({"llm.backend", "local"});
            g.options.push_back(std::move(o));
        }

        // **云端那一项还在，但不再是默认。** rank 压到本地最小那档（4B
        // Q4_K_M = 101）之下，只有一档本地的都装不下——没显卡、或者六成
        // 预算不到 4B 那 7 GB——才轮到它。想要写得更好的人照样点得到。
        //
        // 它不是 kNoneOption——「什么都不装」和「装好了，用这个」是两件事。
        // 前者在 recommend() 里被跳过（那是这一页要解决的状态），
        // 后者是一个完整可用的选择，只差一个密钥。
        {
            Option o;
            o.id = "zhipu-free";
            o.family = "智谱 GLM（云端 · 免费版）";
            o.label = "智谱 · glm-4.7-flash（免费）";
            // **要紧的话写在 note 里，不是 family_note。** 界面上只显示
            // 选中那一档的 note（ModelPicker.vue 里那一句「家族那段话不
            // 摆出来」），family_note 收集了但一个地方都没渲染。
            // **2026-09-14 从 200 字砍到一句。** 原来那段有三处叫人
            // 「去设置页换模型」「填进设置页的大模型那一节」——而模型名和
            // 密钥现在就在点开这一项的那个弹窗里改（用户：「在线模型的名字
            // 是不是也应该在这设置」）。一段说明在一个能直接改的界面上
            // 指着别处，是这一页当时最该删的一条。
            //
            // 哪个模型写得好也不在这儿说了：那是**挑模型**的依据，
            // 现在挂在模型下拉每一项后面（llm::known_models 那本小抄），
            // 挑的时候一眼看得到，不用记。
            o.note =
                "无需下载权重，剧本由云端生成，国内可直接访问。"
                "优点是显卡可全部用于出图和出片；缺点是剧本内容需上传至云端，"
                "断网时无法编剧。密钥可在 bigmodel.cn 控制台申请。";
            o.family_note = o.note;
            o.min_vram_gb = 0.0;
            o.rank = 50;
            o.settings = {
                {"llm.backend", "remote"},
                {"llm.base_url", "https://open.bigmodel.cn/api/paas/v4"},
                {"llm.model", "glm-4.7-flash"}};
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 使用其他外部服务",
            "剧本由其他机器或云端服务生成（如 Ollama、vLLM、DeepSeek）。"
            "请在「设置 ▸ 大模型」中添加服务并填写地址和密钥，"
            "再通过该服务的「选择模型…」选择模型。",
            {{"llm.backend", "remote"}}));

        gs.push_back(std::move(g));
    }

    // ---------------- 出片 ----------------
    {
        Group g;
        g.key = "video";
        g.title = "出片模型（图生视频）";
        g.purpose = "将每一镜的首帧生成为视频。此组体积最大，耗时也最长。";
        g.required = true;
        // **八个键一起管。** 换家族时没用到的必须清空，尤其是 video_lora——
        // 它的默认值指着 H3 的 Turbo LoRA，留着的话会被挂到 Wan 上。
        g.owned_roles = {"video",     "video_high_noise", "video_vae",
                         "video_text_encoder", "video_llm", "video_llm_vision",
                         "video_audio_vae",    "video_lora"};

        for (const auto& spec : kH3) {
            Option o;
            o.id = spec.id;
            o.family = spec.family;
            o.label = std::string(spec.family) + " · " + spec.quant;
            o.quant = spec.quant;
            // 家族说明分两份：光看「完整 / 精简」这两个名字，人一定会挑
            // 前者，而出片这件事上前者只是多占二十多 GB。见上面那两段。
            o.family_note =
                std::string(kH3FamilyNote) +
                (std::string(spec.family).find("精简") != std::string::npos
                     ? kH3PrunedNote
                     : kH3FullNote);
            o.note = quant_note(spec.quant);
            o.min_vram_gb = resident_vram(false, gb(spec.bytes));
            o.rank = spec.rank;

            const std::string path = spec.path;
            const auto slash = path.rfind('/');
            const std::string file =
                slash == std::string::npos ? path : path.substr(slash + 1);
            o.files.push_back({file, spec.repo, path, spec.bytes, "video",
                               "扩散模型，画面与立体声同时生成"});
            // ---- 编码器：**这一项一个人就占四成多，所以给选** ----
            //
            // 43.6 GB 那一档里，编码器 18.2 GB、扩散模型 18.8 GB——两边
            // 一样重。小一档 13.1 GB，省五个 G。在这之前它是按扩散模型
            // 大小自动挑的，用户连名字都看不见（用户 2026-09-17：「我要选」）。
            //
            // 默认还是按扩散模型那一档配：挑了大模型的人多半不想在编码器
            // 上省。它常驻内存，不影响显存门槛。
            const bool big = gb(spec.bytes) >= 15.0;
            const FileSpec enc_big{
                "qwen3vl_32b_minimax_h3-Q4_K_M.gguf", kH3GgufRepo,
                "qwen3vl_32b_minimax_h3-Q4_K_M.gguf", 18218065024ULL,
                "video_llm",
                "文本编码器 Q4_K_M（精简版 Qwen3-VL-32B），每镜只运行一次，"
                "权重常驻内存"};
            const FileSpec enc_small{
                "qwen3vl_32b_minimax_h3-Q2_K_M.gguf", kH3GgufRepo,
                "qwen3vl_32b_minimax_h3-Q2_K_M.gguf", 13102161024ULL,
                "video_llm",
                "文本编码器 Q2_K_M，比 Q4_K_M 节省约 5 GB，对提示词的理解精度稍低"};
            o.files.push_back(big ? enc_big : enc_small);
            o.alts.push_back({"video_llm", "文本编码器",
                              big ? std::vector<FileSpec>{enc_big, enc_small}
                                  : std::vector<FileSpec>{enc_small, enc_big}});

            // ---- 视频 VAE：int8 那份省两个多 G ----
            //
            // int8_convrot 走的是 ComfyUI 那套 `int8_tensorwise` + convrot，
            // 这版 sd.cpp 认（safetensors_io.cpp 的 read_comfy_quant_config）。
            const FileSpec vae_fp16{
                "minimax_h3_video_vae_fp16.safetensors", kH3ComfyRepo,
                "vae/minimax_h3_video_vae_fp16.safetensors", 5207808496ULL,
                "video_vae", "视频 VAE（fp16 原版）"};
            const FileSpec vae_int8{
                "minimax_h3_video_vae_int8_convrot.safetensors", kH3ComfyRepo,
                "vae/minimax_h3_video_vae_int8_convrot.safetensors",
                2811065184ULL, "video_vae",
                "视频 VAE（int8_convrot），节省 2 GB 以上，解码画面略显粗糙"};
            o.files.push_back(vae_fp16);
            o.alts.push_back({"video_vae", "视频 VAE", {vae_fp16, vae_int8}});
            o.files.push_back({"minimax_h3_audio_vae_fp32.safetensors",
                               kH3ComfyRepo,
                               "vae/minimax_h3_audio_vae_fp32.safetensors",
                               605254808ULL, "video_audio_vae",
                               "音频 VAE，缺少此文件时生成的影片没有音轨"});
            o.files.push_back(
                {"loras/minimax_h3_turbo_v4_step600_ema.safetensors", kH3LoraRepo,
                 "minimax_h3_turbo_v4_step600_ema.safetensors",
                 779849816ULL, "video_lora",
                 "Turbo 蒸馏：采样步数从 28 步降至 6 步，"
                 "实测单镜耗时从 242 秒降至 124 秒"});

            // H3 的四个旋钮和 Wan 完全不同，**填错了四处都不报错**：
            // 编码器走 video_llm 不是 video_text_encoder（上面已经这么填了）、
            // cfg 是 1.0 不是 6.0、随机数发生器要 cpu、flow_shift 要 12 不是 3。
            // 错了的表现是出来的片和提示词没关系，人会先去怀疑提示词。
            o.settings.push_back({"models.video_rng", "cpu"});
            o.settings.push_back({"models.video_cfg", 1.0});
            // **0 = 自动**，也就是让 sd.cpp 按架构给 H3 那个 12。
            // 这一条以前漏了，于是从 Wan 换过来的人配置里留着 Wan 的 3.0，
            // 每一镜都用错的 time-shift 跑。写 0 而不是写 12，是为了上游
            // 哪天改了这个数我们能跟上；也把旧配置里那个 3.0 洗掉。
            o.settings.push_back({"models.video_flow_shift", 0.0});
            o.settings.push_back({"models.video_lora_strength", 1.0});
            o.settings.push_back({"models.video_lora_tiers", "both"});
            // **步数交给引擎自己算，这里一定要写 0（＝没填）。**
            //
            // 挂了 Turbo 就该跑 6 步，但那件事 `config::effective_spec` 已经
            // 做了——它看 video_lora 那个文件在不在，在就把出片步数压到 6。
            // 这里再写死一个 6 的后果是**首帧跟着糊掉**：`[tiers].final_steps`
            // 会落进档位表（见 Runtime::profile），而首帧的步数取的正是档位表
            // 那个数（effective_spec 里 frame_steps = table_final_steps）。
            // Turbo 那个 LoRA 只挂在视频模型上，出图那一步没有它，6 步就是
            // 裸跑 6 步。首帧是跨镜头一致性的锚点，糊了后面每一镜都糊。
            //
            // 实测踩到过（2026-09-10，就在这一版上）：设置页从
            // 「出片 6 Turbo · 首帧 30」变成了「出片 6 Turbo · 首帧 6」。
            o.settings.push_back({"tiers.final_steps", 0});
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "暂不下载",
            "跳过此组将无法出片，流程只能进行到分镜。"));

        gs.push_back(std::move(g));
    }

    // ---------------- 出首帧 ----------------
    {
        Group g;
        g.key = "image";
        g.title = "首帧模型（图像编辑）";
        g.purpose =
            "每一镜先生成一张首帧，再由首帧生成视频。"
            "首帧是跨镜头一致性的基准，首帧模糊会导致后续每一镜都模糊。"
            "此系列支持参考图：出场角色的三视图和场景的空景图会一同输入，"
            "使人物和场景在各镜头中保持一致。";
        g.required = true;
        g.owned_roles = {"image", "image_vae", "image_text_encoder",
                         "image_text_encoder_vision"};

        const FileSpec vae{
            "qwen_image_vae.safetensors", "Comfy-Org/Qwen-Image_ComfyUI",
            "split_files/vae/qwen_image_vae.safetensors",
            253806246ULL, "image_vae",
            "VAE，不能与视频模型共用：Wan 与 Qwen-Image 的 VAE 不同，"
            "误用时不会报错，但生成的图像与提示词无关"};

        /// 文本编码器的视觉塔。
        ///
        /// **2509 起非它不可**，而且不带也不报错：sd.cpp 只在日志里说一句
        /// "no vision weights detected, vision disabled" 然后照常跑，参考图
        /// 只剩 VAE 潜空间那一半进 DiT，出来的图和参考对不上。
        /// `ModelsConfig::validate` 为这件事专门留了一条校验（认文件名里的
        /// 2509/2511），这一组不把它一起下下来的话，装完存一下就是那条红字。
        ///
        /// 它在**初版 Edit 那个仓库**下面——2509 那个仓库只放了扩散权重。
        /// 视觉塔是 Qwen2.5-VL-7B 自己的那一份，两个版本共用。
        const FileSpec vision{
            "Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf",
            "QuantStack/Qwen-Image-Edit-GGUF",
            "mmproj/Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf",
            1354163040ULL, "image_text_encoder_vision",
            "文本编码器的视觉模块（mmproj），缺少时参考图只能部分生效，"
            "且不会报错"};

        for (const auto& spec : kImage) {
            Option o;
            o.id = lower_id(std::string("qwen-image-edit-2509-") + spec.quant);
            o.family = "Qwen-Image-Edit 2509";
            o.label = std::string("Qwen-Image-Edit 2509 · ") + spec.quant;
            o.quant = spec.quant;
            o.family_note = kQwenImageFamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = resident_vram(true, gb(spec.bytes));
            o.rank = spec.rank;

            // 落盘名就用仓库里那个名字：`accepts_reference_images` 认的是
            // 文件名里的 "edit"，而 validate 那条认的是 "2509"——两条都靠
            // 这一串字。改名字等于把参考图这条路悄悄关掉。
            o.files.push_back({spec.file, kImageRepo, spec.file, spec.bytes,
                               "image", "扩散模型"});
            o.files.push_back(vae);
            o.files.push_back(image_encoder(gb(spec.bytes)));
            o.files.push_back(vision);
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "暂不下载",
            "跳过此组将无法生成首帧，出片也将缺少起始画面。"));

        gs.push_back(std::move(g));
    }

    // ---------------- 定妆和空景：基础 Qwen-Image ----------------
    //
    // **只管一个键。** 这一组和上面那一组共用 VAE 和文本编码器（同一个
    // Qwen2.5-VL，上面那组已经下了），所以 owned_roles 里只有 image_base
    // ——多写一个键的后果是"选了基础版"会把上面那组写好的 VAE 路径清掉
    // （见本文件头上第三条：换组要清空上一组的键，每个键都写）。
    {
        Group g;
        g.key = "image_base";
        g.title = "定妆和空景模型（文生图）";
        g.purpose =
            "根据文字描述生成角色三视图和空景图。它们是生成首帧时的参考图，"
            "决定人物和场景能否在各镜头中保持一致。";
        g.required = false;
        g.owned_roles = {"image_base"};

        for (const auto& spec : kImageBase) {
            Option o;
            o.id = lower_id(std::string("qwen-image-") + spec.quant);
            o.family = "Qwen-Image 基础版";
            o.label = std::string("Qwen-Image · ") + spec.quant;
            o.quant = spec.quant;
            o.family_note = kQwenImageBaseFamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = resident_vram(true, gb(spec.bytes));
            o.rank = spec.rank;
            // 落盘名照仓库那个（`Qwen_Image-*`）：`accepts_reference_images`
            // 认的是文件名里有没有 "edit"，改名就等于把这一族伪装成 Edit。
            o.files.push_back({spec.file, kImageBaseRepo, spec.file, spec.bytes,
                               "image_base", "扩散模型（基础版，文生图）"});
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 使用首帧模型代替",
            "定妆和空景将使用图像编辑模型进行文生图。可以出图，"
            "但按照该系列的说明，这种方式的效果无法使用。可节省一份权重，"
            "但会牺牲画质。"));

        gs.push_back(std::move(g));
    }

    // ---------------- 配音 ----------------
    {
        Group g;
        g.key = "tts";
        g.title = "配音模型";
        g.purpose = "为台词生成语音。装配成片时按台词时长对齐镜头。";
        // **非必需**：没有它整集是静音的，但剧本、分镜、画面这条路照样走得通。
        // 卡在这儿不让人进首页，等于因为一个 4 GB 的模型把整个程序锁住。
        g.required = false;
        g.owned_roles = {"tts", "tts_decoder"};

        for (const auto& spec : kTts) {
            Option o;
            o.id = lower_id(std::string("qwen3-tts-") + spec.quant);
            o.family = "Qwen3-TTS 12Hz 1.7B";
            o.label = std::string("Qwen3-TTS 12Hz 1.7B · ") + spec.quant;
            o.quant = spec.quant;
            o.family_note = kTtsFamilyNote;
            o.note = quant_note(spec.quant);
            o.min_vram_gb = llm_vram(gb(spec.backbone_bytes));
            o.rank = spec.rank;
            o.files.push_back({spec.backbone, kTtsRepo, spec.backbone,
                               spec.backbone_bytes, "tts", "主干模型（talker）"});
            o.files.push_back({spec.mmproj, kTtsRepo, spec.mmproj,
                               spec.mmproj_bytes, "tts_decoder",
                               "解码器，将码本还原为波形"});
            o.settings.push_back({"tts.backend", "local"});
            g.options.push_back(std::move(o));
        }

        g.options.push_back(none_option(
            "不下载 · 暂不配音",
            // 补下的地方指到那一类（2026-09-28 起设置分九类，下模型在「模型文件」里）
            "剧本、分镜和画面正常生成，但没有配音。"
            "可随时在「设置 ▸ 模型文件」中下载。"));

        gs.push_back(std::move(g));
    }

    return gs;
}

}  // namespace

std::uint64_t Option::total_bytes() const {
    std::uint64_t n = 0;
    for (const auto& f : files) n += f.bytes;
    return n;
}

const Option* Group::find(const std::string& option_id) const {
    for (const auto& o : options) {
        if (o.id == option_id) return &o;
    }
    return nullptr;
}

std::vector<std::string> label_parts(const std::string& label) {
    if (label.empty()) return {};
    // 表里有整条就是它自己一段。
    if (i18n::say(label) != label) return {label};
    static const std::string kSep = " · ";
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const auto k = label.find(kSep, at);
        out.push_back(label.substr(at, k == std::string::npos ? k : k - at));
        if (k == std::string::npos) break;
        at = k + kSep.size();
    }
    return out;
}

std::string say_label(const std::string& label) {
    const auto parts = label_parts(label);
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += " · ";
        out += i18n::say(parts[i]);
    }
    return out;
}

const std::vector<Group>& catalog() {
    static const std::vector<Group> gs = build();
    return gs;
}

std::map<std::string, std::string> recommend(double vram_gb) {
    std::map<std::string, std::string> out;
    for (const auto& g : catalog()) {
        // **编剧模型的预算只给六成。**
        //
        // 它和出图出片模型共用这张卡。三者不同时跑（调度器会把上一个卸掉），
        // 所以"装不装得下"看的是单个；但**每次来回都要重载一遍**，而重载
        // 的代价随模型大小涨——5090 上实测 14B Q4_K_M 重载一次 4.6 秒，
        // 32B 只会更久，而写剧本和出图在一章里要来回切几十次。
        //
        // 挑推荐值时因此往小了留一档。**这只影响默认值**：用户想用 32B
        // 照样点得动，那一档的显存门槛也照实显示。
        const double budget = g.key == "llm" ? vram_gb * 0.6 : vram_gb;
        const Option* best = nullptr;
        for (const auto& o : g.options) {
            // **「不下载」不参与挑选。** 它的门槛是 0，小卡上会是唯一
            // 够得上的一项——于是推荐出来的默认值变成"什么都不装"，
            // 而那正是这一页要解决的状态。
            if (o.id == kNoneOption) continue;
            if (o.min_vram_gb > budget) continue;
            if (best == nullptr || o.rank > best->rank) best = &o;
        }
        if (best == nullptr) {
            // 一个都够不上（比如探不到显卡、按 12 GB 估的机器碰上一组
            // 全是大模型）。**不能留空**——界面上"没有默认值"等于让用户
            // 自己去猜。挑门槛最低的那个，代价写在 note 里。
            for (const auto& o : g.options) {
                if (o.id == kNoneOption) continue;
                if (best == nullptr || o.min_vram_gb < best->min_vram_gb) best = &o;
            }
        }
        if (best != nullptr) out[g.key] = best->id;
    }
    return out;
}

std::string alt_key(const std::string& group_key, const std::string& role) {
    return group_key + "/" + role;
}

std::string type_dir(const std::string& group_key) {
    if (group_key == "image_base") return "image";
    // 另外四组的键本身就是英文的类型名。以后加一组，键就是它的目录——
    // 不在这儿另起一张对照表，两张表迟早对不上。
    return group_key;
}

std::string download_rel(const std::string& group_key, const FileSpec& f) {
    const auto slash = f.name.find_last_of('/');
    const std::string file = slash == std::string::npos ? f.name : f.name.substr(slash + 1);
    return type_dir(group_key) + "/" + file;
}

std::vector<FileSpec> effective_files(
    const std::string& group_key, const Option& o,
    const std::map<std::string, std::string>& selections) {
    if (o.alts.empty() || selections.empty()) return o.files;
    std::vector<FileSpec> out = o.files;
    for (const auto& alt : o.alts) {
        const auto it = selections.find(alt_key(group_key, alt.role));
        if (it == selections.end()) continue;
        // 按文件名认。**不认识就当没挑**——老界面提交一个已经换掉的名字时，
        // 用默认那份比整组不写强（同 config_patch 里对陌生 id 的处理）。
        const auto pick = std::find_if(
            alt.choices.begin(), alt.choices.end(),
            [&it](const FileSpec& f) { return f.name == it->second; });
        if (pick == alt.choices.end()) continue;
        for (auto& f : out) {
            if (f.role == alt.role) f = *pick;
        }
    }
    return out;
}

json config_patch(const std::map<std::string, std::string>& selections) {
    json patch = json::object();

    // 键形如 "models.video_rng"，拆成 {"models": {"video_rng": …}}。
    // 不带点的当顶层标量（vram_gb_override 那种就不在任何小节里）。
    const auto put = [&patch](const std::string& dotted, const json& value) {
        const auto dot = dotted.find('.');
        if (dot == std::string::npos) {
            patch[dotted] = value;
            return;
        }
        const std::string section = dotted.substr(0, dot);
        const std::string key = dotted.substr(dot + 1);
        if (!patch.contains(section) || !patch[section].is_object()) {
            patch[section] = json::object();
        }
        patch[section][key] = value;
    };

    for (const auto& g : catalog()) {
        const auto it = selections.find(g.key);
        if (it == selections.end()) continue;
        const Option* opt = g.find(it->second);
        // 认不出的 id 就跳过这一组。老界面提交一个已经删掉的选项时，
        // 少写一组比整个请求失败好。
        if (opt == nullptr) continue;

        for (const auto& [key, value] : opt->settings) put(key, value);

        // **不下任何文件的选项一律不动 owned_roles。**
        //
        // 判据是「有没有文件」而不是「是不是 kNoneOption」：走云端那一项
        // 也一个文件都不下，而把 models.llm 清空的话，用户以后想切回本地
        // 就得重新去找他早就下好的那个权重叫什么名字——盘上还在，配置里
        // 没了，而界面上只会说"没选模型"。
        if (opt->files.empty()) continue;

        // **每个键都写。** 没用到的写空串，否则上一次选的模型会留在配置里
        // 被当成这一次的一部分——video_lora 就是这么栽的。
        std::map<std::string, std::string> filled;
        // **走 effective_files，不是 opt->files。** 只走一处的话会出现
        // "下的是小编码器、配置里写的是大编码器"，而那种错加载时报的是
        // "权重读不对"，指向完全错误的方向。
        for (const auto& f : effective_files(g.key, *opt, selections)) {
            if (!f.role.empty()) filled[f.role] = f.name;
        }
        for (const auto& role : g.owned_roles) {
            const auto hit = filled.find(role);
            put("models." + role, hit == filled.end() ? std::string() : hit->second);
        }
    }

    return patch;
}

config::Settings with_selections(
    const config::Settings& base,
    const std::map<std::string, std::string>& selections) {
    if (selections.empty()) return base;
    const json patch = config_patch(selections);
    const auto models = patch.find("models");
    // 一档都没认出来（选项 id 全过时了）就原样退回去。**不是错**：
    // 那时候配置里的文件名还是上一次设置页写的，照旧能跑；界面上
    // `pickProblem` 会说这一档认不出来，修它是那一页的事。
    if (models == patch.end() || !models->is_object()) return base;
    config::Settings out = base;
    config::apply_setup_patch(out, json{{"models", *models}});
    return out;
}

}  // namespace changji::setup
