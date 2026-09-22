#pragma once

// 开机自动起服务。
//
// 用户 2026-09-17：「增加在设置中随系统启动开关」。
//
// **三个平台都只是写一个文件**，不碰任何平台 API：
//
//   macOS    ~/Library/LaunchAgents/com.changji.server.plist
//   Linux    ~/.config/autostart/changji.desktop
//   Windows  %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\changji.cmd
//
// 刻意不用 launchctl / 注册表 / systemctl：
//
//   · **写文件这件事三处是同一个形状**，一份代码就够，而那三套 API 是三份，
//     其中两份还要链平台库；
//   · 登录时这三个位置都会被系统自己扫一遍，写完下次登录就生效——不需要
//     "现在就注册"这一步，而那一步恰恰是三套 API 里最容易出错的部分
//     （launchctl 的 bootstrap/bootout 在不同 macOS 版本上行为不一样）；
//   · 关掉就是把文件删了，删不干净的残留只有一个文件，人自己也找得到。
//
// 代价写在这儿：**打开开关之后这一次不会自动起**，要下次登录才生效。界面上
// 那句话要说出来，不然人会以为开关没用。

#include <filesystem>
#include <string>

namespace changji::setup {

/// 这台机器上开机自启这件事的现状。
struct Autostart {
    /// 这个平台支持不支持（认得出三种之一就算支持）。
    bool supported = false;
    /// 现在开着没有（那个文件在不在）。
    bool enabled = false;
    /// 写的是哪个文件。界面上要摆出来——开关失灵时人得知道去哪儿看。
    std::string path;
    /// 开着的话，开机会起的是哪一条命令。
    std::string command;
};

/// 读一遍现状。不改任何东西。
Autostart autostart_status(int port);

/// 开或关。回改完之后的现状；`error` 非空表示没改成。
struct AutostartResult {
    Autostart state;
    std::string error;
};
AutostartResult set_autostart(bool on, int port);

/// 那个文件长什么样。**导出只为了能测**——三套模板各自的语法错了不会报错，
/// 只是开机不起来，而那要重启一次才发现。
std::string autostart_file_body(const std::filesystem::path& exe, int port);

/// 写到哪儿。同上，导出只为了能测。
std::filesystem::path autostart_file_path();

}  // namespace changji::setup
