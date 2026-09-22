#include "doctor/doctor.hpp"

#include "util/say.hpp"

#include "infer/llama_chat.hpp"

// 能力自检那一段要它（probe_facts / can_produce_line），见文件末尾。
#include "infer/node_pick.hpp"
#include "infer/node_registry.hpp"
#include "infer/node_status.hpp"


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "util/httplib.hpp"
#include <nlohmann/json.hpp>

#include "infer/ggml_abi.hpp"
#include "infer/llama_tts.hpp"
// 画布上限跟着模型走
#include "stages/limits.hpp"
#include "models/hardware.hpp"
#include "llm/client.hpp"
#include "infer/sd_backend.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

namespace changji::doctor {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 把 http://host:port/path 拆成 httplib 要的两段。
///
/// httplib 的 Client 构造函数吃 "scheme://host:port"，路径要单独传给 Get。
/// LLM 的地址通常带 /v1 后缀，不拆开会把它拼到 host 里去。
struct SplitUrl {
    std::string origin;  ///< http://127.0.0.1:11434
    std::string prefix;  ///< /v1，可能为空
    bool ok = false;
};

SplitUrl split_url(const std::string& url) {
    SplitUrl r;
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return r;
    size_t host_start = scheme_end + 3;
    size_t slash = url.find('/', host_start);
    if (slash == std::string::npos) {
        r.origin = url;
        r.prefix = "";
    } else {
        r.origin = url.substr(0, slash);
        r.prefix = url.substr(slash);
        while (!r.prefix.empty() && r.prefix.back() == '/') r.prefix.pop_back();
    }
    r.ok = true;
    return r;
}

/// 发一个 GET 并解析 JSON。任何一步失败都返回 nullopt——
/// 调用方只关心「拿没拿到」，不关心是连不上还是解析失败。
std::optional<json> get_json(const std::string& url, const std::string& path,
                             int timeout_s,
                             const httplib::Headers& headers = {}) {
    SplitUrl s = split_url(url);
    if (!s.ok) return std::nullopt;

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    // 这一版没编进 OpenSSL，httplib 发不了 https。
    //
    // **原来这一句是无条件的**，注释写着"阶段 0 未启用 OpenSSL"——而
    // CHANGJI_SSL 早就默认 ON 了。2026-09-13 大模型的默认地址改成云端
    // （https）之后，这句无条件的 return 会让体检对一个**完全正常**的
    // 配置一律报"连不上"。加上这道 #ifndef 之后，编进了 SSL 的版本
    // 照常去连，没编进去的版本才走这条早退。
    if (s.origin.rfind("https://", 0) == 0) return std::nullopt;
#endif

    httplib::Client cli(s.origin);
    cli.set_connection_timeout(timeout_s, 0);
    cli.set_read_timeout(timeout_s, 0);
    auto res = cli.Get(s.prefix + path, headers);
    if (!res || res->status < 200 || res->status >= 300) return std::nullopt;
    return json::parse(res->body, nullptr, false).is_discarded()
               ? std::nullopt
               : std::optional<json>(json::parse(res->body));
}

Check check_runtime() {
    std::ostringstream os;
#if defined(_WIN32)
    os << "Windows";
#elif defined(__APPLE__)
    os << "macOS";
#elif defined(__linux__)
    os << "Linux";
#else
    os << SAY("未知平台");
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    os << " arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    os << " x86_64";
#endif
    os << "  C++" << (__cplusplus / 100 % 100);
    return {SAY("运行时"), Level::OK, os.str(), ""};
}

Check check_ffmpeg(const config::Settings& s) {
    auto found = proc::which(s.assembly.ffmpeg_path);
    if (found) return {"FFmpeg", Level::OK, *found, ""};
    return {"FFmpeg", Level::FAIL, SAY("未找到"),
            SAY("装配环节的硬依赖。\n"
                "Windows: winget install Gyan.FFmpeg\n"
                "macOS:   brew install ffmpeg\n"
                "Debian:  sudo apt install ffmpeg\n"
                "装好后仍找不到就在配置里填 assembly.ffmpeg_path")};
}

Check check_fonts(const config::Settings& s) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)s;
    return {SAY("中文字体"), Level::OK, SAY("系统自带"), ""};
#else
    if (!proc::which("fc-list")) {
        return {SAY("中文字体"), Level::WARN, SAY("无法检测"), ""};
    }
    auto r = proc::run("fc-list", {});
    std::string low = r.out;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const char* k : {"noto sans cjk", "source han", "wqy", "pingfang"}) {
        if (low.find(k) != std::string::npos) {
            return {SAY("中文字体"), Level::OK, SAY("已安装"), ""};
        }
    }
    return {SAY("中文字体"), Level::WARN, SAY("没找到") + " " + s.assembly.subtitle_font,
            SAY("中文字幕会渲染成方框。\nDebian: sudo apt install fonts-noto-cjk")};
#endif
}

Check check_tts(const config::Settings& s) {
    if (s.tts.backend == "http") {
        if (!s.tts.base_url || s.tts.base_url->empty()) {
            // **两条路都说。** 这段话既给 `changji --doctor`（那儿只能改
            // 配置），也给网页上的体检——而网页那一页往下翻两节就是「配音」
            // 的「服务地址」输入框。只说"去配置里填"的人正对着那个框。
            return {SAY("配音"), Level::FAIL, SAY("配了 http 后端但没填地址"),
                    SAY("界面上：设置页「配音」那一节填「服务地址」。\n"
                        "命令行：配置里填 tts.base_url。")};
        }
        return {SAY("配音"), Level::OK, SAYF("独立服务 %1", *s.tts.base_url), ""};
    }
    // 细节由「进程内配音」那一项报（模型在不在、编没编进来），
    // 这里只说清这条路归谁管。
    if (s.tts.backend == "local") {
        return {SAY("配音"), Level::OK, SAY("走进程内（详见「进程内配音」那一项）"), ""};
    }
    // 走到这里就只剩 http 那条，而它上面已经答过了。
    // **ComfyUI 那条 2026-09-10 拆了**，原来这里会去问它的节点清单，
    // 判断装没装 TTS 节点包。
    return {SAY("配音"), Level::WARN, SAYF("配音后端认不出：%1", s.tts.backend),
            SAY("只能是 local（进程内）或 http（外部服务）")};
}

Check check_llm(const config::Settings& s) {
    // **选了进程内就不该去问那个远端地址。** 不分这一支的话，配了
    // backend = "local" 的人会看到"大模型：连不上 http://…"加一句去起服务
    // ——而那个服务他根本没打算起。**报告说错了比不说更糟**，它把人支去
    // 解决一个不存在的问题。（2026-09-14 到 09-19 之间这条路删过，那阵子
    // 这儿报的是「这条路已经没有了」。）
    if (s.llm.backend == "local") {
        if (!infer::llama_chat_available()) {
            return {SAY("大模型"), Level::FAIL,
                    SAY("配的是进程内跑，但这个二进制没编进来"),
                    SAY("构建时要 CHANGJI_LLAMA=ON；或者去项目页「模型」那一行点一下"
                        "编剧模型的名字，把「跑在哪」改成打接口。")};
        }
        // **没填权重就不是 OK。** 报绿的后果是用户点了「写正文」才撞上一个
        // 运行期错误，而他刚看过一份全绿的体检报告。
        //
        // 是 WARN 不是 FAIL：出片那条路不用大模型，分镜表也可以手写。
        // FAIL 会把制作页的开工按钮一起锁掉。`group` 给界面那颗「去挑模型」。
        if (s.models.llm.empty()) {
            Check c{SAY("大模型"), Level::WARN, SAY("进程内跑，但还没挑编剧模型"),
                    SAY("去项目页「模型」那一行点一下编剧模型的名字，挑一档下载。")};
            c.group = "llm";
            return c;
        }
        std::error_code ec;
        const auto p = s.models.resolve(s.models.llm, s.workspace_path());
        if (!std::filesystem::is_regular_file(p, ec)) {
            Check c{SAY("大模型"), Level::WARN,
                    SAYF("进程内跑，但权重文件不在：%1", s.models.llm),
                    SAY("去项目页「模型」那一行点一下编剧模型的名字，把它下完。")};
            c.group = "llm";
            return c;
        }
        return {SAY("大模型"), Level::OK, SAYF("进程内跑（%1）", s.models.llm), ""};
    }

    // **命令行那条不去连地址。** 它根本不打接口——照旧走下面那段的话，
    // 体检上印的是一个没在用的 base_url 和模型名，而人正是照着这一行判断
    // "我配对了没有"。2026-09-18 加这条后端时就是这样：界面上写着
    // 「https://open.bigmodel.cn … glm-5.3-flash」，而实际跑的是本机的 claude。
    if (s.llm.backend == "command") {
        if (s.llm.command.empty()) {
            return {SAY("大模型"), Level::FAIL, SAY("配的是跑本机命令行，但没说跑哪个"),
                    SAY("去项目页「模型」那一行点一下编剧模型的名字，把「程序」填上"
                        "（比如 claude）。")};
        }
        const auto found = proc::which(s.llm.command);
        if (!found) {
            return {SAY("大模型"), Level::FAIL,
                    SAYF("找不到 %1：它不在 PATH 上", s.llm.command),
                    SAY("装好它，或者把「程序」那一栏填成绝对路径。\n"
                        "claude：npm i -g @anthropic-ai/claude-code\n"
                        "装完先在终端里跑一次、按它说的登录——**这条路用的是那个"
                        "命令行自己的登录，不是这儿的 API Key**。")};
        }
        // **不在体检里真跑一趟。** 跑一次就是一次真生成：要花订阅额度、
        // 要等几十秒，而体检是每次打开设置页都跑的。
        // 登录没登录只有真跑才知道，那句话留给第一次生成时它自己说
        // （llm::CommandClient 会把它原样带出来）。
        return {SAY("大模型"), Level::OK, SAYF("跑本机的 %1", *found), ""};
    }

    const std::string& url = s.llm.base_url;
    const std::string& model = s.llm.model;

    // **密钥没填就别去连。** 刚装好的机器就是这个状态（默认是远端的
    // glm-4.7-flash，密钥要用户自己去领）。不分这一支的话，401 会被
    // get_json 当成失败，报出来是"连不上 https://api.z.ai/…"
    // 外加一句"用 Docker 起 ollama"——三样东西全指错方向，而这正是
    // 上面那段注释说的"报告说错了比不说更糟"。
    if (s.llm.needs_api_key() && s.llm.api_key.empty()) {
        return {SAY("大模型"), Level::WARN, SAYF("还没填 API Key（%1）", url),
                SAY("去项目页「模型」那一行点一下编剧模型的名字，在弹出来的窗口里填。\n"
                    "默认走智谱：去 bigmodel.cn 控制台领一把，默认挑的 glm-4.7-flash 本身不要钱。\n"
                    "不想领密钥就把「跑在哪」改成进程内，挑一档编剧模型下载。")};
    }

    httplib::Headers h{{"Authorization", "Bearer " + s.llm.api_key}};
    auto body = get_json(url, "/models", 8, h);
    if (!body) {
        // 本机服务和云服务该做的事不一样，一句话糊过去会把人支错方向。
        return {SAY("大模型"), Level::WARN, SAYF("连不上 %1", url),
                SAY("剧本和分镜要用它。没有它也能手写分镜表。\n") +
                    (s.llm.needs_api_key()
                         // **不是设置页。** 大模型那一节 2026-09-14 从设置页
                         // 整个搬走了（地址、模型名、密钥、温度都在项目页
                         // 那个弹窗里），这句话还在把人往一个没有这些框的
                         // 页面送——而同一个检查上面那条分支（缺 api_key）
                         // 早就改成指项目页了，两条自相矛盾。
                         ? SAY("地址和密钥在项目页「模型」那一行点一下"
                               "编剧模型的名字，在弹出来的窗口里核一眼；"
                               "国内直连不通的服务要自备网络。")
                         : SAY("用 Docker: docker compose up -d ollama\n"
                               "本机装了 Ollama 就确认它已启动"))};
    }
    std::vector<std::string> names;
    if (body->contains("data") && (*body)["data"].is_array()) {
        for (const auto& m : (*body)["data"]) {
            names.push_back(m.value("id", std::string{}));
        }
    }
    std::string family = model.substr(0, model.find(':'));
    for (const auto& n : names) {
        if (n == model || n.rfind(family, 0) == 0) {
            return {SAY("大模型"), Level::OK, url + "  " + model, ""};
        }
    }
    // **在我们那本小抄上的，不算"没有"。** 见 llm::known_models：
    // 智谱的 /models 只列收费那几个，而默认那个 glm-4.7-flash 正是
    // 免费的、不在列表里、却能用——照 names 判的话，一台配置完全正确
    // 的机器每次体检都要挨这一句，而"报告说错了比不说更糟"。
    for (const auto& [id, note] : llm::known_models(url)) {
        if (id != model) continue;
        return {SAY("大模型"), Level::OK, url + "  " + model,
                SAY("这家的 /models 没把它列出来（免费档常这样），但它是能用的。\n") + note};
    }

    if (!names.empty()) {
        // 服务在跑，只是这份清单里没有配置指定的那个。
        //
        // **不能一口咬定"没有这个模型"。** 2026-09-13 实测：智谱的 /models
        // 只列 glm-4.5 ~ glm-5.3-flash 这些收费的，**免费的 glm-4.7-flash
        // 根本不在里面，而它是能用的**（发过去回 200，服务端回的 model 字段
        // 就是 glm-4.7-flash）。照老话术报的话，一台配置完全正确的机器会被
        // 告知"上面没有这个模型"，还附一句 ollama pull——而这是个云服务。
        std::string list;
        for (size_t i = 0; i < names.size() && i < 5; ++i) {
            if (i) list += SAY("、");
            list += names[i];
        }
        if (names.size() > 5)
            list += SAYF(" 等 %1 个", std::to_string(names.size()));
        return {SAY("大模型"), Level::WARN,
                SAYF("%1 不在 %2 的清单里", model, url),
                SAY("有些平台的 /models 不列免费模型（智谱就是），那样的话这条可以"
                    "不管——写剧本时真调得通就行。\n")
                    + SAYF("清单上有的：%1\n", list)
                    + SAYF("确实写错了的话：去项目页那个模型窗口里改，或者 "
                           "export CHANGJI_LLM_MODEL=%1", names[0])};
    }
    // **云服务和本机服务该做的事不一样**，一句话糊过去会把人支错方向——
    // 上面「连不上」那条早就按 `needs_api_key()` 分了两支，这条漏了：
    // 对着智谱（或者任何一个 /models 回空清单的云服务）说一句
    // 「ollama pull glm-4.7-flash」，照着做只会得到一句找不到命令。
    if (s.llm.needs_api_key()) {
        return {SAY("大模型"), Level::WARN, SAYF("%1 的模型清单是空的", url),
                SAY("有些平台的 /models 本来就不列（智谱免费档就这样），那样的话\n"
                    "这条可以不管——写剧本时真调得通就行。\n"
                    "真连错了地址的话：项目页「模型」那一行点一下编剧模型的名字，\n"
                    "在弹出来的窗口里核一眼。")};
    }
    return {SAY("大模型"), Level::WARN, SAYF("%1 一个模型都没有", url),
            SAYF("拉取一个：ollama pull %1", model)};
}

Check check_gpu(const config::Settings& s) {
    // **走 models::detect_gpu()，不再自己问一遍 nvidia-smi。**
    //
    // 这里原来是一份独立的探测：直接 `nvidia-smi --query-gpu=...`。
    // 于是探测逻辑有了两份，而它们会分岔——2026-09-11 在 Mac 上就分岔了：
    // detect_gpu() 已经会按统一内存算苹果芯片的显存了，doctor 却还只认
    // nvidia-smi，于是一台 Mac 上「显卡」那行永远是"本机未探测到"，
    // 而同一个进程里的档位推导用的是另一个数。
    //
    // 现在只有一份。加一种新硬件只要改 detect_gpu()，体检跟着就对。
    if (const auto gpu = models::detect_gpu(); gpu.has_value()) {
        std::ostringstream os;
        os << gpu->name << "  ";
        os.setf(std::ios::fixed);
        os.precision(1);
        os << gpu->vram_gb() << " GB";
        // **统一内存上这两个数都要写出来。**
        // 只写"107.5 GB"，用户看到的是"我买的明明是 128"；只写 128，
        // 预算又会按 128 算，而超过 Metal 那条线系统就开始压缩换页。
        // 说清楚哪个是哪个，比选一个显示省事得多。
        if (gpu->unified()) {
            // ⚠️ **别用 `std::to_string(double)`**：它固定印六位小数，
            // 16 GB 会写成「16.000000 GB」。原来这儿是 `os << 那个 double`，
            // 走的是流的默认精度（六位**有效数字**），印出来就是 16。
            // `%g` 和它一样。
            char gb_buf[32];
            std::snprintf(gb_buf, sizeof(gb_buf), "%g",
                          static_cast<double>(gpu->unified_mb) / 1024.0);
            os << SAYF("（整机 %1 GB，其余留给系统）", gb_buf);
        }
        if (gpu->count > 1) {
            os << SAYF("（共 %1 张，显存是单卡的）", std::to_string(gpu->count));
        }
        return {SAY("显卡"), Level::OK, os.str(), ""};
    }
    if (s.vram_gb_override) {
        std::ostringstream os;
        os << SAYF("按配置的 %1 GB 推导档位",
                   std::to_string(static_cast<int>(*s.vram_gb_override)));
        return {SAY("显卡"), Level::OK, os.str(), ""};
    }
    // **「推理服务在别的机器上」不能无条件说。**
    //
    // 这句话原来是照 ComfyUI 那套写的，而那一层 2026-09-10 就拆了
    // （见 run_checks 里那条注释：「出图出片都在进程内，没有外部服务要连」）。
    // 今天唯一"渲染在别的机器上"的形态是 `[workers].endpoints` 填了跨机
    // 地址——没填的话出图出片就跑在**这台机器**上，没有显卡就是 CPU，
    // 而「出图后端」那一项自己写着「只能用 CPU 跑，一镜要几小时」。
    //
    // 对一台真没卡的机器无条件说「这是正常的」，等于把唯一一条会提醒
    // "你这台跑不动"的线索说成没事。所以按有没有配 endpoints 分开说。
    const bool offloaded = !s.workers.endpoints.empty();
    return {SAY("显卡"), Level::WARN, SAY("本机未探测到，按 12 GB 估算"),
            offloaded
                ? SAY("渲染交给 [workers].endpoints 上那几台了，本机没卡是正常的。\n"
                      "为了让画质档位推导正确，在配置里填 vram_gb_override")
                : SAY("出图和出片跑在这台机器上——没有外部推理服务那一层了。\n"
                      "真没有显卡的话它会退回 CPU——一镜要几小时，不是慢一点。\n"
                      "有卡却探不到：多半是驱动或者容器没透传进来。\n"
                      "确实要在这台上跑，先把档位推导弄对：在配置里填 "
                      "vram_gb_override。\n"
                      "渲染放在别的机器上是另一条路：填 [workers].endpoints。")};
}

Check check_workspace(const config::Settings& s) {
    fs::path path = s.workspace_path();
    std::error_code ec;
    fs::create_directories(path, ec);
    fs::path probe = path / ".changji_write_test";
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out) {
            return {SAY("项目目录"), Level::FAIL,
                    SAYF("%1 不可写", paths::to_utf8(path)),
                    ec ? ec.message() : SAY("创建测试文件失败")};
        }
        out << "ok";
    }
    fs::remove(probe, ec);
    return {SAY("项目目录"), Level::OK, paths::to_utf8(path), ""};
}

/// 进程内出图后端。
///
/// 没链的话不是错误，是一种部署形态：走 ComfyUI 那条路的用户不需要它，
/// 交叉编译到某些平台时也可能关掉。所以是 OK 加一句说明，不是 WARN——
/// 报告里挂一条永远不会去处理的黄字，会让真正的警告没人看。
/// ggml 的 ABI 自检。
///
/// 这一项和别的体检项不一样：**它查的不是环境，是这个二进制自己编得对不对。**
/// 放进体检报告是因为它防的那类问题没有别的信号——编译过、链接过、
/// 起得来，只有读写张量时慢慢踩坏内存。详见 infer/ggml_abi.hpp。
Check check_ggml() {
    if (!infer::ggml_available()) {
        return {"ggml ABI", Level::OK, SAY("没链 ggml（出图和配音都走外部服务）"), ""};
    }
    const auto abi = infer::check_ggml_abi();
    if (abi.ok) return {"ggml ABI", Level::OK, abi.detail, ""};
    // FAIL 不是 WARN：结构体大小对不上之后，这个进程做的任何推理
    // 都不值得相信，继续跑只会把损坏推到更远的地方。
    return {"ggml ABI", Level::FAIL, abi.detail,
            SAY("多半是构建脚本改动引起的。顶层要有 "
                "add_compile_definitions(GGML_MAX_NAME=160)，"
                "而且 ggml 的头和库必须来自同一份源码树。")};
}

/// 进程内配音编进来了没有。
///
/// **不是 WARN 也不是 FAIL。** 没编进来是完全正常的形态——配音走独立
/// HTTP 服务是相当长一段时间的实际形态（ComfyUI 那条 2026-09-10 拆了）。
/// 报警告等于让报告长期挂一条永远不会去处理的黄字。
/// 这一步有没有**别的机器**能接。
///
/// **体检查的是这台机器，而能不能开工看的是整个集群。** 两者原来是一个数：
/// `Report::can_run()` 就是"本机没有 FAIL"，镜头页那两颗按钮直接用它。
/// 于是加了一台五项全绿的远程机器之后，本机因为没装配音模型仍然判 FAIL，
/// 按钮一直是灰的——而那台机器存在的全部理由，就是本机不用装这些。
/// 2026-09-15 实测：远程 `llm/tts/frame/video/assemble` 全 able，页面上
/// 「只出首帧」「出片」两颗都点不动，提示写着本机缺 [models].tts。
///
/// 走缓存的 snapshot，不额外发探活请求（机器表本来就每 15 秒刷一次）。
bool dispatchable_elsewhere(const config::Settings& s, infer::Capability cap) {
    if (s.peer.nodes.empty()) return false;   // 没登记别的机器，省掉这一趟
    const auto nodes = infer::node_registry().snapshot(s);
    for (const auto* n : infer::candidates_for(nodes, cap)) {
        // "local" 就是 infer::kLocalEndpoint 那个字面量（worker_pool.hpp）。
        // 不 include 那个头：它带着整套 worker 池的声明，而这里只要比一个串。
        if (n->url != "local") return true;
    }
    return false;
}

Check check_local_tts(const config::Settings& settings) {
    const auto probe = infer::probe_llama_tts();
    if (!probe.ok) return {SAY("进程内配音"), Level::WARN, probe.detail, ""};
    if (!infer::llama_tts_available()) {
        return {SAY("进程内配音"), Level::OK, probe.detail, ""};
    }

    // 编进来了，接着查配置。**只有 backend 真选了 local 才判警告**——
    // 编进来但不用它是完全正常的形态。
    const bool selected = settings.tts.backend == "local";
    const auto ws = settings.workspace_path();
    const auto backbone = settings.models.resolve(settings.models.tts, ws);
    const auto decoder =
        settings.models.resolve(settings.models.tts_decoder, ws);

    std::error_code ec;
    const bool has_b =
        !backbone.empty() && std::filesystem::is_regular_file(backbone, ec);
    const bool has_d =
        !decoder.empty() && std::filesystem::is_regular_file(decoder, ec);

    if (has_b && has_d) {
        return {SAY("进程内配音"), Level::OK,
                probe.detail + (selected
                    ? SAY("；两份模型都在，[tts].backend = local")
                    : SAY("；两份模型都在（当前没选它）")),
                ""};
    }
    // 缺哪一份要分别点名：只填一个是最常见的配错法。
    //
    // **分隔符跟着有没有第二项走。** 原来是
    // `(has_b ? "" : "[models].tts ") + (has_d ? "" : "…tts_decoder")`——
    // 只缺 backbone（解码器填了）时，第一段自带的那个尾空格后面什么都没接
    // 上，出来就是「缺模型：[models].tts ，配音会退回估算后端」，空格夹在
    // 字和全角逗号中间。而"只填一个"恰恰是这条注释说的最常见的配错法。
    std::string missing;
    if (!has_b) missing = "[models].tts";
    if (!has_d) {
        if (!missing.empty()) missing += " ";
        missing += "[models].tts_decoder";
    }
    // **别的机器能配音的话，本机缺模型就不是"不能开工"。**
    // 见 dispatchable_elsewhere 上那段：判 FAIL 会把镜头页那两颗按钮锁死，
    // 而这一步根本不在本机跑。降成 WARN——仍然说出来（本机确实没有），
    // 但不再挡着开工。
    const bool elsewhere = selected && dispatchable_elsewhere(
                                           settings, infer::Capability::Tts);
    // ⚠️ **坏在哪要排第一句，内部状态排最后。**
    //
    // 这三条原来一律 `probe.detail + "；…"` 开头，而 probe.detail 说的是
    // 「mtmd 已链入，媒体标记 <__media__>，4 线程」——一句**内部一切正常**
    // 的汇报。于是镜头页上那一大块红字的第一行讲的是 mtmd 和线程数，真正
    // 的那句「缺模型」被挤到后半截。人打开页面看见的是一段开发者才懂的话，
    // 而他要知道的只有两件事：坏了什么、怎么办。
    //
    // 好的那一条（两份都在）照旧把 probe.detail 放前面——那时候它就是答案。
    if (elsewhere) {
        return {SAY("进程内配音"), Level::WARN,
                SAYF("本机缺模型：%1。这一步会派给别的机器（机器表里有能配音的）。",
                     missing) + probe.detail,
                SAY("本机也想跑的话，填 [models].tts 和 [models].tts_decoder；"
                    "只靠别的机器就不用管这条。"),
                "tts"};
    }
    // group = tts：设置页据此摆一颗直接跳到配音模型那个窗的按钮。
    // 不摆的话 `fix` 里写的是「填 [models].tts」——叫人去手改配置文件，
    // 而挑模型下模型那套界面本来就有。
    return {SAY("进程内配音"), selected ? Level::FAIL : Level::OK,
            (selected ? SAYF("缺模型：%1，配音会出静音。", missing)
                      : SAYF("缺模型：%1（当前没选它）。", missing)) +
                probe.detail,
            // **两条路都要说。** 只说"下模型"的话，小卡上的用户下完才
            // 发现配音和出片挤不进同一张卡——而外接一个配音服务不用改
            // 一行代码、也不占本机显存，那多半才是他要的那条。
            selected ? SAY("两条路挑一条：\n"
                           "  本机跑：填 [models].tts（Qwen3-TTS 的 talker）和 "
                           "[models].tts_decoder（tokenizer/解码器），两份都是 "
                           "GGUF。\n"
                           "  外接：把上面的「后端」改成「独立 HTTP 服务」并填"
                           "服务地址，本机就不用装配音模型、也不占显存。")
                     : std::string(),
            selected ? "tts" : ""};
}

Check check_sd(const config::Settings& settings) {
    if (!infer::sd_available()) {
        // **别的机器能出图出片的话，本机没编 sd.cpp 就不是"不能开工"。**
        //
        // 和配音那条同一个道理（见 dispatchable_elsewhere 上那段）：那台
        // 机器存在的全部理由，就是本机不用装这些。2026-09-20 实测撞到的
        // 正是这一处——一台 Mac（CHANGJI_SD=OFF）加了一台五项全绿的 L20，
        // 机器表上 frame / video 都亮着，而镜头页三颗按钮全灰，写着
        // 「出图后端 · 没编进来」。**那台卡是为这件事租的。**
        //
        // 要两样都派得出去才降级：只有一样的话，另一样仍然没人能干，而
        // 出片这条链少哪一段都走不完。
        const bool elsewhere =
            sd_level_without_local(
                dispatchable_elsewhere(settings, infer::Capability::Frame),
                dispatchable_elsewhere(settings, infer::Capability::Video)) ==
            Level::WARN;
        if (elsewhere) {
            return {SAY("出图后端"), Level::WARN,
                    SAY("本机没编进来（构建时 CHANGJI_SD=OFF）。出图出片会派给"
                        "别的机器（机器表里有能干的）"),
                    SAY("本机也想跑的话，换一份编了 sd.cpp 的二进制；"
                        "只靠别的机器就不用管这条。")};
        }
        // **拆掉 ComfyUI 之后这就不再是 OK 了。** 原来的话是"出图走推理
        // 服务"——那个服务没了，没编进 sd.cpp 就是一张图都出不来。
        return {SAY("出图后端"), Level::FAIL, SAY("没编进来，出图出片都跑不了"),
                SAY("这份二进制构建时 CHANGJI_SD=OFF。换一份编了的，"
                    "或者自己编时打开 CHANGJI_SD，再按显卡挑一个 GPU 后端："
                    "N 卡 CHANGJI_SD_CUDA、A 卡 CHANGJI_SD_HIP、"
                    "Intel CHANGJI_SD_SYCL，三家都能用的 CHANGJI_SD_VULKAN。"
                    "一个都不开就只能用 CPU 跑，一镜要几小时。"
                    "\n渲染放在别的机器上是另一条路：在设置页把那台加进机器表，"
                    "本机就只管编排和装配。")};
    }
    // 系统信息里带着编进去的后端和 CPU 特性（AVX2、CUDA 之类）。
    // 这一行是出画质问题时第一个要看的东西：同一份模型在 AVX2 和
    // 纯标量上出的图不一样，而用户不会想到去问"你编的时候开了什么"。
    std::string detail = "sd.cpp " + infer::sd_version();
    const std::string info = infer::sd_system_info();
    if (!info.empty()) detail += "\n" + info;
    return {SAY("出图后端"), Level::OK, detail, ""};
}

/// 定妆和空景那一步用的是哪一份权重。
///
/// **`image_base` 留空时会静默退回 `image`**，而 `image` 那一族是图像
/// **编辑**模型。三视图和空景图是从纯文字画出来的（`ref_gen.cpp` 一张
/// 参考图都不传），拿 Edit 权重做这件事，落在 catalog.cpp 自己那段说明
/// 写的退化路径上：「没有任何参考图的镜头会退化成文生图，那时候它出的
/// 东西不能看」。
///
/// 退回这件事本身是对的——不能因为没配就跑不起来。但它不该**不出声**：
/// 出来的图只是难看，不报任何错，人只会以为"这模型就这水平"。
Check check_image_base(const config::Settings& s) {
    const auto& m = s.models;
    if (m.image.empty()) {
        // 首帧那一份都没配，由「本地模型」那条去说，这儿不重复。
        return {SAY("定妆和空景"), Level::OK,
                SAY("首帧模型还没配，这一项先不论"), ""};
    }
    if (!m.image_base.empty()) {
        return {SAY("定妆和空景"), Level::OK,
                SAYF("走基础模型 %1（文生图）", m.image_base), ""};
    }
    if (!config::ModelsConfig::accepts_reference_images(m.image)) {
        // 首帧那一份本来就是基础权重，两件事用同一个是对的。
        return {SAY("定妆和空景"), Level::OK,
                SAYF("和首帧共用 %1（它本来就是基础权重）", m.image), ""};
    }
    return {SAY("定妆和空景"), Level::WARN,
            SAYF("没配 [models].image_base，这一步会拿首帧那份图像编辑模型"
                 "（%1）做文生图", m.image),
            SAY("三视图和空景图没有参考图可编辑——那是文生图，要基础权重。"
                "用编辑权重做这件事出来的图不报错，只是不能看，"
                "而它们又是后面每一镜的参考图。\n"
                "去项目页「模型」那一行点「定妆和空景模型（文生图）」挑一档下下来。")};
}

/// 本地模型文件。
///
/// **一项都没配现在是 WARN。** 以前是 OK，理由是"走 ComfyUI 那条路的用户
/// 根本不需要这一节"——那条路 2026-09-10 拆了。现在出图出片全在进程内，
/// 一项都没配就等于什么都出不来，报绿是在骗人。
///
/// 配了但文件不在也报警告。这种情况几乎一定是笔误或者模型没下完。
Check check_models(const config::Settings& s) {
    const fs::path ws = s.workspace_path();
    const auto& m = s.models;

    const std::vector<std::pair<const char*, const std::string*>> entries = {
        {"llm", &m.llm},
        {"video", &m.video},
        {"video_vae", &m.video_vae},
        {"video_text_encoder", &m.video_text_encoder},
        {"image", &m.image},
        {"image_base", &m.image_base},
    };

    std::vector<std::string> configured, missing;
    for (const auto& [key, val] : entries) {
        if (val->empty()) continue;
        configured.push_back(key);
        std::error_code ec;
        const fs::path p = m.resolve(*val, ws);
        if (!fs::is_regular_file(p, ec)) {
            missing.push_back(std::string(key) + " -> " + paths::to_utf8(p));
        }
    }

    if (configured.empty()) {
        // 是 WARN 不是 FAIL：装好程序还没下模型是常态，那时候用户仍然
        // 要能进界面、能写剧本分镜。FAIL 会把开工按钮一起锁掉。
        return {SAY("本地模型"), Level::WARN, SAY("一个都没配，出图出片跑不了"),
                SAY("至少要 [models].image（首帧）和 [models].video + "
                    "video_vae（视频）。\n"
                    "相对路径是相对 dir 解析的，dir 留空时是项目库下的 models/。\n")
                    + SAYF("当前 dir：%1", paths::to_utf8(m.dir_path(ws))),
                // 一个都没配时先指向出图那一组：它是第一个非它不可的
                //（没有首帧就没有画面），下完它界面会接着指下一个。
                "image"};
    }

    if (!missing.empty()) {
        std::string detail = SAYF("配了 %1 项，其中 %2 项的文件不存在：",
                                  std::to_string(configured.size()),
                                  std::to_string(missing.size()));
        for (const auto& x : missing) detail += "\n  " + x;
        return {SAY("本地模型"), Level::WARN, detail,
                SAY("检查 [models] 里的文件名，以及 dir 指的目录对不对。\n"
                    "相对路径是相对 dir 解析的，dir 留空时是项目库下的 models/。\n")
                    + SAYF("当前 dir：%1", paths::to_utf8(m.dir_path(ws))),
                "image"};
    }

    return {SAY("本地模型"), Level::OK,
            SAYF("%1 个模型文件都在（%2）", std::to_string(configured.size()),
                 paths::to_utf8(m.dir_path(ws))),
            ""};
}

/// 权重放哪这件事，配置写死的值和这台机器对不对得上。
///
/// **这一项是给"配置跟着人换了机器"准备的。** 2026-09-11 实测撞到：
/// 一份为 96 GB 卡写的 `[models].weights = "cpu"` 跟着配置文件到了一张
/// 32 GB 卡上，而那时模型早换成 Q4 了。后果不报错——sd.cpp 老老实实照做，
/// 把 42 GB 权重全放内存（日志里是 `VRAM 0.00MB`），每一步靠 PCIe 搬，
/// 同一台机器上出首帧那条（没写死，走 smart）却是扩散常驻显存、GPU 99%。
///
/// 程序拦不住人写死，但能说一句"这个值和这台机器算出来的不一样"。
/// **只在真的不一样时才说**：写死成和 smart 一致的值是没问题的，
/// 长期挂一条没用的黄字比不说更糟。
Check check_weights(const config::Settings& s) {
    const auto profile = models::HardwareProfile::detect(s.vram_gb_override);
    const double card = profile.gpu.has_value() ? profile.gpu->vram_gb()
                                                : profile.vram_gb;
    if (card <= 0) {
        return {SAY("权重放哪"), Level::OK,
                SAY("没探到显卡，这一项无从比起"), ""};
    }
    const auto ws = s.workspace_path();
    const auto size_gb = [&](const std::string& rel) {
        std::error_code ec;
        const auto p = s.models.resolve(rel, ws);
        if (p.empty()) return 0.0;
        const auto n = std::filesystem::file_size(p, ec);
        return (!ec && n > 0) ? static_cast<double>(n) / (1024.0 * 1024 * 1024)
                              : 0.0;
    };

    std::vector<std::string> off;
    const auto one = [&](const std::string& label, const std::string& set,
                         const std::string& want) {
        // smart 和 auto 本来就是"让程序/sd.cpp 自己定"，没有对不上这回事。
        if (set == "smart" || set == "auto") return;
        if (set == want) return;
        off.push_back(SAYF("%1：配置写的是 \"%2\"，按这张卡和这个模型算出来"
                           "该是 \"%3\"", label, set, want));
    };
    // unified 要传下去，不然这一项在苹果机器上会一直报"对不上"——
    // 它算的是独显那套，而引擎跑的是统一内存那套。
    const bool unified = profile.gpu.has_value() && profile.gpu->unified();
    // 画布也要传：体检说的"该是什么"必须和真跑那条路算的是同一个数，
    // 否则选了 2K 之后这里会一直报"对不上"（或者反过来一直报"一致"而实际
    // 会 OOM）。
    const auto [cw, ch] = s.video.size();
    const double canvas_px = static_cast<double>(cw) * ch;
    one(SAY("出片"), s.models.weights,
        s.models.weights_for(card, size_gb(s.models.video), unified, canvas_px));
    one(SAY("出首帧"), s.models.image_weights,
        s.models.image_weights_for(card, size_gb(s.models.image), unified));

    if (off.empty()) {
        return {SAY("权重放哪"), Level::OK,
                SAY("配置和这台机器算出来的一致"), ""};
    }
    std::string msg;
    for (std::size_t i = 0; i < off.size(); ++i) {
        if (i) msg += SAY("；");
        msg += off[i];
    }
    return {SAY("权重放哪"), Level::WARN, msg,
            SAY("多半是配置从别的机器带过来的。写死的值不会跟着卡变——\n"
                "换成 \"smart\"（程序按卡和模型大小算）或者 \"auto\"\n"
                "（装载时让 sd.cpp 按实际显存自己塞）就不用管了。\n"
                "确实想按现在这样固定的话，这条忽略即可。")};
}

}  // namespace

namespace {

/// 跑一项检查，异常不外泄。
///
/// 体检的意义就是"环境不对时告诉你哪里不对"，所以它自己最不能因为环境不对
/// 而崩掉。实测踩到过：某一项抛了 std::system_error，异常一路穿到
/// std::terminate，进程以 0xC0000409 消失、一个字不打印，而那个错误码
/// 字面意思是"栈缓冲区溢出"，排查方向完全被带偏。
///
/// 现在单项失败降级成一条 WARN，其余检查照跑。
/// 这台机器能产什么。
///
/// **上面那些项是按"缺什么"组织的，这一项是按"能干什么"组织的。**
/// 两种都要：缺什么告诉你去装什么，能干什么告诉你这台在那张
/// 「机器 × 能力」的表里会亮几格——对等互联之后，那是别的机器
/// 看这台的唯一视角。
///
/// 判断和 /status 用的是同一份（infer::probe_facts），不另算一遍。
Check check_produces(const config::Settings& settings) {
    const auto facts = infer::probe_facts(settings);
    const std::string line = infer::can_produce_line(facts);

    std::string missing;
    for (const auto& r : infer::capabilities_of(facts)) {
        if (r.able) continue;
        if (!missing.empty()) missing += "\n";
        missing += SAYF("%1：%2", infer::label_of(r.cap), r.why);
    }
    if (missing.empty()) return {SAY("能产什么"), Level::OK, line};
    // **不是 FAIL。** 一台只写文不出片的机器是完全正当的用法
    // （build-coord 那份就是），这一项只负责把话说清楚。
    return {SAY("能产什么"), Level::WARN, line, missing};
}

template <typename F>
Check guarded(const char* name, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return {SAY(name), Level::WARN, SAYF("这一项检查自己出错了：%1", e.what()),
                SAY("这是 changji 的问题不是你的环境问题，请把这条报上来。\n"
                    "其余检查不受影响。")};
    } catch (...) {
        return {SAY(name), Level::WARN, SAY("这一项检查自己抛了未知异常"),
                SAY("这是 changji 的问题不是你的环境问题，请把这条报上来。")};
    }
}

}  // namespace

/// 出片的画布有没有超出这个模型能画的范围。
///
/// **超出去不是慢一点，是画面坏掉。** MiniMax-H3 的开源权重把画布钉在
/// canvas_max_pixels = 1032192（1344 × 768，官方 diffusers 的
/// MiniMaxH3Blocks 配置），短边 768；商业 API 主打的 2K（1440 短边）靠的是
/// 一个叫 H3-Regenerate-2K 的模块，**不在开源发布里**。我们的 2k 档
/// （1440 × 2560 = 368 万像素）是它的 3.57 倍，等于让模型在训练分布之外跑。
///
/// **只报警，不悄悄降档。** 人选了 2K 却拿到标准档而且没有提示，比出一条
/// 烂片更糟——config/settings.cpp 的 VideoConfig::size 注释里写过这一条，
/// 这里守着它。
Check check_canvas(const config::Settings& s) {
    const auto& limits = stages::video_limits();
    const auto [w, h] = s.video.size();
    const long px = static_cast<long>(w) * h;
    const std::string got = std::to_string(w) + "×" + std::to_string(h);

    if (limits.max_pixels <= 0) {
        return {SAY("出片画布"), Level::OK,
                SAYF("%1（这个模型没给画布上限）", got), ""};
    }
    if (px <= limits.max_pixels) {
        return {SAY("出片画布"), Level::OK, got, ""};
    }
    const double times = static_cast<double>(px) /
                         static_cast<double>(limits.max_pixels);
    char ratio[32];
    std::snprintf(ratio, sizeof(ratio), "%.2f", times);

    // **建议里那两个尺寸现算，不写死。**
    //
    // 这儿原来写的是「hd（704×1280）」和「--upscale-size 1440x2560」——
    // 两个都是**竖屏**的写法。横屏项目（[video].orientation = landscape）
    // 上 hd 是 1280×704、2K 是 2560×1440，于是这条建议给的数是转置过的，
    // 而上面 `got` 印的又是这个项目真实的画幅——两个数并排摆着对不上，
    // 而人正是在排查画布超限时读到它的。界面那边早就踩过同一条：
    // ShowDialog 里那句注释写着「按当前画幅算——写死的话横屏项目看到的
    // 数全是反的」。
    //
    // 顺带这也是第三、第四份拷贝：真身是 VideoConfig::size()，界面那份在
    // api/labels.js 的 VIDEO_QUALITIES（有用例钉着）。档位改过两回
    // （标准档 2026-09-10 换掉、hd 2026-09-11 加回来），写死的迟早对不上。
    auto size_of = [&](const char* quality) {
        config::VideoConfig v = s.video;
        v.quality = quality;
        return v.size();
    };
    const auto [hd_w, hd_h] = size_of("hd");
    const auto [k2_w, k2_h] = size_of("2k");
    const std::string hd_size =
        std::to_string(hd_w) + "×" + std::to_string(hd_h);
    // `--upscale-size` 收的是 `<WxH>`，小写 x，见 main.cpp 的用法说明
    const std::string k2_arg =
        std::to_string(k2_w) + "x" + std::to_string(k2_h);

    return {SAY("出片画布"), Level::WARN,
            SAYF("%1 超出这个模型的画布上限 %2 像素（%3 倍）", got,
                 std::to_string(limits.max_pixels), ratio),
            // **建议要能照着做。** 上一版只说"出完再跑 changji --upscale"，
            // 可这条命令还要一个 ESRGAN 权重，而那个文件**不在首次运行
            // 的下载清单里**（setup/catalog.cpp 一个超分条目都没有）——
            // 照着做的人会卡在"--upscale 还要 --upscale-model"这句上，
            // 而它没说该去下哪个文件。
            SAYF("模型在训练分布之外跑，出来多半是伪影，不是糊一点。\n"
                 "把 [video].quality 调回 hd（%1）。要 2K 就先出标准档，"
                 "再单独超分：\n"
                 "  changji --upscale 成片.mp4 出来的.mp4 \\\n"
                 "    --upscale-model <RealESRGAN_x4plus.pth 的路径> \\\n"
                 "    --upscale-size %2\n"
                 "那个权重要自己下（Real-ESRGAN 的 v0.1.0 release，67 MB），"
                 "清单里没有。逐帧超分没有帧间一致性，做完要看成片。",
                 hd_size, k2_arg)};
}

/// ⚠️ **这里传的名字只在"这一项自己抛异常了"那条路上用**——正常那条路
/// 上，每个 `check_*` 自己返回的 `Check` 里已经带着翻好的名字了。
/// 所以是 `SAY_NOOP`：字面量留原文，`guarded` 里 `SAY(name)` 那一下才翻。
/// `"FFmpeg"` 和 `"ggml ABI"` 是产品名，不翻。
///
/// 这些名字是**给人看的，不是键**：`doctor_json` 把它们放进 `/api/doctor`
/// 的 `name` 字段，`--doctor` 那一栏原样打出来；查过了，网页那一套和桌面端
/// 都不按名字认，golden 里也没钉着。
Report run_checks(const config::Settings& settings) {
    Report r;
    r.checks.push_back(guarded(SAY_NOOP("运行时"), [&] { return check_runtime(); }));
    r.checks.push_back(guarded("FFmpeg", [&] { return check_ffmpeg(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("中文字体"), [&] { return check_fonts(settings); }));

    // 「推理服务」那一项 2026-09-10 随 ComfyUI 一起去掉了：出图出片都在
    // 进程内，没有外部服务要连。出图那条由「出图后端」和「本地模型」两项管。
    r.checks.push_back(guarded(SAY_NOOP("配音"), [&] { return check_tts(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("大模型"), [&] { return check_llm(settings); }));
    r.checks.push_back(guarded("ggml ABI", [&] { return check_ggml(); }));
    r.checks.push_back(guarded(SAY_NOOP("进程内配音"), [&] { return check_local_tts(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("出图后端"), [&] { return check_sd(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("本地模型"), [&] { return check_models(settings); }));
    r.checks.push_back(
        guarded(SAY_NOOP("定妆和空景"), [&] { return check_image_base(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("显卡"), [&] { return check_gpu(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("权重放哪"), [&] { return check_weights(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("出片画布"), [&] { return check_canvas(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("项目目录"), [&] { return check_workspace(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("能产什么"), [&] { return check_produces(settings); }));
    return r;
}


}  // namespace changji::doctor
