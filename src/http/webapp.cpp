#include "http/webapp.hpp"

#include <mutex>
#include <string>
#include <vector>

#include "http/bundled_webapp.inc.hpp"

namespace changji::http {

namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// 把按段存的表拼回"一个文件一条"。
///
/// **只拼一次。** MSVC 的字符串字面量上限是 65535 字节，而 index-*.js
/// 有 131 KB，所以生成器把每个文件切成了连着的几条记录
/// （相邻字面量拼接省不掉这一步——拼完仍算一个字面量）。
/// 每次请求都重拼一遍是白费，所以在第一次用到时拼好放着。
const std::vector<std::pair<std::string, std::string>>& files() {
    static const std::vector<std::pair<std::string, std::string>> kFiles = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [name, chunk] : kBundledWebappChunks) {
            if (!out.empty() && out.back().first == name) {
                out.back().second.append(chunk);
            } else {
                out.emplace_back(std::string(name), std::string(chunk));
            }
        }
        return out;
    }();
    return kFiles;
}

}  // namespace

std::string webapp_content_type(std::string_view path) {
    // charset 一律带上。界面全是中文，少了它简体 Windows 的浏览器
    // 按本地代码页猜，出来是乱码。
    if (ends_with(path, ".html")) return "text/html; charset=utf-8";
    if (ends_with(path, ".js") || ends_with(path, ".mjs"))
        return "text/javascript; charset=utf-8";
    if (ends_with(path, ".css")) return "text/css; charset=utf-8";
    if (ends_with(path, ".json")) return "application/json; charset=utf-8";
    if (ends_with(path, ".svg")) return "image/svg+xml; charset=utf-8";
    if (ends_with(path, ".png")) return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".webp")) return "image/webp";
    if (ends_with(path, ".ico")) return "image/x-icon";
    if (ends_with(path, ".woff2")) return "font/woff2";
    if (ends_with(path, ".woff")) return "font/woff";
    if (ends_with(path, ".map")) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

const std::string* find_webapp_file(std::string_view path) {
    for (const auto& [name, body] : files()) {
        if (name == path) return &body;
    }
    return nullptr;
}

std::string normalize_webapp_path(std::string_view request_path) {
    std::string p(request_path);

    // 查询串和 fragment 不算路径的一部分
    const auto cut = p.find_first_of("?#");
    if (cut != std::string::npos) p.resize(cut);

    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    if (p.empty()) return "index.html";

    // **挡 `..`。** 现在文件都在内存里，翻不出去；但这段规整逻辑
    // 以后要是改成读磁盘，`..` 就是目录穿越。现在挡住，省得那天忘了。
    // 反斜杠也一起挡：Windows 上它同样是分隔符。
    if (p.find("..") != std::string::npos) return {};
    if (p.find('\\') != std::string::npos) return {};

    return p;
}

bool webapp_owns(std::string_view request_path) {
    // 接口不归前端管。**用 "/api/" 而不是 "/api" 裸前缀**：
    // 万一以后有个 /apidoc 之类的页面，用后者会把它误判成接口。
    if (request_path == "/api" || request_path.rfind("/api/", 0) == 0) {
        return false;
    }
    if (request_path == "/ws" || request_path.rfind("/ws/", 0) == 0) {
        return false;
    }
    return true;
}

bool wants_file(std::string_view rel_path) {
    // 只看最后一段有没有点。`/assets/index-abc.js` 有，`/shots` 没有。
    const std::size_t slash = rel_path.find_last_of('/');
    const std::string_view last =
        slash == std::string_view::npos ? rel_path : rel_path.substr(slash + 1);
    if (last.empty()) return false;               // 目录形式，当路由
    const std::size_t dot = last.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    if (dot == 0) return false;                   // ".gitkeep" 这种，不当资源
    return dot + 1 < last.size();                 // 点后面得真有东西
}

std::string webapp_cache_control(std::string_view rel_path) {
    // 带内容哈希的资源：内容一变文件名就变，可以往死里缓存。
    // Vite 出来的是 `assets/名字-哈希.js`。
    if (rel_path.rfind("assets/", 0) == 0 && wants_file(rel_path)) {
        return "public, max-age=31536000, immutable";
    }
    // 其余的（首要是 index.html）每次都回来核一遍。
    // **不能用 no-store**：那样连 304 都不走，每次整包重下。
    return "no-cache";
}

}  // namespace changji::http
