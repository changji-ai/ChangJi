// 把收到的每个参数各打一行，形如 [参数]。
//
// 测 proc::run 的引用规则要有一个**真正的 .exe** 来收参数。
// 拿 .bat 是不行的：Windows 跑 .bat 一定要经过 cmd.exe，于是 cmd 那套
// 分隔规则（`,` `;` `=` 也算分隔符）又回来了，测出来的是 cmd 的行为，
// 不是 CreateProcessW + CommandLineToArgvW 的行为。
//
// 第一版就是拿 .bat 测的，结果换成 CreateProcessW 之后用例照旧红，
// 而那个红是测试工具带来的，不是被测代码的问题。

//
// `--cat`：不打参数，把标准输入**逐字节**原样抄到标准输出。Windows 上
// 没有一个现成的东西做得到这件事——`findstr "^"` 把每个 `\n` 改写成
// `\r\n`，多行的东西就逐字对不上了（命令行后端那几条用例就栽在这儿）。

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--cat") == 0) {
#ifdef _WIN32
        // 文本模式下 CRT 会自己改写换行，两头都切成二进制。
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        char buf[65536];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
            std::fwrite(buf, 1, n, stdout);
        }
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        std::printf("[%s]\n", argv[i]);
    }
    return 0;
}
