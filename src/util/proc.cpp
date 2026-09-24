#include "util/proc.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>

#include "util/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#endif

#include <chrono>
#include <map>
#include <mutex>
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
std::mutex& jobs_mu() {
    static std::mutex m;
    return m;
}
std::map<DWORD, HANDLE>& jobs() {
    static std::map<DWORD, HANDLE> m;
    return m;
}
/// 取走这个进程的作业对象（取走就不在表上了），没有回空。
HANDLE take_job(DWORD pid) {
    std::lock_guard<std::mutex> lock(jobs_mu());
    const auto it = jobs().find(pid);
    if (it == jobs().end()) return nullptr;
    const HANDLE h = it->second;
    jobs().erase(it);
    return h;
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

Result run(const std::string& exe, const std::vector<std::string>& args, int timeout_ms,
           const std::string& stdin_data) {
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
    std::wstring cmdline = paths::from_utf8(quote(*resolved)).wstring();
    for (const auto& a : args) {
        cmdline += L" ";
        cmdline += paths::from_utf8(quote(a)).wstring();
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

    PROCESS_INFORMATION pi{};
    // lpCommandLine 必须可写，CreateProcessW 会就地改它。
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    // stderr 并进 stdout：ffmpeg -version 和 nvidia-smi 的正经输出都在 stderr。
    const DWORD create_err = create_process_std(
        nullptr, mutable_cmd.data(), CREATE_NO_WINDOW, nullptr, nullptr,
        feed_stdin ? in_rd : ::GetStdHandle(STD_INPUT_HANDLE), wr, wr, &pi.hProcess, &pi.hThread,
        nullptr);
    const bool ok = create_err == 0;
    ::CloseHandle(wr);  // 父进程这一份写端要立刻关，否则读端永远等不到 EOF
    if (feed_stdin) ::CloseHandle(in_rd);  // 同理，父进程不留读端
    if (!ok) {
        ::CloseHandle(rd);
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
    // 现在读放在线程里，主线程只等进程。超时就 TerminateProcess，
    // 子进程一死管道就到 EOF，读线程自己收摊。
    std::string collected;
    std::thread reader([&collected, rd] {
        std::array<char, 4096> buf{};
        DWORD got = 0;
        while (::ReadFile(rd, buf.data(), static_cast<DWORD>(buf.size()), &got,
                          nullptr) &&
               got > 0) {
            collected.append(buf.data(), got);
            if (collected.size() > 1u << 20) {
                collected += "\n...(输出过长，已截断)\n";
                break;
            }
        }
    });

    const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    if (::WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        r.timed_out = true;
    }
    reader.join();
    if (writer.joinable()) writer.join();
    ::CloseHandle(rd);
    r.out = std::move(collected);

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

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        if (feed_stdin) {
            ::close(in_fds[0]);
            ::close(in_fds[1]);
        }
        return r;
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
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
    ::close(fds[1]);
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
    std::string collected;
    std::thread reader([&collected, fd = fds[0]] {
        std::array<char, 4096> buf{};
        ssize_t got = 0;
        while ((got = ::read(fd, buf.data(), buf.size())) > 0) {
            collected.append(buf.data(), static_cast<std::size_t>(got));
            if (collected.size() > 1u << 20) {
                collected += "\n...(输出过长，已截断)\n";
                break;
            }
        }
    });

    int status = 0;
    if (timeout_ms > 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (::waitpid(pid, &status, WNOHANG) == pid) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                r.timed_out = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        ::waitpid(pid, &status, 0);
    }
    reader.join();
    if (writer.joinable()) writer.join();
    ::close(fds[0]);
    r.out = std::move(collected);

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
    // 引用规则和上面 run() 那段一样，复用同一个 quote()。
    std::wstring cmd = paths::from_utf8(quote(*resolved)).wstring();
    for (const auto& a : args) {
        cmd += L" ";
        cmd += paths::from_utf8(quote(a)).wstring();
    }
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
        create_process_std(nullptr, mutable_cmd.data(),
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
    ::CloseHandle(pi.hProcess);
    if (job != nullptr) {
        std::lock_guard<std::mutex> lock(jobs_mu());
        jobs()[pi.dwProcessId] = job;
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
    return static_cast<ProcHandle>(pid);
#endif
}

bool alive(ProcHandle h) {
    if (h == 0) return false;
#ifdef _WIN32
    HANDLE p = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(h));
    DWORD code = 0;
    const bool ok = p != nullptr && ::GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    if (p != nullptr) ::CloseHandle(p);
    // 退了就把它那个作业对象收掉。**关句柄会带走它留下的子进程**
    // （KILL_ON_JOB_CLOSE）——头一个都退了，底下还在跑的只会是没人管的孤儿。
    // 下载器每下一个文件起一个进程，不收的话句柄一直攒着。
    if (!ok) {
        if (const HANDLE job = take_job(static_cast<DWORD>(h)); job != nullptr) {
            ::CloseHandle(job);
        }
    }
    return ok;
#else
    // 先收尸，否则僵尸进程 kill(0) 仍然返回 0，永远"活着"。
    int status = 0;
    ::waitpid(static_cast<pid_t>(h), &status, WNOHANG);
    return ::kill(static_cast<pid_t>(h), 0) == 0;
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
    const HANDLE job = take_job(static_cast<DWORD>(h));
    if (job != nullptr) {
        ::TerminateJobObject(job, 1);
    } else {
        HANDLE p = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(h));
        if (p != nullptr) {
            ::TerminateProcess(p, 1);
            ::CloseHandle(p);
        }
    }
    // 等它真的没了再回去：调用方接着要改名、删 `.part`，文件还被占着就改不动。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
    while (std::chrono::steady_clock::now() < deadline && alive(h)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (job != nullptr) ::CloseHandle(job);
#else
    ::kill(static_cast<pid_t>(h), SIGTERM);
    // **等它自己收尾。** 工作进程收到信号会把当前这一镜取消掉再退，
    // 直接来硬的会留下半截的 mp4——那种文件比没有更麻烦，
    // 它看着像成品，要播一遍才发现是坏的。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!alive(h)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(static_cast<pid_t>(h), SIGKILL);
    int status = 0;
    ::waitpid(static_cast<pid_t>(h), &status, 0);
#endif
}

}  // namespace changji::proc
