#include "infer/worker_farm.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/proc.hpp"

namespace fs = std::filesystem;

namespace changji::infer {

int worker_port_for(int base_port, int gpu) { return base_port + gpu; }

/// 一张卡上那个子进程。重拉要知道它原来是怎么起的。
struct Kid {
    proc::ProcHandle h = 0;
    int gpu = 0;
    int port = 0;
    std::string log;
    int restarts = 0;
};

struct WorkerFarm::Impl {
    std::vector<Kid> kids;
    std::string exe;

    /// 看着那几个子进程的那条线程（见 `watch`）。
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    std::thread watcher;

    static proc::ProcHandle launch(const std::string& exe, const Kid& k) {
        return proc::spawn(exe,
                           {"--worker", "--gpu", std::to_string(k.gpu), "--port",
                            std::to_string(k.port)},
                           paths::from_utf8(k.log));
    }

    /// **死了的卡拉回来。** 原来一个子进程死了（CUDA 显存不够当场 abort 是常
    /// 见的）就再也没人管：那张卡一直闲到整个程序重启，而槽数照旧按起服务时
    /// 那几个报——别的机器看这台"还有位置"，派过来的活挤在这台的池子里排队，
    /// 而不是去一台真闲着的机器。
    ///
    /// 同一个端口重拉：派活那头的池子照旧认这个地址，连不上那几下它自己会冷却、
    /// 过一会儿再试（`WorkerRoster`）。**一张卡最多重拉 kMaxRestarts 次**——
    /// 一起来就崩的卡（模型文件坏了、驱动不对）不能每十秒拉一遍。
    void watch() {
        constexpr int kMaxRestarts = 5;
        std::unique_lock lk(mu);
        while (!cv.wait_for(lk, std::chrono::seconds(10), [this] { return stop; })) {
            for (Kid& k : kids) {
                if (k.h != 0 && proc::alive(k.h)) continue;
                if (k.restarts >= kMaxRestarts) continue;
                ++k.restarts;
                if (k.h != 0) proc::kill_spawned(k.h, 0);   // 收掉句柄
                k.h = launch(exe, k);
                std::fprintf(stderr,
                             SAY_NEVER("[多卡] 卡 %d 的工作进程没了，重拉（第 %d 次）\n"),
                             k.gpu, k.restarts);
            }
        }
    }

    std::size_t alive() {
        std::lock_guard lg(mu);
        std::size_t n = 0;
        for (const Kid& k : kids) {
            if (k.h != 0 && proc::alive(k.h)) ++n;
        }
        return n;
    }

    ~Impl() {
        {
            std::lock_guard lg(mu);
            stop = true;
        }
        cv.notify_all();
        if (watcher.joinable()) watcher.join();
        // **倒着杀。** 没什么强理由，但和拉起顺序相反读起来更像栈，
        // 而且真出问题时日志顺序好对。
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            if (it->h != 0) proc::kill_spawned(it->h);
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
    farm->impl_->exe = paths::to_utf8(self);
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
        farm->impl_->kids.push_back(Kid{h, gpu, port, log, 0});
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
    Impl* impl = farm->impl_.get();
    impl->watcher = std::thread([impl] { impl->watch(); });
    return farm;
}

std::size_t WorkerFarm::alive() const { return impl_->alive();
}

}  // namespace changji::infer
