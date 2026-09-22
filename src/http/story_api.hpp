#pragma once

// 故事层的接口。整条流水线的新源头。
//
//   GET  /api/story          读故事（含章节计划）
//   POST /api/story          存可编辑的那几项：梗概、体量、每章时长
//   POST /api/story/outline  让大模型写大纲——**只回草稿，不落库**
//   POST /api/story/adopt    采用一份大纲，落库并算章节计划
//   POST /api/story/plan     按每章时长重算章节计划
//
// 写和采用分开，是照着剧本那边已经立住的规矩：源头没人审过就往下跑，
// 后面几十分钟的渲染全是白跑。所以 AI 写完先摆出来，点了采用才落库。

#include <exception>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"
#include "stages/chapter_write.hpp"

namespace changji::http {

/// GET /api/story?path=… —— 读故事。
///
/// 项目没有 story.json 时回一个空故事而**不是 404**：老项目本来就没有，
/// 而前端要靠「空不空」决定摆大纲页还是摆老的剧本页。404 会被当成出错。
ApiResult get_story(const std::string& path);

/// POST /api/story —— 存梗概、体量、每章时长。
///
/// 梗概会**同时写回 project.json**。老流程的写剧本提示词读的是
/// Project::premise，两边各存一份的话，在这一页改完梗概去写剧本
/// 用的还是旧的那句。
ApiResult post_story(const nlohmann::json& body);

/// POST /api/story/outline —— 大模型写大纲，只回草稿。
///
/// 带上 `stream`（WebSocket 的 job id）就**边写边推**：每 200 毫秒推一条
/// `outline_progress`，里面是"到此为止它写出来的那份"。再带上 `async`
/// 就当场回 202，结果走 job_done——理由和写一章一样，见 job_stream.hpp。
ApiResult post_story_outline(const nlohmann::json& body, llm::Client& client,
                             pipeline::CancelToken& tok);

/// 从**补齐过的半份大纲**里挑界面要显示的那几样（见 stages/json_partial）。
///
/// **导出只为了能测。** 这一层的错法很安静：半份 JSON 里任何字段都可能是
/// null（`"logline":` 刚写完还没开始写值），拿 `value(..., "")` 去取会在
/// null 上抛 type_error——而那会把整条生成搞挂，就为了推一帧进度。
nlohmann::json outline_progress_payload(const nlohmann::json& partial);

/// 写一份大纲并落库。**同步和异步两条路跑的是这同一段**，项目页那颗
/// 「一键成片」（http/oneclick.cpp）也从这儿走——三条路各写一份的话，
/// 记账、守卫、落库那几步迟早对不上。
///
/// `chapters` 非 0 = 就写这么多章（一键成片要 1）；0 = 按体量推。
nlohmann::json write_outline(models::ProjectStore& store,
                             const models::Project& project,
                             const models::Story& existing, std::string premise,
                             models::StoryScale scale,
                             const std::string& keywords,
                             const std::string& stream_id,
                             std::uint32_t variation, llm::Client& client,
                             pipeline::CancelToken& tok, bool peek = false,
                             const std::string& pasted = "",
                             int chapters = 0);

/// POST /api/story/adopt —— 采用一份大纲。
///
/// 整份替换，不做逐章合并：章节 id 是按位置生成的（ch01、ch02…），
/// 新大纲的 ch01 和旧大纲的 ch01 根本不是同一章，按 id 合并只会把
/// 两个故事拌在一起。已经展开过正文时要显式 overwrite，否则 409。
ApiResult post_story_adopt(const nlohmann::json& body);

/// POST /api/story/chapter/delete —— 删一章。body: {project, chapter_id}
///
/// 章节计划跟着改（见 Story::remove_chapter），回包里说改了几条：改过的话
/// 「落成章节」要重跑，界面上要把这句说出来。没有这个 id 是 404。
ApiResult post_story_chapter_delete(const nlohmann::json& body);

/// POST /api/story/plan —— 按每章时长重算章节计划。
ApiResult post_story_plan(const nlohmann::json& body);

/// POST /api/story/revise —— 改原稿的某一段。**只回草稿，不落库。**
///
/// body: {project, chapter_id, from_char, to_char, instruction, history?}
///
/// 故事那一页的重点是创作，而创作不是"按一次按钮生成一整章"——是选中一段
/// 觉得不对的字，说一句"这儿太赶了，铺一下"，看它改完，再决定要不要。
///
/// `history` 是之前的来回（{role, text} 的数组），给对话那条路用。空的就是
/// "选中一段直接说一句"。**两条路共用同一个提示词和同一条写回路径**：两套
/// 机制都能改正文的话，迟早出现"对话改的和选中改的对同一段各有一份"。
///
/// 不落库比大纲那边更要紧：大纲落错了重写一份就是，改稿落错了盖掉的是
/// 作者自己写的字。
ApiResult post_story_revise(const nlohmann::json& body, llm::Client& client,
                            pipeline::CancelToken& tok);

/// POST /api/story/revise/apply —— 把改好的那一段写回去。
///
/// body: {project, chapter_id, from_char, to_char, text}
///
/// **不碰大模型**：这一步是机械的字符串替换 + 重算候选切点 + 重算章节计划。
/// 分出来是因为它必须能单独测——写回去写错位置的话，盖掉的是作者的原文。
///
/// 有说法的钩子会跟着挪位置保住，落在被替换那一段里面的丢掉。
ApiResult post_story_revise_apply(const nlohmann::json& body);

/// POST /api/story/from_episodes —— 从已有章节反推一份故事骨架。
///
/// 给**老项目**用：它们手里只有一章章写好的剧本，没有 story.json，于是
/// 人物关系、全局记忆这些新做的东西一样都用不上。
///
/// **不碰大模型**，也**不重新切章**：一条章节记录就是整整一章，老项目那
/// 几章的边界一个字都不动——边界一挪，已经排好的分镜和出过的片就对不上它
/// 该在的那一段了。反推完顺手把每条 `Episode` 的 chapter_refs 接到对应的
/// 章上。
///
/// 回来的故事里人物、关系、地点都是空的，接着点「读现成正文提结构」
/// （/api/story/analyze）才有——那一步要模型读一遍内容。
ApiResult post_story_from_episodes(const nlohmann::json& body);

/// POST /api/story/import —— 把粘进来的一段文本切成章节。
///
/// 三个入口里的第二条：手里已经有小说/剧本，不必让模型再编一遍。
/// **不碰大模型**，切章节是机械活。和 /outline 一样**只回草稿不落库**。
///
/// 回来的故事只有章节和正文，人物、关系、地点都是空的——那些要读懂内容
/// 才提得出来，是下一步的事。
ApiResult post_story_import(const nlohmann::json& body);

/// POST /api/story/analyze —— 让大模型读一遍已经存下的正文，把结构提出来。
///
/// 粘贴导入只切章节，切完人物关系地点全是空的，走到「设定」那一步资产库
/// 还是空的。这一步补那个洞。
///
/// **正文一个字不动**，只补 summary、hook 和全片的人物/关系/地点。
/// 和别的几个一样**只回草稿不落库**。
ApiResult post_story_analyze(const nlohmann::json& body, llm::Client& client,
                             pipeline::CancelToken& tok);

/// POST /api/story/chapter —— 展开一章的正文。
///
/// **这一个是直接落库的，不回草稿。** 别的几个都遵守「AI 写完先摆出来，
/// 点了采用才落库」，这里破例，理由是两条：一、它只往一个空字段里填东西，
/// 没有什么会被顶掉；二、一部十六章的故事要逐章展开，走草稿-采用就是
/// 三十二次点击。已经有正文的章要显式 overwrite，否则 409。
///
/// 正文落进去之后钩子要重建、章节计划要重算——原来那些候选切点是对着空正文
/// 算出来的。
ApiResult post_story_chapter(const nlohmann::json& body, llm::Client& client,
                             pipeline::CancelToken& tok);

/// POST /api/story/episodes —— 把章节计划落成真的章节。
///
/// 章节计划是计划，章节是流水线真正在跑的东西。分成两步而不是采用大纲时
/// 顺手建出来，是因为重算章节计划（改每章时长）是个随手的动作，而建章节会
/// 动到已经写好剧本、已经出过片的那几章。
///
/// 已经存在的同号章节**只补元数据，绝不碰 script 和 shots**：改一次每章
/// 时长就把写好的剧本冲掉，那是没法接受的。章节计划里没有的老章节一律留着。
/// sync_episodes_to_chapters 的结果。落单的那几个只报不删。
struct EpisodeSync {
    std::vector<std::string> created;
    std::vector<std::string> updated;
    std::vector<std::string> orphans;

    bool touched() const { return !created.empty() || !updated.empty(); }
};

/// 把章节记录对齐到故事的章：一章一条。
///
/// **每一处改动章节的接口，存完 story 都要叫它。** 一章一条是机械映射，
/// 用户 2026-09-16 选的是「自动做，不要按钮」。
EpisodeSync sync_episodes_to_chapters(const models::ProjectStore& store,
                                      const models::Story& story);

ApiResult post_story_episodes(const nlohmann::json& body);

/// 理解故事那**一次**模型调用：读一遍，人物（含长相）、关系、地点（含样貌）、
/// 每章梗概和钩子、全片调子一次出来。结构那半落进 story.json（同 analyze），
/// 长相那半并进 assets.json（同 bible）。同一份回答喂两个解析器，名单天然
/// 一致。整件活（再加逐章写剧本）是 batch.hpp 的 post_story_understand。
/// body: {project, overwrite?}；带 peek 只回提示词，带 paste 收别处跑出来的。
ApiResult post_story_understand_once(const nlohmann::json& body,
                                     llm::Client& client,
                                     pipeline::CancelToken& tok);

/// 写过正文的故事被整份换掉之前挡一下（带 overwrite 才放）。
void refuse_to_clobber(const models::Story& existing, const nlohmann::json& body);

/// 把一份故事落成正式的那份：算章节计划、存盘、章节对齐、梗概同步。
/// 出大纲、粘一份、理解、从网上写——收尾都是它。
nlohmann::json commit_story(const models::ProjectStore& store, models::Project project,
                            models::Story story, const models::Story& existing);

/// 把正文闸门的一次裁决记进 `gates.jsonl`（见 `llm::GateVerdict`）。
/// `e` 传 nullptr = 这一次过了。
///
/// **两条路共用一个函数**：批量那条（batch.cpp，三轮）和单章这条（一轮）
/// 记的是同一件事。各写各的迟早分家，而分了家的表现是账上两条路的列不一样，
/// 统计那头 group by 出来少一半——没有任何报错。
///
/// **只有 `stages::StoryError` 算闸门。** 429、连不上、超时是对面砸了，
/// index.jsonl 那一行里已经写着；混进闸门账里的话「哪条闸门最费钱」第一名
/// 永远是限流，而那根本不是闸门。
///
/// `call_id` 空就整个不记——粘回来的稿子没有调用可以对，见 `llm::note_gate`。
/// `revision` = 这一轮是改稿不是重掷，落成 gates.jsonl 的那一列。
void note_chapter_gate(const std::string& call_id, const std::string& chapter_id,
                       int attempt, int attempts, bool strict,
                       const std::exception* e, bool revision = false);

/// 写一章的请求：提示词、schema、温度、思考流。
///
/// **批量和单章共用一份。** 各拼各的迟早分家——schema 那两个数（场数、
/// 每场段数）改了一边没跟上，两条路写出来的章就不一样长，而没有任何报错。
llm::Request chapter_request(const models::Story& story, const std::string& chapter_id,
                             models::StyleLine style);

/// 改稿的请求：同一份 schema、温度、思考流，提示词换成「上一稿 + 要改的
/// 地方」那份（stages::build_chapter_revision_prompt）。
///
/// 闸门打回之后先走它，不是把同一份提示词再发一遍——那只是再掷一次骰子。
llm::Request chapter_revision_request(const models::Story& story,
                                      const std::string& chapter_id,
                                      models::StyleLine style,
                                      const stages::ChapterRejected& rejected);

}  // namespace changji::http
