#include "http/chat_branch_api.hpp"

#include "util/say.hpp"

// 单独检出引擎仓库时（外层没有 agent/）会话分叉那两条接口的空壳：分叉是外层那份代理的东西，
// 这儿照实说没有。带着外层编的时候这个文件不进构建（cpp/CMakeLists.txt「编哪一份」那段）。

namespace changji::http {

namespace {
[[noreturn]] void absent() {
    throw ApiError(404, SAY("这一版引擎没有会话分叉"));
}
}  // namespace

ApiResult get_chat_forks(const std::string&, const std::string&) { absent(); }
ApiResult post_chat_branch(const nlohmann::json&) { absent(); }

}  // namespace changji::http
