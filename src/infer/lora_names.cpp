#include "infer/lora_names.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "util/say.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// sd.cpp 的 name_conversion.cpp 认的 LoRA 前缀（2026-09-13 对着
/// lora_prefix_vec / underline_lora_prefix_vec / diffuison_model_prefix_vec
/// 和那张 {"diffusion_model.", "model.diffusion_model."} 替换表抄的）。
const char* const kKnownPrefixes[] = {
    "model.diffusion_model.", "diffusion_model.", "transformer.",
    "lora_unet_", "lora_te", "lora_", "lora.", "lycoris_", "lycoris.",
    "unet_", "unet.", "te_", "te1_", "te2_", "te3_", "vae_", "vae.",
    "first_stage_model.", "cond_stage_model.", "text_encoders.",
    // UNet 的裸名 sd.cpp 自己会补前缀
    "down_blocks.", "up_blocks.", "mid_block.", "conv_in.", "conv_out.",
    "time_embedding.", "conv_norm_out.",
};

struct Header {
    json doc;
    std::uint64_t json_len = 0;   ///< 头的字节数（不含 8 字节长度）
};

Header read_header(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error(SAYF("打不开 LoRA：%1", paths::to_utf8(p)));
    unsigned char len_bytes[8];
    if (!in.read(reinterpret_cast<char*>(len_bytes), 8)) {
        throw std::runtime_error(
            SAYF("不是 safetensors（不足 8 字节）：%1", paths::to_utf8(p)));
    }
    std::uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | len_bytes[i];
    if (n == 0 || n > (std::uint64_t{1} << 31)) {
        throw std::runtime_error(
            SAYF("不是 safetensors（头长度不合理）：%1", paths::to_utf8(p)));
    }
    std::string buf(static_cast<std::size_t>(n), '\0');
    if (!in.read(buf.data(), static_cast<std::streamsize>(n))) {
        throw std::runtime_error(
            SAYF("safetensors 头读不全：%1", paths::to_utf8(p)));
    }
    Header h;
    h.json_len = n;
    h.doc = json::parse(buf, nullptr, /*allow_exceptions=*/false);
    if (!h.doc.is_object()) {
        throw std::runtime_error(
            SAYF("safetensors 头不是 JSON 对象：%1", paths::to_utf8(p)));
    }
    return h;
}

}  // namespace

bool lora_key_has_known_prefix(const std::string& key) {
    for (const char* pre : kKnownPrefixes) {
        if (key.rfind(pre, 0) == 0) return true;
    }
    return false;
}

LoraNameReport inspect_lora_names(const fs::path& lora) {
    const Header h = read_header(lora);
    LoraNameReport r;
    for (const auto& [k, v] : h.doc.items()) {
        if (k == "__metadata__") continue;
        if (r.sample.empty()) r.sample = k;
        ++r.tensors;
        if (lora_key_has_known_prefix(k)) ++r.prefixed;
    }
    return r;
}

fs::path ensure_sdcpp_lora_names(const fs::path& lora) {
    const LoraNameReport r = inspect_lora_names(lora);
    if (!r.needs_prefix()) return lora;

    fs::path fixed = lora;
    fixed.replace_extension();
    fixed += ".sdcpp.safetensors";

    std::error_code ec;
    if (fs::is_regular_file(fixed, ec) &&
        fs::last_write_time(fixed, ec) >= fs::last_write_time(lora, ec) &&
        fs::file_size(fixed, ec) > 0) {
        return fixed;   // 上次已经写过，源文件没换
    }

    const Header h = read_header(lora);
    json out = json::object();
    for (const auto& [k, v] : h.doc.items()) {
        out[k == "__metadata__" ? k : "diffusion_model." + k] = v;
    }
    std::string text = out.dump();
    // 头按 8 字节对齐补空格，和 safetensors 官方写法一致（数据段偏移是
    // 相对头尾的，头变长不影响偏移）。
    while (text.size() % 8 != 0) text.push_back(' ');

    std::ifstream in(lora, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(8 + h.json_len));
    fs::path tmp = fixed;
    tmp += ".part";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            throw std::runtime_error(
                SAYF("写不了 %1（LoRA 所在目录不可写？）", paths::to_utf8(tmp)));
        }
        unsigned char len_bytes[8];
        std::uint64_t n = text.size();
        for (int i = 0; i < 8; ++i) len_bytes[i] = static_cast<unsigned char>(n >> (8 * i));
        o.write(reinterpret_cast<const char*>(len_bytes), 8);
        o.write(text.data(), static_cast<std::streamsize>(text.size()));
        std::vector<char> buf(1 << 22);
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) o.write(buf.data(), got);
        }
        if (!o) {
            throw std::runtime_error(
                SAYF("写 %1 时出错，多半是磁盘满了", paths::to_utf8(tmp)));
        }
    }
    fs::rename(tmp, fixed, ec);
    if (ec) {
        throw std::runtime_error(SAYF("改名失败：%1 -> %2：%3",
                                      paths::to_utf8(tmp),
                                      paths::to_utf8(fixed), ec.message()));
    }

    // `[出片]` 走 stderr，是日志不是界面，不包（同 `sd_image.cpp` 那几条）。
    std::fprintf(stderr,
                 SAY_NEVER("[出片] LoRA %s 的张量名是裸的（如 %s），sd.cpp 对不上会**悄悄不挂**。"
                 "已在旁边写了一份加 diffusion_model. 前缀的 %s（%zu 个张量），这一轮用它。\n"),
                 paths::to_utf8(lora.filename()).c_str(), r.sample.c_str(),
                 paths::to_utf8(fixed.filename()).c_str(), r.tensors);
    return fixed;
}

}  // namespace changji::infer
