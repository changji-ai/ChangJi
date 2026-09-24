#pragma once

// 下模型。一个后台线程，一次下一个文件。
//
// **为什么不在进程内自己发 HTTP。** 不是发不了——OpenSSL 已经静态编进来了，
// httplib 能发 https。是不想自己写那一套：43 GB 要多连接、要断点续传、
// 要断了重连，而 aria2c 这些年就是干这个的。所以这里拉起外部下载器，
// 自己盯着它的进度。
//
// **为什么优先 aria2c。** hf-mirror 单连接只有 1～2 MB/s，43 GB 那一档
// 要下六个小时；八连接能到 10 MB/s 以上。没有 aria2c 就退回 curl——
// Windows 10 以后系统自带 curl.exe，Linux 上基本也都有，
// 所以"一台机器上一个下载器都没有"是很罕见的情况，真碰上了就明说要装什么。
//
// **同时下几个，不是一个一个下。**（2026-09-12 改的，用户的原话：
// "将模型改成同时下载而不是一个一个下"。）
//
// 原来这儿写的是"瓶颈是带宽不是并发"。那句话只对一半：带宽跑满之后并发
// 确实没用，但**单连接常常跑不满**——hf-mirror 一条连接 1～2 MB/s，
// 而出口远不止。curl 是单连接，所以那一档并发几个就快几倍。
//
// 几路由 parallel_lanes 定：curl 4 路，aria2c 2 路（它自己已经开了 8 条
// 连接，再乘上去源站要限速）。`CHANGJI_DOWNLOAD_PARALLEL=1` 压回一个一个下。
//
// **代价**：界面上"现在在下什么"不再是一个文件，单个文件的进度也不再
// 单调（谁快谁慢看源站脸色）。所以每一项各有各的进度条，头上那行在
// 同时下多个时说的是"正在下 N 个文件"。
//
// ---
//
// **断点续传是硬要求，不是优化。** 43 GB 下到 90% 时网断一下就要重来的话，
// 这个页面就是不能用的。两个下载器都开着续传（aria2c `--continue`、
// curl `-C -`），而且每个文件下完都核字节数——ssh 掉线会把下载器一起杀掉，
// 留下一个截断的文件，那种文件加载时报的是"权重读不对"，
// 指向完全错误的方向。这一条是 `cpp/tools/fetch_h3.sh` 里栽出来的。

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "setup/catalog.hpp"

namespace changji::setup {

enum class ItemState {
    Pending,   ///< 还没轮到
    Running,   ///< 正在下
    Done,      ///< 下完了，字节数对得上
    Present,   ///< 盘上本来就有，跳过
    Failed,
    Canceled,
};

const char* to_string(ItemState v);

/// 一个文件的下载进度。
struct ItemProgress {
    std::string group;   ///< 属于哪一组（video / image / …）
    std::string option;  ///< 属于哪个选项
    std::string name;    ///< 清单里的名字（`FileSpec::name`），认"这是哪一个文件"用
    /// 落在哪（或者已经在哪），相对模型目录。新下的是 `<类型>/<文件名>`
    /// （见 setup::type_dir）；盘上本来就有的，是它实际所在的那个位置。
    std::string path;
    std::string note;
    std::uint64_t total = 0;
    std::uint64_t downloaded = 0;
    double speed_bps = 0.0;
    ItemState state = ItemState::Pending;
    std::string error;
};

enum class RunState { Idle, Running, Done, Failed, Canceled };

const char* to_string(RunState v);

/// 整体进度。**一次取一整份**，不要分几次读——分开读会拿到互相矛盾的
/// 两半（总数是新的、明细是旧的），界面上表现为进度条倒退。
struct Snapshot {
    RunState state = RunState::Idle;
    std::vector<ItemProgress> items;
    /// 整件事失败的原因。单个文件的错在 items 里。
    std::string error;
    /// 实际用的下载器，界面上要说出来——速度差十倍，用户有权知道。
    std::string tool;
    /// 从哪个源下的（`modelscope` / `huggingface` / `hf-mirror`）。
    /// **下载中途也要看得见**：下不动的时候，第一件要判断的事就是
    /// "是不是走错源了"。
    std::string source;
    std::string dir;
    std::uint64_t total = 0;
    std::uint64_t downloaded = 0;
    double speed_bps = 0.0;
    /// 还要多久，秒。算不出来（速度为 0）时是 -1。
    double eta_seconds = -1.0;

    nlohmann::json to_json() const;
};

/// 要下的一个文件，附带它属于哪一组、哪个选项。
struct Item {
    std::string group;
    std::string option;
    FileSpec file;
    /// 下到哪，相对模型目录（`setup::download_rel`）。空 = 就用 `file.name`。
    std::string rel;
    /// 算好的下载地址。**清单里存的是仓库和路径**，域名由当前的源决定
    /// （见 setup/source.hpp）——所以地址在派活的时候才拼出来，
    /// 不在清单里。
    std::string url;
};

/// 找一个能用的下载器，返回可执行文件名（`aria2c` / `curl`）。
/// 一个都没有返回空串。
std::string pick_tool();

/// 同时下几个。`tool` 是 pick_tool() 的返回值。
///
/// **导出只为了能测**：这个数错了的表现很温和——要么白白慢四倍（都排队），
/// 要么被源站限速（开太多），两种都不会报错，只会让人觉得"今天网不好"。
/// 规则和取舍写在实现那儿。
std::size_t parallel_lanes(std::size_t item_count, const std::string& tool);

/// aria2c 打在控制台上的那一行进度。
///
/// 形如 `[#396de5 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]`。
/// **抽出来是为了能测**——这段解析错了的表现是进度条一直停在 0，
/// 而下载本身是好的，很难往解析上想。
struct Aria2Progress {
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    double speed_bps = 0.0;
};
std::optional<Aria2Progress> parse_aria2_progress(const std::string& text);

/// 把 `3.7MiB` `844KiB` `2.3GiB` `512` 这类写法换算成字节。认不出返回空。
std::optional<std::uint64_t> parse_human_bytes(const std::string& text);

/// 后台下载器。**进程内一个**，见 instance()。
class Downloader {
public:
    static Downloader& instance();

    ~Downloader();

    /// 开工。正在跑的时候再调返回 false，不打断在跑的那一轮。
    ///
    /// `dir` 是模型目录，item 里的相对路径就相对它落盘。
    /// `on_item_done` 每下完一个文件调一次（在后台线程上），
    /// 用来把这一项写回配置——**不等整轮跑完再写**：下了三十分钟之后
    /// 关掉浏览器，已经下好的那几个也该是配好的。
    bool start(std::vector<Item> items, const std::filesystem::path& dir,
               const std::string& source,
               std::function<void(const Item&)> on_item_done);

    /// 停。已经下好的文件留着，下次接着下。
    void cancel();

    Snapshot snapshot() const;
    bool running() const;

private:
    Downloader() = default;

    void run(std::vector<Item> items, std::filesystem::path dir,
             std::function<void(const Item&)> on_item_done);
    /// 下一个文件，返回是否成功。进度直接写进 snap_.items[index]。
    /// `dir` 是模型目录：下之前先在整个目录里找一遍有没有现成的。
    bool fetch_one(const Item& item, const std::filesystem::path& dir,
                   const std::filesystem::path& dest, std::size_t index);

    mutable std::mutex mu_;
    Snapshot snap_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
};

/// 把目录里**已经下全、只是没来得及改名**的 `<名字>.part` 摆到正式名字上，
/// 返回收编了几个。
///
/// 怎么会有这种文件：下载是 curl 子进程写 `.part`、父进程等它退出再核
/// 字节数改名；父进程（worker）中途被杀，`setsid` 派生的 curl 是孤儿、
/// 照样把文件写完，就没人改名了。2026-09-15 实测：Qwen_Image-Q8_0.gguf.part
/// 20 GB 一个字节不差地躺在盘上，这台却一直报"缺 image_base"，人得再点
/// 一次下载才收编。启动时扫一遍，就不用人再点。
///
/// 只认清单里有、字节数**恰好**对上的；多了少了都不碰（那是续传和
/// "写过头重下"的事，见 Downloader::fetch_one）。老位置（平铺在根上）和新位置
/// （`<类型>/`，见 setup::type_dir）两处都看。
std::size_t adopt_finished_parts(const std::filesystem::path& models_dir);

}  // namespace changji::setup
