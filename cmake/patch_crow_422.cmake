# 给 Crow 的状态码表补上 422。
#
# **Crow 1.2.0 的表里没有 422。** 遇到不认识的码它会打一句警告然后
# 把响应改成 500：
#
#     if (!statusCodes.count(res.code)) { CROW_LOG_WARNING << ...; res.code = 500; }
#
# 后果：所有 pydantic 风格的校验错误（缺必填字段、extra="forbid" 违规、
# 缺必填查询参数）在 C++ 侧都是以 500 发出去的，而 body 是对的。
# 前端按状态码分流的话，一个"表单填错了"会被当成"服务端挂了"。
#
# **这个 bug 单元测试看不见**：那边直接调处理函数，检查的是 ApiResult.status，
# 根本不经过 Crow 的写响应那一步。是实时对拍（tests/compat）抓出来的——
# Python 回 422，C++ 回 500。
#
# 上游在 master 上修了（common.h 加了 WEBDAV_UNPROCESSABLE_ENTITY = 422，
# 表也挪进了 http_response.h），但**还没进任何一个发布版**。
# 与其为一行修复把 Crow 换到未发布的 master（那边 onclose 的签名也变了，
# 见 http/server.cpp 里那段注释），不如在这儿补一行。
#
# 换 Crow 版本时这个脚本会**响亮地失败**：找不到锚点就直接 FATAL_ERROR，
# 而不是默默不打补丁然后又回到 500。

function(changji_patch_crow_422 crow_include_dir)
    set(target "${crow_include_dir}/crow/http_connection.h")
    if(NOT EXISTS "${target}")
        message(FATAL_ERROR "补 Crow 的 422 时找不到 ${target}。Crow 的目录结构变了？")
    endif()

    file(READ "${target}" content)

    # 已经有 422 就什么都不做。重跑 cmake 不该把补丁打两遍。
    string(FIND "${content}" "422 Unprocessable" already)
    if(NOT already EQUAL -1)
        return()
    endif()

    set(anchor "{status::CONFLICT, \"HTTP/1.1 409 Conflict\\r\\n\"},")
    string(FIND "${content}" "${anchor}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR
            "补 Crow 的 422 时找不到锚点。Crow 换版本了吗？\n"
            "先确认新版本自带 422（上游 master 已修），确认了就把这个补丁删掉。")
    endif()

    string(REPLACE "${anchor}"
        "${anchor}\n              {422, \"HTTP/1.1 422 Unprocessable Entity\\r\\n\"},  // changji: 见 cmake/patch_crow_422.cmake"
        content "${content}")
    file(WRITE "${target}" "${content}")
    message(STATUS "给 Crow 的状态码表补上了 422（见 cmake/patch_crow_422.cmake）")
endfunction()
