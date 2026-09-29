#include "http/loops_api.hpp"

#include "util/say.hpp"

// 单独检出引擎仓库时（外层没有 agent/）循环那几条接口的空壳：循环是外层那份代理
// 的东西，这儿照实说没有。带着外层编的时候这个文件不进构建。

namespace changji::http {

namespace {
[[noreturn]] void absent() {
    throw ApiError(404, SAY("这一版引擎没有循环"));
}
}  // namespace

ApiResult get_loops() { absent(); }
ApiResult post_loop(const nlohmann::json&) { absent(); }
ApiResult delete_loop(const std::string&, bool) { absent(); }
ApiResult post_loop_run(const nlohmann::json&) { absent(); }
void install_loops(std::shared_ptr<llm::Client>, std::function<RunDeps()>) {}
void loops_tick() {}

}  // namespace changji::http
