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
#include <future>
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
            SAY("装配环节必需。\n"
                "Windows: winget install Gyan.FFmpeg\n"
                "macOS:   brew install ffmpeg\n"
                "Debian:  sudo apt install ffmpeg\n"
                "安装后仍无法找到时，请在配置中填写 assembly.ffmpeg_path")};
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
    return {SAY("中文字体"), Level::WARN, SAY("未找到") + " " + s.assembly.subtitle_font,
            SAY("中文字幕将显示为方框。\n"
                "Debian: sudo apt install fonts-noto-cjk")};
#endif
}

Check check_tts(const config::Settings& s) {
    if (s.tts.backend == "http") {
        if (!s.tts.base_url || s.tts.base_url->empty()) {
            // **两条路都说。** 这段话既给 `changji --doctor`（那儿只能改
            // 配置），也给设置里的环境检测——而界面上有「服务地址」那个输入框，
            // 只说"去配置里填"的人明明有框可填。
            //
            // **路径写全**（2026-09-28 设置页重排起）：环境检测在「通用」里，配音那张
            // 卡并进了「出片」，两者不在同一页了。原来那句「设置页「配音」那一节」
            // 指的是已经没有的「配音」类，照着找会扑空。
            return {SAY("配音"), Level::FAIL, SAY("已选择 HTTP 服务，但未填写服务地址"),
                    SAY("界面上：在「设置 ▸ 出片」的「配音」中填写「服务地址」。\n"
                        "命令行：配置里填 tts.base_url。")};
        }
        return {SAY("配音"), Level::OK, SAYF("HTTP 服务 %1", *s.tts.base_url), ""};
    }
    // 细节由「内置配音」那一项报（模型在不在、编没编进来），
    // 这里只说清这条路归谁管。
    if (s.tts.backend == "local") {
        return {SAY("配音"), Level::OK, SAY("内置（详见「内置配音」一项）"), ""};
    }
    // 走到这里就只剩 http 那条，而它上面已经答过了。
    // **ComfyUI 那条 2026-09-10 拆了**，原来这里会去问它的节点清单，
    // 判断装没装 TTS 节点包。
    return {SAY("配音"), Level::WARN, SAYF("无法识别的配音方式：%1", s.tts.backend),
            SAY("仅支持 local（内置）或 http（外部服务）")};
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
                    SAY("已配置为本地运行，但当前程序未包含本地大模型"),
                    SAY("需要以 CHANGJI_LLAMA=ON 构建；或在「设置 ▸ 大模型」"
                        "中将默认模型改为某个 API 服务的模型。")};
        }
        // **没填权重就不是 OK。** 报绿的后果是用户点了「写正文」才撞上一个
        // 运行期错误，而他刚看过一份全绿的体检报告。
        //
        // 是 WARN 不是 FAIL：出片那条路不用大模型，分镜表也可以手写。
        // FAIL 会把制作页的开工按钮一起锁掉。`group` 给界面那颗「去挑模型」。
        if (s.models.llm.empty()) {
            Check c{SAY("大模型"), Level::WARN, SAY("本地运行，但尚未选择编剧模型"),
                    SAY("请在「设置 ▸ 大模型」的「本地模型」中选择一个版本下载。")};
            c.group = "llm";
            return c;
        }
        std::error_code ec;
        const auto p = s.models.resolve(s.models.llm, s.workspace_path());
        if (!std::filesystem::is_regular_file(p, ec)) {
            Check c{SAY("大模型"), Level::WARN,
                    SAYF("本地运行，但权重文件不存在：%1", s.models.llm),
                    SAY("请在「设置 ▸ 大模型」的「本地模型」中重新下载。")};
            c.group = "llm";
            return c;
        }
        return {SAY("大模型"), Level::OK, SAYF("本地运行（%1）", s.models.llm), ""};
    }

    // **命令行那条不去连地址。** 它根本不打接口——照旧走下面那段的话，
    // 体检上印的是一个没在用的 base_url 和模型名，而人正是照着这一行判断
    // "我配对了没有"。2026-09-18 加这条后端时就是这样：界面上写着
    // 「https://open.bigmodel.cn … glm-5.3-flash」，而实际跑的是本机的 claude。
    if (s.llm.backend == "command") {
        if (s.llm.command.empty()) {
            return {SAY("大模型"), Level::FAIL, SAY("已配置为命令行方式，但未指定程序"),
                    SAY("请在配置文件中填写 [llm].command（例如 claude）。")};
        }
        const auto found = proc::which(s.llm.command);
        if (!found) {
            return {SAY("大模型"), Level::FAIL,
                    SAYF("找不到 %1：不在 PATH 中", s.llm.command),
                    SAY("请安装该程序，或将 [llm].command 设为绝对路径。\n"
                        "claude：npm i -g @anthropic-ai/claude-code\n"
                        "安装后请先在终端中运行一次并按提示登录。"
                        "此方式使用该命令行工具自身的登录，而非此处的 API Key。")};
        }
        // **不在体检里真跑一趟。** 跑一次就是一次真生成：要花订阅额度、
        // 要等几十秒，而体检是每次打开设置页都跑的。
        // 登录没登录只有真跑才知道，那句话留给第一次生成时它自己说
        // （llm::CommandClient 会把它原样带出来）。
        return {SAY("大模型"), Level::OK, SAYF("使用本机命令行 %1", *found), ""};
    }

    const std::string& url = s.llm.base_url;
    const std::string& model = s.llm.model;

    // **密钥没填就别去连。** 刚装好的机器就是这个状态（默认是远端的
    // glm-4.7-flash，密钥要用户自己去领）。不分这一支的话，401 会被
    // get_json 当成失败，报出来是"连不上 https://api.z.ai/…"
    // 外加一句"用 Docker 起 ollama"——三样东西全指错方向，而这正是
    // 上面那段注释说的"报告说错了比不说更糟"。
    if (s.llm.needs_api_key() && s.llm.api_key.empty()) {
        return {SAY("大模型"), Level::WARN, SAYF("未填写 API Key（%1）", url),
                SAY("请在「设置 ▸ 大模型」中点击该服务的「修改」填写密钥。\n"
                    "默认使用智谱：可在 bigmodel.cn 控制台申请密钥，"
                    "默认模型 glm-4.7-flash 免费。\n"
                    "如不想申请密钥，可在「设置 ▸ 大模型」的「本地模型」"
                    "中下载编剧模型，在本机运行。")};
    }

    httplib::Headers h{{"Authorization", "Bearer " + s.llm.api_key}};
    auto body = get_json(url, "/models", 8, h);
    if (!body) {
        // 本机服务和云服务该做的事不一样，一句话糊过去会把人支错方向。
        return {SAY("大模型"), Level::WARN, SAYF("无法连接 %1", url),
                SAY("编写剧本和分镜需要大模型；也可以手动编写分镜表。\n") +
                    (s.llm.needs_api_key()
                         // **不是设置页。** 大模型那一节 2026-09-14 从设置页
                         // 整个搬走了（地址、模型名、密钥、温度都在项目页
                         // 那个弹窗里），这句话还在把人往一个没有这些框的
                         // 页面送——而同一个检查上面那条分支（缺 api_key）
                         // 早就改成指项目页了，两条自相矛盾。
                         ? SAY("请在「设置 ▸ 大模型」中点击该服务的「修改」核对地址和密钥；"
                               "在国内无法直接访问的服务需自行配置网络。")
                         : SAY("使用 Docker：docker compose up -d ollama\n"
                               "如本机已安装 Ollama，请确认其已启动"))};
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
                SAY("该服务的 /models 未列出此模型（免费模型常见此情况），"
                    "但可以正常使用。\n") + note};
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
                SAYF("%1 不在 %2 的模型列表中", model, url),
                SAY("部分平台的 /models 不列出免费模型（如智谱），此时可忽略本条，"
                    "只要编写剧本时能正常调用即可。\n")
                    + SAYF("列表中的模型：%1\n", list)
                    + SAYF("如确实填写有误，请在「设置 ▸ 大模型」中点击该服务的「选择模型…」"
                           "重新选择，或执行 export CHANGJI_LLM_MODEL=%1", names[0])};
    }
    // **云服务和本机服务该做的事不一样**，一句话糊过去会把人支错方向——
    // 上面「连不上」那条早就按 `needs_api_key()` 分了两支，这条漏了：
    // 对着智谱（或者任何一个 /models 回空清单的云服务）说一句
    // 「ollama pull glm-4.7-flash」，照着做只会得到一句找不到命令。
    if (s.llm.needs_api_key()) {
        return {SAY("大模型"), Level::WARN, SAYF("%1 的模型列表为空", url),
                SAY("部分平台的 /models 本身不返回模型列表（如智谱免费版），此时\n"
                    "可忽略本条，只要编写剧本时能正常调用即可。\n"
                    "如地址确实有误，请在「设置 ▸ 大模型」中点击该服务的「修改」\n"
                    "进行核对。")};
    }
    return {SAY("大模型"), Level::WARN, SAYF("%1 中没有任何模型", url),
            SAYF("请拉取一个模型：ollama pull %1", model)};
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
            os << SAYF("（整机 %1 GB，其余预留给系统）", gb_buf);
        }
        if (gpu->count > 1) {
            os << SAYF("（共 %1 张，显存按单卡计）", std::to_string(gpu->count));
        }
        return {SAY("显卡"), Level::OK, os.str(), ""};
    }
    if (s.vram_gb_override) {
        std::ostringstream os;
        os << SAYF("按配置的 %1 GB 推算档位",
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
    return {SAY("显卡"), Level::WARN, SAY("未检测到显卡，按 12 GB 估算"),
            offloaded
                ? SAY("渲染已交给 [workers].endpoints 中的机器，本机无显卡属正常情况。\n"
                      "为使画质档位推算正确，请在配置中填写 vram_gb_override")
                : SAY("出图和出片将在本机运行。\n"
                      "如确实没有显卡，将改用 CPU 运行，每镜需要数小时。\n"
                      "如有显卡却未检测到，通常是驱动问题或容器未透传显卡。\n"
                      "如需在本机运行，请先在配置中填写 vram_gb_override，"
                      "使档位推算正确。\n"
                      "也可以将渲染交给其他机器：填写 [workers].endpoints。")};
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
        return {"ggml ABI", Level::OK, SAY("未链接 ggml（出图和配音均使用外部服务）"), ""};
    }
    const auto abi = infer::check_ggml_abi();
    if (abi.ok) return {"ggml ABI", Level::OK, abi.detail, ""};
    // FAIL 不是 WARN：结构体大小对不上之后，这个进程做的任何推理
    // 都不值得相信，继续跑只会把损坏推到更远的地方。
    return {"ggml ABI", Level::FAIL, abi.detail,
            SAY("通常由构建脚本的改动引起。"
                "顶层需要 add_compile_definitions(GGML_MAX_NAME=160)，"
                "且 ggml 的头文件和库必须来自同一份源码树。")};
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
    if (!probe.ok) return {SAY("内置配音"), Level::WARN, probe.detail, ""};
    if (!infer::llama_tts_available()) {
        return {SAY("内置配音"), Level::OK, probe.detail, ""};
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
        return {SAY("内置配音"), Level::OK,
                probe.detail + (selected
                    ? SAY("；两份模型均已就绪，[tts].backend = local")
                    : SAY("；两份模型均已就绪（当前未使用）")),
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
    // 「机器表」不再是界面上的名字（2026-09-28 起那一类叫「互联」），括号里跟着界面叫，
    // 人才找得到是哪几台在替他干。
    if (elsewhere) {
        return {SAY("内置配音"), Level::WARN,
                SAYF("本机缺少模型：%1。此步骤将分派给其他机器（「互联」"
                     "中有可配音的机器）。",
                     missing) + probe.detail,
                SAY("如需在本机运行，请填写 [models].tts 和 [models].tts_decoder；"
                    "仅使用其他机器时可忽略本条。"),
                "tts"};
    }
    // group = tts：设置页据此摆一颗直接跳到配音模型那个窗的按钮。
    // 不摆的话 `fix` 里写的是「填 [models].tts」——叫人去手改配置文件，
    // 而挑模型下模型那套界面本来就有。
    return {SAY("内置配音"), selected ? Level::FAIL : Level::OK,
            (selected ? SAYF("缺少模型：%1，配音将为静音。", missing)
                      : SAYF("缺少模型：%1（当前未使用）。", missing)) +
                probe.detail,
            // **两条路都要说。** 只说"下模型"的话，小卡上的用户下完才
            // 发现配音和出片挤不进同一张卡——而外接一个配音服务不用改
            // 一行代码、也不占本机显存，那多半才是他要的那条。
            //
            // 外接那一句**报全路径、照界面上的字写**：原来写的是「把上面的「后端」
            // 改成「独立 HTTP 服务」」——那是体检还摆在配音那一页底下时的说法。
            // 2026-09-28 起环境检测在「通用」里、配音那张卡在「出片」里，「上面」
            // 什么都没有；那一行也不叫「后端」了，叫「配音」，选项是「HTTP 服务」。
            selected ? SAY("可任选以下一种方式：\n"
                           "  本机运行：填写 [models].tts（Qwen3-TTS 的 talker）"
                           "和 [models].tts_decoder（tokenizer/解码器），"
                           "两者均为 GGUF 文件。\n"
                           "  外部服务：在「设置 ▸ 出片」的「配音」中选择「HTTP 服务」"
                           "并填写「服务地址」，本机无需安装配音模型，也不占用显存。")
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
                    SAY("本机程序未包含出图组件（构建时 CHANGJI_SD=OFF）。"
                        "出图和出片将分派给其他机器（「互联」中有可用的机器）"),
                    SAY("如需在本机运行，请改用包含 sd.cpp 的程序版本；"
                        "仅使用其他机器时可忽略本条。")};
        }
        // **拆掉 ComfyUI 之后这就不再是 OK 了。** 原来的话是"出图走推理
        // 服务"——那个服务没了，没编进 sd.cpp 就是一张图都出不来。
        return {SAY("出图后端"), Level::FAIL, SAY("程序未包含出图组件，无法出图和出片"),
                SAY("此程序构建时 CHANGJI_SD=OFF。请改用包含该组件的版本，"
                    "或在自行构建时开启 CHANGJI_SD，并按显卡选择 GPU 后端："
                    "NVIDIA 使用 CHANGJI_SD_CUDA，AMD 使用 CHANGJI_SD_HIP，"
                    "Intel 使用 CHANGJI_SD_SYCL，三者通用的是 CHANGJI_SD_VULKAN。"
                    "均未开启时只能使用 CPU 运行，每镜需要数小时。\n"
                    "也可以将渲染交给其他机器：在「设置 ▸ 互联」中添加该机器，"
                    "本机只负责编排和装配。")};
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
                SAY("尚未配置首帧模型，暂不检查此项"), ""};
    }
    if (!m.image_base.empty()) {
        return {SAY("定妆和空景"), Level::OK,
                SAYF("使用基础模型 %1（文生图）", m.image_base), ""};
    }
    if (!config::ModelsConfig::accepts_reference_images(m.image)) {
        // 首帧那一份本来就是基础权重，两件事用同一个是对的。
        return {SAY("定妆和空景"), Level::OK,
                SAYF("与首帧共用 %1（该模型本身即为基础权重）", m.image), ""};
    }
    return {SAY("定妆和空景"), Level::WARN,
            SAYF("未配置 [models].image_base，此步骤将使用首帧的图像编辑模型（%1）"
                 "进行文生图", m.image),
            SAY("三视图和空景图没有可供编辑的参考图，属于文生图，需要基础权重。"
                "使用编辑权重生成时不会报错，但效果无法使用，"
                "而这些图又是后续每一镜的参考图。\n"
                "请在「设置 ▸ 模型文件」中下载「定妆和空景模型（文生图）」。")};
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
        return {SAY("本地模型"), Level::WARN, SAY("尚未配置任何模型，无法出图和出片"),
                SAY("至少需要 [models].image（首帧）"
                    "以及 [models].video 和 video_vae（视频）。\n"
                    "相对路径以 dir 为基准解析；dir 为空时为项目库下的 models/。\n")
                    + SAYF("当前 dir：%1", paths::to_utf8(m.dir_path(ws))),
                // 一个都没配时先指向出图那一组：它是第一个非它不可的
                //（没有首帧就没有画面），下完它界面会接着指下一个。
                "image"};
    }

    if (!missing.empty()) {
        std::string detail = SAYF("已配置 %1 项，其中 %2 项的文件不存在：",
                                  std::to_string(configured.size()),
                                  std::to_string(missing.size()));
        for (const auto& x : missing) detail += "\n  " + x;
        return {SAY("本地模型"), Level::WARN, detail,
                SAY("请检查 [models] 中的文件名以及 dir 所指的目录是否正确。\n"
                    "相对路径以 dir 为基准解析；dir 为空时为项目库下的 models/。\n")
                    + SAYF("当前 dir：%1", paths::to_utf8(m.dir_path(ws))),
                "image"};
    }

    return {SAY("本地模型"), Level::OK,
            SAYF("%1 个模型文件均已就绪（%2）", std::to_string(configured.size()),
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
        return {SAY("权重位置"), Level::OK,
                SAY("未检测到显卡，无法检查此项"), ""};
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
        off.push_back(SAYF("%1：配置值为 \"%2\"，按当前显卡和模型推算应为 \"%3\"", label, set, want));
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
    one(SAY("首帧"), s.models.image_weights,
        s.models.image_weights_for(card, size_gb(s.models.image), unified));

    if (off.empty()) {
        return {SAY("权重位置"), Level::OK,
                SAY("配置与本机推算结果一致"), ""};
    }
    std::string msg;
    for (std::size_t i = 0; i < off.size(); ++i) {
        if (i) msg += SAY("；");
        msg += off[i];
    }
    return {SAY("权重位置"), Level::WARN, msg,
            SAY("通常是配置从其他机器带过来所致。固定值不会随显卡变化，\n"
                "改为 \"smart\"（程序按显卡和模型大小推算）或 \"auto\"\n"
                "（加载时由 sd.cpp 按实际显存自动分配）即可。\n"
                "如确需固定为当前值，可忽略本条。")};
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
    if (missing.empty()) return {SAY("可用功能"), Level::OK, line};
    // **不是 FAIL。** 一台只写文不出片的机器是完全正当的用法
    // （build-coord 那份就是），这一项只负责把话说清楚。
    return {SAY("可用功能"), Level::WARN, line, missing};
}

template <typename F>
Check guarded(const char* name, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return {SAY(name), Level::WARN, SAYF("此项检查自身出错：%1", e.what()),
                SAY("这是 changji 自身的问题，而非环境问题，请反馈此条信息。\n"
                    "其余检查不受影响。")};
    } catch (...) {
        return {SAY(name), Level::WARN, SAY("此项检查抛出未知异常"),
                SAY("这是 changji 自身的问题，而非环境问题，请反馈此条信息。")};
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
                SAYF("%1（此模型未规定画布上限）", got), ""};
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
            SAYF("%1 超出此模型的画布上限 %2 像素（%3 倍）", got,
                 std::to_string(limits.max_pixels), ratio),
            // **建议要能照着做。** 上一版只说"出完再跑 changji --upscale"，
            // 可这条命令还要一个 ESRGAN 权重，而那个文件**不在首次运行
            // 的下载清单里**（setup/catalog.cpp 一个超分条目都没有）——
            // 照着做的人会卡在"--upscale 还要 --upscale-model"这句上，
            // 而它没说该去下哪个文件。
            SAYF("模型超出训练分布运行，结果多为伪影，而不仅是略微模糊。\n"
                 "请将 [video].quality 改回 hd（%1）。如需 2K，请先以标准档出片，"
                 "再单独超分：\n"
                 "  changji --upscale 成片.mp4 输出.mp4 \\\n"
                 "    --upscale-model <RealESRGAN_x4plus.pth 的路径> \\\n"
                 "    --upscale-size %2\n"
                 "该权重需自行下载（Real-ESRGAN 的 v0.1.0 release，67 MB），"
                 "模型清单中未提供。逐帧超分不保证帧间一致，完成后请检查成片。",
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
    // **只有「大模型」那一项真发网络请求**（连 + 读各 8 秒），先在另一条
    // 线程上起跑，其余照旧串行，收尾时按原位置插回去——报告的顺序不变，
    // 最坏情况从"它加上其余全部"压到"它和其余全部取大的那个"。
    //
    // 别把别的项也挪过去：`check_canvas` 读的 `stages::video_limits()` 是
    // 挂在**线程上**的（`ScopedVideoLimits`），换一条线程就读成全局那份。
    // check_llm 只读传进来的 settings，不碰线程上的东西。
    auto llm = std::async(std::launch::async, [&settings] {
        return guarded(SAY_NOOP("大模型"), [&] { return check_llm(settings); });
    });
    Report r;
    r.checks.push_back(guarded(SAY_NOOP("运行时"), [&] { return check_runtime(); }));
    r.checks.push_back(guarded("FFmpeg", [&] { return check_ffmpeg(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("中文字体"), [&] { return check_fonts(settings); }));

    // 「推理服务」那一项 2026-09-10 随 ComfyUI 一起去掉了：出图出片都在
    // 进程内，没有外部服务要连。出图那条由「出图后端」和「本地模型」两项管。
    r.checks.push_back(guarded(SAY_NOOP("配音"), [&] { return check_tts(settings); }));
    const std::size_t llm_at = r.checks.size();
    r.checks.push_back(guarded("ggml ABI", [&] { return check_ggml(); }));
    r.checks.push_back(guarded(SAY_NOOP("内置配音"), [&] { return check_local_tts(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("出图后端"), [&] { return check_sd(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("本地模型"), [&] { return check_models(settings); }));
    r.checks.push_back(
        guarded(SAY_NOOP("定妆和空景"), [&] { return check_image_base(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("显卡"), [&] { return check_gpu(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("权重位置"), [&] { return check_weights(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("出片画布"), [&] { return check_canvas(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("项目目录"), [&] { return check_workspace(settings); }));
    r.checks.push_back(guarded(SAY_NOOP("可用功能"), [&] { return check_produces(settings); }));
    r.checks.insert(r.checks.begin() + static_cast<std::ptrdiff_t>(llm_at), llm.get());
    return r;
}


}  // namespace changji::doctor
