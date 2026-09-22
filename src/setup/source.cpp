#include "setup/source.hpp"

#include <chrono>
#include <mutex>
#include <string>

#include "util/paths.hpp"
#include "util/proc.hpp"

namespace changji::setup {

namespace {

/// 探测用的文件。**挑一个小的**：HEAD 不下正文，但有些 CDN 会按文件大小
/// 做不同的重定向，拿一个真实存在的小文件最接近真实下载那一步。
/// 这个 VAE 两个源上都有，254 MB。
constexpr const char* kProbeRepo = "Comfy-Org/Qwen-Image_ComfyUI";
constexpr const char* kProbePath = "split_files/vae/qwen_image_vae.safetensors";

/// 一次 HEAD，返回耗时（秒）。连不上返回 -1。
///
/// 走 curl 而不是进程内的 httplib。https 现在发得了（OpenSSL 静态编进来了，
/// 见 CMakeLists 里那段），但测速和真实下载走同一条路才有意义——
/// 下载是 aria2c/curl 干的（要多连接和断点续传，见 downloader.hpp）。
double head_seconds(const std::string& url) {
#ifdef _WIN32
    constexpr const char* kNull = "NUL";
#else
    constexpr const char* kNull = "/dev/null";
#endif
    const auto r = proc::run(
        "curl",
        {"-s", "-I", "-o", kNull, "-w", "%{http_code} %{time_total}", "--max-time",
         "6", url},
        9000);
    if (!r.launched || r.exit_code != 0) return -1.0;
    // 输出形如 "302 0.285137"
    const auto space = r.out.find(' ');
    if (space == std::string::npos) return -1.0;
    int code = 0;
    double seconds = -1.0;
    try {
        code = std::stoi(r.out.substr(0, space));
        seconds = std::stod(r.out.substr(space + 1));
    } catch (const std::exception&) {
        return -1.0;
    }
    // 2xx 和 3xx 都算通：两个源都会 302 到各自的 CDN。
    if (code < 200 || code >= 400) return -1.0;
    return seconds;
}

}  // namespace

const char* to_string(Source v) {
    switch (v) {
        case Source::Auto: return "auto";
        case Source::ModelScope: return "modelscope";
        case Source::HuggingFace: return "huggingface";
        case Source::HfMirror: return "hf-mirror";
    }
    return "auto";
}

std::optional<Source> source_from_string(const std::string& s) {
    if (s == "auto") return Source::Auto;
    if (s == "modelscope") return Source::ModelScope;
    if (s == "huggingface") return Source::HuggingFace;
    if (s == "hf-mirror") return Source::HfMirror;
    return std::nullopt;
}

std::string resolve_url(Source source, const std::string& repo,
                        const std::string& path) {
    switch (source) {
        case Source::HuggingFace:
            return "https://huggingface.co/" + repo + "/resolve/main/" + path;
        case Source::HfMirror:
            return "https://hf-mirror.com/" + repo + "/resolve/main/" + path;
        case Source::ModelScope:
        case Source::Auto:
        default:
            // 魔搭的分支叫 master 不是 main，路径里还多一段 /models。
            return "https://modelscope.cn/models/" + repo + "/resolve/master/" + path;
    }
}

namespace {

/// 探测结果的缓存。见头文件里为什么要有它。
struct ProbeCache {
    std::mutex mu;
    bool has = false;
    std::chrono::steady_clock::time_point at{};
    SourceProbe value;
};
ProbeCache& probe_cache() {
    static ProbeCache c;
    return c;
}
constexpr auto kProbeTtl = std::chrono::minutes(5);

SourceProbe probe_sources_uncached();

}  // namespace

SourceProbe probe_sources(bool force) {
    ProbeCache& c = probe_cache();
    if (!force) {
        std::lock_guard lg(c.mu);
        if (c.has && std::chrono::steady_clock::now() - c.at < kProbeTtl) {
            return c.value;
        }
    }
    // **探的时候不拿着锁。** 两次 HEAD 最多 12 秒，拿着锁的话所有并发
    // 请求全排在后面等，白屏反而更长——那正是这个缓存要治的病。
    // 代价是偶尔有两个请求同时去探，各自写一次缓存，写的是同一类值，无所谓。
    SourceProbe fresh = probe_sources_uncached();
    {
        std::lock_guard lg(c.mu);
        c.value = fresh;
        c.at = std::chrono::steady_clock::now();
        c.has = true;
    }
    return fresh;
}

namespace {

SourceProbe probe_sources_uncached() {
    SourceProbe out;

    const std::string forced = paths::env("CHANGJI_MODEL_SOURCE");
    if (!forced.empty()) {
        if (const auto s = source_from_string(forced);
            s.has_value() && *s != Source::Auto) {
            out.source = *s;
            out.how = "env";
            return out;
        }
    }

    if (!proc::which("curl").has_value()) {
        // 探不了。回魔搭：这个程序的用户主要在国内，而且魔搭少一层
        // "墙通不通"的变数。界面上会说明这是没探成的默认值。
        out.how = "default";
        return out;
    }

    out.modelscope_s =
        head_seconds(resolve_url(Source::ModelScope, kProbeRepo, kProbePath));
    out.huggingface_s =
        head_seconds(resolve_url(Source::HuggingFace, kProbeRepo, kProbePath));
    out.how = "probed";

    const bool ms_ok = out.modelscope_s >= 0.0;
    const bool hf_ok = out.huggingface_s >= 0.0;
    if (ms_ok && hf_ok) {
        // 两边都通就比快慢。**不按地理位置猜**：国内挂了代理的机器
        // HuggingFace 可能更快，那就该走 HuggingFace。
        out.source = out.huggingface_s < out.modelscope_s ? Source::HuggingFace
                                                          : Source::ModelScope;
    } else if (hf_ok) {
        out.source = Source::HuggingFace;
    } else {
        // 魔搭通、或者两边都不通。后者多半是这台机器整个没外网，
        // 那时候选哪个都一样，回默认的那个。
        out.source = Source::ModelScope;
    }
    return out;
}

}  // namespace

Source detect_source() {
    // **一个进程只探一次。** 探一次要两个 HEAD、最坏 12 秒，
    // 而这个判断在一次运行里不会变。
    static std::once_flag once;
    static Source cached = Source::ModelScope;
    std::call_once(once, [] { cached = probe_sources().source; });
    return cached;
}

}  // namespace changji::setup
