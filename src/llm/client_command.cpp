// 把本机装着的大模型命令行当后端（`[llm].backend = "command"`）。
//
// 用户 2026-09-18：「增加对 claudecode 和 codex 命令版的支持」。
//
// 这一条路只干一件事：**提示词从标准输入喂进去，把它印出来的当模型的回答。**
// 别的一概复用远端那条——schema 本来就是贴在提示词后面的文字
// （`schema_as_prompt`），本地校验也是同一份，所以结构化输出这边不用另写。

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "llm/call_log.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"
#include "util/proc.hpp"
#include "util/say.hpp"

namespace changji::llm {

namespace {

/// 两个已知命令行的默认参数。
///
/// ⚠️ **写这段时只有 claude（2.1.272）能在手边验，codex 没装。**
/// 所以这张表是"预设"，不是"支持声明"：填错了用户在 `[llm].command_args`
/// 里改一行就能救，而硬编进逻辑里就得重发一个引擎。
///
/// claude 那一串每一项的理由：
///   -p                       非交互，印完就退（帮助里写的就是 useful for pipes）
///   --output-format text     只要正文。json 那档还得再解一层，而我们要的
///                            就是那段字
///   --no-session-persistence 别把每一次生成都存成一个会话；这条流水线
///                            一部电影要叫模型上百次
///   --restricted             **去掉会跑命令和改文件的那些工具**。我们要的
///                            只是"一段字进、一段字出"，而它默认是个带
///                            Bash 和文件编辑的 agent——提示词里全是剧本
///                            正文，不该有任何机会变成对这台机器的操作
///
/// ⚠️ **别加 --bare。** 它写着"OAuth 和钥匙串一概不读，只认 ANTHROPIC_API_KEY"，
/// 而走这条路的人图的正是订阅登录。实测加了就是
/// 「Not logged in · Please run /login」。
const std::vector<std::string>& claude_args() {
    static const std::vector<std::string> v{
        "-p", "--output-format", "text", "--no-session-persistence", "--restricted"};
    return v;
}

/// codex 的非交互子命令。**没在本机验过**，理由见上。
const std::vector<std::string>& codex_args() {
    static const std::vector<std::string> v{"exec", "-"};
    return v;
}

/// 路径里的文件名（不含扩展名），用来认命令。
/// 配置里填的可能是 `claude`，也可能是 `/Users/x/.local/bin/claude`
/// 或者 Windows 上的 `claude.cmd`。
std::string base_name(const std::string& command) {
    std::string s = command;
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const auto dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// 端点这一栏：命令名加参数，**密钥按 argv 的位置抹掉**。
///
/// `redact_secrets`（call_log.cpp）拦不住这一路：它认的是 `Bearer xxx`、
/// `sk-` 打头的长串、和 JSON 里那几个键的值，而命令行参数一样都不沾——
/// `--api-key VALUE` 是**两个独立的 argv**，智谱的密钥又形如
/// `d3b4f0a1e2.9Kx2mQ7v`，既不是 JSON 键值也不以 sk- 打头。
///
/// 失败场景是实打实的：`llm_command_args` 在设置页上是自由文本、配置接口
/// 一个字都不过滤，填成 `["-p","--api-key","d3b4f0a1e2.9Kx2mQ7v"]` 跑一次，
/// index.jsonl 那行的 `endpoint` 就是整把密钥；用户照 docs/提示词日志.md
/// 那句「拷这个目录给别人」把日志发出去，**密钥跟着走**。
/// call_log.hpp 上那条「Authorization 一个字节都不落盘」，在这条路上对应的
/// 就是这一下。**破了是不可逆的。**
///
/// 认的是**位置**，不是形状：上一个 argv 里带 key / token / secret /
/// password 的，下一个整个换成 `***`；`--api-key=VALUE` 这种把值连在同一个
/// argv 里写的，等号后面整段换掉。
///
/// **宁可抹多不抹少**：开关名一个字不动，研究时该看的"这次带的哪几个开关"
/// 照样看得见，而多抹一个值的代价只是少看见一个值。
std::string endpoint_of(const std::string& command,
                        const std::vector<std::string>& args) {
    const auto secretish = [](const std::string& flag) {
        std::string low = flag;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        for (const char* w : {"key", "token", "secret", "password"}) {
            if (low.find(w) != std::string::npos) return true;
        }
        return false;
    };

    std::string out = command;
    bool mask_next = false;
    for (const auto& a : args) {
        if (mask_next) {
            out += " ***";
            mask_next = false;
            continue;
        }
        const auto eq = a.find('=');
        if (eq != std::string::npos) {
            // `--api-key=VALUE`：值就在这个 argv 里，下一个不用动。
            out += " ";
            out += secretish(a.substr(0, eq)) ? a.substr(0, eq + 1) + "***" : a;
            continue;
        }
        out += " " + a;
        // **只有开关才能点名下一个。** 不判 `-` 开头的话，判的是"这个 argv
        // 像不像密钥"，而它可能是**上一个开关的值**：
        // `--append-system-prompt "回答要 token 精简"` 里那句话含 token，
        // 于是紧跟的 `--restricted` 被整个抹成 `***`——而那一栏的用处正是
        // 「这次带了哪几个开关」，`--restricted` 恰好是关掉改文件那些工具的
        // 那一条，日志上看不出带没带。同族的还有 `--max-tokens 8192`。
        mask_next = !a.empty() && a[0] == '-' && secretish(a);
    }
    return out;
}

/// 开一份记录器，构造失败就当作没开日志往下走。
///
/// 长理由在 client.cpp 的同名函数上（「构造这一下抛了，也不许把这一次生成
/// 带走」）：`call_log_options` 里那句 `paths::from_utf8` 在 MSVC 上会抛
/// `filesystem_error`，而它**不是 `LlmError`**，窜出去就把批量那几条整批
/// 打断。
std::optional<CallLog> open_call_log(const Request& req,
                                     const config::LLMConfig& cfg) {
    try {
        return std::optional<CallLog>(std::in_place, "command", "complete", req,
                                      call_log_options(cfg));
    } catch (...) {
        CallLogOptions off;
        off.enabled = false;
        return std::optional<CallLog>(std::in_place, "command", "complete", req,
                                      off);
    }
}

}  // namespace

std::vector<std::string> default_command_args(const std::string& command) {
    const std::string name = base_name(command);
    if (name == "claude") return claude_args();
    if (name == "codex") return codex_args();
    return {};
}

CommandClient::CommandClient(ConfigProvider cfg) : cfg_(std::move(cfg)) {}

CommandClient::CommandClient(config::LLMConfig cfg)
    : cfg_([c = std::move(cfg)] { return c; }) {}

std::string CommandClient::complete(const Request& req, pipeline::CancelToken& tok) {
    const config::LLMConfig cfg = cfg_();
    // 记录器紧跟着配置摆，**在下面那句"没填 llm.command"抛之前**：
    // 没配好也是一次真实的失败，账上不该少这一次。
    // 构造整个包一层，见 open_call_log 上那段。
    auto log_box = open_call_log(req, cfg);
    CallLog& log = *log_box;
    try {
        if (cfg.command.empty()) {
            throw LlmError(SAY("没填 llm.command：要跑哪个程序（比如 claude 或 codex）"));
        }
        // 开跑前先看一眼有没有被取消。**命令行这条路中途打不断**——和远端那条
        // 一样，能查的只有请求前后两个点（见 Client::complete 那段注释）。
        if (tok.cancelled()) throw LlmError(SAY("已取消"));

        std::vector<std::string> args = cfg.command_args;
        if (args.empty()) args = default_command_args(cfg.command);

        // 端点这一栏记的是**命令名加参数**，不是网址——研究时要能一眼看出这次
        // 是喂给 claude 还是 codex、带的哪几个开关。参数里的密钥在
        // endpoint_of 里就抹掉了，见那儿那段。
        log.set_endpoint(endpoint_of(cfg.command, args));

        // ⚠️ **这条路上 model / temperature / reasoning_effort 一个都不发出去**
        // ——用哪个模型、什么温度由那个命令行自己定。研究时别把这三栏当真值
        // （docs/提示词日志.md 里也写着这一句）。不记成空串是因为：空着的话按
        // model 分组就把命令行这条全归进一个空桶，而它其实是按 schema_name
        // 分流过的。
        //
        // **三栏的来路不一样，别一句话带过**：
        //   · `model` 是配置分流出来的（`model_for`）；
        //   · `temperature` 是**调用方填了就用调用方的**，没填才落到配置；
        //   · `reasoning_effort` **压根不是配置项**（`LLMConfig` 里没有这个
        //     字段），它纯粹是这一次调用方要的那个值。
        // 写成"记的是配置里写着什么"的话，按这几栏分桶的人会以为分的是
        // "机器怎么配的"，实际分的是"这一步的调用方要什么"。
        log.set_model(cfg.model_for(req.schema_name),
                      req.temperature.value_or(cfg.temperature_for(req.schema_name)),
                      req.reasoning_effort);

        // schema 贴进提示词——和远端那条同一个函数，不另写一份。
        const std::string prompt =
            req.schema.is_null() || req.schema.empty()
                ? req.prompt
                : schema_as_prompt(req.prompt, req.schema);
        // 喂进 stdin 的就是这一份，也就是模型真正收到的那段字。
        log.set_prompt(prompt);

        const int timeout_ms = static_cast<int>(cfg.command_timeout_s * 1000.0);
        const auto r = proc::run(cfg.command, args, timeout_ms, prompt);
        // 这条路没有 HTTP 状态码，恒 0。
        //
        // proc::Result 把 stdout 和 stderr 并在 out 里，所以失败时那句
        //「Failed to authenticate: OAuth session expired」会进 error.txt 的
        // 第二段——**那正是要留的**。这份 body 里抽不出 usage，记录器会静静地
        // 不记，不抛。
        log.note_response(0, r.out);

        // **「没装这个程序」和「装了但报错」要分开说。** 这是 proc::Result
        // 特意留 launched 这个标志的原因（media/ffmpeg.cpp 也是这么用的）：
        // 两句话把人指向完全不同的方向——一个是去装、一个是去看它说了什么。
        if (!r.launched) {
            throw LlmError(SAYF(
                "找不到 %1：它不在 PATH 上。装好之后在设置页里把路径填全，"
                "或者把 [llm].command 写成绝对路径", cfg.command));
        }
        if (r.timed_out) {
            throw LlmError(SAYF(
                "%1 跑了 %2 秒还没写完，被掐断了。要么这一步太大，"
                "要么它在等什么（比如要登录）。把 llm.command_timeout_s 调大，"
                "或者先在终端里手敲一次看看它说什么",
                cfg.command,
                std::to_string(static_cast<int>(cfg.command_timeout_s))));
        }
        if (tok.cancelled()) throw LlmError(SAY("已取消"));
        if (r.exit_code != 0) {
            // **把它自己那句话原样带出来。** 实测最常见的一条是
            // 「Failed to authenticate: OAuth session expired」——那正是用户
            // 需要知道的，换成"命令行后端失败"等于把理由吃掉。
            // 它走的是 stdout（实测），而 proc::Result 把两股流并在 out 里。
            std::string tail = r.out;
            constexpr std::size_t kMax = 500;
            if (tail.size() > kMax) tail = tail.substr(0, kMax) + "…";
            throw LlmError(SAYF("%1 退出码 %2：%3", cfg.command,
                                std::to_string(r.exit_code),
                                tail.empty() ? SAY("什么都没说") : tail));
        }

        // 跑通了但一个字没吐。**不能当成空回答往下走**：下游按 schema 解析
        // 空串会报一句和这件事毫无关系的解析错。
        std::string out = r.out;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (out.empty()) {
            throw LlmError(SAYF("%1 跑通了，但一个字都没印出来。"
                                "先在终端里手敲一次同样的命令看看",
                                cfg.command));
        }
        log.set_reply(out);
        return out;
    } catch (const LlmError& e) {
        // ⚠️ **裸 `throw;`，异常对象原样往外走。** 写成 `throw e;` 会切片成
        // std::exception，LlmError::status() / is_config_error() 当场失效——
        // 批量那几条靠 is_config_error 止损（见 client.hpp 上那段）。
        log.fail(e.what(), e.status());
        throw;
    } catch (const std::exception& e) {
        log.fail(e.what(), 0);
        throw;
    }
}

}  // namespace changji::llm
