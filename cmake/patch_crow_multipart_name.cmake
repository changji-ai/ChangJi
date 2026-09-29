# 给 Crow 的 multipart 解析补上「这一段没有 name」的判断。
#
# **Crow 1.2.0 拿 name 的那一句不查找没找到：**
#
#     get_header_object(parsed_section.headers, "Content-Disposition")
#         .params.find("name")->second
#
# 一段 multipart 里没有 Content-Disposition、或者有但不带 `name=`，
# `find` 回的是 end()，`->second` 当场解引用 end()——**整个引擎 SEGV**。
# 不是异常，路由外面那层 guard() 接不住。2026-09-25 审出来、拿 ASan 复现过：
#
#     POST /api/character/reference
#     Content-Type: multipart/form-data; boundary=X
#
#     --X\r\nFoo: bar\r\n\r\nabc\r\n--X--\r\n
#
# 同源的页面、本机的脚本、`--host` 对外监听时局域网里谁都发得出来
# （门在 http/request_guard.hpp，multipart 在这三条路由上是放行的）。
#
# 补法：没有 name 的那一段按空串当键收下。三条上传路由都是按名字取
# "file" / "project" 那几段，取不到就是 400「没有上传文件」，和别的
# 缺字段一个待遇。
#
# 换 Crow 版本时找不到锚点就 FATAL_ERROR：新版本要么修了（那就删掉这个
# 补丁），要么改了写法（那得重新看一遍还崩不崩）。**别改成找不到就跳过**
# ——那就是静悄悄回到 SEGV。

function(changji_patch_crow_multipart_name crow_include_dir)
    set(target "${crow_include_dir}/crow/multipart.h")
    if(NOT EXISTS "${target}")
        message(FATAL_ERROR "补 Crow 的 multipart 时找不到 ${target}。Crow 的目录结构变了？")
    endif()

    file(READ "${target}" content)

    # 打过了就不再打。重跑 cmake 不该把补丁打两遍。
    string(FIND "${content}" "changji: 见 cmake/patch_crow_multipart_name.cmake" already)
    if(NOT already EQUAL -1)
        return()
    endif()

    set(safe_prefix "[](const header& h) { auto it = h.params.find(\"name\"); return it == h.params.end() ? std::string() : it->second; }  /* changji: 见 cmake/patch_crow_multipart_name.cmake */ (")

    foreach(which item parsed_section)
        set(anchor "(get_header_object(${which}.headers, \"Content-Disposition\").params.find(\"name\")->second)")
        string(FIND "${content}" "${anchor}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR
                "补 Crow 的 multipart 时找不到锚点（${which}）。Crow 换版本了吗？\n"
                "先确认新版本在一段没有 name 的时候不再解引用 end()，确认了就把这个补丁删掉。")
        endif()
        string(REPLACE "${anchor}"
            "(${safe_prefix}get_header_object(${which}.headers, \"Content-Disposition\")))"
            content "${content}")
    endforeach()

    file(WRITE "${target}" "${content}")
    message(STATUS "给 Crow 的 multipart 补上了「没有 name」的判断（见 cmake/patch_crow_multipart_name.cmake）")
endfunction()
