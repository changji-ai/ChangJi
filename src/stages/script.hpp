#pragma once

// 剧本、选题、预告片。
//
// 三件事共用一套解析——下游的分镜、配音、装配认的是渲染出来的那段文本，
// 不关心它是正片还是预告片。schema 有两份：正片是**四段的**（开场钩子、
// 冲突推进、情绪回报、章尾留扣，按秒排，每段自己的拍数地板），预告片是
// 平的一份。
//
// **四段是 2026-09-12 加的，因为 60 秒的一章写出了 13 秒的剧本。** 成片的
// 时长是剧本自己长出来的：四拍各占几秒、台词两三百字、分镜照着拍子
// 来。我们的剧本原来是一个扁平的 beats 数组，地板写死 minItems 4，60 秒
// 只是一句话交给后面的分镜模型——所以 60 → 13 → 6 每一步都合法地缩水。
// 时长的形状得先在剧本这一层立住。
//
// 和 bible/storyboard 一样，这里只有纯函数，不碰网络也不碰 llama.cpp。

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"

namespace changji::stages {

class ScriptError : public std::runtime_error {
public:
    explicit ScriptError(const std::string& what) : std::runtime_error(what) {}
};

// ---- 文本清洗 ----
//
// 这三个函数存在的原因是同一件事：**形式是我们定的，不是模型定的。**
// 模型爱给动作行加括号、给台词加引号、在开头挂个时间码，无论提示词里
// 怎么说。这些符号会一路流到分镜提示词和字幕里，所以在入口就削掉。

/// 削掉整段外面套的那一对括号或引号。
///
/// 只削套住整段的那一层，中间的括号是内容的一部分，不动。
std::string strip_wrapper(const std::string& text);

/// 削掉动作行开头的时间码标记，比如「[0-3秒] 画面特写：…」。
///
/// 提示词里没让它写，schema 里也没有这一项，模型自己加的。这段文字会原样
/// 进到分镜提示词里，让画面模型去理解一个时间码。
///
/// 只削开头那一对括号，而且要括号里确实像时间码才削：带数字，并且带
/// 秒、s 或者冒号。不做这个限制的话，「（他犹豫了）他开口」这种正常写法
/// 开头也会被削掉。
std::string strip_leading_timecode(const std::string& text);

/// 削掉动作行开头的机位标签，比如「镜头特写：病历单上的日期…」。
///
/// **2026-09-12 实跑撞上的。** 分镜是下一步的事（行业里那叫导演脚本），
/// 剧本里不该有景别和运镜；但模型会写。而这句话的祸不是多余——**渲染出来
/// 「镜头特写：…」和一句台词长得一模一样**，下游全靠冒号认说话人：
/// ScriptReader 会把它画成一个叫「镜头特写」的角色在说话，分镜模型和
/// 配音也照着当台词处理。
///
/// 只削确实像机位标签的那一层：冒号前八个字以内，而且带景别词。
/// 「牌子上写着：营业中」这种不动——它前面那截不是机位词。
std::string strip_camera_prefix(const std::string& text);

/// 整句被圆括号包住的台词，是**舞台提示**，不是要说出口的话。
///
/// **2026-09-13 实跑撞上的**（walk_c ep04，批量出分镜那一轮）：
///
///     ep04_sh005   （脚步声）
///     ep04_sh010   （旁白/环境音）
///
/// 两条都落在台词字段、char_id 为空，也就是会**用旁白音念出来**——观众听到
/// 「脚步声」「旁白斜杠环境音」，字幕上也照写。
///
/// 现有的 is_placeholder_line 管不住：它比对的是一张固定词表（无台词、none、
/// N/A…），而音效提示是开放集合，写多少条都补不全。
///
/// 规则要从结构上来。剧本格式里这叫 parenthetical（业内叫 "wrylie"）：
/// 台词块里的圆括号是给演员的提示——怎么说、做什么动作——**从来不念**。
/// 所以整句被圆括号包住 = 不是台词，一条规则盖住全部写法。
///
/// 只认圆括号 `（）` `()`。**不认 `【】`**：那是段头用的
/// （「【开场钩子 0–5 秒】」），认了会把段头当提示删掉。
bool is_stage_direction(const std::string& text);

/// 削掉开头漏出来的 markdown 列表符号。
///
/// **2026-09-13 实跑撞上的**（walk_c ep02）：模型写的台词是
///
///     林浩：-为什么要在一家普通餐厅下单？
///
/// 那个 `-` 是 markdown 的列表符号漏进了字符串字段。这个毛病有名字，叫
/// 「JSON bleed」——GBNF 约束的是 JSON 的**结构**，字段的**内容**照样会带
/// 训练数据里的格式痕迹。外层解析成功不等于字段值干净。
///
/// 会一路走到字幕上（media/assemble.cpp 把 line.text 原样放进字幕），
/// 观众看得见。
///
/// 只削确实是列表符号的那几个：`-` `*` `+` `•` `·` `・`。
/// **不动 `—`／`——`**——中文里破折号开头是正当写法（话被打断、话外补白），
/// 削了是改文意。削成空串也不削。
std::string strip_list_marker(const std::string& text);

/// 台词里裹着的旁白剥掉，只留说出口的那部分。
///
/// **2026-09-13 实跑撞上的**（walk_c ep02）。模型照抄了原文那一句：
///
///     苏婉    "你来了。"她说，语气平静但带着一丝疲惫。
///
/// 「她说，语气平静但带着一丝疲惫」是旁白，可它在台词字段里——**配音会把它
/// 一起念出来**，成片里就多了一句莫名其妙的第三人称。和「（无台词）」被念出来
/// 是同一类：数据合法，内容是错的。
///
/// schema 的描述里其实写着「kind=dialogue：只写说出口的话，不带引号，不重复
/// 人名」——这句话**模型是看得见的**（2026-09-19 核过：整份 schema 连
/// description 一起贴在提示词后面发出去，见 chapter_write.cpp 里
/// `chapter_schema` 上面那段 ⚠️⚠️）。看得见也照样这么写，所以这件事只能由
/// 引擎来做：描述是劝，劝不动就得有人在回来之后动手。
///
/// （这儿原来写的是"模型从来看不见 description，只有结构会变成 GBNF 约束"。
/// 那是本地后端那条路的事，那条路已经删了——**结论没变，理由变了**：
/// 不是它没读到，是它读到了不照做。）
///
/// 规则不是猜的。文学 NLP（BookNLP 那一路）的通行做法是「引号划出说的话，
/// 说话动词标出提示语，其余当叙述」；中文小说的对话只有四种形式：
///
///     提示语在前   他说：“……”
///     提示语在后   “……”他说。
///     提示语在中   “……，”他说，“……”
///     无提示语     “……”
///
/// **四种里引号都贴着句子的一头。** 所以只在「首尾至少一端是引号」时才剥，
/// 「他说过“再见”，然后走了」这种引号夹在中间的原样留着——那不是对话形式，
/// 剥了会把整句话吃掉只剩两个字。
///
/// 没有引号的行一个字都不动（正常剧本里的台词本来就不带引号）。
/// `speaker` 非空时顺带削掉开头重复的「人名：」。
std::string strip_speech_tags(const std::string& text,
                              const std::string& speaker = "");

/// 把「没人说话」的各种写法统一成空字符串。
///
/// schema 里写的是填空字符串，但模型经常填 none、null、旁白 这类词。
/// 原样当成名字的话，成片里会出现一个叫 none 的角色，
/// 字幕上写着「none：寂静」。
std::string normalize_speaker(const std::string& raw);

/// 这个时长大概能装多少字对白。
int budget_chars(double duration_s);

// ---- 数据 ----

/// 一个选题。
struct PremiseIdea {
    std::string title;
    std::string premise;
    std::string hook;
};

/// 一拍。动作或者一句台词。
struct Beat {
    /// action / dialogue / scene。
    ///
    /// **scene 是 2026-09-15 加的场次头**：换地方或换时间时起的新一场，
    /// text 是「日/夜 · 内/外 · 地点」三样。它不是拍出来的画面，渲染成
    /// 「【第N场 · 夜 · 内 · 天台】」一行，分镜那一步按它把一章切成几场、
    /// 一场一次拆镜（docs/电影质感方案.md：长视频最要紧的是连贯，而连贯
    /// 的单位是场——同一场里同一个地方、同一个时段）。
    std::string kind;
    std::string speaker;  ///< kind 是 action / scene 时是空串
    std::string text;

    // ---- 照着章写时多出来的四栏（2026-09-16）----
    //
    // 按秒排那条路上拍子只有上面三样，照着章写的提示词却要求「环境拍
    // characters 为空」「台词旁边写怎么说的」「特效直接写」——要求了没地方
    // 放，模型要么略过、要么塞进 text 被配音念出来。现在各有各的栏：
    /// 这一拍画面里有谁（人物表里的名字）。动作拍用；空 = 空镜。
    std::vector<std::string> characters;
    /// 这句台词怎么说的（压着嗓子、笑着说）。渲染成「名字（怎么说）：台词」，
    /// 分镜填进 DialogueLine::emotion，配音照着念；**不进 text**。
    std::string delivery;
    /// 潜台词：这个人此刻真正想要什么、在藏什么。渲染成 〔…〕，只给分镜
    /// 挑机位、表情和光用，不进画面描述、不进配音。
    std::string subtext;
    /// 需要特效的地方，写成画面里看得见的样子。渲染成 〔特效：…〕。
    std::string fx;
};

/// 四段里的一段，按秒排。
///
/// 开场钩子 / 冲突推进 / 情绪回报 / 章尾留扣——一章的节奏公式
/// （0–5 秒抛冲突、5–40 推进、40–75 回报、最后 5–10 秒卡死钩子），
/// 按这一章的时长等比缩放，开场不超过 8 秒、留扣不超过 10 秒。
struct ActSpec {
    std::string key;    ///< opening / escalation / payoff / cliff，JSON 里的键
    std::string label;  ///< 段头上那个名字，**随形状变**（见 act_plan）
    /// 这一段该写什么，一句话。给提示词和 schema 描述共用。
    ///
    /// **放在 spec 里而不是让调用方自己去查词表**：提示词、schema、解析
    /// 三处都得用同一组说法，各查各的迟早会错开一处——而错开的表现是
    /// 模型看到的段和我们解析的段对不上，不报错。
    std::string brief;
    int from_s = 0;
    int to_s = 0;
    /// 这一段至少几拍、至多几拍。**地板是 schema 里唯一管用的东西**——
    /// 提示词里"要凑够"模型不听，minItems 写几它就写几。每 4 秒一拍起，
    /// 顶到每 1.5 秒一拍。
    int min_beats = 0;
    int max_beats = 0;
};

/// 摇一个形状种子。每叫一次都不一样，0 除外（0 表示不浮动）。
///
/// **照 ComfyUI 的 seed 那一套。** 那边 seed 是个看得见的输入框，旁边
/// control_after_generate 可以选 randomize——不满意就再摇一次，摇到满意的
/// 就固定下来。我们这儿「再摇一次」就是再点一次「重新改编」，「固定下来」
/// 就是点采用（剧本存进项目，形状跟着定了）。
///
/// 所以**不从 `episode_id` 哈希**：那样同一章永远是同一个形状，人不喜欢这
/// 一章的节奏也换不掉——那不叫多样性，叫每章一个固定的模子。
std::uint32_t random_shape();

/// 把一章按秒切成四段。时长小到切不开也不会崩（最少按 4 秒排）。
///
/// **总量是定的，形状不是。** 一章要撑够多少拍由时长决定（不加这个约束，
/// 60 秒的一章会写出 13 秒）；但「开场占几秒、推进占几秒」不该写死——写死
/// 了每一章都是同一个模子，60 秒永远是 5/28/21/6，连着看几章就是一个样。
///
/// 所以 variation 非零时，各段的比例在下面这几个区间里挑一组：
/// 开场 5%~14%、留扣 7%~17%、推进占中段的 45%~68%。有的章从头压到尾，
/// 有的章开场慢、后半段炸，总拍数还是那么多。
///
/// **秒数浮动还不够，戏的走法也得换。** 只浮动秒数的话，十章看下来是
/// 同一出戏演快一点演慢一点——钩子 → 推进 → 回报 → 留扣，一章不落。
/// 用户的判词「提取出来剧本时间线也都差不多」说的正是这个。所以
/// variation 非零时还从 prompts.toml 的形状表里挑一组（开门见山 / 中途
/// 翻盘 / 一路下坠 / 两头并进…），换掉每一段的标签和说法。槽位名
/// （opening…）不变——那是 schema 的键。
///
/// variation = 0 是**不浮动**：固定比例（8%/10%/57%）加第 0 组形状，
/// 和以前一字不差。语料和单测走的就是这一档。
std::vector<ActSpec> act_plan(double duration_s, std::uint32_t variation = 0);

/// 照着章写时的一场：**拍数按这一场的篇幅给，不按秒，也不套四段。**
///
/// 用户 2026-09-16 的判词：剧本从一章里直接拿，写多长由内容定，不该在
/// 提示词里限制一章多长。正文那一步已经把章写成了几场（每场有目标、
/// 阻碍、转折），剧本照着场走——之前在场之上又套了一副按秒排的四段骨架，
/// 两副骨架叠着，段头还印着「按秒排」。
///
/// 一拍是一件事或一句话，原文六十个字左右一拍起步，二十个字一拍封顶。
/// **地板是 schema 里唯一管用的东西**，所以每场各给各的地板。
struct ScenePlan {
    std::string key;    ///< JSON 里的键：s1、s2…
    std::string where;  ///< 这一场在哪、跟着谁（给提示词看的一行）
    int chars = 0;      ///< 这一场的原文字数
    int min_beats = 0;
    int max_beats = 0;
};

/// 按各场的字数给拍数。`scene_chars` 空时整章当一场。
std::vector<ScenePlan> scene_plan_for_chapter(
    const std::vector<std::pair<std::string, int>>& scene_chars);

/// 给提示词看的那一段：四段各占几秒、各干什么、至少几拍。
///
/// 说法从 `specs[i].brief` 来，不查词表——形状是 act_plan 挑的，
/// 这儿再查一次就可能和它错开。
///
/// 写进提示词，别只放在 schema 的 description 里。
///
/// ⚠️ 理由不是原来写的那个（"GBNF 里只有结构，描述模型从来没看见过"）——
/// 那是已经删掉的本地后端那条路，今天 description 是随 schema 一起贴进提示词
/// 的，模型读得到（见 chapter_write.cpp 里 `chapter_schema` 上面那段 ⚠️⚠️）。
/// 今天还该不该坚持写进提示词——**没重新验过**。能说的只有位置上的差别：
/// 这一段是每次都不一样的活（四段各占几秒、至少几拍），而 schema 那半截压成
/// 一行排在整条提示词最后。要撤这句话，先拿两份提示词各跑几次比一比，别
/// 照着上面那个已经作废的理由撤。
std::string render_act_brief(const std::vector<ActSpec>& specs);

/// 渲染出来的剧本里，段与段之间那一行：「【开场钩子 0–5 秒】」。
///
/// 段头留在正文里而不是另存一份结构，是因为下游全认这段文本：分镜模型
/// 看得到这一段占几秒，页面按它分块显示，人在文本框里改也改得动。
std::string act_header(const std::string& label, int from_s, int to_s);

/// 这一行是不是段头。认的是**形状表里所有的段名**（现在五组共二十条，
/// 去重后十来个），「【字幕】三年后」「【倒计时 10 秒】」都不是。
/// 认出来的话把名字和秒数填进去（秒数没写就是 0）。
///
/// ⚠️ 名单变长之后这道判断比以前松：往形状表里加标签时挑一看就是**段名**
/// 的词。挑了能当舞台提示写的词，用户自己写的那一行会被 strip_act_headers
/// 当段头摘掉。
bool parse_act_header(const std::string& line, std::string* label,
                      int* from_s, int* to_s);
bool is_act_header(const std::string& line);

/// 去掉段头，只留拍子。老项目反推故事时用：段头进了"小说正文"是噪音。
std::string strip_act_headers(const std::string& script);

/// 场次头那一行：「【第1场 · 夜 · 内 · 天台】」。index 从 1 起。
///
/// 和段头一样留在正文里而不是另存一份结构：分镜、页面、人手改，认的都是
/// 这段文本。和段头分得开——段头的名字在形状表里，场次头以「第N场」开头。
std::string scene_header(int index, const std::string& body);

/// 这一行是不是场次头。是的话把序号和「夜 · 内 · 天台」那一截填回去
/// （body 可能是空的：「【第2场】」也认）。
bool parse_scene_header(const std::string& line, int* index, std::string* body);
bool is_scene_header(const std::string& line);

/// 写出来的一段。
struct Act {
    std::string key;
    std::string label;
    int from_s = 0;
    int to_s = 0;
    std::vector<Beat> beats;
};

/// 写出来的一份剧本。
struct ScriptDraft {
    std::string title;
    std::string logline;
    /// 所有拍子，平的一份。acts 非空时它就是四段顺次拼起来的同一批拍子，
    /// speakers() / dialogue_chars() 都看它。
    std::vector<Beat> beats;
    /// 四段。平的回包（预告片、旧模型）解析出来这里是空的，渲染时就没段头。
    std::vector<Act> acts;

    /// 出场说话的角色，按首次出现排序，去重。
    std::vector<std::string> speakers() const;

    /// 对白总字数（**字符**不是字节）。用来判断超没超预算。
    std::size_t dialogue_chars() const;

    /// 渲染成后面几步认的写法。
    ///
    /// 对白一律「名字：台词」，动作单独成行，有段的话每段前面一行段头。
    /// 格式定死，不看模型心情，否则下一步识别角色就开始出错。
    std::string render() const;
};

// ---- 提示词 ----
//
// 三个都**必须和 Python 的对应函数逐字节一致**。

std::string build_script_prompt(const std::string& premise, double duration_s,
                                models::StyleLine style_line,
                                const std::string& previous = "",
                                const std::vector<std::string>& characters = {},
                                std::uint32_t variation = 0);

std::string build_premise_prompt(const std::string& keywords,
                                 models::StyleLine style_line, int count = 3,
                                 const std::vector<std::string>& existing = {});

std::string build_trailer_prompt(const std::string& premise, double duration_s,
                                 models::StyleLine style_line,
                                 const std::string& episodes = "",
                                 const std::vector<std::string>& characters = {});

/// 剧本的 JSON Schema，**平的一份**：title、logline、beats。预告片用它——
/// 预告片是蒙太奇，不分四段。
const nlohmann::ordered_json& script_schema();

/// 正片的 JSON Schema，**四段的**：title、logline，然后 opening / escalation /
/// payoff / cliff 四个对象按顺序各带一个 beats 数组，每段的 minItems /
/// maxItems 按这一章的时长算（见 act_plan）。
///
/// 用四个命名字段而不是一个数组：GBNF 按 properties 的顺序生成，四段就一定
/// 按顺序、一段不少地出来；数组的话每一项的地板没法各不相同。
///
/// characters 非空时，**speaker 收紧成枚举**（这几个名字，外加空串给动作行）。
///
/// **2026-09-12 加的。** 提示词里写着「人物名必须和上面给的一模一样」，实跑
/// 出来的那一章里三个角色写出了四种名字：林浩、Lin Hao、LinHao、Su Wan。后果不是
/// 报错——下一步分镜按名字找 char_id，找不到的那句 char_id 就是空，于是它变成
/// 旁白：不分配音色、不做口型，字幕上还挂着「Su Wan」。
///
/// 分镜那边早就是这么干的（llm_shot_schema 把 char_id 收成枚举），注释里写的是
/// 「收紧成枚举是防止模型凭空造角色最硬的手段」。剧本这一层一直没收。
/// variation 要和提示词、解析那两处用**同一个**，否则段头上的秒数对不上。
nlohmann::ordered_json script_schema(
    double duration_s, const std::vector<std::string>& characters = {},
    std::uint32_t variation = 0);

/// 章模式的 schema：title、logline，然后 scenes 对象里 s1 / s2 … 按顺序各带
/// 一个 beats 数组，地板按那一场的篇幅（见 scene_plan_for_chapter）。拍子是
/// **带 characters / delivery / subtext / fx 的那份**（beat_item_schema 的
/// rich），speaker 和 characters 都收成人物表里的枚举。
nlohmann::ordered_json script_schema_for_chapter(
    const std::vector<ScenePlan>& scenes,
    const std::vector<std::string>& characters);
/// 选题的 JSON Schema。
const nlohmann::ordered_json& premise_schema();

// ---- 解析 ----

/// 解析模型回包。四段的和平的都认。
///
/// duration_s 给了才知道每段占几秒（段头上那个数）；不给的话段还在，
/// 只是段头没有秒数。
ScriptDraft parse_script(const std::string& raw, double duration_s = 0.0,
                         std::uint32_t variation = 0);

/// 解析章模式的回包（script_schema_for_chapter 那份）。各场的拍子顺次拼成
/// 平的一份，没有段；一场的第一拍不是场次头的话，补一个空的场次头
/// （「【第N场】」仍是切点，只是没说在哪）。
ScriptDraft parse_chapter_script(const std::string& raw,
                                 const std::vector<ScenePlan>& scenes);
std::vector<PremiseIdea> parse_premises(const std::string& raw);

}  // namespace changji::stages
