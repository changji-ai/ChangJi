# 让 Crow 在 Windows 上读得了带中文的文件路径。
#
# **Crow 1.2.0 发静态文件走的是窄字符 API**：`set_static_file_info_unsafe` 里
# `stat(path.c_str())`，`do_write_static` 里 `std::ifstream(path.c_str())`。
# 窄字符串在 Windows 上按**系统代码页**（中文系统是 GBK）解，而我们交过去的
# 是 UTF-8——片子目录名一带中文（默认起的名字全是中文），stat 就失败，Crow 把
# 响应改成 404 清掉路径，`/api/media` 照实回 500「打不开文件」。
#
# 后果是 Windows 上**每一张参考图、每一格首帧、每一段片**都读不出来：设定那一格
# 写「图读不出来」，镜头墙一片空，播放器黑屏。2026-09-25 桌面端自检时引擎日志里
# 撞见的（`Cast.qml: Error transferring …/api/media … Internal Server Error`）。
# macOS / Linux 上窄字符串就是 UTF-8，看不出来。
#
# 补法：Windows 上两处都改走 `std::filesystem::u8path`（MSVC 拿它开宽字符 API）。
# 顺手把 Content-Length 换成 64 位的数——MSVC 的 `struct stat` 里 `st_size` 是
# 32 位的，一两个 G 的整部电影本来就会报错长度。
#
# 换 Crow 版本时这个脚本会**响亮地失败**（同 patch_crow_422.cmake）：找不到锚点
# 就 FATAL_ERROR，而不是默默不打补丁、Windows 上又回到读不出图。

function(changji_patch_crow_utf8_path crow_include_dir)
    set(resp "${crow_include_dir}/crow/http_response.h")
    set(conn "${crow_include_dir}/crow/http_connection.h")
    foreach(f IN ITEMS "${resp}" "${conn}")
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "补 Crow 的 UTF-8 路径时找不到 ${f}。Crow 的目录结构变了？")
        endif()
    endforeach()

    # 锚点只认一行里的字，不认换行：Windows 上检出的 Crow 是 CRLF，写成
    # 「#pragma once 加 \n」那种锚点一个都匹配不上，而且不报错。
    # ---- http_response.h：stat 和 Content-Length ----
    file(READ "${resp}" content)
    string(FIND "${content}" "changji_utf8_path" already)
    if(already EQUAL -1)
        set(stat_anchor [=[            file_info.statResult = stat(file_info.path.c_str(), &file_info.statbuf);]=])
        set(len_anchor [=[                this->add_header("Content-Length", std::to_string(file_info.statbuf.st_size));]=])
        foreach(a IN ITEMS "${stat_anchor}" "${len_anchor}")
            string(FIND "${content}" "${a}" pos)
            if(pos EQUAL -1)
                message(FATAL_ERROR
                    "补 Crow 的 UTF-8 路径时找不到锚点（http_response.h）。Crow 换版本了吗？\n"
                    "先确认新版本在 Windows 上认 UTF-8 路径，确认了就把这个补丁删掉。")
            endif()
        endforeach()
        string(REPLACE "${stat_anchor}" [=[            // changji_utf8_path: 见 cmake/patch_crow_utf8_path.cmake
            std::uint64_t changji_size = 0;
#ifdef _WIN32
            {
                std::error_code changji_ec;
                const std::filesystem::path changji_p = std::filesystem::u8path(file_info.path);
                const bool changji_reg = std::filesystem::is_regular_file(changji_p, changji_ec);
                const auto changji_n = changji_reg ? std::filesystem::file_size(changji_p, changji_ec) : 0;
                file_info.statResult = (changji_reg && !changji_ec) ? 0 : -1;
                file_info.statbuf = {};
                if (file_info.statResult == 0)
                {
                    file_info.statbuf.st_mode = S_IFREG;
                    changji_size = static_cast<std::uint64_t>(changji_n);
                }
            }
#else
            file_info.statResult = stat(file_info.path.c_str(), &file_info.statbuf);
            if (file_info.statResult == 0) changji_size = static_cast<std::uint64_t>(file_info.statbuf.st_size);
#endif]=] content "${content}")
        string(REPLACE "${len_anchor}"
            [=[                this->add_header("Content-Length", std::to_string(changji_size));]=]
            content "${content}")
        string(REPLACE "#pragma once" "#pragma once\n#include <cstdint>\n#include <filesystem>  // changji_utf8_path\n#include <system_error>\n"
            content "${content}")
        file(WRITE "${resp}" "${content}")
    endif()

    # ---- http_connection.h：真正读文件那一下 ----
    file(READ "${conn}" content)
    string(FIND "${content}" "changji_utf8_path" already)
    if(already EQUAL -1)
        set(open_anchor [=[                std::ifstream is(res.file_info.path.c_str(), std::ios::in | std::ios::binary);]=])
        string(FIND "${content}" "${open_anchor}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR
                "补 Crow 的 UTF-8 路径时找不到锚点（http_connection.h）。Crow 换版本了吗？")
        endif()
        string(REPLACE "${open_anchor}" [=[#ifdef _WIN32
                // changji_utf8_path: 见 cmake/patch_crow_utf8_path.cmake
                std::ifstream is(std::filesystem::u8path(res.file_info.path), std::ios::in | std::ios::binary);
#else
                std::ifstream is(res.file_info.path.c_str(), std::ios::in | std::ios::binary);
#endif]=] content "${content}")
        string(REPLACE "#pragma once" "#pragma once\n#include <filesystem>  // changji_utf8_path\n"
            content "${content}")
        file(WRITE "${conn}" "${content}")
    endif()
    message(STATUS "给 Crow 补上了 Windows 的 UTF-8 文件路径（见 cmake/patch_crow_utf8_path.cmake）")
endfunction()
