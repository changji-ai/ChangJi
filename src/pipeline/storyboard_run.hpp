#pragma once

// 一章的分镜怎么出：先切场，两场以上一场一次拆，一场就整章一次拆；
// 拆完补台词、查覆盖、推口型、重编号。
//
// 以前这一串在三个接口里各抄一份（POST /api/plan、单章页重出、批量补分镜）。
// 按场拆镜要改的正是这一串的中间那段——三处各改一遍迟早改漏一处，而漏掉
// 的那处表现是「这一章还是整章一次拆的」，不报错。所以收成一个。
//
// 它要一个 llm::Client，所以不放在 stages/（那儿只有纯函数）。

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "llm/client.hpp"
#include "models/character.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"

namespace changji::pipeline {

struct StoryboardRunOptions {
    std::string script;
    models::AssetLibrary assets;
    std::string episode_id;
    /// **只在剧本估不出秒数时兜底。** 镜头数由剧本的内容定，拆完也不压回
    /// 这个数——一章多长由它自己的内容定，整部电影是最后把各章接起来，
    /// 不在这儿也不在装配时切。
    ///
    /// 2026-09-16 之前这儿有个 `content_driven` 开关，关着的时候按一集
    /// 60 秒的配额拆、拆完再压回去，等于把「不够凑、超了压」从剧本挪到了
    /// 分镜。那条路（集模式）当天整个删了，开关跟着没了。
    double duration_s = 60.0;
    /// 模型的思考流，同 llm::Request::on_thinking。可空。
    std::function<void(const std::string&)> on_thinking;
    /// 「正在拆第 2/3 场：夜 · 内 · 天台」这类话往哪儿报。可空。
    std::function<void(const std::string&)> on_progress;
    /// **只看不发**：把每一场真正要发的那段字拼进 `peeked`，一个模型都不调。
    ///
    /// 用户 2026-09-17 要"复制提示词拿到别处去跑"。这一步是按场跑的，
    /// 一章三场就是三份不同的提示词——所以给的是三份，各带一个场次头，
    /// 不是只给第一份。
    bool peek = false;
    /// 在别处跑完粘回来的那几段原文，**一场一段，顺序和场次一致**。
    ///
    /// 非空时一个模型都不调，第 i 场就用 `pasted[i]`。下游一道不少：
    /// 补台词、查覆盖、重编号全照走，而且 `prev_tail` 那条
    /// 衔接也还在——只是它接的是粘回来的上一场。
    ///
    /// 数量对不上就报错，不猜：少一段的话后面几场会整体错位一场，
    /// 而错位出来的分镜表看着是合法的，没有任何报错。
    std::vector<std::string> pasted;
    /// **按场拆时，拆好的那几场先落在这个文件里**（2026-09-27）；下回拆同一章接着用。
    /// 空 = 不留（用例、只看不发、粘回来的那两条也一律不留）。三个接口都给
    /// `storyboard_parts_path(片子, 章号)`。
    ///
    /// 为什么要有：智谱 Coding · glm-5.3-flash 一场要想十几分钟，一章五场，第 4 场
    /// 连接断了——前三场一个镜头都没留下，project.json 里那一章还是 0 镜，一小时白跑。
    ///
    /// **不落进 project.json**，落在旁边一个草稿里：半章镜头一进 project.json，镜头墙、
    /// 接整部电影、前 n 分钟、项目页的计数全得学会认"这章还没拆完"，漏一处就把半章
    /// 当成拆好了；覆盖账也会把每一场都记成一次存盘。草稿谁都不读，整章拆完、查过
    /// 覆盖之后照旧由调用方一次写进 project.json，然后草稿删掉。
    ///
    /// 认不认上回那份看 `key`：剧本、章号、角色和场景的 id、单镜时长上限，任何一样
    /// 变了都当没有（按旧剧本拆的镜头接到新剧本上是错位的，而且不报错）。
    std::filesystem::path parts_file;
};

struct StoryboardRunResult {
    std::vector<models::Shot> shots;
    /// 引擎按剧本补进去的台词句数（见 stages::place_missing_dialogue）。
    int placed_lines = 0;
    /// 拆成了几场。1 = 整章一次拆（没有场次头）。
    int scenes = 1;
    /// 其中几场是接着用上回拆好的（见 `StoryboardRunOptions::parts_file`），没问模型。
    int reused_scenes = 0;
    /// `opts.peek` 为真时，每一场那段提示词拼起来的全文。别的时候是空串。
    std::string peeked;
};

/// 出一章的分镜。分镜表废了抛 stages::StoryboardError，模型那边的错抛
/// llm::LlmError，和以前三处各自抛的一样。
StoryboardRunResult run_storyboard(const StoryboardRunOptions& opts,
                                   llm::Client& client, CancelToken& tok);

/// 一章按场拆的中途草稿放哪儿：`<片子>/.changji/storyboard/<章号>.json`。
/// `.changji/` 是片子里引擎自己那一格（记忆、技能、扩展也在那儿），没有谁会把它当镜头读。
std::filesystem::path storyboard_parts_path(const std::filesystem::path& project_root,
                                            const std::string& episode_id);

}  // namespace changji::pipeline
