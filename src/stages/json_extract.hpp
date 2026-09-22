#pragma once

// 从大模型输出里抠出 JSON。
//
// 即使要求"只输出 JSON"，模型也常包一层 ``` 代码块或者在前面加一句话。
// 三个用大模型的阶段都要过这一关，所以单独放。

#include <string>

#include <nlohmann/json.hpp>

namespace changji::stages {

/// 抠 JSON。找不到合法 JSON 时抛 std::runtime_error。
///
/// 三步，和 Python 的 _extract_json 一致：
/// 1. 整段 strip，如果有 ``` 代码块就取里面的内容
/// 2. 直接解析
/// 3. 还不行就找第一个括号平衡的对象或数组
nlohmann::json extract_json(const std::string& raw);

}  // namespace changji::stages
