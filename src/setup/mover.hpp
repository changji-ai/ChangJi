#pragma once

// 换模型目录时把原来那儿的模型搬过去。
//
// **为什么要有。** 用户 2026-09-24：「换目录的时候如果有模型文件，询问是否把原来的
// 模型搬过去」。不搬的话，换完目录那几组当场变成"缺"——点一下下载就是几十 GB 重下，
// 而文件明明就在另一个目录里躺着。
//
// 两种搬法，**先试改名，不行再复制**：
//   · 同一块盘：`rename`，一瞬间，一个字节都不搬；
//   · 跨盘：复制到 `<目标>.moving`，核过字节数再改成正式名字，最后删掉原来那份。
//     中途停下或者出错，删掉那份 `.moving`，原来的一个字节不动。
//     **不用 `.part` 当后缀**：那个名字归下载器，`adopt_finished_parts` 会把大小
//     恰好对上的 `.part` 收编成模型——一份复制到一半的文件不该有这个机会。
//
// 搬什么：模型文件（`config::is_model_file`），和**下到一半的** `.part`（连同 aria2
// 的控制文件）——后者可能也有几十 GB，留在原地等于白下了。下载器的日志不搬。
//
// ⚠️ **正在被用的文件搬不动**（Windows 上加载着的模型不许改名、不许删）。开搬之前
// 调用方要先把没人用的模型卸掉（`infer::scheduler().evict_all()`）；还有人用着的，
// 这一个照实报「搬不动」，别的照搬——**不先复制再删**：删不掉的话盘上就成了两份。

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::setup {

/// 从哪儿搬到哪儿、要搬什么。开搬之前给界面问人用。
struct MovePlan {
    std::filesystem::path from;
    std::filesystem::path to;
    /// 相对 `from` 的路径，`/` 分隔。
    std::vector<std::string> files;
    std::uint64_t bytes = 0;
    /// 两个目录在同一块盘上：改名就行，一瞬间。不在的话要复制，按盘速算要几分钟到几十分钟。
    bool same_volume = false;
    /// 新目录那块盘还剩多少（新目录还不存在就往上找存在的那一级）。
    std::uint64_t free_bytes = 0;
    /// 原来那个目录太大、没数完。
    bool truncated = false;

    nlohmann::json to_json() const;
};

/// 数一遍原来那个目录里要搬的。`to` 在 `from` 里面时，`to` 底下的不算（那些已经在了）。
/// `from` 不存在回一份空的。
MovePlan plan_move(const std::filesystem::path& from, const std::filesystem::path& to);

/// 搬一个文件。成功回空串，否则回给人看的那句话。
///
/// `progress(已经搬了多少字节)` 在复制时每一段调一次，回 false 就停（删掉半截）。
/// 改名那条路不调它。`canceled` 置位表示是被叫停的，不是出错。
///
/// `force_copy`：不试改名，直接走跨盘那条（复制、核对、删原来那份）。**只给用例**——
/// 用例机器上凑不出两块盘，不这样那一条就永远测不到。
std::string move_one(const std::filesystem::path& src, const std::filesystem::path& dst,
                     const std::function<bool(std::uint64_t)>& progress, bool* canceled,
                     bool force_copy = false);

enum class MoveState { Idle, Running, Done, Failed, Canceled };

const char* to_string(MoveState v);

/// 搬到哪儿了。**一次取一整份**，理由同下载器的 Snapshot。
struct MoveSnapshot {
    MoveState state = MoveState::Idle;
    std::string from;
    std::string to;
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    std::size_t count = 0;
    std::size_t moved = 0;
    /// 正在搬的那一个（相对路径）。
    std::string current;
    double speed_bps = 0.0;
    /// 没搬过去的：`{path, error}`。
    std::vector<std::pair<std::string, std::string>> errors;
    /// 整件事的那句话（有几个没搬过去）。
    std::string error;

    nlohmann::json to_json() const;
};

/// 后台搬。**进程内一个**。
class Mover {
public:
    static Mover& instance();
    ~Mover();

    /// 开搬。已经在搬回 false。`on_done` 在搬完那一下调（后台线程上）。
    bool start(const std::filesystem::path& from, const std::filesystem::path& to,
               std::function<void()> on_done = {});
    void cancel();
    MoveSnapshot snapshot() const;
    bool running() const;

private:
    Mover() = default;
    void run(std::filesystem::path from, std::filesystem::path to,
             std::vector<std::string> files, std::function<void()> on_done);

    mutable std::mutex mu_;
    MoveSnapshot snap_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
};

}  // namespace changji::setup
