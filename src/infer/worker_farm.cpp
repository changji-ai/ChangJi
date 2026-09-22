#include "infer/worker_farm.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/proc.hpp"

namespace fs = std::filesystem;

namespace changji::infer {

int worker_port_for(int base_port, int gpu) { return base_port + gpu; }

struct WorkerFarm::Impl {
    std::vector<proc::ProcHandle> kids;

    ~Impl() {
        // **倒着杀。** 没什么强理由，但和拉起顺序相反读起来更像栈，
        // 而且真出问题时日志顺序好对。
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            proc::kill_spawned(*it);
        }
    }
};

WorkerFarm::WorkerFarm() : impl_(std::make_unique<Impl>()) {}
WorkerFarm::~WorkerFarm() = default;

std::shared_ptr<WorkerFarm> WorkerFarm::start(
    const config::Settings& settings, const models::HardwareProfile& profile,
    HealthProbe healthy) {
    if (!settings.workers.endpoints.empty()) return nullptr;
    if (!settings.workers.auto_spawn) return nullptr;

    const int gpus = profile.gpu.has_value() ? profile.gpu->count : 1;
    if (gpus <= 1) return nullptr;

    const fs::path self = paths::self_exe();
    if (self.empty()) {
        std::fputs(SAY_NEVER("[多卡] 取不到自己的可执行路径，退回单卡进程内跑。\n"),
                   stderr);
        return nullptr;
    }

    std::shared_ptr<WorkerFarm> farm(new WorkerFarm());
    const int base = settings.workers.base_port;
    for (int gpu = 0; gpu < gpus; ++gpu) {
        const int port = worker_port_for(base, gpu);
        const std::string log =
            paths::to_utf8(settings.workspace_path() /
                           ("worker-" + std::to_string(gpu) + ".log"));
        const auto h = proc::spawn(
            paths::to_utf8(self),
            {"--worker", "--gpu", std::to_string(gpu), "--port",
             std::to_string(port)},
            paths::from_utf8(log));
        if (h == 0) {
            std::fprintf(stderr, SAY_NEVER("[多卡] 卡 %d 的工作进程起不来，跳过。\n"), gpu);
            continue;
        }
        farm->impl_->kids.push_back(h);
        const std::string base_url = "http://127.0.0.1:" + std::to_string(port);
        // **等它真的能应答再算数。** 只看 fork 成功的话，模型载不起来的
        // 那张卡会被当成可用的，然后每一镜派过去都失败——而 WorkerRoster
        // 要连着失败几次才会把它隔离，那几镜的重试次数就白烧了。
        // 没给探活函数就只信"进程起来了"——那是测试路径，
        // 生产里 run_deps 一定会传一个真的。
        if (healthy && !healthy(base_url, 120)) {
            std::fprintf(stderr,
                         SAY_NEVER("[多卡] 卡 %d 的工作进程两分钟没应答，"
                                   "跳过。日志在 %s\n"),
                         gpu, log.c_str());
            proc::kill_spawned(h);
            farm->impl_->kids.pop_back();
            continue;
        }
        farm->endpoints_.push_back(base_url);
        std::fprintf(stderr, SAY_NEVER("[多卡] 卡 %d 就绪：%s\n"), gpu, base_url.c_str());
    }

    if (farm->endpoints_.empty()) {
        std::fputs(SAY_NEVER("[多卡] 一个工作进程都没起来，退回单卡进程内跑。\n"), stderr);
        return nullptr;
    }
    std::fprintf(stderr, SAY_NEVER("[多卡] %zu 张卡就绪\n"), farm->endpoints_.size());
    return farm;
}

}  // namespace changji::infer
