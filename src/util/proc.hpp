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

/// 同 `which`，只是 PATH（Windows 上连 PATHEXT）用给的这两串，不读这个进程的环境。
/// `pathext` 空 = 默认那一套（.COM;.EXE;.BAT;.CMD）；别的系统上不看它。
///
/// 给要替子进程换 PATH 的调用方用：`Child` 的 `Options::env` 里写了 PATH，
/// 程序就该先按那个 PATH 找——子进程自己找东西也是按那个找的。
std::optional<std::string> which_in(const std::string& name, const std::string& path_env,
                                    const std::string& pathext);

/// 按 CommandLineToArgvW 的规则给一个参数加引号（Windows 上拼命令行用）。
///
/// `run` / `spawn` 用的就是这一份；`Child`（util/child.cpp）起 .exe 时也走它。
/// **收成一处**：引号规则各写一遍的话，哪天改了一处，另一处照旧把
/// 带空格的路径切成两截，而那种错只在某个参数长得特别时才露出来。
std::string quote_arg(const std::string& s);

#ifdef _WIN32
/// （Windows）CreateProcessW，子进程**只**继承给它的那几个标准句柄。
///
/// 这个进程是多线程的服务：`run`、`spawn`、`Child` 随时在不同线程上起子进程。
/// 老办法是 bInheritHandles=TRUE、"继承全部可继承句柄"——那一刻别的线程上
/// 恰好可继承的管道就漏进这个子进程里。漏进去的后果都是"管道等不到 EOF"：
/// `run` 卡在 join 上，`Child` 的 kill 叫不回卡在写上的那一笔。
///
/// 做法：调用方的句柄一直**不可继承**；这里给每个标准句柄复制一份可继承的，
/// 用 PROC_THREAD_ATTRIBUTE_HANDLE_LIST 点名只给这几份，起完就关。
///
/// 句柄写成 void*：这个头不带 windows.h。`std_*` 三个全空 = 不接标准输入输出
/// （不设 STARTF_USESTDHANDLES，什么都不继承）；某一路给空或无效句柄 = 那一路不接。
/// `app` / `env` / `cwd` 可以是空指针，含义同 CreateProcessW。成功回 0，
/// `*process` / `*thread` 由调用方关；失败回 GetLastError 的值。
unsigned long create_process_std(const wchar_t* app, wchar_t* cmdline, unsigned long flags,
                                 void* env, const wchar_t* cwd, void* std_in, void* std_out,
                                 void* std_err, void** process, void** thread,
                                 unsigned long* pid);
#endif

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
///
/// Windows 上**不开窗口**（CREATE_NO_WINDOW：桌面端是窗口程序，不带这一条的话
/// 每起一个 curl 就弹一个黑框），而且进一个作业对象：杀的时候连它拉起的子进程
/// 一起杀，引擎自己没了它们也跟着走。理由写在 proc.cpp 的 `jobs()` 上。
using ProcHandle = std::uint64_t;
ProcHandle spawn(const std::string& exe, const std::vector<std::string>& args,
                 const std::filesystem::path& log);

/// 杀掉 `spawn` 起的那个。已经退了的话什么都不做。
///
/// 先客气地要求退出（POSIX 是 SIGTERM，Windows 直接结束它那一整个作业——
/// 那边没有对应的东西），等 `grace_ms`，还活着就来硬的。Windows 上 `grace_ms`
/// 是"最多等它退干净多久"：调用方接着要动它写过的文件，还被占着就动不了。
/// **要留出宽限期**：工作进程收到 SIGTERM 会把当前这一镜取消掉再退，
/// 直接 SIGKILL 会留下半截的 mp4。
void kill_spawned(ProcHandle h, int grace_ms = 5000);

/// 那个进程还活着吗。
bool alive(ProcHandle h);

}  // namespace changji::proc
