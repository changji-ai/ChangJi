#include "util/fs_time.hpp"

#include <chrono>
#include <system_error>

namespace fs = std::filesystem;

namespace changji::util {

double file_mtime_unix(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0.0;

#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    // C++20 的正路：让标准库去换纪元。
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
#else
    // **同一个文件读两次必须是同一个数。** 原来的退路是「t − 文件时钟的现在 +
    // 系统时钟的现在」：两次 now() 之间差几十到几百纳秒，而 1.7e9 秒上一个 double
    // 的最小刻度才 240 纳秒——线程在两次 now() 之间被切走一下，同一个没动过的
    // 文件就读出两个不一样的 mtime。「原地重出」靠 mtime 相等认（agent/outcome
    // 的 changed_media），于是没动过的参考图偶尔被报成这一轮新出的（全量用例
    // 2026-09-25 撞到一次，单跑不出）。GCC 13、macOS 的 libc++ 都走这条。
    //
    // 标准库带 `to_sys` 的（libstdc++、libc++ 都有）用它，精确；都没有的话差值
    // **整个进程只算一次**，之后同一个文件永远换出同一个数。
    using FileClock = fs::file_time_type::clock;
    std::chrono::system_clock::time_point sys;
    if constexpr (requires { FileClock::to_sys(t); }) {
        sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(FileClock::to_sys(t));
    } else {
        static const auto offset = std::chrono::system_clock::now().time_since_epoch() -
                                   std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                       FileClock::now().time_since_epoch());
        sys = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(t.time_since_epoch()) +
            offset);
    }
#endif
    return std::chrono::duration<double>(sys.time_since_epoch()).count();
}

}  // namespace changji::util
