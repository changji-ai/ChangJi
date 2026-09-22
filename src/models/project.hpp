#pragma once

// 项目模型与磁盘布局。
//
// 一个项目就是一个自包含的目录，换机器整个拷走即可。目录里只有相对路径，
// 不含任何绝对路径，也不含模型文件（那些跟机器走，不跟项目走）。
//
//     我的电影/
//     ├── project.json          项目元数据与分镜表
//     ├── assets.json           角色与场景资产库
//     ├── changji.toml          项目级配置覆盖（可选）
//     ├── refs/                 角色三视图、场景空景图
//     ├── audio/                配音
//     ├── frames/               逐镜首帧
//     ├── shots/
//     │   ├── draft/            草稿档视频
//     │   └── final/            成片档视频
//     ├── subtitles/
//     └── output/               成片
//
// 移植自 src/changji/models/project.py。
//
// ⚠️ 这个文件里所有 std::string 与 fs::path 的互转必须走
// paths::to_utf8 / paths::from_utf8。项目目录允许是 E:\AI电影\ 这种路径，
// 直接用 path.string() 或 fs::path(str) 会在 MSVC 上抛异常，见 verify/RESULTS.md。

#include <utility>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/json_compat.hpp"
#include "models/shot.hpp"
#include "models/story.hpp"

namespace changji::models {

inline constexpr const char* kProjectFile = "project.json";
inline constexpr const char* kAssetsFile = "assets.json";
inline constexpr int kSchemaVersion = 1;

/// 项目里要建出来的子目录。
const std::vector<std::string>& project_subdirs();

/// 一章。分镜表挂在这里。
///
/// **`Episode` / `episode_id` 是历史留下的名字**：老项目的 project.json 里
/// 写的就是它们，改名等于让老项目打不开。说法按章，标识符照旧。
struct Episode {
    std::string episode_id; ///< ^[a-z0-9_]+$
    std::string title;
    std::string synopsis;
    double target_duration_s = 180.0; ///< 目标时长，> 0
    std::string script;               ///< 剧本原文
    std::vector<Shot> shots;

    /// 它对着故事里的哪一章（story.json 里的 chapter_id）。**一章一条，
    /// 所以正常只有一个**；数组这个形状是按时长切章那个年代留下的，老项目
    /// 里可能真有两个，读的那几处一律取 `front()`。
    ///
    /// 空表示它不是从故事来的——老项目、手动加的、预告片都是这样，它们
    /// 照常走老路径。**确切的字符区间不存在这里**，在 story.plan 里按
    /// episode_id 查；存两份迟早对不上，而对不上的表现是剧本写了隔壁章的
    /// 内容。
    std::vector<std::string> chapter_refs;

    /// 这张分镜表是照**哪一版剧本**拆出来的（拿 `chapter_text_fingerprint`
    /// 算在 `script` 上）。没拆过、或者说不清的时候是空串。
    ///
    /// **2026-09-19 加的，因为剧本重写之后分镜一声不吭地留在原地。**
    /// 勾着「覆盖已有」点一次「理解故事」（或者在剧本页手改一稿），这一章
    /// 的剧本就换了，而写剧本那两条路都不带 regenerate——于是分镜还是照
    /// **老剧本**拆的，台词对不上、场次对不上，界面上一个字都没说。
    /// 补分镜那条判据问的是"有没有能用的镜头"（见 http/batch.cpp 的
    /// `shots_missing`），也照样报「没有要补的」。
    ///
    /// **这一栏只用来说话，不用来自动重拆。** 不自动重拆是有意的，理由写在
    /// http/episodes.hpp 的 post_script 上：重拆会覆盖整张表，手改过的台词、
    /// 调过的顺序、锁住的镜头全没。所以判据归判据，动手那一下仍然只有人按
    /// 「AI 重出分镜」才发生。
    ///
    /// 指纹算在 `script` 上而不是章正文上，两个理由：手改剧本（不动正文）
    /// 一样让分镜过期；而没挂章的那些章（老项目、手动加的、预告片）没有
    /// 正文可照，算在正文上的话它们永远说不清。
    std::string shots_from;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Episode, episode_id, title, synopsis, target_duration_s, script, shots,
        chapter_refs, shots_from)

    /// 这张分镜表是不是照**改之前**那版剧本拆的。
    ///
    /// **空串是"说不清"，不是"过期"。** 老项目的 project.json 里没有
    /// `shots_from` 这个键，读出来空串而剧本非空；把空串当"对不上"的话，
    /// 全项目每一章会在升级那一刻同时挂上牌子——这块牌子是给人看的，
    /// 一上来就人人有份等于没说。
    ///
    /// 所以：**非空且对不上才算过期**。老项目一律放过，等它下次真被拆过，
    /// 这一栏自然就填上了。
    bool shots_stale() const;

    /// 按 order 排序后的镜头。
    ///
    /// Python 用的是 sorted()，稳定排序——order 相同的镜头保持原有先后。
    /// 这里必须用 std::stable_sort，用 std::sort 在 order 有重复时
    /// 顺序会和 Python 不一致，而这个顺序决定最终成片的镜头次序。
    std::vector<Shot> sorted_shots() const;

    const Shot* shot_by_id(const std::string& shot_id) const;
    Shot* shot_by_id(const std::string& shot_id);

    double planned_duration_s() const;
    std::map<std::string, int> counts_by_status() const;

    /// 取处于某个阶段的镜头。断点续跑靠它。
    std::vector<Shot> shots_needing(ShotStatus status) const;

    std::vector<std::string> validate() const;
};

/// 这一章有正文吗——**挂着的那一章在故事里找得到，而且正文非空**。
///
/// 写成正面的条件（"有没有正文"），不是"是不是空章"：后者答不上"故事读
/// 不出来"这一种（story.json 缺了、坏了、这一章被删了），而那几种下面
/// 同样不该照着不存在的正文往下走。理由同 CLAUDE.md「别拿数量当判据」。
///
/// **收在这儿是因为有三处在问同一句话**：批量那三条路挑章
/// （http/batch.cpp）、单章写剧本挡在源头（http/scripting.cpp）、剧本页
/// 那块牌子（http/episodes.cpp 的 script context）。抄三份的话，改一处
/// 漏两处——2026-09-19 那个 bug 就是这么长出来的：「批量补分镜」当时自己
/// 抄了一份判据，另外两条加的东西它没跟上。
///
/// 没挂章的（老项目、手动加的章、预告片）一律 false：它们没有正文可照，
/// 问这句话本身就不成立，各调用点自己决定这种要不要放行。
bool chapter_written(const Story& story, const Episode& ep);

/// 一个项目。
struct Project {
    int schema_version = kSchemaVersion;
    std::string project_id; ///< ^[a-z0-9_-]+$（比镜头 id 多允许连字符）
    std::string title;
    StyleLine style_line = StyleLine::REALISTIC;
    /// 这部电影讲什么。写下一章时当提示词用。
    /// 不存的话，隔天想接着写第六章，得凭记忆把当初那句话重打一遍。
    std::string premise; ///< ≤2000 字
    std::string created_at;
    std::string updated_at;
    std::vector<Episode> episodes;

    /// 上一趟「读故事」读的是哪一版正文（`models::story_text_fingerprint`）。
    ///
    /// **2026-09-19 加的。** 读整本书那一趟原来的跳过判据是「人物表非空就
    /// 不读」，纯粹为省那几分钟。于是写完一章正文再点，新出场的人、新去过
    /// 的地方一个都进不了设定库。
    ///
    /// 这是「理解故事」三步里**唯一**一步不按"只补缺"算的（另外两步是
    /// 没剧本才写、没分镜才拆，见 http/batch.cpp 顶上那段）。用户当天
    /// 定的：新写一章就该重读一遍，那正是读这一步的"缺"。不勾时是只补
    /// 不顶，手改过的设定不会丢。
    ///
    /// 存在 project.json 而不是 story.json：story.json 那份每一趟读完都要
    /// 被模型的回答整个换掉（commit_story），这一栏是引擎自己的账，不该
    /// 跟着进出模型。
    std::string understood_from;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Project, schema_version, project_id, title, style_line, premise,
        created_at, updated_at, episodes, understood_from)

    const Episode* episode_by_id(const std::string& episode_id) const;
    Episode* episode_by_id(const std::string& episode_id);

    void touch();

    std::vector<std::string> validate() const;
};

/// 当前 UTC 时间的 ISO 8601 串。
///
/// 必须和 Python 的 datetime.now(timezone.utc).isoformat() 同格式，
/// 否则两个后端交替写同一个项目时 created_at/updated_at 的形状会变来变去。
/// 那个格式是 2026-09-07T12:34:56.123456+00:00——**微秒六位，时区写成
/// +00:00 而不是 Z**。
std::string utc_now_iso8601();

/// 项目目录布局。所有路径都由项目根推导，绝不写死。
class ProjectPaths {
public:
    explicit ProjectPaths(const std::filesystem::path& root);

    const std::filesystem::path& root() const { return root_; }

    /// 建出全部子目录。
    void ensure() const;

    std::filesystem::path project_file() const;
    std::filesystem::path assets_file() const;
    std::filesystem::path story_file() const;
    std::filesystem::path refs() const;
    /// 角色的参考音色片段。
    ///
    /// **不在 project_subdirs() 里**，是上传第一段音色时才建的——
    /// 那份清单被 project_expectations.json 钉着（老项目的目录结构是
    /// 契约的一部分），为一个可选功能改它不值当。
    std::filesystem::path voices() const;
    std::filesystem::path audio() const;
    std::filesystem::path frames() const;
    std::filesystem::path subtitles() const;
    std::filesystem::path output() const;
    std::filesystem::path logs() const;
    std::filesystem::path shots(const std::string& tier) const;

    /// 绝对路径转成相对项目根的路径。存进 JSON 的一律用这个。
    ///
    /// 用正斜杠，保证在 Windows 上存的项目拿到 Linux 上也能读。
    /// 路径不在项目内时抛异常——存绝对路径会破坏可移植性。
    std::string rel(const std::filesystem::path& p) const;

    /// 相对路径还原成绝对路径。
    std::filesystem::path abs(const std::string& rel_path) const;

private:
    std::filesystem::path root_;
};

/// 这个 id 是不是一章正片（`ep` 加纯数字）。
///
/// **预告片和正片同住 `Project::episodes`。** 于是「这部电影有几章」这种话
/// 一不留神就把预告数进去：一部只剪了预告的电影会显示成「1 章」。
/// 判据和 next_episode_id 里那段是同一条，提出来免得两处各写一遍。
bool is_regular_episode(const std::string& episode_id);

/// 项目的读写。写入用原子替换，避免中途断电留下半个文件。
class ProjectStore {
public:
    explicit ProjectStore(const std::filesystem::path& root);

    const ProjectPaths& paths() const { return paths_; }
    const std::filesystem::path& root() const { return paths_.root(); }

    bool exists() const;

    /// 目录已经是一个项目了。
    ///
    /// **单独一个类型，不是让调用方去认那句话。** `http/projects.cpp` 原来
    /// 拿 `msg.find("已经")` 判该回 409 还是 400——那句话一翻，德语用户新建
    /// 一个重名项目拿到的就是 400，而前端据 409 提示「换个名字」。靠给人看
    /// 的话回推结构，多语言下必错（同 `pipeline::LeftOut` 那处）。
    struct AlreadyAProject : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /// 在空目录里建一个新项目。目录已经是项目时抛 `AlreadyAProject`。
    static ProjectStore create(const std::filesystem::path& root,
                               const std::string& project_id,
                               const std::string& title = "",
                               StyleLine style_line = StyleLine::REALISTIC);

    Project load_project() const;
    AssetLibrary load_assets() const;

    /// 读故事。**文件不存在时返回空 Story，不抛。**
    ///
    /// 老项目没有这个文件，而它们要照样打得开、跑得动。调用方用
    /// Story::empty() 判断是不是该走老路径，别用「文件在不在」——
    /// 那会让每个调用点都去拼一次路径。
    Story load_story() const;

    /// 读还没采用的那份大纲。**没有就返回空 Story，不抛**（同 load_story）。
    ///
    /// **为什么草稿要落库。** AI 出一份大纲要三四十秒到一分多钟，而它
    /// 原来只活在浏览器的一个 ref 里——刷新一下、切个页面、换台机器看，
    /// 那一分钟就白花了，而且界面上连"刚才写了什么"都不剩。
    /// 用户 2026-09-13 报的就是这个："点击让 ai 写大纲，刷新后什么都没有了"。
    ///

    void save_project(Project& project) const;
    void save_assets(const AssetLibrary& assets) const;
    void save_story(const Story& story) const;

    /// 把外部文件复制进项目，返回相对路径。
    ///
    /// 参考图这类素材必须复制进来而不是引用原位置，
    /// 否则项目拷到别的机器就断链。
    std::string copy_into(const std::filesystem::path& src,
                          const std::string& subdir,
                          const std::string& name = "") const;

private:
    ProjectPaths paths_;
};

}  // namespace changji::models
