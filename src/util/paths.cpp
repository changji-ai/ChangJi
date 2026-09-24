#include "util/paths.hpp"

#include <algorithm>
#include <cstdlib>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#include <windows.h>
// CommandLineToArgvW 在这里，不在 windows.h
#include <shellapi.h>
#endif

#include <vector>

namespace changji::paths {

bool same_dir(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    const std::filesystem::path pa = std::filesystem::weakly_canonical(from_utf8(a), ec);
    if (ec) return false;
    const std::filesystem::path pb = std::filesystem::weakly_canonical(from_utf8(b), ec);
    if (ec) return false;
    return pa == pb;
}

std::string dir_key(const std::string& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::weakly_canonical(from_utf8(dir), ec);
    if (ec) return dir;
    return to_utf8(p);
}

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

/// 宽字符转 UTF-8。
///
/// Windows 上不能直接用 getenv：它走 ANSI 代码页，中文路径会被
/// 转成问号。必须 GetEnvironmentVariableW 拿宽字符再自己转 UTF-8。
std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                  static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace
#endif

std::vector<std::string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
    // argv 是按 ANSI 代码页编的。中文路径从命令行传进来，直接当 UTF-8 用
    // 会得到一串乱码字节——拼进 URL 是 400，交给 fs::path 是异常。
    // 而这一切都不报错，只表现为"这个项目打不开"。
    int wide_argc = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wide_argc);
    if (wide != nullptr) {
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(wide_argc));
        for (int i = 0; i < wide_argc; ++i) out.push_back(to_utf8(std::wstring(wide[i])));
        ::LocalFree(wide);
        return out;
    }
    // 取不到宽版本时退回 argv。乱码总比一个参数都没有强。
#endif
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

std::filesystem::path from_utf8(const std::string& s) {
    // C++17 的 u8path 就是为这件事存在的。C++20 起它被标记为弃用。
    //
    // ⚠️ **那条弃用对我们是生效的。** 上一版这儿写的是「本项目全局停在
    // C++17」——那句话只说对了一半：CMakeLists 把 `CMAKE_CXX_STANDARD`
    // 定在 17 是为了**不传染给上游**（llama.cpp / ggml 声明的是下限不是
    // 上限，父作用域给 20 会让它们整个按 20 编，然后在 MSVC 上撞
    // char8_t），而我们自己那个目标下一行就是
    // `target_compile_features(changji PRIVATE cxx_std_20)`。也就是说
    // 这个文件按 C++20 编，那条 -Wdeprecated-declarations 每次构建都在。
    //
    // **还是留着它。** 替代写法要先把 UTF-8 转成宽字符再构造 path
    // （直接 `path(std::string)` 在 Windows 上按 ANSI 代码页解释，
    // 中文项目名当场乱码——这正是 u8path 在这儿的全部理由），而这个文件
    // 里只有反方向的 `to_utf8(wstring)`，正方向得新写一个
    // MultiByteToWideChar。为一条警告去动整套路径的入口，不划算。
    //
    // 所以就地按下这条警告，别让它在 -Wall -Wextra -Wpedantic 里一直响
    // ——理由同 CMakeLists 给 zlib 加 `-w` 那段：「几十条 C4996 会淹掉
    // 真正的警告」。按下的范围只有这一行。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    return std::filesystem::u8path(s);
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

std::string to_utf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return to_utf8(p.wstring());
#else
    return p.string();
#endif
}

void set_env(const std::string& name, const std::string& value) {
#if defined(_WIN32)
    // **用宽字符版**：值里可能有中文路径，窄版在简体 Windows 上按本地
    // 代码页转，非 ASCII 会烂掉。这个项目在窄/宽这件事上栽过好几次。
    ::_wputenv_s(from_utf8(name).wstring().c_str(),
                 from_utf8(value).wstring().c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::string env(const char* name) {
#ifdef _WIN32
    std::wstring wname;
    for (const char* p = name; *p; ++p) wname.push_back(static_cast<wchar_t>(*p));
    DWORD need = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0) return {};
    std::vector<wchar_t> buf(need);
    DWORD got = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), need);
    if (got == 0 || got >= need) return {};
    return to_utf8(std::wstring(buf.data(), got));
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

fs::path home_dir() {
#ifdef _WIN32
    std::string p = env("USERPROFILE");
    if (!p.empty()) return from_utf8(p);
    return from_utf8(env("HOMEDRIVE") + env("HOMEPATH"));
#else
    std::string p = env("HOME");
    return p.empty() ? fs::path("/tmp") : from_utf8(p);
#endif
}

std::vector<fs::path> volume_roots() {
    std::vector<fs::path> out;
    std::error_code ec;
#ifdef _WIN32
    // 光驱没盘、读卡器空着的时候去碰它，老一点的系统会弹「驱动器中没有磁盘」的框。
    // 这一段只问、不该弹任何东西。
    DWORD old_mode = 0;
    ::SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        const UINT type = ::GetDriveTypeW(root.c_str());
        if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
        // **网络盘不去碰**：断开的那种一碰要等二三十秒，挑目录那个框就卡在那儿。
        // 点进去不在，那一头会照实说「这个目录不在」。
        if (type != DRIVE_REMOTE && !fs::is_directory(fs::path(root), ec)) continue;
        out.emplace_back(root);
    }
    ::SetThreadErrorMode(old_mode, nullptr);
#else
    out.emplace_back("/");
    // 挂载点底下一层就是一块盘（或者 `/media/<用户>` 那一层，再点一下就到）。
    // **软链不算**：macOS 的 `/Volumes/Macintosh HD` 指回 `/`，再列一遍就是重复。
#ifdef __APPLE__
    const char* const mounts[] = {"/Volumes"};
#else
    const char* const mounts[] = {"/mnt", "/media", "/run/media"};
#endif
    for (const char* m : mounts) {
        std::vector<fs::path> here;
        for (const auto& e : fs::directory_iterator(
                 m, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code sec;
            if (e.is_symlink(sec) || !e.is_directory(sec)) continue;
            here.push_back(e.path());
        }
        ec.clear();
        std::sort(here.begin(), here.end());
        out.insert(out.end(), here.begin(), here.end());
    }
#endif
    return out;
}



fs::path user_config_dir(const std::string& app_name) {
#if defined(_WIN32)
    // platformdirs 的 user_config_dir 默认 roaming=False，落在 Local 而不是 Roaming
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) return home_dir() / "AppData" / "Local" / app_name;
    return from_utf8(base) / app_name;
#elif defined(__APPLE__)
    return home_dir() / "Library" / "Application Support" / app_name;
#else
    std::string xdg = env("XDG_CONFIG_HOME");
    if (!xdg.empty()) return from_utf8(xdg) / app_name;
    return home_dir() / ".config" / app_name;
#endif
}

fs::path user_data_dir(const std::string& app_name) {
#if defined(_WIN32)
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) return home_dir() / "AppData" / "Local" / app_name;
    return from_utf8(base) / app_name;
#elif defined(__APPLE__)
    return home_dir() / "Library" / "Application Support" / app_name;
#else
    std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return from_utf8(xdg) / app_name;
    return home_dir() / ".local" / "share" / app_name;
#endif
}

fs::path expand_user(const std::string& raw) {
    if (raw.empty()) return {};
    if (raw[0] != '~') return from_utf8(raw);
    if (raw.size() == 1) return home_dir();
    if (raw[1] == '/' || raw[1] == '\\') return home_dir() / from_utf8(raw.substr(2));
    return from_utf8(raw);  // ~someuser 这种形式不支持，原样返回
}

fs::path self_exe() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                         static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    return fs::path(std::wstring(buf.data(), n));
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(buf.c_str()), ec);
    return ec ? fs::path(buf.c_str()) : p;
#else
    // /proc/self/exe 是符号链接，read_symlink 直接给目标。
    // **别用 argv[0]**：它是调用方给什么就是什么，用相对路径起的进程
    // 之后再 chdir 就找不着自己了。
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p;
#endif
}

}  // namespace changji::paths
