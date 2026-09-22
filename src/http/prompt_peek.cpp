#include "http/prompt_peek.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include <nlohmann/json.hpp>

namespace changji::http {

bool take_peek(nlohmann::json& body) {
    if (!body.is_object()) return false;
    const auto it = body.find("peek");
    if (it == body.end()) return false;
    const bool on = it->is_boolean() ? it->get<bool>() : false;
    body.erase(it);
    return on;
}

std::string take_paste(nlohmann::json& body) {
    if (!body.is_object()) return {};
    const auto it = body.find("paste");
    if (it == body.end()) return {};
    std::string raw = it->is_string() ? it->get<std::string>() : std::string();
    body.erase(it);
    return raw;
}

namespace {

/// 粘回来的是不是那份「场的数组」。是就切成一场一段（按 `scene` 排好），
/// 不是回 false，交给文本头那条路。
///
/// **不在这儿校验镜头。** 每一段只是重新包成 `{"shots": [...]}`，里面是什么
/// 由下游 parse_storyboard 说了算——那儿的规矩（枚举清洗、长度、引用）不该
/// 在这儿再抄一份。
bool split_json_scenes(const std::string& pasted, std::vector<std::string>& out) {
    // 快速排除：不以 [ 或 { 开头的根本不用去解，几万字的文本头那种直接跳过。
    std::size_t i = 0;
    while (i < pasted.size() && std::isspace(static_cast<unsigned char>(pasted[i]))) ++i;
    if (i >= pasted.size() || (pasted[i] != '[' && pasted[i] != '{')) return false;

    nlohmann::json doc = nlohmann::json::parse(pasted, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return false;
    // `{"scenes": [...]}` 包一层也认；别的对象（比如单场的 `{"shots": [...]}`）
    // 不是数组形状，回 false 让老路当一段处理。
    if (doc.is_object()) {
        const auto it = doc.find("scenes");
        if (it == doc.end() || !it->is_array()) return false;
        doc = *it;
    }
    if (!doc.is_array() || doc.empty()) return false;

    // 每一项得是 {"shots": [...]}，缺 shots 的不算这种形状。
    struct Part {
        int scene;
        std::size_t pos;
        nlohmann::json shots;
    };
    std::vector<Part> parts;
    for (std::size_t k = 0; k < doc.size(); ++k) {
        const nlohmann::json& item = doc[k];
        if (!item.is_object()) return false;
        const auto sh = item.find("shots");
        if (sh == item.end() || !sh->is_array()) return false;
        int scene = 0;
        const auto sc = item.find("scene");
        if (sc != item.end() && sc->is_number_integer()) scene = sc->get<int>();
        parts.push_back({scene, k, *sh});
    }
    // **按 scene 排回去；没写 scene 的按原顺序。** 全都没写的话等于按顺序，
    // 和文本头那条路一样。
    std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
        if (a.scene != b.scene) return a.scene < b.scene;
        return a.pos < b.pos;
    });
    out.clear();
    for (const Part& p : parts) {
        nlohmann::json one = nlohmann::json::object();
        one["shots"] = p.shots;
        out.push_back(one.dump());
    }
    return true;
}

}  // namespace

std::vector<std::string> split_by_scene(const std::string& pasted) {
    // 先认 JSON 数组（见头文件那段），不是才按文本头切。
    if (std::vector<std::string> parts; split_json_scenes(pasted, parts)) {
        return parts;
    }
    // 行首是 `=====` 就算一个头。**按行认，不按正则**：场次名里什么字符
    // 都可能有（「日 · 外 · 后门货场」带点带空格），而这一行的开头是我们
    // 自己拼的，认它最稳。
    //
    // ⚠️ **认的是那几个等号，不是「第」字。** 原来这儿写的是
    // `"===== 第 "`——而 2026-09-22 那一行被翻译了（`storyboard_run.cpp`
    // 里的 `SAYF("\n\n===== 第 %1/%2 场 =====\n\n", …)`），德语下它是
    // `===== Szene 1 von 3 =====`。认中文的话，**非中文界面上「复制出去、
    // 粘回来」这条路整条断掉**：切不出段来，然后那句"粘回来的只有 1 段"
    // 反过来叫人留着一行他从来没见过的字。
    // 这是同一条老规矩的又一次：**别从给人看的文本里反解结构**。等号是
    // 结构，「第 … 场」是话。
    // 守着这件事的是 `test_i18n.cpp` 里那条"十一种语言的场次头都切得开"。
    static const std::string kMark = "=====";
    std::vector<std::string> out;
    std::string cur;
    bool seen_head = false;
    std::size_t i = 0;
    while (i <= pasted.size()) {
        const std::size_t nl = pasted.find('\n', i);
        const std::string line =
            pasted.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        const bool head = line.rfind(kMark, 0) == 0;
        if (head) {
            if (seen_head) out.push_back(cur);
            cur.clear();
            seen_head = true;
        } else if (seen_head) {
            cur += line;
            cur += '\n';
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    if (seen_head) out.push_back(cur);
    // 一个头都没有：整章一次拆那条路只有一段，别逼人去写分隔头。
    if (out.empty() && !pasted.empty()) out.push_back(pasted);
    return out;
}

ApiResult peek_prompt(const llm::Request& req) {
    return {200,
            {{"peek", true},
             {"stage", req.schema_name},
             // **就是这一份**：user 消息里落的那段。schema 不走
             // response_format，是以文字贴在提示词后面的（见 schema_as_prompt），
             // 所以贴出去的这一段就是模型看到的全部。
             {"prompt", llm::schema_as_prompt(req.prompt, req.schema)}}};
}

}  // namespace changji::http
