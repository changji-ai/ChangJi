#include "setup/autostart.hpp"

#include <fstream>
#include <system_error>

#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;

namespace changji::setup {
namespace {

/// 登录时系统会扫的那个位置。三个平台各一处，见头文件。
fs::path autostart_dir() {
#if defined(_WIN32)
    // %APPDATA% 就是 Roaming。paths::user_config_dir 在 Windows 上给的
    // 正是 Roaming 下的一个子目录，往上两级回到 Roaming。
    const fs::path roaming = paths::user_config_dir("changji").parent_path();
    return roaming / "Microsoft" / "Windows" / "Start Menu" / "Programs" /
           "Startup";
#elif defined(__APPLE__)
    return paths::home_dir() / "Library" / "LaunchAgents";
#else
    return paths::home_dir() / ".config" / "autostart";
#endif
}

const char* file_name() {
#if defined(_WIN32)
    return "changji.cmd";
#elif defined(__APPLE__)
    return "com.changji.server.plist";
#else
    return "changji.desktop";
#endif
}

/// 命令行里那个可执行文件怎么写。
///
/// **路径里有空格是常态**（macOS 的「Application Support」、Windows 的
/// 「Program Files」），三套模板各有各的转义规矩，这儿只管挑对引号。
std::string quoted(const fs::path& p) {
    return "\"" + paths::to_utf8(p) + "\"";
}

}  // namespace

fs::path autostart_file_path() { return autostart_dir() / file_name(); }

std::string autostart_file_body(const fs::path& exe, int port) {
    const std::string p = std::to_string(port);
#if defined(_WIN32)
    // **`start ""` 那个空标题不能省**：`start "C:\path\changji.exe"` 会把
    // 第一个带引号的参数当成窗口标题，于是什么都没起来，而且不报错。
    return "@echo off\r\nstart \"\" /min " + quoted(exe) + " --port " + p +
           "\r\n";
#elif defined(__APPLE__)
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key><string>com.changji.server</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>" + paths::to_utf8(exe) + "</string>\n"
        "    <string>--port</string>\n"
        "    <string>" + p + "</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        // **不设 KeepAlive。** 设了的话人从界面上把服务停掉，launchd 会
        // 立刻再拉起来——「我明明关了它还在」，而没有任何地方说得清为什么。
        "</dict>\n"
        "</plist>\n";
#else
    // `Comment=` 是**桌面环境自己画出来的那一行小字**，翻。产品名不翻
    // （`场记` 是名字，不是话），所以只有后半截是个句子。
    // 这份 .desktop 是按人生成的，写的是他这会儿的界面语言——真要做得全
    // 该写 `Comment[de]=…` 那一族，可那要求同时拿到十一种语言，而这儿
    // 手上只有当前这一种。
    return
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=changji\n"
        "Comment=" + std::string(SAY_NEVER("场记")) + " · " +
        SAY("AI 电影制作平台") + "\n"
        "Exec=" + paths::to_utf8(exe) + " --port " + p + "\n"
        // 没有图形界面的机器上也照起：这本来就是个服务。
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n";
#endif
}

Autostart autostart_status(int port) {
    Autostart out;
    const fs::path exe = paths::self_exe();
    // 取不到自己的路径就写不出命令行，这一项整个不支持。
    out.supported = !exe.empty();
    if (!out.supported) return out;
    const fs::path f = autostart_file_path();
    out.path = paths::to_utf8(f);
    std::error_code ec;
    out.enabled = fs::is_regular_file(f, ec);
    out.command = paths::to_utf8(exe) + " --port " + std::to_string(port);
    return out;
}

AutostartResult set_autostart(bool on, int port) {
    AutostartResult r;
    r.state = autostart_status(port);
    if (!r.state.supported) {
        r.error = SAY("取不到这个程序自己的路径，写不出开机要跑的命令");
        return r;
    }
    const fs::path f = autostart_file_path();
    std::error_code ec;
    if (!on) {
        fs::remove(f, ec);
        if (ec) {
            r.error = SAYF("删不掉 %1：%2", paths::to_utf8(f), ec.message());
        }
        r.state = autostart_status(port);
        return r;
    }
    fs::create_directories(f.parent_path(), ec);
    if (ec) {
        r.error = SAYF("建不了 %1：%2", paths::to_utf8(f.parent_path()),
                       ec.message());
        return r;
    }
    std::ofstream out(f, std::ios::binary | std::ios::trunc);
    if (!out) {
        r.error = SAYF("写不了 %1", paths::to_utf8(f));
        return r;
    }
    out << autostart_file_body(paths::self_exe(), port);
    out.close();
    if (!out) {
        r.error = SAYF("写 %1 的时候出错了", paths::to_utf8(f));
        return r;
    }
    r.state = autostart_status(port);
    return r;
}

}  // namespace changji::setup
