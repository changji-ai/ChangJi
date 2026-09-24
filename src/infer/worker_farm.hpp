#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/hardware.hpp"

namespace changji::infer {

/// 多卡时自己把每张卡的工作进程拉起来。
///
/// **"一个程序运行所有"不该等于"只能用一张卡"。** 用户启动的仍然是一个
/// 命令，多卡的编排由它自己做：探到 N 张卡就拉起 N 个 `--worker --gpu i`
/// 的子进程，把地址填进工作进程池，退出时收掉。
///
/// **为什么是子进程而不是进程内开 N 个上下文。** sd.cpp 的进度回调是
/// 全局的（`sd_set_progress_callback` 没有 user data 能区分是哪个上下文），
/// 进程内并发跑两张卡的话，两路进度会串到一起——而进度是判断"卡住没有"
/// 的唯一依据。另外 CUDA 设备是按进程选的，一个进程里换来换去容易出
/// 难复现的问题。子进程这条路把这两件事都绕开了，而且工作进程协议
/// 本来就有（多机部署用的是同一套）。
///
/// 拉起来的是**自己这一份可执行文件**（`paths::self_exe()`）。靠 PATH 找
/// "changji" 会找到别的构建——机器上常常有好几个 build- 目录，
/// 那时候症状是"工作进程行为和主进程对不上"，极难联想到是版本不同。
class WorkerFarm {
public:
    /// 按配置和这台机器的显卡拉起工作进程。
    ///
    /// 下面任何一条成立就**不拉，返回 nullptr**（调用方退回进程内单卡）：
    ///   - `[workers].endpoints` 非空：用户自己指定了，包括跨机的情况
    ///   - `[workers].auto_spawn = false`
    ///   - 只探到一张卡：单卡进程内跑更省事，也少一次进程间的图片搬运
    ///   - 取不到自己的可执行路径
    ///
    /// 起不来的那几张卡会被跳过（日志里说清是哪张）；一张都起不来时
    /// 同样返回 nullptr。**不抛异常**：多卡拉不起来该退回单卡继续干活，
    /// 而不是让整个服务起不来。
    /// `healthy(地址, 等几秒)` 用来确认某个工作进程真的能应答。
    /// **做成参数而不是写死**：真实那份要发 HTTP，而 HTTP 只链进主目标，
    /// 写死会让测试目标链不过。上面四种"不该动手"的情况根本走不到它。
    using HealthProbe = std::function<bool(const std::string&, int)>;

    /// ⚠️ **多卡给别的机器用这件事，2026-09-17 做砸过一次，证据和结论在这儿。**
    ///
    /// 背景：主程序挂上节点协议之后（一台机器一个进程、一条连接），外来任务
    /// 在主进程里就地跑，而一个进程只用得上一张卡——用户：「只用了一张卡」。
    /// 方向是对的：外来任务得交给这儿拉起的那几个子进程。**实现错了两处**：
    ///
    ///   1. 挂载点的槽数设成了卡数（2），而这儿只成功拉起了一个子进程
    ///      （卡 0 那个两分钟没应答被跳过）。接了两件却只有一个干得动，
    ///      槽永远不空，后面 51 次 `POST /task` 全 409。
    ///      **槽数必须等于这儿活着的子进程数**（`endpoints().size()`），
    ///      拿不到就按 1。
    ///   2. farm 是被第一件外来任务**懒**拉起的：那个任务线程堵在这儿
    ///      探活（每个最多两分钟），期间新来的全 409。**必须在开始接活之前
    ///      就绪**——起服务时后台预热，任务只在 `endpoints()` 非空时才派。
    ///
    /// 卡 0 那个子进程**不是坏的**：手动 `--worker --gpu 0 --port 9011` 和
    /// `--gpu 1 --port 9012` 并排起，两个都在 30 秒内应答 /health。它在
    /// farm 里两分钟不应答，是懒拉起时探活和拉起挤在一起拖的——正是第 2 条。
    ///
    /// 那两条回退过（c6c92a9），之后重做（977b056）——**真正的根因是第三处**：
    /// 子进程 `/health` 只收 GET 而探活发 POST，405 当没应答，自动拉起
    /// 从来没成功过。修掉之后两个子进程 30 秒内都就绪；槽数改成回调
    /// （活着的子进程数，`http/run.hpp` 的 FarmRunner::capacity），预热挪到
    /// 起服务时后台跑。最后一截在派活那边：那台在 `/status` 里报 `slots`，
    /// 这边按它开槽（node_pick.hpp 的 `remote_slots_for`）——不然池只给它
    /// 一个槽，第二张卡照样闲着。
    static std::shared_ptr<WorkerFarm> start(
        const config::Settings& settings,
        const models::HardwareProfile& profile, HealthProbe healthy = {});

    ~WorkerFarm();
    WorkerFarm(const WorkerFarm&) = delete;
    WorkerFarm& operator=(const WorkerFarm&) = delete;

    /// 起来了的那几个的地址，直接喂给 `make_worker_pool`。
    const std::vector<std::string>& endpoints() const { return endpoints_; }

    /// 真正起来了几个。
    std::size_t size() const { return endpoints_.size(); }

    /// **此刻活着的**有几个。死了的那一个在被重拉回来之前不算——报槽数（`/status`
    /// 的 `slots`）要用它，不是用起服务时起来了几个。
    std::size_t alive() const;

private:
    WorkerFarm();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> endpoints_;
};

/// 第 `gpu` 张卡的工作进程该监听哪个端口。
///
/// 单独拿出来是为了能测：端口撞了的表现是"某张卡的工作进程起不来"，
/// 而日志里只有一句连不上，看不出是端口规则算错了。
int worker_port_for(int base_port, int gpu);

}  // namespace changji::infer
