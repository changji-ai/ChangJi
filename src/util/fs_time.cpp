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
    // 退路：拿两个时钟的"现在"作差换算。两次 now() 只隔几微秒，
    // 误差远小于一秒，而这个值只用来显示"几分钟前"和排序。
    const auto sys = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
#endif
    return std::chrono::duration<double>(sys.time_since_epoch()).count();
}

}  // namespace changji::util
