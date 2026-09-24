#include "config/model_index.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <system_error>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::config {

namespace {

/// 缓存多久。`resolve` 一秒里可能被问几十次（`/status`、体检、出片前那一圈），
/// 而模型目录几分钟才变一次。**别拉长**：人把文件拷进来之后，最多等这么久
/// 界面上那一组才变绿——按了「索引」的话立刻。
constexpr auto kFresh = std::chrono::seconds(10);

/// 扫多深。`模型目录/类型/家族/文件` 是三层，再留两层给人自己的排法。
constexpr int kMaxDepth = 5;

/// 最多看多少项（文件和目录一起数）。**有人会把整个盘指成模型目录**，
/// 那时候一次 `resolve` 扫几十万项，出片前那一圈就卡住了。
constexpr std::size_t kMaxEntries = 20000;

struct Cached {
    std::chrono::steady_clock::time_point at;
    ModelIndex index;
};

std::mutex& cache_mu() {
    static std::mutex m;
    return m;
}

std::map<std::string, Cached>& cache() {
    static std::map<std::string, Cached> c;
    return c;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// 缓存按哪一串记。**规范化过再记**：同一个目录写成 `D:\m` 和 `D:/m/`
/// 各扫一份的话，挪了文件只清得掉其中一份。
std::string key_of(const fs::path& dir) {
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(dir, ec);
    return paths::to_utf8(ec ? dir.lexically_normal() : canon);
}

std::string file_name_of(const std::string& rel) {
    const auto slash = rel.find_last_of("/\\");
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

/// 分隔符一律换成 `/`。**别用 `generic_string()`**：它在 Windows 上按 ANSI
/// 代码页转，目录名里有中文就成了问号。
std::string slashed(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::size_t depth_of(const std::string& rel) {
    return static_cast<std::size_t>(std::count(rel.begin(), rel.end(), '/'));
}

ModelIndex scan(const fs::path& dir) {
    ModelIndex out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    // **不跟目录软链**：软链成环（`models/self -> ..`）时 recursive_directory_iterator
    // 会一直转到条数封顶，而扫出来的全是同一批文件的影子。
    fs::recursive_directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    std::size_t seen = 0;
    const fs::recursive_directory_iterator end;
    while (it != end) {
        if (++seen > kMaxEntries) {
            out.truncated = true;
            break;
        }
        const fs::directory_entry& e = *it;
        const std::string name = paths::to_utf8(e.path().filename());
        std::error_code tec;
        if (e.is_directory(tec)) {
            if (!name.empty() && name[0] == '.') {
                it.disable_recursion_pending();
            } else if (it.depth() + 1 >= kMaxDepth) {
                it.disable_recursion_pending();
                out.truncated = true;
            }
        } else if (e.is_regular_file(tec) && is_model_file(e.path())) {
            std::error_code sec;
            const auto bytes = e.file_size(sec);
            if (!sec) {
                out.files.push_back({slashed(paths::to_utf8(e.path().lexically_relative(dir))),
                                     static_cast<std::uint64_t>(bytes)});
            }
        }
        // ⚠️ **出错就收手，别接着 increment。** 出错之后迭代器处在什么状态标准
        // 没说死（MSVC 上是直接变成 end），再往下走一步就是未定义行为。
        // 没扫完照实报 truncated。
        it.increment(tec);
        if (tec) {
            out.truncated = true;
            break;
        }
    }
    std::sort(out.files.begin(), out.files.end(),
              [](const ModelFile& a, const ModelFile& b) { return a.rel < b.rel; });
    return out;
}

}  // namespace

bool is_model_file(const fs::path& p) {
    static const char* const kExt[] = {".gguf", ".safetensors", ".sft", ".ckpt",
                                       ".pt",   ".pth",         ".bin", ".onnx"};
    const std::string ext = lower(paths::to_utf8(p.extension()));
    return std::any_of(std::begin(kExt), std::end(kExt),
                       [&ext](const char* k) { return ext == k; });
}

ModelIndex index_models(const fs::path& dir, bool fresh) {
    const std::string key = key_of(dir);
    const auto now = std::chrono::steady_clock::now();
    if (!fresh) {
        std::lock_guard<std::mutex> lock(cache_mu());
        const auto it = cache().find(key);
        if (it != cache().end() && now - it->second.at < kFresh) return it->second.index;
    }
    // **扫的时候不拿锁**：大目录要扫一会儿，别让别的线程问一个已经在缓存里的
    // 目录也跟着等。两条线程同时扫同一个目录无非多扫一遍。
    ModelIndex fresh_index = scan(dir);
    std::lock_guard<std::mutex> lock(cache_mu());
    cache()[key] = {now, fresh_index};
    return fresh_index;
}

std::optional<fs::path> find_model(const fs::path& dir, const std::string& name,
                                   std::uint64_t bytes) {
    if (name.empty()) return std::nullopt;
    const auto fits = [bytes](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return false;
        if (bytes == 0) return true;
        const auto n = fs::file_size(p, ec);
        return !ec && static_cast<std::uint64_t>(n) == bytes;
    };

    // 原位置上就有：一次 stat，不碰索引。
    const fs::path direct = dir / paths::from_utf8(name);
    if (fits(direct)) return direct;

    const std::string want = file_name_of(name);
    const std::string exact = slashed(name);
    const auto pick = [&](const ModelIndex& idx) -> std::optional<fs::path> {
        const ModelFile* best = nullptr;
        for (const auto& f : idx.files) {
            if (file_name_of(f.rel) != want) continue;
            if (bytes > 0 && f.bytes != bytes) continue;
            if (best == nullptr) { best = &f; continue; }
            const bool f_exact = f.rel == exact;
            const bool b_exact = best->rel == exact;
            if (f_exact != b_exact) {
                if (f_exact) best = &f;
                continue;
            }
            if (depth_of(f.rel) < depth_of(best->rel)) best = &f;
            // 同深度的已经按字母排好了，先来的就是字母小的那个。
        }
        if (best == nullptr) return std::nullopt;
        return dir / paths::from_utf8(best->rel);
    };

    auto hit = pick(index_models(dir));
    if (hit && fits(*hit)) return hit;
    // 缓存里认得、盘上却没了（刚挪走、刚删掉）：重扫一次再说。
    // ⚠️ **缓存里根本没有的不重扫。** 没下的模型 `resolve` 一直在问
    // （`/status`、体检每一圈都问一遍），每问一次翻一遍目录的话缓存就白设了。
    // 刚拷进来的最多等 kFresh 秒；人按「索引」那一下是立刻。
    if (hit) {
        hit = pick(index_models(dir, /*fresh=*/true));
        if (hit && fits(*hit)) return hit;
    }
    return std::nullopt;
}

void forget_model_index() {
    std::lock_guard<std::mutex> lock(cache_mu());
    cache().clear();
}

}  // namespace changji::config
