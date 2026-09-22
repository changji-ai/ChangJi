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
