// LoRA 张量名对不对得上 sd.cpp。来历见 infer/lora_names.hpp 头上那段：
// Turbo LoRA 裸名一个都没匹配上，sd.cpp 不报，「Turbo 6 步」跑了四天
// 实际是基础模型裸跑。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "infer/lora_names.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path temp_dir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path() / paths::from_utf8("changji_lora_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// 写一个最小的 safetensors：给定键名，每个张量 4 个 f32。
fs::path write_st(const fs::path& p, const std::vector<std::string>& keys,
                  bool with_meta = true) {
    json h = json::object();
    if (with_meta) h["__metadata__"] = {{"base_model", "MiniMax-H3"}};
    std::size_t off = 0;
    for (const auto& k : keys) {
        h[k] = {{"dtype", "F32"}, {"shape", {4}}, {"data_offsets", {off, off + 16}}};
        off += 16;
    }
    std::string text = h.dump();
    while (text.size() % 8 != 0) text.push_back(' ');
    std::ofstream o(p, std::ios::binary);
    std::uint64_t n = text.size();
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(n >> (8 * i));
    o.write(reinterpret_cast<const char*>(len), 8);
    o.write(text.data(), static_cast<std::streamsize>(text.size()));
    for (std::size_t i = 0; i < off; ++i) o.put(static_cast<char>(i & 0xff));
    return p;
}

std::string data_of(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    unsigned char len[8];
    in.read(reinterpret_cast<char*>(len), 8);
    std::uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | len[i];
    in.seekg(static_cast<std::streamoff>(8 + n));
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("裸名的 LoRA 会写一份加 diffusion_model. 前缀的副本，数据一个字节不动") {
    const fs::path d = temp_dir("bare");
    const fs::path src = write_st(d / "turbo.safetensors",
                                  {"blocks.0.attn.qkv_proj.lora_A.weight",
                                   "blocks.0.attn.qkv_proj.lora_B.weight",
                                   "token_refiner.blocks.0.mlp.fc1.lora_A.weight",
                                   "final_layer.linear.lora_B.weight"});

    const auto before = infer::inspect_lora_names(src);
    CHECK(before.tensors == 4);
    CHECK(before.prefixed == 0);
    CHECK(before.needs_prefix());
    CHECK(before.sample == "blocks.0.attn.qkv_proj.lora_A.weight");

    const fs::path fixed = infer::ensure_sdcpp_lora_names(src);
    CHECK(fixed != src);
    CHECK(fixed.filename() == paths::from_utf8("turbo.sdcpp.safetensors"));
    REQUIRE(fs::is_regular_file(fixed));

    const auto after = infer::inspect_lora_names(fixed);
    CHECK(after.tensors == 4);
    CHECK(after.prefixed == 4);
    CHECK_FALSE(after.needs_prefix());
    CHECK(after.sample.rfind("diffusion_model.", 0) == 0);
    // 数据段原样
    CHECK(data_of(fixed) == data_of(src));
    CHECK(data_of(fixed).size() == 64);

    SUBCASE("第二次直接复用，不重写") {
        const auto t1 = fs::last_write_time(fixed);
        CHECK(infer::ensure_sdcpp_lora_names(src) == fixed);
        CHECK(fs::last_write_time(fixed) == t1);
    }

    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("已经带前缀的 LoRA 原样用，不写副本") {
    const fs::path d = temp_dir("prefixed");
    for (const char* pre : {"diffusion_model.", "model.diffusion_model.", "lora_unet_", "transformer."}) {
        const fs::path src = write_st(d / (std::string(pre).substr(0, 4) + ".safetensors"),
                                      {std::string(pre) + "blocks.0.attn.qkv_proj.lora_A.weight"});
        CAPTURE(pre);
        CHECK_FALSE(infer::inspect_lora_names(src).needs_prefix());
        CHECK(infer::ensure_sdcpp_lora_names(src) == src);
    }
    // UNet 的裸名 sd.cpp 自己会补，也不算裸
    CHECK(infer::lora_key_has_known_prefix("down_blocks.0.attentions.0.lora_down.weight"));
    CHECK_FALSE(infer::lora_key_has_known_prefix("blocks.0.attn.qkv_proj.lora_A.weight"));
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST_CASE("不是 safetensors 的文件要说清楚，不能当成没张量") {
    const fs::path d = temp_dir("bad");
    const fs::path p = d / "x.safetensors";
    std::ofstream(p, std::ios::binary) << "not a safetensors file";
    CHECK_THROWS(infer::inspect_lora_names(p));
    CHECK_THROWS(infer::ensure_sdcpp_lora_names(p));
    CHECK_THROWS(infer::inspect_lora_names(d / "missing.safetensors"));
    std::error_code ec;
    fs::remove_all(d, ec);
}
