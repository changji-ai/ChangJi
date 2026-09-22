#include "infer/peer_auth.hpp"

#include "util/say.hpp"

#include <algorithm>
#include <cctype>

namespace changji::infer {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

/// 两端的空白。粘贴口令时带上一个换行是常事。
std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

bool is_public_bind(const std::string& host) {
    const std::string h = lower(trim(host));
    // 空 = 没说，而 Crow 没说时听的是 0.0.0.0。算对外。
    if (h.empty()) return true;
    if (h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "[::1]") {
        return false;
    }
    // 127.x.x.x 整段都是回环。
    if (h.rfind("127.", 0) == 0) return false;
    return true;
}

std::string refuse_to_listen(const std::string& host,
                             const std::string& token) {
    if (!is_public_bind(host)) return {};
    if (!trim(token).empty()) return {};
    return SAYF(
        "这个地址（%1）外面连得进来，而 [peer].token 没设——"
        "那样谁都能派活过来烧这张卡、读走这台机器有哪些模型。\n"
        "两条路：\n"
        "  只给本机用：--host 127.0.0.1（本机多卡就是这么跑的）\n"
        "  要给别的机器用：在配置里设 [peer].token，派活那一头填同一个",
        host);
}

bool endpoint_is_local(const std::string& url) {
    std::string rest = trim(url);
    const auto scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    // 砍掉路径和查询串
    const auto slash = rest.find_first_of("/?");
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    // 砍掉端口。**IPv6 要先脱方括号**：[::1]:9001 里那个冒号不是端口分隔符
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        rest = close == std::string::npos ? rest : rest.substr(1, close - 1);
    } else {
        const auto colon = rest.rfind(':');
        if (colon != std::string::npos) rest = rest.substr(0, colon);
    }
    if (rest.empty()) return false;
    return !is_public_bind(rest);
}

bool token_ok(const std::string& auth_header, const std::string& expected) {
    // **没设口令就不接。** 见头文件：安全的那边。
    const std::string want = trim(expected);
    if (want.empty()) return false;

    const std::string h = trim(auth_header);
    const std::string prefix = "bearer ";
    if (h.size() <= prefix.size()) return false;
    if (lower(h.substr(0, prefix.size())) != prefix) return false;
    const std::string got = trim(h.substr(prefix.size()));

    // 长度不同直接否——长度本来就藏不住（它就写在头里）。
    // 等长时逐字节全比完再回，不提前短路。
    if (got.size() != want.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        diff |= static_cast<unsigned char>(got[i] ^ want[i]);
    }
    return diff == 0;
}

std::string bearer_of(const std::string& auth_header) {
    const std::string h = trim(auth_header);
    const std::string prefix = "bearer ";
    if (h.size() <= prefix.size()) return {};
    if (lower(h.substr(0, prefix.size())) != prefix) return {};
    return trim(h.substr(prefix.size()));
}

}  // namespace changji::infer
