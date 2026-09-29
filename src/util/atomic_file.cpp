#include "util/atomic_file.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;

namespace changji::util {

namespace {

/// 链接顺着走到底（最多 16 跳，环了就停在原地）。相对链接按链接所在目录解。
fs::path follow_links(fs::path p) {
    std::error_code ec;
    for (int hop = 0; hop < 16 && fs::is_symlink(p, ec); ++hop) {
        fs::path to = fs::read_symlink(p, ec);
        if (ec) break;
        p = to.is_absolute() ? to : p.parent_path() / to;
    }
    return p;
}

}  // namespace

void write_file_atomic(const fs::path& target_in, const std::string& data,
                       bool private_only) {
    const fs::path target = follow_links(target_in);
    static std::atomic<unsigned> seq{0};
    const fs::path tmp = paths::from_utf8(
        paths::to_utf8(target) + ".tmp" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(seq.fetch_add(1)));
    std::error_code ec;
    bool ok = false;
#ifndef _WIN32
    // 不要求私密的：沿用目标原来的权限（人自己 chmod 过的别给改回去），
    // 没有原文件就按 umask 来。
    mode_t mode = private_only ? 0600 : 0666;
    bool keep_mode = false;
    if (!private_only) {
        struct stat st {};
        if (::stat(target.c_str(), &st) == 0) {
            mode = st.st_mode & 07777;
            keep_mode = true;
        }
    }
    const int fd = ::open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_TRUNC | O_CLOEXEC,
                          mode);
    if (fd >= 0) {
        // open 的 mode 要过 umask；沿用原权限时补一刀（新建的照 umask 来，
        // 私密的那档 0600 过了 umask 只会更严，都不用管）。
        if (keep_mode) (void)::fchmod(fd, mode);
        std::size_t done = 0;
        ok = true;
        while (done < data.size()) {
            const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            done += static_cast<std::size_t>(n);
        }
        // 换名之前先落到盘上：不然断电后看到的可能是"新名字、空内容"。
        if (ok && ::fsync(fd) != 0) ok = false;
        if (::close(fd) != 0) ok = false;
    }
#else
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out << data;
        out.close();
        ok = static_cast<bool>(out);
    }
#endif
    if (ok) {
        fs::rename(tmp, target, ec);
        ok = !ec;
    }
    if (!ok) {
        fs::remove(tmp, ec);
        throw std::runtime_error(SAYF("写不了文件：%1", paths::to_utf8(target)));
    }
}

}  // namespace changji::util
