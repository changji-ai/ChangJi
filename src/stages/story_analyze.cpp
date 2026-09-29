#include "stages/story_analyze.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <map>
#include <optional>

#include "stages/json_extract.hpp"
#include "stages/prompts.inc.hpp"
#include "stages/story_outline.hpp"
#include "stages/storyboard.hpp"
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

std::string head_chars(const std::vector<std::string>& chars, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n && i < chars.size(); ++i) out += chars[i];
    return out;
}

std::string tail_chars(const std::vector<std::string>& chars, std::size_t n) {
    std::string out;
    const std::size_t from = chars.size() > n ? chars.size() - n : 0;
    for (std::size_t i = from; i < chars.size(); ++i) out += chars[i];
    return out;
}

/// 模型回的那个 chapter_id 里的章号。
///
/// **2026-09-19 实跑逮到的**：提示词里每章的抬头是「【ch01】错位的发条」，
/// 而模型把 `chapter_id` 整行抄了回来——`chapter_by_id` 当场查不到，那一条
/// 整个丢掉。表现是**读了一遍等于没读**：人物表更新了，章里的梗概、出场人、
/// 钩子一个字没动，而且全程不报错（只发一章过去时尤其容易撞上，它手里没有
/// 第二个样子可以对照）。
///
/// 所以这儿只认 `ch` 加数字那一截，前后有什么都不管。认不出来就原样返回，
/// 交给上面那句"模型编了个不存在的章号"处理。
std::string chapter_id_in(const std::string& raw) {
    const std::string t = text::strip_ws(raw);
    for (std::size_t i = 0; i + 2 < t.size(); ++i) {
        if (t[i] != 'c' || t[i + 1] != 'h') continue;
        std::size_t j = i + 2;
        while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
        if (j > i + 2) return t.substr(i, j - i);
    }
    return t;
}

/// 在正文里找 needle，返回它**结束之后**那个字符位置；找不到返回 -1。
///
/// 按字符比，不按字节：返回值要当 Hook::at_char 用，而那个位置是按字符算的。
int find_after(const std::string& body, const std::string& needle) {
    const std::string n = text::strip_ws(needle);
    if (n.empty()) return -1;
    const std::size_t byte_pos = body.find(n);
    if (byte_pos == std::string::npos) return -1;
    // 字节位置换成字符位置：数前面有多少个 UTF-8 首字节。
    const std::string prefix = body.substr(0, byte_pos + n.size());
    return static_cast<int>(text::utf8_len(prefix));
}

}  // namespace

std::string render_chapters_for_analysis(const Story& story) {
    // **只发写出来的那几章。**
    //
    // 没正文的章发过去只剩一个光标题，而模型会照着标题把这一章**编出来**：
    // 梗概、出场是谁、在哪，一样不少。2026-09-19 实跑那一趟（llm_log 里
    // 那份 story_understanding）：八章只写了第一章，回来八章全带着梗概和
    // 出场人名单，连"在纺织厂梧桐树下挖出铁盒"都有——而那七章一个字都没有。
    // 用户原话：「都没内容怎么就知道这个人会在空白章节中呢」。
    //
    // 读一遍**只能说它读到的**。没写的章不是"读到的空白"，是"没读到"。
    //
    // 顺带把预算给对了：`per` 原来按**全部**章分，七章空的白占着 7/8 的
    // 额度不用，真有正文的那一章反倒被截到八分之一。
    std::vector<const Chapter*> written;
    for (const auto& c : story.chapters) {
        if (!text::strip_ws(c.text).empty()) written.push_back(&c);
    }
    if (written.empty()) return {};

    std::size_t per = prompt::story_analyze::kBudgetChars / written.size();
    if (per < prompt::story_analyze::kMinPerChapter) per = prompt::story_analyze::kMinPerChapter;
    // 开头给得比结尾多：结尾只要够看出钩子落在哪，开头要交代清楚人和处境。
    const std::size_t head_n = per * 2 / 3;
    const std::size_t tail_n = per - head_n;

    std::string out;
    for (const Chapter* cp : written) {
        const Chapter& c = *cp;
        out += "【" + c.chapter_id + "】" + c.title + "\n";
        const std::vector<std::string> chars = text::utf8_chars(c.text);
        if (chars.size() <= per) {
            out += text::strip_ws(c.text);
        } else {
            out += text::strip_ws(head_chars(chars, head_n));
            // 省略号要标出来。不标的话模型会把断开的两段当成连着的一句，
            // 归纳出一个正文里根本没有的情节。
            out += "\n……（中间略）……\n";
            out += text::strip_ws(tail_chars(chars, tail_n));
        }
        out += "\n\n";
    }
    return out;
}

const ordered& analyze_schema() {
    static const ordered schema = [] {
        // 人物、关系、地点三块和大纲那份**故意长一样**：下游（bible、
        // 剧本提示词）认的是同一个形状，两边不一致的话每个消费者都要分支。
        const ordered& outline = outline_schema();
        ordered props = ordered::object();
        props["logline"] = outline.at("properties").at("logline");
        props["genre"] = outline.at("properties").at("genre");
        props["tone"] = outline.at("properties").at("tone");
        props["characters"] = outline.at("properties").at("characters");
        // **形状一样，收谁不一样。** 出大纲是在编故事，三到五个人正好；这儿是
        // 读**已经写好的正文**，里面说过话的人一个都不能漏——名单之外的人说的话，
        // 写剧本时 speaker 只能挑名单里的名字，就挂到了别人头上。2026-09-25 实测：
        // 营门哨兵六句台词没被收进来，剧本里全记成了女主说的，配音就会是她的声音。
        props["characters"]["description"] =
            "正文里所有说过话的人：主要人物、配角，还有有台词的小角色（哨兵、店员、"
            "司机）也要登记——没名字的按身份起名（「哨兵」）。没登记的人说的话，"
            "后面没人认领。只在叙述里提到、一句话没说的人不用登记。";
        props["characters"]["minItems"] = 1;
        props["characters"]["maxItems"] = 12;
        props["relations"] = outline.at("properties").at("relations");
        props["locations"] = outline.at("properties").at("locations");

        ordered chapter_props = ordered::object();
        chapter_props["chapter_id"] = {
            {"type", "string"}, {"description", "照抄给你的那个，不要改"}};
        chapter_props["summary"] = {
            {"type", "string"}, {"description", "这一章发生了什么，三五句，归纳不是摘抄"}};
        ordered hook_props = ordered::object();
        hook_props["text"] = {
            {"type", "string"},
            {"description", "这里悬着的是什么：悬念、反转，或明确的情绪落点"}};
        hook_props["after"] = {
            {"type", "string"},
            {"description",
             "这个位置前面那句的原文，照抄十到二十个字。程序靠它在正文里定位"}};
        chapter_props["hooks"] = {
            {"type", "array"},
            {"description",
             "这一章里悬着的地方，按先后排，最后一个必须是章尾。"
             "章尾那一个是这一章停在哪的唯一说法，写剧本那一步照着它收口"},
            {"items", {{"type", "object"},
                       {"properties", hook_props},
                       {"required", {"text", "after"}},
                       {"additionalProperties", false}}}};
        // 同上：没写进 required 的，14B 一律不写。见 story_outline.cpp
        // 里那段——两份 schema 是一起栽的，也要一起改。
        chapter_props["characters"] = {
            {"type", "array"},
            {"description", "这一章出场的人物名，照抄上面登记过的名字"},
            {"minItems", 1},
            {"items", {{"type", "string"}}}};
        chapter_props["locations"] = {
            {"type", "array"},
            {"description", "这一章用到的地点名，照抄上面登记过的名字"},
            {"minItems", 1},
            {"items", {{"type", "string"}}}};

        props["chapters"] = {
            {"type", "array"},
            {"description", "每一章一条，chapter_id 照抄，不要漏章"},
            {"items", {{"type", "object"},
                       {"properties", chapter_props},
                       {"required", {"chapter_id", "summary", "hooks",
                                     "characters", "locations"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        // relations 也在里面：不写进去的话模型给空数组，而**采用那一步会用
        // 它盖掉大纲里已经有的那几条**。2026-09-11 实跑：出大纲给了 3 条
        // 关系，读一遍正文之后变成 0 条——看着像"这个故事没有关系"，其实是
        // 模型压根没写这一项。
        s["required"] = {"characters", "chapters", "locations", "relations"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::string build_analyze_prompt(const Story& story, StyleLine style_line) {
    std::string out;
    out += prompt::story_analyze::kSeg0;
    out += style_line == StyleLine::ANIME ? prompt::story_analyze::kHintAnime
                                          : prompt::story_analyze::kHintRealistic;
    out += prompt::story_analyze::kSeg1;
    out += prompt::story_analyze::kRules;
    out += render_known_names(story);
    out += prompt::story_analyze::kChaptersHead;
    out += render_chapters_for_analysis(story);
    out += prompt::story_analyze::kTail;
    return out;
}

std::string render_known_names(const Story& story) {
    // **把已有的名单给它看。** 不给的话它对着正文自己起名，同一个地方换个叫法
    //（2026-09-27「走路带风」：大纲的「城西人才市场」被读成「人才市场大厅」），
    // 名单就成了两份。名字在眼前，它照抄的多；照抄不到的那几个，落库那头再接
    //（apply_analysis 里那段）。没名单（粘贴导入的故事）就一个字不加。
    std::string out;
    std::vector<std::string> chars;
    for (const auto& c : story.characters) {
        if (!text::strip_ws(c.name).empty()) chars.push_back(c.name);
    }
    if (!chars.empty()) {
        out += prompt::story_analyze::kKnownCharactersHead;
        for (std::size_t i = 0; i < chars.size(); ++i) {
            if (i > 0) out += "、";
            out += chars[i];
        }
        out += "\n";
    }
    bool any = false;
    for (const auto& l : story.locations) {
        if (text::strip_ws(l.name).empty()) continue;
        if (!any) {
            out += prompt::story_analyze::kKnownLocationsHead;
            any = true;
        }
        out += l.name;
        if (!l.what.empty()) out += "：" + l.what;
        out += "。";
        if (!l.when.empty()) out += l.when + "。";
        out += "\n";
    }
    return out;
}

std::map<std::string, std::string> unify_story_locations(Story& story) {
    // 存量故事里同一个地方两个叫法（大纲一份、理解一份）的，收成一份：名字取
    // 先出现的那个（大纲的在前，章节和剧本引的是它），说明空着的用后一条补上；
    // 章节里引到别名的改成名单上的名字。判据同 match_place_name。
    std::map<std::string, std::string> alias;
    std::vector<StoryLocation> kept;
    std::vector<std::string> names;
    for (const StoryLocation& l : story.locations) {
        const std::string name = text::strip_ws(l.name);
        if (name.empty()) continue;
        std::optional<std::string> canon;
        for (const std::string& k : names) {
            if (k == name) {
                canon = k;
                break;
            }
        }
        if (!canon.has_value()) canon = match_place_name(name, names);
        if (!canon.has_value()) {
            names.push_back(name);
            StoryLocation c = l;
            c.name = name;
            kept.push_back(std::move(c));
            continue;
        }
        if (*canon != l.name) alias[l.name] = *canon;
        auto it = std::find_if(kept.begin(), kept.end(),
                               [&](const StoryLocation& k) { return k.name == *canon; });
        if (it == kept.end()) continue;
        if (it->what.empty() && !l.what.empty()) it->what = l.what;
        if (it->when.empty() && !l.when.empty()) it->when = l.when;
    }
    story.locations = std::move(kept);
    for (Chapter& ch : story.chapters) {
        std::vector<std::string> fixed;
        std::set<std::string> seen;
        for (const std::string& n : ch.locations) {
            const auto al = alias.find(n);
            const std::string m = al == alias.end() ? n : al->second;
            if (seen.insert(m).second) fixed.push_back(m);
        }
        ch.locations = std::move(fixed);
    }
    return alias;
}

int canonicalize_location_names(models::AssetLibrary& lib,
                                const std::vector<std::string>& names) {
    // 资产里叫别名的场景改回名单上的名字（id 不动，镜头上的引用不受影响）。
    // 改完同名的那几条由 dedupe_locations 按名字收拢。
    int renamed = 0;
    for (auto& [id, loc] : lib.locations) {
        const std::string name = text::strip_ws(loc.name);
        if (name.empty()) continue;
        bool exact = false;
        for (const std::string& n : names) {
            if (text::strip_ws(n) == name) {
                exact = true;
                break;
            }
        }
        if (exact) continue;
        const auto canon = match_place_name(name, names);
        if (!canon.has_value() || *canon == name) continue;
        loc.name = *canon;
        ++renamed;
    }
    return renamed;
}

Story apply_analysis(const Story& story, const std::string& raw, bool overwrite) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");

    // 从原来那份出发：正文、章名、章节 id、章节计划全部照旧。
    Story out = story;
    // **不勾就不顶**：这三栏填着字的时候，这一趟读出来的不算数。整段规矩
    // 在 story_analyze.hpp 上 `overwrite` 那一段。
    const auto take = [&](std::string& dst, const char* key) {
        const std::string v = text::clean_field(get_str(data, key));
        if (v.empty()) return;
        if (!overwrite && !text::strip_ws(dst).empty()) return;
        dst = v;
    };
    take(out.logline, "logline");
    take(out.genre, "genre");
    take(out.tone, "tone");

    // `names` 是**合并之后的全名单**：下面 relations 和每一章的出场人都拿
    // 它过滤，漏掉旧人的话，不勾那一档会把旧章的出场人全过滤没。
    std::set<std::string> names;
    std::vector<StoryCharacter> keep_chars;
    if (!overwrite) {
        for (const StoryCharacter& c : out.characters) {
            if (c.name.empty() || !names.insert(c.name).second) continue;
            keep_chars.push_back(c);
        }
    }
    out.characters = std::move(keep_chars);   // 勾了就是空的，同原来的 clear()
    // **守卫钉的是"这一趟读出来几个人"，不是合并完剩几个。** 不勾那一档
    // 旧名单本来就在，拿合并后的数去判，模型返回空数组也能蒙混过去——而
    // 那正是这条守卫要抓的（见 test_story_outline「没读出人物」那条）。
    std::set<std::string> read_names;
    const auto chars = data.find("characters");
    if (chars != data.end() && chars->is_array()) {
        for (const auto& c : *chars) {
            StoryCharacter sc;
            sc.name = text::clean_field(get_str(c, "name"));
            if (sc.name.empty() || !read_names.insert(sc.name).second) continue;
            // 和旧名单重名：**旧的那条算数**，这一条整条丢掉。人物的身份、
            // 欲望、说话方式是人会去手改的东西。
            if (!names.insert(sc.name).second) continue;
            sc.identity = text::clean_field(get_str(c, "identity"));
            sc.want = text::clean_field(get_str(c, "want"));
            sc.fear = text::clean_field(get_str(c, "fear"));
            sc.arc = text::clean_field(get_str(c, "arc"));
            sc.voice = text::clean_field(get_str(c, "voice"));
            out.characters.push_back(std::move(sc));
        }
    }
    if (read_names.empty()) {
        throw StoryError("大模型没从正文里读出任何人物");
    }

    // 关系按**无序对**判重：「前任」这条边 a→b 和 b→a 是同一条，按有序对
    // 收的话不勾那一档每读一遍就多出一条反着写的。
    const auto edge = [](const std::string& a, const std::string& b) {
        return a < b ? a + '\n' + b : b + '\n' + a;
    };
    std::set<std::string> edges;
    std::vector<Relation> keep_rels;
    if (!overwrite) {
        for (const Relation& r : out.relations) {
            if (names.count(r.a) == 0 || names.count(r.b) == 0) continue;
            if (!edges.insert(edge(r.a, r.b)).second) continue;
            keep_rels.push_back(r);
        }
    }
    out.relations = std::move(keep_rels);
    const auto rels = data.find("relations");
    if (rels != data.end() && rels->is_array()) {
        for (const auto& r : *rels) {
            Relation rel;
            rel.a = text::clean_field(get_str(r, "a"));
            rel.b = text::clean_field(get_str(r, "b"));
            // 两端都得是登记过的人，和大纲那条一样：指向不存在的人时，
            // 提示词里会凭空多出一个角色。
            if (names.count(rel.a) == 0 || names.count(rel.b) == 0) continue;
            if (!overwrite && !edges.insert(edge(rel.a, rel.b)).second) continue;
            rel.kind = text::clean_field(get_str(r, "kind"));
            rel.tension = text::clean_field(get_str(r, "tension"));
            out.relations.push_back(std::move(rel));
        }
    }

    std::set<std::string> loc_names;
    // **地方名单只有一份。** 模型读正文时爱给同一个地方换个叫法——2026-09-27
    // 「走路带风」：大纲写的「城西人才市场」，理解那一步回来是「人才市场大厅」，
    // 「军营营区」回来是「营区门口」，「城中村出租屋」回来是「林知夏的出租屋」。
    // 原来这儿按名字全等去重，同一个地方两条都留下：圣经照第二份定妆、剧本照
    // 第一份写场次头，拆分镜时一场都接不上（每一场都被写成「不在清单里」）。
    //
    // 现在回来的名字先往已有名单上接（match_place_name，和场次头接资产库同一套
    // 四手）：接上的就是那个地方，**名字用名单上的**（章节、剧本、资产都认它），
    // what / when 按勾决定顶不顶；接不上才是新地方。勾了「全部重出」名字也不换
    // ——名字是这个地方的身份，换了下游全断；换的只是说明。
    std::vector<std::string> canon_names;   // 名单上的名字，按原顺序
    for (const StoryLocation& l : story.locations) {
        if (!l.name.empty() && loc_names.insert(l.name).second) canon_names.push_back(l.name);
    }
    std::vector<StoryLocation> merged;
    if (!overwrite) merged = story.locations;   // 只补不顶：已有的原样留着
    std::map<std::string, std::string> alias;   // 模型的叫法 → 名单上的名字
    const auto locs = data.find("locations");
    if (locs != data.end() && locs->is_array()) {
        for (const auto& l : *locs) {
            StoryLocation sl;
            sl.name = text::clean_field(get_str(l, "name"));
            if (sl.name.empty()) continue;
            sl.what = text::clean_field(get_str(l, "what"));
            sl.when = text::clean_field(get_str(l, "when"));
            std::optional<std::string> canon;
            if (loc_names.count(sl.name)) {
                canon = sl.name;
            } else {
                canon = match_place_name(sl.name, canon_names);
            }
            if (!canon.has_value()) {
                loc_names.insert(sl.name);
                canon_names.push_back(sl.name);
                merged.push_back(std::move(sl));
                continue;
            }
            if (*canon != sl.name) alias[sl.name] = *canon;
            auto it = std::find_if(merged.begin(), merged.end(),
                                   [&](const StoryLocation& m) { return m.name == *canon; });
            if (it == merged.end()) {
                // 勾了重出：名单上的名字留着，说明用这一趟的。
                sl.name = *canon;
                merged.push_back(std::move(sl));
                continue;
            }
            if (!sl.what.empty() && (overwrite || it->what.empty())) it->what = sl.what;
            if (!sl.when.empty() && (overwrite || it->when.empty())) it->when = sl.when;
        }
    }
    out.locations = std::move(merged);

    const auto chaps = data.find("chapters");
    if (chaps == data.end() || !chaps->is_array()) return out;
    for (const auto& c : *chaps) {
        const std::string id = chapter_id_in(get_str(c, "chapter_id"));
        Chapter* target = out.chapter_by_id(id);
        if (target == nullptr) continue;  // 模型编了个不存在的章号

        // **没正文的章一个字不动。**
        //
        // 这一章根本没发给它（`render_chapters_for_analysis` 只发写出来
        // 的），它还回来一条就是照标题编的。盖上去的后果是三样一起坏：
        // 大纲写的梗概被换成一份没人写过的剧情、出场人名单凭空长出来
        // （用户 2026-09-19：「都没内容怎么就知道这个人会在空白章节中
        // 呢」）、钩子挂在一段不存在的正文上。
        //
        // 挡在这儿而不是只靠上面那半：提示词是"给它看什么"，这儿是
        // "它说什么算数"——两件事。模型顺着 ch01 往下编一个 ch02 出来，
        // 只有这儿拦得住。
        if (text::strip_ws(target->text).empty()) continue;

        // **这一章已经填着的那一栏，不勾就一个字不动。**
        //
        // 用户 2026-09-19：「第一章的梗概被顶掉这个不行」。他的走法是写一章
        // → 理解故事 → 再写一章 → 再点一次：第二下本该只去读新写的那章，
        // 而原来这儿整本重落，第一章的梗概、出场人、地点、钩子全被这一趟的
        // 结果换掉——包括他手改过的。判据逐栏各判各的（不是整章一刀切），
        // 因为新写出正文的那一章通常**梗概已经有了**（大纲阶段写的）而出场
        // 人、地点、钩子还空着，那几栏正是这一下要补的。
        const std::string summary = text::strip_ws(get_str(c, "summary"));
        if (!summary.empty() &&
            (overwrite || text::strip_ws(target->summary).empty())) {
            target->summary = summary;
        }

        // **要填就先清空，而且要去重。**
        //
        // 2026-09-11 实跑出来的样子：`['陈默','林景明','陈默','林景明','苏婉']`，
        // 地点那份还混着同一个地方的两种叫法（"高架桥下那家通宵咖啡馆" 和
        // "高架桥下的咖啡馆"）。原因是两条：一是这儿只 push 不 clear，而
        // 大纲那一步已经往里写过一份了；二是模型自己也会把同一个名字写两遍。
        //
        // 所以这一栏只有两种下场：**整份换掉，或者一个字不动**。往里追加是
        // 第三种，那正是上面那两种重复的来路。
        if (overwrite || target->characters.empty()) {
            target->characters.clear();
            std::set<std::string> seen_chars;
            for (const auto& n : get_str_array(c, "characters")) {
                if (names.count(n) && seen_chars.insert(n).second) {
                    target->characters.push_back(n);
                }
            }
        }
        if (overwrite || target->locations.empty()) {
            target->locations.clear();
            std::set<std::string> seen_locs;
            for (const auto& raw_name : get_str_array(c, "locations")) {
                // 章里写的也归到名单上的名字（上面那段的别名表）。
                const auto al = alias.find(raw_name);
                const std::string n = al == alias.end() ? raw_name : al->second;
                if (loc_names.count(n) && seen_locs.insert(n).second) {
                    target->locations.push_back(n);
                }
            }
        }

        // 钩子那一栏的"空"是**有没有说法**，不是数组空不空：story_reverse
        // 机械登记的段落边界候选只有位置、没有说法，这一章满满一排候选也还
        // 是"没人给过说法"，该填。
        //
        // 反过来，已经有说法时整段跳过——`put` 自己那条"有说法的不覆盖"挡不
        // 住**新加**一条，而钩子一变，commit_story 就会重算章节计划。
        const bool hook_told =
            std::any_of(target->hooks.begin(), target->hooks.end(),
                        [](const Hook& h) { return !text::strip_ws(h.text).empty(); });
        if (!overwrite && hook_told) continue;

        // 钩子落在哪：拿模型抄的那句原文去正文里查。**查不到就丢掉那一条**
        // ——一章有好几个钩子，查不到的全堆到章尾的话，章尾会被一个中间
        // 情节的说法占掉。只有下面那条老形状的单钩子值得兜底。
        const int len = target->text_len();
        const auto put = [&](int at, const std::string& why) {
            if (why.empty() || at < 0 || at > len) return;
            for (auto& h : target->hooks) {
                if (h.at_char == at) {
                    // 已经有说法的不覆盖：先到的是按先后给的那几个，
                    // 盖掉等于把中间那一条钩子的说法丢了。
                    if (h.text.empty()) h.text = why;
                    return;
                }
            }
            Hook h;
            h.at_char = at;
            h.text = why;
            target->hooks.push_back(std::move(h));
        };

        const auto hooks = c.find("hooks");
        if (hooks != c.end() && hooks->is_array()) {
            for (const auto& h : *hooks) {
                const std::string why = text::clean_field(get_str(h, "text"));
                if (why.empty()) continue;
                const int at = find_after(target->text, get_str(h, "after"));
                if (at >= 0) put(at, why);
            }
        }
        // 老形状：单个 hook + hook_after，查不到时兜底挂章尾。
        const std::string legacy = text::clean_field(get_str(c, "hook"));
        if (!legacy.empty()) {
            int at = find_after(target->text, get_str(c, "hook_after"));
            if (at < 0 || at > len) at = len;
            put(at, legacy);
        }
    }

    return out;
}

}  // namespace changji::stages
