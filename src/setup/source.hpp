#pragma once

// 从哪儿下模型。
//
// **国内从魔搭下，国外从 HuggingFace 下。** 用户 2026-09-10 定的。
//
// 这件事必须做，不是优化：这台国内机器上实测 `huggingface.co` 直连
// **超时**（curl 返回码 000，等了 4.2 秒），而魔搭 302 只要 0.28 秒。
// 只挂 HuggingFace 的话，国内用户点下载之后看到的是速度几十 KB/s 然后
// 失败——看着像"这个程序坏了"，而不是"这条网络走不通"。
//
// ---
//
// **三个源的仓库名、文件路径、字节数完全一致**（2026-09-10 逐个核过
// 十一个仓库）。所以清单里存的是 `仓库 + 路径 + 字节数`，换源只换域名。
// 这一条是有前提的：**加新条目时必须两个源都核一遍**。哪天某个源上的
// 文件是另一个 revision，字节数就对不上了，而下载完成的判据正是字节数——
// 表现会是"下完了却报大小不对，重试三次全失败"。
//
// 加源的话除了这里，还要在 `resolve_url` 里补一条，并且把清单里每个仓库
// 在新源上核一遍。

#include <optional>
#include <string>

namespace changji::setup {

enum class Source {
    /// 探一下再定。默认值。
    Auto,
    /// 魔搭（modelscope.cn）。国内。
    ModelScope,
    /// HuggingFace 官方。国外。
    HuggingFace,
    /// HuggingFace 的国内镜像。**不参与自动挑选**，是手动的退路：
    /// 魔搭上万一缺某个文件时还有一条路可走。
    HfMirror,
};

const char* to_string(Source v);

/// 认字符串。认不出返回空——**不要悄悄退回默认值**：
/// 前端传了个拼错的源，静默换成别的会让人以为自己选的生效了。
std::optional<Source> source_from_string(const std::string& s);

/// 拼出下载地址。
///
/// `repo` 形如 `city96/Qwen-Image-gguf`，`path` 是仓库内的相对路径。
/// **抽出来是为了能测**：拼错了的表现是 404，而 404 在下载器眼里
/// 和"网络不通"长得一样。
///
/// 传 `Auto` 会当成 `ModelScope`——调用方本该先 `detect_source()`，
/// 但真漏了的话给一条能用的路比拼出个空串好。
std::string resolve_url(Source source, const std::string& repo,
                        const std::string& path);

/// 探这台机器该走哪个源。**进程内只探一次**，结果缓存。
///
/// 办法是拿 curl 对两边各发一个 HEAD，谁快用谁；都不通就用魔搭
/// （这个程序的用户主要在国内，而且魔搭没有墙这一层变数）。
/// 环境变量 `CHANGJI_MODEL_SOURCE` 顶掉探测，取值同 `to_string`。
///
/// 探不了（机器上没有 curl）也回魔搭，界面上会说明没探成。
Source detect_source();

/// 探测的结果连同它是怎么来的。界面上要说清楚——
/// 用户看到"正在从魔搭下"时，得能知道这是探出来的还是他自己选的。
struct SourceProbe {
    Source source = Source::ModelScope;
    /// `probed`（探出来的）/ `env`（环境变量顶的）/ `default`（探不了）
    std::string how = "default";
    /// 各个源的 HEAD 耗时（秒）。连不上是 -1。界面上原样显示。
    double modelscope_s = -1.0;
    double huggingface_s = -1.0;
};
/// 探一眼两个下载源哪个快。
///
/// **结果会缓存住**（默认 5 分钟）。这一步要向两个远端各发一次 HEAD，
/// 每次最多 6 秒——而 `/bff/setup/state` 每次都调它，前端的路由守卫又
/// **等着它才渲染**。于是刷新一次页面先白屏三到四秒，网络差的时候十几秒，
/// 用户报的"刷新页面白屏"就是这个。答案在一次会话里几乎不会变，没有理由
/// 每次都去问。
///
/// `force = true` 跳过缓存重新探（模型窗上让用户手动重试用）。
SourceProbe probe_sources(bool force = false);

}  // namespace changji::setup
