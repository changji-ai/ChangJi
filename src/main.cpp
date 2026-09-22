// changji —— AI 电影生产流水线，C++ 后端。
//
// 阶段 0：骨架。只有 /api/health、/api/doctor 和一个 WebSocket 端点，
// 目的是把工具链和跨平台构建先跑通，别等写了两万行才发现某个依赖
// 在树莓派上编不过。

#include "util/say.hpp"
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>

#ifdef _MSC_VER
#include <intrin.h>   // __cpuid / __cpuidex，给上面 cpu_ok 用
#endif

#include "infer/sd_upscale.hpp"
#include "infer/worker_server.hpp"
#include "config/settings.hpp"
#include "infer/llama_tts.hpp"
#include "infer/scheduler.hpp"
#include "doctor/doctor.hpp"
#include "http/server.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace {

/// 解析 --port 的参数。
///
/// **不用 std::atoi。** 它解不出来就返回 0，而 0 在 bind 里是合法的——
/// 意思是"操作系统随便挑一个空闲端口"。于是 `--port abc` 会静默地
/// 起在一个随机端口上，而用户以为它在自己敲的那个上面。
///
/// 更常见的是打错一个字符：`--port 8O80`（字母 O）被 atoi 解成 **8**，
/// 服务起在 8 端口，用户连 8080 连不上，而程序一声没吭。
/// 这一类"静默地理解成别的意思"是最难查的，因为症状离原因很远。
int parse_port(const std::string& raw) {
    const bool digits =
        !raw.empty() && std::all_of(raw.begin(), raw.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    long v = digits ? std::strtol(raw.c_str(), nullptr, 10) : -1;
    if (!digits || v < 1 || v > 65535) {
        std::cerr << SAYF("--port 要一个 1 到 65535 的整数，收到的是「%1」",
                          raw)
                  << "\n";
        std::exit(2);
    }
    return static_cast<int>(v);
}

// 构建脚本会用 -DCHANGJI_VERSION="…" 覆盖它。留个兜底是为了
// 不经过 CMake 的编法（临时 g++ 一下）也编得过。
#ifndef CHANGJI_VERSION
#define CHANGJI_VERSION "dev"
#endif

/// 一个 double 印成流的默认样子（六位**有效数字**）。
///
/// ⚠️ **别用 `std::to_string(double)`**：它固定印六位小数，2.5 会变成
/// 「2.500000」。2026-09-22 在显卡那一行栽过一次（「16.000000 GB」）。
std::string num_str(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

void print_usage() {
    // **整块一个键。** 帮助屏是一整页排好版的东西：选项列宽、缩进、
    // 哪几行是一段，全是一起看的。切成四十个键的话，翻译的人看不见版面，
    // 而对齐的空格数会在每一种语言里各错各的。
    std::cout << SAY(
        "用法: changji [选项]\n"
        "\n"
        "  --port <n>            监听端口，默认 8080\n"
        "  --host <addr>         监听地址，默认 127.0.0.1（只有本机连得上）\n"
        "                        要让局域网连进来才填 0.0.0.0——这套接口没有\n"
        "                        鉴权，连上就能读项目、改分镜、起流水线\n"
        "  --project <目录>      默认打开的项目。这个项目里的 changji.toml\n"
        "                        也会一并读进来，盖过全局配置——不给就只有全局的\n"
        "  --doctor              跑一遍环境体检然后退出\n"
        // **这四个不是工作进程专用的。** 它们原来排在下面那个「当工作进程
        // 跑」的小标题底下，而解析根本不看 --worker——照那个排版读，想生成
        // 一份配置模板的人会以为 --init-config 只在多卡那条路上有用。
        "  --init-config         生成一份带注释的配置模板然后退出\n"
        "  --force               配合 --init-config：已有配置也照样覆盖\n"
        "  --help                显示这段话\n"
        "  --version             打印版本号然后退出\n"
        "\n"
        "  当工作进程跑（多卡时一张卡起一个，见方案「多卡和多机」）：\n"
        "  --worker              以工作进程模式起，只算不发接口\n"
        "  --gpu <n>             绑第几张卡，默认 0\n"
        "\n"
        // **超分这一族原来一个字都没有。** 三个选项解析里都有，体检里「出片
        // 画布」那条还专门教人敲 `changji --upscale …`——照着敲完想
        // `--help` 核一眼参数顺序，翻遍这一页找不到。
        "  把一段视频超分然后退出（体检里那句「要 2K 就出完再跑」指的就是它）：\n"
        "  --upscale <入> <出>   要超分的视频，和写到哪儿\n"
        "  --upscale-model <文件>\n"
        "                        ESRGAN 权重：Real-ESRGAN v0.1.0 release 里那份\n"
        "                        RealESRGAN_x4plus.pth（67 MB）。模型清单里没有，\n"
        "                        要自己下\n"
        "  --upscale-size <WxH>  目标尺寸，形如 1440x2560\n"
        "                        三个一起给才算数，少一个会当场说缺哪个\n"
        "\n"
        "  用进程内配音念一句然后退出（不用起服务，也不用建项目）：\n"
        "  --say <文本>          要念的话\n"
        "  --out <文件>          写到哪儿，默认 say.wav\n"
        "  --voice <音频>        参考音色，一段人声片段（wav/mp3/flac）。\n"
        "                        不给就用模型自带的\n"
        "  --tts-model <文件>    临时指定骨干，盖过配置里的 [models].tts\n"
        "  --tts-decoder <文件>  临时指定解码器，盖过 [models].tts_decoder\n"
        "\n"
        "配置优先级：环境变量 > 项目目录的 changji.toml > 用户全局配置 > 内置默认值\n");
}


/// `--say`：用进程内配音念一句，写成 wav。
///
/// **阶段 9 的实机判据就是这一条。** 那条合成路径代码写完很久了，
/// 但机器上一直没有 Qwen3-TTS 的权重，所以一次都没执行过。
/// 权重到位之后，这一条命令是最短的验证路径——不用起服务、不用建项目、
/// 不用 ffmpeg，出不出得了声一句话就知道。
///
/// 报错要说清楚是**哪一步**断的：没编进来（改构建）、没配路径（改配置）、
/// 载不起来（文件不对或显存不够）、合成失败（模型或参数）。
/// 只说一句"配音失败"的话，用户下一步该干什么全靠猜。
int run_say(const changji::config::Settings& settings, const std::string& text,
            const std::string& voice, const std::string& out,
            const std::string& model_override,
            const std::string& decoder_override) {
    using namespace changji;

    if (!infer::llama_tts_available()) {
        std::cerr << SAY("这个二进制没编进程内配音。\n"
                         "  用 -DCHANGJI_LLAMA=ON 重新配置构建。\n");
        return 1;
    }

    const auto ws = settings.workspace_path();
    const auto backbone =
        model_override.empty() ? settings.models.resolve(settings.models.tts, ws)
                               : paths::from_utf8(model_override);
    const auto decoder =
        decoder_override.empty()
            ? settings.models.resolve(settings.models.tts_decoder, ws)
            : paths::from_utf8(decoder_override);
    if (backbone.empty() || decoder.empty()) {
        std::cerr << SAY(
            "配置里缺模型路径。要这两项：\n"
            "  [models].tts          骨干（qwen-talker-*.gguf）\n"
            "  [models].tts_decoder  解码器（qwen-tokenizer-12hz-*.gguf）\n"
            "两份都没有的话跑一遍 download_tts_gguf.ps1。\n"
            "也可以不改配置，直接用 --tts-model / --tts-decoder 指过来。\n");
        return 1;
    }

    std::cout << SAYF("骨干   %1\n解码器 %2\n", paths::to_utf8(backbone),
                      paths::to_utf8(decoder))
              << SAY("载入中（1 GB 出头，第一次会慢）…\n");

    std::string why;
    auto engine = infer::LlamaTts::load(backbone, decoder, /*use_gpu=*/true, why);
    if (!engine) {
        std::cerr << SAYF("载不起来：%1\n", why);
        return 1;
    }
    std::cout << SAYF("载好了，采样率 %1 Hz\n",
                      std::to_string(engine->sample_rate()));

    infer::LlamaTtsRequest req;
    req.text = text;
    req.out = paths::from_utf8(out);
    if (!voice.empty()) req.speaker_ref = paths::from_utf8(voice);

    double seconds = 0;
    if (!engine->synthesize(req, seconds, why)) {
        std::cerr << SAYF("合成失败：%1\n", why);
        return 1;
    }

    std::cout << SAYF("出声了：%1，%2 秒\n", out, num_str(seconds));
    // 这一条是给判据用的：静音检测那套判据（见 stages/tts_backends.hpp）
    // 认为 1.05 秒以下多半是空音频。这里只提醒，不当失败——
    // 念一个字本来就可能不到一秒。
    if (seconds < 1.05) {
        std::cout << SAY(
            "  ⚠️ 不到 1.05 秒。流水线里的静音检测会把这种当空音频拦下来。\n"
            "     念一句长一点的再看看是不是真的出声了。\n");
    }
    return 0;
}

/// UTF-8 字符串占多少个终端列。
///
/// 不能按字节数算：一个汉字是 3 个字节但只占 2 列。也不能按字符数算：
/// 汉字占 2 列而 ASCII 占 1 列。
///
/// ⚠️ **原来是「非 ASCII 一律按 2 列」**——界面全是中文的时候这没问题，
/// 2026-09-22 体检那一栏翻成印地语和阿拉伯语之后当场歪了：天城文和阿拉伯
/// 文每个字母只占 1 列，而元音符号（मा 里那个 ा、ि 那类）根本不占列，
/// 按 2 列算出来的宽度能差一倍，右边那一列全撞在一起。
///
/// 现在按码点分三档：**组合记号 0 列、东亚宽和全角 2 列、其余 1 列**。
/// 这是终端的老规矩（wcwidth 那一套），够这张表用。
size_t display_width(const std::string& s) {
    const auto zero_width = [](unsigned int cp) {
        return (cp >= 0x0300 && cp <= 0x036F) ||    // 拉丁组合符
               (cp >= 0x0483 && cp <= 0x0489) ||    // 西里尔组合符
               (cp >= 0x0591 && cp <= 0x05BD) ||    // 希伯来
               (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||  // 阿拉伯
               (cp >= 0x06D6 && cp <= 0x06ED) ||
               (cp >= 0x0900 && cp <= 0x0903) ||    // 天城文
               (cp >= 0x093A && cp <= 0x094F && cp != 0x093D) ||
               (cp >= 0x0951 && cp <= 0x0957) ||
               (cp >= 0x0962 && cp <= 0x0963) ||
               cp == 0x200B || cp == 0x200C || cp == 0x200D ||    // 零宽
               cp == 0x200E || cp == 0x200F || cp == 0xFEFF;
    };
    const auto wide = [](unsigned int cp) {
        return (cp >= 0x1100 && cp <= 0x115F) ||    // 谚文字母
               (cp >= 0x2E80 && cp <= 0x303E) ||    // 部首、中日韩标点
               (cp >= 0x3041 && cp <= 0x33FF) ||    // 假名、谚文、方块
               (cp >= 0x3400 && cp <= 0x4DBF) ||    // 扩展 A
               (cp >= 0x4E00 && cp <= 0x9FFF) ||    // 基本区
               (cp >= 0xA000 && cp <= 0xA4CF) ||    // 彝文
               (cp >= 0xAC00 && cp <= 0xD7A3) ||    // 谚文音节
               (cp >= 0xF900 && cp <= 0xFAFF) ||    // 兼容表意
               (cp >= 0xFE30 && cp <= 0xFE6F) ||    // 竖排、小写变体
               (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角
               (cp >= 0xFFE0 && cp <= 0xFFE6) ||
               (cp >= 0x1F300 && cp <= 0x1FAFF);    // 绘文字
    };
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned int cp = c;
        size_t len = 1;
        if ((c & 0x80) == 0) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            len = 4;
        }
        for (size_t k = 1; k < len && i + k < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        i += len;
        if (zero_width(cp)) continue;
        w += wide(cp) ? 2 : 1;
    }
    return w;
}

/// 命令行跑体检。首次部署时最常用的一条命令，
/// 不用先起服务再开浏览器就能知道缺什么。
int run_doctor(const changji::config::Settings& settings) {
    auto report = changji::doctor::run_checks(settings);

    size_t width = 0;
    for (const auto& c : report.checks) {
        width = std::max(width, display_width(c.name));
    }

    for (const auto& c : report.checks) {
        const char* symbol = c.level == changji::doctor::Level::OK     ? "✓"
                             : c.level == changji::doctor::Level::WARN ? "!"
                                                                       : "✗";
        std::cout << "  " << symbol << "  " << c.name;
        for (size_t i = display_width(c.name); i < width + 2; ++i) std::cout << ' ';
        // detail 也可能是多行的（"出图后端"那一项带着 sd.cpp 的
        // System Info）。不缩进的话它会顶格贴在报告中间，把对齐冲掉。
        std::cout << changji::text::indent_rest(c.detail, "        ") << "\n";
        if (!c.fix.empty()) {
            std::cout << "        "
                      << changji::text::indent_rest(c.fix, "        ") << "\n";
        }
    }
    std::cout << "\n";

    size_t failed = 0, warned = 0;
    for (const auto& c : report.checks) {
        if (c.level == changji::doctor::Level::FAIL) ++failed;
        if (c.level == changji::doctor::Level::WARN) ++warned;
    }
    if (failed) {
        std::cout << SAYN("有 %n 项必须先解决才能出片。\n", static_cast<long long>(failed));
    } else if (warned) {
        std::cout << SAYN("可以跑，但有 %n 项建议处理。\n", static_cast<long long>(warned));
    } else {
        std::cout << SAY("一切就绪。\n");
    }
    return failed ? 1 : 0;
}

}  // namespace

namespace {

/// 真正的入口。main 只负责把异常兜住。
int run(int argc, char** argv);

}  // namespace

namespace {

/// 这台 CPU 撑不撑得起这个二进制。撑不住就说清楚，别让它闷声崩掉。
///
/// **发布版按 AVX2 基线编**（CI 里 GGML_NATIVE=OFF，也就是 2013 年
/// Haswell 起的那条线）。CPU 没有 AVX2 的话，跑到用了那些指令的地方会
/// 收到 SIGILL——表现是**进程直接没了，一个字都不打**，dmesg 里才有一行
/// `trap invalid opcode`，而普通用户根本不会去看 dmesg。
///
/// 2026-09-11 在一台 QEMU Virtual CPU 2.5+ 的机器上实测到：服务起得来、
/// 首页也发得出，一打开设置页（体检那条路）就没了。看着像"网页把服务
/// 搞崩了"，其实是 CPU 不支持。
///
/// 检查本身不能用 AVX2——`__builtin_cpu_supports` 走的是 CPUID，安全。
bool cpu_ok(std::string& missing) {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
    // **MSVC 没有 __builtin_cpu_supports，得自己问 CPUID。**
    // windows-x64 那个包正是 MSVC 编的，而 Windows 上老 CPU 最常见——
    // 少了这一支，最需要这个提示的平台反而没有。
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0);
    const int max_leaf = regs[0];
    if (max_leaf >= 1) {
        __cpuid(regs, 1);
        // leaf 1, ECX bit 12 = FMA
        if ((regs[2] & (1 << 12)) == 0) { missing = "FMA"; return false; }
    }
    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        // leaf 7 子叶 0, EBX bit 5 = AVX2
        if ((regs[1] & (1 << 5)) == 0) { missing = "AVX2"; return false; }
    } else {
        missing = "AVX2";   // 连 leaf 7 都没有的 CPU，肯定没 AVX2
        return false;
    }
#elif defined(__GNUC__) || defined(__clang__)
    if (!__builtin_cpu_supports("avx2")) { missing = "AVX2"; return false; }
    if (!__builtin_cpu_supports("fma"))  { missing = "FMA";  return false; }
#endif
#endif
    (void)missing;
    return true;
}

}  // namespace

/// 引擎这一趟说哪国话。
///
/// **桌面端自己设**（它知道界面选的是哪一种，见 `desktop/main.cpp`），
/// 可命令行这条路原来一次都没设过——`changji --doctor` 在任何机器上都说
/// 中文，而体检报告那几百句早就翻好了，只是没人打开开关。
///
/// 顺序：`CHANGJI_LANG` 说了算，没有就跟系统的 `LC_ALL` / `LC_MESSAGES` /
/// `LANG`。**点号后面那截要去掉**（`de_DE.UTF-8` → `de_DE`），那是字符集
/// 不是语言；`say()` 自己还会从 `de_DE` 退一步试 `de`。
void speak_as_env_says() {
    for (const char* key : {"CHANGJI_LANG", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(key);
        if (v == nullptr || *v == '\0') continue;
        std::string lang = v;
        // `C` 和 `POSIX` 不是语言，是"别本地化"。
        if (lang == "C" || lang == "POSIX") return;
        const auto dot = lang.find('.');
        if (dot != std::string::npos) lang.erase(dot);
        const auto at = lang.find('@');
        if (at != std::string::npos) lang.erase(at);
        changji::i18n::speak(lang);
        return;
    }
}

int main(int argc, char** argv) {
    speak_as_env_says();

    // **--version 和 --help 要放行。**
    //
    // 那两条是"任何情况下都该答得上来"的。CPU 撑不住的机器上也一样——
    // 报故障时第一句话就是版本号，而"跑不了"和"跑不了的是哪一版"是两件事。
    // 2026-09-11 实测发现的：新包在没有 AVX2 的机器上 `--version` 打印的是
    // 那段 CPU 说明，版本号反而问不到了。
    bool asking_meta = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version" || a == "--help" || a == "-h") asking_meta = true;
    }

    // **再看 CPU 撑不撑得住。** 见 cpu_ok。
    if (std::string missing; !asking_meta && !cpu_ok(missing)) {
        std::cerr
            << SAYF("这台机器的 CPU 不支持 %1，跑不了这个版本。\n"
                    "发布版是按 AVX2 基线编的（2013 年的 Haswell 之后都有）。\n"
                    "常见于很老的机器、或者虚拟机里选了通用 CPU 型号——\n"
                    "后者改一下虚拟机的 CPU 型号（host-passthrough 之类）就行。\n"
                    "要在这台上跑，只能自己编一份：\n", missing)
            << "  cmake -S cpp -B build -DGGML_NATIVE=OFF -DGGML_AVX2=OFF\n";
        return 2;
    }

    // 顶层兜异常。没有它的话，任何漏出来的异常会走 std::terminate 到 abort，
    // 在 Windows 上表现为进程以 0xC0000409 消失、一个字都不打印，
    // 而那个错误码字面意思是"栈缓冲区溢出"，会把人往完全错误的方向带。
    // （这是实测踩到的：--doctor 在 MSVC 下就这样静默死掉。）
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << SAYF("出错了：%1\n", e.what());
        return 1;
    } catch (...) {
        std::cerr << SAY("出错了：未知异常\n");
        return 1;
    }
}

namespace {

int run(int argc, char** argv) {
    changji::http::Options opts;
    bool want_doctor = false;
    bool want_worker = false;
    int worker_gpu = 0;
    bool want_force = false;
    bool want_init = false;

    // Windows 上 argv 是按 ANSI 代码页编的，中文参数直接用是乱码。
    // 现在的选项都是 ASCII 所以碰不上，但将来加 --config <路径> 就会踩——
    // 而那时候的表现是"配置文件找不到"，看不出是参数被编码毁了。
    // 当年的对拍程序就是这么栽的一次（那个目标随 Python 引擎一起删了）。
    const std::vector<std::string> av = changji::paths::utf8_args(argc, argv);

    // --say 那一组。**这是阶段 9 唯一的实机判据**：进程内配音那条路
    // 有没有真的能出声，不跑一次是不知道的（代码写完了但一直没有权重）。
    // 做成命令行而不是接口，是因为它要在"整条流水线还跑不起来"的时候
    // 就能单独验——出一章要模型、要 ffmpeg，那些是另外的坎。
    std::string say_text, say_voice, say_model, say_decoder;
    // 超分那条命令行的参数。见 sd_upscale.hpp 里为什么要有它。
    std::string up_in, up_out, up_model;
    int up_w = 0, up_h = 0;
    std::string say_out = "say.wav";

    // 默认打开的项目。**它同时决定读不读那个项目里的 changji.toml**——
    // 不给就只有全局配置，和以前一样。
    std::optional<std::filesystem::path> project_dir;

    for (std::size_t i = 1; i < av.size(); ++i) {
        const std::string& a = av[i];
        auto next = [&](const std::string& what) -> std::string {
            if (i + 1 >= av.size()) {
                std::cerr << SAYF("%1 后面要跟%2\n", a, what);
                std::exit(2);
            }
            return av[++i];
        };
        if (a == "--help" || a == "-h") { print_usage(); return 0; }
        // 装好之后手上这个二进制是哪一版，只有它说得出来。
        // 报故障时第一句话就是这个。
        else if (a == "--version") { std::cout << CHANGJI_VERSION << "\n"; return 0; }
        else if (a == "--port") opts.port = parse_port(next(SAY("端口号")));
        else if (a == "--host") opts.host = next(SAY("监听地址"));
        else if (a == "--worker") want_worker = true;
        else if (a == "--gpu") worker_gpu = std::atoi(next(SAY("显卡序号")).c_str());
        else if (a == "--doctor") want_doctor = true;
        else if (a == "--project")
            project_dir = changji::paths::from_utf8(next(SAY("项目目录")));
        else if (a == "--say") say_text = next(SAY("要念的话"));
        else if (a == "--out") say_out = next(SAY("输出文件名"));
        else if (a == "--voice") say_voice = next(SAY("参考音色文件"));
        // **两个临时覆盖。** 没有它们的话，想拿 --say 试一份刚下好的模型
        // 就得先去改配置文件——而"改了配置去试，试完再改回来"这件事
        // 本身就容易忘记改回来。
        else if (a == "--tts-model") say_model = next(SAY("骨干模型文件"));
        else if (a == "--tts-decoder") say_decoder = next(SAY("解码器文件"));
        else if (a == "--upscale") {
            up_in = next(SAY("要超分的视频"));
            up_out = next(SAY("输出文件"));
        }
        else if (a == "--upscale-model") up_model = next(SAY("ESRGAN 权重文件"));
        else if (a == "--upscale-size") {
            const std::string wh = next(SAY("目标尺寸，形如 1088x1920"));
            const auto x = wh.find('x');
            if (x == std::string::npos) {
                std::cerr << SAY("--upscale-size 要写成 1088x1920 这样\n");
                return 2;
            }
            up_w = std::atoi(wh.substr(0, x).c_str());
            up_h = std::atoi(wh.substr(x + 1).c_str());
        }
        else if (a == "--init-config") want_init = true;
        else if (a == "--force") want_force = true;
        else {
            std::cerr << SAY("不认识的选项：") << a << "\n\n";
            print_usage();
            return 2;
        }
    }

    if (want_init) {
        // **已经有配置就不覆盖。**
        //
        // write_default_config() 是无条件截断写的。一个已经配好模型路径、
        // 工作区、后端地址的用户，只是想看看模板长什么样而敲了 --init-config，
        // 结果那些全没了——而且没有任何提示，因为命令"成功"了。
        //
        // 这不是假想：写这段之前我自己就是拿它当"看一眼默认配置在哪"的
        // 探针用的，一敲就把人家的配置盖了。
        const auto existing = changji::config::user_config_path();
        std::error_code ec;
        if (!want_force && std::filesystem::exists(existing, ec)) {
            std::cerr << SAYF("配置文件已经在了：%1\n"
                              "  不覆盖。真要重新生成一份就加 --force"
                              "（原来那份会没）。\n",
                              changji::paths::to_utf8(existing));
            return 1;
        }
        try {
            auto p = changji::config::write_default_config();
            std::cout << SAYF("配置模板已写入 %1\n", changji::paths::to_utf8(p));
            return 0;
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }

    changji::config::Settings settings;
    try {
        settings = changji::config::load_settings(project_dir);
    } catch (const std::exception& e) {
        // 配置解析失败是致命的，而且消息里带着是哪个文件第几行。
        // 用默认值硬撑会让用户以为配置生效了。
        std::cerr << e.what() << "\n";
        return 1;
    }

    auto errs = settings.validate();
    if (!errs.empty()) {
        std::cerr << SAY("配置有问题：\n");
        for (const auto& e : errs) std::cerr << "  - " << e << "\n";
        return 1;
    }

    // 超分：把出好的成片逐帧过 ESRGAN 再压到目标尺寸。
    // **它是"不换硬件把分辨率拉上去"的那条路**——H3 在 32 GB 的卡上
    // 只出得了 960×544。风险是逐帧超分没有帧间一致性，细密纹理会闪，
    // 所以做完一定要看成片，别只看单帧。
    if (!up_in.empty()) {
        if (up_model.empty()) {
            std::cerr << SAY("--upscale 还要 --upscale-model 指一个 ESRGAN 权重\n");
            return 2;
        }
        if (up_w <= 0 || up_h <= 0) {
            std::cerr << SAY("--upscale 还要 --upscale-size，形如 1088x1920\n");
            return 2;
        }
        try {
            changji::infer::upscale_video(changji::paths::from_utf8(up_in),
                                          changji::paths::from_utf8(up_out),
                                          changji::paths::from_utf8(up_model),
                                          up_w, up_h, settings.assembly);
            std::cout << SAYF("超分好了：%1\n", up_out);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }

    if (!say_text.empty()) {
        return run_say(settings, say_text, say_voice, say_out, say_model,
                       say_decoder);
    }

    // **协调者也绑卡。** 不绑的话 llama.cpp 默认把配音模型摊到所有卡上，
    // 实测比只用一张慢 39%（PCIe 上的通信开销比省下的算力多），
    // 而且会和每一个 worker 抢显存。配 [workers].gpu 就绑。
    if (!want_worker && settings.workers.gpu >= 0) {
        changji::paths::set_env("CUDA_VISIBLE_DEVICES",
                                std::to_string(settings.workers.gpu));
    }

    if (want_worker) {

        // 工作进程：只算，不发接口、不碰项目文件。

        // 见方案「多卡和多机怎么用起来」。

        changji::infer::WorkerOptions wo;

        wo.port = opts.port;

        wo.host = opts.host;

        wo.gpu = worker_gpu;

        if (!changji::infer::run_worker(settings, wo)) return 1;

        return 0;

    }


    if (want_doctor) return run_doctor(settings);

    changji::http::run(settings, opts);

    // **退出之前把显存里的模型放掉。**
    //
    // 2026-09-19 实撞：本机跑着 Qwen3-4B 的时候 pkill 一下，进程在**静态
    // 析构**里当场 abort——
    //     ggml_abort ← ggml_metal_rsets_free ← ggml_metal_device_free
    //     ← vector<unique_ptr<ggml_metal_device>>::~vector ← __cxa_finalize_ranges
    // ggml 的 Metal 设备表和我们这边握着模型的那几个静态变量在不同的编译
    // 单元里，谁先析构是**未指定的**。设备先没了、模型还握着资源集，
    // ggml 就 abort。落一份崩溃报告，退出码非零。
    //
    // 数据不会丢（story.json 早落盘了），但「正常退出留一份 crash report」
    // 会把下一个查问题的人带偏，而且脚本按退出码判断是否成功时会误判。
    //
    // 在 main 里显式卸，析构顺序这件事就不用赌了：这会儿 ggml 的静态变量
    // 都还活着。借着的槽不强卸（evict_all 的规矩），那种情况下还是可能撞上
    // ——但那要求退出时正好有一次生成没跑完，而 run() 是等服务停了才返回的。
    changji::infer::scheduler().evict_all();
    return 0;
}

}  // namespace
