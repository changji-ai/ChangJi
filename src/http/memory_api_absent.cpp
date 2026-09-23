#include "http/memory_api.hpp"

#include "util/say.hpp"

// 单独检出引擎仓库时（外层没有 agent/）那三条接口的空壳：记忆是外层那份代理
// 的东西，这儿照实说没有，而不是编不过、或者假装一条都没记。
// 带着外层编的时候这个文件不进构建（cpp/CMakeLists.txt「编哪一份」那段）。

namespace changji::http {

namespace {
[[noreturn]] void absent() {
    throw ApiError(404, SAY("这一版引擎没有记忆"));
}
}  // namespace

ApiResult get_memory(const std::string&) { absent(); }
ApiResult post_memory_forget(const nlohmann::json&) { absent(); }
ApiResult post_memory_move(const nlohmann::json&) { absent(); }

}  // namespace changji::http
