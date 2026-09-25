// 资产类的编辑接口：/api/character /api/location /api/style。
//
// 和 editing.cpp（改镜头）分开放，因为这三个共享一条重要语义：
// 它们改的是**全片共用**的东西，所以要连带把未锁定的镜头退回未开工。
// 改镜头只影响那一个镜头，不需要。

#include <set>
#include <string>
#include <vector>

#include "http/editing.hpp"
#include "http/reset.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, SAYF("请求里缺少字符串字段 %1", key));
    }
    return body.at(key).get<std::string>();
}

std::string need_string(const json& v, const char* field) {
    if (!v.is_string()) {
        throw ApiError(400, SAYF("改动不合法：%1 要是字符串", field));
    }
    return v.get<std::string>();
}

double need_num(const json& v, const char* field) {
    if (!v.is_number()) {
        throw ApiError(400, SAYF("改动不合法：%1 要是数字", field));
    }
    return v.get<double>();
}

/// 把所有未锁定的镜头退回未开工，返回改了几个。
///
/// 角色外观和场景是所有镜头共用的，改了它们等于全片的提示词都变了。
/// 不重置的话已完成的镜头会继续用旧设定，同一个角色前后长得不一样。
///
/// 跳过 PLANNED（本来就没开工）和 LOCKED（人工确认过，不再重跑）。
/// 这批字段里有没有**真的**改动。
///
/// 界面一次提交整张表单，所以"字段出现在请求里"不代表用户改了它。
/// 按值比，值没变就不算改，也就不该触发重跑——光看字段在不在的话，
/// 改个音色也会把全片镜头退回重跑，已经渲染好的成片档白白重来一遍。
bool changed(const json& current, const json& patch,
             const std::set<std::string>& keys) {
    for (const auto& k : keys) {
        if (!patch.contains(k) || patch.at(k).is_null()) continue;
        if (!current.contains(k) || current.at(k) != patch.at(k)) return true;
    }
    return false;
}

/// 校验 patch 里没有多余字段，对应 pydantic 的 extra="forbid"。
/// 违规是 422 不是 400，见 readonly.hpp 里 unprocessable 的注释。
void reject_extra(const json& patch, const std::set<std::string>& allowed) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (allowed.count(it.key()) == 0) {
            throw unprocessable("patch", it.key(),
                                "Extra inputs are not permitted",
                                it.value(), "extra_forbidden");
        }
    }
}

/// 取 reset_shots。**默认值三个接口不一样**，不能统一。
///
/// CharacterUpdateRequest 和 LocationUpdateRequest 默认 true，
/// StyleUpdateRequest 默认 **false**。这是 Python 里一处刻意的不对称：
/// 改全片风格是常做的事，默认把整部电影推翻重跑代价太大；
/// 而改角色外观或场景是"这个东西定稿了"的动作，频率低得多。
///
/// 我第一版三个都用了 true，对拍立刻抓到——两条 style 用例都是
/// Python 返回 reset_shots:0 而我返回 1。
bool want_reset(const json& body, bool default_value) {
    if (!body.contains("reset_shots") || body.at("reset_shots").is_null()) {
        return default_value;
    }
    if (!body.at("reset_shots").is_boolean()) {
        throw ApiError(400, SAY("reset_shots 要是真假值"));
    }
    return body.at("reset_shots").get<bool>();
}

/// 三个接口共用的开头：取项目、取 patch、查多余字段。
ProjectStore open_for_patch(const json& body, const json& patch,
                            const std::set<std::string>& allowed) {
    const std::string project_path = need_str(body, "project");
    if (project_path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    if (!patch.is_object()) throw ApiError(400, SAY("patch 要是一个对象"));
    reject_extra(patch, allowed);
    return ProjectStore(paths::from_utf8(project_path));
}

json patch_of(const json& body) {
    return body.contains("patch") ? body.at("patch") : json::object();
}

}  // namespace

ApiResult post_character(const json& body) {
    static const std::set<std::string> kAllowed = {
        "name", "identity", "body", "face", "attire", "style",
        "voice_id", "lora_trigger", "lora_strength",
    };
    const json patch = patch_of(body);
    ProjectStore store = open_for_patch(body, patch, kAllowed);
    const std::string char_id = need_str(body, "char_id");
    // 读→改→存一把锁（CLAUDE.md 第十四条）。
    const auto store_guard = store.lock();

    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, SAYF("没有角色 %1", char_id));
    Character& c = it->second;

    // 外观五段是一致性的锚点，改了会影响所有引用它的镜头。
    // 改名字、音色、LoRA 这些不影响画面，不该触发重跑。
    static const std::set<std::string> kAppearance = {
        "identity", "body", "face", "attire", "style",
    };
    const json before_appearance = c.appearance;
    const bool touched = changed(before_appearance, patch, kAppearance);

    Character draft = c;
    for (auto pit = patch.begin(); pit != patch.end(); ++pit) {
        const std::string& k = pit.key();
        const json& v = pit.value();
        if (v.is_null()) continue;

        if (k == "identity")           draft.appearance.identity = need_string(v, "identity");
        else if (k == "body")          draft.appearance.body = need_string(v, "body");
        else if (k == "face")          draft.appearance.face = need_string(v, "face");
        else if (k == "attire")        draft.appearance.attire = need_string(v, "attire");
        else if (k == "style")         draft.appearance.style = need_string(v, "style");
        else if (k == "name")          draft.name = need_string(v, "name");
        else if (k == "voice_id")      draft.voice_id = need_string(v, "voice_id");
        else if (k == "lora_trigger")  draft.lora_trigger = need_string(v, "lora_trigger");
        else if (k == "lora_strength") draft.lora_strength = need_num(v, "lora_strength");
    }

    std::vector<std::string> errs;
    draft.validate(errs);
    if (!errs.empty()) throw ApiError(400, SAYF("改动不合法：%1", errs.front()));

    c = std::move(draft);
    const std::string rendered = c.render_prompt(assets.style.style_line);
    store.save_assets(assets);

    // 只退画面里有他的那几镜（reset.hpp 上那段）。
    const int reset =
        (touched && want_reset(body, true)) ? reset_shots_with_character(store, char_id) : 0;
    return {200, {{"saved", true}, {"rendered", rendered}, {"reset_shots", reset}}};
}

ApiResult post_location(const json& body) {
    static const std::set<std::string> kAllowed = {
        "name", "space", "lighting", "palette",
    };
    const json patch = patch_of(body);
    ProjectStore store = open_for_patch(body, patch, kAllowed);
    const std::string location_id = need_str(body, "location_id");
    const auto store_guard = store.lock();   // 读→改→存一把锁

    AssetLibrary assets = store.load_assets();
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) throw ApiError(404, SAYF("没有场景 %1", location_id));
    Location& l = it->second;

    // 只有这三个影响画面。改名字不触发重跑。
    static const std::set<std::string> kVisual = {"space", "lighting", "palette"};
    const json before = l;
    const bool touched = changed(before, patch, kVisual);

    Location draft = l;
    for (auto pit = patch.begin(); pit != patch.end(); ++pit) {
        const std::string& k = pit.key();
        const json& v = pit.value();
        if (v.is_null()) continue;
        if (k == "name")          draft.name = need_string(v, "name");
        else if (k == "space")    draft.space = need_string(v, "space");
        else if (k == "lighting") draft.lighting = need_string(v, "lighting");
        else if (k == "palette")  draft.palette = need_string(v, "palette");
    }

    std::vector<std::string> errs;
    draft.validate(errs);
    if (!errs.empty()) throw ApiError(400, SAYF("改动不合法：%1", errs.front()));

    l = std::move(draft);
    const std::string rendered = l.render_prompt(assets.style.style_line);
    store.save_assets(assets);

    // 只退在这个场景里的那几镜。
    const int reset =
        (touched && want_reset(body, true)) ? reset_shots_at_location(store, location_id) : 0;
    return {200, {{"saved", true}, {"rendered", rendered}, {"reset_shots", reset}}};
}

ApiResult post_style(const json& body) {
    // ⚠️ **aspect_ratio 不在这儿收。** 它由画幅派生（见
    // config::VideoConfig::aspect_ratio），改画幅走 /bff/project/video。
    // 2026-09-14 之前它在这个白名单里，于是「横屏 + 9:16」配得出来
    // 也不报错，出来是参考图竖的、成片横的。老前端要是还在发它，
    // 422 说得清「这儿改不了」，比默默写进去强。
    static const std::set<std::string> kAllowed = {
        "global_style", "negative_prompt",
    };
    const json patch = patch_of(body);
    ProjectStore store = open_for_patch(body, patch, kAllowed);
    const auto store_guard = store.lock();   // 读→改→存一把锁

    AssetLibrary assets = store.load_assets();
    const json before = assets.style;

    StyleProfile draft = assets.style;
    for (auto pit = patch.begin(); pit != patch.end(); ++pit) {
        const std::string& k = pit.key();
        const json& v = pit.value();
        if (v.is_null()) continue;
        if (k == "global_style")         draft.global_style = need_string(v, "global_style");
        else if (k == "negative_prompt") draft.negative_prompt = need_string(v, "negative_prompt");
    }
    assets.style = std::move(draft);
    store.save_assets(assets);

    // 全片风格层所有镜头都会带上，这三个字段任一变了就得全部重跑。
    const bool touched = changed(before, patch,
                                 {"global_style", "negative_prompt"});
    // 注意默认 false，和另外两个接口不一样
    const int reset = (touched && want_reset(body, false)) ? reset_all_shots(store) : 0;
    return {200, {{"saved", true}, {"reset_shots", reset}}};
}

}  // namespace changji::http
