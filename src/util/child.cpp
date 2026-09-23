#include "util/child.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "util/paths.hpp"
#include "util/proc.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// macOS 的 unistd.h 不声明它；Linux 上重复声明无害。
extern char** environ;
#endif

namespace changji::proc {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

/// 队列里每一行除了内容再按这么多字节记账：一个失控的扩展狂打空行的话，
/// 光按内容算是 0 字节，deque 却一直在长。
constexpr std::size_t kLineOverhead = 32;

enum class WriteResult { ok, broken, timed_out, cancelled };

/// 读到的字节切成行。
///
/// 半行留在 `pending` 里接着攒：一行 1 MB 的 JSON 要读十几次才齐，
/// 每次只在新来的那一段里找换行，不回头重扫已经攒下的。
/// 一行攒过了 `max_line` 就整行扔掉：`pending` 立刻放掉，后面的字节一直扔到
/// 下一个换行为止——一个往 stdout 灌不带换行的东西的扩展，内存不会跟着涨。
struct LineSplitter {
    explicit LineSplitter(std::size_t cap) : max_line(cap) {}

    std::size_t max_line;
    std::string pending;
    bool dropping = false;
    std::size_t dropped = 0;  // 扔掉了几行，交给 note_dropped 之后清零

    void feed(const char* p, std::size_t n, std::vector<std::string>& ready) {
        while (n > 0) {
            const void* nl = std::memchr(p, '\n', n);
            const std::size_t len =
                nl != nullptr ? static_cast<std::size_t>(static_cast<const char*>(nl) - p) : n;
            if (!dropping) {
                if (pending.size() + len > max_line) {
                    dropping = true;
                    std::string().swap(pending);
                } else {
                    pending.append(p, len);
                }
            }
            if (nl == nullptr) return;
            if (dropping) {
                dropping = false;
                ++dropped;
            } else {
                // 行尾的 `\r` 去掉（Windows 上的程序爱写 `\r\n`）。
                if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                ready.push_back(std::move(pending));
                pending.clear();
            }
            p += len + 1;
            n -= len + 1;
        }
    }

    /// 读到头了：没以换行结尾的最后半行也算一行。
    void finish(std::vector<std::string>& ready) {
        if (dropping) {
            dropping = false;
            ++dropped;
        } else if (!pending.empty()) {
            if (pending.back() == '\r') pending.pop_back();
            ready.push_back(std::move(pending));
            pending.clear();
        }
    }
};

}  // namespace

struct Child::Impl {
#ifdef _WIN32
    HANDLE process = nullptr;
    /// 装着它和它拉起的一切。`KILL_ON_JOB_CLOSE`：这个句柄一关，里面的全死。
    /// 放不进去时是空（见 launch），那时只能杀它自己。
    HANDLE job = nullptr;
    /// stdin 这边的写端：命名管道的客户端，重叠 I/O（见 make_stdin_pipe）。
    HANDLE in_wr = nullptr;
    HANDLE write_event = nullptr;   // 一笔重叠写的完成事件
    HANDLE cancel_event = nullptr;  // kill 置上：卡在写上的那一笔立刻回来
    HANDLE out_rd = nullptr;
    /// 读线程自己的句柄，给 `CancelSynchronousIo` 用，见 stop_reader。
    std::atomic<HANDLE> reader_self{nullptr};
#else
    pid_t pid = -1;
    int in_fd = -1;  // O_NONBLOCK：写要能被 kill 和超时叫回来
    int out_fd = -1;
    /// 叫醒读线程用的自管道：kill 往里写一个字节，poll 就回来了。
    int wake_rd = -1;
    int wake_wr = -1;
    mutable std::mutex reap_mu;
    /// 知道它退了。**只看不收**（waitid 带 WNOWAIT）：僵尸留着，它的 pid——也就是
    /// 它的进程组号——就一直占着，kill 时整组杀才不会杀到别人头上。
    mutable bool exited = false;
    mutable bool reaped = false;            // kill 里收过尸了
    mutable bool reaped_elsewhere = false;  // 被别处收走了：那个号可能已经是别人的
    mutable int code = -1;
#endif

    std::size_t max_line = 0;
    std::size_t max_queued = 0;
    fs::path stderr_log;

    /// 写 stdin 的锁。写本身能被叫回来（kill 置 `killing`），所以 kill 拿得到它。
    std::timed_mutex write_mu;
    bool in_closed = false;  // write_mu 管着
    std::atomic<bool> killing{false};

    // 读线程攒下的行。
    std::mutex mu;
    std::condition_variable cv;        // 来了新行 / 读线程收摊
    std::condition_variable space_cv;  // 有行被取走了 / 该收摊了（反压用）
    std::deque<std::string> lines;
    std::size_t queued_bytes = 0;  // mu 管着
    bool eof = false;              // mu 管着：读线程已经收摊
    std::atomic<bool> stopping{false};
    std::thread reader;

    std::mutex kill_mu;
    bool killed = false;  // kill_mu 管着

    ~Impl();

    void reader_loop();
    bool wait_for_room();
    void publish(std::vector<std::string>& ready);
    void note_dropped(LineSplitter& split);
    WriteResult write_all(const char* p, std::size_t n, Clock::time_point deadline, bool bounded);
    void close_stdin_locked();
    std::optional<int> poll_exit() const;
    bool wait_exit(int ms) const;
    void kill_tree();
    void reap();
    void stop_reader();
};

namespace {

/// 「找不到」那句话。给的是路径就说没有这个文件，给的是名字就说 PATH 里没有
/// ——两种情况人要去查的地方不一样。
std::string not_found_message(const std::string& exe) {
    if (exe.find('/') != std::string::npos || exe.find('\\') != std::string::npos) {
        return "找不到 " + exe + "（没有这个文件）";
    }
    return "找不到 " + exe + "（PATH 里没有这个程序）";
}

/// 环境变量名是不是 `want`。Windows 上不分大小写（`Path` 就是 `PATH`）。
bool env_name_is(const std::string& name, const char* want) {
#ifdef _WIN32
    const std::size_t n = std::strlen(want);
    if (name.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != want[i]) return false;
    }
    return true;
#else
    return name == want;
#endif
}

/// 找程序。`env` 里改了 PATH / PATHEXT 的先按改过的找：子进程自己找东西也是按
/// 那个找的，配置里写「PATH 加上某个 node 的目录」就是要用那个 node。
/// 找不到再按这个进程自己的 PATH 找。
std::optional<std::string> resolve_exe(const Child::Options& o) {
    const std::string* path_ov = nullptr;
    const std::string* ext_ov = nullptr;
    for (const auto& [k, v] : o.env) {
        if (env_name_is(k, "PATH")) path_ov = &v;
#ifdef _WIN32
        if (env_name_is(k, "PATHEXT")) ext_ov = &v;
#endif
    }
    if (path_ov != nullptr || ext_ov != nullptr) {
        auto found = which_in(o.exe, path_ov != nullptr ? *path_ov : paths::env("PATH"),
                              ext_ov != nullptr ? *ext_ov : paths::env("PATHEXT"));
        if (found.has_value()) return found;
    }
    return which(o.exe);
}

#ifdef _WIN32

/// 起完就关的句柄。launch 里一路上七八个句柄，哪一步失败都要全关干净。
struct Owned {
    HANDLE h = nullptr;
    Owned() = default;
    explicit Owned(HANDLE x) : h(x == INVALID_HANDLE_VALUE ? nullptr : x) {}
    ~Owned() {
        if (h != nullptr) ::CloseHandle(h);
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    HANDLE release() {
        HANDLE x = h;
        h = nullptr;
        return x;
    }
};

/// 系统错误码 → 一句人话（跟着系统语言，中文系统上是中文）。
std::string system_message(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring w = (n > 0 && buf != nullptr) ? std::wstring(buf, n) : std::wstring();
    if (buf != nullptr) ::LocalFree(buf);
    // 系统那句话带着句号和换行，我们要把它嵌进自己的句子里。
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' ||
                          w.back() == L'.' || w.back() == static_cast<wchar_t>(0x3002))) {
        w.pop_back();
    }
    if (w.empty()) return "错误码 " + std::to_string(code);
    return paths::to_utf8(fs::path(w));
}

bool is_batch(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
    return ext == L".cmd" || ext == L".bat";
}

/// `\\server\share\…`（连 `\\?\…` 一起算）。
bool is_unc(const fs::path& p) {
    const std::wstring& w = p.native();
    auto slash = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    return w.size() >= 2 && slash(w[0]) && slash(w[1]);
}

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
std::string cmd_quote(const std::string& s, bool force) {
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

std::wstring cmd_exe_path() {
    // 不读 COMSPEC：那是用户环境里的一个变量，改过的话起的就不是 cmd 了。
    wchar_t buf[MAX_PATH];
    const UINT n = ::GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\Windows\\System32\\cmd.exe";
    return std::wstring(buf, n) + L"\\cmd.exe";
}

/// 环境块：这个进程的环境叠上 `overrides`，按名字**不分大小写**排好序。
///
/// 排序不是讲究：CreateProcessW 的文档要求环境块按名字排好（不分大小写、
/// 不看区域设置），不排的话有的程序查环境变量会查漏。比大小用
/// `CompareStringOrdinal(..., TRUE)`，就是文档说的那种比法。
struct EnvLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                      static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

std::vector<wchar_t> build_env_block(const std::map<std::string, std::string>& overrides) {
    std::map<std::wstring, std::wstring, EnvLess> vars;  // 名字 → 整条 "K=V"
    if (wchar_t* block = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p != L'\0'; p += std::wcslen(p) + 1) {
            const std::wstring entry(p);
            // `=C:=C:\foo` 这种以 `=` 开头的是 cmd 记各盘当前目录的隐藏变量，
            // 名字要从第二个字符起找 `=`。照样传下去，cmd 起批处理用得着。
            const auto eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            vars.emplace(entry.substr(0, eq), entry);
        }
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& [k, v] : overrides) {
        if (k.empty() || k.find('=') != std::string::npos) continue;  // 不成名字
        const std::wstring wk = paths::from_utf8(k).wstring();
        // 同名（不分大小写）就地换掉。`Path` 和 `PATH` 各留一份的话，
        // 子进程读到哪一份看运气。
        vars[wk] = wk + L"=" + paths::from_utf8(v).wstring();
    }
    std::vector<wchar_t> out;
    for (const auto& [k, entry] : vars) {
        out.insert(out.end(), entry.begin(), entry.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    if (out.size() == 1) out.push_back(L'\0');  // 一个变量都没有时也要两个 NUL
    return out;
}

/// stderr 接到哪儿：日志文件（追加）或者空设备。句柄不可继承，
/// 由 `create_process_std` 复制一份点名交给子进程。
HANDLE open_stderr_target(const fs::path& log) {
    if (!log.empty()) {
        std::error_code ec;
        if (log.has_parent_path()) fs::create_directories(log.parent_path(), ec);
        // 只要 FILE_APPEND_DATA：每一笔写都落在文件尾，跟别的进程同写一个
        // 日志也不会互相盖。共享读写删：人可以一边开着看、一边清掉它。
        HANDLE h = ::CreateFileW(log.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        // 开不了日志不算起不来：扩展照跑，只是这回没日志。
    }
    return ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
}

/// 子进程 stdin 的那条管道。
///
/// **不用 CreatePipe**：匿名管道只能同步写，扩展不读 stdin 的时候 WriteFile 一直
/// 卡着——没有超时，kill 也叫不回来（CancelSynchronousIo 得知道是哪条线程在写，
/// 而写的是调用方的线程）。所以开一条命名管道：
///   · 子进程那头（服务端，读）是普通的**同步**句柄，跟 CreatePipe 给的读端是
///     同一种东西（匿名管道本来就是命名管道的服务端）——npx、node、python 看不出区别；
///   · 这边（客户端，写）开成**重叠 I/O**，写的时候同时等「写完了」「该停了」「到点了」。
/// 名字带进程号、序号和随机数；FIRST_PIPE_INSTANCE + 只许一个实例 + 拒绝远程：
/// 别人抢不到这个名字，这边连上之后也再没人连得进来。Python 的 asyncio 和 libuv
/// 在 Windows 上给子进程开管道都是这个办法。
bool make_stdin_pipe(HANDLE* child_end, HANDLE* our_end, DWORD* error) {
    static std::atomic<unsigned long> seq{0};
    std::random_device rnd;
    wchar_t name[128];
    std::swprintf(name, 128, L"\\\\.\\pipe\\changji-child-%lu-%lu-%08x%08x",
                  static_cast<unsigned long>(::GetCurrentProcessId()), ++seq, rnd(), rnd());
    // 64 KB：跟 stdout 那条一样。默认的 4 KB 下，一行稍长的 JSON 就要等对面读一轮。
    HANDLE srv = ::CreateNamedPipeW(
        name, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
        1 << 16, 0, nullptr);
    if (srv == INVALID_HANDLE_VALUE) {
        *error = ::GetLastError();
        return false;
    }
    HANDLE cli = ::CreateFileW(name, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                               nullptr);
    if (cli == INVALID_HANDLE_VALUE) {
        *error = ::GetLastError();
        ::CloseHandle(srv);
        return false;
    }
    *child_end = srv;
    *our_end = cli;
    return true;
}

bool launch(Child::Impl& im, const Child::Options& opts, const std::string& resolved,
            std::string* err) {
    const std::string& name = opts.exe;
    auto fail = [&](DWORD code) {
        *err = name + " 起不来：" + system_message(code);
        return false;
    };
    fs::path exe_path = paths::from_utf8(resolved);
    exe_path.make_preferred();

    // ---- 命令行 ----
    std::wstring app;
    std::wstring cmdline;
    if (is_batch(exe_path)) {
        // `npx` 在 Windows 上是 `npx.cmd`。CreateProcessW 不认批处理，要 cmd.exe
        // 来跑。/s /c "…"：cmd 去掉最外面那一对引号，里面原样当一行命令；
        // /d 不跑 AutoRun（注册表里谁挂了什么都别掺进来）；/e:ON 给 `%cd:~,%`
        // 那个垫子用；/v:OFF 让 `!` 不当延迟展开。
        for (const auto& a : opts.args) {
            if (a.find_first_of(std::string("\r\n\0", 3)) != std::string::npos) {
                *err = name + " 起不来：参数里有换行，经 cmd.exe 转不过去（" + name +
                       " 是个批处理）";
                return false;
            }
        }
        std::string line = "cmd.exe /e:ON /v:OFF /d /s /c \"";
        line += cmd_quote(paths::to_utf8(exe_path), true);
        for (const auto& a : opts.args) {
            line += ' ';
            line += cmd_quote(a, false);
        }
        line += '"';
        app = cmd_exe_path();
        cmdline = paths::from_utf8(line).wstring();
    } else {
        // 程序路径同时给 lpApplicationName：不给的话 CreateProcessW 自己再按
        // 命令行第一段去找一遍，`C:\Program Files\…` 这种路径会先试 `C:\Program`。
        app = exe_path.wstring();
        cmdline = paths::from_utf8(quote_arg(paths::to_utf8(exe_path))).wstring();
        for (const auto& a : opts.args) {
            cmdline += L" ";
            cmdline += paths::from_utf8(quote_arg(a)).wstring();
        }
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');  // lpCommandLine 必须可写

    std::vector<wchar_t> env = build_env_block(opts.env);
    const std::wstring cwd = opts.cwd.empty() ? std::wstring() : opts.cwd.wstring();

    // ---- 三条标准句柄 ----
    //
    // 这边的句柄**全都不可继承**：给子进程的那三个由 create_process_std 复制一份
    // 可继承的、点名交过去（PROC_THREAD_ATTRIBUTE_HANDLE_LIST）。这个进程是个
    // 多线程的服务，别的线程随时在起子进程；这边的句柄哪一刻可继承，就可能漏进
    // 一个不相干的子进程里——漏的是写端，这条管道就等不到 EOF。
    HANDLE raw_in_rd = nullptr;
    HANDLE raw_in_wr = nullptr;
    DWORD pipe_err = 0;
    if (!make_stdin_pipe(&raw_in_rd, &raw_in_wr, &pipe_err)) return fail(pipe_err);
    Owned in_rd(raw_in_rd);
    Owned in_wr(raw_in_wr);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    HANDLE raw_out_rd = nullptr;
    HANDLE raw_out_wr = nullptr;
    // 64 KB：一行上 MB 的 JSON 不至于一次只挪 4 KB。
    if (!::CreatePipe(&raw_out_rd, &raw_out_wr, &sa, 1 << 16)) return fail(::GetLastError());
    Owned out_rd(raw_out_rd);
    Owned out_wr(raw_out_wr);

    Owned err_h(open_stderr_target(opts.stderr_log));
    if (err_h.h == nullptr) return fail(::GetLastError());

    Owned write_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Owned cancel_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (write_event.h == nullptr || cancel_event.h == nullptr) return fail(::GetLastError());

    // ---- 起 ----
    //
    // CREATE_SUSPENDED：先放进 Job 再让它跑。反过来的话 npx 可能在放进去之前
    // 就已经拉起了 node，那个 node 就不在 Job 里，关的时候漏掉。
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    const DWORD create_err = create_process_std(
        app.c_str(), mutable_cmd.data(), flags, env.data(), cwd.empty() ? nullptr : cwd.c_str(),
        in_rd.h, out_wr.h, err_h.h, &process, &thread, nullptr);
    if (create_err != 0) return fail(create_err);
    // 子进程那一头父进程一份都不留：留着 out_wr 的话，它退了读端也等不到 EOF。
    // （in_rd / out_wr / err_h 出了这个函数自己关。）

    // ---- 放进 Job ----
    //
    // 放不进去（外面那一层 Job 不许嵌套，老系统上会遇到）就不放：扩展照样起，
    // 只是关的时候只杀得了它自己。为这个拒绝起来，人什么都用不了。
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li,
                                       sizeof(li)) ||
            !::AssignProcessToJobObject(job, process)) {
            ::CloseHandle(job);
            job = nullptr;
        }
    }
    ::ResumeThread(thread);
    ::CloseHandle(thread);

    im.process = process;
    im.job = job;
    im.in_wr = in_wr.release();
    im.out_rd = out_rd.release();
    im.write_event = write_event.release();
    im.cancel_event = cancel_event.release();
    return true;
}

#else  // POSIX

/// fork 之后关 fd 的上界，fork 之前算好。理由同 proc.cpp 的 `fd_upper_bound`
/// （`sysconf` 不在 async-signal-safe 那张表上；上限钉住免得空转一百万次）。
int fd_limit() {
    const long n = ::sysconf(_SC_OPEN_MAX);
    if (n <= 0) return 256;
    return static_cast<int>(n > 4096 ? 4096 : n);
}

/// 开一条两头都带 CLOEXEC 的管道。
///
/// 别的线程随时可能 fork——那边 exec 之前我们的管道要是没带 CLOEXEC，
/// 就会漏进一个不相干的子进程里，这条管道要等它退了才有 EOF。
/// Linux 有 pipe2 一步到位；macOS 没有，只能 pipe 完马上补（中间那一瞬
/// 挡不住，好在 proc.cpp 那边的子进程 exec 前会把多余的 fd 全关掉）。
bool make_pipe(int fds[2]) {
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

/// 把 fd 挪到 3 以上。
///
/// 这个进程要是没有 stdin（被当成守护进程起的），pipe 会发回 0、1、2 这几个号，
/// 子进程里 dup2 到 0/1/2 时就会互相踩：dup2(同一个号) 什么都不做，
/// CLOEXEC 还留着，exec 一过那一路就没了。
void lift_fd(int& fd) {
    if (fd > STDERR_FILENO) return;
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) return;
    ::close(fd);
    fd = moved;
}

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

bool launch(Child::Impl& im, const Child::Options& opts, const std::string& resolved,
            std::string* err) {
    const std::string& name = opts.exe;

    // ⚠️ **SIGPIPE 整个进程忽略掉。** 扩展随时可能死，死了以后往它的 stdin 写
    // 会收到 SIGPIPE——默认处置是杀掉整个进程，也就是把引擎自己打死。
    // 进程级的忽略而不是逐线程挡：proc.cpp 的写线程本来就是这么做的，
    // 一个服务进程也没有谁指望靠 SIGPIPE 退出。子进程里会改回默认（见下），
    // 不然这个"忽略"会跟着 exec 传给扩展。
    static std::once_flag sigpipe_once;
    std::call_once(sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });

    // ---- fork 之前把要用的东西全备好 ----
    //
    // 这是个多线程进程：fork 之后、exec 之前只能调 async-signal-safe 的函数，
    // 分配内存都不行（别的线程可能正攥着 malloc 的锁，子进程里那把锁永远不放）。
    std::vector<std::string> argv_s;
    argv_s.push_back(resolved);
    argv_s.insert(argv_s.end(), opts.args.begin(), opts.args.end());
    std::vector<char*> argv;
    for (auto& s : argv_s) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::map<std::string, std::string> vars;  // 名字 → 整条 "K=V"
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string entry(*e);
        const auto eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        vars.emplace(entry.substr(0, eq), entry);
    }
    for (const auto& [k, v] : opts.env) {
        if (k.empty() || k.find('=') != std::string::npos) continue;
        vars[k] = k + "=" + v;
    }
    std::vector<std::string> env_s;
    env_s.reserve(vars.size());
    for (auto& kv : vars) env_s.push_back(std::move(kv.second));
    std::vector<char*> envp;
    for (auto& s : env_s) envp.push_back(s.data());
    envp.push_back(nullptr);

    const std::string cwd = opts.cwd.empty() ? std::string() : opts.cwd.native();
    const std::string log = opts.stderr_log.empty() ? std::string() : opts.stderr_log.native();
    if (!log.empty() && opts.stderr_log.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(opts.stderr_log.parent_path(), ec);
    }
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    const int fd_max = fd_limit();

    auto fail_errno = [&](int e) {
        *err = name + " 起不来：" + std::generic_category().message(e);
        return false;
    };

    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    int wake[2] = {-1, -1};
    int errp[2] = {-1, -1};  // exec 失败时子进程把 errno 写回来，见下
    auto close_all = [&] {
        for (int* fds : {in, out, wake, errp}) {
            close_fd(fds[0]);
            close_fd(fds[1]);
        }
    };
    if (!make_pipe(in) || !make_pipe(out) || !make_pipe(wake) || !make_pipe(errp)) {
        const int e = errno;
        close_all();
        return fail_errno(e);
    }
    lift_fd(in[0]);
    lift_fd(out[1]);
    lift_fd(errp[1]);

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        close_all();
        return fail_errno(e);
    }
    if (pid == 0) {
        // ---- 子进程：下面只许调 async-signal-safe 的 ----
        //
        // 自成一个进程组：关的时候 kill(-pgid) 连 npx 拉起的 node 一起带走。
        ::setpgid(0, 0);
        ::signal(SIGPIPE, SIG_DFL);
        ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
        ::dup2(in[0], STDIN_FILENO);
        ::dup2(out[1], STDOUT_FILENO);
        // stderr 打开时不带 CLOEXEC：碰巧拿到 2 号时 dup2 什么都不做，带着
        // CLOEXEC 的话 exec 一过它就没了。
        int e = -1;
        if (!log.empty()) e = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (e < 0) e = ::open("/dev/null", O_WRONLY);
        if (e >= 0 && e != STDERR_FILENO) ::dup2(e, STDERR_FILENO);
        // 继承来的其余 fd 全关掉（理由见 proc.cpp 的 close_inherited_fds）。
        // errp[1] 留着——它带 CLOEXEC，exec 成功时自己会关。
        for (int fd = STDERR_FILENO + 1; fd < fd_max; ++fd) {
            if (fd != errp[1]) ::close(fd);
        }
        int report[2] = {0, 0};
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            report[0] = 1;
            report[1] = errno;
        } else {
            ::execve(argv[0], argv.data(), envp.data());
            report[0] = 2;  // execve 只有失败才会回来
            report[1] = errno;
        }
        ssize_t w;
        do {
            w = ::write(errp[1], report, sizeof(report));
        } while (w < 0 && errno == EINTR);
        ::_exit(127);
    }

    // ---- 父进程 ----
    // 两头都设一遍进程组：子进程那句还没跑到时，这边 kill(-pid) 会落空。
    ::setpgid(pid, pid);
    close_fd(in[0]);
    close_fd(out[1]);
    close_fd(errp[1]);

    // **exec 成没成要在这儿问清楚。** fork 总是成功的，程序没有执行权限、
    // 是个坏掉的脚本（#! 指向不存在的解释器）都要到 exec 那一步才露出来。
    // 不问的话起来就是一个"活着、但一声不吭"的扩展，人只看得到超时。
    // 管道带 CLOEXEC：exec 成功它就关了，这边读到 0 字节。
    int report[2] = {0, 0};
    std::size_t got = 0;
    while (got < sizeof(report)) {
        const ssize_t n = ::read(errp[0], reinterpret_cast<char*>(report) + got,
                                 sizeof(report) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    close_fd(errp[0]);
    if (got == sizeof(report)) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        close_all();
        if (report[0] == 1) {
            *err = name + " 起不来：进不了工作目录 " + cwd + "（" +
                   std::generic_category().message(report[1]) + "）";
            return false;
        }
        return fail_errno(report[1]);
    }

    // 写端设成非阻塞：对面不读时写不能一直卡着，要能被超时和 kill 叫回来
    // （见 write_all）。只动这一头——子进程那头是另一个打开的文件，不受影响。
    const int fl = ::fcntl(in[1], F_GETFL);
    if (fl >= 0) ::fcntl(in[1], F_SETFL, fl | O_NONBLOCK);

    im.pid = pid;
    im.in_fd = in[1];
    im.out_fd = out[0];
    im.wake_rd = wake[0];
    im.wake_wr = wake[1];
    return true;
}

#endif

}  // namespace

// ---------------------------------------------------------------------------
// Impl

Child::Impl::~Impl() {
#ifdef _WIN32
    for (HANDLE h : {in_wr, out_rd, job, process, write_event, cancel_event, reader_self.load()}) {
        if (h != nullptr) ::CloseHandle(h);
    }
#else
    for (int* fd : {&in_fd, &out_fd, &wake_rd, &wake_wr}) close_fd(*fd);
#endif
}

/// 攒着的行超过上限就停下不读，等 `read_line` 取走一些。回 false = 该收摊了。
///
/// 停下不读，扩展往 stdout 写就会卡住——这正是要的：一个失控的扩展只能卡住
/// 它自己，吃不光这台机器的内存。
bool Child::Impl::wait_for_room() {
    std::unique_lock<std::mutex> lk(mu);
    space_cv.wait(lk, [&] { return queued_bytes < max_queued || stopping.load(); });
    return !stopping.load();
}

/// 切好的行交出去。
void Child::Impl::publish(std::vector<std::string>& ready) {
    if (ready.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& s : ready) {
            queued_bytes += s.size() + kLineOverhead;
            lines.push_back(std::move(s));
        }
    }
    ready.clear();
    cv.notify_all();
}

/// 扔掉了超长的行：给了日志就往里记一句。不记的话，人看到的只是"那次调用
/// 没回话"，查不到是回话太长被扔了。
void Child::Impl::note_dropped(LineSplitter& split) {
    const std::size_t n = split.dropped;
    split.dropped = 0;
    if (n == 0 || stderr_log.empty()) return;
    std::ofstream f(stderr_log, std::ios::app | std::ios::binary);
    if (!f) return;
    const std::string cap = max_line % (std::size_t{1} << 20) == 0
                                ? std::to_string(max_line >> 20) + " MB"
                                : std::to_string(max_line) + " 字节";
    f << "\n[场记] 这个扩展往 stdout 写了 " << n << " 行超过 " << cap
      << " 的东西，整行扔掉了（没交给对话）。\n";
}

void Child::Impl::reader_loop() {
    std::vector<char> buf(1 << 16);
    LineSplitter split(max_line);
    std::vector<std::string> ready;
#ifdef _WIN32
    // 先给自己拿一个真句柄（GetCurrentThread 只是个伪句柄，别的线程用不了），
    // kill 靠它 CancelSynchronousIo 把卡在 ReadFile 上的这条线程叫回来。
    HANDLE self = nullptr;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                      &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    reader_self.store(self);
    while (wait_for_room()) {
        DWORD got = 0;
        // 失败只有两种：对面全关了（ERROR_BROKEN_PIPE，就是 EOF），
        // 或者 kill 把这次读取消了。都是收摊。
        if (!::ReadFile(out_rd, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            break;
        }
        if (got == 0) continue;  // 对面写了 0 字节，不是 EOF
        split.feed(buf.data(), got, ready);
        publish(ready);
        if (split.dropped > 0) note_dropped(split);
    }
#else
    while (wait_for_room()) {
        pollfd fds[2] = {{out_fd, POLLIN, 0}, {wake_rd, POLLIN, 0}};
        const int r = ::poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents != 0) break;  // kill 叫醒的
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t n = ::read(out_fd, buf.data(), buf.size());
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            if (n == 0) break;  // EOF
            split.feed(buf.data(), static_cast<std::size_t>(n), ready);
            publish(ready);
            if (split.dropped > 0) note_dropped(split);
        } else if ((fds[0].revents & POLLNVAL) != 0) {
            break;
        }
    }
#endif
    split.finish(ready);
    if (split.dropped > 0) note_dropped(split);
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& s : ready) {
            queued_bytes += s.size() + kLineOverhead;
            lines.push_back(std::move(s));
        }
        eof = true;
    }
    cv.notify_all();
}

/// 写一段，写完、断了、到点了、被 kill 叫回来了，四种结局。
///
/// **两边都不能一直卡在写上**：扩展不读 stdin 时管道几十 KB 就满，老办法（同步写）
/// 在那儿一直等，kill 叫不回来、调用方也设不了时限。
///   · Windows：写端是重叠 I/O（见 make_stdin_pipe），同时等「写完」「kill」「到点」；
///     没写完就回来的那一笔先 CancelIoEx 再等它真落地——OVERLAPPED 在栈上，
///     I/O 还挂着就返回的话，系统会往一块已经不在的内存里写结果。
///   · POSIX：写端非阻塞，写不进去就 poll 等可写，每 50 毫秒看一眼 `killing`。
WriteResult Child::Impl::write_all(const char* p, std::size_t n, Clock::time_point deadline,
                                   bool bounded) {
    auto left_ms = [&]() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
            .count();
    };
    while (n > 0) {
        if (killing.load()) return WriteResult::cancelled;
#ifdef _WIN32
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n, std::size_t{1} << 24));
        OVERLAPPED ov{};
        ov.hEvent = write_event;
        ::ResetEvent(write_event);
        DWORD put = 0;
        if (!::WriteFile(in_wr, p, want, nullptr, &ov)) {
            if (::GetLastError() != ERROR_IO_PENDING) return WriteResult::broken;
            DWORD wait = INFINITE;
            if (bounded) {
                const long long left = left_ms();
                wait = left > 0 ? static_cast<DWORD>(std::min<long long>(left, 0x7fffffff)) : 0;
            }
            const HANDLE hs[2] = {write_event, cancel_event};
            const DWORD r = ::WaitForMultipleObjects(2, hs, FALSE, wait);
            if (r != WAIT_OBJECT_0) {
                ::CancelIoEx(in_wr, &ov);
                ::GetOverlappedResult(in_wr, &ov, &put, TRUE);  // 等取消真落地
                if (r == WAIT_OBJECT_0 + 1) return WriteResult::cancelled;
                if (r == WAIT_TIMEOUT) return WriteResult::timed_out;
                return WriteResult::broken;
            }
        }
        if (!::GetOverlappedResult(in_wr, &ov, &put, FALSE) || put == 0) {
            return WriteResult::broken;
        }
        p += put;
        n -= put;
#else
        const ssize_t put = ::write(in_fd, p, n);
        if (put > 0) {
            p += put;
            n -= static_cast<std::size_t>(put);
            continue;
        }
        if (put < 0 && errno == EINTR) continue;
        if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int wait = 50;
            if (bounded) {
                const long long left = left_ms();
                if (left <= 0) return WriteResult::timed_out;
                wait = static_cast<int>(std::min<long long>(left, 50));
            }
            pollfd pfd{in_fd, POLLOUT, 0};
            ::poll(&pfd, 1, wait);
            continue;  // 可写了、对面关了（下一次 write 回 EPIPE）、或者该看一眼 killing
        }
        return WriteResult::broken;  // EPIPE：对面没了（SIGPIPE 已经忽略掉）
#endif
    }
    return WriteResult::ok;
}

void Child::Impl::close_stdin_locked() {
    if (in_closed) return;
    in_closed = true;
#ifdef _WIN32
    if (in_wr != nullptr) ::CloseHandle(in_wr);
    in_wr = nullptr;
#else
    close_fd(in_fd);
#endif
}

std::optional<int> Child::Impl::poll_exit() const {
#ifdef _WIN32
    // 不拿 STILL_ACTIVE（259）比退出码：一个程序真以 259 退出时会被当成还活着。
    if (::WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return std::nullopt;
    DWORD c = 1;
    ::GetExitCodeProcess(process, &c);
    return static_cast<int>(c);
#else
    std::lock_guard<std::mutex> lk(reap_mu);
    if (exited) return code;
    // **只看不收**（WNOWAIT）。在这儿就 waitpid 收尸的话，pid 当场还给系统；
    // 调用方可能过好久才 kill（对话那头会把断了的连接留着几十秒、几个小时），
    // 到那时 kill(-pid) 杀的可能是另一个恰好拿到这个号的进程组。
    siginfo_t si;
    std::memset(&si, 0, sizeof(si));  // 有的系统 WNOHANG 没东西时不碰它
    int r;
    do {
        r = ::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        // ECHILD：被别处收走了（谁 waitpid(-1) 了，或者 SIGCHLD 设成了忽略）。
        // 退出码拿不到；那个号从此不能再碰。
        exited = true;
        reaped_elsewhere = true;
        code = -1;
        return code;
    }
    if (si.si_pid == 0) return std::nullopt;  // 还在跑
    exited = true;
    // 被信号打死的照 shell 的规矩记成 128 + 信号号。
    code = si.si_code == CLD_EXITED ? si.si_status : 128 + si.si_status;
    return code;
#endif
}

bool Child::Impl::wait_exit(int ms) const {
#ifdef _WIN32
    return ::WaitForSingleObject(process, static_cast<DWORD>(std::max(0, ms))) == WAIT_OBJECT_0;
#else
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, ms));
    for (;;) {
        if (poll_exit().has_value()) return true;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}

void Child::Impl::kill_tree() {
#ifdef _WIN32
    if (job != nullptr) {
        ::TerminateJobObject(job, 1);
        ::CloseHandle(job);
        job = nullptr;
    } else if (!poll_exit().has_value()) {
        ::TerminateProcess(process, 1);
    }
#else
    poll_exit();  // 先刷新一遍：被别处收走了的话，下面一个信号都不能发
    {
        std::lock_guard<std::mutex> lk(reap_mu);
        // 领头的还没被收尸（活着，或者是个僵尸）时 pid 还占着，这个进程组号不会
        // 给别人。整组杀，领头的已经退了也要杀：npx 读到 EOF 自己走了，node 还在。
        if (!reaped && !reaped_elsewhere) {
            ::kill(-pid, SIGKILL);
            if (!exited) ::kill(pid, SIGKILL);
        }
    }
#endif
    // 杀了就等它真没了。有上限：卡在内核里（D 状态、驱动不放手）的进程
    // SIGKILL 也要等，那种时候 kill 不能跟着一直卡着。
    wait_exit(5000);
}

/// 真收尸（POSIX）。只在整组杀完之后：收了 pid 就还给系统了。
void Child::Impl::reap() {
#ifndef _WIN32
    std::lock_guard<std::mutex> lk(reap_mu);
    if (!exited || reaped || reaped_elsewhere) return;  // 没退（卡在内核里）就不等了
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    reaped = true;
#endif
}

void Child::Impl::stop_reader() {
    stopping.store(true);
    // 拿一下锁再通知：读线程要么还没看条件（会看到 stopping），要么已经在等（收得到）。
    { std::lock_guard<std::mutex> lk(mu); }
    space_cv.notify_all();
    if (!reader.joinable()) {
        std::lock_guard<std::mutex> lk(mu);
        eof = true;
        return;
    }
    // 正常情况下整组一死管道就到 EOF，读线程自己收摊。叫它回来是给
    // "还有个不在组里的进程攥着写端"那种情况的：不叫的话 join 永远等不到。
#ifdef _WIN32
    // CancelSynchronousIo 只取消"正在进行的"那一次读；它这会儿要是恰好在两次
    // 读之间，这一下落空，下一次 ReadFile 照样卡住——所以隔一会儿再来一次，
    // 直到它真收摊。
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu);
            if (cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return eof; })) break;
        }
        if (HANDLE h = reader_self.load()) ::CancelSynchronousIo(h);
    }
#else
    if (wake_wr >= 0) {
        const char b = 1;
        ssize_t w;
        do {
            w = ::write(wake_wr, &b, 1);
        } while (w < 0 && errno == EINTR);
    }
#endif
    reader.join();
#ifdef _WIN32
    if (out_rd != nullptr) ::CloseHandle(out_rd);
    out_rd = nullptr;
#else
    close_fd(out_fd);
#endif
}

// ---------------------------------------------------------------------------
// Child

Child::Child(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Child::~Child() {
    if (impl_) kill();
}

std::unique_ptr<Child> Child::start(const Options& opts, std::string* error) {
    std::string local_err;
    std::string* err = error != nullptr ? error : &local_err;
    if (opts.exe.empty()) {
        *err = "没有给要起的程序";
        return nullptr;
    }
    std::optional<std::string> resolved;
    try {
        resolved = resolve_exe(opts);
    } catch (const std::exception&) {
        resolved.reset();  // 名字里有坏的 UTF-8：当找不到
    }
    if (!resolved.has_value()) {
        *err = not_found_message(opts.exe);
        return nullptr;
    }
#ifdef _WIN32
    // 批处理经 cmd.exe 跑，而 cmd 不认网络路径当当前目录：它不报错，悄悄换到
    // Windows 目录里接着跑——扩展读写的就全是别处的文件。在这儿先说清楚。
    // 放在"目录在不在"前面：不为一个注定跑不了的目录去网络上问一圈。
    if (!opts.cwd.empty() && is_unc(opts.cwd) && is_batch(paths::from_utf8(*resolved))) {
        *err = "批处理不能在网络路径 " + paths::to_utf8(opts.cwd) + " 里跑（" + opts.exe +
               " 要经 cmd.exe 起，cmd 不认这种目录，会悄悄换到 Windows 目录里去跑）";
        return nullptr;
    }
#endif
    if (!opts.cwd.empty()) {
        std::error_code ec;
        if (!fs::is_directory(opts.cwd, ec)) {
            *err = opts.exe + " 起不来：工作目录不存在（" + paths::to_utf8(opts.cwd) + "）";
            return nullptr;
        }
    }

    auto impl = std::make_unique<Impl>();
    // 0 = 不设顶。
    impl->max_line = opts.max_line_bytes > 0 ? opts.max_line_bytes : SIZE_MAX;
    impl->max_queued = opts.max_queued_bytes > 0 ? opts.max_queued_bytes : SIZE_MAX;
    impl->stderr_log = opts.stderr_log;
    try {
        if (!launch(*impl, opts, *resolved, err)) return nullptr;
    } catch (const std::exception&) {
        // UTF-8 转宽字符那一步：参数或环境变量里有坏字节时 MSVC 会抛。
        // 这一层要给人一句话，不是一个穿到对话线程上的异常。
        *err = opts.exe + " 起不来：参数或环境变量里有不是 UTF-8 的字节";
        return nullptr;
    }

    std::unique_ptr<Child> child(new Child(std::move(impl)));
    try {
        Impl* im = child->impl_.get();
        im->reader = std::thread([im] { im->reader_loop(); });
    } catch (const std::system_error&) {
        child->kill(0);
        *err = opts.exe + " 起不来：开不了读它输出的线程";
        return nullptr;
    }
    return child;
}

bool Child::write_line(const std::string& line) { return write_line(line, -1); }

bool Child::write_line(const std::string& line, int timeout_ms) {
    Impl& im = *impl_;
    // 先看它还在不在：它退了而孙子进程还攥着 stdin 时，写是写得进去的，
    // 但那不是调用方想说话的那一个。
    if (im.killing.load() || !alive()) return false;
    const bool bounded = timeout_ms >= 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    std::unique_lock<std::timed_mutex> lk(im.write_mu, std::defer_lock);
    if (bounded) {
        // 等别的线程写完也算在时限里。等不到就回 false，这时候什么都没写，stdin 不算断。
        if (!lk.try_lock_until(deadline)) return false;
    } else {
        lk.lock();
    }
    if (im.in_closed || im.killing.load()) return false;
    WriteResult r = im.write_all(line.data(), line.size(), deadline, bounded);
    if (r == WriteResult::ok) r = im.write_all("\n", 1, deadline, bounded);
    if (r != WriteResult::ok) {
        // 断了、到点了、被叫回来了：这一行可能已经写进去半截，再往后写只会把
        // 对面的协议搅乱。这条 stdin 从此算断了（关掉，对面读到 EOF）。
        im.close_stdin_locked();
        return false;
    }
    return true;
}

std::optional<std::string> Child::read_line(int timeout_ms) {
    Impl& im = *impl_;
    std::unique_lock<std::mutex> lk(im.mu);
    im.cv.wait_for(lk, std::chrono::milliseconds(std::max(0, timeout_ms)),
                   [&] { return !im.lines.empty() || im.eof; });
    if (im.lines.empty()) return std::nullopt;
    std::string s = std::move(im.lines.front());
    im.lines.pop_front();
    im.queued_bytes -= s.size() + kLineOverhead;
    lk.unlock();
    im.space_cv.notify_one();  // 反压那头可能正等着腾地方
    return s;
}

bool Child::alive() const { return !impl_->poll_exit().has_value(); }

std::optional<int> Child::exit_code() const { return impl_->poll_exit(); }

void Child::kill(int grace_ms) {
    Impl& im = *impl_;
    std::lock_guard<std::mutex> guard(im.kill_mu);
    if (im.killed) return;

    // 0. 卡在写上的那一笔先叫回来（它不回来，下面拿不到写锁）。
    im.killing.store(true);
#ifdef _WIN32
    ::SetEvent(im.cancel_event);
#endif
    // 1. 客气一下：关掉它的 stdin。守规矩的扩展读到 EOF 自己收尾退出。
    //    写是叫得回来的（Windows 立刻，POSIX 最多 50 毫秒），拿锁带个上限只是保险。
    {
        std::unique_lock<std::timed_mutex> wl(im.write_mu, std::defer_lock);
        if (wl.try_lock_for(std::chrono::seconds(2))) im.close_stdin_locked();
    }
    // 2. 等它自己走。
    im.wait_exit(grace_ms);
    // 3. 整组杀掉。领头的已经自己退了也要杀：它拉起的孙子进程不一定跟着退。
    im.kill_tree();
    // 4. 收尸（POSIX）。整组杀完才收：收了 pid 就还给系统了。
    im.reap();
    // 5. 第 1 步没拿到写锁的话再来一次。
    {
        std::unique_lock<std::timed_mutex> wl(im.write_mu, std::defer_lock);
        if (wl.try_lock_for(std::chrono::seconds(2))) im.close_stdin_locked();
    }
    // 6. 读线程收摊。
    im.stop_reader();
    im.killed = true;
}

}  // namespace changji::proc
