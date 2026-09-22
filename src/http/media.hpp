#pragma once

// GET /api/media —— 取项目内的文件。
//
// 两件事 Python 那边是白拿的、这里必须自己写：
//
// 1. **越界检查**。Starlette 没做，是路由里手写的 relative_to 判断；
//    这里同样手写，但要用 weakly_canonical 处理 .. 和符号链接。
//
// 2. **HTTP Range**。Python 用 FastAPI 的 FileResponse，Starlette 自带
//    206 Partial Content。Crow 没有——不实现的话前端 <video> 标签
//    **拖不动进度条**，只能从头播。方案第三节把这一条单独点了名。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>
#include <string>

namespace changji::http {

/// 把 rel 解析成项目内的绝对路径。
///
/// 分开成纯函数是为了能单测越界检查——那是安全边界，
/// 不该只在起了服务之后才验证。
struct MediaTarget {
    int status = 200;             ///< 200 表示解析成功
    std::string detail;           ///< 出错时的说明
    std::filesystem::path path;   ///< 解析出来的绝对路径
};

MediaTarget resolve_media(const std::string& project_path, const std::string& rel);

/// 解析 Range 请求头。
///
/// **三种拒绝方式不是一回事，对拍抓出来的就是这个。** 原来的实现
/// 一律返回 nullopt、调用方一律回 416，而 Python（Starlette 的
/// FileResponse）分得很细：语法不认识回 400，起点越界才回 416。
/// 对浏览器来说这两个的含义不同——416 带着"文件真实长度是多少"，
/// 是"你要的范围不存在，照这个长度重来"；400 是"你的请求本身是坏的"。
///
/// 下面这套判定**照抄 Starlette 的顺序**（starlette/responses.py 的
/// `_parse_range_header`）：先看单位，再逐段解析，空了回 400，
/// 起点越界回 416，起点不小于终点回 400。顺序换了结果就会变——
/// 比如 `bytes=5000-6000` 在越界和大小关系上都不合法，先查哪个
/// 决定了它回 416 还是 400。
struct ByteRange {
    std::uint64_t first = 0;
    std::uint64_t last = 0;  ///< 闭区间，含 last
};

enum class RangeVerdict {
    Ok,              ///< 解析出一个区间，回 206
    Ignore,          ///< 当没看见这个头，回整个文件 200
    Malformed,       ///< 语法不认识，回 400
    NotSatisfiable,  ///< 起点越界，回 416（要带上 `Content-Range: bytes */N`）
};

struct RangeParse {
    RangeVerdict verdict = RangeVerdict::Malformed;
    ByteRange range;
};

/// file_size 用来把开放式区间（`bytes=100-`、`bytes=-500`）算成具体范围。
///
/// **多区间（`bytes=0-99,200-299`）判 `Ignore`，这是一处有意的偏差。**
/// Python 那边回 206 + `multipart/byteranges`。我们不实现它：浏览器的
/// `<video>` 不会发多区间，而且那个格式的分隔串是随机生成的，
/// 两侧永远逐字节对不上，对拍也验不了。回整个文件是安全的降级
/// ——任何客户端都认，等同于服务端不支持分段。
RangeParse parse_range(const std::string& header, std::uint64_t file_size);

/// 按扩展名猜 Content-Type。
///
/// 不装 libmagic：项目里只会出现 mp4/png/jpg/webp/wav 这几种，
/// 而且猜错的后果是浏览器不播，不是安全问题。
std::string content_type_for(const std::filesystem::path& p);

}  // namespace changji::http
