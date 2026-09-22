#include "http/media.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <utility>
#include <vector>

#include "models/project.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;

namespace changji::http {

MediaTarget resolve_media(const std::string& project_path, const std::string& rel) {
    MediaTarget out;
    if (project_path.empty()) {
        return {400, SAY("没有指定项目目录"), {}};
    }

    const models::ProjectPaths paths(paths::from_utf8(project_path));
    std::error_code ec;

    // **先确认这是个项目目录。**
    //
    // 下面那道越界检查防的是"从根跳出去"（rel 里的 `..` 和符号链接），
    // 防不了"把根指到别处"——`?path=/etc&rel=passwd` 算出来的 root 就是
    // /etc，passwd 正好在它之下，检查一路放行。
    //
    // 别的接口没有这个口子，是因为它们都要先 `load_project()`：读不到
    // project.json 就 400 了，顺带把目录管住。这一条不读项目文件，
    // 所以要自己管一次。
    //
    // 默认只监听 127.0.0.1，所以这不是敞着的洞；但 `--host` 放到局域网上
    // （文档里"电脑和手机都能开"就是这么用）之后，同网段的人本来只能看
    // 这一部电影的图和片，不该顺手能读这台机器上任何一个文件。
    //
    // 界面上每一次 mediaUrl 都是拿 session.projectPath 拼的，那必然是个
    // 已经打开过的项目，这一道不会挡到正常用法。
    if (!fs::is_regular_file(paths.project_file(), ec)) {
        return {400, SAY("这不是一个项目目录"), {}};
    }

    // 先拼再规范化。weakly_canonical 会把 .. 和符号链接都解开，
    // 这是越界检查能成立的前提——只做字符串前缀比较的话，
    // rel 传 "../../etc/passwd" 就穿出去了。
    const fs::path target = fs::weakly_canonical(
        paths.root() / changji::paths::from_utf8(rel), ec);
    const fs::path root = fs::weakly_canonical(paths.root(), ec);

    // 判断 target 是不是在 root 之下。用 lexically_relative 而不是
    // 字符串前缀：后者会把 /a/bc 误判成在 /a/b 之下。
    const fs::path relative = target.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
        return {403, SAY("只能访问项目目录内的文件"), {}};
    }

    if (!fs::is_regular_file(target, ec)) {
        return {404, SAY("文件不存在"), {}};
    }

    out.path = target;
    return out;
}

namespace {

/// Python 的 `int()`：可以带正负号，前后空白已经在调用方剥掉了。
/// 解不出来返回 false，对应 Starlette 里那个 `except ValueError: continue`。
bool parse_int(const std::string& s, long long& out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i >= s.size()) return false;
    for (std::size_t k = i; k < s.size(); ++k) {
        if (std::isdigit(static_cast<unsigned char>(s[k])) == 0) return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    // Python 的 int 没有上限，我们有。超了就当解不出来——
    // 真实请求里不会出现二十位的数字，而静默截断会让越界判定错掉。
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

}  // namespace

RangeParse parse_range(const std::string& header, std::uint64_t file_size) {
    const std::size_t eq = header.find('=');
    if (eq == std::string::npos) return {RangeVerdict::Malformed, {}};

    std::string units = trim(header.substr(0, eq));
    std::transform(units.begin(), units.end(), units.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (units != "bytes") return {RangeVerdict::Malformed, {}};

    const std::string rest = header.substr(eq + 1);
    const auto size = static_cast<long long>(file_size);

    // 逐段解析。**解不出来的段是"跳过"不是"报错"**，这是 Starlette 的
    // 做法；全都跳过了才算语法不认识。
    std::vector<std::pair<long long, long long>> ranges;  // [start, end)
    std::size_t pos = 0;
    while (pos <= rest.size()) {
        const std::size_t comma = rest.find(',', pos);
        const std::string part =
            trim(rest.substr(pos, comma == std::string::npos ? std::string::npos
                                                             : comma - pos));
        pos = comma == std::string::npos ? rest.size() + 1 : comma + 1;
        if (part.empty() || part == "-") continue;
        const std::size_t dash = part.find('-');
        if (dash == std::string::npos) continue;

        const std::string first_s = trim(part.substr(0, dash));
        const std::string last_s = trim(part.substr(dash + 1));

        long long start = 0;
        long long end = size;
        if (first_s.empty()) {
            // "bytes=-500"：末尾 500 字节。注意 Python 允许 int("-5")，
            // 于是 "bytes=--5" 算出来的起点比文件还大，最后判 416 而不是 400。
            long long n = 0;
            if (!parse_int(last_s, n)) continue;
            start = size - n > 0 ? size - n : 0;
        } else {
            if (!parse_int(first_s, start)) continue;
            long long e = 0;
            if (!last_s.empty() && parse_int(last_s, e) && e < size) {
                end = e + 1;
            } else if (!last_s.empty() && !parse_int(last_s, e)) {
                continue;  // "bytes=0-abc"：整段跳过
            }
        }
        ranges.emplace_back(start, end);
    }

    if (ranges.empty()) return {RangeVerdict::Malformed, {}};
    for (const auto& [start, end] : ranges) {
        if (start < 0 || start >= size) return {RangeVerdict::NotSatisfiable, {}};
    }
    for (const auto& [start, end] : ranges) {
        if (start >= end) return {RangeVerdict::Malformed, {}};
    }
    // 多区间：见头文件里那段"有意的偏差"。
    if (ranges.size() > 1) return {RangeVerdict::Ignore, {}};

    ByteRange r;
    r.first = static_cast<std::uint64_t>(ranges[0].first);
    r.last = static_cast<std::uint64_t>(ranges[0].second - 1);
    return {RangeVerdict::Ok, r};
}

std::string content_type_for(const fs::path& p) {
    std::string ext = paths::to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".avi")  return "video/x-msvideo";
    if (ext == ".webm") return "video/webm";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".wav")  return "audio/wav";
    if (ext == ".mp3")  return "audio/mpeg";
    // **这两个是盘上可能躺着的。** flac 今天还收（upload.cpp 的
    // `voice_types()`）；m4a 2026-09-15 起不收了——进程内那条配音走
    // miniaudio，只认 wav / mp3 / flac，收下只会在第一句台词那儿炸。
    //
    // **但这张表不跟着删 m4a**：管的是两件不同的事。那边是"往后还收不
    // 收"，这边是"已经在盘上的还发不发得对"。在那之前传过 m4a 的项目，
    // voices/ 里那一段还在，试听它的时候浏览器读得了（读不了的是配音那
    // 条路）——发成 application/octet-stream 的话它连播都播不出来。
    // 同一个道理写在 upload.cpp 的 `voice_stale_exts` 上。
    if (ext == ".m4a")  return "audio/mp4";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".srt")  return "application/x-subrip";
    if (ext == ".ass")  return "text/x-ssa";
    if (ext == ".json") return "application/json";
    if (ext == ".txt")  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

}  // namespace changji::http
