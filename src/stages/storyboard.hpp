#pragma once

// 分镜生成。
//
// 两阶段生成的第二阶段。角色和场景已经在资产库里有 id 了，
// 这一阶段的 schema 里没有外观字段可写——给了模型就会忍不住在分镜里
// 复述一遍，而复述必然有偏差，那正是漂移的来源。
//
// 和 bible 一样，这里**不碰网络也不碰 llama.cpp**，只有纯函数。

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/shot.hpp"
#include "stages/limits.hpp"

namespace changji::stages {

class StoryboardError : public std::runtime_error {
public:
    /// `code` 是**闸门代号**，和 `stages::StoryError::code()` 同一件事：
    /// 一个稳定的短名，落进 `gates.jsonl` 那一列给「哪条闸门最费钱」分组。
    /// 那句话里带着这一次的数（「分镜表有 7 个镜头不合法」），分不了组。
    ///
    /// 默认空串：不是闸门的那些（目标时长填错了、资产库是空的）留空。
    ///
    ///   bad_json        JSON 解析不动
    ///   no_shot_list    返回的不是镜头列表
    ///   empty_shots     返回了空的分镜表
    ///   shot_invalid    有镜头字段不合法
    ///   unknown_asset   引用了没注册的角色 / 地点
    ///   coverage_gap    拆完发现分镜表不完整（在 run_storyboard 里）
    explicit StoryboardError(const std::string& what, std::string code = {})
        : std::runtime_error(what), code_(std::move(code)) {}

    const std::string& code() const { return code_; }

private:
    std::string code_;
};

/// 视频模型支持的时长档位。
///
/// **和 Python 一样在 fps=24 下算死**，不跟着 assembly.fps 走。
/// 那边 DURATION_SLOTS 是模块导入时用默认 fps 算的模块级常量，
/// 配置里改了 fps 也不会重算。这一点照抄——不是因为它对，
/// 而是因为改了会让两边的分镜表对不上。
const std::vector<double>& duration_slots();

/// 时长配额。先定骨架再填内容，比让模型自己算总时长可靠得多。
struct DurationQuota {
    /// 档位 -> 个数。用 map 而不是 unordered_map：describe() 要按档位排序。
    std::map<double, int> slots;

    double total_s() const;
    int shot_count() const;

    /// "3 个 2 秒镜头，5 个 5 秒镜头" 这样的一句话。
    std::string describe() const;

    /// 按目标时长分配镜头。
    ///
    /// 节奏上短镜头占多数，长镜头留给情绪戏。全用同一时长会平铺直叙。
    static DurationQuota for_duration(double target_s);
};

/// 把任意时长吸附到最近的可生成档位。
double snap_duration(double seconds);

/// 「一镜一件事」那一档：不超过 5 秒的档位里最长的一个。配额补差、镜数
/// 地板都按它算，不按最长档位（单镜上限 15 秒时最长档位会把 60 秒的目标
/// 凑成 75 秒）。
double pad_slot();

/// 章模式：一段剧本大概要拍多久。台词按念的速度、动作拍按每拍三秒估——
/// 只是拆镜头的目标量，真时长以配音为准。至少 20 秒。
double estimate_script_seconds(const std::string& script);

/// 向上吸附。配音时长反推镜头时长时用，宁长勿短。
double ceil_duration(double seconds);

/// 分镜数的地板和天花板。0 表示不限。
///
/// **2026-09-12 加的，因为 60 秒的一章出过两镜六秒。** 配额那句话
/// （"合计 16 个镜头，总时长 60 秒，必须严格按配额"）提示词里一个字没少，
/// 模型照样只出两镜——schema 里 shots 数组没有 minItems，两镜在语法上
/// 挑不出毛病。和剧本那边一样：数量只有写进 schema 才管用。
struct ShotCountBounds {
    int min_items = 0;
    int max_items = 0;
};

/// 数剧本里有几拍：非空行数，段头（「【开场钩子 0–5 秒】」）和场次头
/// （「【第1场 · 夜 · 内 · 天台】」）不算。
int count_beats(const std::string& script);

// ---- 按场拆镜（2026-09-15）----
//
// 长视频最要紧的是连贯，连贯的单位是场：同一场里同一个地方、同一个时段。
// 整章一次拆时模型顾不过来（实跑 12 镜里 8 镜没填场景），而且十分钟的章
// 一百多镜一次响应装不下。所以剧本里有两场以上时，一场一次拆：这一场的
// 地点、时段钉死传进去，所有镜头的 location_id 由引擎盖成这一场的。
// 只有一场（老剧本没有场次头）时走整章那条路，一个字不变。

/// 剧本里的一场戏。
struct SceneBlock {
    /// 场次头上的序号，从 1 起。**0 = 剧本里没有场次头**，整章就是一场。
    int index = 0;
    std::string time;   ///< 时段：日 / 夜 / 黄昏…，可能空
    std::string inout;  ///< 内 / 外，可能空
    std::string place;  ///< 地点名，可能空
    std::string body;   ///< 场次头里那一截原文，「夜 · 内 · 天台」
    /// 这一场的剧本，**不含场次头那一行**（段头留着，它说明这一段占几秒）。
    std::string text;
    /// 按地点名接到资产库的场景 id。接不上就空，让模型自己填（同整章那条路）。
    std::optional<std::string> location_id;
    /// 分到的时长（秒），assign_scene_seconds 填。
    double seconds = 0.0;
};

/// 按场次头把剧本切成几场。没有场次头就是一场（index 0，text 是整份剧本）。
/// 第一个场次头之前的行（比如段头）归到第一场。
std::vector<SceneBlock> split_scenes(const std::string& script,
                                     const models::AssetLibrary& assets);

/// 把场次头那一截「夜 · 内 · 天台」拆成时段、内外、地点。
/// 分隔符认 ·、/、，、、；认不出类别的那一截当地点。
void parse_scene_body(const std::string& body, SceneBlock& out);

/// 地点名接到资产库：先按名字全等，再按互相包含（「天台」⊂「夜晚天台」），
/// 多个命中取名字最长的那个。接不上返回空。
std::optional<std::string> resolve_scene_location(
    const std::string& place, const models::AssetLibrary& assets);

/// 把一章的时长按各场的拍数分给各场，每场至少最短那一档。
void assign_scene_seconds(std::vector<SceneBlock>& scenes, double target_s);

/// 一场的分镜提示词（prompts.toml [storyboard_scene]）。
/// `prev_tail` 是上一场最后一镜的画面，空的就不写那一段。
std::string build_scene_storyboard_prompt(const SceneBlock& scene,
                                          int total_scenes,
                                          const models::AssetLibrary& assets,
                                          const DurationQuota& quota,
                                          const std::string& episode_id,
                                          const std::string& prev_tail);

/// 一场的 schema：整章那份之上，把**这一场已经定死的两栏**钉成定值，
/// 而且进 required——这一场所有镜头都在这个地方、都属于这一场，模型没得选。
///
///   · `location_id`：接上了场景的话钉成它；
///   · `scene_id`：钉成 `sN`（`scene_index`，1 起）。
///
/// ⚠️ **scene_id 不钉的话，模型要为一个它猜不出、而且猜了也白猜的值花力气。**
/// 拆完之后 `stamp_scene` 会把这一栏整个盖成 `sN`——也就是说模型填什么都不
/// 作数，可它在 required 里，非填不可。2026-09-17 从思考流里读到的原话：
/// 「scene_id 可能填什么? 需要 pattern。可能填 ep01_sc01? …用户没指定。
/// 得选择。」整整一段推理花在一个会被覆盖的字段上。
///
/// 顺带治住另一件早就记在案的事（见 `attach_scene_location` 那段）：
/// 「模型十次有八次把场景 id 填进 scene_id 就完事了」。两栏都钉死，填错都
/// 没地方填。
///
/// `scene_index <= 0` 表示不知道第几场，那时候不钉 scene_id。
nlohmann::ordered_json llm_scene_shot_schema(
    const models::AssetLibrary& assets, ShotCountBounds bounds,
    const std::optional<std::string>& location_id, int scene_index = 0);

/// 拆出来的镜头盖上这一场的印：scene_id = "sN"、location_id 统一、
/// 第一镜不接上一场的帧。
/// 把一份平的镜头表按场分组，回那份「场的数组」：
///
///     [ {"scene": 1, "scene_id": "s1", "shots": [...]}, ... ]
///
/// 用户 2026-09-18：「把每一场的 json 合并成 json 数组」。这是引擎产出的那
/// 半：`/api/plan` 回它，人复制出去；粘回来时 split_by_scene 认的是同一个
/// 形状。**分组靠 scene_id（stamp_scene 盖的 "s<场号>"），保留原顺序**——
/// 没场次头的老剧本全部是同一个 scene_id，那就是一项。
nlohmann::json scenes_array(const std::vector<models::Shot>& shots);

void stamp_scene(std::vector<models::Shot>& shots, const SceneBlock& scene);

/// 从目标时长和剧本的拍数推分镜数的上下限。
///
/// 地板是**物理下限**：单镜最长 5 秒，60 秒至少 12 镜，少于它总时长凑不够。
/// 但不能高过剧本的拍数——五行的剧本硬要 12 镜出来的是空镜（chapter_write
/// 那边的教训：模型没话说的时候，多给它几个格子只会得到几格垃圾）。
/// 天花板给得宽（配额和拍数里大的那个的两倍），只防写个没完。
ShotCountBounds shot_count_bounds(const DurationQuota& quota, double target_s,
                                  int beats);

/// 生成给大模型的 JSON Schema。
///
/// 从 pydantic 导出的 Shot schema 出发（见 shot_schema.inc.hpp），
/// 做四件事：删掉运行时字段、把角色和场景 id 收紧成枚举、
/// 去掉 needs_lipsync（那个由规则算）、把运动那两栏钉成必填。
///
/// 收紧成枚举是防止模型凭空造角色最硬的手段。
///
/// **`required` 这张表本身就是最硬的那个旋钮。** 不在表里的字段模型会整个
/// 略过，解析时补上结构体默认值，全程不报错——实测一份 198 镜的项目里
/// `motion_prompt` 空了 198 个、`camera_move` 全是 static，而同一张表里
/// 在 required 里的 `shot_size` 有五种取值。加字段进来要想清楚，
/// 不加则等于默认它不会被填。
///
/// bounds 给了就把镜头数写进 shots 的 minItems / maxItems。
nlohmann::ordered_json llm_shot_schema(const models::AssetLibrary& assets,
                                       ShotCountBounds bounds = {});

/// 拼提示词。**输出必须和 Python 的 build_prompt 逐字节一致。**
///
/// 角色和场景只给 id 和名字，不给外观描述。
std::string build_storyboard_prompt(const std::string& script,
                                    const models::AssetLibrary& assets,
                                    const DurationQuota& quota,
                                    const std::string& episode_id);

/// location_id 空着但 scene_id 正是一个已注册场景时，把它接上。
///
/// 模型十次有八次把场景 id 填进 scene_id 就完事了。后果不是报错——
/// 分镜表照样合法，是渲染时只在 location_id 有值时才把场景描述拼进提示词，
/// 于是空间和光线那一段整个丢掉，同一个房间在每个镜头里都长得不一样。
///
/// 返回是否改动过，调用方要靠它决定用不用存盘。
bool link_location(nlohmann::json& item, const std::set<std::string>& known);

/// 这一句是不是「占位符」——模型该填空数组时填进来的那种。
///
/// **2026-09-12 实跑撞上的，而且这个会出声。** schema 里写着「这一镜没有人
/// 说话就填空数组」，模型照样在 dialogue 里塞了一句 `（无台词）`：十四镜里
/// 有四镜是这样。它不会被任何校验拦下——是合法的 DialogueLine，字数也够——
/// 然后一路走到配音，**成片里真的有人念出「无台词」三个字**。
///
/// 和 normalize_speaker 是同一个病：那边管说话人栏里的 none / 旁白，
/// 这边管台词栏里的（无台词）。形式是我们定的，不是模型定的。
bool is_placeholder_line(const std::string& text);

/// 把有台词但没进角色列表的说话人补进去。
///
/// 这不是分镜错误，只是漏填：说话的人必然在场。
/// 程序补上比让整条命令挂掉合理。
void add_missing_speakers(nlohmann::json& item,
                          const std::set<std::string>& known);

/// 解析模型返回，产出镜头列表。
/// 把运动描述里那几段 `[a-b秒]` 的末段夹到 `dur`——短了补满，长了截回。
///
/// **两个地方要用**，所以是纯函数：解析分镜时（模型写短/写长了），以及
/// 时长被改过之后（rebalance_durations 为了凑总时长换档，motion_prompt
/// 不跟着改就成了陈的——2026-09-16 实测一个 6 秒的镜头挂着 `[0-15秒]`）。
///
/// 一段都没有的（模型没按格式写）整句包成 `[0-N秒]`：至少时间轴是满的。
std::string motion_covering(const std::string& motion_prompt, double dur);

/// 把运动描述里那些**会把主体带出画**的分句摘掉，返回摘完的那一份。
/// 没有可摘的（或者摘完就空了）原样返回。
///
/// **这是闸门判出「片中硬切」之后的兜底。** 图生视频只能动第一帧里已经有
/// 的东西，写了「走向门口」「推门进来」「镜头穿过走廊」，模型只能凭空造
/// 画面外的人和空间——造出来的就是半路换掉的那一场戏。
///
/// 提示词第 6 条已经把这类写法从 3/3 压到 1/18（2026-09-16 三章实测），
/// 但没清零，而残留的那一条照样崩。而现在的重试只换种子——闸门自己那句
/// 话就写着「换种子重出视频没用」。种子换了一百遍，那句「走向门口」还在。
///
/// 所以摘掉它再重出：镜头少演一个动作，比整镜半路换成另一场戏强得多。
/// @param allow_empty 全句都危险、摘完什么都不剩时怎么办。
///
/// **假（默认，给 motion_prompt 用）**：原样还回去。宁可留一个会崩的镜头，
/// 也不交一段空的运动描述——空的那一段模型同样会自由发挥，而且连线索都没了。
///
/// **真（给角色的 action 用）**：摘空就让它空着。这一栏和 motion_prompt 的
/// 处境不一样：它只是首帧提示词的一个输入，空着就是"这个人没有特别的动作"，
/// 不存在"模型拿着空描述去编"这回事。
///
/// 2026-09-17 之前这两处共用假那一档，于是「走向门口」这种**整句都危险**的
/// action 原样留了下来——而那正是「人整个走出画面」的来源（用户报过的
/// 成片毛病之一），test_storyboard 里那条「角色 action 里的进画出画也要摘」
/// 也一直红着。
std::string defuse_motion(const std::string& motion_prompt,
                          bool allow_empty = false);

/// 把在场角色的 action 里会把主体带出画的分句一并摘掉（同 defuse_motion）。
/// **两处都要摘**：拼运动提示词时 characters[].action 会被重新接到
/// motion_prompt 后面（PromptComposer::motion_prompt），只摘 motion_prompt
/// 等于没摘，闸门会在同一镜上反复退回（2026-09-16 查出）。
/// 返回有没有真摘掉东西。
bool defuse_actions(std::vector<models::CharacterInShot>& characters);

/// 剧本正文里的一句台词：谁说的、说了什么、怎么说的。
///
/// 章模式的剧本渲染成「名字（压着嗓子）：台词〔潜台词〕」——括号里是
/// delivery（分镜填进 DialogueLine::emotion，配音照着念），〔〕里是潜台词
/// （只给分镜看）。这里把三样拆开：said 里没有〔〕，name 里没有括号。
struct DialogueEntry {
    std::string name;
    std::string said;
    std::string delivery;
};
std::vector<DialogueEntry> script_dialogue_entries(const std::string& script);

/// 运动描述里有几段 `[a-b秒]`。0 = 没按格式写。
///
/// **判「这一镜是不是规划了好几件事要演」用的**：分了两段以上的，那个时长
/// 是分镜的判断，配音不该拿一句短台词把它压回去（见 AudioStage::lock_duration）。
int count_motion_segments(const std::string& motion_prompt);

/// 把带时间轴的运动描述按 `weights` 的比重切成几份，每一份的时间轴重新从
/// 0 起算。台词装不下、一镜拆成几镜时用它——不切的话每一镜都挂着同一条
/// 完整时间轴，拆出来的几镜会演出一模一样的画面。
/// 段数不够分（或者原文没写时间轴）时，每一份都是原文。
std::vector<std::string> split_motion(const std::string& motion_prompt,
                                      const std::vector<double>& weights);

std::vector<models::Shot> parse_storyboard(const std::string& raw,
                                           const models::AssetLibrary& assets);

/// 把镜头编号和顺序重排成规整的一套。
///
/// **2026-09-12 加的，因为模型编出来的 id 是坏的。** 一次实跑里出了
/// `ep61_sh002`（`episode_id` 都错了）、`ep01_sh6`（没补零）、`ep01_s1h11`（打错字）。
/// 提示词里写着「三位数字，按顺序递增」，schema 的 `^[a-z0-9_]+$` 也全放行
/// ——**三种写法都合法，所以一句都不报**。而首帧、配音、成片的文件名都是
/// 从 shot_id 拼的：`episode_id` 错的那一镜会写到别的章的目录里去。
///
/// 按 order 稳定排序，然后 order 重排成 0..n-1、id 重排成
/// `<episode_id>_shNNN`。顺便治了 order 重复——重复时镜头次序是不定的，
/// 而那个次序就是成片的次序。
///
/// **只在分镜刚出来时调。** 配音那一步会拆镜（`free_shot_id` 发 `_b` 后缀），
/// 那之后再重排就会和已经落盘的音频文件名对不上。
void renumber_shots(std::vector<models::Shot>& shots,
                    const std::string& episode_id);

/// 检查分镜有没有**废掉**。
///
/// 大模型很容易只写画面不写台词，产出一部哑剧。这类问题在生成阶段就能检出，
/// 不该等到配音阶段发现一句话都没有。
///
/// 两条，都是「这张分镜表没法用」：一句台词都没有、一个角色都没有。
/// 调用方拿到非空就该丢掉重来。
///
/// **漏几句台词不在这儿**，那个见 missing_dialogue_lines——分镜表还能用，
/// 丢掉它等于把几分钟的显卡时间也一起丢了。
std::vector<std::string> check_coverage(const std::string& script,
                                        const std::vector<models::Shot>& shots);

/// 把剧本里漏掉的台词按顺序补进镜头，返回补了几句。
///
/// **2026-09-12 加的，因为分镜模型根本不搬台词。** 把原始输出 dump 出来看，
/// 一章剧本九句台词，模型只写了两句——不是解析吃掉了，是压根没生成。这个
/// 靠校验和清洗救不了：句子不在那儿。
///
/// 而台词本来就在剧本里，有顺序、有说话人、一字不差。分镜师真正该干的是
/// 画面——景别、运镜、构图；台词是从剧本抄过来的，不该指望模型重打一遍。
/// 行业里也是这么分的（分镜头脚本叫「导演脚本」，台词那一栏照抄剧本）。
/// 所以这一步改成：**模型排画面，台词由引擎放**。
///
/// 落位办法：已经落对的那些当锚点，漏掉的按它在剧本里的前后关系插到相邻
/// 锚点之间；一个锚点都没有就按比例摊到各镜。位置是估的，但**台词、说话人、
/// 音色、口型、字幕全是准的**——比整句话消失强得多，而且人在镜头那一页
/// 拖一下就能改。
///
/// 补进去的镜头会因为多了台词而时长不够？不会：配音那一步按语音时长
/// 重新锁定镜头时长（lock_duration），本来就是这么设计的。
int place_missing_dialogue(std::vector<models::Shot>& shots,
                           const std::string& script,
                           const models::AssetLibrary& assets);

/// 剧本里哪几句台词没落到任何镜头上，按剧本顺序。
///
/// **2026-09-12 加的。** 原来根本没人查：一章 17 拍出 12 镜、整个章尾留扣
/// 那一段（连同这一章的钩子）没进分镜，照样算通过，存下去，到成片才看得出
/// 这一章结尾不对。章尾那一下是把人拉着往下看的地方，丢的恰恰是最要紧的
/// 那一段。
///
/// **但它不是致命错。** 实跑里九句丢一句是常事，而出一次分镜要两三分钟的
/// 显卡时间——为了中间漏的一句把整张表扔掉，人什么也拿不到，还得从头再等
/// 一遍。所以这里只把漏掉的报回去：表照存，界面上说清楚漏了哪几句，人可以
/// 在镜头那一页把它补进某一镜，也可以重出。
///
/// 比对时空白和标点都不算数：模型把一句拆成两镜、或者把句号换成逗号，都是
/// 同一句话落地了。真丢了的那种是整句都不在，那个照样查得出来。
std::vector<std::string> missing_dialogue_lines(
    const std::string& script, const std::vector<models::Shot>& shots);

/// 这一串镜头**真正会出多长**。
///
/// **不是 `sum(duration_s)`。** 分镜表里那个秒数是名义值——从档位表
/// {2,3,4,5,6,8,10,12,15} 里挑的整数——而模型只能按格子出帧（Wan 4n+1、
/// MiniMax-H3 17k+5），所以名义 4 秒的镜头实际出 107 帧 = 4.458 秒。
///
/// 装配那边一直是对的（media/assemble.cpp 的 build_timeline 按
/// `real_duration_s` 排时间轴，注释里写着"按名义值排的话误差会逐镜累积"），
/// **规划这边却一直在按名义值加**——同一章两套秒。
///
/// 实测（walk_c ep01，目标 60 秒）：rebalance 把名义总长精算到 57.00 秒，
/// 成片是 62.54 秒。**它压的那个数根本不是成片长度。** Wan 那会儿每镜只
/// 差 0.042 秒（1%），藏得住；换 H3 之后单镜最多差 0.583 秒，就露出来了。
/// 每一场至少留一个交代地方的大景（LS / MLS）。
///
/// 表里分布看着匀、却一个大景都没有的时候补一个。2026-09-16 实测 ep07：
/// MS 5 / CU 4 / MCU 8，十七镜没有一个 LS 或 MLS——用户报的
/// 「都是近景没有远景」就是这一种，而「整表塌成一种」那道线够不着。
///
/// 两道地板：一场至少一个大景；整章**每六镜至少一个**。
/// 后面那道是 2026-09-16 量了九章之后加的——一章只有一场时，前一道就只
/// 保证十七镜里一个大景（实测 5%），用户那句「都是近景没有远景」照旧成立。
/// 补的时候按「动了最不伤」挑：先空镜，再没台词的，最后每场的收尾镜；
/// 有台词的镜头能不动就不动。整表不足四镜一个字不碰。
/// **大景是少数真出得来的景别**：首帧的取景听参考图的，而场景空景图本身
/// 就是一张大景，所以标成 LS 的镜头是真会出成大景（见 prompt_compose.cpp）。
/// 把说不通的 `continuous_with_prev` 抹掉，回抹了几镜。
///
/// **接戏这一栏措辞按不住，只能事后判。** 2026-09-17 实测（ep02、glm-5.3）：
/// 17 镜里 16 镜标了接戏，而 schema 的 description 明明写着「同一场景、
/// 同一时刻、动作连着才填 true」。换成更严的提示词也一样——这一项由模型
/// 定，不由措辞定（同一份提示词换 glm-4.5-air 是 0 镜，换 glm-5.3 是 16 镜）。
///
/// 而它不是个创作字段，是个**技术开关**：标了它，出片时会拿上一镜真出来的
/// 最后一帧当这一镜的首帧（render.cpp 的 chain_frames，默认开）。于是这一镜
/// 自己写的 first_frame_prompt、景别、机位**全部作废**——那正是用户报的
/// 「好几个镜头产生的视频都有问题」。
///
/// 三条判据都是机械的，不掺审美：
///   · 第一镜没有"上一镜"可接；
///   · 换了场就不是"同一时刻"；
///   · **自相矛盾**：一边说接着上一镜，一边把景别或机位换了。接戏会把上一镜
///     的尾帧当首帧，那一帧的取景就是上一镜的——这时候再声明另一个景别/机位，
///     出来的画面必然不是你要的。实测 16 个标记里 15 个是这一种。
///
/// 是抹掉不是报错，同 drop_unknown_enums。
int drop_impossible_continuity(std::vector<models::Shot>& shots);

void ensure_establishing_shots(std::vector<models::Shot>& shots);

double real_total_s(const std::vector<models::Shot>& shots, int fps = 24);

/// 同上，但用**指定的**那份格子，不读进程里那一份全局的。
/// 只读接口要用它：那些接口从来不设全局的那份，读到的是"上一次跑的是
/// 哪部电影"。见 http/readonly.cpp 的 limits_for_project。
double real_total_s(const std::vector<models::Shot>& shots,
                    const VideoLimits& limits, int fps);

/// 把总时长拉回目标值。
///
/// 偏差优先摊到无对白的过渡镜上，有台词的镜头不动，
/// 因为它们的时长是由配音定的。原地改，同时返回引用方便串联。
///
/// **按成片长度算，不按名义值算**（见 real_total_s）：target_s 和
/// tolerance_s 说的都是片子真正有多长。
///
/// fps 默认 24：分镜那三个入口（planning / episodes / batch）手边没有
/// AssemblyConfig，而 MiniMax-H3 本来就只跑 24fps（"another requested value
/// is overridden"，sd.cpp docs/minimax_h3.md），所以那里用默认值是对的。
/// 配音之后那次重排在 pipeline/episode.cpp，那儿有真的 fps，照传。
std::vector<models::Shot>& rebalance_durations(std::vector<models::Shot>& shots,
                                               double target_s,
                                               double tolerance_s = 3.0,
                                               int fps = 24);

}  // namespace changji::stages
