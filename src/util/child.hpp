#pragma once

// 一个一直跑着、靠标准输入输出一行一行说话的子进程（2026-09-24）。
//
// 给 MCP 的本机扩展用：它们是 `npx …` / `uvx …` / `python server.py` 这种
// 命令，起来之后从 stdin 读一行 JSON、往 stdout 写一行 JSON，一直跑到被关掉。
// `proc::run`（跑完就走）和 `proc::spawn`（只起不说话）都做不了这件事。
//
// ---
//
// 几条要紧的：
//
//   · **Windows 上 `npx` 是 `npx.cmd`**：按 PATHEXT 找（同 `proc::which`），
//     找到 .cmd / .bat 就经 cmd.exe 起，命令行照 `proc` 那套引号规矩拼。
//   · **关掉要连根拔**：npx 会再拉起 node，只杀 npx 的话 node 留在后台一直占着
//     端口和内存。Windows 上进程放进一个 Job（`KILL_ON_JOB_CLOSE`），POSIX 上
//     自成一个进程组（`setpgid`），关的时候整组一起杀。
//   · **stderr 不能不接**：扩展往 stderr 打日志，管道满了它就卡在写上，看着像
//     没回话。给了日志文件就写进去，没给就丢进空设备。
//   · **读有超时**：扩展卡住的时候对话不能跟着卡死。后台一条线程一直在读，
//     `read_line` 等的是它攒下的行。
//   · **写也叫得回来**：扩展不读 stdin 时写会卡住（管道缓冲只有几十 KB）。
//     `kill` 能把卡在写上的那一笔叫回来；带超时的 `write_line` 到点自己回来。
//   · **内存有顶**：一行超过 `max_line_bytes` 整行扔掉；攒着没人取的行超过
//     `max_queued_bytes` 读线程就停下不读（扩展写 stdout 跟着卡住），等取走一些
//     再接着读。一个失控的扩展吃不光这台机器的内存。
//   · 起不来（命令找不到、权限不够）的那句话**说人话**，写进 `error`：界面上
//     那一行要摆给人看。

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace changji::proc {

class Child {
public:
    struct Options {
        /// 程序名或路径。名字按 PATH（Windows 上连 PATHEXT）找：`env` 里改了
        /// PATH / PATHEXT 的，先按改过的找，找不到再按这个进程自己的找。
        std::string exe;
        std::vector<std::string> args;
        /// 追加或覆盖的环境变量；其余照这个进程自己的环境继承。
        /// Windows 上名字不分大小写（`Path` 盖得住 `PATH`）。
        std::map<std::string, std::string> env;
        /// 在哪个目录起。空 = 跟这个进程一样。
        /// Windows 上经 cmd.exe 起的批处理不能在网络路径（`\\server\share`）里跑：
        /// cmd 不认，会悄悄换到 C:\Windows——那种情况 `start` 直接说起不来。
        std::filesystem::path cwd;
        /// stderr 写进哪个文件（追加，目录不在就建）。空 = 丢掉。
        std::filesystem::path stderr_log;
        /// stdout 上一行最长多少字节。超过的那一行**整行扔掉**（一直扔到下一个
        /// 换行），`read_line` 拿不到它；给了 `stderr_log` 的话往里记一句。0 = 不设顶。
        std::size_t max_line_bytes = std::size_t{32} << 20;
        /// 读出来还没被 `read_line` 取走的行最多攒多少字节。攒满了读线程停下
        /// 不读，等取走一些再接着读（反压）。0 = 不设顶。
        std::size_t max_queued_bytes = std::size_t{64} << 20;
    };

    /// 起一个。起不来回空，`error` 里写一句人话（「找不到 npx」「……起不来：拒绝访问」）。
    static std::unique_ptr<Child> start(const Options& opts, std::string* error);

    /// 关掉（连它拉起的子进程一起），见 `kill`。
    ~Child();

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    /// 往它的 stdin 写一行（自动补 `\n`）。它已经没了、管道断了回 false。
    /// 对面一直不读的话会一直等，直到对面读了、或者别的线程调了 `kill`。
    bool write_line(const std::string& line);

    /// 同上，但最多等 `timeout_ms`（负数 = 不限）。到点还没写完回 false，
    /// **而且这条 stdin 从此算断了**（之后的写都回 false）：那一行可能已经写进去
    /// 半截，再往后写只会把对面的协议搅乱。等别的线程写完也算在这段时间里——
    /// 那种情况下什么都没写，stdin 不算断。
    bool write_line(const std::string& line, int timeout_ms);

    /// 读一行（不含行尾的 `\r\n` / `\n`）。`timeout_ms` 内没有新的一行回空；
    /// 它已经退出、stdout 读完了也回空——用 `alive()` 分这两种。
    std::optional<std::string> read_line(int timeout_ms);

    /// 还在跑。
    bool alive() const;

    /// 退出码。还在跑回空。被信号打死的（POSIX）记成 128 + 信号号；
    /// 被别处收走、拿不到的记成 -1。
    std::optional<int> exit_code() const;

    /// 关掉：先把卡在写上的那一笔叫回来、关它的 stdin（守规矩的扩展读到 EOF
    /// 自己退），等 `grace_ms`，然后整组杀掉（它自己退了也杀：它拉起的不一定跟着退）。
    /// 可以重复调，可以从别的线程调。
    void kill(int grace_ms = 1500);

    struct Impl;

private:
    explicit Child(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace changji::proc
