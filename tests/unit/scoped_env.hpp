#pragma once

// 测试里临时改环境变量，析构时还原。
//
// 单独一个头文件是因为已经有两处要用（配置的环境变量覆盖、`which` 找
// PATH），各写一份迟早会分家——而这种小工具分了家最难发现：
// 两份行为不一样时，出问题的是"另一个文件里的测试莫名其妙地挂了"。
//
// ⚠️ **Windows 上必须走宽字符 API，不能用 `_putenv_s`。**
//
// `_putenv_s` 是窄（ANSI）接口。而 `paths::env()` 返回的是 **UTF-8**，
// 把 UTF-8 字节喂给 ANSI 接口，只要值里有非 ASCII 就会被解坏——
// 这台机器的 PATH 里就有中文目录名。实测：存下 2266 字符的 PATH，
// 原样还原之后只剩 1448 字符。
//
// 后果不是"这条用例挂了"，而是**它之后的每条用例都在一个坏掉的 PATH 上跑**：
// where.exe 找不到、nvidia-smi 找不到，于是 test_readonly 里的显卡检测
// 变成 null 然后报"和 Python 不一致"。**报错的地方离原因隔了两个文件。**
//
// 第一版就是这么写的，上面那段是查出来之后补的。

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

#include "util/paths.hpp"

namespace changji::test {

class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value)
        : name_(std::move(name)) {
        old_ = paths::env(name_.c_str());
        had_ = !old_.empty();
        set(value);
    }
    ~ScopedEnv() { set(had_ ? old_ : std::string()); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set(const std::string& v) {
#ifdef _WIN32
        // 借 fs::path 做 UTF-8 → 宽字符的转换：paths::from_utf8 已经
        // 把 MSVC 上那个"窄字符串按 ANSI 解"的坑处理过了。
        const std::wstring wname = paths::from_utf8(name_).wstring();
        if (v.empty()) {
            ::SetEnvironmentVariableW(wname.c_str(), nullptr);
            return;
        }
        const std::wstring wval = paths::from_utf8(v).wstring();
        ::SetEnvironmentVariableW(wname.c_str(), wval.c_str());
#else
        if (v.empty()) {
            ::unsetenv(name_.c_str());
        } else {
            ::setenv(name_.c_str(), v.c_str(), 1);
        }
#endif
    }
    std::string name_;
    std::string old_;
    bool had_ = false;
};

/// 把**用户级**配置目录指到一个空目录，析构时还原。
///
/// `config::load_settings(项目目录)` 先读 `user_config_path()`（用户级），
/// 再叠上项目里的 `changji.toml`。所以任何走 `load_settings` 的用例，
/// 默认都在读**这台开发机上真实的那份配置**。
///
/// 后果是"在干净机器上过、在真在用的机器上挂"，而且报错离原因很远：
///   * 2026-09-10 服务器上配了模型，于是「没写的项保持空」常年红一个，
///     红着红着就没人看了，真回归也照样漏过去；
///   * 2026-09-13 服务器上 `[models].video` 是 MiniMax-H3，而 H3 只出
///     24fps，于是写回那条「fps 改成 30 了吗」在服务器上挂、在 Windows
///     上过——两台机器的差别根本不在被测代码里。
///
/// ⚠️ **三个平台换的不是同一个变量**，写漏一个就等于这道隔离在那台机器上
/// 不存在（上面第二条正是这么发生的）：
///   * Windows：`LOCALAPPDATA`
///   * macOS：**`HOME`**——`user_config_dir` 的 `__APPLE__` 分支走的是
///     `$HOME/Library/Application Support/changji`，一个字都不看
///     `XDG_CONFIG_HOME`；
///   * 其它：`XDG_CONFIG_HOME` **和 `XDG_DATA_HOME`**。
///
/// ⚠️ **数据目录和配置目录在 Linux 上不是同一个变量。** `user_config_dir` 读
/// `XDG_CONFIG_HOME`，而 `user_data_dir` 读 **`XDG_DATA_HOME`**（退回
/// `~/.local/share`）——只换前者的话，任何往数据目录写东西的用例都会写到那台
/// 机器上真实的 `~/.local/share/changji/` 里。2026-09-18 提示词日志
/// （`llm/call_log.cpp`）落的就是数据目录，跑一次单元测试就往人家家里写一堆
/// 文件。Windows 和 macOS 上两者本来就是同一个根，所以只有这一支要补。
///
/// ⚠️ **隔离目录用纯 ASCII 名。** 把带中文的路径塞进 `LOCALAPPDATA`，
/// Windows 上会抛 "No mapping for the Unicode character exists in the
/// target multi-byte code page"——环境变量那条路上有一步窄字符转换。
class ScopedUserConfigDir {
public:
    explicit ScopedUserConfigDir(const std::string& tag = "default")
        : dir_(std::filesystem::temp_directory_path() /
               ("changji_empty_cfg_" + tag)) {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
        const std::string v = paths::to_utf8(dir_);
#ifdef _WIN32
        guards_.push_back(std::make_unique<ScopedEnv>("LOCALAPPDATA", v));
#elif defined(__APPLE__)
        guards_.push_back(std::make_unique<ScopedEnv>("HOME", v));
#else
        guards_.push_back(std::make_unique<ScopedEnv>("XDG_CONFIG_HOME", v));
        guards_.push_back(std::make_unique<ScopedEnv>("XDG_DATA_HOME", v));
#endif
    }

    ~ScopedUserConfigDir() {
        guards_.clear();  // 先还原环境变量，再删目录
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    ScopedUserConfigDir(const ScopedUserConfigDir&) = delete;
    ScopedUserConfigDir& operator=(const ScopedUserConfigDir&) = delete;

    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path dir_;
    // **一个平台可能要换好几个变量**（Linux 上配置和数据分两个），所以存一列
    // 而不是一个。析构按加进来的顺序还原，彼此不相干。
    std::vector<std::unique_ptr<ScopedEnv>> guards_;
};

}  // namespace changji::test
