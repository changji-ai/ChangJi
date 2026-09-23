#include "http/mcp_api.hpp"

#include "util/say.hpp"

// 单独检出引擎仓库时（外层没有 agent/）扩展那几条接口的空壳：扩展是外层那份
// 代理的东西，这儿照实说没有。带着外层编的时候这个文件不进构建
// （cpp/CMakeLists.txt「编哪一份」那段）。

namespace changji::http {

namespace {
[[noreturn]] void absent() {
    throw ApiError(404, SAY("这一版引擎没有扩展"));
}
}  // namespace

ApiResult get_mcp(const std::string&, const llm::HttpPost&) { absent(); }
ApiResult post_mcp_add(const nlohmann::json&, const llm::HttpPost&) { absent(); }
ApiResult post_mcp_remove(const nlohmann::json&, const llm::HttpPost&) { absent(); }
ApiResult post_mcp_move(const nlohmann::json&, const llm::HttpPost&) { absent(); }
ApiResult post_mcp_enable(const nlohmann::json&, const llm::HttpPost&) { absent(); }
ApiResult post_mcp_trust(const nlohmann::json&, const llm::HttpPost&) { absent(); }
void mcp_shutdown() {}

}  // namespace changji::http
