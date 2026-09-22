#include "stages/ref_images.hpp"

#include "stages/bible.hpp"
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

const std::vector<RefPose>& ref_poses() {
    static const std::vector<RefPose> poses = {
        {"front", "正面", prompt::ref_images::kPoseFront},
        {"three_quarter", "四分之三侧", prompt::ref_images::kPoseThreeQuarter},
        {"back", "背面", prompt::ref_images::kPoseBack},
    };
    return poses;
}

namespace {

const RefPose& pose_or_front(const std::string& key) {
    for (const auto& p : ref_poses()) {
        if (p.key == key) return p;
    }
    return ref_poses().front();
}

/// 把几段拼成一句，空的那几段跳过。
///
/// 不跳的话会拼出「，，」这种串，而这个串会原样进出图提示词——
/// 分词器把它当内容，画面上会多出说不清的东西。
std::string join_parts(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        const std::string s = text::rstrip_punct(text::strip_ws(p));
        if (s.empty()) continue;
        if (!out.empty()) out += "，";
        out += s;
    }
    return out;
}

}  // namespace

std::string build_character_ref_prompt(const Character& c,
                                       const StyleProfile& style,
                                       const std::string& pose_key) {
    const RefPose& pose = pose_or_front(pose_key);
    // **外观块逐字用 render_prompt 那一份。** 参考图和后面每一镜必须是
    // 同一个人，而那靠的就是这段字逐字节相同——这里自己再写一套形容词，
    // 参考图上那个人和成片里那个人就是两个人。
    return join_parts({
        prompt::ref_images::kCharacterHead,
        pose.phrase,
        c.render_prompt(style.style_line),
        prompt::ref_images::kCharacterTail,
        style.global_style,
    });
}

std::string build_location_ref_prompt(const Location& l,
                                      const StyleProfile& style) {
    return join_parts({
        prompt::ref_images::kLocationHead,
        l.render_prompt(style.style_line),
        // 「里面不能有人」写在外观块后面：写前面的话会被后面那串具体的
        // 空间描述冲淡——出图提示词里靠后的词权重低，但这一条是硬要求，
        // 所以单独成段而不是塞在开头一长串里。
        prompt::ref_images::kLocationNoPeople,
        style.global_style,
    });
}

std::string ref_negative(const StyleProfile& style) {
    std::string out = style.negative_prompt.empty()
                          ? default_negative(style.style_line)
                          : style.negative_prompt;
    out += prompt::ref_images::kNegativeExtra;
    return out;
}

}  // namespace changji::stages
