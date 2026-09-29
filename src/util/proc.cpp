#include "util/proc.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace changji::proc {

namespace fs = std::filesystem;

namespace {

#ifndef _WIN32
/// fork 之后、execv 之前，把**继承来的其余 fd 全关掉**。
///
/// 不关的话，子进程会拿着父进程当时开着的一切——其中要命的是那张
/// **监听套接字**。实测撞到的：worker 正在下 82 GB 模型（四个 curl 子
/// 进程），把 worker 本身杀掉之后，端口仍然被占着——
///
///   LISTEN 127.0.0.1:9101 users:(("curl",pid=16500,fd=19),…)
///
/// 于是 worker 重启不起来，报的是 `bind: Address already in use`，
/// 而真凶是它自己派生的下载进程，还要等几十分钟下完才肯松手。
/// 那句报错完全指不到这上面，人只会去找"谁占了 9101"。
///
/// 上界在 fork 之前算好：`sysconf` 不在 async-signal-safe 那张表上，
/// 而 fork 和 exec 之间只能调表上的函数。`close` 在表上。
int fd_upper_bound() {
    const long n = ::sysconf(_SC_OPEN_MAX);
    // 取不到就按 POSIX 的下限兜底；上限钉住，免得在 _SC_OPEN_MAX 是
    // 一百万的机器上空转一百万次 close。真实的 fd 都挤在最小的那几十个里。
    if (n <= 0) return 256;
    return static_cast<int>(n > 4096 ? 4096 : n);
}

void close_inherited_fds(int upper) {
    for (int fd = STDERR_FILENO + 1; fd < upper; ++fd) ::close(fd);
}
#endif

#ifdef _WIN32
/// `spawn` 起的每一个进程各配一个**作业对象**（Job Object），按进程 id 记着。
///
/// **为什么要有。** 原来的杀法是先 `GenerateConsoleCtrlEvent` 再 `TerminateProcess`
/// 那**一个**进程——够不着它的子进程。scoop / chocolatey 装的 `aria2c.exe` 是个
/// 垫片，真干活的那个 aria2c 是它拉起来的孙子：杀掉垫片，孙子照下不误；再点一次
/// 下载又开一个，两个进程往同一个 `.part` 里写，文件越写越大（downloader.cpp 里
/// 「比应有的还大」那一段在 Linux 上就是这么栽的，那边是 setsid 出来的孤儿）。
/// 2026-09-24 修「下载模型停不下来」时一并堵上。
///
/// 进了作业对象之后：
///   · 杀的时候 `TerminateJobObject`，**整棵树一起走**；
///   · 设了 KILL_ON_JOB_CLOSE：引擎自己没了（崩了、被任务管理器结束），句柄
///     一关，底下的也跟着走，不留孤儿接着写盘、接着占端口。
///
/// 加不进去（老系统上外面那层作业不许嵌套）就退回原来那样只杀一个，不拦着起。
/// **进程句柄也一直拿着**：拿着它，这个进程 id 就不会被系统挪给别的进程。原来只按
/// id 记、起完就关句柄——它退了之后 Windows 很快把这个 id 发给别人，之后的
/// `kill_spawned` 拿 id 去 OpenProcess + TerminateProcess，结束的是**那个不相干的
/// 进程**（工作进程崩了五次不再拉起、那一格留着死 id，关停时照杀）。
struct Spawned {
    HANDLE job = nullptr;       ///< 可能是空（加不进作业对象的老系统）
    HANDLE process = nullptr;
};
std::mutex& jobs_mu() {
    static std::mutex m;
    return m;
}
std::map<DWORD, Spawned>& jobs() {
    static std::map<DWORD, Spawned> m;
    return m;
}
/// 取走这个进程那一条（取走就不在表上了）。不在表上（不是 spawn 起的、或者早就
/// 收过了）回两个空。
Spawned take_spawned(DWORD pid) {
    std::lock_guard<std::mutex> lock(jobs_mu());
    const auto it = jobs().find(pid);
    if (it == jobs().end()) return {};
    const Spawned s = it->second;
    jobs().erase(it);
    return s;
}
void close_spawned(const Spawned& s) {
    if (s.job != nullptr) ::CloseHandle(s.job);
    if (s.process != nullptr) ::CloseHandle(s.process);
}
#else
/// `spawn` 起的、**还没收过尸**的那几个进程。收了尸（`waitpid` 拿到了它）这个 pid
/// 就还给系统了，随时会发给别的进程——之后再拿它 `kill` 打到的是别人（远端那台
/// 是 root 跑的）。所以收过的从表上拿掉，`alive` / `kill_spawned` 只认表上的。
std::mutex& spawned_mu() {
    static std::mutex m;
    return m;
}
std::set<pid_t>& spawned() {
    static std::set<pid_t> s;
    return s;
}
#endif


/// 给参数加引号，按 **CommandLineToArgvW 的规则**。
///
/// 从 2026-09-08 起 `run` 不再经过 cmd.exe（改走 CreateProcessW），
/// 所以这里要迁就的是 C 运行时的命令行解析，不是 shell 的。
/// 两套规则差很多——cmd 会把 `,` `;` `=` 也当分隔符，还会展开 `%VAR%`；
/// CommandLineToArgvW 只认空格和制表符做分隔，引号里什么都是字面量。
///
/// 规则本身（微软文档 "Parsing C++ Command-Line Arguments"）：
///   - 2n 个反斜杠后跟引号  → n 个反斜杠，引号起界定作用
///   - 2n+1 个反斜杠后跟引号 → n 个反斜杠加一个字面引号
///   - 不跟引号的反斜杠     → 原样
/// 所以只有**紧挨着引号的**那些反斜杠要翻倍，路径里的 `C:\a\b` 不用动。
std::string quote(const std::string& s) {
    if (s.empty()) return "\"\"";
    // **判断"要不要引"时用的是并集，比 argv 规则宽。**
    //
    // argv 规则只把空格和制表符当分隔符，照理只需要看这两个。但
    // `which()` 可能返回一个 **.bat/.cmd**（PATHEXT 里就有，而 ffmpeg
    // 的某些装法正是 .bat 包装），而 Windows 跑 .bat 一定要经过
    // cmd.exe——那时候 `,` `;` `=` `&` 这些又变回分隔符了。
    // 多引几个字符对 .exe 没有害处，对 .bat 是必需的。
    if (s.find_first_of(" \t\"'&|<>(),;=^") == std::string::npos) return s;

    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : s) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');  // 翻倍，再加一个转义引号
            out += '"';
        } else {
            out.append(slashes, '\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, '\\');  // 结尾的反斜杠要翻倍，否则会转义掉收尾的引号
    out += '"';
    return out;
}

}  // namespace

std::string quote_arg(const std::string& s) { return quote(s); }

namespace {

/// `run` 收输出的上限。
constexpr std::size_t kRunOutMax = 1u << 20;

/// 往收着的输出里接一段，**到上限就不再接，但管道照读**（读的那头见
/// `drain_*`）。原来到上限就不读了——子进程下一次写就卡在满了的管道上：不设
/// 超时的（出片那一步的编码）永远不回来，设了超时的白等到超时、报成超时。
/// 截口落在字的边界上：半个字进了报错，一路到 `project.json` 存盘时 dump 抛。
void append_capped(std::string& out, const char* p, std::size_t n, bool& over, bool mark) {
    if (over) return;
    if (out.size() + n <= kRunOutMax) {
        out.append(p, n);
        return;
    }
    out.append(p, kRunOutMax - out.size());
    out = text::drop_partial_utf8_tail(std::move(out));
    if (mark) out += "\n...(输出过长，已截断)\n";
    over = true;
}

/// 进程退了之后，读管道的最多再等这么久。管道的 EOF 要等**所有**拿着写端的
/// 进程都关掉——子进程自己退了、它拉起的孙进程还拿着（批处理、`sh wrap.sh`、
/// venv 的 python 启动器都是这样），读线程就一直等下去，`run` 永不返回。
constexpr auto kDrainGrace = std::chrono::seconds(2);

/// 等读线程自己收摊，最多等 `kDrainGrace`；到点还没收就叫它们停。
void settle_readers(std::atomic<bool>& stop, const std::atomic<int>& running) {
    const auto until = std::chrono::steady_clock::now() + kDrainGrace;
    while (running.load() > 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true);
}

#ifdef _WIN32
/// 读一条匿名管道直到写端全关，或者 `stop`。**不能闷头 ReadFile**：那一下要等
/// 到有数据或者 EOF 才回来，叫它停也停不下（见 `kDrainGrace`）。先 Peek，有
/// 多少读多少，没有就歇一下再看。
void drain_pipe(HANDLE h, std::string& out, const std::atomic<bool>& stop, bool mark) {
    std::array<char, 4096> buf{};
    bool over = false;
    for (;;) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;  // 写端全关了
        if (avail == 0) {
            if (stop.load()) break;
            ::Sleep(5);
            continue;
        }
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(avail, buf.size()));
        if (!::ReadFile(h, buf.data(), want, &got, nullptr) || got == 0) break;
        append_capped(out, buf.data(), got, over, mark);
    }
}
#else
/// 同上，POSIX：poll 着读，`stop` 了就不等了。
void drain_fd(int fd, std::string& out, const std::atomic<bool>& stop, bool mark) {
    std::array<char, 4096> buf{};
    bool over = false;
    for (;;) {
        pollfd pf{fd, POLLIN, 0};
        const int ready = ::poll(&pf, 1, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            if (stop.load()) break;
            continue;
        }
        const ssize_t got = ::read(fd, buf.data(), buf.size());
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        append_capped(out, buf.data(), static_cast<std::size_t>(got), over, mark);
    }
}
#endif

}  // namespace

/// 经 cmd.exe 起批处理时给一个参数加引号。
///
/// **跟 `proc::quote_arg` 不是一套规矩。** 批处理一定过 cmd.exe，而 cmd 有
/// 两件 CommandLineToArgvW 没有的事：
///   · 引号是开关，`\"` 在它眼里是"反斜杠 + 开关一下"——引号里的 `&` 跟着
///     漏到引号外面，后半截就成了另一条命令。所以引号写成 `""`（开关两下，
///     状态不变）；
///   · `%VAR%` 在引号里照样展开，而命令行模式下 `%%` 不是转义。借的是 Rust
///     标准库修 BatBadBut（CVE-2024-24576）时的办法：每个 `%` 前面垫一个
///     `%cd:~,%`（取 cd 的空子串，展开成空），让 cmd 配不成一对。
/// 除了字母数字和少数几个安全的符号，ASCII 里别的都引上；非 ASCII 不用引。
///
/// 带引号的参数到了那头拆成什么，看批处理最后把 `%*` 交给谁：2026-09-24 实测
/// 经 .cmd 交给 node（npx.cmd 就是这样），带引号、`&`、`%CD%`、`!`、`^` 的
/// 十几种参数全部原样到达；交给按 CommandLineToArgvW 拆的程序，带引号的那个
/// 会拆歪（`""` 在它那儿会顺带结束引号）。但两种情况下 `&` 都漏不出去——
/// 拆歪只是参数不对，漏出去是多跑一条命令。
std::string cmd_quote_arg(const std::string& s, bool force) {
    bool need = force || s.empty() || s.back() == '\\';
    constexpr std::string_view kSafe = "#$*+-./:?@\\_";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80) continue;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if (kSafe.find(ch) != std::string_view::npos) continue;
        need = true;
    }
    std::string out;
    if (need) out += '"';
    std::size_t slashes = 0;
    for (const char c : s) {
        if (c == '\\') {
            ++slashes;
            out += c;
            continue;
        }
        if (c == '"') {
            out.append(slashes, '\\');  // 紧挨引号的反斜杠翻倍，再补一个引号成 `""`
            out += '"';
        } else if (c == '%') {
            out += "%%cd:~,";
        }
        slashes = 0;
        out += c;
    }
    if (need) {
        out.append(slashes, '\\');  // 结尾的反斜杠翻倍，否则吃掉收尾的引号
        out += '"';
    }
    return out;
}

bool is_batch_file(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || path.find_first_of("/\\", dot) != std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ".cmd" || ext == ".bat";
}

std::optional<std::string> batch_command_line(const std::string& exe,
                                              const std::vector<std::string>& args) {
    // /s /c "…"：cmd 去掉最外面那一对引号，里面原样当一行命令；/d 不跑 AutoRun
    //（注册表里谁挂了什么都别掺进来）；/e:ON 给 `%cd:~,%` 那个垫子用；/v:OFF
    // 让 `!` 不当延迟展开。
    for (const auto& a : args) {
        if (a.find_first_of(std::string("\r\n\0", 3)) != std::string::npos) return std::nullopt;
    }
    std::string line = "cmd.exe /e:ON /v:OFF /d /s /c \"";
    line += cmd_quote_arg(exe, true);
    for (const auto& a : args) {
        line += ' ';
        line += cmd_quote_arg(a, false);
    }
    line += '"';
    return line;
}

#ifdef _WIN32
std::wstring cmd_exe_path() {
    // 不读 COMSPEC：那是用户环境里的一个变量，改过的话起的就不是 cmd 了。
    wchar_t buf[MAX_PATH];
    const UINT n = ::GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\Windows\\System32\\cmd.exe";
    return std::wstring(buf, n) + L"\\cmd.exe";
}

namespace {

/// 起一个程序的命令行（和 lpApplicationName）。**批处理要经 cmd.exe 起**：
/// `which` 按 PATHEXT 找得到 `.bat` / `.cmd`，CreateProcessW 遇到它们自己悄悄
/// 转给 cmd.exe，而 `quote` 那套 `\"` 在 cmd 眼里是引号开关——参数里一个 `"`
/// 后面的 `&`、`|`、`%VAR%` 就漏成了另一条命令（BatBadBut）。`{prompt}` 里是
/// 故事梗概，人贴的、模型写的都有。参数里有换行的批处理起不了，回 false。
bool windows_command(const std::string& resolved, const std::vector<std::string>& args,
                     std::wstring& app, std::wstring& cmd) {
    if (is_batch_file(resolved)) {
        const auto line = batch_command_line(resolved, args);
        if (!line) return false;
        app = cmd_exe_path();
        cmd = paths::from_utf8(*line).wstring();
        return true;
    }
    app.clear();
    cmd = paths::from_utf8(quote(resolved)).wstring();
    for (const auto& a : args) {
        cmd += L" ";
        cmd += paths::from_utf8(quote(a)).wstring();
    }
    return true;
}

}  // namespace
#endif

std::optional<std::string> which(const std::string& name) {
    return which_in(name, paths::env("PATH"), paths::env("PATHEXT"));
}

std::optional<std::string> which_in(const std::string& name, const std::string& path_env,
                                    const std::string& pathext_in) {
    // 已经是个能直接用的路径就不用找了。
    // from_utf8：`fs::exists(std::string)` 在 MSVC 上按 ANSI 代码页转，
    // 路径里有中文（用户名、片子目录）就当场抛。
    std::error_code ec;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        if (fs::exists(paths::from_utf8(name), ec)) return name;
    }

    if (path_env.empty()) return std::nullopt;

#ifdef _WIN32
    const char sep = ';';
    // PATHEXT 决定哪些后缀算可执行。写死 .exe 会漏掉 .bat 包装的工具，
    // ffmpeg 的某些安装方式就是这样。
    std::vector<std::string> exts;
    std::string pathext = pathext_in;
    if (pathext.empty()) pathext = ".COM;.EXE;.BAT;.CMD";
    {
        size_t start = 0;
        while (start <= pathext.size()) {
            size_t end = pathext.find(';', start);
            if (end == std::string::npos) end = pathext.size();
            std::string e = pathext.substr(start, end - start);
            if (!e.empty()) exts.push_back(e);
            start = end + 1;
        }
    }
    exts.push_back("");  // 名字里已经带后缀的情况
#else
    (void)pathext_in;
    const char sep = ':';
    const std::vector<std::string> exts{""};
#endif

    size_t start = 0;
    while (start <= path_env.size()) {
        size_t end = path_env.find(sep, start);
        if (end == std::string::npos) end = path_env.size();
        std::string dir = path_env.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;

        for (const auto& ext : exts) {
            fs::path candidate = paths::from_utf8(dir) / paths::from_utf8(name + ext);
            if (fs::is_regular_file(candidate, ec)) {
                return paths::to_utf8(candidate);
            }
        }
    }
    return std::nullopt;
}

#ifdef _WIN32
unsigned long create_process_std(const wchar_t* app, wchar_t* cmdline, unsigned long flags,
                                 void* env, const wchar_t* cwd, void* std_in, void* std_out,
                                 void* std_err, void** process, void** thread,
                                 unsigned long* pid) {
    const HANDLE self = ::GetCurrentProcess();
    const HANDLE src[3] = {std_in, std_out, std_err};
    const bool use_std = std_in != nullptr || std_out != nullptr || std_err != nullptr;

    // 每一路复制一份**可继承**的给子进程。调用方自己那份一直不可继承，
    // 别的线程上同时在起的子进程（哪怕它不点名）也拿不到它。
    // 同一个句柄给了两路（run 的 stdout 和 stderr 是同一条管道）就共用一份：
    // HANDLE_LIST 里不许有重复。
    HANDLE dup[3] = {nullptr, nullptr, nullptr};
    std::vector<HANDLE> inherit;
    for (int i = 0; i < 3; ++i) {
        if (src[i] == nullptr || src[i] == INVALID_HANDLE_VALUE) continue;
        for (int j = 0; j < i; ++j) {
            if (src[j] == src[i]) dup[i] = dup[j];
        }
        if (dup[i] != nullptr) continue;
        // 复制不了（比如这个进程根本没有控制台，GetStdHandle 给的是个空壳）
        // 就那一路不接，子进程照起——跟以前"继承到一个无效句柄"是一个结果。
        if (::DuplicateHandle(self, src[i], self, &dup[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            inherit.push_back(dup[i]);
        } else {
            dup[i] = nullptr;
        }
    }
    auto close_dups = [&] {
        for (HANDLE h : inherit) ::CloseHandle(h);
    };

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOW);
    if (use_std) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = dup[0];
        si.StartupInfo.hStdOutput = dup[1];
        si.StartupInfo.hStdError = dup[2];
    }
    std::vector<unsigned char> attr_buf;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    if (!inherit.empty()) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attr_buf.resize(size);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        // `inherit` 的内存要活到 CreateProcessW 返回：这里存的是指针，不是拷贝。
        if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
            const DWORD e = ::GetLastError();
            close_dups();
            return e != 0 ? e : ERROR_GEN_FAILURE;
        }
        if (!::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         inherit.data(), inherit.size() * sizeof(HANDLE),
                                         nullptr, nullptr)) {
            const DWORD e = ::GetLastError();
            ::DeleteProcThreadAttributeList(attrs);
            close_dups();
            return e != 0 ? e : ERROR_GEN_FAILURE;
        }
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        si.lpAttributeList = attrs;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(app, cmdline, nullptr, nullptr, inherit.empty() ? FALSE : TRUE,
                                     flags, env, cwd, &si.StartupInfo, &pi);
    const DWORD e = ok ? 0 : ::GetLastError();
    if (attrs != nullptr) ::DeleteProcThreadAttributeList(attrs);
    close_dups();  // 子进程已经拿到自己那份了
    if (!ok) return e != 0 ? e : ERROR_GEN_FAILURE;
    *process = pi.hProcess;
    *thread = pi.hThread;
    if (pid != nullptr) *pid = pi.dwProcessId;
    return 0;
}
#endif

Result run(const std::string& exe,
           const std::vector<std::string>& args,
           int timeout_ms,
           const std::string& stdin_data,
           bool split_stderr) {
    Result r;

    // 先确认这个程序存在。找不到就是 launched=false，
    // 调用方靠它区分"没装"和"装了但报错"（media/ffmpeg.cpp 就是这么用的）。
    const auto resolved = which(exe);
    if (!resolved.has_value()) return r;

#ifdef _WIN32
    // ---- Windows：CreateProcessW，不经过 cmd ----
    //
    // **原来这里走的是 _wpopen，也就是把命令交给 cmd.exe。** 换掉的理由
    // 不是洁癖，是踩出来的：cmd 把 `,` `;` `=` 也当参数分隔符，于是
    // ffmpeg 的滤镜串（`scale=640:-2,setsar=1,fps=24`）会被切成碎片。
    // 那次是靠加引号绕过去的，但绕不掉的还有：cmd 在引号里照样展开
    // `%VAR%`，而且 popen 根本没法设超时——文件头那条 TODO 写的就是
    // "装配环节接进来之前必须换成 CreateProcess 加超时"，现在到期了。
    //
    // 不经过 shell 之后，上面那一整类问题都不存在了：引用规则只剩
    // CommandLineToArgvW 那一套（见 quote()），`%` 不再被展开。
    std::wstring app;
    std::wstring cmdline;
    if (!windows_command(*resolved, args, app, cmdline)) {
        r.launched = true;   // 装着，是这一回的参数交不过去
        r.exit_code = 1;
        r.out = SAYF("%1 起不来：参数里有换行，经 cmd.exe 转不过去（%1 是个批处理）", exe);
        return r;
    }

    // 管道两头都**不可继承**：给子进程的那几个由 create_process_std 复制一份
    // 可继承的、点名交过去（见它的注释）。不这么做的话，别的线程上同时起的
    // 子进程会把这里的写端也继承走，读端要等那个不相干的进程退了才有 EOF。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return r;

    // 要喂标准输入就再开一条。不喂就把父进程那个原样给它（老行为）。
    HANDLE in_rd = nullptr;
    HANDLE in_wr = nullptr;
    const bool feed_stdin = !stdin_data.empty();
    if (feed_stdin) {
        if (!::CreatePipe(&in_rd, &in_wr, &sa, 0)) {
            ::CloseHandle(rd);
            ::CloseHandle(wr);
            return r;
        }
    }

    // stderr 单独收的话再开一条（见 proc.hpp 的 split_stderr）。
    HANDLE err_rd = nullptr;
    HANDLE err_wr = nullptr;
    if (split_stderr && !::CreatePipe(&err_rd, &err_wr, &sa, 0)) {
        ::CloseHandle(rd);
        ::CloseHandle(wr);
        if (feed_stdin) {
            ::CloseHandle(in_rd);
            ::CloseHandle(in_wr);
        }
        return r;
    }

    PROCESS_INFORMATION pi{};
    // lpCommandLine 必须可写，CreateProcessW 会就地改它。
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    // **作业对象**：超时要杀的是**整棵树**。只杀直接起的那一个的话，它拉起的
    // 孙进程（批处理经 cmd.exe、venv 的 python 启动器再起真解释器）还拿着管道的
    // 写端，读的那头等不到 EOF。先挂起、进了作业再放它跑，同 spawn。
    // 不设 KILL_ON_JOB_CLOSE：正常跑完它留下的东西不归这儿管。
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);

    // stderr 并进 stdout：ffmpeg -version 和 nvidia-smi 的正经输出都在 stderr。
    const DWORD create_err = create_process_std(
        app.empty() ? nullptr : app.c_str(), mutable_cmd.data(),
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
        feed_stdin ? in_rd : ::GetStdHandle(STD_INPUT_HANDLE), wr, split_stderr ? err_wr : wr,
        &pi.hProcess, &pi.hThread, nullptr);
    const bool ok = create_err == 0;
    if (ok) {
        if (job != nullptr && !::AssignProcessToJobObject(job, pi.hProcess)) {
            ::CloseHandle(job);
            job = nullptr;
        }
        ::ResumeThread(pi.hThread);
    } else if (job != nullptr) {
        ::CloseHandle(job);
        job = nullptr;
    }
    ::CloseHandle(wr);  // 父进程这一份写端要立刻关，否则读端永远等不到 EOF
    if (split_stderr) ::CloseHandle(err_wr);
    if (feed_stdin) ::CloseHandle(in_rd);  // 同理，父进程不留读端
    if (!ok) {
        ::CloseHandle(rd);
        if (split_stderr) ::CloseHandle(err_rd);
        if (feed_stdin) ::CloseHandle(in_wr);
        return r;
    }
    r.launched = true;

    // **写也要在自己的线程上。** 理由和下面读那段是同一条：管道缓冲只有
    // 几 KB，父进程闷头写完再去读的话，子进程先把 stdout 写满就双方互等。
    std::thread writer;
    if (feed_stdin) {
        writer = std::thread([in_wr, &stdin_data] {
            std::size_t off = 0;
            while (off < stdin_data.size()) {
                DWORD put = 0;
                const DWORD want =
                    static_cast<DWORD>(std::min<std::size_t>(stdin_data.size() - off, 1u << 16));
                if (!::WriteFile(in_wr, stdin_data.data() + off, want, &put, nullptr) ||
                    put == 0) {
                    break;  // 子进程提前关了输入（很多命令读够了就不读了）
                }
                off += put;
            }
            // 关掉才有 EOF；不关的话子进程会一直等下一段。
            ::CloseHandle(in_wr);
        });
    }

    // **读管道必须和等超时并行。**
    //
    // 第一版是先把管道读到 EOF 再 WaitForSingleObject——而管道的 EOF 要等
    // 子进程退出才会来，所以超时永远是在"它已经结束了"之后才开始计时，
    // 等于没有。用例里 800 毫秒的超时实际等了 4123 毫秒才回来，
    // 就是这么露出来的。
    //
    // 现在读放在线程里，主线程只等进程。超时就把整个作业杀掉；进程退了之后
    // 读线程最多再等 kDrainGrace（见它上面那段），到点叫停。
    std::atomic<bool> stop{false};
    std::atomic<int> running{split_stderr ? 2 : 1};
    std::string collected;
    std::thread reader([&collected, &stop, &running, rd] {
        drain_pipe(rd, collected, stop, /*mark=*/true);
        running.fetch_sub(1);
    });

    // stderr 那条同样边跑边读：不读的话它写满几 KB 的管道缓冲就卡住不动。
    std::string errs;
    std::thread err_reader;
    if (split_stderr) {
        err_reader = std::thread([&errs, &stop, &running, err_rd] {
            drain_pipe(err_rd, errs, stop, /*mark=*/false);
            running.fetch_sub(1);
        });
    }

    const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    if (::WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_TIMEOUT) {
        if (job == nullptr || !::TerminateJobObject(job, 1)) ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        r.timed_out = true;
    }
    settle_readers(stop, running);
    reader.join();
    if (err_reader.joinable()) err_reader.join();
    if (writer.joinable()) writer.join();
    ::CloseHandle(rd);
    if (split_stderr) ::CloseHandle(err_rd);
    if (job != nullptr) ::CloseHandle(job);
    r.out = std::move(collected);
    r.err = std::move(errs);

    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return r;

#else
    // ---- POSIX：fork + execvp ----
    //
    // execvp 直接吃 argv 数组，**根本不需要引用**——上面 Windows 那一堆
    // 引用规则在这边一条都用不上。
    int fds[2];
    if (::pipe(fds) != 0) return r;
    // 要喂标准输入就再开一条。不喂就让子进程继承父进程那个（老行为）。
    const bool feed_stdin = !stdin_data.empty();
    int in_fds[2] = {-1, -1};
    if (feed_stdin && ::pipe(in_fds) != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }

    // fork 之前算好，见 close_inherited_fds 上面那段。
    const int fd_max = fd_upper_bound();

    // stderr 单独收的话再开一条（见 proc.hpp 的 split_stderr）。
    int err_fds[2] = {-1, -1};
    if (split_stderr && ::pipe(err_fds) != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        if (feed_stdin) {
            ::close(in_fds[0]);
            ::close(in_fds[1]);
        }
        return r;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        if (split_stderr) {
            ::close(err_fds[0]);
            ::close(err_fds[1]);
        }
        if (feed_stdin) {
            ::close(in_fds[0]);
            ::close(in_fds[1]);
        }
        return r;
    }
    if (pid == 0) {
        // **自己一个进程组**：超时杀的是整组（见下面 kill(-pid)）。只杀它一个的
        // 话，`sh wrap.sh` 这种拉起的孙进程还拿着管道写端，读的那头等不到 EOF。
        // 进了别的组就不能再读终端（读了会被 SIGTTIN 停住），不喂输入的时候
        // 标准输入接到 /dev/null——`run` 起的都是一次性命令，本来就不该等人敲键盘。
        ::setpgid(0, 0);
        if (!feed_stdin) {
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDIN_FILENO);
                ::close(devnull);
            }
        }
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        if (split_stderr) {
            ::close(err_fds[0]);
            ::dup2(err_fds[1], STDERR_FILENO);
            ::close(err_fds[1]);
        } else {
            ::dup2(fds[1], STDERR_FILENO);
        }
        ::close(fds[1]);
        if (feed_stdin) {
            ::close(in_fds[1]);
            ::dup2(in_fds[0], STDIN_FILENO);
            ::close(in_fds[0]);
        }
        close_inherited_fds(fd_max);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(resolved->c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(resolved->c_str(), argv.data());
        ::_exit(127);  // execv 只有失败才会回来
    }
    // 父进程这头也设一次：子进程那一句还没跑到时就超时了的话，kill(-pid) 找不到组。
    ::setpgid(pid, pid);
    ::close(fds[1]);
    if (split_stderr) ::close(err_fds[1]);
    r.launched = true;

    // **写也要在自己的线程上。** 管道缓冲只有几 KB，父进程闷头写完再去读的
    // 话，子进程先把 stdout 写满就双方互等，最后只能靠超时收场。
    //
    // ⚠️ **SIGPIPE 要挡掉。** 很多命令读够了就关掉输入（`head` 是极端例子），
    // 那时候继续写会收到 SIGPIPE——默认处置是**杀掉整个进程**，也就是把引擎
    // 自己打死。这里按字节写、忽略 EPIPE 退出即可。
    std::thread writer;
    if (feed_stdin) {
        ::close(in_fds[0]);
        writer = std::thread([fd = in_fds[1], &stdin_data] {
#ifdef SIGPIPE
            ::signal(SIGPIPE, SIG_IGN);
#endif
            std::size_t off = 0;
            while (off < stdin_data.size()) {
                const ssize_t put =
                    ::write(fd, stdin_data.data() + off, stdin_data.size() - off);
                if (put <= 0) {
                    if (put < 0 && errno == EINTR) continue;
                    break;  // EPIPE：对面不读了
                }
                off += static_cast<std::size_t>(put);
            }
            ::close(fd);  // 关掉才有 EOF
        });
    }

    // 同 Windows 那段：读和等要并行，否则超时形同虚设。
    std::atomic<bool> stop{false};
    std::atomic<int> running{split_stderr ? 2 : 1};
    std::string collected;
    std::thread reader([&collected, &stop, &running, fd = fds[0]] {
        drain_fd(fd, collected, stop, /*mark=*/true);
        running.fetch_sub(1);
    });

    // stderr 那条同样边跑边读，理由同 Windows 那段。
    std::string errs;
    std::thread err_reader;
    if (split_stderr) {
        err_reader = std::thread([&errs, &stop, &running, fd = err_fds[0]] {
            drain_fd(fd, errs, stop, /*mark=*/false);
            running.fetch_sub(1);
        });
    }

    int status = 0;
    if (timeout_ms > 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (::waitpid(pid, &status, WNOHANG) == pid) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                // 整组一起杀；组没设上（极少见）就退回只杀它自己。
                if (::kill(-pid, SIGKILL) != 0) ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                r.timed_out = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        ::waitpid(pid, &status, 0);
    }
    settle_readers(stop, running);
    reader.join();
    if (err_reader.joinable()) err_reader.join();
    if (writer.joinable()) writer.join();
    ::close(fds[0]);
    if (split_stderr) ::close(err_fds[0]);
    r.out = std::move(collected);
    r.err = std::move(errs);

    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return r;
#endif
}

// ---------------------------------------------------------------------------
// 起一个不等它结束的子进程。多卡时每张卡一个工作进程，都是这么起的。

ProcHandle spawn(const std::string& exe, const std::vector<std::string>& args,
                 const fs::path& log) {
    const auto resolved = which(exe);
    if (!resolved.has_value()) return 0;

#ifdef _WIN32
    // 批处理经 cmd.exe 起，见 windows_command。
    std::wstring app;
    std::wstring cmd;
    if (!windows_command(*resolved, args, app, cmd)) return 0;
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    // 日志句柄不可继承，点名交给子进程（理由见 create_process_std）。
    // 没有日志就三路都不接、什么都不继承，跟以前一样。
    HANDLE out = INVALID_HANDLE_VALUE;
    if (!log.empty()) {
        out = ::CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    const HANDLE log_h = out != INVALID_HANDLE_VALUE ? out : nullptr;

    // 作业对象，理由见 jobs() 上面那段。
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                       sizeof(info))) {
            ::CloseHandle(job);
            job = nullptr;
        }
    }

    // ⚠️ **CREATE_NO_WINDOW 不能少。** 桌面端是个窗口程序，自己没有控制台；
    // 窗口程序起一个控制台程序（curl、aria2c）而不带这一条，系统就给它**新开
    // 一个终端窗口**——下模型时一路一个，四路就是四个黑框（2026-09-24 用户报的
    // 「下载模型的时候弹出终端」）。人去关那几个框，下载器被关掉、这边当它断了
    // 重试，**又弹一个**。输出本来就接到日志文件里，那个窗口里什么都没有。
    //
    // CREATE_SUSPENDED：先挂起、进了作业再放它跑。不挂起的话，它在进作业之前
    // 那一瞬间拉起的子进程就不在作业里，杀不到。
    PROCESS_INFORMATION pi{};
    const bool ok =
        create_process_std(app.empty() ? nullptr : app.c_str(), mutable_cmd.data(),
                           CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                           nullptr, nullptr, nullptr, log_h, log_h, &pi.hProcess,
                           &pi.hThread, &pi.dwProcessId) == 0;
    if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
    if (!ok) {
        if (job != nullptr) ::CloseHandle(job);
        return 0;
    }
    if (job != nullptr && !::AssignProcessToJobObject(job, pi.hProcess)) {
        ::CloseHandle(job);
        job = nullptr;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(jobs_mu());
        jobs()[pi.dwProcessId] = Spawned{job, pi.hProcess};
    }
    return static_cast<ProcHandle>(pi.dwProcessId);
#else
    // 同上：fork 之前算好。
    const int fd_max = fd_upper_bound();

    const pid_t pid = ::fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        // **自己开一个会话。** 不开的话父进程收到 Ctrl-C 时整个进程组
        // 一起被打断，工作进程来不及把当前这一镜收尾。
        ::setsid();
        if (!log.empty()) {
            const int fd = ::open(log.c_str(),
                                  O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                ::dup2(fd, STDOUT_FILENO);
                ::dup2(fd, STDERR_FILENO);
                ::close(fd);
            }
        }
        // stdin 接到 /dev/null：工作进程不读输入，留着终端句柄会让它
        // 在后台被 SIGTTIN 停住。
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        // **这一句是给下载进程的。** 它们一跑就是几十分钟，而在此之前
        // 每一个都攥着 worker 的监听套接字不放，见上面那段注释。
        close_inherited_fds(fd_max);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(resolved->c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(resolved->c_str(), argv.data());
        ::_exit(127);
    }
    {
        std::lock_guard<std::mutex> lock(spawned_mu());
        spawned().insert(pid);
    }
    return static_cast<ProcHandle>(pid);
#endif
}

bool alive(ProcHandle h) {
    if (h == 0) return false;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(jobs_mu());
    const auto it = jobs().find(static_cast<DWORD>(h));
    if (it == jobs().end()) return false;   // 不是我们起的，或者早收过了
    DWORD code = 0;
    if (::GetExitCodeProcess(it->second.process, &code) && code == STILL_ACTIVE) return true;
    // 退了就把它那一条收掉。**关作业对象会带走它留下的子进程**
    // （KILL_ON_JOB_CLOSE）——头一个都退了，底下还在跑的只会是没人管的孤儿。
    // 下载器每下一个文件起一个进程，不收的话句柄一直攒着。
    close_spawned(it->second);
    jobs().erase(it);
    return false;
#else
    const auto pid = static_cast<pid_t>(h);
    std::lock_guard<std::mutex> lock(spawned_mu());
    if (spawned().count(pid) == 0) return false;   // 不是我们起的，或者早收过了
    // 先收尸，否则僵尸进程 kill(0) 仍然返回 0，永远"活着"。收到了（或者它根本
    // 不是我们的孩子）就从表上拿掉：这个 pid 从这一刻起不归我们了。
    int status = 0;
    const pid_t got = ::waitpid(pid, &status, WNOHANG);
    if (got == 0) return true;
    spawned().erase(pid);
    return false;
#endif
}

void kill_spawned(ProcHandle h, int grace_ms) {
    if (h == 0) return;
#ifdef _WIN32
    // **Windows 上没有"客气地要求退出"。** 原来这儿发 CTRL_BREAK：它只到得了
    // 和我们共用一个控制台的进程，而 spawn 起的都带 CREATE_NO_WINDOW（各有各的
    // 隐形控制台），桌面端自己更是连控制台都没有——那一下从来没送到过，只是白等
    // 宽限期。收到了也一样：默认的处理就是当场 ExitProcess，不比下面这一下客气。
    //
    // 所以直接来：整个作业一起结束（它和它拉起的一切），没有作业就只结束它自己。
    // **只动表上那一条拿着的句柄**，不按 id 去开别人的进程（见 Spawned）。
    const Spawned s = take_spawned(static_cast<DWORD>(h));
    if (s.process == nullptr) return;
    if (s.job == nullptr || !::TerminateJobObject(s.job, 1)) ::TerminateProcess(s.process, 1);
    // 等它真的没了再回去：调用方接着要改名、删 `.part`，文件还被占着就改不动。
    ::WaitForSingleObject(s.process, static_cast<DWORD>(std::max(grace_ms, 0)));
    close_spawned(s);
#else
    const auto pid = static_cast<pid_t>(h);
    {
        std::lock_guard<std::mutex> lock(spawned_mu());
        if (spawned().count(pid) == 0) return;   // 早收过了：这个 pid 可能已经是别人的
    }
    ::kill(pid, SIGTERM);
    // **等它自己收尾。** 工作进程收到信号会把当前这一镜取消掉再退，
    // 直接来硬的会留下半截的 mp4——那种文件比没有更麻烦，
    // 它看着像成品，要播一遍才发现是坏的。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!alive(h)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::lock_guard<std::mutex> lock(spawned_mu());
    if (spawned().count(pid) == 0) return;   // 就在刚才那一下被别处收了
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    spawned().erase(pid);
#endif
}

}  // namespace changji::proc
