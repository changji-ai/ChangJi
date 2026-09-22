#include "infer/sd_backend.hpp"

#include <cstdio>
#include <mutex>

#ifdef CHANGJI_HAVE_SD
#include <stable-diffusion.h>
#endif

namespace changji::infer {

#ifdef CHANGJI_HAVE_SD

namespace {

std::mutex g_sink_mu;
SdLogSink g_sink;

/// sd.cpp 的等级比我们多一档（它有 VERBOSE），压到四档。
///
/// VERBOSE 压成 debug 而不是 info：那一档是逐层加载张量的流水账，
/// 一次加载几百行。当 info 的话正常运行的日志里全是它。
int map_level(sd_log_level_t l) {
    switch (l) {
        case SD_LOG_DEBUG:
        case SD_LOG_VERBOSE: return 0;
        case SD_LOG_INFO:    return 1;
        case SD_LOG_WARN:    return 2;
        case SD_LOG_ERROR:   return 3;
    }
    return 1;
}

void log_trampoline(sd_log_level_t level, const char* text, void* /*data*/) {
    SdLogSink sink;
    {
        std::lock_guard lg(g_sink_mu);
        sink = g_sink;
    }
    if (sink && text) sink(map_level(level), text);
}

}  // namespace

bool sd_available() { return true; }

std::string sd_version() {
    const char* v = ::sd_version();
    return v ? v : "";
}

std::string sd_system_info() {
    const char* s = ::sd_get_system_info();
    return s ? s : "";
}

std::vector<std::string> sd_sample_methods() {
    std::vector<std::string> out;
    for (int i = 0; i < SAMPLE_METHOD_COUNT; ++i) {
        const char* n = ::sd_sample_method_name(static_cast<sample_method_t>(i));
        if (n) out.emplace_back(n);
    }
    return out;
}

std::vector<std::string> sd_schedulers() {
    std::vector<std::string> out;
    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        const char* n = ::sd_scheduler_name(static_cast<scheduler_t>(i));
        if (n) out.emplace_back(n);
    }
    return out;
}

void sd_log_to_stderr(int min_level) {
    sd_set_log_sink([min_level](int level, const std::string& text) {
        if (level < min_level) return;
        static const char* kTag[] = {"debug", "info", "warn", "error"};
        const char* tag = (level >= 0 && level < 4) ? kTag[level] : "info";
        // sd.cpp 自己的消息大多带换行，带了就不再补一个。
        std::fputs(("[sd " + std::string(tag) + "] " + text).c_str(), stderr);
        if (text.empty() || text.back() != '\n') std::fputc('\n', stderr);
        std::fflush(stderr);
    });
}

void sd_set_log_sink(SdLogSink sink) {
    {
        std::lock_guard lg(g_sink_mu);
        g_sink = std::move(sink);
    }
    // 只注册一次就够，回调里查的是那个可变的 sink。
    // 每次都调 sd_set_log_callback 也没坏处，但那样就得考虑
    // 它在别的线程正回调时被换掉——现在换的是 sink，有锁。
    static std::once_flag once;
    std::call_once(once, [] { ::sd_set_log_callback(log_trampoline, nullptr); });
}

#else   // 没链 sd.cpp

bool sd_available() { return false; }
std::string sd_version() { return {}; }
std::string sd_system_info() { return {}; }
std::vector<std::string> sd_sample_methods() { return {}; }
std::vector<std::string> sd_schedulers() { return {}; }
void sd_set_log_sink(SdLogSink) {}
// 没链 sd.cpp 就没有日志可接。**两个分支都要有**——协调者那份是
// CHANGJI_SD=OFF 编的，少了这个链接期就挂，而工作进程那份编得好好的，
// 一半成功一半失败最容易被当成"编译很慢"忽略过去。
void sd_log_to_stderr(int) {}

#endif

}  // namespace changji::infer
