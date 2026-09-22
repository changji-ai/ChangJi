#include "util/say.hpp"
#include "models/character.hpp"

// 默认画风的词在 prompts.toml 的 [style] 里。这个头只有常量，不依赖 stages 的
// 任何东西，models 层 include 它不算倒挂。
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace changji::models {

namespace {

// rstrip_punct / strip_ws 搬去了 util/text.hpp——分镜阶段解析大模型输出时
// 也要用同一套清洗规则，两份拷贝迟早会分叉，而它们的输出进的是同一个提示词。
using text::rstrip_punct;
using text::strip_ws;

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string sep_for(StyleLine line) {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，用中文逗号。
    return line == StyleLine::ANIME ? ", " : "，";
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool has_prefix_slug(const std::string& s, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (s.size() <= n) return false;
    if (s.compare(0, n, prefix) != 0) return false;
    return std::all_of(s.begin() + static_cast<long>(n), s.end(),
                       [](unsigned char c) {
                           return (c >= 'a' && c <= 'z') ||
                                  (c >= '0' && c <= '9') || c == '_';
                       });
}

// 参考音色的偏好顺序。真正能用哪些由服务端说了算，
// 这里只是没得选时的默认倾向：中文样本优先，其次按性别分。
//
// 踩过的坑（Python 侧注释原样带过来）：写死 vibevoice 的中文样本提交上去，
// 节点校验直接拒绝，整条流水线断在配音这一步。文件在磁盘上，但不在这个
// 节点的列表里。参考音色在 ComfyUI 那边是个下拉框，装了哪些插件就有哪些
// 选项，换一台机器列表就不一样，所以不能假设任何一条路径一定存在。
const std::array<const char*, 6> kFemaleMarks = {
    "female", "woman", "_f_", "belinda", "mabel", "sophie"
};
const std::array<const char*, 7> kMaleMarks = {
    "male", "man", "_m_", "chadwick", "eastwood", "freeman", "attenborough"
};

// ⚠️ **这两张表是拿来认字的，不是拿来显示的**：从中文名字里猜性别
// （「妈」「哥」这些字）。包进 `SAY()` 就是让界面语言决定认不认得出中文
// 名字，而名字是用户自己写在故事里的。一个字都不许动。
const std::array<const char*, 10> kFemaleWords = {
    "女", "妈", "母", "姐", "妹", "婆", "娘", "妻", "太太", "阿姨"
};
const std::array<const char*, 10> kMaleWords = {
    "男", "爸", "父", "哥", "弟", "爷", "叔", "夫", "先生", "伯"
};

template <std::size_t N>
bool contains_any(const std::string& hay, const std::array<const char*, N>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* n) {
        return hay.find(n) != std::string::npos;
    });
}

std::string voice_gender_of(const std::string& name) {
    const std::string low = lower_ascii(name);
    // female 里含 male，先判 female
    if (contains_any(low, kFemaleMarks)) return "female";
    if (contains_any(low, kMaleMarks)) return "male";
    return "";
}

}  // namespace

const char* to_string(StyleLine v) {
    return v == StyleLine::ANIME ? "anime" : "realistic";
}

std::string default_style(StyleLine style_line) {
    return style_line == StyleLine::ANIME ? stages::prompt::style::kDefaultAnime
                                          : stages::prompt::style::kDefaultRealistic;
}

// ── AppearanceBlock ────────────────────────────────────────────────────

std::string AppearanceBlock::render(StyleLine style_line) const {
    const std::array<const std::string*, 5> raw = {
        &identity, &body, &face, &attire, &style
    };
    std::vector<std::string> parts;
    parts.reserve(raw.size());
    for (const std::string* p : raw) {
        std::string cleaned = rstrip_punct(strip_ws(*p));
        if (!cleaned.empty()) parts.push_back(std::move(cleaned));
    }
    return join(parts, sep_for(style_line));
}

// ── Character ──────────────────────────────────────────────────────────

void Character::validate(std::vector<std::string>& errs) const {
    if (!has_prefix_slug(char_id, "c_")) {
        errs.push_back(SAY("角色 id 必须以 c_ 开头且只含小写字母数字下划线，当前是 ") +
                       char_id);
    }
    if (name.empty()) {
        errs.push_back(SAYF("%1：name 不能为空", char_id));
    }
    if (appearance.identity.empty()) {
        errs.push_back(SAYF("%1：appearance.identity 不能为空", char_id));
    }
    if (appearance.face.empty()) {
        errs.push_back(SAYF("%1：appearance.face 不能为空", char_id));
    }
    if (appearance.attire.empty()) {
        errs.push_back(SAYF("%1：appearance.attire 不能为空", char_id));
    }
    if (lora_strength < 0.0 || lora_strength > 2.0) {
        errs.push_back(SAYF("%1：lora_strength 要在 0 到 2 之间", char_id));
    }
}

/// 取某个服装状态的描述，对不上就退回默认那身。
///
/// ⚠️ **今天这条 for 循环永远走不进去。** `wardrobe` 这个数组没有任何一处
/// 会往里写：定妆那一步不产（[bible] 的提示词反过来还叮嘱「剧情中的换装
/// 不在这里写」）、`/api/character` 的白名单里没有 `wardrobe`、界面上一个
/// 字都没有。唯一的办法是手改 assets.json。
///
/// 而分镜那边是**被提示词教着填**的（prompts.toml 的 storyboard_rules 第 4
/// 条：「换装只能通过 wardrobe_state 填一个状态名」），模型于是认认真真写
/// suit_torn、雨中。对不上，这里一声不吭退回 `appearance.attire`——出来的
/// 每一帧穿的都是那身干净的常服，中间没有任何地方会提一句。
///
/// 退回本身是对的（语料 prompt_compose 里那条「没这个状态」钉的就是它），
/// 缺的是上游：**谁来创建 WardrobeVariant**。
std::string Character::wardrobe_desc(const std::string& wardrobe_state) const {
    if (!wardrobe_state.empty() && wardrobe_state != "default") {
        for (const auto& v : wardrobe) {
            if (v.wardrobe_id == wardrobe_state) return v.description;
        }
    }
    return appearance.attire;
}

std::string Character::render_prompt(StyleLine style_line,
                                     const std::string& wardrobe_state) const {
    AppearanceBlock block = appearance;
    if (wardrobe_state != "default") {
        // 换装时只替换 attire 那一段，其余逐字节不变
        block.attire = wardrobe_desc(wardrobe_state);
    }
    std::string rendered = block.render(style_line);
    if (lora_trigger.has_value() && !lora_trigger->empty()) {
        rendered = *lora_trigger + sep_for(style_line) + rendered;
    }
    return rendered;
}

std::optional<std::string> Character::ref_for_pose(
    const std::string& face_pose) const {
    // 对应 Python 的那张字典。**退回不是逐级退到底的**，原来这儿写着
    // 「拿不到就逐级退回，最后退到正面图」，而实际的链子是：
    //
    //   front                     → 正面（没有就是没有）
    //   three_quarter / profile   → 侧面 → 正面
    //   back                      → 背面 → 侧面 ✕ 到此为止
    //   其它（off_screen、认不出来的）→ 正面
    //
    // 差别落在**只画了正面**的角色上（三视图那一格停在 1/3 是常事）：
    // 背面的镜头一张参考图都拿不到，而参考图正是跨镜头认脸的唯一手段。
    // 不报错，表现是那几镜的人长得不太像。
    //
    // 这一条**是照 Python 那张字典抄的**（golden/character_render.json 的
    // ref_for_pose 钉着它），所以要改成"背面也退到正面"是一个产品决定
    // ——拿正面图去锚一个背身镜头是好是坏，得先有人拍板——不是顺手清理。
    if (face_pose == "front") return ref_front;
    if (face_pose == "three_quarter" || face_pose == "profile") {
        return ref_three_quarter.has_value() ? ref_three_quarter : ref_front;
    }
    if (face_pose == "back") {
        return ref_back.has_value() ? ref_back : ref_three_quarter;
    }
    // off_screen 和任何认不出来的取值都走 .get(face_pose, self.ref_front)
    return ref_front;
}

// ── Location ───────────────────────────────────────────────────────────

void Location::validate(std::vector<std::string>& errs) const {
    if (!has_prefix_slug(location_id, "loc_")) {
        errs.push_back("场景 id 必须以 loc_ 开头且只含小写字母数字下划线，当前是 " +
                       location_id);
    }
    if (name.empty()) errs.push_back(location_id + "：name 不能为空");
    if (space.empty()) errs.push_back(location_id + "：space 不能为空");
    if (lighting.empty()) errs.push_back(location_id + "：lighting 不能为空");
}

std::string Location::render_prompt(StyleLine style_line, bool with_lighting) const {
    // 注意：这里 Python 用的是 p.strip() 而不是 rstrip 标点，
    // 和 AppearanceBlock.render 不一样。不要"顺手统一"，
    // 输出必须逐字节对齐（with_lighting = true 那条路）。
    std::vector<std::string> parts;
    for (const std::string* p : {&space, &lighting, &palette}) {
        if (p == &lighting && !with_lighting) continue;
        std::string t = strip_ws(*p);
        if (!t.empty()) parts.push_back(std::move(t));
    }
    return join(parts, sep_for(style_line));
}

// ── AssetLibrary ───────────────────────────────────────────────────────

namespace {

/// 两个 JSON 类型共用的实现。
template <typename J>
void asset_library_to_json(J& j, const AssetLibrary& t) {
    j = J::object();
    j["characters"] = t.characters;
    j["locations"] = t.locations;
    j["style"] = J(nlohmann::json(t.style));
}

template <typename J>
void asset_library_from_json(const J& j, AssetLibrary& t) {
    const AssetLibrary def{};
    // 用 contains 而不是 at：缺字段要退回默认值，对应宏的 WITH_DEFAULT 语义。
    t.characters = j.contains("characters")
                       ? j.at("characters").template get<OrderedMap<Character>>()
                       : def.characters;
    t.locations = j.contains("locations")
                      ? j.at("locations").template get<OrderedMap<Location>>()
                      : def.locations;
    t.style = j.contains("style")
                  ? nlohmann::json(j.at("style")).template get<StyleProfile>()
                  : def.style;
}

}  // namespace

void to_json(nlohmann::json& j, const AssetLibrary& t) { asset_library_to_json(j, t); }
void from_json(const nlohmann::json& j, AssetLibrary& t) { asset_library_from_json(j, t); }
void to_json(nlohmann::ordered_json& j, const AssetLibrary& t) { asset_library_to_json(j, t); }
void from_json(const nlohmann::ordered_json& j, AssetLibrary& t) { asset_library_from_json(j, t); }


std::vector<std::string> AssetLibrary::character_ids() const {
    // 排序，对应 Python 的 sorted(self.characters)。
    // 注意这跟遍历顺序是两回事：遍历要插入顺序（响应里的数组），
    // 这里要字典序（给大模型做约束解码的枚举）。
    return characters.sorted_keys();
}

std::vector<std::string> AssetLibrary::location_ids() const {
    return locations.sorted_keys();
}

std::vector<std::string> AssetLibrary::validate_references(
    const std::vector<std::string>& char_ids,
    const std::vector<std::string>& loc_ids) const {
    std::vector<std::string> problems;
    // 去重加排序，对应 Python 的 sorted(char_ids - set(...))
    for (const auto& cid : std::set<std::string>(char_ids.begin(), char_ids.end())) {
        if (characters.find(cid) == characters.end()) {
            problems.push_back("角色 " + cid + " 未在资产库注册");
        }
    }
    for (const auto& lid : std::set<std::string>(loc_ids.begin(), loc_ids.end())) {
        if (locations.find(lid) == locations.end()) {
            problems.push_back("场景 " + lid + " 未在资产库注册");
        }
    }
    return problems;
}

std::vector<std::string> AssetLibrary::validate() const {
    std::vector<std::string> errs;
    for (const auto& kv : characters) {
        if (kv.first != kv.second.char_id) {
            errs.push_back("资产库的键 " + kv.first + " 与角色自己的 id " +
                           kv.second.char_id + " 不一致");
        }
        kv.second.validate(errs);
    }
    for (const auto& kv : locations) {
        if (kv.first != kv.second.location_id) {
            errs.push_back("资产库的键 " + kv.first + " 与场景自己的 id " +
                           kv.second.location_id + " 不一致");
        }
        kv.second.validate(errs);
    }
    return errs;
}

// ── 配音辅助 ───────────────────────────────────────────────────────────

std::string guess_gender(const std::string& identity) {
    const bool female = contains_any(identity, kFemaleWords);
    const bool male = contains_any(identity, kMaleWords);
    if (female && !male) return "female";
    if (male && !female) return "male";
    return "";
}

std::optional<std::string> pick_voice(const std::vector<std::string>& available,
                                      const std::string& gender, int index) {
    std::vector<std::string> pool;
    for (const auto& v : available) {
        if (!v.empty() && v != "none") pool.push_back(v);
    }
    if (pool.empty()) return std::nullopt;

    // 对应 Python 的 rank()：(性别档, 中文档, 名字本身)
    const auto rank = [&](const std::string& v) {
        const std::string vg = voice_gender_of(v);
        int gender_rank;
        if (gender.empty() || vg.empty()) {
            gender_rank = 1;              // 分不出来的排中间
        } else if (vg == gender) {
            gender_rank = 0;
        } else {
            gender_rank = 2;              // 性别相反的排最后
        }
        const std::string low = lower_ascii(v);
        const int zh_rank =
            (low.find("zh_") != std::string::npos ||
             low.find("zh-") != std::string::npos) ? 0 : 1;
        return std::make_tuple(gender_rank, zh_rank, v);
    };

    std::sort(pool.begin(), pool.end(), [&](const std::string& a, const std::string& b) {
        return rank(a) < rank(b);
    });

    const auto best = rank(pool.front());
    const int best_gender = std::get<0>(best);
    const int best_zh = std::get<1>(best);

    std::vector<std::string> tier;
    for (const auto& v : pool) {
        const auto r = rank(v);
        if (std::get<0>(r) == best_gender && std::get<1>(r) == best_zh) {
            tier.push_back(v);
        }
    }
    // 同一档里按角色序号轮着分，不同角色至少声音不同。
    // Python 的 % 对负数返回非负，C++ 不是，所以先归一化。
    const int n = static_cast<int>(tier.size());
    int i = index % n;
    if (i < 0) i += n;
    return tier[static_cast<std::size_t>(i)];
}

namespace {

/// 判重用的名字：去掉前后空白。**只做这一步**——见 dedupe_locations 头上
/// 那段，模糊匹配会把两个真不一样的地方粘死。
std::string dedupe_key(const std::string& name) {
    return std::string(text::strip_ws(name));
}

/// 一条场景"值不值得留"。数越大越该留。
/// 顺序：分镜引用着 > 有空景图 > 描述写得全。
std::tuple<int, int, int> weight_of(const Location& l, bool in_use) {
    const int has_ref = (l.ref_empty.has_value() && !l.ref_empty->empty()) ? 1 : 0;
    const int filled = static_cast<int>(!l.space.empty()) +
                       static_cast<int>(!l.lighting.empty()) +
                       static_cast<int>(!l.palette.empty());
    return {in_use ? 1 : 0, has_ref, filled};
}

std::tuple<int, int, int> weight_of(const Character& c, bool in_use) {
    const auto& a = c.appearance;
    const int refs = static_cast<int>(c.ref_front.has_value()) +
                     static_cast<int>(c.ref_three_quarter.has_value()) +
                     static_cast<int>(c.ref_back.has_value());
    const int filled = static_cast<int>(!a.identity.empty()) +
                       static_cast<int>(!a.body.empty()) +
                       static_cast<int>(!a.face.empty()) +
                       static_cast<int>(!a.attire.empty());
    return {in_use ? 1 : 0, refs, filled};
}

/// 空着的字段从被丢掉的那条补上。**只补空的**：留下来的那条是挑出来的，
/// 它写了的东西不该被淘汰掉的那条盖掉。
void fill_gaps(Location& keep, const Location& drop) {
    if (keep.space.empty())    keep.space = drop.space;
    if (keep.lighting.empty()) keep.lighting = drop.lighting;
    if (keep.palette.empty())  keep.palette = drop.palette;
    if ((!keep.ref_empty.has_value() || keep.ref_empty->empty()) &&
        drop.ref_empty.has_value() && !drop.ref_empty->empty()) {
        keep.ref_empty = drop.ref_empty;
    }
}

void fill_gaps(Character& keep, const Character& drop) {
    if (keep.appearance.identity.empty()) keep.appearance.identity = drop.appearance.identity;
    if (keep.appearance.body.empty())     keep.appearance.body = drop.appearance.body;
    if (keep.appearance.face.empty())     keep.appearance.face = drop.appearance.face;
    if (keep.appearance.attire.empty())   keep.appearance.attire = drop.appearance.attire;
    if (keep.appearance.style.empty())    keep.appearance.style = drop.appearance.style;
    if (!keep.ref_front.has_value())          keep.ref_front = drop.ref_front;
    if (!keep.ref_three_quarter.has_value())  keep.ref_three_quarter = drop.ref_three_quarter;
    if (!keep.ref_back.has_value())           keep.ref_back = drop.ref_back;
    if (!keep.voice_id.has_value())           keep.voice_id = drop.voice_id;
    if (!keep.voice_ref_audio.has_value())    keep.voice_ref_audio = drop.voice_ref_audio;
}

/// 两种资产一套逻辑，只有"怎么称重"和"补哪些字段"不一样。
template <typename T>
IdRemap dedupe_by_name(OrderedMap<T>& items, const std::set<std::string>& in_use) {
    // 名字 → 这个名字下所有的 id，按原顺序。
    std::map<std::string, std::vector<std::string>> by_name;
    std::vector<std::string> order;
    for (const auto& kv : items) {
        const std::string k = dedupe_key(kv.second.name);
        // 名字空着的一律不碰：它们互相之间什么都证明不了。
        if (k.empty()) continue;
        if (by_name.find(k) == by_name.end()) order.push_back(k);
        by_name[k].push_back(kv.first);
    }

    IdRemap remap;
    for (const std::string& name : order) {
        const std::vector<std::string>& ids = by_name[name];
        if (ids.size() < 2) continue;

        // 挑一条留下。并列时取 id 字典序最小的——**结果要可复现**，
        // 不然同一份库跑两遍能收出两个不同的 id 来。
        std::string keep = ids.front();
        auto best = weight_of(items.at(keep), in_use.count(keep) != 0);
        for (const std::string& id : ids) {
            const auto w = weight_of(items.at(id), in_use.count(id) != 0);
            if (w > best || (w == best && id < keep)) {
                best = w;
                keep = id;
            }
        }

        for (const std::string& id : ids) {
            if (id == keep) continue;
            fill_gaps(items.at(keep), items.at(id));
            remap[id] = keep;
        }
    }

    for (const auto& kv : remap) items.erase(kv.first);
    return remap;
}

}  // namespace

IdRemap dedupe_locations(AssetLibrary& lib, const std::set<std::string>& in_use) {
    return dedupe_by_name(lib.locations, in_use);
}

IdRemap dedupe_characters(AssetLibrary& lib, const std::set<std::string>& in_use) {
    return dedupe_by_name(lib.characters, in_use);
}

}  // namespace changji::models
