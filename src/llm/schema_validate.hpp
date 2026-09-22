#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "stages/json_extract.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::llm {
namespace detail {

using Json = nlohmann::json;
using Schema = nlohmann::ordered_json;

inline const Schema* resolve_ref(const Schema& root, const std::string& ref) {
    constexpr const char* prefix = "#/$defs/";
    if (ref.rfind(prefix, 0) != 0) return nullptr;
    const std::string key = ref.substr(std::char_traits<char>::length(prefix));
    const auto defs = root.find("$defs");
    if (defs == root.end() || !defs->is_object()) return nullptr;
    const auto it = defs->find(key);
    return it == defs->end() ? nullptr : &*it;
}

inline bool type_matches(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return true;
}

/// `in_item` = 现在查的是某个数组里的一项。
///
/// **一项缺字段，不该把另外二十几项一起废掉。** 这个项目早有这条规矩：
/// storyboard.cpp 的 drop_unknown_enums 写着「是抹掉不是报错：这一栏填错
/// 不值得把另外十几个好镜头一起作废」。2026-09-17 实撞到同一件事的另一面：
///     大模型输出不符合 script Schema：$.scenes.s1.beats[12] 缺少必填字段 characters
/// 二十六拍里有一拍没填「画面里有谁」，一次五分钟的改编整份作废——**而下游
/// 本来就受得住**（script.cpp:1207 那句 `it != item.end() && it->is_array()`，
/// 缺了就是空数组，正好是 schema 自己写的「环境拍、空镜填空数组」）。
///
/// 顶层的必填还是拦：那一层缺了就是整份输出没有内容，不是"其中一项不完整"。
inline std::optional<std::string> validate_value(
    const Json& value, const Schema& schema, const Schema& root,
    const std::string& path, bool in_item = false) {
    if (const auto ref = schema.find("$ref"); ref != schema.end() && ref->is_string()) {
        const Schema* target = resolve_ref(root, ref->get<std::string>());
        if (target == nullptr) return SAYF("%1 使用了无法解析的 $ref", path);
        return validate_value(value, *target, root, path, in_item);
    }

    if (const auto choices = schema.find("anyOf");
        choices != schema.end() && choices->is_array()) {
        for (const auto& choice : *choices) {
            if (!validate_value(value, choice, root, path, in_item).has_value())
                return std::nullopt;
        }
        return SAYF("%1 不符合 anyOf 中的任何一种结构", path);
    }

    if (const auto choices = schema.find("oneOf");
        choices != schema.end() && choices->is_array()) {
        int matched = 0;
        for (const auto& choice : *choices) {
            if (!validate_value(value, choice, root, path, in_item).has_value()) ++matched;
        }
        if (matched != 1) return SAYF("%1 不符合 oneOf 的唯一一种结构", path);
    }

    if (const auto type = schema.find("type"); type != schema.end()) {
        bool matched = true;
        if (type->is_string()) {
            matched = type_matches(value, type->get<std::string>());
        } else if (type->is_array()) {
            matched = false;
            for (const auto& one : *type) {
                if (one.is_string() && type_matches(value, one.get<std::string>())) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return SAYF("%1 的类型不符合 schema", path);
    }

    // **枚举对不上不在这儿拦。**
    //
    // 这一层是用来接住"整份输出坏了"的——被 length 截断、内容过滤掐掉、
    // 压根不是 JSON。枚举填错不是那一类：它只坏一个字段，而**下游本来就
    // 按「当它没填过」处理**（storyboard.cpp 的 drop_unknown_enums，那儿
    // 写着「是抹掉不是报错：这一栏填错不值得把另外十几个好镜头一起作废」）。
    //
    // 在这儿 return 一个错，等于把那条规矩推翻：2026-09-16 实测，一章
    // 十七镜里第三镜的 face_pose 填了个表外的值，整个 /api/plan 回 400，
    // 十七镜全没了——而那个字段本来会被抹成默认值，一镜都不该丢。
    //
    // const 那一条留着：它不是"一个字段填错"，是整份输出走错了分支。
    (void)0;
    if (const auto constant = schema.find("const");
        constant != schema.end() && value != Json(*constant)) {
        return SAYF("%1 不等于 schema 规定的常量", path);
    }

    if (value.is_string()) {
        const std::size_t length = text::utf8_len(value.get<std::string>());
        // **写短了不在这儿拦，写空了才拦。** 理由同上面枚举那一段。
        //
        // 这些 minLength 本来就是"推一把"的数，不是地板：last_line 的下限
        // 当初还从 12 降到 4 过，理由是「"走吧。"四个字正是一章的收口」。
        // 差一两个字和"整份输出坏了"根本不是一回事，而在这儿 return 一个错
        // 的代价是整章作废——2026-09-16 实测：
        //     异步作业砸了：大模型输出不符合 chapter Schema：
        //     $.scenes[2].worse 太短
        // 写了十分钟、三场戏都在，就因为第三场的一句话短了几个字，一个字
        // 都没留下。而这些模型（走远端 API 的那些）根本不按 GBNF 生成，
        // minLength 对它们从来只是建议。
        //
        // **空串还是拦**：那不是"写短了"，是这一栏压根没写——和被截断、
        // 被内容过滤掐掉是同一类，正是这一层该接住的。
        if (const auto it = schema.find("minLength");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() > 0 && length == 0) {
            return SAYF("%1 是空的", path);
        }
        if (const auto it = schema.find("maxLength");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() >= 0 &&
            length > static_cast<std::size_t>(it->get<long long>())) {
            return SAYF("%1 太长", path);
        }
    }

    if (value.is_array()) {
        if (const auto it = schema.find("minItems");
            it != schema.end() && it->is_number_integer() &&
            it->get<long long>() >= 0 &&
            value.size() < static_cast<std::size_t>(it->get<long long>())) {
            return SAYF("%1 的项目太少", path);
        }
        // **写多了不在这儿拦，写少了才拦。** 理由同上面枚举和 minLength。
        //
        // 2026-09-16 / 17 两次实撞，都是同一句：
        //     大模型输出不符合 script Schema：$.scenes.s2.beats 的项目太多
        // 一次五分钟的改编，就因为第二场多写了一拍，整份作废——而多出来的
        // 那一拍下游本来就吃得下（拍子会变成镜头，多一个少一个不是结构问题）。
        //
        // **minItems 还是拦**：少写了几场、几拍，下游没法凭空补出来，那是
        // 真的"这份输出不能用"。多和少在这儿不是对称的。
        (void)0;
        if (const auto items = schema.find("items");
            items != schema.end() && items->is_object()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                // 从这儿往下就是"数组里的一项"了
                if (auto err = validate_value(value[i], *items, root,
                                              path + "[" + std::to_string(i) + "]",
                                              /*in_item=*/true)) {
                    return err;
                }
            }
        }
    }

    if (value.is_object()) {
        // 数组里的一项缺字段不在这儿拦，见函数头上那段。
        if (const auto required = schema.find("required");
            !in_item && required != schema.end() && required->is_array()) {
            for (const auto& key : *required) {
                if (key.is_string() && !value.contains(key.get<std::string>())) {
                    return SAYF("%1 缺少必填字段 %2", path,
                                key.get<std::string>());
                }
            }
        }
        const auto properties = schema.find("properties");
        if (properties != schema.end() && properties->is_object()) {
            for (const auto& item : properties->items()) {
                const auto actual = value.find(item.key());
                if (actual == value.end()) continue;
                // 一项里面再往下（对象的字段、字段里的数组）都还算这一项
                if (auto err = validate_value(*actual, item.value(), root,
                                              path + "." + item.key(), in_item)) {
                    return err;
                }
            }
            // **多写了一个键不在这儿拦。** 理由同上面枚举、minLength、
            // maxItems 那三段——这一层接的是"整份输出坏了"，多一个键不是
            // 那一类：**下游是按名字取字段的，多出来的那个根本碰不到。**
            //
            // 这个项目自己在别处早就立过同一条规矩：`/api/run` 刻意不
            // forbid 多余的键，注释写着「前端和引擎的版本不一定同步升，
            // 多一个键就 422 会让整个功能挂掉」，改成照收不误、把不认识的
            // 列回去。模型这头更该这样——带推理的模型会把思考编成键名塞
            // 进来（2026-09-12 实见 `last_line_ending_context_hint_for_...`
            // 那一串，prompts.toml 的 chapter_write tail 上面记着），
            // 在这儿判废就是拿一次十分钟的正文去换一个没人读的键。
            //
            // **schema 里那句 `additionalProperties: false` 留着**：它是
            // 给模型看的、也进 GBNF 语法，那才是真管用的地方。这儿只是
            // 事后那一道，不该比语法还硬。
        }
    }

    return std::nullopt;
}

}  // namespace detail

inline std::optional<std::string> validate_json_schema(
    const nlohmann::json& value, const nlohmann::ordered_json& schema) {
    if (schema.is_null() || schema.empty()) return std::nullopt;
    return detail::validate_value(value, schema, schema, "$");
}

inline std::optional<std::string> validate_structured_output(
    const std::string& raw, const nlohmann::ordered_json& schema) {
    if (schema.is_null() || schema.empty()) return std::nullopt;
    try {
        const nlohmann::json value = stages::extract_json(raw);
        return validate_json_schema(value, schema);
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
}

}  // namespace changji::llm
