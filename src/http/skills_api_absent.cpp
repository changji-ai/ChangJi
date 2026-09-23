#include "http/skills_api.hpp"

#include "util/say.hpp"

// 单独检出引擎仓库时（外层没有 agent/）技能那几条接口的空壳：技能是外层那份
// 代理的东西，这儿照实说没有。带着外层编的时候这个文件不进构建
// （cpp/CMakeLists.txt「编哪一份」那段）。

namespace changji::http {

namespace {
[[noreturn]] void absent() {
    throw ApiError(404, SAY("这一版引擎没有技能"));
}
}  // namespace

ApiResult get_skills(const std::string&) { absent(); }
ApiResult post_skills_install(const nlohmann::json&, const llm::HttpGet&) { absent(); }
ApiResult post_skills_forget(const nlohmann::json&) { absent(); }
ApiResult post_skills_move(const nlohmann::json&) { absent(); }

}  // namespace changji::http
