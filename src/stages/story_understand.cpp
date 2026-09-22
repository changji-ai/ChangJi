#include "stages/story_understand.hpp"

#include "stages/prompts.inc.hpp"
#include "stages/bible.hpp"
#include "stages/story_analyze.hpp"

namespace changji::stages {

using ordered = nlohmann::ordered_json;
using models::Story;
using models::StyleLine;

namespace {

/// 把 minItems / maxItems 整棵树摘掉。故事里有几个人就是几个人，两个也行。
void drop_counts(ordered& node) {
    if (node.is_object()) {
        node.erase("minItems");
        node.erase("maxItems");
        for (auto& kv : node.items()) drop_counts(kv.value());
    } else if (node.is_array()) {
        for (auto& v : node) drop_counts(v);
    }
}

/// 把 `from` 里的字段搬进 `into`，已有的不动；`required` 合并。
void graft(ordered& into, const ordered& from) {
    const ordered& fp = from.at("properties");
    ordered& ip = into["properties"];
    for (const auto& kv : fp.items()) {
        if (!ip.contains(kv.key())) ip[kv.key()] = kv.value();
    }
    std::vector<std::string> req = into.value("required", std::vector<std::string>{});
    for (const auto& r : from.value("required", std::vector<std::string>{})) {
        bool have = false;
        for (const auto& x : req) have = have || x == r;
        if (!have) req.push_back(r);
    }
    into["required"] = req;
}

}  // namespace

const ordered& understand_schema() {
    static const ordered schema = [] {
        ordered s = analyze_schema();
        const ordered& b = bible_schema();
        graft(s["properties"]["characters"]["items"],
              b.at("properties").at("characters").at("items"));
        graft(s["properties"]["locations"]["items"],
              b.at("properties").at("locations").at("items"));
        s["properties"]["global_style"] = b.at("properties").at("global_style");
        std::vector<std::string> req = s.value("required", std::vector<std::string>{});
        req.push_back("global_style");
        s["required"] = req;
        drop_counts(s);
        return s;
    }();
    return schema;
}

std::string build_understand_prompt(const Story& story, StyleLine style_line) {
    std::string out;
    out += prompt::story_understand::kHead;
    out += style_line == StyleLine::ANIME ? prompt::story_understand::kHintAnime
                                          : prompt::story_understand::kHintRealistic;
    out += prompt::story_understand::kSeg1;
    out += render_chapters_for_analysis(story);
    out += prompt::story_understand::kTail;
    return out;
}

}  // namespace changji::stages
