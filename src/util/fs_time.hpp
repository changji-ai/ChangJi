#pragma once

// 文件修改时间 -> Unix 秒。
//
// 单独一个文件，因为**直接用 `last_write_time().time_since_epoch()` 是错的**，
// 而错得不显眼：`std::filesystem::file_time_type` 的纪元由实现定，
// MSVC 用的是 Windows FILETIME 的 1601-01-01。一个刚写的文件在那上面是
// 一百三十多亿秒，前端拿它当 Unix 秒算出来是 2381 年，
// `humanAgo()` 一律显示"刚刚"——每个项目都显示"刚刚"，看起来像是没坏。

#include <filesystem>

namespace changji::util {

/// 文件的修改时间，Unix 秒（带小数）。取不到返回 0。
///
/// 对齐 Python 的 `os.stat().st_mtime`：那是真正的 Unix 时间戳，
/// 前端的 humanAgo() 也是按这个算的。
double file_mtime_unix(const std::filesystem::path& p);

}  // namespace changji::util
