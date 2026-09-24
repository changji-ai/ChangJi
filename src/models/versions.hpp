#pragma once

// 一块内容是谁写的、被谁盖了、盖掉的那份留在哪。
//
// **为什么有这一层。** 2026-09-24 起写作按对话分道：同一部片子两条对话可以
// 同时在写（`pipeline::JobTable::start` 的 `lane`）。存盘一直是整个文件替换，
// 没有版本、没有备份——一条对话写好的第 3 章被另一条盖掉，就是没了，而且
// 谁都不知道。用户定的规矩：**被盖的那条对话要来问人：恢复我的、保留盖上来
// 的、还是合并**。
//
// 做法：存盘那一刻（`ProjectStore::save_*`，所有写都走这三处）比一遍盘上的旧
// 文件和要写的新内容，**按块**看哪几块变了：
//
//   · `outline`          大纲骨架（一句话故事、类型、每章的标题和梗概）
//   · `chapter/<章>`      一章正文
//   · `script/<章>`       一章剧本
//   · `board/<章>`        一章分镜（只认人会当成"分镜改了"的那几栏）
//
// 账记在 `<项目>/versions/owners.json`（每块：谁、指纹、什么时候）。一块被换掉、
// 而账上写着它是**另一条对话**写的、盘上那份就是它写的那份（指纹对得上）——
// 这就是"盖了别人"：旧的那份整块存到 `versions/<块>/<时刻>-<编号>.json`，记一条
// 覆盖（`versions/overwrites.json`），再告诉被盖的那条对话（`set_overwrite_sink`）。
//
// **人改的不问**：页面上的按钮、手改的稿子（`util::current_writer()` 没立），
// 照样记账、照样留底，但人怎么改是人自己的事。**出片不算作者**
// （`Writer::derived`）：它回填状态、锁时长、拆镜，改的不是谁的作品。
//
// **不在这儿问人。** 这一层只管认出来、存下来、报出去；问人、按人的决定写回
// 是对话那一层的事（`agent/`），单独检出引擎仓库时没有那一层，账照记、底照留。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::models {

class ProjectPaths;

/// 一次"盖了别人"。
struct Overwrite {
    std::string id;
    std::string unit;        ///< `chapter/ch03` 这种
    std::string owner_lane;  ///< 被盖掉的那一份是谁写的（`chat:<编号>`）
    std::string by_lane;     ///< 谁盖的
    std::string by_title;    ///< 盖的那一件是什么活
    std::string stash;       ///< 被盖掉那份，相对项目目录
    std::int64_t at = 0;     ///< 毫秒
    /// open：还没问 / 问了没答；restored / kept / merged：人拍了板。
    std::string state = "open";

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Overwrite, id, unit, owner_lane,
                                                by_lane, by_title, stash, at, state)
};

/// 存盘时认出来的"盖了别人"报给谁。`root` 是项目目录（UTF-8）。
///
/// **出了存盘锁才叫**，可以在里面慢慢写对话记录、起一轮对话。
/// 没装（单独检出的引擎）就只记在盘上。
using OverwriteSink =
    std::function<void(const std::string& root, const std::vector<Overwrite>&)>;
void set_overwrite_sink(OverwriteSink sink);

/// 这部片子记下来的覆盖。`only_open` 只要还没拍板的。
std::vector<Overwrite> list_overwrites(const std::filesystem::path& root,
                                       bool only_open = false);
std::optional<Overwrite> find_overwrite(const std::filesystem::path& root,
                                        const std::string& id);
/// 改一件覆盖的状态（restored / kept / merged）。找不到回 false。
bool settle_overwrite(const std::filesystem::path& root, const std::string& id,
                      const std::string& state);
/// 被盖掉的那一份（留底文件里的 `payload`）。读不出来抛。
nlohmann::json overwritten_payload(const std::filesystem::path& root,
                                   const Overwrite& o);

/// 一块内容**此刻**在盘上的样子（`payload` 同留底那份的形状）。没有就是 null。
nlohmann::json unit_payload_now(const std::filesystem::path& root,
                                const std::string& unit);

namespace detail {
/// 存盘时比一遍新旧（`file` 是 "story" / "project"）。**在存盘锁里调。**
/// 回这一次认出来的覆盖——调用方出了锁再交给 sink。
std::vector<Overwrite> note_overwrites(const ProjectPaths& paths,
                                       const std::string& file,
                                       const nlohmann::json& before,
                                       const nlohmann::json& after);
/// 交给 sink（没装就什么都不做）。
void report_overwrites(const std::string& root, const std::vector<Overwrite>& found);
}  // namespace detail

}  // namespace changji::models
