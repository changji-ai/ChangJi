#pragma once

// 逐章展开正文。故事分三层里的最后一层。
//
//     梗概 → 故事大纲 → **章节正文**
//
// 补的是 AI 那条路上最后一个洞：大纲写出来的故事只有「这一章发生什么，
// 三五句」，没有正文。于是写剧本时展开的是梗概，而按字符切分那套机器
// （episode_text）一直返回空。粘贴进来的故事天生有正文，走不到这一步。
//
// 一次只写一章，每次带的是**压缩的全局记忆 + 上一章结尾**，不是前面所有
// 章的正文——那和一章接一章往下写的失忆是同一个道理，二十章的正文谁也塞不下。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"
#include "stages/story_outline.hpp"   // StoryError

namespace changji::stages {

/// 一个钩子：这一章里悬着的一个地方。最后那一个是章尾，也就是这一章停在哪。
struct DraftHook {
    std::string text;  ///< 这里悬着的是什么
    /// 这个位置前面那句原文，照抄十到二十个字。程序靠它在正文里定位。
    ///
    /// 让模型报字符偏移是行不通的——它数不准，报出来的数会落在别的段落上。
    /// 让它抄一句原文，程序自己去查，这是唯一可靠的定位手段。
    std::string after;
};

/// 模型写回来的一场戏。
///
/// **场是 2026-09-12 加的写作单位**，见 models::Scene 上那段注释：没有它
/// 的时候模型把整章梗概平摊成四十个一句话的段落，通篇是概述不是场景。
struct DraftScene {
    std::string where;
    std::string pov;
    std::string who;               ///< 这一场谁在场，至少两个人
    std::string goal;
    std::string obstacle;
    std::string worse;             ///< 这一场收场时局面比开场时更糟在哪儿
    std::string turn;              ///< 这一场结束时局面变成什么。就是这一场的收口
    std::vector<std::string> paragraphs; ///< 这一场的正文，一段一项
};

/// 模型写回来的一章。
struct ChapterDraft {
    std::string text;
    /// 这一章的场次，按先后。正文就是把它们的段落顺次拼起来的。
    std::vector<DraftScene> scenes;
    /// 这一章里所有悬着的地方，按先后。
    ///
    /// **场次表出现之后，这一栏只剩下兼容老形状的用处**：切点现在由场的
    /// 末尾直接给出（程序自己数得出位置），不再靠模型抄一句原文回来定位。
    /// 老草稿、老 schema 回来的 hooks 还认。
    std::vector<DraftHook> hooks;
};

/// 一道闸挑出来的一处毛病。`code` 是闸门代号（同 StoryError::code），
/// `what` 是那句话——给人看，也**原样给模型看**（见 build_chapter_revision_prompt），
/// 所以里面要说清是第几场、是哪句。
struct ChapterProblem {
    std::string code;
    std::string what;
};

/// 稿子被闸门打回，**但稿子还在手里**。
///
/// 2026-09-19 之前每一道闸都是"见着就抛"：抛出去的只有一句话，稿子随异常
/// 一起没了，上层能做的只剩把同一份提示词再发一遍——而重试只是再掷一次
/// 骰子（本文件里复读那段的原话）。闸门明明知道坏在第几场、是哪句，这件事
/// 从来没告诉过模型。
///
/// 现在**能改的**那些闸（字数、复读、情绪标签、没对白、两场撞车、点题、
/// 转述钩子、又停在上一章停过的地方）不各自抛，先攒成一份清单，整章看完
/// 再一起抛这个，解析好的稿子一并带上。上层拿它拼「上一稿 + 要改的地方」
/// 那份提示词（build_chapter_revision_prompt），让模型**改那几处**，不是
/// 整章重来。
///
/// 手里没稿的那几种（JSON 解析不动、正文是空的、自言自语摘不干净）照旧
/// 抛裸的 StoryError——没有稿子可改，只能重掷。
///
/// `what()` 是第一条毛病那句话（多于一条时带一句「另有 N 处」），
/// `code()` 是第一条的代号：账上（gates.jsonl）照旧按第一道闸归类，和攒
/// 清单之前记的是同一样东西——闸门的先后顺序没动。
class ChapterRejected : public StoryError {
public:
    ChapterRejected(ChapterDraft draft, std::vector<ChapterProblem> problems);

    const ChapterDraft& draft() const { return draft_; }
    const std::vector<ChapterProblem>& problems() const { return problems_; }

private:
    ChapterDraft draft_;
    std::vector<ChapterProblem> problems_;
};

/// 把解析好的稿子还原成模型要交的那个形状：scenes[] 里 who 是数组、每场
/// 最后一段拆回 last_line。改稿时贴给模型看的就是它——贴**清理过**的这份
/// 而不是它上次吐的原文，是为了让它在已经摘掉占位符、去过重的稿上改。
/// 老形状（没有场次表）退回 `{"text": …}`。
nlohmann::ordered_json draft_to_json(const ChapterDraft& d);

/// 拼「改稿」提示词：原来那份写章的提示词（规矩、底子、这一章要写什么）
/// 一字不少，后面接上一稿和毛病清单，要求只改点到的地方。
///
/// **不是另写一份短的。** 少了规矩和人物表，模型改着改着会把人名改了、
/// 把上一章的结尾再演一遍——那些本来就是靠提示词管着的。
std::string build_chapter_revision_prompt(const models::Story& story,
                                          const std::string& chapter_id,
                                          models::StyleLine style_line,
                                          const ChapterDraft& draft,
                                          const std::vector<ChapterProblem>& problems);

/// 章节正文在模型那份 JSON 里的字段名。
///
/// **不能各写各的。** 这个名字有两个读者：`chapter_schema()` 写进 schema，
/// 而流式那一层（`stages::JsonFieldStreamer`）要照着它从 token 流里把正文
/// 抠出来推给编辑器。2026-09-11 就栽在这儿——schema 从 `text` 改成
/// `paragraphs`，流式那边没跟着改，于是它一个字都抠不出来：编辑器整整
/// 一两分钟一动不动，而**后端不报任何错**（正文照样解析、落库、重算章节计划），
/// 查起来毫无线索。
///
/// 现在两边都用这一个常量，再配一条用例钉住它确实是 schema 里的键。
///
/// ⚠️ 2026-09-12 起这个键**在每一场里各有一个**（`scenes[].paragraphs`），
/// 不再是顶层唯一的那一个。流式那一层因此要开「一路收到底」那个开关
/// （`JsonFieldStreamer` 的 repeating），否则它收完第一场就 done()，
/// 编辑器里只看得到三分之一——而后端照样不报错。
inline constexpr const char* kChapterBodyField = "paragraphs";

/// 场次表在模型那份 JSON 里的字段名。
inline constexpr const char* kChapterScenesField = "scenes";

/// 一章的基准篇幅。
///
/// **一章是一个完整的故事单元。** 三千字是网文一章的常见体量，也和粘贴
/// 导入那边按字数切章用的 kImportTargetChars 对齐——AI 写的章和人粘进来
/// 的章该是同一个数量级，不然同一部电影里两种来源的章长得不一样。
inline constexpr int kChapterTargetChars = 3000;

/// 一章的篇幅下限，按「回落时长能消化多少字」的这么多倍算。
///
/// **名字是切段时代留下的**：那时一章要按时长切成好几集，这个 3 保证的是
/// 「在任何每集时长下一章都跨好几集」。切段 2026-09-18 拔掉之后它只剩一个
/// 作用——回落时长被调得很大时，一章的篇幅跟着抬上去，不至于被三千字那个
/// 打底数压回去。
inline constexpr int kEpisodesPerChapter = 3;

/// 这一章该写多长。
///
/// **章的篇幅由故事本身定**：三千字打底（kChapterTargetChars），回落时长
/// 调得大时按它抬上去（kEpisodesPerChapter），两个取大的。早先这里是反过
/// 来的，直接按「这一章值多长 × 一秒消化多少字」算——而那个时长在大纲阶段
/// 就是回落值，算出来永远只有最低那点量。端到端实跑时露的馅：四章写出来
/// 340/621/399/371 字。
int chapter_target_chars(const models::Story& story);

/// 一段平均多少字。**注意是平均数，不是中位数。**
///
/// 2026-09-11 量过起点主流长篇（《诡秘之主》第 2~6 章）：段长**中位** 33 字，
/// 八成以上的段只有一句话——但那是中位数，真实的一章里还夹着上百字的长段，
/// 平均数比中位数高一截。
///
/// 上一版拿 33 那个中位数当平均数用（写的是 35），段数 = 字数/35 进了语法，
/// 于是「每段三十五字」成了硬指标：模型只能一段一句、句句等长，写出来是
/// 字幕不是小说。取 45，段长才有长有短的余地。
inline constexpr int kCharsPerParagraph = 45;

/// 一场戏最少写多少字。
///
/// 一场要铺得开「谁想干什么、谁拦着、局面变成什么」，还要有对白往返和
/// 感官细节。低于这个数就又被压回概述了。
inline constexpr int kSceneMinChars = 600;

/// 一场戏最多写多少字。再长的话一场里就装进了好几件事，而分镜是一场一次
/// 拆镜的，那一次要吞下的东西跟着涨。
inline constexpr int kSceneMaxChars = 1500;

/// 一场戏该写多少字。**跟着回落时长走**（`story.episode_duration_s`）。
///
/// 这个数当年写死 1000，2026-09-12 实跑改掉：那时一章还要按时长切成好几
/// 段，一场 1000 字在 30 秒那一档里横跨快两段，刀只能落在场中间，「停在
/// 场尾」从 86% 掉到 63%。切段拔掉之后那个理由没了，**跟着时长走这件事
/// 留着**——它今天管的是一场戏的体量：时长定得长，一场就该写得开一点。
///
/// 夹在 600 和 1500 之间：短了写不成一场戏，长了一场里装进好几件事。
int scene_target_chars(const models::Story& story);

/// 这一章写成几场戏。**按字数算。**
///
/// 第一版是按「这一章要切成几段」算的（一场一段，刀口最整齐）。2026-09-12
/// 实跑当场露馅：30 秒那一档里一章要 4 场，而一章的梗概只撑得起一两件事
/// ——模型就从全局地点表里抓了下一章的地方来凑，四章都在同一个楼顶演同一
/// 件事。
///
/// 夹在 2 和 5 之间：少于两场就退回了上一版那种平摊；再多每场分不到一千
/// 字，又会被压回概述。
int chapter_target_scenes(const models::Story& story);

/// 一场戏写多少字。
int chapter_scene_chars(const models::Story& story);

/// 一场戏写多少段。从每场字数算，进 schema 卡住。
int chapter_scene_paras(const models::Story& story);

// ⚠️ **这套 schema 是按人际戏剧的语汇搭的，换个类型会露馅。**
//
// 2026-09-12 拿一句规则怪谈的梗概跑了三遍（当年那个题材阅读量同比涨
// 300%、占番茄巅峰榜 TOP50 四成），对照都市情感那句跑出来的数：
//
//                   都市情感        规则怪谈
//   停在场尾        100(无浮动)     100(无浮动)
//   空章/撞车/分镜话 全 0            全 0
//   对白中位        38%             20%
//
// **结构那几项全泛化**——停在场尾、零空章、零撞车是这套 schema 的性质，
// 不是那句梗概的性质。**但类型一点没泛化**：三个钩子都是「有人走开」
// （加快脚步走出去 / 脚步声渐远 / 身影融入夜色），通篇没有规则、没有
// 威胁、没有「第七条写了什么」，模型把悬疑设定写成了都市情感的腔调。
//
// 根子在这儿：goal / obstacle / worse / turn / fear / voice **全是关系向
// 的词**，没有一处放得下「规则、威胁、信息解谜」。对白掉一半也同源——
// 规则怪谈里人少、独处多，而 who 硬要求每场两个人，模型照填了两个名字，
// 戏却是一个人面对规则，对白自然写不起来。
//
// 要支持这类题材，缺的不是再加一个字段，是**按类型换一套场的字段**。
// 这件事没做，先记在这儿。

/// 写正文用多低的温度。
///
/// **默认 0.7 太散。** 2026-09-12 把同一份代码连跑两组三遍，四章里对白
/// 最低那一章的比例，一组是 21%（19~27），另一组是 1%（0~32）——同样的
/// 提示词、同样的 schema，一章能写成 35% 也能写成 0%。生成时间也在
/// 290~795 秒之间摆。那种 0% 的章拍出来就是一章默片，对成片是坏掉的
/// 交付物，不是「这一版没那么灵」。
///
/// 0.5 是拿稳定性换一点发挥。这一步的产出是**给下一步用的原料**（章节计划、
/// 剧本、分镜都照着它走），原料的下限比它的上限要紧。
///
/// ⚠️ **这个数是 2026-09-14 才第一次真正生效的，而它是照本地 14B 调的。**
/// 在那之前 llm::build_payload 发的一直是 cfg.temperature，`req.temperature`
/// 没人读——也就是说上面那两组实测是在**本地那条路**上做的，远端从头到尾
/// 跑的都是配置里的 0.7。接上线之后这个数开始压到云端模型头上，而云端那家
/// （智谱 GLM）从没按这个数测过。
///
/// 所以：**它现在是一个没验过的旋钮，不是一个验过的结论。** 用户抱怨
/// "每次写出来的都差不多"时，这一条是嫌疑之一——0.5 换来的稳定，在一个
/// 本来就比 14B 稳的模型上可能只剩下"稳"。要调就先把模型钉住再改，
/// 别和提示词的改动混在一天里（上一次那个教训见本文件 chapter_write 那段）。
inline constexpr double kChapterTemperature = 0.5;

/// 请求里带的 JSON Schema。
///
/// **形状是 scenes[]，每一场自己带 paragraphs**。2026-09-11 的教训是措辞它
/// 不听，形状它才听：正文还是一个 text 字符串时，14B 三章里两章把梗概原样
/// 抄进去就收工；拆成一场一项之后就没这回事了。
///
/// ⚠️ **但「它没得选」这句今天不成立了。** 那时候本地后端把 schema 转成
/// GBNF，下限上限是解码期硬的；那条路 d87590e 删了，而远端**从来没走过**
/// `response_format`（见 client.cpp:294）。今天整份 schema 是以文字贴在提示
/// 词后面的，minItems / minLength 只是写在那儿的数——真正拦得住的是回来之后
/// `checked_output` 那道松校验和解析时的闸门。详见 chapter_write.cpp 里
/// `chapter_schema` 上面那段 ⚠️⚠️。
///
/// 每一场的 where/pov/goal/obstacle 排在 paragraphs **前面**是有意的：
/// 模型是顺着往下生成的，先把这一场在哪、跟谁走、要什么、谁拦着填掉，
/// 后面那几百字才有地方落。turn 也排在前面——先知道这一场停在哪，才写得到
/// 那儿去（「先想好在哪儿断，再写到那儿」）。
nlohmann::ordered_json chapter_schema(int target_scenes, int paras_per_scene);

/// 拼提示词。chapter_id 不存在时抛。
std::string build_chapter_prompt(const models::Story& story,
                                 const std::string& chapter_id,
                                 models::StyleLine style_line);

/// 正文至少要有目标篇幅的这么多，否则算模型没写。
///
/// 五分之一给得很松——**要拦的是"根本没写"，不是"写短了"**。
/// 2026-09-11 实跑时模型把章标题填进了正文字段，四章各写出 1~2 个字，
/// 而这些**被静默存了下来**：故事看着有四章，章节计划也照样排出四条，
/// 到写剧本那一步才会发现无米下锅。和剧本那边「整章一句台词都没有」
/// 是同一类闸门——宁可报错重来，不要留一份看着正常的空壳。
inline constexpr double kChapterMinRatio = 0.2;

/// 这两句话是不是在说同一件事。
///
/// 两条任一成立：去标点之后互相包含、十个字以上连着一样。
///
/// 「换个说法的同一件事」抓不到——量过了，用词重合度在真撞车和真不同
/// 之间分不开（0.30 对 0.27），那个信号在语义里不在字面里。
///
/// 场的 turn 就是这一场的收口，两场停在同一件事上，观众看到的是剧情在
/// 原地打转。
/// 导出来是因为「这一章和上一章停在同一件事上」那道闸在 apply_chapter 里，
/// 而它和 parse_chapter 里章内那道用的必须是同一把尺子。
bool scenes_repeat_beat(const std::string& a, const std::string& b);

/// 解析模型返回。
///
/// `min_chars` 给 0 表示不查长度（拼提示词的单测用得着）。
///
/// `strict` = 软闸开不开。**闸分两档，这是 2026-09-12 用三轮实跑换来的：**
///
/// - **硬闸**（没写出正文、复读、模型的自言自语、英文占位符、短得离谱）
///   任何时候都拦。它们拦下来的东西**没法用**——落库就是一章垃圾。
/// - **软闸**（整章没对白、情绪标签成串、两场停在同一件事、章尾点题）
///   只在还有下一次机会时拦。它们拦下来的东西是**能用但不够好**。
///
/// 分档的理由很实在：软闸一律硬拦的话，14B 连着两次写不对，那一章就是
/// **0 字**——而 0 字比「写得一般」差得多。三轮实跑里软闸每轮清掉 1~3 章，
/// 把想提的分自己打没了。最后一次尝试放它过去，软闸就只会提分，不会清零。
///
/// ---- 闸门代号 ----
///
/// 每一道闸打回时都给 `StoryError` 带一个代号，落进 `gates.jsonl` 那一列
/// （见 `llm::GateVerdict`）。**加新闸门就得给它一个代号**：漏了的话那一次
/// 打回在账上归进空代号那一桶，「哪条闸门最费钱」少数一笔，而没有任何报错。
///
///   bad_json            JSON 解析不动 / 返回的不是对象
///   no_body             正文字段是空的
///   too_short           字数不到下限（模型把标题填进了正文）
///   model_aside         正文里混进模型的自言自语，且摘不干净
///   repetition          正文在复读
///   emotion_label       情绪标签成串（软）
///   no_dialogue         整章几乎没有对白（软）
///   scene_turn_dup      两场停在同一件事上（软）
///   turn_replayed       这一场的 turn 是上一场已经写过的那句话（软）
///   para_dup            有一段原样写了两遍
///   hearsay_hook        章尾只说「说了一句话」，没说是哪句（软）
///   on_the_nose         有一场的最后一段在点题（软）
///   turn_repeats_prev   这一章又停在上一章停过的地方（在 apply_chapter 里）
///
/// ---- 抛的是哪一种 ----
///
/// bad_json / no_body / model_aside 抛裸的 `StoryError`：手里没有能改的稿。
/// 其余全部**攒成清单**，整章看完再抛一个 `ChapterRejected`，稿子一并带着——
/// 上层拿它去改稿而不是重掷。攒清单只改了"什么时候抛"，没改"先看哪一道"：
/// 第一条毛病仍然是原来会被抛出来的那一道，`code()` 也是它的。
ChapterDraft parse_chapter(const std::string& raw, int min_chars = 0,
                           bool strict = true);

/// 把写好的一章并回故事里。
///
/// **只动这一章。** 正文落进去之后钩子要重建：段落边界重新登记成候选位置
/// （原来那些是对着空正文算的，全都作废），再把**每一场的末尾**登记成有说法
/// 的位置——说法用的是**那一场的收尾那一句**，那一句空着才退回 turn
/// （为什么不用 turn，见 chapter_write.cpp 里那一段）。大纲里那一章的钩子
/// 说法留着挂在章尾。
///
/// 场的位置是**程序数出来的**：正文是把各场的段落顺次拼起来的，第几段结束
/// 就是第几场结束，一个字都不用模型报。上一版靠模型抄一句原文回来查位置
/// （DraftHook::after），抄错一个字那一场就落不到位置上。
models::Story apply_chapter(const models::Story& story,
                            const std::string& chapter_id,
                            const ChapterDraft& draft);

}  // namespace changji::stages
