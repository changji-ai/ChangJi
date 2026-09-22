#pragma once

// 故事层。整条流水线的新源头。
//
// 原来的源头是「一句梗概 + 逐章续写」：给一句梗概加前三章原文，让模型自己
// 想这一章该发生什么。于是没有全局结构、写到第五章开始失忆（前三章之外的事
// 不在上下文里）、故事也永远没有终点、角色库是增量拼的。
// 详见 docs/故事优先重构方案.md 第一节。
//
// 现在是：先有完整故事（分章），人物关系和场景从故事里提，一章就是一个
// 拍摄单元。**章数是按体量算出来的，不是填的。**
//
// 这个文件只有数据和校验，不碰网络也不碰大模型。切分算法在
// stages/story_plan.hpp，那里也是纯函数。这么切是因为两者的验证方式不同：
// 数据结构靠往返序列化钉住，切分靠构造语料算出预期切点。
//
// ⚠️ 章节正文里的位置一律按 **UTF-8 字符**计，不是字节。中文一个字三字节，
// 按字节存切点的话，切线会落在一个汉字的中间，截出来的那段是非法 UTF-8——
// 它会一路流到提示词和字幕，最后表现成「整轨字幕不显示」这种离得很远的故障。

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/json_compat.hpp"

namespace changji::models {

inline constexpr const char* kStoryFile = "story.json";

/// 故事体量。用户给这个，不给章数。
///
/// 什么都不给的话模型写出来的故事长度是随机的：同一句梗概可能给一个三章就完的
/// 段子，也可能铺开二十章。所以体量这个决定躲不掉，只是不该用「我要写 N 章」
/// 来表达——让人先数好数目再动笔，正是原来那套的毛病。
enum class StoryScale { SHORT, MEDIUM, LONG };

NLOHMANN_JSON_SERIALIZE_ENUM(StoryScale, {
    {StoryScale::SHORT, "short"},
    {StoryScale::MEDIUM, "medium"},
    {StoryScale::LONG, "long"},
})

const char* to_string(StoryScale v);

/// 这个体量建议写多少章。给大纲提示词用。
int suggested_chapters(StoryScale scale);

/// 故事从哪来。三个入口，产物都是同一个 Story。
enum class StorySource { AI, PASTED, KEYWORDS };

NLOHMANN_JSON_SERIALIZE_ENUM(StorySource, {
    {StorySource::AI, "ai"},
    {StorySource::PASTED, "pasted"},
    {StorySource::KEYWORDS, "keywords"},
})

const char* to_string(StorySource v);

/// 故事里的一个人。
///
/// **这里没有任何描述长相的字段**，和分镜表是同一个道理：外观只存在于
/// 资产库（models/character.hpp），由程序机械拼接。这里只有剧作信息——
/// 他是谁、他要什么、他怎么变。给大纲和剧本用，不给出图用。
struct StoryCharacter {
    std::string name;     ///< 剧本里的称呼，全片一字不改
    std::string identity; ///< 一句话身份
    std::string want;     ///< 他要什么。没有欲望的人物推不动情节
    /// 他怕什么——怕被谁看见什么、怕失去什么、怕自己其实是什么样的人。
    ///
    /// **2026-09-12 加的。** 人物表原来只有「他要什么」，而**怕什么和要
    /// 什么一样重要**：人物的核心驱动力往往不是欲望而是恐惧，90% 的人设
    /// 翻车死于「全能感」——完美但无味。一个人怕什么，决定了他在场上躲
    /// 什么、哪句话不肯说、被戳到时为什么突然变脸，对白的潜台词全从这儿来。
    ///
    /// **光填进人物表不够**：写正文那一步也要拿到它，拿不到会怎样写在
    /// stages/chapter_write.cpp 拼人物块的那一段，这儿不再写一遍。
    std::string fear;
    std::string arc;      ///< 从什么变成什么
    /// 他说话什么样：长句还是短句、认不认错、生气时是提高声音还是不说话。
    ///
    /// **2026-09-12 加的，因为所有人说话都一个腔调。** 人物表里有身份、
    /// 欲望、弧光，唯独没有「怎么开口」——于是正文里每个人的台词都是同一
    /// 个人写的。对白是电影最主要的东西，人物立不立得住基本就看这个。
    ///
    /// 和 identity 一样**不写长相**：说话方式是听得见的，不是看得见的。
    std::string voice;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        StoryCharacter, name, identity, want, fear, arc, voice)
};

/// 人物关系的一条边。
///
/// 原来整套系统里**没有这个东西**——角色是一个个孤立的外观块，谁和谁什么关系
/// 只存在于剧本正文里，模型每章重新理解一遍，于是关系会漂。
struct Relation {
    std::string a;       ///< 人物名，对应 StoryCharacter::name
    std::string b;
    std::string kind;    ///< 前任、母女、上下级
    std::string tension; ///< 这段关系里绷着的是什么

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Relation, a, b, kind, tension)
};

/// 故事里的一个地方。同样不含画面细节——空间、光、色板在资产库那边。
struct StoryLocation {
    std::string name;
    std::string what; ///< 什么地方
    std::string when; ///< 什么时间、什么光

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(StoryLocation, name, what, when)
};

/// 章节里的一个钩子：正文走到这儿时，悬着的是什么。
///
/// **一章要停在钩子上。** 章尾得是悬念、反转或者情绪落点，人才肯接着看下
/// 一章；写到哪儿算哪儿的话，一章就停在半句话上。
struct Hook {
    /// 落在本章正文的第几个字符（UTF-8 字符数，不是字节）。
    /// 0 表示章首、正文长度表示章尾。
    ///
    /// **今天它只用来对位**：再读一遍正文（story_analyze、story_revise）时
    /// 按这个位置找回同一条钩子，找不到才新加。2026-09-18 之前按每集时长
    /// 切章的年代，它是切点——那条路拔掉了，位置的用途只剩对位。
    int at_char = 0;
    std::string text; ///< 这个钩子是什么，会成为这一章的钩子说明

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Hook, at_char, text)
};

/// 一章里的一场戏。**正文的写作单位，也是拆分镜的单位。**
///
/// 加这个之前，一章就是一堆段落。模型拿到「这一章发生 A、B、C」，只能把
/// 三件事平摊成四十个一句话的段落——每段推进一整个事件，叙述时长远短于
/// 故事时间，叙事学上这叫**概述**。小说读起来是小说，靠的是**场景**：
/// 叙述时长约等于故事时间，一个动作一段、一句对白一段。实测那一章
/// （1674 字写完重逢、决裂、离开、回来、和好五件事）就是全篇概述的样子，
/// 用户的说法是「只能叫剧本不能叫小说」。
///
/// 有了场之后：一章挑两三件要紧的事，**一件一场**，实时地写；其余用一两句
/// 过渡带过去。场与场之间天然是一个收口，最后一场收在哪儿就是这一章停在
/// 哪儿——**不用再让模型抄原文来定位**。
struct Scene {
    /// 在本章正文里的区间 [from_char, to_char)，按 UTF-8 字符。
    int from_char = 0;
    int to_char = 0;

    std::string where;    ///< 在哪、什么时候、什么光
    std::string pov;      ///< 这一场跟谁走。一场只进一个人的心里，不跳
    /// 这一场谁在场。**至少两个人**——只有一个人的场写不出对白。
    ///
    /// 2026-09-12 加的：四章的对白比例一直在 17%~40% 之间大幅波动，低的
    /// 那几章都是一个人在场里看和想。电影那边管两个人的戏叫「对手戏」，
    /// 一个人听到刺激源并作出反应（或者不反应）就构成标准冲突；一个人
    /// 从头想到尾的场，拍出来就是一段默片。
    std::string who;
    std::string goal;     ///< 这一场里他想要什么
    std::string obstacle; ///< 谁、什么拦着
    /// 这一场收场时，局面比开场时更糟在哪儿。
    ///
    /// **2026-09-12 加的，因为一章三场原地打转。** 实跑出来的一章里，三场
    /// 都在同一个地方对着同一样东西，三个收尾是同一个手势的变奏（手指僵在
    /// 半空 / 手指在伞柄上方停住 / 指尖即将碰到又缩回），三场的钩子长得
    /// 一样。
    ///
    /// 编剧的老规矩是「通过事情的扭转，使情况比这场戏刚开始时更加恶劣」，
    /// 换个说法就是「每一场都要有信息增量」。**局面更糟这件事没法重复三遍**
    /// ——填得出来，场与场就自然往前走了。
    std::string worse;
    /// 这一场结束时局面变成什么。**它就是这一场的钩子**（最后一场那个就是
    /// 整章的钩子，见 stages::plan_episodes），所以不能是
    /// 「他们和好了」这种收束，要是一个悬着的新局面。
    std::string turn;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Scene, from_char, to_char, where, pov, who, goal, obstacle, worse, turn)
};

/// 一章。故事层的情节单元，同时是流水线的拍摄单元：一章出一个片段，多长
/// 由这一章自己的内容定；整部电影是最后把各章接起来（pipeline/film_join）。
///
/// 2026-09-18 之前这儿写的是「章不等于集：章是故事的单位，集是时长的单
/// 位」——那是为了「按每集时长拆分」那个诉求。产品定位改成电影制作平台
/// 之后，成片是一部完整的电影、一个文件，按时长切段整条拔掉，那个理由
/// 跟着没了。
struct Chapter {
    std::string chapter_id; ///< ^ch[0-9]+$
    std::string title;
    std::string summary;    ///< 三五句。大纲阶段就有，是压缩的全局记忆的一部分
    /// 这一章抖出来的那件新事——它推翻了前面谁的什么认知。
    ///
    /// **2026-09-12 加的，因为四章零反转。** 实跑出来的大纲是「前任回来 →
    /// 打电话 → 坦白 → 和解」：每章都在推进，但没有一章让人重新理解前面
    /// 发生过的事。卖座的电影每隔几章抖出一个身份/关系/事实/动机的反转，
    /// 网文那边叫「信息差」——读者或人物知道了一件之前不知道的事。
    ///
    /// 空着不算错（粘贴导入的故事、老项目都没有），只是那一章少了个劲。
    std::string reveal;
    /// 这一章埋下的、后面才回收的那样东西。
    ///
    /// **2026-09-12 加的，因为这条线从来没建模过。** 有 reveal（每章抖出
    /// 一件新事），但没有任何东西被**埋**下去——于是每一章的反转都是当场
    /// 冒出来的，观众没有「原来如此」那一下。电影的成法是「临近结尾把伏笔
    /// 全回收」「延迟回收，隔得越久炸得越响」，网文那边叫细节伏笔：
    /// 第一章那条项链，后面才揭示它是什么。
    ///
    /// 埋的是看得见的东西：一个物件、一句没头没尾的话、一个当时说不通的
    /// 细节。不是「暗示他有秘密」那种说明。
    std::string plant;
    std::string text;       ///< 正文。逐章展开之后才有，没展开时是空串
    std::vector<Hook> hooks;
    /// 这一章的场次。AI 展开正文之后才有；粘贴导入的故事没有（那边只能
    /// 按段落边界分）。空着不影响出片，只是这一章的钩子退回段落边界那一档
    ///（stages::paragraph_hooks）。
    std::vector<Scene> scenes;
    std::vector<std::string> characters; ///< 出场人物名
    std::vector<std::string> locations;  ///< 用到的地方名

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Chapter, chapter_id, title, summary, reveal, plant, text, hooks, scenes,
        characters, locations)

    /// 正文的字符数（UTF-8 字符，不是字节）。
    int text_len() const;
};

/// 章节计划的一条。一章覆盖故事的哪一段。**结构名 `EpisodePlan` 和
/// `episode_id` 是历史留下的名字**，老项目的 story.json 里就写着它们。
///
/// 区间是 [from, to)：`to_char` **不含**。这一条要写在注释里而不是靠人记，
/// 因为含不含差一个字，而那个字很可能是句号——截出来的那段就从半句话开始。
struct EpisodePlan {
    std::string episode_id;
    std::string title;
    std::string hook;                 ///< 这一章停在哪。说不出来时是空串
    double target_duration_s = 60.0;

    std::string from_chapter; ///< 起始章 id
    int from_char = 0;        ///< 起始章内的起始字符，含
    std::string to_chapter;   ///< 末章 id（这一条**触及**的最后一章）
    int to_char = 0;          ///< 末章内的结束字符，不含

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        EpisodePlan, episode_id, title, hook, target_duration_s,
        from_chapter, from_char, to_chapter, to_char)
};

/// 一部电影的故事。存在项目目录下的 story.json，和 project.json 平级。
///
/// 单独一个文件而不是塞进 project.json：正文全文可能十几万字，而
/// project.json 是每次改分镜状态都要原子重写一遍的——把十几万字绑在那条
/// 写路径上，每出一镜就多写一次全文。
struct Story {
    std::string premise; ///< 一两句。≤2000 字，和 Project::premise 同源
    StoryScale scale = StoryScale::MEDIUM;
    StorySource source = StorySource::AI;

    std::string logline;
    std::string genre;
    std::string tone;

    std::vector<StoryCharacter> characters;
    std::vector<Relation> relations;
    std::vector<StoryLocation> locations;
    std::vector<Chapter> chapters;

    /// 一章按多长写、多长拍。**字段名是历史留下的**，老项目的 story.json
    /// 里就叫 `episode_duration_s`。
    ///
    /// 两个用处：写正文时按它定一章的篇幅（stages::prose_budget_chars），
    /// 以及章还没有正文时估它值多长（stages::plan_episodes 的回落）。
    /// 有正文之后时长按字数估，轮不到它。
    double episode_duration_s = 60.0;
    /// 章节计划。plan_episodes() 的产物，一章一条，人可以改。
    std::vector<EpisodePlan> plan;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Story, premise, scale, source, logline, genre, tone,
        characters, relations, locations, chapters,
        episode_duration_s, plan)

    /// 有没有东西。story.json 不存在时读出来的是个空 Story，
    /// 调用方靠这个判断「这个项目还没走新流程」，走老路径。
    bool empty() const;

    const Chapter* chapter_by_id(const std::string& chapter_id) const;
    Chapter* chapter_by_id(const std::string& chapter_id);

    /// 删一章的账：章节计划里丢了几条、挪了几条。
    struct ChapterRemoval {
        bool removed = false;
        int plan_dropped = 0; ///< 整条落在这一章里的条目，跟着没了
        int plan_moved = 0;   ///< 跨着这一章的条目，起止收缩到还在的章上
    };

    /// 删一章，**顺手把章节计划改对**。
    ///
    /// 不能只从 chapters 里抹掉：计划每一条记的是 from_chapter → to_chapter，
    /// 删了中间一章，跨着它的那一条就指着一个不存在的 id——不报错，落成章节
    /// 时才炸。所以：只覆盖这一章的条目一起删；起点在这一章的，起点挪到
    /// 下一章开头；终点在这一章的，终点挪到上一章末尾。
    ///
    /// 一章一条之后新算出来的计划不会跨章，**老 story.json 里的还会**——
    /// 收缩那一支留着就是为了它们。
    ///
    /// 章节 id 不重排：id 只是 id，重排会把计划里剩下的引用全弄错。
    ChapterRemoval remove_chapter(const std::string& chapter_id);

    /// 展开了正文的章节数。大纲写完是 0，逐章展开时往上涨。
    int written_chapters() const;

    std::vector<std::string> validate() const;
};

/// 一章正文的指纹。**没正文是空串**，不是"空串的哈希"。
///
/// 干什么用的：让「照正文出来的那些东西」说得清自己是照**哪一版**出来的。
///
/// **它本身只是"一段文字的指纹"，不认得自己算的是不是章正文**，所以同一份
/// 规矩也拿去算别的：`Episode::shots_from` 算在**剧本**上（那张分镜表是照
/// 哪一版剧本拆的）。名字里的"正文"说的是它头一个用处，不是它的限制。
///
/// 空串得到空指纹，两处都指着这一条当"说不清"用，各自决定怎么办：
/// `shots_from` 空了一律放过（牌子是给人看的，人人有份等于没说），
/// `story_text_fingerprint` 空了就是"这本书一个字都还没写"。
///
/// 比的是 `strip_ws` 之后的那段字：编辑器落个尾空行不该算改过。
std::string chapter_text_fingerprint(const std::string& text);

/// 全书正文的指纹。**没正文的章不参与**。
///
/// 所以加一章空大纲章、删一章空章都不会让它变——「理解故事」读的是正文，
/// 空章里没有可读的东西，不值得为它重读一遍整本书（那是几分钟）。
/// 一旦哪一章写出正文、或者改了正文，它就变。
std::string story_text_fingerprint(const Story& story);

}  // namespace changji::models
