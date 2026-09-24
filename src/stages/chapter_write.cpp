#include "stages/chapter_write.hpp"

#include "stages/repetition.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "stages/prompts.inc.hpp"
#include "stages/json_extract.hpp"
#include "stages/script.hpp"
#include "stages/story_import.hpp"
#include "stages/story_outline.hpp"
#include "stages/story_plan.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

int index_of(const Story& story, const std::string& chapter_id) {
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        if (story.chapters[i].chapter_id == chapter_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// 在正文里找 needle，返回它结束之后那个**字符**位置；找不到返回 -1。
int find_after(const std::string& body, const std::string& needle) {
    const std::string n = text::strip_ws(needle);
    if (n.empty()) return -1;
    const std::size_t byte_pos = body.find(n);
    if (byte_pos == std::string::npos) return -1;
    return static_cast<int>(text::utf8_len(body.substr(0, byte_pos + n.size())));
}

/// 去掉标点和引号，只留下字。比对「这两句是不是在说同一件事」用。
std::string bare(const std::string& s) {
    static const char* kDrop[] = {"“", "”", "‘", "’", "，", "。", "！", "？",
                                  "…", "、", "：", "；", "「", "」", "—",
                                  " ", "\n", "\t", "\"", "'"};
    std::string out = s;
    for (const char* d : kDrop) {
        std::string::size_type i = 0;
        while ((i = out.find(d, i)) != std::string::npos) {
            out.erase(i, std::string(d).size());
        }
    }
    return out;
}

/// 这两句是不是在说同一件事。
///
/// **为什么要这个：场的 turn 就是这一场的收口，两场停在同一件事上是致命的。**
/// 2026-09-12 实跑：一章里场 1 的结尾和场 3 的 turn 都是「我回来不是为了
/// 道歉」，两处的收口几乎一样；另一处是第三章的第一场把第二章的末场原样
/// 重演，那两章的章尾一字不差。观众看到的是剧情在原地打转。
///
/// 判据刻意做得宽松：短的那句被长的那句包住，或者两句有十个字以上连着
/// 一样。十个汉字连着撞车，正常写作里几乎只会是复制粘贴。
bool same_beat(const std::string& a, const std::string& b) {
    const std::vector<std::string> x = text::utf8_chars(bare(a));
    const std::vector<std::string> y = text::utf8_chars(bare(b));
    if (x.size() < 8 || y.size() < 8) return false;

    const auto join = [](const std::vector<std::string>& v, std::size_t from,
                         std::size_t to) {
        std::string s;
        for (std::size_t i = from; i < to; ++i) s += v[i];
        return s;
    };
    const std::string sx = join(x, 0, x.size());
    const std::string sy = join(y, 0, y.size());
    if (sx.find(sy) != std::string::npos || sy.find(sx) != std::string::npos) {
        return true;
    }
    // 最长公共子串。turn 都是几十个字，平方级别的比法足够快。
    constexpr std::size_t kRun = 10;
    if (x.size() >= kRun && y.size() >= kRun) {
        for (std::size_t i = 0; i + kRun <= x.size(); ++i) {
            if (sy.find(join(x, i, i + kRun)) != std::string::npos) return true;
        }
    }

    // **「用词重合度」那条试过，算不出来，撤了。**
    //
    // 要抓的是这种（2026-09-12 实跑，两处正是相邻两场的收口）：
    //   「林悦认出男人是沈嘉诚，并意识到那把伞对她有特殊意义」
    //   「林悦意识到这把伞对她意义非凡，而对方显然知道这一点」
    // 一个字都不连着一样，上面两条都不响，而观众看到的是同一件事演两遍。
    //
    // 拿相邻两字的 Dice 系数量了一遍：这一对是 **0.30**，而真的不同的两
    // 件事「林悦把伞递给他」和「林悦把信收进抽屉锁好」是 **0.27**——
    // 两者分不开。门槛卡在中间只会两头都错。**让这两句算撞车的信号是
    // 语义上的（都是「她意识到伞的意义」），不在字面里**，这一层量不出来。
    //
    // 留着上面两条：一字不差和大段照抄，它们抓得准。剩下的交给写的时候
    // 别写重（提示词里那条规则），以及人眼。
    return false;
}

}  // namespace

bool scenes_repeat_beat(const std::string& a, const std::string& b) {
    return same_beat(a, b);
}

int chapter_target_chars(const Story& story) {
    const int cap = prose_budget_chars(story.episode_duration_s);
    return std::max(kChapterTargetChars, cap * kEpisodesPerChapter);
}

int chapter_target_scenes(const Story& story) {
    // 一章多少字 ÷ 一场多少字。而一场多少字跟着回落时长走
    // （`scene_target_chars`）。
    //
    // 这个数改过两回，两回都是实跑逼的（那时一章还要按时长切成好几段）：
    // ① 最早直接按段数算，30 秒那一档里一章要 4 场，而当时一章只写得出
    //    1500 字，每场分不到 400 字——模型就从全局地点表里抓了下一章的
    //    地方来凑，四章都在同一个楼顶演同一件事。于是改成按字数算、
    //    一场写死一千字。
    // ② 写死一千字之后，30 秒那一档里一场横跨快两段，刀只能落在场中间，
    //    停在场尾从 86% 掉到 63%。所以又改回跟着时长走——只是这回一场有
    //    600 字的下限兜着，不会再被压回概述。
    //
    // 上限 5：时长定得短时，一章要多切几场才跟得上。
    return std::clamp(chapter_target_chars(story) / scene_target_chars(story), 2, 5);
}

int scene_target_chars(const Story& story) {
    const int cap = prose_budget_chars(story.episode_duration_s);
    return std::clamp(cap > 0 ? cap : kSceneMinChars, kSceneMinChars, kSceneMaxChars);
}

int chapter_scene_chars(const Story& story) {
    // 就是 scene_target_chars，不另算一遍。原来按「章的字数 ÷ 场数」再算
    // 一遍，30 秒那一档算出 600 字/场而一场的容量是 450 字，60 秒那一档是
    // 1000 对 900——同一件事两个数，一场该写多少字就有了两份真相。
    return std::max(200, scene_target_chars(story));
}

int chapter_scene_paras(const Story& story) {
    return std::max(6, chapter_scene_chars(story) / kCharsPerParagraph);
}

// ⚠️⚠️ **下面这些 description 模型一个字不落全看得见。整份 schema 就是提示词
// 的后半截。**
//
// 2026-09-19 实测核过：捞一次真实 chapter 调用落盘的 .prompt.txt（
// `<数据目录>/llm_log/<项目>/<id>.prompt.txt`），3986 个字里 "description"
// 出现 10 次——**和这个函数里写的条数一个不差**，"minLength" 9 次，
// "这一场的正文"、"至少三成" 各 1 次，原文照搬。
//
// 怎么到的：`llm::schema_as_prompt()`（client.cpp）把整份 schema
// `dump(0)` 压成一行接在提示词后面，中间垫一句
// `kSchemaSuffix`——「……还有**描述里写的那些数量和字数限制**，都要照着来」。
// 摘掉的只有 pydantic 自动生成的 `title` 和用不到的 `$defs`：enum、required、
// minItems、maxItems、minLength、description 一条不少。远端（client_http）、
// 命令行（client_command）和 2026-09-19 接回来的进程内（local_client）
// **三条路都走这个函数**——本地那条只接回了模型，**没有把 GBNF 接回来**
// （infer/llama_chat.hpp 上那段），所以下面这段"看得见"对它同样成立。
//
// ---- 这段话 2026-09-12 写的是反的，别照着它下结论 ----
//
// 那时候还有第三条路 local_client.cpp：提示词和 schema 分两路传，schema 只走
// `json_schema_to_grammar()` 变成 GBNF，而 GBNF 里只有结构，description 在
// 转换时就丢了。那条路 **d87590e「编剧这条路只剩远端」整个删掉了**，连带
// GBNF 一起没了；原来那段话却留在这儿，把"模型看不到描述"当成今天的事实。
// 照着它改，会把本该写进 description 的手艺话往外搬——搬到一个它本来就在的
// 地方去。
//
// ---- 那么今天到底什么管得住模型 ----
//
// 两样，**都在生成之后**，解码期一个约束都没有：
//
// 一、`llm::checked_output()` 那道本地校验（schema_validate.hpp）。
//     **它是故意松的**：类型不对、required 缺键、字段是空串、maxLength
//     超长、minItems 太少才拦；写短了（minLength）、写多了（maxItems）、
//     枚举填错**一律放过**——理由写在那个文件里，一句话说就是"差几个字"和
//     "整份输出坏了"不是一回事，而在那儿报错的代价是整章作废。
// 二、这个文件底下那一排闸门（复读、情绪标签、整章没对白、两场撞车、
//     章尾点题……）。真正卡质量的是它们。
//
// 所以「想让它照做一件事」今天有三档，**代价递增**：
//   · 写进提示词或 description —— 它真的会读到，但它可以不听；
//   · 写成 min/max —— 只在回来之后拦，而且好几条明说了不拦；
//   · 写成一道闸门 —— 拦得住，代价是打回一次就得整章重写一遍。
//
// ⚠️ **这份文件里凡是写着「语法」「GBNF」「模型没得选」的地方，讲的都是那条
// 已经删掉的本地路。** 那些观察当时是真的（也正是现在这些形状的由来），但
// 今天没有任何东西在解码期拦着模型——看见"语法不让它…"要读成"当年语法不让
// 它，现在只剩提示词在劝、闸门在拦"。
ordered chapter_schema(int target_scenes, int paras_per_scene) {
    // 场数只让它往上多一场，不让往下少一场：少一场的话，那一场会长到
    // 一千八百字——一场里装进好几件事，收口说不清停在哪，分镜那一次也要
    // 一口吞下它（2026-09-12 实跑）。
    const int min_scenes = std::max(2, target_scenes);
    const int max_scenes = std::max(min_scenes + 1, target_scenes + 1);

    // 段数的上下限：目标的一半到一倍半。下限让它没法一两段交差，上限让它
    // 没法写个没完。
    // 减一：最后一句现在是单独一栏，从 paragraphs 里挪出去了。
    //
    // **这个数管的是段长，不是篇幅。** 2026-09-12 走了个来回才弄明白：
    //
    // 一、原来是三分之二（每场约 13 段）。它有时候高过这一场真有的内容，
    //     而 minItems 当年是 GBNF 硬约束——语法里没有"结束数组"这个选项，
    //     模型只能接着吐，吐出来的不是正文是下一个字段名：
    //     `last_line": "..."`、`turn_note": ...`，最糟一章里一连 22 段
    //     `last_line": null}]}] }`。三跑 1715 段里 29 段这样。用户看出来的
    //     原话是"好多明显是凑字数的字符"——字面意义上的凑字数。
    //     ⚠️ **那条 GBNF 的路已经没了**（见本文件顶上那段 ⚠️⚠️）。今天
    //     minItems 只在回来之后由 `checked_output` 拦一道「项目太少」：
    //     模型写少了不会被逼着凑字，而是整章当场作废。压力换了个方向，
    //     这个数照旧是这个数——它管的仍然是段长。
    // 二、于是降到一半。字段名没了（实跑 0 段），但模型立刻改成"少写几段、
    //     每段写长"：段长中位从 70 涨到 99，而量过的真实网文是 33、
    //     传统小说 60，我们算段数用的 kCharsPerParagraph 是 45。
    // 三、试过用 items.maxLength 把段压短——**阴性结果**，只削长尾不动
    //     中位数，还会把句子从中间切断。见那一段注释。
    //
    // 四、于是提到四分之三（每场约 16 段），赌"它逼的是把同样的内容拆开写"。
    //     **赌输了，当天就撤。** 段长确实降到 59（好），但一跑里整章丢了
    //     一章：模型写着写着没话说了，语法不让它停，于是反复尝试跳到下一个
    //     字段，把 token 烧光、JSON 没收尾。落盘里那一段长这样：
    //         ],\"goal' : ' ', ' obs tac le': '', ' tur n''''],
    //         \"goal' : ' ', ' obst ac le': '', ' turn''''],…
    //     （加了 DRY 之后它每次还得拼得不一样，才出现 obs tac le /
    //      obst ac le / obsta cle 这种畸形写法。）
    //     **模型没话说的时候，多给它几个格子只会得到几格垃圾。**
    //
    // 所以停在一半。段长那件事得换条路：它是"一段里塞了几件事"的问题，
    // 不是"格子不够多"的问题，拿 minItems 硬掰会掰到没内容可填。
    const int min_items = std::max(6, paras_per_scene / 2 - 1);
    const int max_items = std::max(min_items + 6, paras_per_scene * 3 / 2);

    const ordered schema = [&] {
        // **顺序就是生成顺序。** 模型顺着往下写，先把这一场的底子填掉，
        // 后面那几百字才有地方落；turn 也排在正文前面，它才写得到那儿去。
        ordered scene_props = ordered::object();
        scene_props["where"] = {
            {"type", "string"},
            {"description", "这一场在哪、什么时候、什么光"},
            {"minLength", 4}};
        scene_props["pov"] = {
            {"type", "string"},
            {"description", "这一场跟着谁走。只有他心里想什么可以写"},
            {"minLength", 1}};
        // 单人场是合法的：独处时仍然可以有实时行动、电话或环境阻力。
        // 对白比例由正文守卫检查，不再靠凭空塞第二个人来保证。
        scene_props["who"] = {
            {"type", "array"},
            {"description",
             "这一场真正出现在场上的人物，一人一项，写人物表里的名字。独处场填一人，不要为了凑数虚构第二个人"},
            {"minItems", 1},
            {"maxItems", 3},
            {"items", {{"type", "string"}, {"minLength", 1}}}};
        scene_props["goal"] = {
            {"type", "string"},
            {"description", "他在这一场里想做成什么"},
            {"minLength", 4}};
        scene_props["obstacle"] = {
            {"type", "string"},
            {"description", "谁、什么拦着他"},
            {"minLength", 4}};
        // **排在 turn 前面。** 先定这一场把局面推到多糟，turn 才是那件事
        // 落地的那一刻；反过来的话 turn 已经写完了，worse 只能事后补一个
        // 说法，而补出来的多半是 turn 换个说法。
        scene_props["worse"] = {
            {"type", "string"},
            {"description",
             "这一场收场时，局面比开场时更糟在哪儿：多了一个麻烦、少了一条退路、或者观众多知道了一件让人更担心的事。**每一场都要比上一场更糟**——三场都停在同一个僵持上，就是原地打转，成片里这三场看着一模一样"},
            {"minLength", 8}};
        scene_props["turn"] = {
            {"type", "string"},
            {"description",
             "这一场结束时局面变成什么，而且要悬着。它就是这一场的收口，也是把人拉着往下看的那一下——**要拍得出来**：一个动作、一句说出口的话、一样刚被看见的东西。「谁意识到了什么」「谁明白了什么」这种在脑子里发生的事不算，镜头拍不到。把事情了结掉的也不算"},
            {"minLength", 6}};
        // ⚠️ **给一个字段规定形状，会把整段生成都推向那个形状。**
        //
        // 这是 2026-09-12 连着四轮量出来的，四轮的数摆在一起才看得见：
        //
        //   轮次  last_line 的描述        四章对白段         钩子形态
        //    8    只说「动作或台词」      44/50/22/25       动作台词混合
        //    9    ＋「是台词就带引号」    18/29/22/23       6 台词 1 动作
        //   10    ＋「三种形状轮着来」    17/11/17/30       5 动作 4 台词
        //   11    压缩＋对白那条提前      19/24/21/37       7 动作 1 台词
        //   12    退回第 8 轮那版          46/26/38/11       5 台词 4 动作
        //
        // 第九轮起 last_line 一带上形状指令，**整章跟着往那个形状偏**：
        // 要求带引号就通篇往台词走、给三种形状就通篇往动作描写走，而对白
        // 比例被这个偏向带着掉了一半。第十一轮先按「长描述挤掉短描述」去
        // 修（把对白那条提到最前、给出具体的量），只回收了一部分；第十二轮
        // 把形状指令整个撤掉，两项当场都回来了——**所以不是挤占，是外溢**。
        //
        // **这一栏的描述别再加料。** 要管整场戏的形状就去管 paragraphs
        // 那条，那才是它该待的地方。
        scene_props[kChapterBodyField] = {
            {"type", "array"},
            {"description",
             "这一场的正文，一段一项。**这一场里至少三成的段落要有人说话**，对白和动作交替着来：连着三段没人开口，画面就停住了——这段正文是剧本的原料，成片里对白要占七成以上的时间，正文里没有的话下一步只能凭空编。一段推进一两秒钟的事：一个动作、一句话、一次看见。小说体，不是梗概也不是分镜，长短要错落。写到 turn 发生那一刻就停"},
            {"minItems", min_items},
            {"maxItems", max_items},
            // **每段至少 20 字，这是字数唯一管用的杠杆。** 2026-09-11 量过：
            // 12 出 1500~1900 字，20 出 2000~2700。当时退回 16，是因为 20
            // 会让模型在最后一段拿「”'”””””」凑数——现在 strip_quote_runs
            // 把那种尾巴清掉了，所以回到 20。2026-09-12 实跑四章稳定只写到
            // 目标的一半（1100~1800 对 3000），就是被 16 压着的。
            //
            // **每段最少 4 个字。2026-09-12 晚从 20 降下来的，这是段长
            // 那件事的真正答案。**
            //
            // 20 这个数当初是拿来"拧字数"的，注释里白纸黑字写着
            // 「12 出 1500~1900，20 出 2000~2700」。代价一直没人算：
            // **它在明令禁止短对白**。「"别碰。"」只有 5 个字，当年语法不让
            // 它收引号，模型只能挂一个动作或者一句体感上去才凑得到 20。
            // （今天没有语法了：minLength 是随 schema 贴在提示词里的一句话，
            // 而回来之后那道本地校验**明说了写短了不拦**。它从"禁止"降成了
            // "劝"——但劝一个错的数照样能把短对白劝没，所以这个 4 还是 4。）
            //
            // 实测 319 段的分布，20 处是一堵墙：
            //     0~ 9 字   1 段
            //    10~19 字   4 段     ← 加起来才 1.6%
            //    20~29 字  52 段     ← 墙
            //    30~39 字  56 段
            // 而量过的真实网文段长中位是 33，里面**大量是短对白**。
            //
            // 这一条同时解释了另外两件一直没想通的事：
            // · "体感套话"（他感到一阵…、指节泛白）为什么是涨得最多的一项
            //   ——那是短句被迫凑长度时挂上去的；
            // · 压 maxLength 为什么动不了中位数（300→150 时 78→83）
            //   ——决定分布形状的是地板不是天花板。
            //
            // 4 足够挡住一两个字的垃圾，又放得进「"走吧。"」这种真台词。
            //
            // 每段最长 300 字：真实网文最长的段也就三百来字。实跑时模型把
            // 一千多字的自言自语塞进了一段（见 parse_chapter 里那道闸）。
            //
            // ⚠️ **别拿它当"段长"的旋钮，试过了，是阴性结果。**
            // 2026-09-12 把它压到 150 想把段长中位从 99 拉回 45，实跑：
            //   上限 300：62 段，中位 78，九分位 151，最长 300，超 150 的 17 段
            //   上限 150：62 段，中位 83，九分位 120，最长 150，超 150 的  0 段
            // **中位数纹丝不动**——它只削掉长尾，削不动主峰。而且一旦真卡住
            // 就是把句子从中间切断：那一跑里唯一贴着 150 的段收在
            // 「…吻，"我需要你在接下来的三十分钟内，」——逗号后面没了。
            // maxLength 的活儿是拦住病态的千字长段（它本来就是干这个的），
            // 不是塑形。塑形要动的是段**数**，见 min_items 那段。
            {"items", {{"type", "string"}, {"minLength", 4}, {"maxLength", 300}}}};
        // **最后一句单独成一栏，排在 paragraphs 后面。**
        //
        // 「写完 turn 就停，别再加一段点题」这句话说了三遍——提示词里一条、
        // schema 描述里一句、解析时一道软闸——三道都没拦住：实跑里一半的
        // 章还是在 turn 后面补一段「那一刻，她终于可以告诉自己……」，
        // 而那一段正好落在这一场该收口的地方，悬念当场被填平。
        //
        // 禁令拦不住就别再加第四道。**把最后一句抬成一个字段，当年语法里就
        // 没有位置再写下一段了**——和「一章分几场」是同一个办法：那时候管得
        // 住 14B 的不是措辞，是它没得选。
        //
        // ⚠️ **今天它有得选了。** GBNF 随本地那条路一起删了（见本文件顶上那段
        // ⚠️⚠️），模型完全可以在 paragraphs 里先补一段点题、再老实填上
        // last_line。这一栏今天还剩两样：它在 `required` 里，缺了或者留空会被
        // `checked_output` 拦下；以及它给"这一场该停在哪"一个明确的位置。
        // 真正拦点题的是解析时那道 `on_the_nose` 闸——**别看着这段话以为结构
        // 已经拦住了，就去把那道闸撤了。**
        //
        // 短：网文的章末钩子占最后一两百字，一句话的量。
        //
        // 下限 12 → 4（2026-09-12 晚，理由同 paragraphs）：一场的收口
        // 常常就是一句短台词——「"走吧。"」四个字，12 的下限直接把它禁掉，
        // 逼模型在钩子后面再挂半句，而挂上去的那半句正好是在填平悬念。
        scene_props["last_line"] = {
            {"type", "string"},
            {"description",
             "这一场的最后一句：局面变成那样的**那一刻**。一个动作，或者一句说出口的话。写完它这一场就结束了——不要写回头总结的句子。这里要的是正文，不是说明"},
            {"minLength", 4},
            {"maxLength", 120}};

        ordered props = ordered::object();
        props[kChapterScenesField] = {
            {"type", "array"},
            {"description",
             "这一章的几场戏，按先后。一场 = 一个地方、一段连着的时间里，谁想做成一件事、谁拦着、最后局面变了"},
            {"minItems", min_scenes},
            {"maxItems", max_scenes},
            {"items", {{"type", "object"},
                       {"properties", scene_props},
                       {"required", {"where", "pov", "who", "goal", "obstacle",
                                     "worse", "turn", kChapterBodyField,
                                     "last_line"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {kChapterScenesField};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::string build_chapter_prompt(const Story& story,
                                 const std::string& chapter_id,
                                 StyleLine style_line) {
    const int idx = index_of(story, chapter_id);
    if (idx < 0) throw StoryError("没有这一章：" + chapter_id);
    const Chapter& me = story.chapters[static_cast<std::size_t>(idx)];

    std::string out;
    out += prompt::chapter_write::kSeg0;
    out += style_line == StyleLine::ANIME ? prompt::chapter_write::kHintAnime
                                          : prompt::chapter_write::kHintRealistic;
    out += prompt::chapter_write::kSeg1;
    out += std::to_string(chapter_target_scenes(story));
    out += prompt::chapter_write::kSeg1b;
    out += std::to_string(chapter_scene_chars(story));
    out += prompt::chapter_write::kSeg2;
    out += prompt::chapter_write::kRules;
    out += prompt::chapter_write::kContextHead;

    // ---- 压缩的全局记忆 ----
    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    // **题材原来根本没渲染进来。** 2026-09-12 实跑：大纲把规则怪谈写对了
    // （规则、命案、顶罪、监控录像，伏笔前后咬合），而正文出来是都市情感
    // 的腔调——这一步既没拿到 genre（压根没渲染），又拿不到 tone（大纲
    // 交了空串）。下面那几十条写作规矩是照都市情感调出来的，没有题材顶着，
    // 模型就按那几条的默认口味写。
    if (!story.genre.empty()) {
        out += "【题材】";
        out += story.genre;
        out += "。**下面那些写作规矩，照这个题材的路数用**——悬疑里「拦着他的」";
        out += "是线索断了或者有人瞒着，「局面更糟」是又少一条退路；";
        out += "不是每种故事都在谈感情。\n";
    }
    out += "\n【人物】\n";
    for (const auto& c : story.characters) {
        out += c.name;
        if (!c.identity.empty()) out += "：" + c.identity;
        out += "。";
        if (!c.want.empty()) out += "他要的是：" + c.want + "。";
        // **怕什么和要什么一样重要**：人物的核心驱动力往往不是欲望而是
        // 恐惧（为什么见 models/story.hpp 的 StoryCharacter::fear）。这一步
        // 拿不到的话就是「小传里有、戏里没有」，人物行为看着突兀。
        if (!c.fear.empty()) out += "他怕的是：" + c.fear + "。";
        // **说话方式要摆出来。** 不给的话正文里每个人的台词都是同一个人
        // 写的——而人物在戏里唯一开口的地方就是对白，立不立得住基本就
        // 看这个。
        if (!c.voice.empty()) out += "说话：" + c.voice + "。";
        // **弧光原来从没进过这一步。** 大纲里填了、人物表里存着，而写正文
        // 的提示词里没有——模型写每一章时不知道这个人要往哪儿走，于是每章
        // 都在原地反应。落地的方式不是让谁说「我不一样了」，是同一种处境
        // 下他这次的选择和上次相反，读者自己看出来。
        if (!c.arc.empty()) out += "他会从" + c.arc + "。";
        out += "\n";
    }
    if (!story.relations.empty()) {
        out += "\n【关系】\n";
        for (const auto& r : story.relations) {
            out += r.a + " — " + r.b;
            if (!r.kind.empty()) out += "：" + r.kind;
            out += "。";
            if (!r.tension.empty()) out += r.tension + "。";
            out += "\n";
        }
    }
    // ---- 地方：**只给这一章用得着的那几个** ----
    //
    // 原来发的是全片清单，然后在硬性要求里花一条（原第 14 条）叫模型
    // 「用得着哪一两个就只写那一两个；跑遍全城，说明是在拿地点凑场数」。
    // 那是拿措辞去治一个**给多了**造成的问题：二十个地方摆在眼前，模型
    // 自然会用。章自己就带着 `locations`（读故事那条路上它在 required 里、
    // minItems 1，见 stages/story_analyze.cpp），照它筛就是了——上下文小
    // 一截，那条规则也跟着没了。
    //
    // **筛不出来就照旧给全份**：大纲那条路上这一栏是"给了就收、没给也不拦"
    // （story_outline.cpp 里那段写着为什么不敢收紧），空着的时候宁可多给，
    // 也不能让这一章没有地方可写。那时候把那句提醒贴在这儿——**贴在名单
    // 旁边比写在两千字之前的规则表里管用**。
    if (!story.locations.empty()) {
        std::vector<const models::StoryLocation*> use;
        for (const auto& l : story.locations) {
            for (const auto& want : me.locations) {
                if (l.name == want) { use.push_back(&l); break; }
            }
        }
        const bool all = use.empty();
        out += all ? "\n【地方】（整个故事的清单，不是这一章的；用得着哪一两个"
                     "就只写那一两个，跑遍全城说明是在拿地点凑场数）\n"
                   : "\n【地方】（这一章用得着的）\n";
        if (all) {
            for (const auto& l : story.locations) use.push_back(&l);
        }
        for (const models::StoryLocation* l : use) {
            out += l->name;
            if (!l->what.empty()) out += "：" + l->what;
            out += "。";
            if (!l->when.empty()) out += l->when + "。";
            out += "\n";
        }
    }

    // ---- 前情：之前每章一句 ----
    //
    // 不是前面所有章的正文。那和一章接一章往下写的失忆是同一个道理——二十章的正文
    // 谁也塞不下，截断之后早的那些照样丢。
    std::string recap;
    for (int i = 0; i < idx; ++i) {
        const Chapter& c = story.chapters[static_cast<std::size_t>(i)];
        recap += std::to_string(i + 1) + " " + c.title;
        if (!c.summary.empty()) recap += "：" + text::collapse_ws(c.summary);
        recap += "\n";
    }
    // **前面几章埋下的东西，单拎一份出来。**
    //
    // 混在前情提要里模型看不见——前情是「已经发生过的，不要重写」，而埋下
    // 的东西恰恰是**还没兑现、等着这一章或后面某一章去收**的。不单列的话
    // 每一章的反转都是当场冒出来的，观众没有「原来如此」那一下。
    std::string planted;
    for (int i = 0; i < idx; ++i) {
        const Chapter& c = story.chapters[static_cast<std::size_t>(i)];
        if (c.plant.empty()) continue;
        planted += "· " + c.plant + "（第 " + std::to_string(i + 1) + " 章埋的）\n";
    }

    if (!recap.empty()) {
        out += "\n【前情提要】（已经发生过的，不要重写）\n";
        out += text::truncate_utf8(recap, prompt::chapter_write::kRecapMaxChars);
    }

    // ---- 上一章的结尾，用来接语气 ----
    if (idx > 0) {
        const Chapter& prev = story.chapters[static_cast<std::size_t>(idx - 1)];
        const std::string tail = text::strip_ws(prev.text);
        if (!tail.empty()) {
            out += "\n【上一章是这么结束的】\n";
            const std::vector<std::string> chars = text::utf8_chars(tail);
            std::string piece;
            const std::size_t from =
                chars.size() > prompt::chapter_write::kPrevTailMaxChars
                    ? chars.size() - prompt::chapter_write::kPrevTailMaxChars
                    : 0;
            for (std::size_t i = from; i < chars.size(); ++i) piece += chars[i];
            out += text::strip_ws(piece);
            out += "\n";
        }
        // **上一章停在哪，单说一句。** 2026-09-12 实跑：第二章把第一章的
        // 第一场原样重演了一遍，两章第一场的 turn 一字不差。原文的结尾
        // 贴在上面它也看得到，但那是一段叙述，读不出"故事已经走到这儿了"；
        // 把最后一场的 turn 单拎出来，起点才是明确的。
        if (!prev.scenes.empty() && !prev.scenes.back().turn.empty()) {
            out += "**上一章停在这件事上：" + prev.scenes.back().turn +
                   "。这一章从它之后接着往下走，不要把它再演一遍。**\n";
        }
    }

    if (idx == 0) out += prompt::chapter_write::kFirstHead;

    // ---- 这一章要写的 ----
    out += "\n【这一章】" + me.title + "\n";
    if (!me.summary.empty()) out += me.summary + "\n";
    // **这一章抖出来的那件事。** 单拎一行，因为它是这一章存在的理由：
    // 没有它，一章就只是「又见了一面」——2026-09-12 实跑的四章零反转。
    if (!planted.empty()) {
        out += "\n【前面埋下、还没收的】\n";
        out += planted;
        out += "写到用得上的时候就收一样回来——别解释它当初为什么在那儿，";
        out += "让它自己撞上现在这件事。\n";
    }
    if (!me.plant.empty()) {
        out += "\n**这一章要埋下：";
        out += me.plant;
        out += "。放进某一场里，当时不解释、不点破——后面的章会来收它。**\n";
    }
    if (!me.reveal.empty()) {
        out += "**这一章要抖出来的是：" + me.reveal +
               "。它得在某一场里真的发生——让人看见、听见，"
               "不是谁总结一句。**\n";
    }
    if (!me.hooks.empty() && !me.hooks.back().text.empty()) {
        // **这是最后一场的 turn 该是什么的说法，不是一句正文。** 上一版
        // 写「这一章要停在」，14B 把那行字原样抄成了章尾（见 2026-09-11
        // 的记录）。挂到 turn 那一栏上，它才知道这是要它转写的一件事。
        out += "\n【最后一场的 turn 要落到这件事上】" + me.hooks.back().text + "\n";
    }

    out += prompt::chapter_write::kTail;
    return out;
}

/// 这一章的对白引号要不要换、换哪一对。**判据看整章**，所以先定一次，
/// 再拿定下来的这个去改正文和每一段——两边各判各的话，`d.text` 换了而
/// `scenes[].paragraphs` 没换，后面几道读段落的闸（章尾那句是不是转述、
/// 有没有点题、这一场有没有人说话）看到的仍是旧引号。
enum class QuoteFix { None, CurlySingle, Corner, AsciiSingle };

QuoteFix detect_quote_fix(const std::string& whole) {
    if (whole.find("“") != std::string::npos || whole.find("”") != std::string::npos) {
        return QuoteFix::None;
    }
    // 模型在 JSON 字符串里不敢写 “”（以为要转义），整章对白全用 ‘’ 顶替。
    // 中文小说的对白是 “”，‘’ 只在引号套引号时出现。
    if (whole.find("‘") != std::string::npos) return QuoteFix::CurlySingle;
    // **『』和「」也算。** 2026-09-19 拿 Qwen3-4B 本机真写一章：改稿那一轮
    // 明明按清单把对白补上了，却全写成『这表……能修吗？』——守卫只数 “，
    // 于是这一章在它眼里还是零对白，靠最后一轮软闸放行才落的库。小模型
    // （尤其繁体语料多的）拿角括号当对白引号是常态。整章一个 “ 都没有而有
    // 角括号，就是这种情况；**有 “ 的说明它分得清**，那时候角括号多半是
    // 书名号（「『修表匠』那一章」），换了会把一句话劈成两半。
    if (whole.find("『") != std::string::npos || whole.find("「") != std::string::npos) {
        return QuoteFix::Corner;
    }
    return QuoteFix::AsciiSingle;
}

std::string apply_quote_fix(std::string s, QuoteFix fix) {
    if (fix == QuoteFix::None) return s;
    const auto swap = [&s](const std::string& from, const std::string& to) {
        std::string::size_type i = 0;
        while ((i = s.find(from, i)) != std::string::npos) {
            s.replace(i, from.size(), to);
            i += to.size();
        }
    };
    if (fix == QuoteFix::CurlySingle) {
        swap("‘", "“");
        swap("’", "”");
        return s;
    }
    if (fix == QuoteFix::Corner) {
        swap("『", "“");
        swap("』", "”");
        swap("「", "“");
        swap("」", "”");
        return s;
    }
    // **ASCII 的单引号也算。** 2026-09-12 实跑：整章对白写成 '这一次，
    // 我们不走回头路了。'，上面只认弯引号，于是这一章在守卫眼里是
    // 「一句对白都没有」——被打回两次，第三次宽松放行，而宽松那次连
    // 「两场不能撞同一件事」也一并跳过了，两场的收口一字不差。
    // **一个引号的写法吃掉了两道闸。**
    //
    // 成对才换：单个撇号是英文缩写和所有格（don't、Lin's），换了就成了
    // 半个引号挂在句子中间。
    //
    // `even_only` 给双引号用，理由见下面那段。
    const auto pair_up = [&](char q, bool even_only) {
        std::string out;
        bool open = true;
        int left = 0;
        for (const char c : s) left += (c == q) ? 1 : 0;
        if (left < 2) return;
        if (even_only && left % 2 != 0) return;
        for (const char c : s) {
            if (c != q) {
                out += c;
                continue;
            }
            if (open && left < 2) {  // 落单的那个原样留着
                out += c;
                continue;
            }
            out += open ? "“" : "”";
            open = !open;
            --left;
        }
        s = out;
    };
    // **单引号无条件配，双引号要数成对。**
    //
    // 2026-09-12 曾经把双引号那条整个撤掉，理由写的是「双引号在 JSON 字符串
    // 里还要转义，模型很少写」。**那句话 2026-09-20 被证伪了**：采蒸馏语料时
    // `chapter=low` 下 glm-5.3 的 355 次正文调用里，70 次（20%）把对白写成
    // ASCII 直引号——它不但写，还不转义，整份 JSON 当场断掉（那一层归
    // json_extract 的 escape_stray_quotes 管）。救回来之后闸门又看不见这些
    // 对白，同一章被 no_dialogue 打回：**一个引号的写法吃掉两道闸**，
    // 和当年单引号那次一模一样。
    //
    // 撤掉的那次真正的毛病不是"换双引号"，是**换了不该换的那些**：叙述句里
    // 撒着几个落单的双引号，配对之后「今天是个新起点。“她说给自己听。”只是
    // 我自己走。」——里外颠倒，叙述被劈成对白。落单就是奇数。
    //
    // 所以这次带着两道闸回来，缺一不可：
    //   · 整章一个 “ 都没有（`detect_quote_fix` 已经保证了）——它会写弯引号
    //     的话，剩下那些直引号多半是英寸、代码或者引用，不是对白；
    //   · **数量是偶数**（这儿这条）。奇数就是有落单的，宁可一个都不换。
    pair_up('\'', /*even_only=*/false);
    pair_up('"', /*even_only=*/true);
    return s;
}

/// 整章定一次、正文和每一段一起换。见 detect_quote_fix。
///
/// ⚠️ ASCII 单引号那一档（`pair_up`）是**有状态**的：正文那一遍从头数到尾，
/// 段落那一遍每段各数各的。一段里引号不成对时两边会错开——那一段在正文里
/// 可能被当成"接着上一段的引号"，在自己那一遍里是落单的、原样留着。这不是
/// 新毛病：这之前段落**一个字都没换过**。真要治得让 pair_up 也按整章走一遍
/// 再切回段落，而那要求正文和段落严格同源（`strip_quote_runs` 已经破了这一
/// 条），不值当。
void normalize_quotes(ChapterDraft& d) {
    const QuoteFix fix = detect_quote_fix(d.text);
    if (fix == QuoteFix::None) return;
    d.text = apply_quote_fix(std::move(d.text), fix);
    for (DraftScene& sc : d.scenes) {
        for (std::string& p : sc.paragraphs) p = apply_quote_fix(std::move(p), fix);
    }
}

/// 摘掉分镜的话。
///
/// 「镜头拉远」「画面渐暗」「镜头定格在上面那行字」——这是分镜的语言，
/// 不是小说的语言，而后面另有一步专门把正文变成拍子。提示词里第 18 条
/// 写了不要这么写，但 2026-09-12 量方差那两跑里，一跑干净、另一跑又冒
/// 出来了：**规则不是没写，是在方差里时有时无。**
///
/// 按小句摘，不摘整段：「纸条落在收银台的一角，镜头定格在上面那行字」
/// 这样的段落里，前半句是正经正文，整段丢掉就把内容一起丢了。
///
/// **词表只收连着的词组，不收单字。** 「画面」「镜头」单独出现常常是正当
/// 的——照片的画面、摄影机的镜头都是实物。
// 逐段清洗这一串的顺序：
//   strip_json_echo      模型把 JSON 片段抄进正文
//   fix_unpaired_quotes  只有右引号、没有左引号
//   strip_camera_talk    「镜头拉远」这类分镜的话
//   strip_emphasis       markdown 的 ** __
//   strip_list_marker    行首的 - * + •（见 stages/script.hpp）
//
// **最后那一道是 2026-09-13 补的。** 实跑正文里出现过
// 「--1层停尸间的门再次打开……」——第一个减号是 markdown 列表符号，
// 第二个是负一层的负号。它会一路走进钩子的说法、剧本和字幕。
// 能安全用在这儿，靠的是 strip_list_marker 里那条「后面紧跟数字的减号
// 是符号」：两轮下来削掉第一个、留住第二个，正好。

/// 摘掉 markdown 的强调标记，**只去标记，留文字**。
///
/// 2026-09-13 实跑：`内容只有四个字：**“别多管闲事。”**`。标记本身会原样
/// 落进正文、钩子的说法和字幕。
///
/// 和引用块那条的分寸不一样：引用块**整段都不是小说**，直接摘段；强调标记
/// 出现在**正常段落**里，摘段会把内容一起摘掉，所以只去符号。
///
/// **成对才去**：落单的那个多半是内容里本来就有的（`3*4`），去掉是改字。
static std::string strip_emphasis(const std::string& para) {
    static const char* kMarks[] = {"**", "__"};
    std::string out = para;
    for (const char* m : kMarks) {
        const std::string mark = m;
        std::size_t n = 0;
        for (std::size_t i = 0; (i = out.find(mark, i)) != std::string::npos;
             i += mark.size()) {
            ++n;
        }
        if (n < 2) continue;
        const std::size_t drop = n - (n % 2);   // 多出来的落单那个留着
        std::string cleaned;
        std::size_t at = 0, done = 0;
        while (at < out.size()) {
            if (done < drop && out.compare(at, mark.size(), mark) == 0) {
                at += mark.size();
                ++done;
                continue;
            }
            cleaned += out[at];
            ++at;
        }
        out = std::move(cleaned);
    }
    return out;
}

static std::string strip_camera_talk(const std::string& para) {
    static const char* kCamera[] = {
        "镜头拉远", "镜头拉近", "镜头定格", "镜头切", "镜头对准", "镜头扫过",
        "画面切到", "画面切换", "画面渐暗", "画面定格", "画面淡出",
        "特写镜头", "闪回画面", "切入画面",
    };
    const auto dirty = [&](const std::string& s) {
        for (const char* w : kCamera) {
            if (s.find(w) != std::string::npos) return true;
        }
        return false;
    };
    if (!dirty(para)) return para;

    // 按逗号和句号切小句，逐句筛，再拼回去。
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i < para.size();) {
        if (para.compare(i, 3, "，") == 0 || para.compare(i, 3, "。") == 0) {
            cur += para.substr(i, 3);
            parts.push_back(cur);
            cur.clear();
            i += 3;
            continue;
        }
        cur += para[i];
        ++i;
    }
    if (!cur.empty()) parts.push_back(cur);

    std::string out;
    for (const std::string& one : parts) {
        if (dirty(one)) continue;
        out += one;
    }
    out = text::strip_ws(out);
    // 摘完剩不下什么就还回原样：宁可留一句分镜话，也别把一段摘成半截。
    return text::utf8_len(out) >= 8 ? out : para;
}

/// 一段里只有右引号、没有左引号时，把第一个补成左引号。
///
/// 2026-09-12 实跑：最后一场的收尾是 `苏妍点头微笑。”好的。”`——两个都是
/// 右引号。normalize_quotes 只在**整章**没有 “ 时才动手，而这一章别处是
/// 正常的，所以这一段漏过去了，原样落进正文、落进钩子的说法、落进字幕。
///
/// 只在这一段里左右数目不齐、而右引号是偶数个时才修：那就是「成对的，
/// 只是左边那个写错了」。别的情况不碰——引号跨段的写法（一个人连说几段）
/// 在中文小说里是正当的。
static std::string fix_unpaired_quotes(const std::string& para) {
    int left = 0;
    int right = 0;
    for (std::size_t i = 0; i + 2 < para.size() + 1;) {
        if (para.compare(i, 3, "“") == 0) { ++left; i += 3; continue; }
        if (para.compare(i, 3, "”") == 0) { ++right; i += 3; continue; }
        ++i;
    }
    if (left > 0 || right < 2 || right % 2 != 0) return para;
    std::string out = para;
    const std::size_t at = out.find("”");
    if (at != std::string::npos) out.replace(at, std::string("”").size(), "“");
    return out;
}

/// 当年语法卡死了每段的最短长度，模型想在下限之前收口时会用一串引号凑数
/// （实跑：「……面对一切了。”'”””””」）。中文正文里不存在三个以上连着的
/// 引号，整串删掉，一个两个的照旧。
///
/// GBNF 那条路已经没了（见本文件顶上那段 ⚠️⚠️），逼它凑数的压力小了；
/// 这道清洗留着，因为 minLength 照旧写在提示词里劝着它。
static std::string strip_quote_runs(const std::string& s) {
    const auto is_quote = [](const std::string& ch) {
        return ch == "“" || ch == "”" || ch == "‘" || ch == "’" || ch == "\"" ||
               ch == "'";
    };
    const std::vector<std::string> chars = text::utf8_chars(s);
    std::string out;
    std::size_t i = 0;
    while (i < chars.size()) {
        if (!is_quote(chars[i])) {
            out += chars[i++];
            continue;
        }
        std::size_t j = i;
        while (j < chars.size() && is_quote(chars[j])) ++j;
        if (j - i < 3) {
            for (std::size_t k = i; k < j; ++k) out += chars[k];
        }
        i = j;
    }
    return out;
}

/// 把模型当成正文写出来的 **JSON 字段名** 摘掉。
///
/// 2026-09-12 实跑逮到的，用户先看出来的（"好多明显是凑字数的字符"）：
/// 一场的内容讲完了，但 paragraphs 的 minItems 还没满，而**当年**语法里没有
/// "结束数组"这个选项——于是模型把下一个字段名当成一段正文吐出来：
///
///     last_line": "现在，该还债了。"
///     turn_note": 林远主动发起攻击……        （turn_note 根本不是我们的字段）
///     last_line": null}]}] }                （一章里一连 22 段都是这个）
///
/// 三跑一共 1715 段，29 段是这样，占 1.69%；最糟的一章占了 22 段。
/// 32B 那边也有，不是哪个模型独有的毛病，是闸和内容量对不上。
///
/// GBNF 删掉之后（见本文件顶上那段 ⚠️⚠️）没有东西再逼它接着吐了，这一档的
/// 量应该降了——**但没量过**，所以这道清洗留着。要撤先去 llm_log 里数一数
/// 今天还有没有。
///
/// **能救的救，救不了的丢。** 冒号后面还有真话的，把字段名那截摘掉、
/// 留下正文（上面头两条，那本来就是这一场该有的最后一句）；后面只剩
/// JSON 标点的，整段丢掉。根子在 minItems 太高，已经一起调低了——
/// 这道只是兜底，分寸和 strip_quote_runs 一样：能就地修好的别打回。
static std::string strip_json_echo(const std::string& para) {
    // 头上必须是 ASCII 标识符——中文正文不会长这样，误伤不了。
    std::size_t i = 0;
    while (i < para.size() &&
           (std::isalnum(static_cast<unsigned char>(para[i])) != 0 ||
            para[i] == '_')) {
        ++i;
    }
    if (i == 0) return para;
    std::size_t j = i;
    if (j < para.size() && para[j] == '"') ++j;   // 它常把那半个引号也带出来
    if (j < para.size() && para[j] == ':') {
        ++j;
    } else if (para.compare(j, 3, "：") == 0) {
        j += 3;
    } else {
        return para;   // 没冒号，那就是普通正文，别动
    }

    std::string rest = text::strip_ws(para.substr(j));
    // 后面只剩 JSON 的标点和 null：整段都是垃圾，丢掉。
    // 中文是多字节的，落不进这个集合，所以真正文不会被判成标点。
    if (rest.find_first_not_of("nul{}[],:; \t\"'") == std::string::npos) {
        return std::string();
    }
    // 剩下的是真正文。外面那层半角引号是 JSON 的，摘掉；中文引号是
    // 对白自己的，留着。
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
        rest = text::strip_ws(rest.substr(1, rest.size() - 2));
    }
    return rest;
}

/// 正文按场次表重拼：场里的段落顺次接起来（分隔符同 `push_text`）。一段都不剩的
/// 场一起摘掉（理由同收稿时「一段都没写出来的场不留」）。
///
/// ⚠️ **有场次表的时候，场次表才是正文的源头**：落库时按每一场的段落数数出场的
/// 位置（`apply_chapter`），占位符那一道也是按段落重拼的正文。只改 `d.text` 的清洗
/// 会被后面那一次重拼整个抹掉——复读、成串引号又回到正文里，而守卫量的是清洗过的
/// 那一份，于是按构造就放过去了；场的段落数也和正文对不上，场的位置往后错。
/// 所以有场次表时，每一道清洗都改段落，再拿这个重拼。
static void rebuild_text_from_scenes(ChapterDraft& d) {
    d.scenes.erase(std::remove_if(d.scenes.begin(), d.scenes.end(),
                                  [](const DraftScene& sc) { return sc.paragraphs.empty(); }),
                   d.scenes.end());
    std::string t;
    for (const DraftScene& sc : d.scenes) {
        for (const std::string& p : sc.paragraphs) {
            if (!t.empty()) t += "\n";
            t += p;
        }
    }
    d.text = std::move(t);
}

ChapterDraft parse_chapter(const std::string& raw, int min_chars, bool strict) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what(), "bad_json");
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象", "bad_json");

    ChapterDraft d;
    // **能改的闸不各自抛，攒到这儿。** 整章看完一起抛 ChapterRejected，稿子
    // 一并带着——上层拿它改稿而不是重掷（见头文件那段）。先看哪一道没变，
    // 所以清单第一条就是原来会被抛出来的那一道。
    std::vector<ChapterProblem> problems;
    const auto reject = [&problems](const char* code, std::string what) {
        problems.push_back({code, std::move(what)});
    };
    /// 按场去重时丢掉的第一段够长的原样重复（第几场、哪一段），见 `para_dup`。
    std::optional<std::pair<std::size_t, std::string>> dropped_dup;

    // 一段落进正文。**分隔符必须和 JsonFieldStreamer::kArraySeparator 一致**，
    // 否则编辑器里边写边看的那一版和最后落库的段距对不上。
    const auto push_text = [&d](const std::string& one) {
        if (one.empty()) return;
        if (!d.text.empty()) d.text += "\n";
        d.text += one;
    };
    const auto push_para = [&d](const json& p) {
        if (!p.is_string()) return;
        const std::string one = text::strip_ws(p.get<std::string>());
        if (one.empty()) return;
        if (!d.text.empty()) d.text += "\n";
        d.text += one;
    };

    // 现在的形状：scenes[] 一场一项，每一场自己带 paragraphs。
    // 老形状两种都还认——顶层 paragraphs（改 schema 之前的草稿）、
    // 顶层 text 一个字符串（更早的那一版）。
    if (const auto ss = data.find(kChapterScenesField);
        ss != data.end() && ss->is_array()) {
        for (const auto& s : *ss) {
            if (!s.is_object()) continue;
            DraftScene sc;
            sc.where = text::clean_field(get_str(s, "where"));
            sc.pov = text::clean_field(get_str(s, "pov"));
            if (const auto w = s.find("who"); w != s.end() && w->is_array()) {
                for (const auto& n : *w) {
                    if (!n.is_string()) continue;
                    const std::string one = text::clean_field(n.get<std::string>());
                    if (one.empty()) continue;
                    if (!sc.who.empty()) sc.who += "、";
                    sc.who += one;
                }
            } else {
                // 老形状：一个字符串。改 schema 之前存的草稿走这条。
                sc.who = text::clean_field(get_str(s, "who"));
            }
            sc.goal = text::clean_field(get_str(s, "goal"));
            sc.obstacle = text::clean_field(get_str(s, "obstacle"));
            sc.worse = text::clean_field(get_str(s, "worse"));
            sc.turn = text::clean_field(get_str(s, "turn"));
            if (const auto ps = s.find(kChapterBodyField);
                ps != s.end() && ps->is_array()) {
                for (const auto& p : *ps) {
                    if (!p.is_string()) continue;
                    const std::string one = strip_list_marker(
                        strip_emphasis(strip_camera_talk(fix_unpaired_quotes(
                            strip_json_echo(text::strip_ws(p.get<std::string>()))))));
                    if (one.empty()) continue;
                    sc.paragraphs.push_back(one);
                    push_text(one);
                }
            }
            // 最后一句接在这一场的末尾，就是一个普通段落。落库之后没人
            // 分得出它当初是单独一栏——那一栏当年是为了**语法上不给总结留
            // 位置**（今天只剩 required 那一半管用，见 chapter_schema 里
            // last_line 上面那段）。
            if (const auto last = s.find("last_line");
                last != s.end() && last->is_string()) {
                const std::string one = strip_list_marker(
                    strip_emphasis(strip_camera_talk(fix_unpaired_quotes(
                        strip_json_echo(text::strip_ws(last->get<std::string>()))))));
                if (!one.empty()) {
                    sc.paragraphs.push_back(one);
                    push_text(one);
                }
            }
            // 一段都没写出来的场不留：留着的话它在场次表里占一个位置，
            // 而它的起止位置和上一场的末尾重合，钩子的说法会挂到上一场
            // 收尾那一下上去。
            if (sc.paragraphs.empty()) continue;
            d.scenes.push_back(std::move(sc));
        }
    } else if (const auto ps = data.find(kChapterBodyField);
               ps != data.end() && ps->is_array()) {
        for (const auto& p : *ps) push_para(p);
    } else {
        d.text = text::strip_ws(get_str(data, "text"));
    }
    if (!d.scenes.empty()) {
        for (DraftScene& sc : d.scenes) {
            std::vector<std::string> kept;
            for (const std::string& p : sc.paragraphs) {
                std::string one = text::strip_ws(strip_quote_runs(p));
                if (!one.empty()) kept.push_back(std::move(one));
            }
            sc.paragraphs = std::move(kept);
        }
        rebuild_text_from_scenes(d);
    } else {
        d.text = strip_quote_runs(d.text);
    }
    normalize_quotes(d);
    if (d.text.empty()) throw StoryError("大模型没写出正文", "no_body");
    // 失控往下写个没完的时候截住。这段正文会整份存进 story.json，
    // 而且后面每一章的提示词都要读它。
    d.text = text::truncate_utf8(d.text, prompt::chapter_write::kMaxChars);


    // **模型的自言自语：摘掉那一段，别废整章。**
    //
    // JSON 这个壳把它关在字符串里（当年是语法关的，今天是提示词要求的），
    // 它想解释、想纠正自己的时候，那些话就落进
    // 某一段正文——实跑原样：一段 1164 字的「不符合用户要求的“只输出
    // JSON”，请忽略此部分内容……」。字数守卫、复读守卫都抓不到它。
    //
    // 原来是见着就打回，而它是**硬闸**——这是最后一道还能把整章清成 0 字的
    // 闸了（占位符、引号、分镜话、复读都已经改成就地摘）。中文小说正文里
    // 不会出现这些词，认得出、摘得掉：摘掉含它的那一段，剩下的照收。
    {
        static const char* kSelfTalk[] = {"JSON", "json", "请忽略", "用户要求",
                                          "输出应"};
        constexpr char kNewline = '\n';
        const auto is_self_talk = [&](const std::string& s) {
            for (const char* w : kSelfTalk) {
                if (s.find(w) != std::string::npos) return true;
            }
            // **词表认不全，再加一条结构上的。**
            //
            // 2026-09-13 用全新项目跑批量写正文时撞到的：ch02 四十八段里
            // **十五段是 markdown 引用块**，内容是模型在跟自己讨论提示词——
            //
            //   > 注意：根据规则20，每一场都要有人说话。此场景若只有林浅
            //     一人，必须让另一个人进来……
            //   > 重新审视规则：「不要冒出没在人物表里的人」。那么谁可以在场？
            //   > 唯一的办法是：**回忆不是戏**，但**录音**可以是戏的一部分吗？
            //
            // 上面那张词表一个都没命中（没有 JSON、没有"请忽略"），于是
            // 一章五千字里有近三分之一不是小说，还会当原料
            // 喂给剧本改编。
            //
            // 判据取**行首的 markdown 引用符**：中文小说正文里不会用 `>`
            // 开头（引语用「」或引号），这个信号是干净的；而"根据规则N"
            // 那种措辞是开放集合，往词表里补永远补不全。markdown 标题
            // （`## 第三场`）同理。
            //
            // **要逐行看，不能只看开头。** 这个谓词有两个用法：外面拿整章
            // 问一次当闸门，里面再逐段问。锚在字符串开头的话，整章那一次
            // 问到的是第一段正文，闸门永远不开——上面那几个关键词能中，
            // 只是因为 find 是在全文里搜的。
            std::size_t at = 0;
            while (at <= s.size()) {
                std::size_t end = s.find(kNewline, at);
                if (end == std::string::npos) end = s.size();
                const std::string head = text::strip_ws(s.substr(at, end - at));
                if (!head.empty() && head[0] == '>') return true;
                if (head.size() >= 2 && head[0] == '#' &&
                    (head[1] == '#' || head[1] == ' ')) {
                    return true;
                }
                if (end == s.size()) break;
                at = end + 1;
            }
            return false;
        };
        if (is_self_talk(d.text)) {
            for (DraftScene& sc : d.scenes) {
                std::vector<std::string> kept;
                for (std::string& para : sc.paragraphs) {
                    if (!is_self_talk(para)) kept.push_back(std::move(para));
                }
                sc.paragraphs = std::move(kept);
            }
            std::string cleaned;
            std::string line;
            const auto take = [&](const std::string& one) {
                if (text::strip_ws(one).empty() || is_self_talk(one)) return;
                if (!cleaned.empty()) cleaned += "\n";
                cleaned += one;
            };
            for (const char c : d.text) {
                if (c != '\n') {
                    line += c;
                    continue;
                }
                take(line);
                line.clear();
            }
            take(line);
            // **摘完剩不下一半就打回。** 老形状（顶层 text 一个字符串）整份
            // 就是一行，含了那句话整份都会被摘掉；而真出现这种情况，那一份
            // 产出本来也不能用。摘得动的才摘，摘不动的照旧打回重试。
            if (text::utf8_len(cleaned) < text::utf8_len(d.text) / 2) {
                throw StoryError("正文里混进了模型的解释（「请忽略」这一类），"
                                 "而且摘不干净。重试一次",
                                 "model_aside");
            }
            d.text = cleaned;
        }
    }

    // **复读先摘、摘不干净再打回。**
    //
    // 字数守卫抓不住复读：实跑那次写了 1124 字、稳稳过了 600 的下限，而
    // 「你早就走了，我只是还在等。」一字不差出现了八次。只量长度不看内容
    // 的话，这段东西会一路存进 story.json，再写成剧本、排成分镜、配成音、
    // 渲成片——一整条流水线为一段复读机跑了一个多小时。
    //
    // 但**直接打回是硬闸，三次都撞上去那一章就空了**：2026-09-12 把温度
    // 降到 0.5 之后实跑，ch03 的「我只是怕你会后悔。」出现四次，三次尝试
    // 全被这道闸拦下，那一章落成 0 字。降温度本来就更容易走进复读循环，
    // 两件事撞一块了。
    //
    // 所以先摘：同一段原样出现第二次以后的那些直接删掉，剩下的照收。
    // 摘完还判复读（说明是句级的循环，不是整段重复），才打回。
    // 和占位符、引号、分镜话一个路子——能就地修好的别废掉整章。
    // **一字不差的重复段，什么时候都丢——不要等守卫响。**
    //
    // 下面那一整块去重是挂在 `if (!check_repetition(...).ok)` 里面的，
    // 也就是**守卫不响就一次都不跑**。于是轻度复读（没到守卫的阈值）里
    // 一字不差的段落原样留在正文里。2026-09-16 实测 hulian-test ch08：
    //     他微笑，嘴角上扬，风声呼啸，灰尘在光束里漂浮。
    //     他微笑，嘴角上扬，风声呼啸，灰尘在光束里漂浮。
    // 相邻两段一个字都不差，守卫没响，于是它就这么存进了 story.json。
    //
    // 丢一个和前面一字不差的段落，不可能丢错东西——那就是"写了两遍"的
    // 定义。**只挑够长的丢**：短段落原样重复是正当的手法（「"嗯。"」
    // 「他没说话。」），阈值借句级那道的 kRepeatMinSentenceChars。
    if (!d.scenes.empty()) {
        // 有场次表：在段落上丢，再重拼（见 `rebuild_text_from_scenes`）。
        std::set<std::string> seen_para;
        for (std::size_t k = 0; k < d.scenes.size(); ++k) {
            std::vector<std::string> kept;
            for (std::string& p : d.scenes[k].paragraphs) {
                const std::string trimmed = text::strip_ws(p);
                if (trimmed.empty()) continue;
                if (text::utf8_len(trimmed) >= kRepeatMinSentenceChars &&
                    !seen_para.insert(bare(trimmed)).second) {
                    // 丢了照样要记：「整段原样重复」那道软闸（`para_dup`）靠它。
                    // 原来这道闸量的是没去过重的场次表，丢在这儿之后它就量不到了。
                    if (!dropped_dup && text::utf8_len(bare(trimmed)) >= 20) {
                        dropped_dup = std::make_pair(k, trimmed);
                    }
                    continue;
                }
                kept.push_back(trimmed);
            }
            d.scenes[k].paragraphs = std::move(kept);
        }
        rebuild_text_from_scenes(d);
    } else {
        std::set<std::string> seen_para;
        std::string kept;
        std::istringstream in(d.text);
        std::string one;
        while (std::getline(in, one)) {
            const std::string trimmed = text::strip_ws(one);
            if (trimmed.empty()) continue;
            if (text::utf8_len(trimmed) >= kRepeatMinSentenceChars &&
                !seen_para.insert(bare(trimmed)).second) {
                continue;
            }
            if (!kept.empty()) kept += "\n";
            kept += trimmed;
        }
        if (!text::strip_ws(kept).empty()) d.text = kept;
    }

    if (!check_repetition(d.text).ok) {
        // **按句摘，不是按段摘。** 第一版摘的是整段重复，而 2026-09-12
        // 实跑里复读的是**一句话**——「你知道我最恨什么吗？」在三个不同的
        // 段落里各出现一次，段级去重一句都抓不到，守卫照样判，三次撞完
        // 那一章还是空的。摘的粒度必须和守卫量的粒度一样（见 repetition.hpp
        // 的 kRepeatMaxSame，它数的是句）。
        std::map<std::string, int> seen;
        std::set<std::string> seen_para;
        std::string kept;
        std::string line;
        // 一段摘完剩下什么（整段丢掉就回空）。按行摘和按场里的段落摘共用这一个。
        const auto clean_para = [&](const std::string& one) -> std::string {
            if (text::strip_ws(one).empty()) return {};
            // **整段原样重复的，不管句子多短都丢。** 句级那道有 8 字下限
            // （短句重复是正常的），于是「他没说话。」这种整段重复会漏过去
            // ——实跑的重复段中位一直是 1（0~3），就是这么剩下的。
            if (!seen_para.insert(bare(one)).second) return {};
            std::string keep_para;
            for (const std::string& sent : split_sentences(one)) {
                // **键和长度都得用守卫那一个（repeat_key）。**
                //
                // 这儿原来用的是本文件的 `bare()`，它连句内的逗号句号一起
                // 剥掉，而守卫只剥首尾的引号。同一句「他微笑，嘴角上扬。」：
                // 守卫量到 9 个字、够着 8 的下限，数到三次判废；这儿量到
                // 7 个字、够不着下限，**一次都不摘**。那一句于是按构造就是
                // 救不回来的——2026-09-16 实测，ch08 写了十分钟，日志里
                // 「正文在复读：这一句出现了 3 次：「他微笑，嘴角上扬。」」，
                // 整章作废，而重试只是再掷一次骰子。
                //
                // 上面那段注释一直写着"摘的粒度必须和守卫量的粒度一样"——
                // 粒度是一样了，归一函数没一样。现在共用一个：摘完之后守卫
                // 数到的每一句都只剩一次，必过。
                const std::string key = repeat_key(sent);
                // 短句重复是正常的（「我知道。」「为什么？」），不动。
                if (text::utf8_len(key) >= kRepeatMinSentenceChars &&
                    ++seen[key] > 1) {
                    continue;
                }
                keep_para += sent;
            }
            return text::strip_ws(keep_para);
        };
        const auto take_line = [&](const std::string& one) {
            const std::string para = clean_para(one);
            if (para.empty()) return;
            if (!kept.empty()) kept += "\n";
            kept += para;
        };
        if (!d.scenes.empty()) {
            // 有场次表：在场里的段落上摘，再重拼（见 `rebuild_text_from_scenes`）。
            // 规矩同下面按行那一支：摘到只剩一半以下就不动。
            ChapterDraft trial = d;
            for (DraftScene& sc : trial.scenes) {
                std::vector<std::string> keep;
                for (const std::string& p : sc.paragraphs) {
                    std::string para = clean_para(p);
                    if (!para.empty()) keep.push_back(std::move(para));
                }
                sc.paragraphs = std::move(keep);
            }
            rebuild_text_from_scenes(trial);
            if (text::utf8_len(trial.text) >= text::utf8_len(d.text) / 2) {
                d.scenes = std::move(trial.scenes);
                d.text = std::move(trial.text);
            }
        } else {
            for (const char c : d.text) {
                if (c != '\n') {
                    line += c;
                    continue;
                }
                take_line(line);
                line.clear();
            }
            take_line(line);
            // 摘到只剩一半以下就别要了——那说明整章就是一段复读，留着也没用。
            if (text::utf8_len(kept) >= text::utf8_len(d.text) / 2) d.text = kept;
        }
    }

    if (const auto rep = check_repetition(d.text); !rep.ok) {
        reject("repetition", "正文在复读：" + rep.detail + "。只留一处，别处换个说法");
    }

    // **情绪标签不收。** 实跑那一章（chapter_check8 / ch02）1674 个字里，
    // 「神情复杂」「眼中满是惊讶与疑问」「心中涌起难以言喻的情绪」
    // 「内心充满期待」这类说法出现了十几次——它们把感受替读者做完了，
    // 是这份正文读起来像分镜表而不像小说的主要原因之一。提示词里已经列了
    // 这些词，但措辞 14B 不一定听（2026-09-11 的教训），所以这儿再拦一道。
    //
    // 阈值按每千字放宽：偶尔冒一个是行文，成串出现才是这个毛病。全禁的话
    // 14B 会反复撞墙，一章要重试好几轮。
    {
        // 词表和提示词共用 prompts.toml 那一份：两边各抄一份的时候
        // （提示词 11 个、这儿 14 个），模型会因为没被告知的词被打回。
        int hits = 0;
        // 每个词各出现几次，按词表顺序。改稿时要把词一个个点出来——只说
        // 「这类出现了 14 次」，模型不知道去找哪几个。
        std::vector<std::pair<std::string, int>> seen;
        for (const char* w : prompt::chapter_write::kEmotionLabels) {
            int n = 0;
            std::string::size_type i = 0;
            while ((i = d.text.find(w, i)) != std::string::npos) {
                ++n;
                i += std::string(w).size();
            }
            if (n > 0) seen.emplace_back(w, n);
            hits += n;
        }
        const int chars = static_cast<int>(text::utf8_len(d.text));
        // 每千字允许三个，且整章至少要够四个才算数。
        const int budget = std::max(4, chars * 3 / 1000);
        if (strict && hits > budget) {
            std::string listing;
            for (const auto& [w, n] : seen) {
                if (!listing.empty()) listing += "、";
                listing += "「" + w + "」×" + std::to_string(n);
            }
            reject("emotion_label",
                   "正文在贴情绪标签（" + listing + "，共 " + std::to_string(hits) +
                       " 次）：这些词替读者把感受做完了，每一处都改成身体在干什么");
        }
    }

    // **一句对白都没有的不收。** 2026-09-12 实跑四章里有一章通篇零对白
    // （65 段全是叙述），而下一步是把这段正文改成剧本——正文里没人说话，
    // 那一章出来就是默片。和剧本那边「整章一句台词都没有」是同一道闸。
    //
    // **按场查，不按章查。** 第一版只查整章有没有对白，于是出现了「三场里
    // 两场有对白、第三场是一个人在厂房里回忆五百字」——那一场拍出来就是
    // 一段默片，而整章的账是平的，查不出来。
    //
    // **只查按场写回来的那一份。** 老形状（顶层 text / paragraphs）是改
    // schema 之前的草稿和粘贴导入那条路——那些正文不是照着现在这份提示词
    // 写的，拿现在的规矩去卡它们只会把打得开的故事变成打不开的。
    // **允许有一场是安静的，不许多数场都安静。** 第一版是「每一场都必须有
    // 对白」，2026-09-12 实跑当场打脸：四章里三章连着两次被打回，最后是空的
    // ——一章里夹一个内心场是 14B 的常态，为它把整章废掉，得到的是零分而
    // 不是高分。闸门要拦的是「整章没人说话」，不是「有一场没人说话」。
    if (!d.scenes.empty()) {
        int spoken = 0;
        std::string::size_type q = 0;
        while ((q = d.text.find("“", q)) != std::string::npos) {
            ++spoken;
            q += std::string("“").size();
        }
        // **门槛从「整章两处」提到「至少一成的段落有人说话」。**
        //
        // 两处那个下限太松：2026-09-12 三跑的基线里，每一跑都有一章掉到
        // 个位数（4%、1%、25%），而它们都过了两处这道闸。那种章不是没人
        // 在场——`who` 填着两三个名字、梗概本身就是一场对话——是模型**把
        // 对话整段转述掉了**（「他向她解释这些年…」），一句引号都没有。
        // 一成是很低的门槛，真实网文是 13%~42%。
        int paras = 1;
        for (const char c : d.text) paras += (c == '\n') ? 1 : 0;
        // **门槛 10%，试过 8%，退回来了。**
        //
        // 降到 8% 的本意是少烧一次重试（10% 那版生成时间涨了 34%）。
        // 2026-09-12 三跑实测：**没省下，反而更慢**（中位 685 → 825 秒），
        // 而且最低那章的范围从 9~20 松回 0~23——有一跑里两章对白直接 0%，
        // 正是这道闸本来要拦的东西。门槛低一点，模型就在线下方多待一会儿，
        // 重试反而更多。
        const bool too_few = spoken * 10 < paras;
        if (strict && (spoken < 2 || too_few) && text::utf8_len(d.text) > 400) {
            // 点名哪几场一句都没有：改稿时模型该去的正是那几场。
            std::string silent;
            for (std::size_t k = 0; k < d.scenes.size(); ++k) {
                bool any = false;
                for (const auto& p : d.scenes[k].paragraphs) {
                    if (p.find("“") != std::string::npos) {
                        any = true;
                        break;
                    }
                }
                if (any) continue;
                if (!silent.empty()) silent += "、";
                silent += std::to_string(k + 1);
            }
            reject("no_dialogue",
                   "整章几乎没有对白（" + std::to_string(paras) + " 段里只有 " +
                       std::to_string(spoken) + " 处" +
                       (silent.empty() ? std::string()
                                       : "，第 " + silent + " 场一句都没有") +
                       "）：对话被整段转述掉了，要把话原样写出来，带 “” 引号");
        }
    }

    // **两场不能停在同一件事上。** 场的 turn 就是这一场的收口，两场撞车
    // 观众看到的是剧情在原地打转。连正文也一起比：实跑那次是场 1 的
    // 结尾那句话，被场 3 拿去当了 turn。
    for (std::size_t k = 0; strict && k < d.scenes.size(); ++k) {
        const std::string& t = d.scenes[k].turn;
        if (t.empty()) continue;
        for (std::size_t j = 0; j < k; ++j) {
            // 一场只记一条：撞了就不再往前比，清单上一场一行就够模型改。
            if (same_beat(t, d.scenes[j].turn)) {
                reject("scene_turn_dup",
                       "第 " + std::to_string(j + 1) + " 场和第 " + std::to_string(k + 1) +
                           " 场停在同一件事上（「" + t +
                           "」）：这两场的收口会一模一样，给第 " + std::to_string(k + 1) +
                           " 场换一个更糟的局面收口");
                break;
            }
            bool replayed = false;
            for (const auto& p : d.scenes[j].paragraphs) {
                if (!same_beat(t, p)) continue;
                reject("turn_replayed",
                       "第 " + std::to_string(k + 1) + " 场的 turn 是第 " +
                           std::to_string(j + 1) + " 场里已经写过的那句话（「" + t +
                           "」）：同一件事演了两遍，第 " + std::to_string(k + 1) +
                           " 场要往它之后走");
                replayed = true;
                break;
            }
            if (replayed) break;
        }
    }

    // **「turn 拍不出来就打回」这道闸试过，撤了。**
    //
    // turn 是这一场的结尾，而「她意识到自己一直被骗」这种事发生在脑子里，
    // 镜头拍不到——道理是对的，schema 的描述和提示词里都写了这条，而且
    // 有效：写进描述那一轮，12 章全部停在场尾，钩子是「手指在塑料瓶上
    // 停顿了一瞬」「他将伞轻轻放回地上」这种具体动作。
    //
    // 但再加一道守卫去硬拦就**赔本**了：2026-09-12 实跑，一章三次都撞在
    // 它上面，生成时间从 375 秒涨到 785 秒，而最后一次宽松放行，turn 照样
    // 是「林遥开始怀疑」——闸在烧时间，不改结果。**模型满足不了的软闸
    // 只是把一次生成变成三次生成。**

    // **整段原样重复不收（软闸）。** 复读守卫要一句出现三次才响，而实跑
    // 里常见的是同一段二十几个字的话在一章里出现**两次**——够不着那道
    // 闸，却已经是读者能看出来的原地打转。
    if (strict && dropped_dup) {
        reject("para_dup",
               "有一段原样写了两遍（第 " + std::to_string(dropped_dup->first + 1) +
                   " 场里的「" + text::truncate_utf8(dropped_dup->second, 24) +
                   "…」）：一章里同一段话不该出现第二次，把后一处改掉");
    } else if (strict) {
        std::set<std::string> seen;
        bool found = false;
        for (std::size_t k = 0; k < d.scenes.size() && !found; ++k) {
            for (const std::string& p : d.scenes[k].paragraphs) {
                const std::string key = bare(p);
                if (text::utf8_len(key) < 20) continue;  // 短句重复是正常的
                if (seen.insert(key).second) continue;
                reject("para_dup",
                       "有一段原样写了两遍（第 " + std::to_string(k + 1) + " 场里的「" +
                           text::truncate_utf8(p, 24) +
                           "…」）：一章里同一段话不该出现第二次，把后一处改掉");
                found = true;
                break;
            }
        }
    }

    // **收尾写成转述不收（软闸）。**
    //
    // 2026-09-12 实跑出来的两个钩子：「他低声说了一句，没人回答。」和
    // 「他又低声说了一句，语气比刚才更坚定。」——**都没说他说了什么**。
    // 收尾那一句是这一章的钩子，观众听不见那句话，这一章就等于没有结尾；
    // 行业里管这叫水台词：重复、拖沓、无信息量。
    //
    // **这一道是守卫不是描述。** 第九到十二轮量过：往 last_line 的描述里
    // 加形状指令（「是台词就带引号」）会外溢，把整章都推向台词、对白比例
    // 反而塌掉。守卫只在写完之后判，不参与生成，没有那个副作用。
    //
    // 词表只收**明说了「说了一句话」却不给内容**的那几种写法，模型改起来
    // 很容易（把那句话写出来就行）——不是它满足不了的那种闸。
    if (strict) {
        static const char* kHearsay[] = {"说了一句", "说了什么", "说了些什么",
                                         "问了一句", "回了一句", "应了一句",
                                         "开口说了", "低声说了"};
        // **只管每章最后一场。** 第一版每场都管，实跑生成时间翻倍
        // （420 → 850 秒）——模型一直写转述、一直被打回，而按第三轮记下的
        // 判据，时间翻倍就说明它满足不了。章尾那个钩子最要紧（它是这一章
        // 留给下一章的悬念，也是这一章的落点），先只守它，看能不能
        // 把重试砍掉大半而保住对白下限。
        if (!d.scenes.empty() && !d.scenes.back().paragraphs.empty()) {
            const std::string& last = d.scenes.back().paragraphs.back();
            if (last.find("“") == std::string::npos) {  // 话没写出来
                for (const char* w : kHearsay) {
                    if (last.find(w) == std::string::npos) continue;
                    reject("hearsay_hook",
                           std::string("章尾那一句只说了「") + w +
                               "」，没说那句话是什么：这是这一章留给下一章的悬念，"
                               "观众得听见那句话——把它原样写出来，带 “” 引号");
                    break;
                }
            }
        }
    }

    // **章尾点题不收（软闸）。** 每一场的最后一段应该就是那个 turn，
    // 写到它发生那一刻就停。实跑里模型有一半的章会在 turn 后面再加一段
    // 总结——「那一刻，林夏知道……」「窗外的雨还在下，却再也无法打湿
    // 她的心」——那种句子一出来，悬念当场被填平，而那一段正好落在这一场
    // 该收口的地方。
    if (strict && !d.scenes.empty()) {
        static const char* kWrapUp[] = {"终于", "从此", "那一刻", "这一刻",
                                        "但至少", "再也", "明白了", "放下了",
                                        "释然"};
        for (std::size_t k = 0; k < d.scenes.size(); ++k) {
            const DraftScene& sc = d.scenes[k];
            if (sc.paragraphs.empty()) continue;
            const std::string& last = sc.paragraphs.back();
            for (const char* w : kWrapUp) {
                if (last.find(w) == std::string::npos) continue;
                reject("on_the_nose",
                       "第 " + std::to_string(k + 1) + " 场的最后一段在点题（「" + w +
                           "」，那一段是「" + text::truncate_utf8(last, 24) +
                           "…」）：写到 turn 发生那一刻就该停，后面那一段会把悬念填平，"
                           "把它删掉或者改成一个动作");
                break;
            }
        }
    }

    // **占位符摘掉，不废整章。**
    //
    // 2026-09-12 实跑：schema 里写了「最后一段就是上面那个 turn」，模型
    // 把它当成元指令，最后一段吐出来的是
    // `turn_sentence_from_above_repeated_but_in_correct_place`——一串下划线
    // 连着的英文，原样落进正文、落进钩子、落进字幕。
    //
    // 第一版是见着就打回。**那是错的**：它是硬闸，三次尝试全撞上去就落成
    // 0 字——同一天实跑，一轮里 ch03、ch04 两章都这么没的。而这种垃圾是
    // **认得出、摘得掉**的：中文正文里不会出现十六个连着的英文字母或
    // 下划线。摘掉那一段，剩下的照收；真摘到不够长了，下面的字数闸会拦。
    //
    // 分寸和 strip_quote_runs 一样：能就地修好的别打回，打回的代价是整章。
    {
        const auto has_placeholder = [](const std::string& s) {
            int run = 0;
            for (const char c : s) {
                const bool wordish = (c >= 'a' && c <= 'z') ||
                                     (c >= 'A' && c <= 'Z') || c == '_';
                run = wordish ? run + 1 : 0;
                if (run >= 16) return true;
            }
            return false;
        };
        if (has_placeholder(d.text)) {
            // 场次表里先摘，正文按剩下的段落重拼——**两边必须一起摘**，
            // 只摘正文的话场的段落数就对不上了，而场的位置正是按段落数
            // 数出来的（见 apply_chapter）。
            for (DraftScene& sc : d.scenes) {
                std::vector<std::string> kept;
                for (std::string& p : sc.paragraphs) {
                    if (!has_placeholder(p)) kept.push_back(std::move(p));
                }
                sc.paragraphs = std::move(kept);
            }
            std::string cleaned;
            const auto keep_line = [&](const std::string& line) {
                if (line.empty() || has_placeholder(line)) return;
                if (!cleaned.empty()) cleaned += "\n";
                cleaned += line;
            };
            if (!d.scenes.empty()) {
                for (const DraftScene& sc : d.scenes) {
                    for (const std::string& p : sc.paragraphs) keep_line(p);
                }
            } else {
                // 老形状（没有 scenes）：按行摘。
                std::string line;
                for (const char c : d.text) {
                    if (c != '\n') {
                        line += c;
                        continue;
                    }
                    keep_line(line);
                    line.clear();
                }
                keep_line(line);
            }
            d.text = cleaned;
        }
    }

    // **字数闸摆在所有就地清洗之后。** 原来它排在最前头：后面摘自言自语、摘复读、
    // 摘占位符、按场次表重拼，摘到不够长的一章它早就量过了、放过去了——而占位
    // 符那一段的注释写着"摘到不够长了，下面的字数闸会拦"，下面其实没有闸。
    //
    // 重拼会把前面那一刀截长也抹掉，这儿再截一次。
    d.text = text::truncate_utf8(d.text, prompt::chapter_write::kMaxChars);
    // **短得离谱的不收。** 见 kChapterMinRatio：模型会把章标题填进正文
    // 字段，一两个字也是合法 JSON，静默存下去的话故事看着有几章、
    // 实际全是空壳，到写剧本那一步才发现无米下锅。
    const int got = static_cast<int>(text::utf8_len(d.text));
    if (min_chars > 0 && got < min_chars) {
        reject("too_short", "正文只写出 " + std::to_string(got) + " 个字，至少要 " +
                                std::to_string(min_chars) +
                                " 个：每一场都要写成场面，靠对白往返和感官细节撑起来，"
                                "不是多塞几件事");
    }

    const auto hooks = data.find("hooks");
    if (hooks != data.end() && hooks->is_array()) {
        for (const auto& h : *hooks) {
            DraftHook dh;
            dh.text = text::clean_field(get_str(h, "text"));
            dh.after = text::strip_ws(get_str(h, "after"));
            if (dh.text.empty()) continue;
            d.hooks.push_back(std::move(dh));
        }
    }
    // 老形状：只有一个 hook_after，说法在大纲那一章上。留着是因为改 schema
    // 之前存下来的草稿还可能走到这儿。
    const std::string legacy = text::strip_ws(get_str(data, "hook_after"));
    if (d.hooks.empty() && !legacy.empty()) {
        DraftHook dh;
        dh.after = legacy;
        d.hooks.push_back(std::move(dh));
    }
    if (!problems.empty()) throw ChapterRejected(std::move(d), std::move(problems));
    return d;
}

Story apply_chapter(const Story& story, const std::string& chapter_id,
                    const ChapterDraft& draft) {
    Story out = story;
    Chapter* me = out.chapter_by_id(chapter_id);
    if (me == nullptr) throw StoryError("没有这一章：" + chapter_id);

    // **这一章不能停在上一章已经停过的地方。** 2026-09-12 实跑：第三章的
    // 第一场把第二章的末场原样重演了一遍，两场的 turn 一字不差——切出来
    // ep08 和 ep09 的钩子完全一样。提示词里已经把上一章的 turn 单拎了
    // 一行出来说"别再演一遍"，14B 照样演，所以这儿再拦一道。
    //
    // 放在 apply 而不是 parse：只有这里才看得到上一章。
    {
        const int idx = index_of(out, chapter_id);
        if (idx > 0) {
            const Chapter& prev = out.chapters[static_cast<std::size_t>(idx - 1)];
            if (!prev.scenes.empty()) {
                const std::string& last = prev.scenes.back().turn;
                for (const auto& sc : draft.scenes) {
                    if (last.empty() || !scenes_repeat_beat(sc.turn, last)) continue;
                    throw ChapterRejected(
                        draft, {{"turn_repeats_prev",
                                 "这一章又停在上一章停过的地方（「" + sc.turn +
                                     "」）：上一章就是这么结束的，这一场要从它之后往下走，"
                                     "换一件新发生的事收口"}});
                }
            }
        }
    }

    // 大纲那个钩子的说法要留着——它是**这一章整体**该停在哪，和中间几场
    // 收在哪不是一回事，所以它归章尾。
    std::string chapter_hook;
    for (const auto& h : me->hooks) {
        if (!h.text.empty()) chapter_hook = h.text;
    }

    me->text = draft.text;

    // **钩子全部重建，别删这一步。** 原来那些位置是对着空正文算出来的
    // （大纲阶段一律 at_char = 0），正文落进去之后它们一个都不成立了：
    // 留着就等于声称这一章第一个字那儿悬着一件事。
    //
    // 2026-09-18 查过一遍今天谁按位置读它：按时长切段那条链拔掉之后，
    // 拿 at_char 说事的只剩 story_analyze——用户粘正文进来时，那头把读出来
    // 的说法往**已有候选**上对位（按 at_char 找，找不到才新加一条，和下面
    // 那个 put 同一套）。一堆钉死在 0 上的假位置会把对位全带偏，而这件事
    // 一声不响。
    me->hooks = paragraph_hooks(me->text);
    const int len = me->text_len();

    // **场的位置是数出来的，不是模型报的。** 正文就是各场的段落顺次拼起来
    // 的，所以第几段结束就是第几场结束——`paragraph_breaks` 给的是每个段落
    // 边界的字符位置，累加段数一查就得到。
    //
    // 上一版靠模型抄一句原文回来（DraftHook::after），程序再去正文里查，
    // 抄错一个字那一场就落不到位置上，只能挂在一个说不出为什么的段落边界上。
    me->scenes.clear();
    // 每一场的收尾那一句，和 me->scenes 一一对应。下面拿它当钩子的说法。
    std::vector<std::string> scene_closing;
    if (!draft.scenes.empty()) {
        const std::vector<int> breaks = paragraph_breaks(text::strip_ws(me->text));
        int para = 0;
        int from = 0;
        for (std::size_t i = 0; i < draft.scenes.size(); ++i) {
            const DraftScene& sc = draft.scenes[i];
            para += static_cast<int>(sc.paragraphs.size());
            // 最后一场一律收在章尾：中间那些段落数对得上，末尾多一段少一段
            // （空段被丢掉、正文被截断）都不该让最后一场停在正文中间。
            int to = len;
            if (i + 1 < draft.scenes.size() && para >= 1 &&
                para - 1 < static_cast<int>(breaks.size())) {
                to = breaks[static_cast<std::size_t>(para - 1)];
            }
            to = std::clamp(to, from, len);
            if (to <= from) continue;  // 截断之后落在同一个点上的场不留
            Scene s;
            s.from_char = from;
            s.to_char = to;
            s.where = sc.where;
            s.pov = sc.pov;
            s.who = sc.who;
            s.goal = sc.goal;
            s.obstacle = sc.obstacle;
            s.worse = sc.worse;
            s.turn = sc.turn;
            me->scenes.push_back(std::move(s));
            scene_closing.push_back(sc.paragraphs.back());
            from = to;
        }
    }

    // 在已有候选上补说法；那个位置还没有候选就新加一个。
    const auto put = [&](int at, const std::string& why) {
        if (why.empty()) return;
        if (at < 0 || at > len) at = len;
        for (auto& h : me->hooks) {
            if (h.at_char == at) {
                // 同一个位置已经有说法了就不覆盖：先到的是模型按先后给的，
                // 后到的多半是章尾那一个，盖掉等于把中间那一场的说法丢了。
                if (h.text.empty()) h.text = why;
                return;
            }
        }
        Hook h;
        h.at_char = at;
        h.text = why;
        me->hooks.push_back(std::move(h));
    };

    // **每一场的末尾就是一个有说法的切点**，而说法用的是**那一场的收尾
    // 那一句**，不是 turn。
    //
    // 两栏都在，选收尾那一句是因为它可靠得多。同一场实跑出来的两者：
    //   turn：「他意识到自己当年可能误解了事情的真相」
    //   收尾：「我以为你是走了，我才……」
    // turn 老是写成在脑子里发生的事——schema 的描述里要求过「要拍得出
    // 来」，管一阵子又退回去，而且一章里两场的 turn 常常是同一件事换个
    // 说法（ep01 和 ep03 的钩子几乎一模一样）。收尾那一句没有这个毛病：
    // 它被 last_line 那一栏约束成「turn 发生的那一刻」，必然是一个动作
    // 或者一句说出口的话，而且每一场各不相同。
    //
    // 行业上分场大纲的钩子习惯写成一句概括（「男主发现真相」），好给后面
    // 的编剧留空间。**这条流水线是反的**：正文已经写完了，钩子的用处是
    // 告诉写剧本那一步「这一场停在哪」，以及给人看——那句实际的收尾比
    // 概括准。turn 照旧存在场次表里，写剧本那一步拿得到。
    for (std::size_t i = 0; i < me->scenes.size(); ++i) {
        const std::string& closing =
            i < scene_closing.size() ? scene_closing[i] : std::string();
        put(me->scenes[i].to_char,
            closing.empty() ? me->scenes[i].turn : closing);
    }

    for (const auto& dh : draft.hooks) {
        // 查不到就不放：一章有好几个钩子，查不到的那个要是都堆到章尾，
        // 章尾会被一个中间情节的说法占掉。**只有章尾那一个值得兜底。**
        const int at = find_after(me->text, dh.after);
        if (at >= 0) put(at, dh.text);
    }
    // 章尾兜底：大纲给的那句挂上去，模型自己标了章尾就不动它。
    put(len, chapter_hook);

    return out;
}

// ---------------------------------------------------------------------------
// 改稿：把坏在第几处回给模型
// ---------------------------------------------------------------------------

namespace {

/// 打回那句话：第一条毛病，多于一条时说一声还有几处。
std::string rejected_summary(const std::vector<ChapterProblem>& ps) {
    if (ps.empty()) return "稿子被打回，但没说为什么";
    std::string s = ps.front().what;
    if (ps.size() > 1) {
        s += "（另有 " + std::to_string(ps.size() - 1) + " 处，见改稿清单）";
    }
    return s;
}

/// 「甲、乙、丙」拆回数组。parse_chapter 收 who 时是这么拼的。
std::vector<std::string> split_names(const std::string& joined) {
    std::vector<std::string> out;
    const std::string sep = "、";
    std::string::size_type at = 0;
    while (at <= joined.size()) {
        const auto next = joined.find(sep, at);
        const std::string one =
            text::strip_ws(joined.substr(at, next == std::string::npos ? std::string::npos
                                                                        : next - at));
        if (!one.empty()) out.push_back(one);
        if (next == std::string::npos) break;
        at = next + sep.size();
    }
    return out;
}

}  // namespace

ChapterRejected::ChapterRejected(ChapterDraft draft, std::vector<ChapterProblem> problems)
    : StoryError(rejected_summary(problems),
                 problems.empty() ? std::string() : problems.front().code),
      draft_(std::move(draft)),
      problems_(std::move(problems)) {}

ordered draft_to_json(const ChapterDraft& d) {
    if (d.scenes.empty()) return ordered{{"text", d.text}};
    ordered scenes = ordered::array();
    for (const DraftScene& sc : d.scenes) {
        ordered s = ordered::object();
        s["where"] = sc.where;
        s["pov"] = sc.pov;
        s["who"] = split_names(sc.who);
        s["goal"] = sc.goal;
        s["obstacle"] = sc.obstacle;
        s["worse"] = sc.worse;
        s["turn"] = sc.turn;
        // 收稿时 last_line 接在了 paragraphs 末尾（见 parse_chapter），
        // 还原时拆回去：模型看到的形状要和 schema 一样，它才知道往哪儿填。
        ordered paras = ordered::array();
        for (std::size_t i = 0; i + 1 < sc.paragraphs.size(); ++i) {
            paras.push_back(sc.paragraphs[i]);
        }
        s[kChapterBodyField] = std::move(paras);
        s["last_line"] = sc.paragraphs.empty() ? std::string() : sc.paragraphs.back();
        scenes.push_back(std::move(s));
    }
    return ordered{{kChapterScenesField, std::move(scenes)}};
}

std::string build_chapter_revision_prompt(const Story& story,
                                          const std::string& chapter_id,
                                          StyleLine style_line,
                                          const ChapterDraft& draft,
                                          const std::vector<ChapterProblem>& problems) {
    std::string out = build_chapter_prompt(story, chapter_id, style_line);
    // 「只输出 JSON」那一段要留在最末尾——它后面紧接着贴 schema
    // （llm::schema_as_prompt）。先摘下来，改稿那几段接完再放回去。
    const std::string tail = prompt::chapter_write::kTail;
    if (out.size() >= tail.size() &&
        out.compare(out.size() - tail.size(), tail.size(), tail) == 0) {
        out.erase(out.size() - tail.size());
    }
    out += prompt::chapter_write::kReviseHead;
    // 缩进 0：换行留着（模型好读、人好 diff），缩进的空格不留，和 schema
    // 贴过去那份同一个规矩（CLAUDE.md「量比例再动手」那一节）。
    out += draft_to_json(draft).dump(0);
    out += prompt::chapter_write::kReviseProblemsHead;
    for (std::size_t i = 0; i < problems.size(); ++i) {
        out += std::to_string(i + 1) + ". " + problems[i].what + "\n";
    }
    out += prompt::chapter_write::kReviseTail;
    out += tail;
    return out;
}

}  // namespace changji::stages
