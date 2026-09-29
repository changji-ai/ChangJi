#pragma once

// nlohmann/json 到 v3.11.3 为止不认 std::optional，而 Python 那边一半的字段
// 是 `X | None`。这里补上：nullopt 序列化成 null，null 反序列化成 nullopt，
// 键缺失也当 nullopt。
//
// 不用 NLOHMANN_DEFINE_TYPE 的 WITH_DEFAULT 变体自带的缺失处理来代替——
// 那个只管键缺失，不管显式的 null，而 Python 写出来的 JSON 里
// `"last_frame_prompt": null` 是常态。

#include <optional>
#include <nlohmann/json.hpp>

namespace nlohmann {

template <typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json& j, const std::optional<T>& opt) {
        if (opt.has_value()) {
            j = *opt;
        } else {
            j = nullptr;
        }
    }

    static void from_json(const json& j, std::optional<T>& opt) {
        if (j.is_null()) {
            opt = std::nullopt;
        } else {
            opt = j.get<T>();
        }
    }
};

}  // namespace nlohmann

namespace changji::models {

/// 递归去掉**对象里**值为 null 的键，交给 `..._WITH_DEFAULT` 按默认值补。
///
/// 那个宏读每一栏是 `j.value(key, 默认值)`：键不在回默认值，**键在而值是 null
/// 就抛 type_error.302**。于是 project.json 里一栏 `"title": null`（手改的、老版本
/// 写的、别的工具生成的）让整部片子读不进来——每一条接口都回 400，包括拿来修它
/// 的那几条。去掉之后和「没写这一栏」一样：std::optional 那几栏本来就是 nullopt。
///
/// **数组里的 null 不动**：几组数组是按下标对齐的（台词和它的时长），抽掉一个
/// 后面全错一格，比读不进来更难发现。
template <typename J>
void drop_nulls(J& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end();) {
            if (it->is_null()) {
                it = j.erase(it);
            } else {
                drop_nulls(*it);
                ++it;
            }
        }
    } else if (j.is_array()) {
        for (auto& e : j) drop_nulls(e);
    }
}

}  // namespace changji::models
