#include "util/say.hpp"

#include "infer/capability.hpp"

namespace changji::infer {

namespace {

/// 这个键配了而且文件在盘上吗。
bool has(const NodeFacts& f, const std::string& key) {
    const auto it = f.models.find(key);
    return it != f.models.end() && it->second;
}

}  // namespace

const char* to_string(Capability c) {
    switch (c) {
        case Capability::Llm: return "llm";
        case Capability::Tts: return "tts";
        case Capability::Frame: return "frame";
        case Capability::Video: return "video";
        case Capability::Assemble: return "assemble";
    }
    return "unknown";
}

std::optional<Capability> capability_from(const std::string& s) {
    for (const Capability c : all_capabilities()) {
        if (s == to_string(c)) return c;
    }
    return std::nullopt;
}

/// 「某某没配，或者文件不在」。**一句话一个键**：这句话本来在五处各写
/// 一遍，只有中间那个键名不同——翻译要翻五遍，而改一个字得改五处。
std::string kMissing(const char* key) {
    return SAYF("%1 没配，或者文件不在", key);
}

const std::string& label_of(Capability c) {
    switch (c) {
        case Capability::Llm: return SAY("写文");
        case Capability::Tts: return SAY("配音");
        case Capability::Frame: return SAY("首帧");
        case Capability::Video: return SAY("出片");
        case Capability::Assemble: return SAY("装配");
    }
    return SAY("");
}

const std::vector<Capability>& all_capabilities() {
    // 顺序就是流水线的顺序，那张表按它排列。
    static const std::vector<Capability> kAll = {
        Capability::Llm, Capability::Tts, Capability::Frame,
        Capability::Video, Capability::Assemble};
    return kAll;
}

std::string missing_for(Capability c, const NodeFacts& f) {
    switch (c) {
        case Capability::Llm:
            // **指到远端就不看本机有什么**：那条路上这台只是个转发的。
            if (f.llm_remote) return {};
            // 进程内那条（2026-09-19 接回来的）：配了 local 而且权重在，
            // 还得这个二进制真编进了 llama.cpp 的文本那半。
            //
            // ⚠️ **别拿 `models["llm"]` 单独当"能写文"**：权重在盘上而配置指
            // 着远端时，没有任何东西会去读它。判的是 `llm_local`（配置 +
            // 文件两样都对）——**算多了不会在任何地方报错**，人照着那张表去
            // 配，只会一直查不出来为什么写不动。
            if (f.llm_local) {
                if (!f.built_with_llama_chat) {
                    return SAY("配的是进程内写文，但这个二进制没编进 llama.cpp"
                               "（构建时要 CHANGJI_LLAMA=ON）");
                }
                return {};
            }
            return SAY("写文要指到远端服务（[llm].backend = remote 加地址），"
                       "或者在本机装一份编剧模型（backend = local 加 [models].llm）");

        case Capability::Tts:
            if (f.tts_remote) return {};
            if (!f.built_with_llama_tts) {
                return SAY("这个二进制没编 llama.cpp，配音只能指到外部服务");
            }
            // 两个都要：骨干和解码器分开放，缺一个出不了声。
            // **缺了不会报错**，只会退回估算后端出一段静音——所以这里
            // 必须拦住，别让它"成功"。
            // **一句话一个键。** 这句话原来在五处各写一遍，只有中间那个
            // 键名不同——翻译要翻五遍，而改一个字得改五处。
            if (!has(f, "tts")) return kMissing("[models].tts");
            if (!has(f, "tts_decoder")) return kMissing("[models].tts_decoder");
            return {};

        case Capability::Frame:
            if (!f.built_with_sd) {
                return SAY("这个二进制没编 sd.cpp，出不了图");
            }
            if (!has(f, "image")) return kMissing("[models].image");
            // VAE 两个键都认：图像专用的没配就退回视频那份，
            // 和 cannot_do 那边的规矩一致。
            if (!has(f, "image_vae") && !has(f, "video_vae")) {
                return SAY("VAE 没配，或者文件不在（[models].image_vae "
                           "不填就退回 video_vae）");
            }
            return {};

        case Capability::Video:
            if (!f.built_with_sd) {
                return SAY("这个二进制没编 sd.cpp，出不了片");
            }
            if (!has(f, "video")) return kMissing("[models].video");
            if (!has(f, "video_vae")) return kMissing("[models].video_vae");
            // **出片要 ffmpeg 把帧编成 mp4。** 这一条是实机烧出来的：
            // 扩散 8 步全跑完，到最后编码那一步才报找不到 ffmpeg。
            if (!f.ffmpeg_ok) return SAY("没有 ffmpeg，出的帧编不成 mp4");
            return {};

        case Capability::Assemble:
            if (!f.ffmpeg_ok) return SAY("没有 ffmpeg，装配和烧字幕都做不了");
            return {};
    }
    return SAY("认不出这个能力");
}

std::vector<CapabilityReport> capabilities_of(const NodeFacts& f) {
    std::vector<CapabilityReport> out;
    out.reserve(all_capabilities().size());
    for (const Capability c : all_capabilities()) {
        CapabilityReport r;
        r.cap = c;
        r.why = missing_for(c, f);
        r.able = r.why.empty();
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace changji::infer
