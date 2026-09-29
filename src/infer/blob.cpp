#include "infer/blob.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "util/say.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace changji::infer {

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error(SAYF("读不了：%1", paths::to_utf8(p)));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

bool blob_id_ok(const std::string& id) {
    if (id.size() != 40) return false;
    for (const char c : id) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex) return false;
    }
    return true;
}

fs::path blob_path(const fs::path& cache_root, const std::string& id) {
    if (!blob_id_ok(id)) return {};
    return cache_root / "blobs" / id.substr(0, 2) / id.substr(2);
}

std::string blob_id_of(const fs::path& file) {
    return text::sha1_hex(read_all(file));
}

namespace {
/// 碰一下：刷新修改时间，blob_prune 按它判"多久没人用"。
void touch(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
}
}  // namespace

bool blob_present(const fs::path& cache_root, const std::string& id) {
    const auto p = blob_path(cache_root, id);
    if (p.empty()) return false;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    touch(p);   // 派活方问"你有没有"，就是它还要用
    return true;
}

std::size_t blob_prune(const fs::path& cache_root, fs::file_time_type::duration max_age) {
    const fs::path root = cache_root / "blobs";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    const auto cutoff = fs::file_time_type::clock::now() - max_age;
    std::size_t gone = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (it->last_write_time(e2) < cutoff && !e2) {
            if (fs::remove(it->path(), e2)) ++gone;
        }
    }
    return gone;
}

std::string blob_store(const fs::path& cache_root, const std::string& id,
                       const std::string& bytes) {
    if (!blob_id_ok(id)) {
        return SAY("内容指纹不合法：要 40 个小写十六进制字符");
    }

    // **先核对再落地。** 截断的 png 照样"存下来了"，之后报的是
    // "权重读不对"或者一张半截图，指向完全错误的方向。
    const std::string real = text::sha1_hex(bytes);
    if (real != id) {
        return SAYN("内容对不上指纹：说好的是 %1，收到的是 %2（%n 字节）。"
                    "多半是传了一半",
                    static_cast<long long>(bytes.size()), id, real);
    }

    const auto dest = blob_path(cache_root, id);
    std::error_code ec;
    if (fs::is_regular_file(dest, ec)) {   // 本来就有
        touch(dest);
        return {};
    }

    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        return SAYF("建不出目录：%1", paths::to_utf8(dest.parent_path()));
    }

    // **先写临时文件再改名。** 直接往目标写的话，写到一半进程没了就留下
    // 一个"指纹对得上文件名、内容却是半截"的 blob——而之后每一次
    // `blob_present` 都会说它在，再也不会重传。改名是原子的，没有这个洞。
    fs::path tmp = dest;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return SAYF("写不了：%1", paths::to_utf8(tmp));
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return SAYF("写坏了：%1", paths::to_utf8(tmp));
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        // Windows 上目标已存在时 rename 会失败。那说明别人刚好也存了
        // 同一份——内容一样，无所谓谁的，清掉临时文件当成功。
        std::error_code ec2;
        if (fs::is_regular_file(dest, ec2)) {
            fs::remove(tmp, ec2);
            return {};
        }
        return SAYF("改名失败：%1", paths::to_utf8(dest));
    }
    return {};
}

std::string blob_ref_id(const std::string& s) {
    static const std::string kPrefix = "blob:";
    if (s.rfind(kPrefix, 0) != 0) return {};
    const std::string id = s.substr(kPrefix.size());
    return blob_id_ok(id) ? id : std::string();
}

fs::path resolve_input(const fs::path& cache_root, const std::string& spec) {
    const std::string id = blob_ref_id(spec);
    if (id.empty()) {
        // 不是 blob: 记法，那就是一个路径——同机那条路，直接用。
        return paths::from_utf8(spec);
    }
    const auto p = blob_path(cache_root, id);
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        throw std::runtime_error(
            SAYF("要的这份输入不在这台机器上：%1。派活那头该先把它传过来"
                 "（POST /blob/<id>）",
                 spec));
    }
    return p;
}

fs::path cache_root_of(const fs::path& workspace) {
    return workspace / "cache";
}

fs::path task_sandbox(const fs::path& cache_root, const std::string& task_id) {
    // task_id 是对面给的，会被拼进路径——**按最坏的算**，只留安全字符。
    std::string safe;
    for (const char c : task_id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        safe += ok ? c : '_';
    }
    if (safe.empty()) safe = "unnamed";
    return cache_root / "tasks" / safe;
}

std::string blob_adopt(const fs::path& cache_root, const fs::path& file) {
    const std::string bytes = read_all(file);
    const std::string id = text::sha1_hex(bytes);
    const auto why = blob_store(cache_root, id, bytes);
    if (!why.empty()) throw std::runtime_error(why);
    return id;
}

}  // namespace changji::infer
