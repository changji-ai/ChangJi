#pragma once

// 首次运行要下哪些模型。
//
// **为什么要有这张表。** 装好程序之后，用户面对的是一个空的 `[models]`
// 小节和十来个键（video / video_llm / video_vae / video_audio_vae /
// image / image_text_encoder / …），每个键填什么、去哪儿下、下哪个量化，
// 全靠翻 `cpp/tools/setup_gpu_box.md`。那份文档是给部署的人看的，
// 不是给第一次打开界面的人看的。
//
// 这张表把"一套能跑的组合"变成一个可以点的选项：一个选项 = 一组配套的
// 文件 + 它要的旋钮。选中之后写回配置的是**整组键**，没用到的一律清空。
//
// ---
//
// **三条硬规则，加条目时必须遵守：**
//
// 一，`bytes` 必须是**核对过的真实字节数**，不许估。下载完成的判据就是
//     它——大小对得上才算下完。这不是洁癖：下载中断留下的截断文件，
//     加载时报的是"权重读不对"，指向完全错误的方向
//     （`cpp/tools/fetch_h3.sh` 里那段注释就是这么栽出来的）。
//     这张表里每一个数都来自 HuggingFace 的 `?blobs=true` 和魔搭的
//     `repo/files` 两个接口，2026-09-10 各核过一遍，**两边一致**。
//
// 二，**一个选项里的文件必须是配套的**。Wan 的 VAE 和 Qwen-Image 的不是
//     一回事，喂错了 sd.cpp 不报错——它照常加载，然后出一张和提示词
//     没关系的图。所以文件不是按"角色"各选各的，是整组一起选。
//
// 三，**换组要清空上一组的键。** `[models].video_lora` 默认指着 H3 的
//     Turbo LoRA；用户选了 Wan 之后那一项还留着的话，sd.cpp 会拿一个
//     给 H3 做的 LoRA 往 Wan 上挂。所以每组声明自己管哪些键
//     （`owned_roles`），写回时**每个键都写**，没用到的写空串。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"

namespace changji::setup {

/// 一档的名字里**要翻的是哪几段**。
///
/// 这些名字是拼出来的：`MiniMax-H3 完整 · Q4_K_M`、`不下载 · 之后再说`。
/// 整条当一个键的话，同一个「完整」要在表里写十遍，而且每加一档量化就多
/// 一句要翻的话——而多出来的那一句和上一句只差一个型号代号。
///
/// 规矩两条：**表里有整条就用整条**（`智谱 GLM（云端 · 免费档）` 那种里头
/// 自己带着 ` · ` 的，切开就碎了），否则按 ` · ` 切开一段一段翻。
///
/// ⚠️ **出 json 那头和用例走的是同一个函数。** 判"翻没翻全"和"到底怎么翻"
/// 必须是同一条规矩——分开写两份，迟早一份认整条一份认分段，而那时候用例
/// 是绿的、界面上是半句中文。
std::vector<std::string> label_parts(const std::string& label);

/// 把一档的名字翻过来。段与段之间还是 ` · `。
std::string say_label(const std::string& label);

/// 一个要下的文件。
struct FileSpec {
    /// 落在模型目录下的相对路径。可以带子目录（`llm/Qwen3-14B-Q4_K_M.gguf`）。
    std::string name;
    /// 仓库名，形如 `city96/Qwen-Image-gguf`。
    ///
    /// **存仓库和路径，不存完整地址**：同一个文件在魔搭和 HuggingFace 上
    /// 仓库名、路径、字节数完全一样，只有域名不同（2026-09-10 逐个核过
    /// 十一个仓库）。地址由 `setup::resolve_url` 按当前源拼出来。
    std::string repo;
    /// 仓库内的相对路径，形如 `split_files/vae/qwen_image_vae.safetensors`。
    std::string path;
    /// 真实字节数。见文件头第一条规则。**两个源上必须一致**——
    /// 不一致的话下载完成的判据就废了，见 setup/source.hpp。
    std::uint64_t bytes = 0;
    /// 填进 `[models]` 的哪个键。空串表示这个文件不进配置。
    std::string role;
    /// 一句话说明这个文件是干什么的。界面上逐条显示。
    std::string note;
};

/// 同一个角色下可以换的几份。
///
/// **为什么要有这个。** 一档 H3 要下五个文件，而其中**文本编码器一个人
/// 就占四成多**（43.6 GB 里的 18.2 GB），视频 VAE 再占 5.2 GB。这两样
/// 上游都有更小的一档，而在这之前它们是按扩散模型大小自动挑的，用户
/// 连名字都看不见。用户 2026-09-17：「我要选」。
///
/// 和文件头第二条规则（文件必须配套）不冲突：这里换的是**同一个模型自己
/// 的不同量化**，不是换一家的零件。跨家族的搭配仍然不许。
struct FileAlt {
    /// 换的是哪个键：`video_llm` / `video_vae`。
    std::string role;
    /// 界面上这一行叫什么：「文本编码器」「视频 VAE」。
    std::string title;
    /// 可挑的几份，**第一项是默认**。
    std::vector<FileSpec> choices;
};

/// 一个「版本」：一套配套的文件，加上它要的旋钮。
struct Option {
    std::string id;
    /// 模型家族，比如 "MiniMax-H3"。同家族的不同量化排在一起。
    std::string family;
    /// 界面上显示的名字，形如 "MiniMax-H3 · Q4_K_M ＋ Turbo"。
    std::string label;
    /// 量化档，比如 "Q4_K_M"、"fp8"。空串表示这个选项不涉及量化。
    std::string quant;
    /// 这个家族是什么、好在哪、代价是什么。**同一家族的每一档都是同一句**，
    /// 界面按家族只显示一遍。必须说代价——只说好处的话，用户会一律挑
    /// 最大的那个，然后在出片时 OOM。
    std::string family_note;
    /// 这一档量化本身的一句话（"画质和体积最平衡的一档"）。
    std::string note;
    /// **权重能常驻显存所需的显存（GB）。不是"跑不起来的下限"。**
    ///
    /// 低于这个数照样能跑：`[models].weights = "smart"` 会把权重放系统内存，
    /// 用到才搬进显存——每一步都在等 PCIe，慢几倍，但出得来东西。
    /// 5090 上实测过这个差别：图像模型权重常驻是 35 秒一张，
    /// 放内存是 190 秒一张。
    ///
    /// **这个数是算出来的，不是手填的**（见 catalog.cpp 的 resident_vram_*）：
    /// 拿引擎真正在用的那两个函数（`ModelsConfig::weights_for` /
    /// `image_weights_for`）反着扫一遍，找它从 "cpu" 翻成常驻的那一点。
    /// 手填的话公式一改这张表就开始骗人，而且不会有任何报错。
    double min_vram_gb = 0.0;
    /// 同组里的高低排序，大的更好。挑默认值时在够得上的里面取最大的。
    int rank = 0;
    std::vector<FileSpec> files;
    /// 这一档里可以换的那几个角色。`files` 里放的是每个 alt 的默认那份，
    /// 换了之后用 `effective_files` 取真正要下的清单。
    std::vector<FileAlt> alts;
    /// 额外要写回的配置项，键是带小节的全名（`models.video_rng`）。
    /// **不是可有可无的**：H3 不设 `video_rng = "cpu"` 出来的片和提示词
    /// 对不上，而且不报错。
    std::vector<std::pair<std::string, nlohmann::json>> settings;

    std::uint64_t total_bytes() const;
};

/// 一组：干同一件事的若干个版本，选一个。
struct Group {
    /// `video` / `image` / `llm` / `tts`
    std::string key;
    std::string title;
    /// 这一组是干什么用的，一句话。
    std::string purpose;
    /// 缺了它整条流水线就跑不通。非必需的组可以整组跳过。
    bool required = true;
    /// 这一组管 `[models]` 里的哪些键。写回时**每个键都写**，
    /// 选中的选项没用到的写空串。见文件头第三条规则。
    std::vector<std::string> owned_roles;
    std::vector<Option> options;

    const Option* find(const std::string& option_id) const;
};

/// 全部条目。**进程内是同一份常量**，不读盘。
const std::vector<Group>& catalog();

/// 按这张卡挑每一组的默认选项，返回 {组: 选项 id}。
///
/// 规则：够得上（`min_vram_gb <= vram_gb`）的里面挑 `rank` 最大的；
/// 一个都够不上就挑门槛最低的那个——**不能返回空**，
/// 界面上"没有默认值"等于让用户自己去猜。
std::map<std::string, std::string> recommend(double vram_gb);

/// 把选择展开成 `save_user_config` 吃的那种 patch：`{小节: {键: 值}}`。
///
/// `selections` 是 {组: 选项 id}。组不在表里、或者选项 id 不认识就跳过
/// 那一组——**不报错**：老版本的界面可能提交一个已经删掉的 id，
/// 那时候少写一组比整个请求失败好。
///
/// 值为 `"none"` 的选项表示这一组不下载（用外接服务），那时候只写
/// 它自己的 `settings`，`owned_roles` 一概不动——用户可能本来就手配了
/// 一套模型，只是这次不想重下。
nlohmann::json config_patch(const std::map<std::string, std::string>& selections);

/// 替换档在 `selections` 里用的键：`"video/video_llm"`。
///
/// **和组键共用一张表**，这样从界面到写配置一路只有一个 map，不用为了
/// 两个下拉再开一条通道。认不出的键一律忽略。
std::string alt_key(const std::string& group_key, const std::string& role);

/// 这一档真正要下的文件：`o.files` 按 `selections` 里挑的替换档换过之后。
///
/// 没挑、或者挑了个不认识的，就用 `files` 里原来那份（也就是每个 alt 的
/// 第一项）。**下载、算总量、写配置三处必须都走它**，只走一处的话会出现
/// "下的是小编码器、配置里写的是大编码器"，而那种错加载时报的是
/// "权重读不对"，指向完全错误的方向。
std::vector<FileSpec> effective_files(
    const std::string& group_key, const Option& o,
    const std::map<std::string, std::string>& selections);

/// 按这几档在内存里覆一层，回一份新的配置。**不落盘、不动 `base`。**
///
/// **只落 `models` 那一节。** 一个选项的 `settings` 里还带着 `llm.model`、
/// `tts.backend`、`tiers.final_steps` 这些——那些是设置页那一趟写盘时才该
/// 动的东西，而且那一趟有它自己的"别冲掉用户手挑的"规矩
/// （见 setup_api.cpp 里 `same_service_key` 那一段）。这儿每跑一个任务就
/// 覆一遍，照单全收的话，用户在设置页手改的大模型型号会被每一次出片
/// 悄悄改回去。这儿要的只有一件事：**这部电影用哪几个模型文件**。
///
/// 两个调用方，两头都要：跑一章时把这部电影挑的档位覆到本机配置上，
/// 以及工作进程拿到派来的活之后覆到**它自己**那份上——它的模型目录和
/// 派活那台不是同一个，所以带过去的只能是档位 id，不能是路径。
config::Settings with_selections(
    const config::Settings& base,
    const std::map<std::string, std::string>& selections);

/// 新下的文件放在模型目录下哪个子目录：`llm` / `video` / `image` / `tts`。
///
/// **按这一组管的是哪类模型分，名字用英文**（用户 2026-09-24：「下载模型应该
/// 放到模型目录下相应类型的目录里，目录用英语」）。原来是平铺在模型目录根上，
/// 一档 H3 五个文件、Qwen-Image 四个，混在一起分不清哪个是哪一组的。
///
/// 定妆和空景那一组（`image_base`）也进 `image`：它和首帧那一组是同一族图像
/// 模型，分开放的话同一个家族散在两处。
///
/// ⚠️ **这只管"新下的放哪"，不管"去哪儿找"。** 配置里记的仍然是 `FileSpec::name`
/// （机器无关，跟着项目的档位 id 走），找文件由 `ModelsConfig::resolve` 按名字
/// 在整个模型目录里找（config/model_index.hpp）——所以老版本平铺在根上的那几份
/// 不用挪、也不会重下。
std::string type_dir(const std::string& group_key);

/// 这个文件新下时落在哪，相对模型目录：`<type_dir>/<文件名>`。
///
/// 只取 `f.name` 的文件名那一段：`loras/x.safetensors` 落到 `video/x.safetensors`，
/// `llm/Qwen3-…gguf` 还是 `llm/Qwen3-…gguf`。
std::string download_rel(const std::string& group_key, const FileSpec& f);

/// 这一组里那个"不下载"的选项 id。约定值，别处也用它比较。
inline constexpr const char* kNoneOption = "none";

}  // namespace changji::setup
