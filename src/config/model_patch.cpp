#include "config/model_patch.hpp"

#include <map>

namespace changji::config {

using json = nlohmann::json;

std::string* models_field(ModelsConfig& m, const std::string& role) {
    // 一张平表而不是一串 if：漏一个角色的表现是"下完了但配置里没写上"，
    // 而那要到出片时才报"模型没配"，离这里隔着十万八千里。
    // 加字段时这里也要加——test_setup.cpp 拿 catalog 里出现过的角色查这张表。
    // **成员指针类型先起个别名。** 直接把 `std::string ModelsConfig::*`
    // 写进模板实参表里，MSVC 解析不了（error C2059，然后 kMap 整个没声明成）；
    // GCC 接受，所以只在 Windows 上编不过。别名两边都认。
    using Field = std::string ModelsConfig::*;
    static const std::map<std::string, Field> kMap = {
        {"llm", &ModelsConfig::llm},
        {"video", &ModelsConfig::video},
        {"video_high_noise", &ModelsConfig::video_high_noise},
        {"video_vae", &ModelsConfig::video_vae},
        {"video_text_encoder", &ModelsConfig::video_text_encoder},
        {"video_llm", &ModelsConfig::video_llm},
        {"video_llm_vision", &ModelsConfig::video_llm_vision},
        {"video_audio_vae", &ModelsConfig::video_audio_vae},
        {"video_lora", &ModelsConfig::video_lora},
        {"image", &ModelsConfig::image},
        {"image_base", &ModelsConfig::image_base},
        {"image_vae", &ModelsConfig::image_vae},
        {"image_text_encoder", &ModelsConfig::image_text_encoder},
        {"image_text_encoder_vision",
         &ModelsConfig::image_text_encoder_vision},
        {"tts", &ModelsConfig::tts},
        {"tts_decoder", &ModelsConfig::tts_decoder},
    };
    const auto it = kMap.find(role);
    return it == kMap.end() ? nullptr : &(m.*(it->second));
}

void apply_setup_patch(Settings& s, const json& patch) {
    if (patch.contains("models") && patch["models"].is_object()) {
        for (const auto& [key, value] : patch["models"].items()) {
            if (std::string* field = models_field(s.models, key)) {
                if (value.is_string()) *field = value.get<std::string>();
                continue;
            }
            // 这一页会写的几个非路径旋钮。H3 和 Wan 的这几个数不一样，
            // 而填错了三处都不报错——出来的片和提示词没关系而已。
            if (key == "dir" && value.is_string()) {
                const auto v = value.get<std::string>();
                s.models.dir = v.empty() ? std::optional<std::string>()
                                         : std::optional<std::string>(v);
            } else if (key == "video_rng" && value.is_string()) {
                s.models.video_rng = value.get<std::string>();
            } else if (key == "video_lora_tiers" && value.is_string()) {
                s.models.video_lora_tiers = value.get<std::string>();
            } else if (key == "video_cfg" && value.is_number()) {
                s.models.video_cfg = value.get<double>();
            } else if (key == "video_flow_shift" && value.is_number()) {
                s.models.video_flow_shift = value.get<double>();
            } else if (key == "video_lora_strength" && value.is_number()) {
                s.models.video_lora_strength = value.get<double>();
            }
        }
    }
    if (patch.contains("tiers") && patch["tiers"].is_object()) {
        const auto& t = patch["tiers"];
        if (t.contains("final_steps") && t["final_steps"].is_number_integer()) {
            s.tiers.final_steps = t["final_steps"].get<int>();
        }
        if (t.contains("draft_steps") && t["draft_steps"].is_number_integer()) {
            s.tiers.draft_steps = t["draft_steps"].get<int>();
        }
    }
    // **这三项要一起落。** 原来只落 backend，于是选了云端那一项之后，
    // 文件里是智谱的地址和模型，内存里还是上一家的——写剧本仍然发往
    // 上一家，而且要等到重启才"自己好了"。反过来同样难查：2026-09-14
    // 那个 bug 里，这一页把 llm.model 冲回默认只改了文件没改内存，
    // 于是界面上一切正常，下次开机才跳回去。
    if (patch.contains("llm") && patch["llm"].is_object()) {
        const auto& l = patch["llm"];
        if (l.contains("backend") && l["backend"].is_string()) {
            s.llm.backend = l["backend"].get<std::string>();
        }
        if (l.contains("base_url") && l["base_url"].is_string()) {
            s.llm.base_url = l["base_url"].get<std::string>();
        }
        if (l.contains("model") && l["model"].is_string()) {
            s.llm.model = l["model"].get<std::string>();
        }
    }
    if (patch.contains("tts") && patch["tts"].is_object() &&
        patch["tts"].contains("backend") && patch["tts"]["backend"].is_string()) {
        s.tts.backend = patch["tts"]["backend"].get<std::string>();
    }
}

}  // namespace changji::config
