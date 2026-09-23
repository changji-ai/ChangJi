// util/proc 的测试：PATH 查找和跑子进程。
//
// 这一层原来没有直接测试，而**体检和整个装配环节都压在它上面**：
// ffmpeg、ffprobe、nvidia-smi、fc-list 全靠它拉起来。
//
// 最要紧的一条是 `Result::launched`——它区分"跑了但失败"和"根本没这个
// 程序"。体检就是靠它决定报"没装 ffmpeg（去装）"还是"ffmpeg 报错了
// （看看这段输出）"。这两句话把人指向完全不同的方向，弄反了很费时间。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "scoped_env.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 建一个临时目录，里面放一个"可执行文件"。
/// 只验 which 找不找得到，不真跑它，所以内容无所谓。
fs::path make_fake_bin(const std::string& filename) {
    const fs::path dir = fs::temp_directory_path() / "changji_which_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::ofstream(dir / filename) << "echo hi\n";
    return dir;
}

}  // namespace

TEST_CASE("which：在 PATH 里找得到") {
#ifdef _WIN32
    const std::string file = "changji_fake_tool.bat";
#else
    const std::string file = "changji_fake_tool";
#endif
    const fs::path dir = make_fake_bin(file);
    const test::ScopedEnv guard("PATH", paths::to_utf8(dir));

#ifdef _WIN32
    // **不带后缀也要找得到。** Windows 上靠 PATHEXT 补后缀，
    // 写死 .exe 会漏掉 .bat 包装的工具——ffmpeg 的某些安装方式就是这样，
    // 那时候体检会报"没装 ffmpeg"而它其实装了。
    const test::ScopedEnv pathext("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    const auto found = proc::which("changji_fake_tool");
#else
    const auto found = proc::which("changji_fake_tool");
#endif
    REQUIRE(found.has_value());
    CHECK(found->find("changji_fake_tool") != std::string::npos);

    std::error_code ec;
}

TEST_CASE("which：找不到就是 nullopt，不是空串") {
    // 空串会被调用方当成"找到了一个名字为空的程序"，然后拿去 run。
    //
    // **PATH 指向一个空目录，而不是清空 PATH。** 清空的话同一个进程里
    // 别的用例会跟着遭殃——第一版就是这么写的，结果 test_readonly 里
    // 那条显卡检测挂了（nvidia-smi 找不到，gpu 变成 null），
    // 而报错看起来跟 proc 毫无关系。
    const fs::path empty = fs::temp_directory_path() / "changji_empty_bin";
    std::error_code ec;
    fs::create_directories(empty, ec);
    const test::ScopedEnv guard("PATH", paths::to_utf8(empty));
    CHECK_FALSE(proc::which("changji_这个程序不存在_12345").has_value());
    fs::remove_all(empty, ec);
}

TEST_CASE("which：已经是路径就直接用，不去翻 PATH") {
    const fs::path dir = make_fake_bin("tool.txt");
    const std::string full = paths::to_utf8(dir / "tool.txt");
    // PATH 指向一个空目录，证明它没去翻 PATH（理由同上一条）。
    const fs::path empty = fs::temp_directory_path() / "changji_empty_bin2";
    std::error_code ec2;
    fs::create_directories(empty, ec2);
    const test::ScopedEnv guard("PATH", paths::to_utf8(empty));
    const auto found = proc::which(full);
    REQUIRE(found.has_value());
    CHECK(*found == full);

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(empty, ec);
}

TEST_CASE("run：程序不存在时 launched 为假，而不是「跑了但退出码非零」") {
    // **这一条是这个文件里最重要的。** 体检靠 launched 决定说哪句话：
    //   launched == false → "没装，去装"
    //   launched == true 而 exit_code != 0 → "装了但报错，看这段输出"
    // 弄反了的话，用户会照着"去装"再装一遍已经装好的东西。
    const auto r = proc::run("changji_这个程序不存在_12345", {"--version"});
    CHECK_FALSE(r.launched);
    CHECK(r.exit_code != 0);
}

TEST_CASE("run：跑得起来的命令，输出和退出码都对") {
    // **挑命令要迁就 run 的实现**：它先 which(exe) 再交给 popen，
    // 所以 exe 必须是 PATH 里查得到的**真文件**。
    // Windows 上 `echo` 是 cmd 的内建命令、不是文件，which 找不到它——
    // 第一版就是这么挂的。改用 where.exe，它一定在，而且输出可预期。
#ifdef _WIN32
    const auto r = proc::run("where.exe", {"cmd"});
    CHECK(r.launched);
    CHECK(r.exit_code == 0);
    // where cmd 会打出 cmd.exe 的完整路径。
    CHECK(r.out.find("cmd") != std::string::npos);
#else
    const auto r = proc::run("echo", {"changji_proc_ok"});
    CHECK(r.launched);
    CHECK(r.exit_code == 0);
    CHECK(r.out.find("changji_proc_ok") != std::string::npos);
#endif
}

TEST_CASE("run：跑起来了但失败，launched 仍然为真") {
    // 「跑起来了但失败」和「没这个程序」是两件事——上一条测前者，
    // 「程序不存在」那条测后者。两者都靠 launched 区分。
#ifdef _WIN32
    // where 查一个不存在的名字：退出码非零，但它自己是跑起来了的。
    const auto r = proc::run("where.exe", {"changji_不存在_98765"});
#else
    const auto r = proc::run("cat", {"/changji_不存在_98765"});
#endif
    CHECK(r.launched);
    CHECK(r.exit_code != 0);
}

TEST_CASE("run：路径里有空格也跑得起来") {
    // 路径里有空格（"C:\\Program Files\\" 那种）是常态。拼命令行时不给每个部分单独
    // 加引号，前半段会被当成命令、后半段当成参数。
    //
    // **拿一个真的带空格的路径去跑**，而不是去看输出里参数有没有被切开
    // ——第一版想用 where.exe 的报错文字来判断，但它不回显搜的那个名字，
    // 那条断言其实什么都没验到。能观察到的才算数。
    const fs::path dir = fs::temp_directory_path() / paths::from_utf8("changji 带空格的目录");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
#ifdef _WIN32
    const fs::path tool = dir / "tool.bat";
    { std::ofstream f(tool); f << "@echo changji_space_ok\n"; }
#else
    const fs::path tool = dir / "tool.sh";
    { std::ofstream f(tool); f << "#!/bin/sh\necho changji_space_ok\n"; }
    fs::permissions(tool, fs::perms::owner_all, ec);
#endif

    const auto r = proc::run(paths::to_utf8(tool), {});
    CHECK(r.launched);
    CHECK_MESSAGE(r.out.find("changji_space_ok") != std::string::npos,
                  "exit=" << r.exit_code << " out=[" << r.out << "]");
    CHECK(r.exit_code == 0);

    fs::remove_all(dir, ec);
}

// ── 中文路径这条线 ─────────────────────────────────────────────────
//
// 这个项目的项目名、模型目录基本都是中文，而 Windows 上**每一个吃窄字符串
// 的 API 都会把 UTF-8 按 ANSI 代码页重新解释**。踩过的地方已经有三处：
//   - stbi_write_png 的 fopen（sd_image.cpp 里有注释）
//   - _popen（proc.cpp，就是上面那几条用例炸出来的）
//   - _putenv_s（测试自己的 ScopedEnv）
//
// 下面这条不测某一个函数，测的是**这条规矩还立着没有**：
// 拿一个中文路径真的跑一次子进程。

TEST_CASE("中文路径也跑得起来（这条规矩每次都得重新验）") {
    // **必须 from_utf8。** 窄字符串字面量在 /utf-8 下是 UTF-8 字节，
    // 而 MSVC 的 fs::path(std::string) 按 ANSI 代码页转宽字符，
    // 中文在 GBK 里往往转不过去，直接抛
    // "No mapping for the Unicode character exists in the target
    // multi-byte code page"。
    //
    // 写这条用例时我自己先踩了一次——**测的就是这条规矩，写的时候还是忘了**。
    // 这大概就是它值得有一条用例的原因。
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_中文目录_测试");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
#ifdef _WIN32
    const fs::path tool = dir / paths::from_utf8("工具.bat");
    { std::ofstream f(tool); f << "@echo changji_cjk_ok\n"; }
#else
    const fs::path tool = dir / paths::from_utf8("工具.sh");
    { std::ofstream f(tool); f << "#!/bin/sh\necho changji_cjk_ok\n"; }
    fs::permissions(tool, fs::perms::owner_all, ec);
#endif

    const auto r = proc::run(paths::to_utf8(tool), {});
    CHECK(r.launched);
    CHECK_MESSAGE(r.out.find("changji_cjk_ok") != std::string::npos,
                  "exit=" << r.exit_code << " out=[" << r.out << "]");

    fs::remove_all(dir, ec);
}

// ── 参数引用 ───────────────────────────────────────────────────────
//
// 每一个 ffmpeg 参数都要过 proc.cpp 里那个 quote()。ffmpeg 的滤镜串里
// 冒号、单引号、逗号、等号、方括号、反斜杠全是家常便饭
// （`subtitles=xx.ass:force_style='Fontsize=24'` 这种），
// 引错一个字符，整条命令的语义就变了——而症状是 ffmpeg 报一句
// 看不懂的参数错误，指不到是我们拼坏的。
//
// **这里不去推 cmd.exe 的引用规则，而是真的跑一遍看回来的是什么。**
// 推规则很容易推出一个"看起来对"的结论，然后测试和实现一起错。

TEST_CASE("参数里的特殊字符原样传过去") {
    // **靶子必须是真的 .exe**（tests/tools/argecho.cpp，构建时一起编出来）。
    // 第一版拿 .bat 当靶子，而 Windows 跑 .bat 一定经过 cmd.exe——
    // 测出来的是 cmd 的分隔规则，不是 CreateProcessW + CommandLineToArgvW 的。
    // 换成 CreateProcessW 之后那条用例照旧红，而那个红是测试工具带来的。
    const std::string exe = CHANGJI_ARGECHO;

    for (const char* arg : {
             "simple",
             "with space",
             "a:b",                                  // 滤镜里的分隔符
             "force_style=QFontsize=24Q",            // 带单引号（下面会换掉）
             "scale=640:-2,fps=24",                  // 逗号和等号
             "[0:v][1:a]",                           // 方括号和冒号
             "C:XXtempXXa b.ass",                    // 反斜杠加空格
             "100%%",                                // 百分号：cmd 会不会吃掉
         }) {
        std::string a = arg;
        // 把占位符换成真的字符，免得源码里堆一层层转义看不清。
        for (auto& c : a) {
            if (c == 'Q') c = 0x27;  // 单引号
        }
        std::string fixed;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] == 'X' && i + 1 < a.size() && a[i + 1] == 'X') {
                fixed += '\\';
                ++i;
            } else {
                fixed += a[i];
            }
        }
        CAPTURE(fixed);
        const auto r = proc::run(exe, {fixed});
        CHECK(r.launched);
        // 回显里必须能原样找到这个参数。找不到就说明被 shell 改写了。
        CHECK_MESSAGE(r.out.find("[" + fixed + "]") != std::string::npos,
                      "回来的是 [" << r.out << "]");
    }

}

TEST_CASE("超时会把子进程杀掉，而不是把调用线程钉住") {
    // **换掉 popen 之前根本做不到这一条**：popen 没有超时接口，
    // 一个卡死的 ffmpeg 会把整个工作线程钉住，而外面看到的只是
    // "这一章一直在跑"，没有任何别的信号。proc.cpp 文件头那条 TODO
    // 写的就是"装配环节接进来之前必须换成 CreateProcess 加超时"。
    const auto t0 = std::chrono::steady_clock::now();
#ifdef _WIN32
    // ping 本机 5 次，每次隔一秒——一个不会秒回、又一定存在的命令。
    const auto r = proc::run("ping.exe", {"-n", "5", "127.0.0.1"}, 800);
#else
    const auto r = proc::run("sleep", {"5"}, 800);
#endif
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    CHECK(r.launched);
    CHECK(r.timed_out);
    // 800 毫秒的超时，不该等满 5 秒。给足余量，只证明"没等到自然结束"。
    CHECK_MESSAGE(elapsed < 4000, "实际等了 " << elapsed << " 毫秒");
}

// ---------------------------------------------------------------------------
// 喂标准输入
// ---------------------------------------------------------------------------
//
// 大模型的命令行后端（claude / codex）靠它把提示词送进去：几万字塞不进命令行
// 参数（Windows 上整条命令行 32 KB 封顶），而 `claude -p` 的帮助写的就是
// "useful for pipes"。
//
// 三条都是"错了不会当场报错"的那一类：
//   · 不关写端 → 子进程一直等下一段，最后靠超时收场，看着像模型很慢；
//   · 写和读不并行 → 管道缓冲（几 KB）一满就双方互等，同样只能靠超时；
//   · POSIX 上不挡 SIGPIPE → 对面读够了就关输入，父进程继续写会**被信号
//     打死**，也就是整个引擎没了。

TEST_CASE("喂标准输入：子进程收得到，而且收得完整") {
#ifdef _WIN32
    const char* exe = "findstr.exe";
    const std::vector<std::string> args{"/n", "^"};  // 每行前面加行号，原样回显
#else
    const char* exe = "cat";
    const std::vector<std::string> args{};
#endif
    const auto r = proc::run(exe, args, 10000, "changji_stdin_ok\n");
    REQUIRE(r.launched);
    CHECK(r.timed_out == false);
    CHECK(r.out.find("changji_stdin_ok") != std::string::npos);
}

TEST_CASE("喂标准输入：不喂的时候行为和以前一模一样") {
    // 空串 = 不接管 stdin。老调用点一个都没改，这条盯着别把它们弄坏。
#ifdef _WIN32
    const auto r = proc::run("where.exe", {"cmd"});
#else
    const auto r = proc::run("echo", {"changji_proc_ok"});
#endif
    REQUIRE(r.launched);
    CHECK(r.exit_code == 0);
}

TEST_CASE("喂标准输入：几十万字也要进得去，而且不会卡死") {
    // **这一条才是真正要防的那件事。** 提示词就是这个量级（拆分镜那一步
    // 光 schema 就几十个字段），而管道缓冲只有几 KB：写和读不并行的话，
    // 这一条会卡到超时为止。给 20 秒，正常应当一秒内回来。
    std::string big;
    big.reserve(300000);
    while (big.size() < 300000) big += "长提示词一行。\n";
#ifdef _WIN32
    // ⚠️ **搜索词别写中文**：findstr 把参数按 ANSI 代码页（GBK）理解，
    // 而喂进去的是 UTF-8——一行都对不上，输出 0 字节，看着像管道没通。
    // `/v` + 一个永远不出现的 ASCII 串 = 每一行原样吐回来，和 cat 一个意思。
    const auto r = proc::run("findstr.exe", {"/v", "/c:changji_never_here"}, 20000, big);
#else
    const auto r = proc::run("cat", {}, 20000, big);
#endif
    REQUIRE(r.launched);
    CHECK_MESSAGE(r.timed_out == false, "卡住了——写和读没有并行");
    // 输出上限是 1 MB（见 run 里那段截断），所以只核对"确实回来了很多"
    CHECK(r.out.size() > 100000);
}

TEST_CASE("喂标准输入：对面读够了就关掉，父进程不能被打死") {
    // POSIX 上继续往已关闭的管道写会收到 SIGPIPE，默认处置是杀掉进程——
    // 也就是把引擎自己打死。head 读够两行就退，正是这个形状。
    // 能跑到断言这一行就说明没被信号带走。
#ifdef _WIN32
    // Windows 上没有 SIGPIPE，WriteFile 直接返回失败，这条不适用
    WARN("Windows 上没有 SIGPIPE，跳过");
#else
    std::string big;
    while (big.size() < 200000) big += "一行\n";
    const auto r = proc::run("head", {"-n", "2"}, 10000, big);
    REQUIRE(r.launched);
    CHECK(r.timed_out == false);
    CHECK(r.out.find("一行") != std::string::npos);
#endif
}

