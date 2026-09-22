// 把收到的每个参数各打一行，形如 [参数]。
//
// 测 proc::run 的引用规则要有一个**真正的 .exe** 来收参数。
// 拿 .bat 是不行的：Windows 跑 .bat 一定要经过 cmd.exe，于是 cmd 那套
// 分隔规则（`,` `;` `=` 也算分隔符）又回来了，测出来的是 cmd 的行为，
// 不是 CreateProcessW + CommandLineToArgvW 的行为。
//
// 第一版就是拿 .bat 测的，结果换成 CreateProcessW 之后用例照旧红，
// 而那个红是测试工具带来的，不是被测代码的问题。

#include <cstdio>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::printf("[%s]\n", argv[i]);
    }
    return 0;
}
