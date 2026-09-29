# 给 Crow 发静态文件那条路加上「从哪儿开始、发多长」。
#
# `/api/media` 的分段请求（Range）原来是把那一段**读进内存**再回：crow 分块
# 写文件那条路（`do_write_static`，16 KB 一块、占用是常数）只认整个文件。
# 而浏览器的 `<video>` 第一下发的就是 `bytes=0-`，每拖一次进度条是
# `bytes=N-`——照字面都是"到文件尾"。放一部一两个 G 的整部电影，每拖一次
# 就是把剩下的一两个 G 读进内存，crow 再拷一份进响应（2026-09-25 审出来）。
#
# **截短不行。** RFC 允许回得比要的短，浏览器也认；可桌面端连远端引擎时
# 是 Qt Multimedia 在放，它底下是 FFmpeg 的 http——拿到一个比 Content-Range
# 总长短的 206，报一句 "Stream ends prematurely" 就断了（它默认不重连）。
#
# 所以让分段也走分块写：static_file_info 多两栏（偏移、长度），
# do_write_static 先 seek 再只发那么长；response 上加一个公开的
# `changji_static_range()` 设它们（file_info 是私有的）。`clear()` 把
# file_info 整个重置，同一条连接上下一个请求不会带着上一次的偏移。
#
# 换 Crow 版本时找不到锚点就 FATAL_ERROR：别改成找不到就跳过——那样
# `changji_static_range` 编不过倒还好，最怕的是编得过、偏移却没人理，
# 回的是从头开始的字节配着中间那段的 Content-Range，画面花掉而不报错。

function(changji_patch_crow_static_range crow_include_dir)
    set(resp "${crow_include_dir}/crow/http_response.h")
    set(conn "${crow_include_dir}/crow/http_connection.h")
    foreach(f "${resp}" "${conn}")
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "补 Crow 的分段发文件时找不到 ${f}。Crow 的目录结构变了？")
        endif()
    endforeach()

    file(READ "${resp}" r)
    string(FIND "${r}" "changji_static_range" already)
    if(already EQUAL -1)
        # 1. static_file_info 多两栏
        set(a1 "            struct stat statbuf;\n            int statResult;\n        };")
        string(FIND "${r}" "${a1}" p1)
        # 2. 公开的设置函数，挂在 set_static_file_info 前面（那一段是 public 的）
        set(a2 "        /// Return a static file as the response body\n        void set_static_file_info(std::string path)")
        string(FIND "${r}" "${a2}" p2)
        if(p1 EQUAL -1 OR p2 EQUAL -1)
            message(FATAL_ERROR
                "补 Crow 的分段发文件时找不到锚点（http_response.h）。Crow 换版本了吗？\n"
                "先看新版本能不能按区间发静态文件，能的话 /api/media 改用它、删掉这个补丁。")
        endif()
        string(REPLACE "${a1}"
            "            struct stat statbuf;\n            int statResult;\n            std::uint64_t changji_offset = 0;  // changji: 见 cmake/patch_crow_static_range.cmake\n            std::uint64_t changji_length = static_cast<std::uint64_t>(-1);\n        };"
            r "${r}")
        string(REPLACE "${a2}"
            "        /// changji: 只发文件里 [offset, offset + length) 那一段。先调 set_static_file_info_unsafe。\n        /// 见 cmake/patch_crow_static_range.cmake。Content-Length 跟着改成 length。\n        void changji_static_range(std::uint64_t offset, std::uint64_t length)\n        {\n            file_info.changji_offset = offset;\n            file_info.changji_length = length;\n            set_header(\"Content-Length\", std::to_string(length));\n        }\n\n${a2}"
            r "${r}")
        file(WRITE "${resp}" "${r}")
    endif()

    file(READ "${conn}" c)
    string(FIND "${c}" "changji_left" already)
    if(already EQUAL -1)
        set(a3 "                char buf[16384];\n                is.read(buf, sizeof(buf));\n                while (is.gcount() > 0)\n                {\n                    buffers[0] = asio::buffer(buf, is.gcount());\n                    do_write_sync(buffers);\n                    is.read(buf, sizeof(buf));\n                }")
        string(FIND "${c}" "${a3}" p3)
        if(p3 EQUAL -1)
            message(FATAL_ERROR
                "补 Crow 的分段发文件时找不到锚点（http_connection.h 的 do_write_static）。"
                "Crow 换版本了吗？见 cmake/patch_crow_static_range.cmake。")
        endif()
        string(REPLACE "${a3}"
            "                char buf[16384];\n                // changji: 只发 [offset, offset + length)，见 cmake/patch_crow_static_range.cmake\n                std::uint64_t changji_left = res.file_info.changji_length;\n                if (res.file_info.changji_offset > 0) is.seekg(static_cast<std::streamoff>(res.file_info.changji_offset));\n                is.read(buf, static_cast<std::streamsize>(std::min<std::uint64_t>(sizeof(buf), changji_left)));\n                while (is.gcount() > 0)\n                {\n                    buffers[0] = asio::buffer(buf, is.gcount());\n                    do_write_sync(buffers);\n                    changji_left -= static_cast<std::uint64_t>(is.gcount());\n                    if (changji_left == 0) break;\n                    is.read(buf, static_cast<std::streamsize>(std::min<std::uint64_t>(sizeof(buf), changji_left)));\n                }"
            c "${c}")
        file(WRITE "${conn}" "${c}")
    endif()
    message(STATUS "给 Crow 发静态文件补上了按区间发（见 cmake/patch_crow_static_range.cmake）")
endfunction()
