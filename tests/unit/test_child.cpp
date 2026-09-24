// util/child 的测试：一直跑着、按标准输入输出一行一行说话的子进程。
//
// 靶子是 tests/tools/child_stub.cpp（构建时一起编出来的真 .exe），档位见它的
// 文件头。MCP 的本机扩展全压在这一层上：`npx …` 起不来、关不干净、stderr
// 把它憋住、写卡死在它不读的 stdin 上，表现出来都是"扩展没回话"，从那一头
// 查不到这儿来。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "scoped_env.hpp"
#include "util/child.hpp"
#include "util/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace changji;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

const std::string kStub = CHANGJI_CHILD_STUB;

long long ms_since(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

bool wait_until(const std::function<bool()>& ok, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (ok()) return true;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

long long this_pid() {
#ifdef _WIN32
    return static_cast<long long>(::GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

/// 用完就删的临时目录。名字带进程号和序号：同一台机器上两份测试一起跑时
/// 不会删掉对方正在用的目录。
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        static int seq = 0;
        path = fs::temp_directory_path() /
               paths::from_utf8("changji_child_" + tag + "_" + std::to_string(this_pid()) + "_" +
                                std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::unique_ptr<proc::Child> start_stub(std::vector<std::string> args,
                                        proc::Child::Options opts = {}) {
    opts.exe = kStub;
    opts.args = std::move(args);
    std::string err;
    auto c = proc::Child::start(opts, &err);
    REQUIRE_MESSAGE(c != nullptr, err);
    return c;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// 一个转手调 stub 的包装：Windows 上是 .cmd（走 cmd.exe 那条路），别处是
/// sh 脚本。`npx` 在 Windows 上就是这么个东西。
fs::path write_wrapper(const fs::path& dir, const std::string& name) {
    fs::path stub = paths::from_utf8(kStub);
    stub.make_preferred();
#ifdef _WIN32
    const fs::path p = dir / paths::from_utf8(name + ".cmd");
    std::ofstream f(p, std::ios::binary);
    f << "@\"" << paths::to_utf8(stub) << "\" %*\r\n";
#else
    const fs::path p = dir / paths::from_utf8(name);
    {
        std::ofstream f(p, std::ios::binary);
        f << "#!/bin/sh\nexec \"" << paths::to_utf8(stub) << "\" \"$@\"\n";
    }
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                           fs::perms::others_read | fs::perms::others_exec);
#endif
    return p;
}

/// 孙子进程还在不在。
///
/// Windows 上先拿住它的句柄再问（见 GrandchildWatch）；这边 POSIX 上只有 pid：
/// 它的父进程（stub）死了以后它归 init 收尸，收之前是个僵尸——kill(pid, 0)
/// 对僵尸照样回 0，所以 Linux 上再看一眼 /proc 里的状态。
#ifndef _WIN32
bool pid_gone(long long pid) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return true;
#ifdef __linux__
    const std::string stat = slurp("/proc/" + std::to_string(pid) + "/stat");
    const auto rp = stat.rfind(')');
    if (rp != std::string::npos && rp + 2 < stat.size()) {
        const char state = stat[rp + 2];
        if (state == 'Z' || state == 'X') return true;
    }
#endif
    return false;
}
#endif

/// 盯着一个孙子进程。Windows 上**在它活着的时候就拿住句柄**：只拿 pid 的话，
/// 它死后那个号可能已经分给了别的进程，再按号去问会问到一个不相干的活人。
struct GrandchildWatch {
    long long pid = 0;
#ifdef _WIN32
    HANDLE h = nullptr;
#endif
    explicit GrandchildWatch(long long p) : pid(p) {
#ifdef _WIN32
        h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(p));
#endif
    }
    ~GrandchildWatch() {
#ifdef _WIN32
        if (h != nullptr) ::CloseHandle(h);
#endif
    }
    bool alive_now() const {
#ifdef _WIN32
        return h != nullptr && ::WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
#else
        return !pid_gone(pid);
#endif
    }
    bool gone_within(int ms) const {
#ifdef _WIN32
        return h != nullptr && ::WaitForSingleObject(h, static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
#else
        return wait_until([&] { return pid_gone(pid); }, ms);
#endif
    }
};

/// 起一个会再拉起孙子进程的 stub，等它把孙子的 pid 写下来。
long long start_with_grandchild(std::unique_ptr<proc::Child>& c, const fs::path& pidfile) {
    c = start_stub({"grandchild", paths::to_utf8(pidfile)});
    // stub 先写 pid 文件、再进 echo：echo 回来了，文件就一定写好了。
    REQUIRE(c->write_line("sync"));
    const auto r = c->read_line(5000);
    REQUIRE(r.has_value());
    CHECK(*r == "sync");
    const std::string text = slurp(pidfile);
    REQUIRE_FALSE(text.empty());
    return std::stoll(text);
}

/// 把**这个进程自己的** stderr 临时换成一个文件，析构时换回来。
/// 用来查子进程的 stderr 有没有漏到我们头上。
class ParentStderrToFile {
public:
    explicit ParentStderrToFile(const fs::path& file) {
#ifdef _WIN32
        // 可继承：要是哪天实现退回"继承父进程的 stderr"，子进程就真能写进来。
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        h_ = ::CreateFileW(file.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        old_ = ::GetStdHandle(STD_ERROR_HANDLE);
        if (h_ != INVALID_HANDLE_VALUE) ::SetStdHandle(STD_ERROR_HANDLE, h_);
#else
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
#endif
    }
    ~ParentStderrToFile() {
#ifdef _WIN32
        ::SetStdHandle(STD_ERROR_HANDLE, old_);
        if (h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_);
#else
        std::fflush(stderr);
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
#endif
    }
    ParentStderrToFile(const ParentStderrToFile&) = delete;
    ParentStderrToFile& operator=(const ParentStderrToFile&) = delete;

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE old_ = nullptr;
#else
    int saved_ = -1;
#endif
};

}  // namespace

TEST_CASE("Child：一行一来一回，中文和 1 MB 的长行都原样回来") {
    auto c = start_stub({"echo"});

    CHECK(c->write_line("你好，场记"));
    auto r = c->read_line(3000);
    REQUIRE(r.has_value());
    CHECK(*r == "你好，场记");

    // 顺序不乱：连写三行，按写的顺序回来。
    CHECK(c->write_line("一"));
    CHECK(c->write_line("二"));
    CHECK(c->write_line("三"));
    CHECK(c->read_line(3000) == std::optional<std::string>("一"));
    CHECK(c->read_line(3000) == std::optional<std::string>("二"));
    CHECK(c->read_line(3000) == std::optional<std::string>("三"));

    // 行尾的 \r 去掉：Windows 上的程序爱写 \r\n，那个 \r 不该留在内容里。
    CHECK(c->write_line("crlf\r"));
    CHECK(c->read_line(3000) == std::optional<std::string>("crlf"));

    // **一行 1 MB。** 扩展回一张图（base64）就是这个量级；管道一次只挪几十 KB，
    // 这一行要读十几次才齐。
    std::string big;
    while (big.size() < (1u << 20)) big += "场记 {\"k\":\"v\"} 0123456789 ";
    CHECK(c->write_line(big));
    r = c->read_line(10000);
    REQUIRE(r.has_value());
    CHECK(r->size() == big.size());
    CHECK(*r == big);

    // 带时限的那个写法，对面在读的时候照常写完。
    CHECK(c->write_line(big, 10000));
    r = c->read_line(10000);
    REQUIRE(r.has_value());
    CHECK(*r == big);
    CHECK(c->write_line("短的", 2000));
    CHECK(c->read_line(3000) == std::optional<std::string>("短的"));
}

TEST_CASE("Child：没有新的一行时按时回空，不卡住") {
    auto c = start_stub({"echo"});

    const auto t0 = Clock::now();
    const auto r = c->read_line(200);
    const auto elapsed = ms_since(t0);
    CHECK_FALSE(r.has_value());
    CHECK(elapsed >= 150);
    CHECK_MESSAGE(elapsed < 1500, "等了 " << elapsed << " 毫秒");
    // 超时不是退出：它还活着，还能接着说话。
    CHECK(c->alive());
    CHECK_FALSE(c->exit_code().has_value());
    CHECK(c->write_line("still here"));
    CHECK(c->read_line(3000) == std::optional<std::string>("still here"));
}

TEST_CASE("Child：退出码拿得到；退了之后读立刻回空、写回 false") {
    auto c = start_stub({"exit", "7"});
    REQUIRE(wait_until([&] { return !c->alive(); }, 3000));
    CHECK(c->exit_code() == std::optional<int>(7));
    // 问几遍都是同一个数（POSIX 上只看不收，见 child.cpp 的 poll_exit）。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(c->exit_code() == std::optional<int>(7));
    CHECK_FALSE(c->alive());

    // 退了、stdout 读完了：不该再等满超时。
    const auto t0 = Clock::now();
    CHECK_FALSE(c->read_line(3000).has_value());
    CHECK(ms_since(t0) < 1000);

    // 往一个已经没了的进程写：回 false，不崩（POSIX 上就是不被 SIGPIPE 打死）。
    CHECK_FALSE(c->write_line("anyone?"));
    CHECK_FALSE(c->write_line("anyone?", 500));

    // kill 之后退出码还是它自己的那个，不会被改成"被杀"。
    c->kill();
    CHECK(c->exit_code() == std::optional<int>(7));
}

TEST_CASE("Child：起不来时回空，并说一句指得到地方的人话") {
    std::string err;

    // 给的是名字：说 PATH 里没有。
    proc::Child::Options o;
    o.exe = "changji_no_such_program_xyz";
    CHECK(proc::Child::start(o, &err) == nullptr);
    CHECK(err.find("changji_no_such_program_xyz") != std::string::npos);
    CHECK(err.find("PATH") != std::string::npos);

    // 给的是路径：说"没有这个文件"，不是"PATH 里没有"——人要去查的地方不一样。
    TempDir d("missing");
    o.exe = paths::to_utf8(d.path / "nope.exe");
    err.clear();
    CHECK(proc::Child::start(o, &err) == nullptr);
    CHECK(err.find("nope.exe") != std::string::npos);
    CHECK(err.find("没有这个文件") != std::string::npos);
    CHECK(err.find("PATH") == std::string::npos);

    // 工作目录不存在：程序是有的，那句话要指到那个目录上。
    o.exe = kStub;
    o.args = {"echo"};
    o.cwd = d.path / "no_such_dir";
    err.clear();
    CHECK(proc::Child::start(o, &err) == nullptr);
    CHECK(err.find("工作目录") != std::string::npos);
    CHECK_MESSAGE(err.find(paths::to_utf8(o.cwd)) != std::string::npos, "说的是：" << err);

    // 不要那句话也行，不崩。
    o.exe = "changji_no_such_program_xyz";
    CHECK(proc::Child::start(o, nullptr) == nullptr);
}

TEST_CASE("Child：stderr 灌满也不卡；给了日志文件就追加进去") {
    // 扩展往 stderr 打日志。没人接的话管道一满它就卡在那次写上，
    // 看着像"没回话"。4 MB 远远超过任何管道缓冲。
    {
        auto c = start_stub({"stderr", "4096"});
        CHECK(c->write_line("after"));
        CHECK(c->read_line(5000) == std::optional<std::string>("after"));
    }

    TempDir d("log");
    proc::Child::Options o;
    o.stderr_log = d.path / "sub" / "stub.log";
    fs::create_directories(o.stderr_log.parent_path());
    { std::ofstream(o.stderr_log, std::ios::binary) << "old\n"; }
    {
        auto c = start_stub({"stderr", "4096"}, o);
        CHECK(c->write_line("after"));
        CHECK(c->read_line(5000) == std::optional<std::string>("after"));
        c->kill();
    }
    std::error_code ec;
    const auto size = fs::file_size(o.stderr_log, ec);
    CHECK_FALSE(ec);
    CHECK(size >= 4u * 1024 * 1024 + 4);
    // 追加，不是覆盖：上一回的日志还在开头。
    std::ifstream in(o.stderr_log, std::ios::binary);
    std::string head(4, '\0');
    in.read(head.data(), 4);
    CHECK(head == "old\n");

    // 日志目录不存在也起得来（自己建出来）。
    TempDir d2("log2");
    o.stderr_log = d2.path / "a" / "b" / "stub.log";
    {
        auto c = start_stub({"stderr", "1"}, o);
        CHECK(c->write_line("ok"));
        CHECK(c->read_line(5000) == std::optional<std::string>("ok"));
    }
    CHECK(fs::file_size(o.stderr_log, ec) >= 1024);
}

TEST_CASE("Child：没给日志时 stderr 进空设备，不漏到这个进程自己的 stderr 上") {
    // 漏过来的话，扩展的日志会混进引擎自己的日志里——而且那一路要是个管道，
    // 没人读就又回到"它卡在写 stderr 上"。
    TempDir d("nullerr");
    const fs::path sink = d.path / "parent_stderr.txt";
    {
        const ParentStderrToFile swap(sink);
        auto c = start_stub({"stderr", "64"});
        // stub 先写完 stderr 再进 echo：echo 回来了，那 64 KB 就已经写出去了。
        CHECK(c->write_line("after"));
        CHECK(c->read_line(5000) == std::optional<std::string>("after"));
        c->kill();
    }
    std::error_code ec;
    CHECK(fs::file_size(sink, ec) == 0);
    CHECK_FALSE(ec);
}

TEST_CASE("Child：环境变量叠在这个进程自己的环境上") {
    const test::ScopedEnv inherited("CHANGJI_CHILD_INHERITED", "从父进程来");
    proc::Child::Options o;
    o.env["CHANGJI_CHILD_OVERRIDE"] = "覆盖 value=1";

    {
        auto c = start_stub({"printenv", "CHANGJI_CHILD_OVERRIDE"}, o);
        CHECK(c->read_line(3000) == std::optional<std::string>("覆盖 value=1"));
    }
    {
        // 给了覆盖项，别的照样继承。
        auto c = start_stub({"printenv", "CHANGJI_CHILD_INHERITED"}, o);
        CHECK(c->read_line(3000) == std::optional<std::string>("从父进程来"));
    }
    {
        auto c = start_stub({"printenv", "CHANGJI_CHILD_NEVER_SET"}, o);
        CHECK(c->read_line(3000) == std::optional<std::string>("(unset)"));
    }
#ifdef _WIN32
    {
        // Windows 上名字不分大小写：`Path` 和 `PATH` 是同一个，覆盖要盖到原来那个上，
        // 不能各留一份让子进程看运气。
        const test::ScopedEnv old("CHANGJI_CHILD_CASE", "old");
        proc::Child::Options o2;
        o2.env["changji_child_case"] = "new";
        auto c = start_stub({"printenv", "CHANGJI_CHILD_CASE"}, o2);
        CHECK(c->read_line(3000) == std::optional<std::string>("new"));
    }
#endif
}

TEST_CASE("Child：env 里改了 PATH，就先按改过的 PATH 找程序") {
    // 配置里写「PATH 加上某个 node 的目录」，要用的就是那个 node。
    // 按这个进程自己的 PATH 找的话，找到的是另一个，或者根本找不到。
    TempDir d("pathov");
    write_wrapper(d.path, "changji_only_here");

    proc::Child::Options o;
    o.exe = "changji_only_here";
    o.args = {"echo"};
    std::string err;
    CHECK(proc::Child::start(o, &err) == nullptr);  // 这个进程的 PATH 里没有它

#ifdef _WIN32
    o.env["Path"] = paths::to_utf8(d.path);  // 大小写不同也要认
    o.env["PATHEXT"] = ".CMD";               // 只有改过的 PATHEXT 才认 .cmd
#else
    o.env["PATH"] = paths::to_utf8(d.path);
#endif
    err.clear();
    auto c = proc::Child::start(o, &err);
    REQUIRE_MESSAGE(c != nullptr, err);
    CHECK(c->write_line("via env PATH"));
    // 等得宽一点、红了说清拿到的是什么：2026-09-24 刚编完跑的第一遍红过一次，
    // 单独跑、十二份并行跑都绿，而 `{?} == {?}` 分不出是超时还是回错了话。
    // 这条要多起一个 cmd.exe、跑一个刚写出来的 .cmd——刚落地的文件第一次跑会被
    // 扫一遍，最慢的正是这一步。绿的时候一回就返回，等得宽不花时间。
    const auto got = c->read_line(15000);
    const std::string seen = got ? "got \"" + *got + "\"" : "got nothing within 15 s";
    CHECK_MESSAGE(got == std::optional<std::string>("via env PATH"), seen);
}

TEST_CASE("Child：在指定目录里起，目录名带中文和空格也行") {
    TempDir d("工作 目录");
    proc::Child::Options o;
    o.cwd = d.path;
    auto c = start_stub({"pwd"}, o);
    const auto r = c->read_line(3000);
    REQUIRE(r.has_value());
    // 比"是不是同一个地方"，不比字符串：macOS 上 /var 是 /private/var 的软链。
    std::error_code ec;
    CHECK_MESSAGE(fs::equivalent(paths::from_utf8(*r), d.path, ec), "回来的是 " << *r);
}

TEST_CASE("Child：参数原样过去（空格、引号、&、中文、结尾的反斜杠）") {
    const std::vector<std::string> tricky = {
        "plain",
        "with space",
        "引号\"在中间",
        "a&b|c<d>e",
        "中文 参数",
        "",
        "C:\\dir with space\\",
        "tab\there",
        "%PATH%",
        "^caret^",
        "semi;colon,comma=eq",
        "back\\\\\"slash",
    };
    std::vector<std::string> args = {"args"};
    args.insert(args.end(), tricky.begin(), tricky.end());
    auto c = start_stub(args);
    for (const auto& want : tricky) {
        const auto r = c->read_line(3000);
        REQUIRE(r.has_value());
        CHECK(*r == want);
    }
    CHECK(c->read_line(3000) == std::optional<std::string>("--end--"));
}

TEST_CASE("Child：一条线程写、另一条读，互不干扰") {
    auto c = start_stub({"echo"});
    constexpr int kLines = 300;
    std::thread writer([&] {
        for (int i = 0; i < kLines; ++i) c->write_line("line " + std::to_string(i));
    });
    int got = 0;
    for (int i = 0; i < kLines; ++i) {
        const auto r = c->read_line(5000);
        if (!r.has_value() || *r != "line " + std::to_string(i)) break;
        ++got;
    }
    writer.join();
    CHECK(got == kLines);
}

TEST_CASE("Child：对面不读 stdin 时，卡在写上的那一笔 kill 叫得回来") {
    // 扩展卡死、不读 stdin：管道几十 KB 就满，这一笔写就卡在那儿。
    // 叫不回来的话，关这个扩展的那条线程也跟着卡死在拿写锁上。
    auto c = start_stub({"sleep", "60000"});
    const std::string big(std::size_t{2} << 20, 'w');
    std::atomic<bool> done{false};
    bool result = true;
    Clock::time_point returned_at{};
    std::thread writer([&] {
        result = c->write_line(big);
        returned_at = Clock::now();
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(done.load());  // 真卡住了：2 MB 远超管道缓冲，对面一个字节都不读

    const auto t0 = Clock::now();
    c->kill(100);
    const auto kill_ms = ms_since(t0);
    writer.join();
    CHECK(done.load());
    CHECK_FALSE(result);
    CHECK_MESSAGE(std::chrono::duration_cast<std::chrono::milliseconds>(returned_at - t0).count() <
                      1000,
                  "写那一笔 kill 之后才回来");
    CHECK_MESSAGE(kill_ms < 3000, "kill 花了 " << kill_ms << " 毫秒");
    CHECK_FALSE(c->alive());
}

TEST_CASE("Child：带时限的写到点就回来，这条 stdin 从此算断") {
    auto c = start_stub({"sleep", "60000"});
    const std::string big(std::size_t{2} << 20, 'w');
    const auto t0 = Clock::now();
    CHECK_FALSE(c->write_line(big, 300));
    const auto elapsed = ms_since(t0);
    CHECK(elapsed >= 250);
    CHECK_MESSAGE(elapsed < 1500, "等了 " << elapsed << " 毫秒");
    // 只是写断了，进程还在（要不要关它是调用方的事）。
    CHECK(c->alive());
    // 那一行可能已经进去半截：后面的写一律回 false，而且立刻回。
    const auto t1 = Clock::now();
    CHECK_FALSE(c->write_line("x", 1000));
    CHECK_FALSE(c->write_line("y"));
    CHECK(ms_since(t1) < 200);
    c->kill(100);  // sleep 不理 EOF，别让析构等满默认的宽限期
}

TEST_CASE("Child：超长的一行整行扔掉，后面的照常；给了日志就记一句") {
    TempDir d("maxline");
    proc::Child::Options o;
    o.max_line_bytes = 1024;
    o.stderr_log = d.path / "stub.log";
    auto c = start_stub({"echo"}, o);

    // 正好到顶：照交。
    CHECK(c->write_line(std::string(1024, 'a')));
    CHECK(c->read_line(3000) == std::optional<std::string>(std::string(1024, 'a')));
    // 多一个字节：整行扔掉。
    CHECK(c->write_line(std::string(1025, 'b')));
    // 200 KB：要读好几次才到换行，每一次都要接着扔。
    CHECK(c->write_line(std::string(200 * 1024, 'c')));
    CHECK(c->write_line("after"));
    // 扔掉的那两行拿不到，下一行照常。
    CHECK(c->read_line(3000) == std::optional<std::string>("after"));
    c->kill();

    const std::string log = slurp(o.stderr_log);
    CHECK_MESSAGE(log.find("扔掉") != std::string::npos, "日志里是：" << log);
    CHECK(log.find("1024 字节") != std::string::npos);
}

TEST_CASE("Child：攒着没人取的行满了就停下不读，取走了接着读，一行不丢") {
    // 一个失控的扩展狂往 stdout 写，而对话那头一时没来取：不设顶的话，
    // 这边的内存跟着它一直涨。设了顶，它自己卡在写 stdout 上。
    proc::Child::Options o;
    o.max_queued_bytes = 64 * 1024;
    auto c = start_stub({"spew", "2048"}, o);  // 2 MB，远超这 64 KB 加管道缓冲
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // 没有反压的话 2 MB 早读完了、它早退了。
    CHECK(c->alive());

    const std::string want(1023, 'y');
    int got = 0;
    while (got < 2048) {
        const auto r = c->read_line(5000);
        if (!r.has_value() || *r != want) break;
        ++got;
    }
    CHECK(got == 2048);
    CHECK(wait_until([&] { return !c->alive(); }, 5000));
    CHECK(c->exit_code() == std::optional<int>(0));
}

TEST_CASE("Child：kill 连它拉起的孙子进程一起关掉") {
    // npx 会再拉起 node。只杀 npx 的话 node 留在后台，一直占着端口和内存，
    // 而且攥着同一条 stdout 管道——读线程永远等不到 EOF。
    TempDir d("gc");
    std::unique_ptr<proc::Child> c;
    const long long gpid = start_with_grandchild(c, d.path / "gc.pid");
    const GrandchildWatch gc(gpid);
    REQUIRE(gc.alive_now());

    const auto t0 = Clock::now();
    c->kill();
    // stub 读到 EOF 自己就退了，不该等满宽限期；孙子不理 EOF，靠整组杀。
    CHECK(ms_since(t0) < 4000);
    CHECK_FALSE(c->alive());
    CHECK(gc.gone_within(5000));
    // 读线程也收摊了：读立刻回空。
    const auto t1 = Clock::now();
    CHECK_FALSE(c->read_line(2000).has_value());
    CHECK(ms_since(t1) < 1000);
}

TEST_CASE("Child：kill 可以重复调；不理 EOF 的也杀得掉") {
    auto c = start_stub({"sleep", "60000"});
    const auto t0 = Clock::now();
    c->kill(200);
    CHECK_MESSAGE(ms_since(t0) < 3000, "kill 花了 " << ms_since(t0) << " 毫秒");
    CHECK_FALSE(c->alive());
    REQUIRE(c->exit_code().has_value());
    CHECK(*c->exit_code() != 0);

    // 再调几次：什么都不做，也不崩。
    const auto t1 = Clock::now();
    c->kill();
    c->kill(0);
    CHECK(ms_since(t1) < 500);
    CHECK_FALSE(c->write_line("x"));
    CHECK_FALSE(c->read_line(50).has_value());
}

TEST_CASE("Child：析构就是 kill，孙子进程也跟着没") {
    TempDir d("gc_dtor");
    std::unique_ptr<proc::Child> c;
    const long long gpid = start_with_grandchild(c, d.path / "gc.pid");
    const GrandchildWatch gc(gpid);
    REQUIRE(gc.alive_now());
    c.reset();  // 只析构，不显式 kill
    CHECK(gc.gone_within(5000));
}

#ifndef _WIN32
TEST_CASE("Child：exec 那一步失败也说得出来（没有执行权限、坏的 #!、进不了目录）") {
    // fork 总是成功的；这几种都要到 exec 才露出来。不问清楚的话起来的是一个
    // "活着、但一声不吭"的扩展，人只看得到超时。
    TempDir d("execerr");
    std::string err;
    proc::Child::Options o;

    const fs::path noexec = d.path / "noexec.sh";
    { std::ofstream(noexec) << "#!/bin/sh\necho hi\n"; }
    fs::permissions(noexec, fs::perms::owner_read | fs::perms::owner_write);
    o.exe = paths::to_utf8(noexec);
    CHECK(proc::Child::start(o, &err) == nullptr);
    CHECK(err.find("noexec.sh") != std::string::npos);
    CHECK(err.find("起不来") != std::string::npos);

    const fs::path bad = d.path / "badshebang.sh";
    { std::ofstream(bad) << "#!/changji/no/such/interpreter\n"; }
    fs::permissions(bad, fs::perms::owner_all);
    o.exe = paths::to_utf8(bad);
    err.clear();
    CHECK(proc::Child::start(o, &err) == nullptr);
    CHECK(err.find("badshebang.sh") != std::string::npos);
    CHECK(err.find("起不来") != std::string::npos);

    // 目录在、但进不去（没有 x 权限）：is_directory 过得去，chdir 过不去。
    // root 不受权限管，这一段在 root 下跳过。
    if (::geteuid() != 0) {
        const fs::path locked = d.path / "locked";
        fs::create_directories(locked);
        fs::permissions(locked, fs::perms::none);
        o.exe = kStub;
        o.args = {"echo"};
        o.cwd = locked;
        err.clear();
        CHECK(proc::Child::start(o, &err) == nullptr);
        CHECK(err.find("工作目录") != std::string::npos);
        fs::permissions(locked, fs::perms::owner_all);  // 不然 TempDir 删不掉
    }
}
#endif

#ifdef _WIN32
TEST_CASE("Child：批处理经 cmd.exe 起（路径带空格、按名字从 PATH 找）") {
    // Windows 上 `npx` 其实是 `npx.cmd`。CreateProcessW 不认批处理，要经 cmd.exe，
    // 而 cmd 的引号和 `%` 规矩跟 .exe 那套不一样。
    TempDir d("cmd wrap");
    write_wrapper(d.path, "changji_stub_wrap");

    {
        proc::Child::Options o;
        o.exe = paths::to_utf8(d.path / "changji_stub_wrap.cmd");
        const std::vector<std::string> tricky = {"a b", "中文", "%PATH%", "x&y", "100%", "q(1)"};
        o.args = {"args"};
        o.args.insert(o.args.end(), tricky.begin(), tricky.end());
        std::string err;
        auto c = proc::Child::start(o, &err);
        REQUIRE_MESSAGE(c != nullptr, err);
        for (const auto& want : tricky) {
            const auto r = c->read_line(5000);
            REQUIRE(r.has_value());
            CHECK(*r == want);
        }
        CHECK(c->read_line(3000) == std::optional<std::string>("--end--"));
        CHECK(c->write_line("hello"));
        CHECK(c->read_line(3000) == std::optional<std::string>("hello"));
    }

    {
        // **参数里的引号和 `&` 不能让 cmd 多跑一条命令**（BatBadBut 那一类）。
        // 照 .exe 那套写成 `\"` 的话，cmd 把那个引号当开关，后面的 `&` 就落在
        // 引号外面——这一行会真的建出 pwned.txt。
        // 参数到了 stub 那头怎么拆不管（按 CommandLineToArgvW 拆会拆歪），
        // 只管没有多出来的那条命令。
        proc::Child::Options o;
        o.exe = paths::to_utf8(d.path / "changji_stub_wrap.cmd");
        o.cwd = d.path;
        o.args = {"args", "a\" & type nul > pwned.txt & \"b", "end"};
        std::string err;
        auto c = proc::Child::start(o, &err);
        REQUIRE_MESSAGE(c != nullptr, err);
        bool ended = false;
        for (int i = 0; i < 10 && !ended; ++i) {
            const auto r = c->read_line(5000);
            if (!r.has_value()) break;
            ended = (*r == "--end--");
        }
        CHECK(ended);
        c->kill();
        CHECK_FALSE(fs::exists(d.path / "pwned.txt"));
    }

    {
        // 按名字找：PATHEXT 里有 .CMD，`changji_stub_wrap` 就能找到那个批处理。
        const test::ScopedEnv path("PATH", paths::to_utf8(d.path));
        const test::ScopedEnv pathext("PATHEXT", ".COM;.EXE;.BAT;.CMD");
        proc::Child::Options o;
        o.exe = "changji_stub_wrap";
        o.args = {"echo"};
        std::string err;
        auto c = proc::Child::start(o, &err);
        REQUIRE_MESSAGE(c != nullptr, err);
        CHECK(c->write_line("via PATH"));
        CHECK(c->read_line(5000) == std::optional<std::string>("via PATH"));
    }
}

TEST_CASE("Child：批处理不能在网络路径里跑，直接说清楚") {
    // cmd.exe 不认 `\\server\share` 当当前目录：不报错，悄悄换到 C:\Windows 去跑。
    // 扩展于是读写的全是别处的文件。宁可起不来，也别在错的地方跑。
    TempDir d("unc");
    const fs::path wrap = write_wrapper(d.path, "changji_stub_wrap");
    proc::Child::Options o;
    o.exe = paths::to_utf8(wrap);
    o.args = {"echo"};
    o.cwd = paths::from_utf8("\\\\changji-no-such-host\\share\\dir");
    std::string err;
    const auto t0 = Clock::now();
    CHECK(proc::Child::start(o, &err) == nullptr);
    CHECK_MESSAGE(err.find("网络路径") != std::string::npos, "说的是：" << err);
    // 先判这个、不去网络上问那个目录在不在：一个不存在的主机名能让人等好几秒。
    CHECK(ms_since(t0) < 1000);
}
#endif
