#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "setup/mover.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>

#include "config/model_index.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::setup {

namespace {

/// 数多深、数多少，同 config/model_index.cpp：有人会把整块盘指成模型目录。
constexpr int kMaxDepth = 5;
constexpr std::size_t kMaxEntries = 20000;

/// 复制时一段多大。**别太小**：几十 GB 的文件按 64 KB 一段读写，光系统调用就要
/// 几十万次；也别太大，停下要等这一段写完。
constexpr std::size_t kChunk = 8u << 20;

std::string slashed(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

/// 要不要搬它：模型文件，和下到一半的那两样（`.part`、aria2 的控制文件）。
bool worth_moving(const fs::path& p) {
    if (config::is_model_file(p)) return true;
    const std::string name = paths::to_utf8(p.filename());
    return ends_with(name, ".part") || ends_with(name, ".part.aria2");
}

/// 往上找到一个存在的目录（新目录多半还不存在——它正是这一步要建的）。
fs::path existing_ancestor(fs::path p) {
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec)) {
        const fs::path up = p.parent_path();
        if (up == p) break;
        p = up;
    }
    return p;
}

fs::path normal(const fs::path& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

bool same_volume(const fs::path& a, const fs::path& b) {
    const fs::path x = existing_ancestor(a);
    const fs::path y = existing_ancestor(b);
#ifdef _WIN32
    std::wstring rx = x.root_name().wstring();
    std::wstring ry = y.root_name().wstring();
    for (auto& c : rx) c = static_cast<wchar_t>(::towlower(c));
    for (auto& c : ry) c = static_cast<wchar_t>(::towlower(c));
    return !rx.empty() && rx == ry;
#else
    struct stat sx{}, sy{};
    if (::stat(x.c_str(), &sx) != 0 || ::stat(y.c_str(), &sy) != 0) return false;
    return sx.st_dev == sy.st_dev;
#endif
}

/// 系统那句错误，**按 UTF-8 回**。MSVC 的 `ec.message()` 走的是 ANSI 代码页，
/// 中文系统上是 GBK 字节，嵌进 JSON 就是一串方块。
std::string err_text(const std::error_code& ec) {
#ifdef _WIN32
    if (ec.category() == std::system_category()) {
        wchar_t* buf = nullptr;
        const DWORD n = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(ec.value()), 0, reinterpret_cast<LPWSTR>(&buf), 0,
            nullptr);
        std::wstring w = (n > 0 && buf != nullptr) ? std::wstring(buf, n) : std::wstring();
        if (buf != nullptr) ::LocalFree(buf);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' ||
                              w.back() == L'.' || w.back() == static_cast<wchar_t>(0x3002))) {
            w.pop_back();
        }
        if (!w.empty()) return paths::to_utf8(fs::path(w));
    }
#endif
    return ec.message();
}

/// 改名失败是不是因为"不在同一块盘上"——只有这一种才该退到复制。
/// 别的（被占用、没权限）复制也救不了，复制完删不掉原来那份，盘上就成了两份。
bool cross_device(const std::error_code& ec) {
    if (ec == std::errc::cross_device_link) return true;
#ifdef _WIN32
    if (ec.category() == std::system_category() && ec.value() == ERROR_NOT_SAME_DEVICE) {
        return true;
    }
#endif
    return false;
}

}  // namespace

json MovePlan::to_json() const {
    return {{"from", paths::to_utf8(from)},
            {"to", paths::to_utf8(to)},
            {"count", files.size()},
            {"bytes", bytes},
            {"sameVolume", same_volume},
            {"freeBytes", free_bytes},
            // 跨盘时新目录那块盘装不装得下。同一块盘改名不占地方，不用比。
            {"fits", same_volume || free_bytes >= bytes},
            {"truncated", truncated}};
}

MovePlan plan_move(const fs::path& from_in, const fs::path& to_in) {
    MovePlan p;
    p.from = normal(from_in);
    p.to = normal(to_in);
    p.same_volume = same_volume(p.from, p.to);
    {
        std::error_code ec;
        const auto s = fs::space(existing_ancestor(p.to), ec);
        if (!ec) p.free_bytes = static_cast<std::uint64_t>(s.available);
    }
    std::error_code ec;
    if (p.from == p.to || !fs::is_directory(p.from, ec)) return p;

    fs::recursive_directory_iterator it(p.from, fs::directory_options::skip_permission_denied, ec);
    if (ec) return p;
    const fs::recursive_directory_iterator end;
    std::size_t seen = 0;
    while (it != end) {
        if (++seen > kMaxEntries) {
            p.truncated = true;
            break;
        }
        const fs::directory_entry& e = *it;
        std::error_code tec;
        if (e.is_directory(tec)) {
            const std::string name = paths::to_utf8(e.path().filename());
            // 新目录就在原来那个里面：那一截里的已经在该在的地方了。
            if ((!name.empty() && name[0] == '.') || normal(e.path()) == p.to) {
                it.disable_recursion_pending();
            } else if (it.depth() + 1 >= kMaxDepth) {
                it.disable_recursion_pending();
                p.truncated = true;
            }
        } else if (e.is_regular_file(tec) && worth_moving(e.path())) {
            std::error_code sec;
            const auto n = e.file_size(sec);
            if (!sec) {
                p.files.push_back(slashed(paths::to_utf8(e.path().lexically_relative(p.from))));
                p.bytes += static_cast<std::uint64_t>(n);
            }
        }
        it.increment(tec);
        if (tec) {
            p.truncated = true;
            break;
        }
    }
    std::sort(p.files.begin(), p.files.end());
    return p;
}

namespace {

/// 两个文件一个字节一个字节地比。读不动的算不一样。
bool same_bytes(const fs::path& a, const fs::path& b) {
    std::ifstream ia(a, std::ios::binary);
    std::ifstream ib(b, std::ios::binary);
    if (!ia || !ib) return false;
    std::vector<char> x(kChunk);
    std::vector<char> y(kChunk);
    for (;;) {
        ia.read(x.data(), static_cast<std::streamsize>(x.size()));
        ib.read(y.data(), static_cast<std::streamsize>(y.size()));
        const auto na = ia.gcount();
        const auto nb = ib.gcount();
        if (na != nb) return false;
        if (na <= 0) return !ia.bad() && !ib.bad();
        if (std::memcmp(x.data(), y.data(), static_cast<std::size_t>(na)) != 0) return false;
    }
}

}  // namespace

std::string move_one(const fs::path& src, const fs::path& dst,
                     const std::function<bool(std::uint64_t)>& progress, bool* canceled,
                     bool force_copy) {
    if (canceled != nullptr) *canceled = false;
    std::error_code ec;
    const auto want = fs::file_size(src, ec);
    if (ec) return SAYF("无法移动：%1", err_text(ec));

    if (fs::exists(dst, ec)) {
        // **同一个文件换了个写法**（大小写不敏感的盘挂在 Linux 上、bind mount、硬链接）：
        // weakly_canonical 认不出来，下面按「那边已经有一份一样大的」处理就是删掉唯一
        // 的那一份。什么都不做。
        std::error_code eq;
        if (fs::equivalent(src, dst, eq)) return {};
        std::error_code sec;
        const auto there = fs::file_size(dst, sec);
        // 新目录里已经有一份一样大的（上一回搬到一半、或者人先拷过去了）：**逐字节比
        // 过**才算搬过了。只比大小的话，人拿资源管理器 / rsync --preallocate 拷到一半
        // 的那份（大小先占满了）会被当成全的，删掉的是完整的原件。
        if (!sec && there == want) {
            if (!same_bytes(src, dst)) {
                return SAY("新目录中已有同名、同大小但内容不同的文件（可能未复制完整），"
                           "已跳过。");
            }
            fs::remove(src, ec);
            if (ec) return SAYF("已复制，但无法删除原文件：%1", err_text(ec));
            return {};
        }
        // **不盖**：大小不一样就是两份不同的东西，盖掉哪一份都可能是人要的那份。
        return SAY("新目录中已有同名但大小不同的文件，已跳过。");
    }

    fs::create_directories(dst.parent_path(), ec);
    if (ec) return SAYF("无法移动：%1", err_text(ec));

    if (!force_copy) {
        fs::rename(src, dst, ec);
        if (!ec) return {};
        if (!cross_device(ec)) return SAYF("无法移动：%1", err_text(ec));
    }

    // ---- 跨盘：复制、核字节数、改名、删原来那份 ----
    fs::path tmp = dst;
    tmp += ".moving";
    bool stopped = false;
    {
        // 先开原来那份：开不了就别在新目录里留一个空的 `.moving`。
        std::ifstream in(src, std::ios::binary);
        if (!in) return SAYF("复制过程中出错：%1", paths::to_utf8(src));
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return SAYF("复制过程中出错：%1", paths::to_utf8(tmp));
        std::vector<char> buf(kChunk);
        std::uint64_t done = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize n = in.gcount();
            if (n <= 0) break;
            out.write(buf.data(), n);
            // 写不进去多半是盘满了：留着半截只会占地方。
            if (!out) {
                out.close();
                fs::remove(tmp, ec);
                return SAYF("复制过程中出错：%1", paths::to_utf8(tmp));
            }
            done += static_cast<std::uint64_t>(n);
            if (progress && !progress(done)) {
                stopped = true;
                break;
            }
        }
        if (!stopped && in.bad()) {
            out.close();
            fs::remove(tmp, ec);
            return SAYF("复制过程中出错：%1", paths::to_utf8(src));
        }
    }
    if (stopped) {
        fs::remove(tmp, ec);
        if (canceled != nullptr) *canceled = true;
        return "-";
    }
    std::error_code sec;
    if (fs::file_size(tmp, sec) != want || sec) {
        fs::remove(tmp, ec);
        return SAY("复制后大小不一致，已保留原文件。");
    }
    // **落盘了再删原件**：复制的那份还在页缓存里就删了原来那份，这时断电两份都没了。
    paths::sync_to_disk(tmp);
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, sec);
        return SAYF("无法移动：%1", err_text(ec));
    }
    fs::remove(src, ec);
    if (ec) return SAYF("已复制，但无法删除原文件：%1", err_text(ec));
    return {};
}

const char* to_string(MoveState v) {
    switch (v) {
        case MoveState::Idle: return "idle";
        case MoveState::Running: return "running";
        case MoveState::Done: return "done";
        case MoveState::Failed: return "failed";
        case MoveState::Canceled: return "canceled";
    }
    return "idle";
}

json MoveSnapshot::to_json() const {
    json errs = json::array();
    for (const auto& [path, why] : errors) errs.push_back({{"path", path}, {"error", why}});
    return {{"state", to_string(state)},
            {"from", from},
            {"to", to},
            {"total", total},
            {"done", done},
            {"count", count},
            {"moved", moved},
            {"current", current},
            {"speedBps", speed_bps},
            {"errors", errs},
            {"error", error}};
}

Mover& Mover::instance() {
    static Mover m;
    return m;
}

Mover::~Mover() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Mover::running() const { return running_.load(); }

MoveSnapshot Mover::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
}

void Mover::cancel() { cancel_ = true; }

bool Mover::start(const fs::path& from, const fs::path& to, std::function<void()> on_done) {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    MovePlan plan = plan_move(from, to);
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = MoveSnapshot{};
        snap_.state = MoveState::Running;
        snap_.from = paths::to_utf8(plan.from);
        snap_.to = paths::to_utf8(plan.to);
        snap_.total = plan.bytes;
        snap_.count = plan.files.size();
    }
    worker_ = std::thread(&Mover::run, this, plan.from, plan.to, std::move(plan.files),
                          std::move(on_done));
    return true;
}

void Mover::run(fs::path from, fs::path to, std::vector<std::string> files,
                std::function<void()> on_done) {
    using clock = std::chrono::steady_clock;
    std::uint64_t finished = 0;   // 前面几个文件一共多少字节
    bool stopped = false;
    for (const std::string& rel : files) {
        if (cancel_) {
            stopped = true;
            break;
        }
        const fs::path src = from / paths::from_utf8(rel);
        const fs::path dst = to / paths::from_utf8(rel);
        std::error_code ec;
        const auto size = fs::file_size(src, ec);
        {
            std::lock_guard<std::mutex> lock(mu_);
            snap_.current = rel;
        }
        auto last_at = clock::now();
        std::uint64_t last_done = 0;
        double smoothed = 0.0;
        const auto progress = [&](std::uint64_t done) {
            const auto now = clock::now();
            const double dt = std::chrono::duration<double>(now - last_at).count();
            if (dt >= 0.5) {
                const double speed = static_cast<double>(done - last_done) / dt;
                smoothed = smoothed <= 0.0 ? speed : smoothed * 0.7 + speed * 0.3;
                last_at = now;
                last_done = done;
            }
            std::lock_guard<std::mutex> lock(mu_);
            snap_.done = finished + done;
            snap_.speed_bps = smoothed;
            return !cancel_.load();
        };
        bool canceled = false;
        const std::string why = move_one(src, dst, progress, &canceled);
        if (canceled) {
            stopped = true;
            break;
        }
        finished += ec ? 0 : static_cast<std::uint64_t>(size);
        std::lock_guard<std::mutex> lock(mu_);
        snap_.done = finished;
        if (why.empty()) {
            ++snap_.moved;
        } else {
            snap_.errors.emplace_back(rel, why);
        }
    }

    // 搬空了的子目录收掉（`video/`、`loras/` 这种）。**原来那个目录本身不动**：
    // 它可能是人自己的一个文件夹，里面还有别的东西——空不空都留给人去处理。
    std::vector<fs::path> dirs;
    for (const std::string& rel : files) {
        fs::path d = (from / paths::from_utf8(rel)).parent_path();
        while (d != from && d.has_relative_path() && d.parent_path() != d) {
            dirs.push_back(d);
            d = d.parent_path();
        }
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    std::stable_sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();   // 深的先删
    });
    for (const auto& d : dirs) {
        std::error_code ec;
        if (fs::is_directory(d, ec) && fs::is_empty(d, ec)) fs::remove(d, ec);
    }
    config::forget_model_index();

    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.current.clear();
        snap_.speed_bps = 0.0;
        if (stopped || cancel_) {
            snap_.state = MoveState::Canceled;
        } else if (!snap_.errors.empty()) {
            snap_.state = MoveState::Failed;
            snap_.error = SAYF("有 %1 个文件未能移动，原因见各条目。",
                               std::to_string(snap_.errors.size()));
        } else {
            snap_.state = MoveState::Done;
        }
    }
    if (on_done) on_done();
    running_ = false;
}

}  // namespace changji::setup
