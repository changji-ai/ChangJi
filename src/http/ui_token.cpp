#include "http/ui_token.hpp"

#include <fstream>
#include <iterator>
#include <random>

#include "util/atomic_file.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::http {

namespace {

std::string trimmed(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

constexpr std::size_t kMinLen = 16;

}  // namespace

std::string fresh_token() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 4; ++i) {
        const unsigned v = rd();
        for (int k = 0; k < 8; ++k) out += kHex[(v >> (k * 4)) & 0xF];
    }
    return out;
}

std::string token_problem(std::string_view token) {
    if (token.size() < kMinLen) return SAY("口令至少需要 16 个字符");
    for (const char c : token) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '-' || c == '_' || c == '.' || c == '~';
        if (!ok) return SAY("口令只能包含字母、数字和 - _ . ~");
    }
    return {};
}

MachineToken resolve_machine_token(const std::string& configured, const std::filesystem::path& data_dir) {
    if (std::string c = trimmed(configured); !c.empty()) return {c, "config"};
    if (std::string env = trimmed(paths::env("CHANGJI_UI_TOKEN")); !env.empty()) return {env, "env"};
    const auto file = data_dir / "ui_token";
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::string s = trimmed(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
            // 太短的不认（有人手改成了「123」）：重生一个。
            if (s.size() >= kMinLen) return {s, "file"};
        }
    }
    std::string t = fresh_token();
    try {
        std::error_code ec;
        std::filesystem::create_directories(data_dir, ec);
        util::write_file_atomic(file, t + "\n", /*private_only=*/true);
    } catch (...) {
        // 存不下来：照样用这一个。调用方还会往配置里搬，那一处也存不下就是重启就换。
    }
    return {t, "new"};
}

}  // namespace changji::http
