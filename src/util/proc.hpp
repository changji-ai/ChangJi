// 子进程与可执行文件查找。
//
// 体检要跑 ffmpeg -version、nvidia-smi、fc-list；后面的装配环节
// 整个都是拉起 ffmpeg。先把这层抽出来，免得每处各写一遍管道。

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace changji::proc {

struct Result {
    int exit_code = -1;
    std::string out;   ///< stdout 和 stderr 合并。ffmpeg 习惯把版本信息写到 stderr
    /// 进程有没有起来。区分「跑了但失败」和「根本没这个程序」。
    ///
    /// **靠先查一遍 PATH 得出，不是靠 popen 的返回值**——popen 只要
    /// shell 起得来就算成功，命令不存在它也返回非空。
    /// `media/ffmpeg.cpp` 靠这个标志决定说"找不到 ffmpeg"还是
    /// "ffmpeg 报错了"，两句话把人指向完全不同的方向。
    bool launched = false;

    /// 超时被杀掉了。
    ///
    /// 原来 popen 根本没法设超时——一个卡死的 ffmpeg 会把整个工作线程钉住，
    /// 而外面看到的只是"这一章一直在跑"，没有任何别的信号。
    /// 换成 CreateProcessW / fork 之后才有了这一项。
    bool timed_out = false;
};

/// 在 PATH 里找可执行文件，返回绝对路径。找不到返回 nullopt。
/// 相当于 Unix 的 which / Windows 的 where。
std::optional<std::string> which(const std::string& name);

/// 跑一个命令并抓取输出。
///
/// 参数逐个传。**不经过 shell**：Windows 上是 CreateProcessW，
/// 别处是 fork + execv。
///
/// 原来走的是 popen（也就是交给 cmd.exe / sh），换掉是因为踩了两次：
/// cmd 把 `,` `;` `=` 也当参数分隔符，ffmpeg 的滤镜串会被切碎；
/// 而且 popen 没法设超时。不经过 shell 之后这两类问题都不存在了。
///
/// timeout_ms 为 0 表示不限时；超时会杀掉子进程并把 `timed_out` 置位。
///
/// `stdin_data` 非空就喂给子进程的标准输入，喂完关掉（子进程才看得到 EOF）。
/// 空串 = 不接管，沿用父进程的 stdin，行为和以前一模一样。
///
/// **为什么要有它。** 大模型的命令行后端（claude / codex）要吃的提示词
/// 动辄几万字，而命令行参数是有上限的——Windows 上整条命令行 32 KB 封顶，
/// 超了 CreateProcessW 直接失败。走标准输入没有这个限制，而且 `claude -p`
/// 的帮助里写的就是 "useful for pipes"。
///
/// ⚠️ **喂和读必须并行**：管道缓冲区只有几 KB，父进程闷头写完再去读的话，
/// 一旦子进程先把 stdout 写满就双方互等——两边都不动，最后靠超时才收场。
/// 所以下面是写一条线程、读一条线程。
Result run(const std::string& exe,
           const std::vector<std::string>& args,
           int timeout_ms = 15000,
           const std::string& stdin_data = {});

/// 起一个**不等它结束**的子进程，返回一个能用来杀它的句柄。
///
/// `run` 是同步的：起了就等，等到它退出或者超时。工作进程要跑几小时，
/// 那条路用不上。
///
/// 子进程的 stdout/stderr 追加写到 `log`（空则丢弃）。**不能继承父进程的**：
/// 多张卡就是多个子进程，混在一起的日志分不清是哪张卡出的错。
///
/// 返回 0 表示起不来。句柄在 POSIX 上是 pid，Windows 上是进程 id。
using ProcHandle = std::uint64_t;
ProcHandle spawn(const std::string& exe, const std::vector<std::string>& args,
                 const std::filesystem::path& log);

/// 杀掉 `spawn` 起的那个。已经退了的话什么都不做。
///
/// 先客气地要求退出（POSIX 是 SIGTERM，Windows 直接 Terminate——那边没有
/// 对应的东西），等 `grace_ms`，还活着就来硬的。
/// **要留出宽限期**：工作进程收到 SIGTERM 会把当前这一镜取消掉再退，
/// 直接 SIGKILL 会留下半截的 mp4。
void kill_spawned(ProcHandle h, int grace_ms = 5000);

/// 那个进程还活着吗。
bool alive(ProcHandle h);

}  // namespace changji::proc
