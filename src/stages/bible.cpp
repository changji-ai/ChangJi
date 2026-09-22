#include "stages/bible.hpp"

#include <set>
#include <string>
#include <vector>

#include "stages/prompts.inc.hpp"
#include "stages/json_extract.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 从 JSON 对象里取字符串，缺了或不是字符串就返回空串。
///
/// 对应 Python 的 item.get("x", "")。不抛异常是刻意的：
/// 模型漏字段是常态，缺一个 body 不该让整份圣经作废。
std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

const ordered& bible_schema() {
    // 手写而不是从 Python 导出，因为它要和 Python 的字典**结构**一致，
    // 不要求序列化字节一致——远端服务收到的是解析后的对象。
    // 用 ordered_json 是为了字段顺序稳定，方便出问题时和 Python 的
    // 请求体逐行 diff。
    static const ordered schema = [] {
        ordered character_props = ordered::object();
        // ⚠️ **这儿不要举人名当例子。** 原来写的是"如 lin_wan"，而 lin_wan
        // 就是林晚——模型会连着把这个名字用到 name 上去，于是十个项目里
        // 有八个女主角叫林晚。同一个形状在 prompts.toml 的 [bible]
        // [bible_story] 里也各有一份，一起拆掉了。
        character_props["key"] = {
            {"type", "string"},
            {"description", "英文小写下划线短标识，按这个角色的中文名音译"}};
        character_props["name"] = {{"type", "string"},
                                   {"description", "剧本里的中文称呼"}};
        character_props["identity"] = {{"type", "string"},
                                       {"description", "身份：性别、年龄段、气质。一句话"}};
        character_props["body"] = {{"type", "string"}, {"description", "体型和身高感"}};
        character_props["face"] = {
            {"type", "string"},
            {"description", "五官、发型、发色、瞳色。这段会在几十个镜头里逐字复用，写得具体且不要含糊"}};
        character_props["attire"] = {{"type", "string"}, {"description", "默认服装"}};

        ordered location_props = ordered::object();
        location_props["key"] = {{"type", "string"}, {"description", "英文小写下划线短标识"}};
        location_props["name"] = {{"type", "string"}, {"description", "中文场景名"}};
        location_props["space"] = {{"type", "string"}, {"description", "空间结构和布景"}};
        location_props["lighting"] = {{"type", "string"}, {"description", "光线基调"}};
        location_props["palette"] = {{"type", "string"}, {"description", "色彩方案"}};

        ordered props = ordered::object();
        props["characters"] = {
            {"type", "array"},
            {"description", "剧本里所有有名有姓或有台词的角色"},
            {"items", {{"type", "object"},
                       {"properties", character_props},
                       {"required", {"key", "name", "identity", "face", "attire"}},
                       {"additionalProperties", false}}}};
        props["locations"] = {
            {"type", "array"},
            {"description", "剧本里出现的场景"},
            {"items", {{"type", "object"},
                       {"properties", location_props},
                       {"required", {"key", "name", "space", "lighting"}},
                       {"additionalProperties", false}}}};
        props["global_style"] = {{"type", "string"},
                                 {"description", "全片统一的画风、色温、质感。一句话"}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"characters", "locations", "global_style"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

ordered bible_schema_for_story(const Story& story) {
    ordered schema = bible_schema();

    ordered character_names = ordered::array();
    for (const auto& character : story.characters) {
        if (!text::strip_ws(character.name).empty()) {
            character_names.push_back(character.name);
        }
    }
    ordered& characters = schema["properties"]["characters"];
    characters["minItems"] = character_names.size();
    characters["maxItems"] = character_names.size();
    characters["items"]["properties"]["name"]["enum"] = character_names;

    ordered location_names = ordered::array();
    for (const auto& location : story.locations) {
        if (!text::strip_ws(location.name).empty()) {
            location_names.push_back(location.name);
        }
    }
    ordered& locations = schema["properties"]["locations"];
    locations["minItems"] = location_names.size();
    locations["maxItems"] = location_names.size();
    locations["items"]["properties"]["name"]["enum"] = location_names;
    return schema;
}

std::string build_bible_prompt(const std::string& script, StyleLine style_line) {
    // 各段来自 prompts.toml 的 [bible]，和 Python 逐字节一致。
    const char* hint = style_line == StyleLine::ANIME ? prompt::bible::kHintAnime
                                                      : prompt::bible::kHintRealistic;
    std::string out;
    out += prompt::bible::kPrefix;
    out += hint;
    out += prompt::bible::kMiddle;
    out += script;
    out += prompt::bible::kTail;
    return out;
}

std::string render_story_for_bible(const Story& story) {
    std::string out;

    // 调子先写。美术看这几行定色温和质感，后面每个人每个地方都在这个
    // 调子下面。放在名单后面的话，模型已经把人写完了才读到"荒诞"。
    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.genre.empty()) out += "【题材】" + story.genre + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    if (!out.empty()) out += "\n";

    out += "【人物】\n";
    for (const auto& c : story.characters) {
        out += c.name;
        if (!c.identity.empty()) out += "：" + c.identity;
        out += "。";
        // 欲望和弧线给进去是为了让美术判断气质（想复仇的人和想赎罪的人
        // 眼神不一样）。提示词里另外写死了"不要把它们写进外观"。
        if (!c.want.empty()) out += "他要的是：" + c.want + "。";
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

    out += "\n【地点】\n";
    for (const auto& l : story.locations) {
        out += l.name;
        if (!l.what.empty()) out += "：" + l.what;
        out += "。";
        if (!l.when.empty()) out += l.when + "。";
        out += "\n";
    }

    // 分章只给调子用，所以可以截。人物表和地点表不截——它们是名单，
    // 截掉一半等于漏掉几个人，而漏掉的那个后面分镜里指不到。
    std::string chapters;
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        const auto& c = story.chapters[i];
        chapters += std::to_string(i + 1) + " " + c.title;
        if (!c.summary.empty()) {
            chapters += "：" + text::collapse_ws(c.summary);
        }
        chapters += "\n";
    }
    if (!chapters.empty()) {
        out += "\n【分章】\n";
        out += text::truncate_utf8(chapters, prompt::bible_story::kChaptersMaxChars);
    }

    return out;
}

std::string build_bible_prompt_from_story(const Story& story,
                                          StyleLine style_line) {
    const char* hint = style_line == StyleLine::ANIME ? prompt::bible::kHintAnime
                                                      : prompt::bible::kHintRealistic;
    std::string out;
    out += prompt::bible_story::kPrefix;
    out += hint;
    out += prompt::bible_story::kMiddle;
    out += render_story_for_bible(story);
    out += prompt::bible_story::kTail;
    return out;
}

std::string default_negative(StyleLine style_line) {
    // 词在 prompts.toml 的 [style] 里，为什么写实线不加"压卡通"也写在那儿。
    std::string out = prompt::style::kNegativeBase;
    if (style_line == StyleLine::ANIME) out += prompt::style::kNegativeAnimeExtra;
    return out;
}

AssetLibrary parse_bible(const std::string& raw, StyleLine style_line,
                         const std::string& aspect_ratio) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw BibleError(e.what());
    }
    if (!data.is_object()) throw BibleError("大模型没有返回对象");

    AssetLibrary lib;

    // 角色。voice_order 用的是**遍历下标**而不是插入序号——
    // key 为空的会被 continue 跳过，但下标照样加。这是 Python 的行为
    // （enumerate 在 continue 之前就取好了 index），照抄。
    int index = 0;
    if (data.contains("characters") && data["characters"].is_array()) {
        for (const auto& item : data["characters"]) {
            const int this_index = index++;
            const std::string raw_key = get_str(item, "key").empty()
                                            ? get_str(item, "name")
                                            : get_str(item, "key");
            const std::string key = text::slug(raw_key);
            if (key.empty()) continue;

            const std::string char_id = "c_" + key;
            Character c;
            c.char_id = char_id;
            c.name = get_str(item, "name").empty() ? key : get_str(item, "name");
            // 音色留空，配音时按服务端实际有哪些参考音频再定。
            // 这里写死路径的话，换一台推理服务就可能对不上，
            // 节点校验不过整条流水线直接断在配音这一步。
            c.voice_id = std::nullopt;
            c.voice_gender = guess_gender(get_str(item, "identity"));
            c.voice_order = this_index;
            c.appearance.identity = text::clean_field(get_str(item, "identity"));
            c.appearance.body = text::clean_field(get_str(item, "body"));
            c.appearance.face = text::clean_field(get_str(item, "face"));
            c.appearance.attire = text::clean_field(get_str(item, "attire"));

            std::vector<std::string> errs;
            c.validate(errs);
            if (!errs.empty()) {
                throw BibleError("角色 " + key + " 的设定不合法：" + errs.front());
            }
            lib.characters[char_id] = std::move(c);
        }
    }

    if (data.contains("locations") && data["locations"].is_array()) {
        for (const auto& item : data["locations"]) {
            const std::string raw_key = get_str(item, "key").empty()
                                            ? get_str(item, "name")
                                            : get_str(item, "key");
            const std::string key = text::slug(raw_key);
            if (key.empty()) continue;

            const std::string loc_id = "loc_" + key;
            Location l;
            l.location_id = loc_id;
            l.name = get_str(item, "name").empty() ? key : get_str(item, "name");
            l.space = text::clean_field(get_str(item, "space"));
            l.lighting = text::clean_field(get_str(item, "lighting"));
            l.palette = text::clean_field(get_str(item, "palette"));

            std::vector<std::string> errs;
            l.validate(errs);
            if (!errs.empty()) {
                throw BibleError("场景 " + key + " 的设定不合法：" + errs.front());
            }
            lib.locations[loc_id] = std::move(l);
        }
    }

    if (lib.characters.empty()) {
        throw BibleError(
            "大模型没有产出任何角色。检查剧本里是否真的有人物，"
            "或者换一个更强的模型");
    }

    lib.style.style_line = style_line;
    lib.style.global_style = text::clean_field(get_str(data, "global_style"));
    lib.style.negative_prompt = default_negative(style_line);
    lib.style.aspect_ratio = aspect_ratio;
    return lib;
}

AssetLibrary parse_bible_for_story(const std::string& raw, StyleLine style_line,
                                   const std::string& aspect_ratio,
                                   const Story& story) {
    AssetLibrary lib = parse_bible(raw, style_line, aspect_ratio);

    std::set<std::string> expected_characters;
    for (const auto& character : story.characters) {
        const std::string name = text::strip_ws(character.name);
        if (!name.empty()) expected_characters.insert(name);
    }
    std::set<std::string> actual_characters;
    for (const auto& item : lib.characters) {
        actual_characters.insert(text::strip_ws(item.second.name));
    }
    if (actual_characters != expected_characters) {
        throw BibleError("定妆返回的角色名单与故事不一致：必须一个不多、一个不少，名字逐字相同");
    }

    std::set<std::string> expected_locations;
    for (const auto& location : story.locations) {
        const std::string name = text::strip_ws(location.name);
        if (!name.empty()) expected_locations.insert(name);
    }
    std::set<std::string> actual_locations;
    for (const auto& item : lib.locations) {
        actual_locations.insert(text::strip_ws(item.second.name));
    }
    if (actual_locations != expected_locations) {
        throw BibleError("定妆返回的地点名单与故事不一致：必须一个不多、一个不少，名字逐字相同");
    }
    return lib;
}

}  // namespace changji::stages
