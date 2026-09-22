#pragma once

// 用户在配置里写的一条「命令模板」怎么变成一次子进程调用。
//
// 配乐（[sound].music_command）和时序放大（[upscale].command）都是外部
// 程序——它们在 Python 生态里，我们只约定一条命令和几个占位符。
//
// **不经过 shell**（proc::run 就是这么设计的：cmd.exe 会把逗号分号当分隔
// 符，ffmpeg 滤镜串会被切碎），所以这里要自己把模板切成 argv：按空白切，
// 双引号 / 单引号包起来的当一个参数。占位符是**整个参数替换**（一个参数
// 正好是 `{prompt}` 时换成整段描述，带空格也还是一个参数），也认参数里
// 嵌着的（`--out={out}`）。

#include <map>
#include <string>
#include <vector>

namespace changji::util {

/// 把模板切成参数列表并替换占位符。`vars` 的键不带花括号（"prompt"）。
///
/// 模板是空的或者只有空白时返回空列表——调用方按「没配」处理。
/// 没在 `vars` 里的占位符原样留着，不报错：那多半是用户写给自己脚本的。
std::vector<std::string> expand_command(
    const std::string& tmpl, const std::map<std::string, std::string>& vars);

struct CommandOutcome {
    bool ok = false;
    /// 没跑起来（找不到程序）/ 超时 / 退出码非零，各是什么在这句里。
    std::string error;
    std::string output;
};

/// 跑 `expand_command` 出来的那串。第一个是程序，先在 PATH 里找一遍。
/// `timeout_s` ≤ 0 = 不限时。
CommandOutcome run_command(const std::vector<std::string>& argv,
                           double timeout_s);

}  // namespace changji::util
