#include "util/cmdline.hpp"

#include <cmath>

#include "util/proc.hpp"

namespace changji::util {

namespace {

/// 按空白切，引号内的算一个。反斜杠不当转义——Windows 路径里全是它。
std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    char quote = 0;
    for (const char c : s) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            in_token = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_token) {
                out.push_back(cur);
                cur.clear();
                in_token = false;
            }
            continue;
        }
        cur += c;
        in_token = true;
    }
    if (in_token) out.push_back(cur);
    return out;
}

}  // namespace

std::vector<std::string> expand_command(
    const std::string& tmpl, const std::map<std::string, std::string>& vars) {
    std::vector<std::string> argv = split_args(tmpl);
    for (std::string& arg : argv) {
        for (const auto& [key, value] : vars) {
            const std::string ph = "{" + key + "}";
            for (std::size_t pos = arg.find(ph); pos != std::string::npos;
                 pos = arg.find(ph, pos + value.size())) {
                arg.replace(pos, ph.size(), value);
            }
        }
    }
    return argv;
}

CommandOutcome run_command(const std::vector<std::string>& argv,
                           double timeout_s) {
    CommandOutcome out;
    if (argv.empty()) {
        out.error = "命令是空的";
        return out;
    }
    // 先在 PATH 里找一遍；找不到就按写的路径试（绝对路径本来就不在 PATH 里）。
    std::string exe = argv.front();
    if (const auto found = proc::which(exe)) exe = *found;
    const std::vector<std::string> rest(argv.begin() + 1, argv.end());
    const int timeout_ms =
        timeout_s > 0.0 ? static_cast<int>(std::lround(timeout_s * 1000.0)) : 0;
    const proc::Result r = proc::run(exe, rest, timeout_ms);
    out.output = r.out;
    if (!r.launched) {
        out.error = "起不来：" + argv.front() + "（找不到程序，或者没有执行权限）";
        return out;
    }
    if (r.timed_out) {
        out.error = "超时被杀掉了（" + std::to_string(static_cast<int>(timeout_s)) +
                    " 秒）";
        return out;
    }
    if (r.exit_code != 0) {
        out.error = "退出码 " + std::to_string(r.exit_code);
        if (!r.out.empty()) {
            // 只留尾巴：Python 的栈是最后几行有用。
            const std::size_t keep = 1200;
            out.error += "：" + (r.out.size() > keep
                                     ? "…" + r.out.substr(r.out.size() - keep)
                                     : r.out);
        }
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace changji::util
