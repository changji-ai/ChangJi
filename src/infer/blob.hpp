#pragma once

// 按内容存的小仓库。派活到别的机器上时，输入文件走它。
//
// **为什么不直接传路径。** `Task.dest`、`start_image`、
// `prompts.reference_images` 现在都是路径，同机（多卡那条路）成立，
// 跨机一条都不成立——对面根本没有那个目录。
//
// **为什么按内容寻址，而不是"把文件传过去"。** 一章 22 镜，每镜三五张
// 参考图，而那几张在整章里是**同一批文件**。按路径传就是同一张脸传
// 二十二遍；按内容寻址，第二镜开始对面回一句"我有了"就完事。
//
// 指纹用 **SHA-1**，不是 SHA-256。理由是它已经在这个仓库里
// （`util/text.cpp`，ws 握手和种子都在用，测过），而这里的用途是
// **内容寻址 + 防截断**：认出"这两个文件是同一个"，以及"传过来的这份
// 没缺字节"。不是在防有人构造碰撞——这套东西的范围是自己的几台机器
// （见方案「对等互联」那一节开头）。哪天范围变了，这一条要重新判。
//
// **截断的文件是这条路上最阴的故障**：png 少几个字节照样"存下来了"，
// 加载时报的是"权重读不对"或者一张半截图，指向完全错误的方向。
// 所以收的时候先算指纹再落地，对不上就不落地。

#include <cstdint>
#include <filesystem>
#include <string>

namespace changji::infer {

/// 内容指纹长什么样：40 个小写十六进制字符。
///
/// **这是一道安全边界，不是格式洁癖。** 这个串会被拼进文件路径
/// （`POST /blob/<id>`），不查的话 `..%2f..%2fetc%2fpasswd` 就写到
/// 别处去了。`http/media.hpp` 那边的越界检查是同一类事，那儿也是
/// 手写的、也单独测。
bool blob_id_ok(const std::string& id);

/// 这个 blob 该躺在哪。`<cache>/blobs/ab/cdef…`
///
/// **前两位切一层子目录**：一个项目跑下来几千个 blob，全平铺在一个
/// 目录里，Windows 上光是 `ls` 就要好几秒。
///
/// id 不合法时回空路径——调用方必须先判空，不能拿它去拼。
std::filesystem::path blob_path(const std::filesystem::path& cache_root,
                                const std::string& id);

/// 算一个文件的内容指纹。读不了抛 `std::runtime_error`。
std::string blob_id_of(const std::filesystem::path& file);

/// 这个 blob 在不在。
bool blob_present(const std::filesystem::path& cache_root,
                  const std::string& id);

/// 收下一个 blob。**先核指纹再落地**，对不上不写。
///
/// 回空串表示存下了（或者本来就有）；否则是一句给人看的错误。
std::string blob_store(const std::filesystem::path& cache_root,
                       const std::string& id, const std::string& bytes);

/// 任务里的输入路径可以写成 `blob:<40 位指纹>`。
///
/// **为什么用前缀而不是加一组新字段。** 输入现在是一串路径
/// （`reference_images` 是数组、`start_image` 是可选项），给每一个都配一个
/// 平行的 blob 字段，两边就得一一对齐——而对不齐的表现是"这一镜用错了
/// 参考图"，不报错、看不出来。一个前缀把两种记法收在同一个字段里，
/// 对不齐这件事从根上没有了。
///
/// 认出来回指纹，不是这种记法回空串。
std::string blob_ref_id(const std::string& s);

/// 把任务里的一个输入解析成真实路径。
///
/// `blob:<id>` 去仓库里找，找不到抛——**这是对的**：派活方本该在派活
/// 之前把它传过来，没传就是链路错了，拿个空路径接着跑只会在深处炸。
/// 不是 `blob:` 记法的原样当路径用（同机那条路，一个字节都不用搬）。
std::filesystem::path resolve_input(const std::filesystem::path& cache_root,
                                    const std::string& spec);

/// blob 仓库和沙箱的根：`<项目库>/cache`。
std::filesystem::path cache_root_of(const std::filesystem::path& workspace);

/// 跑外来任务的沙箱：`<cache>/tasks/<task_id>`。
///
/// **外来任务绝不能写进项目目录**。理由不是防人（都是自己的机器），
/// 是防串项目：那台机器上有它自己的项目，而派活方给的 dest 是**它那边**
/// 的路径，照着写下去就是把别人的产物写进自己的项目里。
std::filesystem::path task_sandbox(const std::filesystem::path& cache_root,
                                   const std::string& task_id);

/// 把一个已有的文件塞进 blob 仓库，回它的指纹。
///
/// 派活那一头用它：算出指纹、问对面有没有、没有才传。
std::string blob_adopt(const std::filesystem::path& cache_root,
                       const std::filesystem::path& file);

}  // namespace changji::infer
