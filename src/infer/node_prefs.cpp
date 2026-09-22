#include "infer/node_prefs.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::infer {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path node_prefs_path(const fs::path& workspace) {
    return workspace / "nodes.json";
}

NodePrefs load_node_prefs(const fs::path& workspace) {
    NodePrefs out;
    const auto p = node_prefs_path(workspace);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return out;

    std::ifstream in(p);
    if (!in) return out;
    std::ostringstream ss;
    ss << in.rdbuf();

    // 坏了就当没关任何东西，见头文件：默认值挑"不改变行为"的那边。
    const auto j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;
    const auto it = j.find("off");
    if (it == j.end() || !it->is_object()) return out;

    for (const auto& [url, caps] : it->items()) {
        if (!caps.is_array()) continue;
        std::set<Capability> set;
        for (const auto& c : caps) {
            if (!c.is_string()) continue;
            // 认不出的能力名跳过。**这儿不报错**：这份文件是程序自己写的，
            // 认不出多半是降级回了老版本，那时候忽略比拒绝启动好。
            if (const auto cap = capability_from(c.get<std::string>())) {
                set.insert(*cap);
            }
        }
        if (!set.empty()) out[url] = std::move(set);
    }
    return out;
}

void save_node_prefs(const fs::path& workspace, const NodePrefs& prefs) {
    json off = json::object();
    for (const auto& [url, caps] : prefs) {
        if (caps.empty()) continue;   // 空的就别留一行噪音
        json arr = json::array();
        for (const Capability c : caps) arr.push_back(to_string(c));
        off[url] = std::move(arr);
    }

    const auto p = node_prefs_path(workspace);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    // 先写临时文件再改名：写到一半断电留下半截 JSON 的话，下次读回来
    // 是"没关任何东西"——静悄悄地把用户的设置丢了。
    const auto tmp = p.parent_path() / "nodes.json.part";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            throw std::runtime_error(SAYF("写不了 %1", paths::to_utf8(tmp)));
        }
        f << json{{"off", off}}.dump(2) << "\n";
        if (!f) {
            throw std::runtime_error(SAYF("写坏了 %1", paths::to_utf8(tmp)));
        }
    }
    // ⚠️ **别先删目标。** 这儿原来是 `fs::remove(p, ec)` 再 rename，两条
    // 都不必要而且有害：`fs::rename` 在标准里就要求目标存在时替换掉它
    // （POSIX 语义，MSVC 底层是 MoveFileEx 加 MOVEFILE_REPLACE_EXISTING
    // ——项目存盘那条走的就是这个，见 models/project.cpp）。先删的话，
    // **rename 一旦失败，用户关掉的那些格子就全没了**：下次读回来是
    // "一个都没关"，而这一函数整个存在的理由就是别静悄悄丢掉这份设置
    // （上面那段注释说的正是这件事）。
    fs::rename(tmp, p, ec);
    if (ec) {
        // 那份半成品别留在盘上碍事——下一趟会重写它。
        std::error_code rm;
        fs::remove(tmp, rm);
        throw std::runtime_error(SAYF("替换文件失败：%1", paths::to_utf8(p)));
    }
}

}  // namespace changji::infer
