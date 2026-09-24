#pragma once

// macOS：这一份 .app 摆在哪儿——在不在「应用程序」里、要不要问人搬进去。
//
// 桌面端的 dmg 打开就是一颗 changji.app 和「双击运行」四个字（没摆
// /Applications 替身，见 `desktop/package_macos.sh`）。于是**最常见的第一次
// 打开，就是在 dmg 里直接双击**——那一份跑在一个随机的只读路径上，人一推出
// dmg 程序就跟着没了，下次还得去「下载」里找那个 dmg。所以一起来先看一眼
// 自己在哪儿，不在「应用程序」里就问一句要不要搬（`desktop/relocate_mac.cpp`
// 管问和搬，这儿只管**判**）。
//
// 纯字符串，单独摆在这儿是为了能进 doctest（`test_app_place.cpp`）。三处都是
// 别的项目里真踩过的，一写错就是一整类机器判反：
//
//   一、**前缀要带尾斜杠比。** `/Applications2/changji.app` 按裸前缀比会被
//       算成「装好了」——最该问的那台反而不问。挂载点同理：同名的 dmg 挂
//       第二次叫 `/Volumes/changji 1`，按裸前缀比它"在" `/Volumes/changji` 里。
//   二、**被映射（App Translocation）的那一档要排在最前面判。** 带着隔离
//       属性直接双击时，系统不在原地跑，而是映射到
//       `/private/var/folders/…/AppTranslocation/<UUID>/d/changji.app`——既不在
//       「应用程序」里、也不像下载目录，可它**恰恰是最该问的那一种**。
//   三、**判子串，不调 `SecTranslocateIsTranslocatedURL`。** 那一族不在公开
//       头文件里，而这个路径形状从 10.12 起没变过。

#include <string>
#include <vector>

namespace changji::util {

enum class AppPlace {
    applications,   ///< /Applications 或 ~/Applications 底下（子目录也算：人有权自己归类）
    translocated,   ///< 系统映射出来的只读随机路径（直接从 dmg / 解压出来就双击）
    elsewhere,      ///< 别的哪儿都算：下载、桌面、dmg 挂载点、U 盘
    not_bundled,    ///< 根本不是 .app（直接跑构建目录里的裸二进制）——不适用
};

/// 去掉结尾多余的 `/`（`/Applications/changji.app/` 和不带斜杠的是同一个地方）。
inline std::string trim_trailing_slash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

/// `path` 在不在 `dir` 底下。**带尾斜杠比**（见文件头第一条）；`dir` 本身不算在底下。
inline bool path_is_under(const std::string& path_in, const std::string& dir_in) {
    const std::string path = trim_trailing_slash(path_in);
    const std::string dir = trim_trailing_slash(dir_in);
    if (dir.empty()) return false;
    const std::string prefix = dir == "/" ? dir : dir + "/";
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

/// - `bundle`：`NSBundle.mainBundle.bundlePath`
/// - `home`：当前用户的主目录
inline AppPlace app_place(const std::string& bundle_in, const std::string& home) {
    const std::string bundle = trim_trailing_slash(bundle_in);
    static const std::string kExt = ".app";
    if (bundle.size() <= kExt.size()
        || bundle.compare(bundle.size() - kExt.size(), kExt.size(), kExt) != 0) {
        return AppPlace::not_bundled;
    }
    if (bundle.find("/AppTranslocation/") != std::string::npos) return AppPlace::translocated;
    if (path_is_under(bundle, "/Applications")) return AppPlace::applications;
    if (!trim_trailing_slash(home).empty() && path_is_under(bundle, home + "/Applications")) {
        return AppPlace::applications;
    }
    return AppPlace::elsewhere;
}

/// 「以后再说」记在哪个键上。
///
/// ⚠️ **被映射的那一份每次启动路径都不一样**（随机 UUID），拿它当键等于永远
/// 对不上，于是每次都问——所以先换成原来那个地方（`original`，映射前的路径，
/// 系统肯告诉就有）；换不到就折成一个固定串。
inline std::string decline_key(const std::string& bundle, AppPlace place,
                               const std::string& original) {
    if (place != AppPlace::translocated) return trim_trailing_slash(bundle);
    if (!original.empty()) return trim_trailing_slash(original);
    return "<translocated>";
}

/// `path` 落在哪个挂载点里——用来认"这一份是不是躺在一个 dmg 里"（搬完要把
/// 那个 dmg 推出去）。`mounts` 只该给**磁盘映像**的挂载点（`hdiutil info`
/// 列出来的那些）：给了 U 盘的，搬完就把人的 U 盘推出去了。
///
/// 回最长的那一个（挂载点可以套着挂）；哪个都不在里面回空串。
inline std::string mount_holding(const std::string& path,
                                 const std::vector<std::string>& mounts) {
    std::string best;
    for (const std::string& m : mounts) {
        const std::string mm = trim_trailing_slash(m);
        if (mm.size() > best.size() && path_is_under(path, mm)) best = mm;
    }
    return best;
}

}  // namespace changji::util
