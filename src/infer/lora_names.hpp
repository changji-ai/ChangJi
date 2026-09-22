#pragma once

// LoRA 文件的张量名对不对得上 sd.cpp。
//
// **2026-09-13 坐实的一件事：MiniMax-H3 的 Turbo LoRA 从来没作用上过。**
// 挂着它和摘掉它出的片**字节相同**，`apply_loras completed, taking 0.04s`
// ——780 MB 的 LoRA 对着 18 GB 的模型，0.04 秒就是一个张量都没匹配上。
// 而 sd.cpp **一个字都不报**：at_runtime 模式下过滤器挑不出任何属于扩散
// 模型的张量，就干脆不挂适配器，连「(0 / 518) applied」那句都没有。
// 所以「Turbo 6 步」跑了四天，实际是基础模型裸跑 6 步——画面静、糊、
// 中途硬切，都有它一份。
//
// 原因在张量名。这份 LoRA（larryvrh/MiniMax-H3-Turbo-Lora）的名字是裸的：
//
//     blocks.0.attn.qkv_proj.lora_A.weight
//
// sd.cpp 的 name_conversion 只认带前缀的 LoRA 名（`diffusion_model.`、
// `model.diffusion_model.`、`lora_unet_`、`transformer.` 这些），裸名只给
// UNet 那几种（`down_blocks.` / `up_blocks.` / `mid_block.`）补前缀，
// DiT 的 `blocks.` 不在列。ComfyUI 那边有人专门发了一份加了
// `diffusion_model.` 前缀的转换版（drbaph/MiniMax-H3-Turbo-Lora-ComfyUI），
// 就是为了这个。
//
// 实测：把头里的键统统加上 `diffusion_model.`，其它一个字节不动，
// sd.cpp 报「(518 / 518) LoRA tensors have been applied」，同一镜同种子
// 的输出变了，清晰度 280 → 375。
//
// 这里做的就是那件事：加载前看一眼头，没有任何一个键带认得的前缀，就在
// 旁边写一份 `<名字>.sdcpp.safetensors`（只改头，数据段原样拷）并改用它。
// safetensors 的格式是 8 字节小端长度 + JSON 头 + 数据段，键改名不动
// 偏移，所以这一步不需要任何张量库。

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace changji::infer {

/// 一份 LoRA 头里的情况。
struct LoraNameReport {
    std::size_t tensors = 0;      ///< 张量数（不含 __metadata__）
    std::size_t prefixed = 0;     ///< 已经带认得的前缀的
    std::string sample;           ///< 第一个键，报错时给人看
    bool needs_prefix() const { return tensors > 0 && prefixed == 0; }
};

/// 只读头，不动文件。打不开或不是 safetensors 抛 std::runtime_error。
LoraNameReport inspect_lora_names(const std::filesystem::path& lora);

/// 这个键 sd.cpp 认不认得出是扩散模型的 LoRA 张量。
bool lora_key_has_known_prefix(const std::string& key);

/// 保证交给 sd.cpp 的那份 LoRA 的张量名带前缀。
///
/// 键都带前缀就原样返回；裸名就写一份 `<stem>.sdcpp.safetensors` 在旁边
/// 并返回它（已经有一份且不比源文件旧就直接复用）。写不了（目录只读）
/// 抛 std::runtime_error，调用方决定退回原文件还是报错。
std::filesystem::path ensure_sdcpp_lora_names(const std::filesystem::path& lora);

}  // namespace changji::infer
