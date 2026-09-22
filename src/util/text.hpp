#pragma once

// 文本处理。都是从 Python 侧一比一搬过来的，每个都对应一处具体的
// 字符串操作——这些函数的输出会进提示词，而提示词要求逐字节一致，
// 所以这里没有"差不多就行"的余地。

#include <cstddef>
#include <string>
#include <vector>

namespace changji::text {

/// 剥掉尾部的空白和中英文标点，对应 Python 的 rstrip("。.；;，,、 ")。
///
/// 必须按 UTF-8 字符剥，不能按字节。中文标点每个 3 字节，
/// 按字节剥会把前一个汉字劈成半个，产出乱码——而这个串会出现在
/// 每一个镜头的提示词里，一旦坏掉是全片性的。
std::string rstrip_punct(std::string s);

/// 对应 Python 的 str.strip()：只剥 ASCII 空白，两端都剥。
std::string strip_ws(const std::string& s);

/// 对应 Python 的 re.sub(r"\s+", " ", s)：连续空白压成一个空格。
///
/// 只认 ASCII 空白。Python 的 \s 在 str 上默认也匹配 Unicode 空白
/// （比如全角空格 U+3000），这里不匹配——见 .cpp 里的说明。
std::string collapse_ws(const std::string& s);

/// 清洗模型给的字段：压空白、两端去空白、剥尾部标点。
///
/// 对应 bible.py 的 _clean()。尾部标点必须去掉，因为这些字段拼提示词时
/// 用逗号连接，模型带来的句号会让结果变成"冷静克制。，身姿笔挺。，"
/// 这样标点重复的串，而且这个串会出现在每一个镜头里。
std::string clean_field(const std::string& s);

/// 削掉标题自己带的那一层编号：「第二部：被折叠的时间」→「被折叠的时间」。
///
/// **2026-09-13 用全新项目走前三步时撞到的。** 模型出的大纲是：
///
///     ch01   第1章：不该存在的呼吸      ← 阿拉伯数字 + 「章」
///     ch02   第二部：被折叠的时间        ← 中文数字 + 「部」
///     ch03   第三部：纸上的谎言
///
/// 编号自己就不一致（章/部混用），而**界面上本来就有编号前缀**，于是显示成
///
///     故事页   第 2 章 · 第二部：被折叠的时间
///     章节页   2   第1章：不该存在的呼吸
///
/// 两个页面各错一种。StoryView 里有个守卫 `/^第.+章/`，但它只认「章」，
/// 认不出「部」；章节页压根没有。**清洗归引擎**——同一条规则不该在两个
/// 前端各写一遍，写了也总有一处漏。
///
/// **必须有明确的分隔符才削**（`：` `:` `、` `.` 空格），或者削完为空。
/// 「部」在中文里也是量词，「第二部手机」不带分隔符，削了就成了「手机」。
/// 认的单位：章、部、回、节、篇、集。
std::string strip_leading_ordinal(const std::string& s);

/// UTF-8 字符数（不是字节数）。
///
/// 靠"续接字节高两位是 10"来数，不解码码点。中文一个字三字节，
/// 用 s.size() 当字数会让所有长度判断偏大三倍。
std::size_t utf8_len(const std::string& s);

/// 这串字节是不是合法的 UTF-8。
///
/// **为什么要它。** nlohmann 解析 JSON 时**不校验** UTF-8（照单全收），
/// 但 `dump()` 时校验——于是一段 GBK 的中文进来之后一路无事，直到把它
/// 回显进某个响应里才抛 `type_error.316`，而那时候已经出了 try 块，
/// 用户拿到的是一个空白的 500。
///
/// 2026-09-12 实撞：往工作进程派一个带中文台词的配音任务，请求体是 GBK，
/// 工作进程回 500，日志里只有一行 "invalid UTF-8 byte at index 174"。
/// 跨机时两头编码不一样是迟早的事，所以在入口处就要判掉。
bool is_valid_utf8(const std::string& s);

/// 把非法字节换成 `?`，让这串一定塞得进 JSON。
///
/// **专治"错误消息本身炸掉"**：nlohmann 的解析错误里会带上出错位置附近
/// 的原始字节，把它拼进 `{"detail": …}` 再 dump，就在报错的路上又抛一次
/// ——而第二次没人接，用户拿到的是一个空白的 500。
std::string sanitize_utf8(const std::string& s);

/// 一个 UTF-8 字符占几字节，从起始字节判断。非法起始字节按 1 算。
std::size_t utf8_char_len(unsigned char lead);

/// 把 UTF-8 串切成一个个**字符**（不是字节）。
///
/// 断行、估时长、按字数硬切，全部要按字符算。按字节算会把一个中文字
/// 劈成三段，产出非法 UTF-8——那个串一路往下走，最后表现成
/// "字幕整轨不显示"或者"nlohmann 序列化时抛异常"。
std::vector<std::string> utf8_chars(const std::string& s);

/// 除第一行外，每一行前面加上 pad。
///
/// 给体检报告排版用：第一行接在项目名后面，后续行要缩进到同一列。
/// 原来只有 `fix` 那一段做了这件事，`detail` 是原样打出去的——
/// 而"出图后端"那一项的 detail 是**多行的**（sd.cpp 的 System Info
/// 带着一整行 CPU 特性），于是它顶格贴在报告中间，把整张表的对齐冲掉了。
/// 那是用户看到的第一屏。
std::string indent_rest(const std::string& s, const std::string& pad);

/// 一个 UTF-8 字符的码点。不是合法字符时返回 0。
char32_t utf8_codepoint(const std::string& ch);

/// 按 UTF-8 字符截断到最多 n 个字符。
///
/// 不能直接 substr：中文一个字 3 字节，按字节截会把最后一个字劈成半个，
/// 产出非法 UTF-8。这个串会进错误消息，而错误消息要序列化成 JSON——
/// nlohmann 遇到非法 UTF-8 会抛异常，于是"报错"这件事本身又炸一次。
std::string truncate_utf8(const std::string& s, std::size_t n);

/// SHA-1，返回小写十六进制。
///
/// 只为 slug() 的退路存在。不引第三方库是因为整个项目就这一处用到摘要，
/// 为它拉一个 OpenSSL 或者 cryptopp 不划算——交叉编译到树莓派时
/// 那些库的配置是另一套要维护的东西。
std::string sha1_hex(const std::string& data);

/// SHA-1，返回**原始 20 字节**。
///
/// WebSocket 握手要的是这个：Sec-WebSocket-Accept 是
/// base64(sha1(key + 固定 GUID))，中间不经过十六进制。
/// 拿 sha1_hex 的输出去 base64 会得到一个 56 字符的串，
/// 服务端算出来的是 28 字符——握手失败，而报错只是"连不上"。
std::string sha1_raw(const std::string& data);

/// 标准 base64（带 = 补齐，不是 URL 变体）。
std::string base64_encode(const std::string& data);

/// 目录名转成合法的项目 id，对应 web/server.py 的 _slugify()。
///
/// **和下面那个 slug() 规则不一样**，别合并：分隔符是连字符不是下划线，
/// 退路的前缀是 "p-" 且取 8 位摘要不是 6 位。两处产生的 id 进的是不同的
/// 字段（项目 id vs 角色 id），合并会让其中一边的 id 悄悄变形，
/// 而 id 变形意味着老项目打不开。
std::string project_slug(const std::string& name);

/// 转成合法的 id 片段，对应 bible.py 的 _slug()。
///
/// 规则：小写，非 [a-z0-9_] 的连续片段换成一个下划线，两端去下划线。
/// 结果为空时用 "x" 加内容 SHA-1 的前 6 位——中文名会走到这条退路。
std::string slug(const std::string& text);

}  // namespace changji::text
