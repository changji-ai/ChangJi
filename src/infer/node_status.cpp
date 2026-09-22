#include "util/say.hpp"

#include "infer/node_status.hpp"

#include <filesystem>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>  // gethostname
#endif

#include "infer/llama_chat.hpp"
#include "infer/llama_tts.hpp"
#include "infer/local_exec.hpp"
#include "infer/sd_backend.hpp"
#include "media/ffmpeg.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// `[models]` 里这些键会被查一遍。**顺序无所谓，缺项才要紧**：
/// 少查一个键，能力判断就会说"能干"，而派过去是一镜失败。
std::vector<std::pair<std::string, std::string>> model_entries(
    const config::ModelsConfig& m) {
    return {
        {"llm", m.llm},
        {"tts", m.tts},
        {"tts_decoder", m.tts_decoder},
        {"image", m.image},
        {"image_vae", m.image_vae},
        {"video", m.video},
        {"video_vae", m.video_vae},
    };
}

/// 这台机器叫什么。身份不靠它（那是密钥的事），它只是给人看的。
std::string node_name() {
    for (const char* key : {"CHANGJI_NODE_NAME", "COMPUTERNAME", "HOSTNAME"}) {
        const std::string v = paths::env(key);
        if (!v.empty()) return v;
    }
#ifndef _WIN32
    // **`HOSTNAME` 在 Linux 和 macOS 上是个 shell 变量，不是导出的环境变量**
    // ——bash 自己设它，但不 export；`COMPUTERNAME` 又只有 Windows 有。
    // 所以上面那一圈在非 Windows 上几乎必然一个都取不到，这张表里每一台
    // 都叫「未命名」：一列专门用来区分机器的东西，一台也区分不了。
    // 离线时还更难看——node_registry 那边 `n.name` 回落成 cfg.url，
    // 于是同一行把地址印两遍。
    //
    // 实测：本机 `wangchaos-iMac.local`、远程 `iZrj9iklf7dusoa3eh8qi4Z`，
    // 两台的主机名都好好的，两个进程的 environ 里却都没有 HOSTNAME。
    // 主机名要用 gethostname 问，不能指望环境变量。
    char host[256];
    if (::gethostname(host, sizeof host) == 0) {
        host[sizeof host - 1] = '\0';
        if (host[0] != '\0') return host;
    }
#endif
    return SAY("未命名");
}

/// 这个卷还剩多少字节。问不到回 0。
std::uint64_t free_bytes_of(const std::filesystem::path& dir) {
    std::error_code ec;
    // **问的是这个目录所在的卷。** 目录还不存在时往上退一级再问——
    // 模型目录常常是"还没建"的状态，而那时候恰恰最需要知道装不装得下。
    std::filesystem::path p = dir;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    if (p.empty()) return 0;
    const auto info = std::filesystem::space(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(info.available);
}

}  // namespace

NodeFacts probe_facts(const config::Settings& s) {
    NodeFacts f;
    f.built_with_sd = sd_available();
    // 配音和写文各问各的：同一个构建开关，两个门面。
    f.built_with_llama_tts = llama_tts_available();
    f.built_with_llama_chat = llama_chat_available();

    try {
        const media::FFmpeg ff(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                               media::default_runner());
        ff.check();
        f.ffmpeg_ok = true;
    } catch (const std::exception&) {
        f.ffmpeg_ok = false;
    }

    const auto ws = s.workspace_path();
    for (const auto& [key, name] : model_entries(s.models)) {
        if (name.empty()) {
            f.models[key] = false;
            continue;
        }
        std::error_code ec;
        f.models[key] =
            std::filesystem::is_regular_file(s.models.resolve(name, ws), ec);
    }

    // **「能写文」= 这台有一条走得通的大模型路**，不是"指到了远端"。
    // 名字还叫 llm_remote 是历史（capability.hpp 那边一并说了）。
    // 2026-09-18 多了命令行那条：跑本机的 claude / codex 一样能写文，
    // 漏了的话跨机那张表会说这台不能写——而它明明能。
    f.llm_remote = (s.llm.backend == "remote" && !s.llm.base_url.empty()) ||
                   (s.llm.backend == "command" && !s.llm.command.empty());
    // 进程内那条：配了 local 而且权重文件真在。编没编进来另一个旗子说。
    f.llm_local = s.llm.backend == "local" && f.models["llm"];
    f.tts_remote = s.tts.backend == "http" && s.tts.base_url.has_value() &&
                   !s.tts.base_url->empty();
    return f;
}

std::string can_produce_line(const NodeFacts& f) {
    std::string out;
    for (const auto& r : capabilities_of(f)) {
        if (!r.able) continue;
        if (!out.empty()) out += SAY("、");
        out += label_of(r.cap);
    }
    return out.empty() ? SAY("什么都产不了") : out;
}

json node_status_json(const config::Settings& s,
                      const models::HardwareProfile& profile) {
    const NodeFacts f = probe_facts(s);

    json caps = json::array();
    for (const auto& r : capabilities_of(f)) {
        caps.push_back({{"cap", to_string(r.cap)},
                        {"label", label_of(r.cap)},
                        {"able", r.able},
                        {"why", r.why}});
    }

    json gpus = json::array();
    if (profile.gpu) {
        // **报的是卡 0 的显存乘以卡数吗？不是。** vram_mb 是单卡的，
        // count 是张数——加起来去查档位表会算出一张卡根本跑不动的分辨率，
        // 见 GPUInfo 那两条注释。这里两个数分开报，谁要用谁自己判。
        gpus.push_back({{"name", profile.gpu->name},
                        {"vram_mb", profile.gpu->vram_mb},
                        {"count", profile.gpu->count},
                        {"unified_mb", profile.gpu->unified_mb}});
    }

    const auto models_dir = s.models.dir_path(s.workspace_path());
    return json{
        {"name", node_name()},
        {"version", CHANGJI_VERSION},
        {"built_with", {{"sd", f.built_with_sd},
                        {"llama_tts", f.built_with_llama_tts},
                        {"ffmpeg", f.ffmpeg_ok}}},
        {"gpus", gpus},
        {"capabilities", caps},
        {"models_dir", paths::to_utf8(models_dir)},
        {"models_dir_free_bytes", free_bytes_of(models_dir)},
        // 忙不忙问的是本机那个执行位——它才是"这张卡此刻有没有人用"
        // 的唯一真相，`Scheduler` 那边的 loaded 只说明模型还在显存里。
        {"busy", local_exec().busy()},
        {"waiting", local_exec().waiting()},
    };
}

}  // namespace changji::infer
