// macOS：这一份 .app 在不在「应用程序」里（`util/app_place.hpp`）。
//
// 两种坏法都贵：漏判 = 人一直从 dmg 里跑，推出 dmg 程序就没了；误判 = 已经
// 装好的机器每次启动都被一个框拦一下。判据就是一串前缀比较——最容易写错、
// 也最容易测的那一段。

#include <doctest/doctest.h>

#include "util/app_place.hpp"

using changji::util::AppPlace;
using changji::util::app_place;
using changji::util::decline_key;
using changji::util::mount_holding;
using changji::util::path_is_under;

namespace {
const std::string kHome = "/Users/tester";
}  // namespace

TEST_CASE("装在哪儿：「应用程序」里的不打扰") {
    CHECK(app_place("/Applications/changji.app", kHome) == AppPlace::applications);
    CHECK(app_place("/Applications/changji.app/", kHome) == AppPlace::applications);
    // 子文件夹也算装好了——人有权自己归类。
    CHECK(app_place("/Applications/影视/changji.app", kHome) == AppPlace::applications);
    // 非管理员账号只能装在自己的 ~/Applications。
    CHECK(app_place(kHome + "/Applications/changji.app", kHome) == AppPlace::applications);
}

TEST_CASE("装在哪儿：长得像但不是（前缀要带尾斜杠）") {
    CHECK(app_place("/Applications2/changji.app", kHome) == AppPlace::elsewhere);
    CHECK(app_place(kHome + "/ApplicationsOld/changji.app", kHome) == AppPlace::elsewhere);
    // 别人主目录底下的不是这个人的安装位置。
    CHECK(app_place("/Users/other/Applications/changji.app", kHome) == AppPlace::elsewhere);
    // 主目录取不到时，不能把 "/Applications" 拼成相对路径去比。
    CHECK(app_place("/Applications/changji.app", "") == AppPlace::applications);
    CHECK(app_place("/Downloads/changji.app", "") == AppPlace::elsewhere);
}

TEST_CASE("装在哪儿：该问的那几处") {
    CHECK(app_place(kHome + "/Downloads/changji.app", kHome) == AppPlace::elsewhere);
    CHECK(app_place(kHome + "/Desktop/changji.app", kHome) == AppPlace::elsewhere);
    CHECK(app_place("/Volumes/changji/changji.app", kHome) == AppPlace::elsewhere);
}

TEST_CASE("装在哪儿：被映射的那一档排在最前面") {
    CHECK(app_place("/private/var/folders/ab/cd/T/AppTranslocation/DEAD-BEEF/d/changji.app",
                    kHome) == AppPlace::translocated);
    CHECK(app_place("/var/folders/ab/cd/T/AppTranslocation/DEAD-BEEF/d/changji.app", kHome)
          == AppPlace::translocated);
}

TEST_CASE("装在哪儿：不是 .app 就不判（构建目录里的裸二进制）") {
    CHECK(app_place(kHome + "/build/desktop/changji-desktop", kHome) == AppPlace::not_bundled);
    CHECK(app_place(".app", kHome) == AppPlace::not_bundled);
}

TEST_CASE("「以后再说」的键：被映射的换成原来那个地方") {
    const std::string moved = "/private/var/folders/x/T/AppTranslocation/1234/d/changji.app";
    CHECK(decline_key(moved, AppPlace::translocated, "/Volumes/changji/changji.app")
          == "/Volumes/changji/changji.app");
    // 系统不肯说原来在哪儿时，折成一个固定串——拿随机路径当键等于每次都问。
    CHECK(decline_key(moved, AppPlace::translocated, "") == "<translocated>");
    CHECK(decline_key(kHome + "/Downloads/changji.app/", AppPlace::elsewhere, "")
          == kHome + "/Downloads/changji.app");
}

TEST_CASE("在哪个 dmg 里：同名的第二个挂载点不算") {
    const std::vector<std::string> mounts = {"/Volumes/changji", "/Volumes/changji 1"};
    CHECK(mount_holding("/Volumes/changji/changji.app", mounts) == "/Volumes/changji");
    CHECK(mount_holding("/Volumes/changji 1/changji.app", mounts) == "/Volumes/changji 1");
    CHECK(mount_holding(kHome + "/Downloads/changji.app", mounts).empty());
    // 挂载点自己不算"在里面"。
    CHECK_FALSE(path_is_under("/Volumes/changji", "/Volumes/changji/"));
    // 套着挂的取最里面那一层。
    CHECK(mount_holding("/Volumes/a/b/changji.app", {"/Volumes/a", "/Volumes/a/b"})
          == "/Volumes/a/b");
}
