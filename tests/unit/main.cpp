// doctest 的入口。单独一个文件，这样测试主体改动时不用重编 doctest 本身。
//
// **整个测试进程都不读这台机器上真实的用户配置。**
//
// `load_settings` 先读 `user_config_path()` 再叠项目那份。以前只在几个
// 栽过跟头的用例里各自隔离（scoped_env.hpp 里那段来历：2026-09-10
// test_models_config、2026-09-13 test_writeback），别的用例照读真实配置。
// 于是又栽第三次：拆分镜那几条接口开始按项目算单镜上限
// （config::apply_video_limits），在服务器上一跑，`[models].video` 是
// MiniMax-H3，全局的帧数格子被改成 17k+5，**下游七个和它无关的用例**
// （render / readonly / planning 的对拍）跟着挂——本机全绿，服务器全红。
//
// 一处一处补隔离补不完：每加一条会读配置的路，就得记得回来给每个调它的
// 用例加一行。所以在这儿一次性把三个平台的配置目录都指到一个空目录，
// 用例里原有的 ScopedUserConfigDir 照常嵌套（各自设、各自还原）。
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "scoped_env.hpp"

int main(int argc, char** argv) {
    // 作用域盖住整个 doctest 运行；析构时还原环境变量、删掉空目录。
    changji::test::ScopedUserConfigDir isolated_config("whole_run");
    return doctest::Context(argc, argv).run();
}
