#include "stages/story_outline.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "stages/json_extract.hpp"
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 从 JSON 对象里取字符串，缺了或不是字符串就返回空串。
///
/// 和 bible.cpp 的同名函数一个道理：模型漏字段是常态，
/// 缺一个 tone 不该让整份大纲作废。
std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// 取字符串数组，非字符串项跳过。
std::vector<std::string> get_str_array(const json& obj, const char* key) {
    std::vector<std::string> out;
    if (!obj.is_object()) return out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (!v.is_string()) continue;
        const std::string s = text::clean_field(v.get<std::string>());
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

std::string chapter_id(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ch%02zu", i + 1);
    return buf;
}

}  // namespace

const ordered& outline_schema() {
    static const ordered schema = [] {
        ordered character_props = ordered::object();
        character_props["name"] = {
            {"type", "string"}, {"description", "剧本里的中文称呼，全片一字不改"}};
        character_props["identity"] = {
            {"type", "string"}, {"description", "一句话身份。不要写长相、发型、服装"}};
        character_props["want"] = {
            {"type", "string"},
            {"description", "这个人物主动要的东西。写「希望生活变好」等于没写"}};
        // 紧跟着 want：**要什么和怕什么是一对**，挨着填模型才会让它们互相
        // 顶着（他想要的那样东西，正好要他做最怕的那件事）。
        character_props["fear"] = {
            {"type", "string"},
            {"description",
             "他怕什么：怕被谁看见什么、怕失去什么、怕自己其实是什么样的人。不是「怕黑」这种毛病，是会让他在关键时刻躲开、说谎、突然翻脸的那件事"},
            {"minLength", 8}};
        character_props["arc"] = {
            {"type", "string"}, {"description", "从什么变成什么"}};
        // **minLength 不能省。** 写进 required 只保证这个键在，不保证它有
        // 内容——2026-09-12 实跑，三个人物的 voice 全是空串，这一栏等于
        // 没加，而那一轮的「改进」实际上一个字都没生效。空字符串是合法的
        // JSON 字符串，语法采样照样让它过。
        character_props["voice"] = {
            {"type", "string"},
            {"description",
             "他说话什么样：长句还是短句、认不认错、生气时是提高声音还是干脆不说话、有没有嘴上的习惯。写得出来才分得清谁在说话。不要写长相"},
            {"minLength", 8}};

        ordered relation_props = ordered::object();
        relation_props["a"] = {{"type", "string"}, {"description", "人物名，必须在 characters 里"}};
        relation_props["b"] = {{"type", "string"}, {"description", "人物名，必须在 characters 里"}};
        relation_props["kind"] = {{"type", "string"}, {"description", "前任、母女、上下级"}};
        relation_props["tension"] = {
            {"type", "string"},
            {"description",
             "这段关系里绷着的是什么。只写关系名不够——"
             "「前任」不算，「谁欠谁一句没说出口的道歉」才算"}};

        ordered location_props = ordered::object();
        location_props["name"] = {{"type", "string"}, {"description", "中文地点名"}};
        location_props["what"] = {
            {"type", "string"},
            {"description",
             "什么地方，具体到看得见：在当地什么位置、什么时候开放、"
             "进去先感知到什么，写成只可能是这一处的样子"}};
        location_props["when"] = {
            {"type", "string"}, {"description", "什么时间、什么光"}};

        ordered chapter_props = ordered::object();
        chapter_props["title"] = {{"type", "string"}, {"description", "章标题，十个字以内"}};
        // **排在 summary 前面。** 模型顺着往下生成：先定这一章抖出什么，
        // 后面那几句梗概才会围着它写。反过来的话它先把梗概写完，再回头
        // 凑一个「反转」，凑出来的多半是梗概里那件事换个说法。
        chapter_props["reveal"] = {
            {"type", "string"},
            {"description",
             "这一章抖出来的那件新事：读者或人物知道了一件之前不知道的事，或者局面变成了之前不可能的样子。可以是反转（身份、关系、事实、动机），也可以只是一个新的处境。不是「又见了一面」「又谈了一次」"}};
        // 跟在 reveal 后面：这一章抖出什么、埋下什么，是一对。
        chapter_props["plant"] = {
            {"type", "string"},
            {"description",
             "这一章埋下的、后面几章才回收的那样东西：一个物件、一句没头没尾的话、一个当时说不通的细节。埋的时候不解释，读者当时看不出它要紧。**最后一章的 reveal 要回收前面埋的某一样**，不要凭空冒出来。这一章没有要埋的就填空串，别硬凑"}};
        chapter_props["summary"] = {
            {"type", "string"}, {"description", "这一章发生什么，三五句"}};
        chapter_props["hook"] = {
            {"type", "string"},
            {"description", "这一章结束时悬着的那件事：悬念、反转，或明确的情绪落点"}};
        // ⚠️ **没写进 required 的字段，14B 一律不写。**
        //
        // 2026-09-11 实跑：出一份大纲 + 读一遍正文，两边都是
        // `locations: []`、每一章的 `characters` 和 `locations` 也都是空的
        // ——而人物那一项（在 required 里）给了三个、关系给了三条。不是模型
        // 读不懂，是语法采样里那些字段可以合法地不出现，它就不出现。
        //
        // 后果不是"少一点信息"：设定页的「场景」那一格永远是空的，空景图
        // 无从谈起；再往后排分镜时，不知道这一章在哪儿发生、谁在场——
        // 而那正是第二步全部的输入。
        //
        // 所以能定的都写进 required，再加 minItems。见
        // project-ai-chapter-quality 那条：这个体量的模型，只有 schema 管得住。
        // ⚠️ **这两项刻意不写进 required，也不设 minItems。**
        //
        // 试过。加上之后出一份大纲从三十几秒变成 **278 秒，而且最后截断在
        // 半截 JSON 上**（"大模型没有返回对象"）——语法一收紧，14B 就一路
        // 写到 token 上限也收不了口。大纲这一步的产出本来就长（四章，每章
        // 标题、梗概、钩子），再逼它给每章两份名单就过界了。
        //
        // 不加也不亏：每章谁在场、在哪儿，**「读故事」那一步是照着正文读
        // 出来的，比大纲阶段凭空想的准**，而那份 schema 里它们是 required
        // （见 story_analyze.cpp）。大纲这儿给了就收，没给也不拦。
        chapter_props["characters"] = {
            {"type", "array"},
            {"description", "这一章出场的人物名，照抄上面登记过的名字"},
            {"items", {{"type", "string"}}}};
        chapter_props["locations"] = {
            {"type", "array"},
            {"description", "这一章用到的地点名，照抄上面登记过的名字"},
            {"items", {{"type", "string"}}}};

        ordered props = ordered::object();
        // 用户没给梗概时，模型自己定的那个选题写在这里。给了梗概时这一项
        // 会被忽略——不让它改写用户写的那句话。
        props["premise"] = {
            {"type", "string"},
            {"description",
             "这部电影讲什么，一句话，具体到人物和处境。用户已经给了梗概时照抄"}};
        props["logline"] = {{"type", "string"}, {"description", "一句话说清这个故事"}};
        // **必填，而且要有内容。** 2026-09-12 拿一句规则怪谈的梗概实跑，
        // 大纲把规则、命案、顶罪、监控录像全写对了，伏笔也前后咬合，而
        // 正文写出来是都市情感的腔调——查下来根子在这两栏是**空的**：
        // 它们没进 required，模型就不填，而写正文那一步渲染它们时看到空
        // 就跳过，于是**那一步根本不知道这是个悬疑故事**。
        //
        // minLength 不能省：进 required 只保证键在，不保证有内容——
        // voice 那一栏栽过同样的跟头（写进 required 之后三个人物全是空串）。
        props["genre"] = {
            {"type", "string"},
            {"description",
             "题材：都市情感、悬疑、规则怪谈、年代、女性成长……写具体一点，后面每一章的正文都照着它的路数写"},
            {"minLength", 2}};
        props["tone"] = {
            {"type", "string"},
            {"description", "调子：克制、荒诞、冷硬、温吞……一个词到一句话"},
            {"minLength", 2}};
        props["characters"] = {
            {"type", "array"},
            {"description", "故事里的主要人物和重要配角，共三到五人"},
            {"minItems", 3},
            {"maxItems", 5},
            {"items", {{"type", "object"},
                       {"properties", character_props},
                       {"required", {"name", "identity", "want", "fear", "voice"}},
                       {"additionalProperties", false}}}};
        props["relations"] = {
            {"type", "array"},
            {"description", "人物之间有戏的关系，两端都要是上面登记过的人"},
            {"items", {{"type", "object"},
                       {"properties", relation_props},
                       {"required", {"a", "b", "kind", "tension"}},
                       {"additionalProperties", false}}}};
        props["locations"] = {
            {"type", "array"},
            {"description", "故事里的地方。每个都要有人在那儿演戏，别列背景板"},
            {"minItems", 1},
            {"items", {{"type", "object"},
                       {"properties", location_props},
                       {"required", {"name", "what"}},
                       {"additionalProperties", false}}}};
        props["chapters"] = {
            {"type", "array"},
            {"description", "按顺序的章节。最后一章要把主线了结"},
            {"items", {{"type", "object"},
                       {"properties", chapter_props},
                       {"required", {"title", "reveal", "plant", "summary", "hook"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"logline", "genre", "tone", "characters", "chapters",
                         "locations", "relations"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

namespace {

/// 从种子里取第 k 个 0~1 之间的数。
///
/// 和 script.cpp 里那个 frac 是同一个（xorshift 混一道再取模）。**没有合成
/// 一处**：那边混的理由是"`episode_id` 只差一个字"，这边是"一次抽四五样，相邻的 k
/// 不能给出相邻的结果"，两边的需求将来会各自变。两份十行的纯函数比一个
/// 两处都要迁就的公共函数好维护。
double frac(std::uint32_t seed, int k) {
    std::uint32_t x = seed + static_cast<std::uint32_t>(k) * 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<double>(x % 10000u) / 10000.0;
}

/// 按种子从一张词表里挑一项。
template <std::size_t N>
const char* pick(const char* const (&table)[N], std::uint32_t seed, int k) {
    return table[static_cast<std::size_t>(frac(seed, k) * static_cast<double>(N)) % N];
}

/// 按种子挑 `want` 个**不重复**的姓。
///
/// 不用 shuffle：要的只是几个，而 shuffle 要把九十项全排一遍。
/// 撞上重复就往后挪一格——池子比要的数大一个数量级，挪不了几次。
std::vector<std::string> pick_surnames(std::uint32_t seed, std::size_t want) {
    const auto& pool = prompt::story_outline_spark::kSurnames;
    const std::size_t n = std::size(pool);
    want = std::min(want, n);
    std::vector<std::string> out;
    std::set<std::size_t> used;
    for (std::size_t i = 0; out.size() < want; ++i) {
        std::size_t at =
            static_cast<std::size_t>(frac(seed, 100 + static_cast<int>(i)) *
                                     static_cast<double>(n)) % n;
        while (used.count(at) != 0) at = (at + 1) % n;
        used.insert(at);
        out.emplace_back(pool[at]);
    }
    return out;
}

}  // namespace

std::string build_outline_names(std::uint32_t variation) {
    if (variation == 0) return "";
    namespace sp = prompt::story_outline_spark;
    std::string out = sp::kNamesPre;
    const auto names = pick_surnames(variation, sp::kSurnamesPick);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += "、";
        out += names[i];
    }
    out += sp::kNamesMid;
    out += pick(sp::kNameShapes, variation, 7);
    out += "。\n";
    out += sp::kNamesPost;
    return out;
}

std::string build_outline_spark(std::uint32_t variation) {
    if (variation == 0) return "";
    namespace sp = prompt::story_outline_spark;
    std::string out = sp::kSparkHead;
    out += std::string(sp::kSparkField) + pick(sp::kFields, variation, 1) + "\n";
    out += std::string(sp::kSparkBond) + pick(sp::kBonds, variation, 2) + "\n";
    out += std::string(sp::kSparkPressure) + pick(sp::kPressures, variation, 3) + "\n";
    out += std::string(sp::kSparkTone) + pick(sp::kTones, variation, 4);
    out += sp::kSparkTail;
    return out;
}

std::string build_outline_prompt(const std::string& premise, StoryScale scale,
                                 StyleLine style_line,
                                 const std::string& keywords,
                                 std::uint32_t variation, int chapters) {
    const char* hint = style_line == StyleLine::ANIME
                           ? prompt::story_outline::kHintAnime
                           : prompt::story_outline::kHintRealistic;
    std::string out;
    out += prompt::story_outline::kSeg0;
    out += hint;
    out += prompt::story_outline::kSeg1;
    out += std::to_string(chapters > 0 ? chapters : suggested_chapters(scale));
    out += prompt::story_outline::kSeg2;
    out += prompt::story_outline::kRules;
    // 只要一章时把「章数按需要来」那条收回来，见 prompts.toml 那段。
    if (chapters == 1) out += prompt::story_outline::kOneChapter;

    // 姓氏池：**每条路都拼**。名字和用户给的方向不冲突——他写的是故事，
    // 不是花名册；梗概里真提到了名字，拼进去那段话里写着"照用，一个字不改"。
    out += build_outline_names(variation);

    const std::string kw = text::strip_ws(keywords);
    if (!kw.empty()) {
        out += prompt::story_outline::kKeywordsPre;
        out += text::truncate_utf8(kw, prompt::story_outline::kKeywordsMaxChars);
        out += prompt::story_outline::kKeywordsPost;
    }

    // 梗概可空：空着就让模型自己定选题。三个入口里只有「我自己有个想法」
    // 那条是从手写的一句话开始的，把它做成硬门槛等于又把人摁回空白框前面。
    const std::string p = text::strip_ws(premise);
    if (p.empty()) {
        // 选题空间：**只在这儿拼，而且得连关键词也没有。** 用户已经给了方向
        // 的话，再塞一组随机的场域和关系是跟他对着干——他写"外卖员和程序员"，
        // 我们塞"远洋渔船"，出来的东西他认不出是自己要的。
        //
        // 而这条路正是雷同最严重的一条：提示词里一个变量都没有，同一个按钮
        // 按十次，模型拿到的是同一串字节十次。
        if (kw.empty()) out += build_outline_spark(variation);
        out += prompt::story_outline::kNoPremise;
    } else {
        out += prompt::story_outline::kTailHead;
        out += text::truncate_utf8(p, prompt::story_outline::kPremiseMaxChars);
    }
    out += prompt::story_outline::kTailEnd;
    return out;
}

Story parse_outline(const std::string& raw, const std::string& premise,
                    StoryScale scale) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");

    Story story;
    // 用户给了就用用户的，一个字不动；没给才收模型自己定的那个选题。
    // 反过来（总是用模型回的）会让它悄悄改写用户写的那句话。
    std::string used_premise = text::strip_ws(premise);
    if (used_premise.empty()) {
        used_premise = text::strip_ws(get_str(data, "premise"));
    }
    story.premise = text::truncate_utf8(used_premise, 2000);
    story.scale = scale;
    story.source = StorySource::AI;
    story.logline = text::clean_field(get_str(data, "logline"));
    story.genre = text::clean_field(get_str(data, "genre"));
    story.tone = text::clean_field(get_str(data, "tone"));

    // 人物。名字是后面一切的键：分镜提示词按名字找角色，关系按名字连边。
    // 所以重名的只留第一个，没名字的直接丢——留下来会变成一个叫空串的角色。
    std::set<std::string> names;
    const auto chars = data.find("characters");
    if (chars != data.end() && chars->is_array()) {
        for (const auto& c : *chars) {
            if (story.characters.size() >= prompt::story_outline::kMaxCharacters) break;
            StoryCharacter sc;
            sc.name = text::clean_field(get_str(c, "name"));
            if (sc.name.empty() || !names.insert(sc.name).second) continue;
            sc.identity = text::clean_field(get_str(c, "identity"));
            sc.want = text::clean_field(get_str(c, "want"));
            sc.fear = text::clean_field(get_str(c, "fear"));
            sc.arc = text::clean_field(get_str(c, "arc"));
            sc.voice = text::clean_field(get_str(c, "voice"));
            story.characters.push_back(std::move(sc));
        }
    }

    // 关系。两端都得是登记过的人——指向不存在的人时，界面上那条边画不出来，
    // 而提示词里会凭空多出一个角色，那正是要防的漂移。丢掉比留着强。
    const auto rels = data.find("relations");
    if (rels != data.end() && rels->is_array()) {
        for (const auto& r : *rels) {
            Relation rel;
            rel.a = text::clean_field(get_str(r, "a"));
            rel.b = text::clean_field(get_str(r, "b"));
            if (names.count(rel.a) == 0 || names.count(rel.b) == 0) continue;
            rel.kind = text::clean_field(get_str(r, "kind"));
            rel.tension = text::clean_field(get_str(r, "tension"));
            story.relations.push_back(std::move(rel));
        }
    }

    std::set<std::string> loc_names;
    const auto locs = data.find("locations");
    if (locs != data.end() && locs->is_array()) {
        for (const auto& l : *locs) {
            StoryLocation sl;
            sl.name = text::clean_field(get_str(l, "name"));
            if (sl.name.empty() || !loc_names.insert(sl.name).second) continue;
            sl.what = text::clean_field(get_str(l, "what"));
            sl.when = text::clean_field(get_str(l, "when"));
            story.locations.push_back(std::move(sl));
        }
    }

    const auto chaps = data.find("chapters");
    if (chaps == data.end() || !chaps->is_array() || chaps->empty()) {
        throw StoryError("大纲里一章都没有");
    }
    for (const auto& c : *chaps) {
        if (story.chapters.size() >= prompt::story_outline::kMaxChapters) break;
        Chapter ch;
        ch.chapter_id = chapter_id(story.chapters.size());
        // 标题自己带的编号要削掉：界面上本来就有「第 N 章」的前缀，
        // 不削就成了「第 2 章 · 第二部：被折叠的时间」。见 strip_leading_ordinal。
        ch.title = text::strip_leading_ordinal(
            text::clean_field(get_str(c, "title")));
        ch.summary = text::strip_ws(get_str(c, "summary"));
        ch.reveal = text::clean_field(get_str(c, "reveal"));
        ch.plant = text::clean_field(get_str(c, "plant"));
        if (ch.title.empty() && ch.summary.empty()) continue;

        // 大纲阶段没有正文，所以钩子的位置只能是 0——而正文为空时
        // 0 既是章首也是章尾，语义上就是「这一章结束时悬着的那件事」。
        // 校验那边要求 at_char ∈ [0, 正文长度]，这里正好落在边界上。
        const std::string hook = text::clean_field(get_str(c, "hook"));
        if (!hook.empty()) {
            Hook h;
            h.at_char = 0;
            h.text = hook;
            ch.hooks.push_back(std::move(h));
        }

        // 出场人物只留登记过的。模型经常在章节里蹦出一个没在 characters
        // 里登记的名字，留着的话后面按名字找角色会找不到。
        for (const auto& n : get_str_array(c, "characters")) {
            if (names.count(n)) ch.characters.push_back(n);
        }
        for (const auto& n : get_str_array(c, "locations")) {
            if (loc_names.count(n)) ch.locations.push_back(n);
        }
        story.chapters.push_back(std::move(ch));
    }
    if (story.chapters.empty()) throw StoryError("大纲里一章都没有");

    return story;
}

}  // namespace changji::stages
