/*
 * 一个一直跑着、按标准输入输出一行一行说话的小程序。
 *
 * `proc::Child` 的靶子，也是扩展（MCP）那几条用例的假扩展（`mcp` 那一档）。
 * 必须是真的 .exe，理由同 argecho.cpp：拿 .bat 当靶子，测出来的是
 * cmd.exe 的规矩，不是被测代码的。
 *
 *   changji_child_stub <mode> [args...]
 *
 *   echo              stdin 每一行原样打回去，读到 EOF 退出（0）
 *   exit <code>       立刻以这个码退出，什么都不打
 *   stderr <kb>       往 stderr 灌这么多 KB 的 'x'，然后同 echo
 *   printenv <name>   打这个环境变量的值（没有就打 `(unset)`），然后同 echo
 *   pwd               打当前目录（UTF-8），然后同 echo
 *   args ...          剩下的参数一个一行，再打 `--end--`，然后同 echo
 *   grandchild <file> 再起一份自己（`sleep 60000`），把它的 pid 写进文件，然后同 echo
 *   sleep <ms>        睡这么久，退出（0）
 *   spew <n>          往 stdout 打 n 行、每行 1023 个 'y'（连换行正好 1 KB），退出（0）
 *   mcp [flags...]    一个最小的 MCP 服务端（一行一个 JSON-RPC 2.0），见 run_mcp
 *
 * 每打一行都 flush。对面是管道，不 flush 的话 CRT 攒满 4 KB 才真写，
 * 用例就等着一行永远不来的回话。
 *
 * Windows 上 stdout 切二进制：文本模式下 CRT 把换行改写成 CR LF，
 * 那样测的是"会不会去掉 CR"，不是"原样回来没有"。stdin 也切：文本模式下
 * 一个 0x1A（Ctrl-Z）就被当成文件尾。
 *
 * ⚠️ 这个文件里的中文只写在块注释里，而且收尾的星号斜杠前面总隔着空白。
 * CMake 没给这个目标 /utf-8，MSVC 按 936 代码页读它：一个中文字的最后
 * 一个字节会跟后面那个字节凑成一个"双字节字"吞掉。吞掉的是行注释的换行，
 * 下一行代码就整行进了注释（2026-09-24 实撞：`using json = …` 那行没了，
 * 报一百个语法错）。块注释里吞掉一个换行或空格无所谓。
 */

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

/*
 * 回包的键按写进去的顺序出来（jsonrpc 在前、id 在后），对着协议文档看得顺眼。
 */
using json = nlohmann::ordered_json;

void out_line(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void sleep_ms(long long ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#ifdef _WIN32
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                              nullptr, nullptr);
    }
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    }
    return out;
}
#endif

/*
 * 命令行参数，UTF-8。Windows 上 `char** argv` 是按 ANSI 代码页编的，
 * 中文参数进来就坏了，而测的正是"中文参数原样过去没有"。
 */
std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int n = 0;
    if (wchar_t** w = ::CommandLineToArgvW(::GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i) out.push_back(narrow(w[i]));
        ::LocalFree(w);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

std::optional<std::string> get_env(const std::string& name) {
#ifdef _WIN32
    const std::wstring wname = widen(name);
    ::SetLastError(0);
    const DWORD need = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0) {
        if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string();
    }
    std::wstring buf(need, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), need);
    buf.resize(got);
    return narrow(buf);
#else
    const char* v = std::getenv(name.c_str());
    if (v == nullptr) return std::nullopt;
    return std::string(v);
#endif
}

std::string current_dir() {
#ifdef _WIN32
    const DWORD need = ::GetCurrentDirectoryW(0, nullptr);
    std::wstring buf(need, L'\0');
    const DWORD got = ::GetCurrentDirectoryW(need, buf.data());
    buf.resize(got);
    return narrow(buf);
#else
    std::vector<char> buf(4096);
    while (::getcwd(buf.data(), buf.size()) == nullptr) {
        if (buf.size() > (1u << 20)) return {};
        buf.resize(buf.size() * 2);
    }
    return std::string(buf.data());
#endif
}

int run_echo() {
    std::string line;
    while (std::getline(std::cin, line)) out_line(line);
    return 0;
}

/*
 * 再起一份自己睡着，pid 写进文件。标准输入输出照样传给它：npx 拉起的
 * node 就是这样攥着同一条管道的，关的时候漏掉它，管道就等不到 EOF。
 */
int run_grandchild(const std::string& self, const std::string& pidfile) {
    long long pid = 0;
#ifdef _WIN32
    (void)self;
    std::wstring me(32768, L'\0');
    me.resize(::GetModuleFileNameW(nullptr, me.data(), static_cast<DWORD>(me.size())));
    std::wstring cmd = L"\"" + me + L"\" sleep 60000";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(me.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        return 2;
    }
    pid = static_cast<long long>(pi.dwProcessId);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    FILE* f = ::_wfopen(widen(pidfile).c_str(), L"wb");
#else
    std::fflush(stdout);
    const pid_t p = ::fork();
    if (p < 0) return 2;
    if (p == 0) {
        ::execl(self.c_str(), self.c_str(), "sleep", "60000", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    pid = static_cast<long long>(p);
    FILE* f = std::fopen(pidfile.c_str(), "wb");
#endif
    if (f == nullptr) return 2;
    const std::string text = std::to_string(pid);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return run_echo();
}

/*
 * ---------------------------------------------------------------------------
 * mcp：一个最小的 MCP 服务端。
 *
 *   --banner       先往 stdout 打一行不是 JSON 的 `stub mcp ready`
 *                  （真有扩展这么干：起来先打一句欢迎词）
 *   --slow-ms=N    每个回话之前睡 N 毫秒
 *   --ping         tools/call 回话之前先反问一句 ping（id "srv-1"），
 *                  等到对面回了再答；等的时候来的别的消息攒着，答完再处理
 *   --die-on-call  一收到 tools/call 就以 3 退出，不回话
 *   --no-init      initialize 不理
 *
 * 工具分两页（tools/list 带 cursor "p2" 拿第二页），为的是测翻页。
 */

struct McpFlags {
    bool banner = false;
    bool ping = false;
    bool die_on_call = false;
    bool no_init = false;
    long long slow_ms = 0;
};

json tools_page1() {
    return json::array({
        json::parse(R"({"name":"echo","title":"Echo","description":"Echo text back","inputSchema":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]},"annotations":{"readOnlyHint":true}})"),
        json::parse(R"({"name":"add","description":"Add two numbers","inputSchema":{"type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"}}}})"),
    });
}

json tools_page2() {
    return json::array({
        json::parse(R"({"name":"fail","description":"Always fails","inputSchema":{"type":"object"}})"),
        json::parse(R"({"name":"env","description":"Read an env var","inputSchema":{"type":"object","properties":{"name":{"type":"string"}}}})"),
        json::parse(R"({"name":"cwd","description":"Current directory","inputSchema":{"type":"object"}})"),
        json::parse(R"({"name":"slow","description":"Sleep then answer","inputSchema":{"type":"object","properties":{"ms":{"type":"number"}}}})"),
    });
}

std::string str_arg(const json& args, const char* key) {
    if (args.is_object() && args.contains(key) && args[key].is_string()) {
        return args[key].get<std::string>();
    }
    return {};
}

double num_arg(const json& args, const char* key) {
    if (args.is_object() && args.contains(key) && args[key].is_number()) {
        return args[key].get<double>();
    }
    return 0.0;
}

json text_result(const std::string& text) {
    json item = json::object();
    item["type"] = "text";
    item["text"] = text;
    json r = json::object();
    r["content"] = json::array();
    r["content"].push_back(item);
    return r;
}

/*
 * 读下一条 JSON 对象。不是 JSON 的行跳过（真扩展会往 stdout 混进别的东西）。
 */
bool read_json(json& out) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        out = std::move(j);
        return true;
    }
    return false;
}

void send(const json& j) {
    /* replace：万一哪个字符串里混进了坏的 UTF-8，照样发得出去，不当场抛。 */
    out_line(j.dump(-1, ' ', false, json::error_handler_t::replace));
}

int run_mcp(const std::vector<std::string>& args) {
    McpFlags f;
    for (const auto& a : args) {
        if (a == "--banner") f.banner = true;
        else if (a == "--ping") f.ping = true;
        else if (a == "--die-on-call") f.die_on_call = true;
        else if (a == "--no-init") f.no_init = true;
        else if (a.rfind("--slow-ms=", 0) == 0) f.slow_ms = std::atoll(a.c_str() + 10);
    }
    std::fputs("stub mcp started\n", stderr);
    std::fflush(stderr);
    if (f.banner) out_line("stub mcp ready");

    auto respond = [&](const json& id, const json& result) {
        sleep_ms(f.slow_ms);
        json r = json::object();
        r["jsonrpc"] = "2.0";
        r["id"] = id;
        r["result"] = result;
        send(r);
    };
    auto respond_error = [&](const json& id, int code, const char* message) {
        sleep_ms(f.slow_ms);
        json e = json::object();
        e["code"] = code;
        e["message"] = message;
        json r = json::object();
        r["jsonrpc"] = "2.0";
        r["id"] = id;
        r["error"] = e;
        send(r);
    };

    /* 等 ping 回话时插进来的消息，答完再处理 */
    std::deque<json> backlog;
    for (;;) {
        json msg;
        if (!backlog.empty()) {
            msg = std::move(backlog.front());
            backlog.pop_front();
        } else if (!read_json(msg)) {
            break;
        }
        /* 没有 method 的是回包之类；没有 id 的是通知，都不回 */
        if (!msg.contains("method") || !msg["method"].is_string()) continue;
        if (!msg.contains("id")) continue;
        const json id = msg["id"];
        const std::string method = msg["method"].get<std::string>();
        const json params =
            msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();

        if (method == "initialize") {
            if (f.no_init) continue;
            json r = json::object();
            r["protocolVersion"] =
                params.contains("protocolVersion") && params["protocolVersion"].is_string()
                    ? params["protocolVersion"]
                    : json("2025-06-18");
            r["capabilities"] = json::object();
            r["capabilities"]["tools"] = json::object();
            r["serverInfo"] = json::object();
            r["serverInfo"]["name"] = "stub";
            r["serverInfo"]["version"] = "1";
            respond(id, r);
        } else if (method == "tools/list") {
            json r = json::object();
            if (params.contains("cursor") && params["cursor"] == "p2") {
                r["tools"] = tools_page2();
            } else {
                r["tools"] = tools_page1();
                r["nextCursor"] = "p2";
            }
            respond(id, r);
        } else if (method == "tools/call") {
            if (f.die_on_call) {
                std::fflush(stdout);
                std::exit(3);
            }
            if (f.ping) {
                json p = json::object();
                p["jsonrpc"] = "2.0";
                p["id"] = "srv-1";
                p["method"] = "ping";
                send(p);
                bool answered = false;
                json m;
                while (read_json(m)) {
                    if (!m.contains("method") && m.contains("id") && m["id"] == "srv-1") {
                        answered = true;
                        break;
                    }
                    backlog.push_back(std::move(m));
                }
                /* 等着等着对面关了 */
                if (!answered) return 0;
            }
            const std::string name = params.contains("name") && params["name"].is_string()
                                         ? params["name"].get<std::string>()
                                         : std::string();
            const json a = params.contains("arguments") && params["arguments"].is_object()
                               ? params["arguments"]
                               : json::object();
            if (name == "echo") {
                respond(id, text_result(str_arg(a, "text")));
            } else if (name == "add") {
                /* 整数就打整数（2+3 打 "5"，不是 "5.0"），不然照 JSON 的写法打小数 */
                const double s = num_arg(a, "a") + num_arg(a, "b");
                const bool integral = std::floor(s) == s && std::fabs(s) < 1e15;
                respond(id, text_result(integral ? std::to_string(static_cast<long long>(s))
                                                 : json(s).dump()));
            } else if (name == "fail") {
                json r = text_result("boom");
                r["isError"] = true;
                respond(id, r);
            } else if (name == "env") {
                respond(id, text_result(get_env(str_arg(a, "name")).value_or("(unset)")));
            } else if (name == "cwd") {
                respond(id, text_result(current_dir()));
            } else if (name == "slow") {
                sleep_ms(static_cast<long long>(num_arg(a, "ms")));
                respond(id, text_result("done"));
            } else {
                respond_error(id, -32602, "unknown tool");
            }
        } else {
            respond_error(id, -32601, "method not found");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const std::vector<std::string> args = utf8_args(argc, argv);
    if (args.size() < 2) {
        std::fputs("usage: changji_child_stub <mode> [args...]\n", stderr);
        return 64;
    }
    const std::string& mode = args[1];
    const std::string arg2 = args.size() > 2 ? args[2] : std::string();

    if (mode == "echo") return run_echo();
    if (mode == "exit") return std::atoi(arg2.c_str());
    if (mode == "stderr") {
        const long long kb = std::atoll(arg2.c_str());
        const std::string chunk(1024, 'x');
        for (long long i = 0; i < kb; ++i) std::fwrite(chunk.data(), 1, chunk.size(), stderr);
        std::fflush(stderr);
        return run_echo();
    }
    if (mode == "printenv") {
        out_line(get_env(arg2).value_or("(unset)"));
        return run_echo();
    }
    if (mode == "pwd") {
        out_line(current_dir());
        return run_echo();
    }
    if (mode == "args") {
        for (std::size_t i = 2; i < args.size(); ++i) out_line(args[i]);
        out_line("--end--");
        return run_echo();
    }
    if (mode == "grandchild") return run_grandchild(args[0], arg2);
    if (mode == "sleep") {
        sleep_ms(std::atoll(arg2.c_str()));
        return 0;
    }
    if (mode == "spew") {
        const long long lines = std::atoll(arg2.c_str());
        const std::string line(1023, 'y');
        for (long long i = 0; i < lines; ++i) out_line(line);
        return 0;
    }
    if (mode == "mcp") {
        return run_mcp(std::vector<std::string>(args.begin() + 2, args.end()));
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 64;
}
