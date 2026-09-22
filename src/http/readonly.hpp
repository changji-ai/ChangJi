#pragma once

// 阶段 2 的只读接口。
//
// 六个接口：/api/hardware /api/project /api/shots /api/assets
//           /api/settings /api/projects
//
// 移植自 src/changji/web/server.py 里对应的路由。
//
// 契约标准是**结构兼容**（字段名、嵌套结构、取值、状态码一致，key 顺序不管），
// 见方案第三节。错误响应要和 FastAPI 的 HTTPException 一致：
// 状态码加一个 {"detail": "..."} 的 body——前端只认这个形状。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "util/say.hpp"
#include "models/hardware.hpp"

namespace changji::http {

/// 接口处理的结果：状态码加 JSON body。
///
/// 不直接返回 crow::response 是为了让单元测试能不起服务就把这些逻辑跑一遍。
/// 阶段 2 的对拍要比对的是 body，不是 HTTP 传输本身。
struct ApiResult {
    int status = 200;
    nlohmann::json body;
};

/// 抛出它等价于 FastAPI 的 raise HTTPException(status, detail)。
///
/// detail 可以是字符串，也可以是任意 JSON。后者是为了对上 FastAPI 自己的
/// 校验错误：pydantic 的 extra="forbid" 之类违规回的是 **422** 而不是 400，
/// 而且 detail 是一个结构化数组
/// （[{type, loc, msg, input}]），不是一句话。对拍语料抓到过这一条。
class ApiError : public std::runtime_error {
public:
    ApiError(int status, const std::string& detail)
        : std::runtime_error(detail), status_(status), detail_(detail) {}

    /// 带完整 JSON body 的那种。**不做成构造函数重载**：
    /// const char* 到 std::string 和到 nlohmann::json 都是一次用户定义转换，
    /// 两个构造函数会让 ApiError(400, "字面量") 变成歧义调用。
    static ApiError with_body(int status, const nlohmann::json& body) {
        ApiError e(status, body.dump());
        e.detail_ = body;
        return e;
    }

    int status() const { return status_; }
    const nlohmann::json& detail() const { return detail_; }

private:
    int status_;
    nlohmann::json detail_;
};

/// FastAPI 在 pydantic 校验失败时回的那种 422，**字段嵌在子模型里**的情形。
///
/// loc 的头两段固定是 ["body", <模型字段名>]，第三段是出问题的键。
/// 字段直接挂在请求模型上时 loc 只有两段，用下面的 unprocessable_top。
/// 形状对不上的话前端拿到的 detail 是数组而不是字符串，
/// 错误提示会显示成 [object Object]——那是现有行为，要原样保住。
inline ApiError unprocessable(const std::string& model_field,
                              const std::string& key,
                              const std::string& msg,
                              const nlohmann::json& input,
                              const std::string& type) {
    return ApiError::with_body(422, nlohmann::json{{"detail", nlohmann::json::array({
        nlohmann::json{
            {"type", type},
            {"loc", nlohmann::json::array({"body", model_field, key})},
            {"msg", msg},
            {"input", input},
        }})}});
}

/// 同上，但字段**直接挂在请求模型上**，loc 只有 ["body", <键>] 两段。
///
/// 两个函数不能合并：段数是 pydantic 按模型嵌套层数生成的，
/// 多一段少一段前端高亮的就是别的字段。
inline ApiError unprocessable_top(const std::string& key,
                                  const std::string& msg,
                                  const nlohmann::json& input,
                                  const std::string& type) {
    return ApiError::with_body(422, nlohmann::json{{"detail", nlohmann::json::array({
        nlohmann::json{
            {"type", type},
            {"loc", nlohmann::json::array({"body", key})},
            {"msg", msg},
            {"input", input},
        }})}});
}

/// 把一个数值边界按 pydantic 的写法印出来。
///
/// **不能用 std::to_string。** 那个对 double 固定给六位小数，
/// 1800.0 会变成 "1800.000000"，而 pydantic 的消息里是 "1800"。
/// 整数值就印整数，有小数才印小数。
inline std::string bound_text(double v) {
    if (v == static_cast<double>(static_cast<long long>(v))) {
        return std::to_string(static_cast<long long>(v));
    }
    std::string s = std::to_string(v);
    // 去掉尾巴上的零：0.500000 -> 0.5
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

/// 数值越界的那种 422。**比 unprocessable_top 多一个 ctx。**
///
/// pydantic 在越界错误里带一个 ctx 说明边界是多少（{"le": 1800.0}）。
/// 少了它前端就没法在提示里填出"最大 1800"——只能把 msg 整句显示出来，
/// 而那是英文的。这一条是实时对拍抓出来的。
inline ApiError out_of_range(const std::string& key, const std::string& msg,
                             const nlohmann::json& input,
                             const std::string& type,
                             const std::string& bound_key,
                             const nlohmann::json& bound) {
    return ApiError::with_body(422, nlohmann::json{{"detail", nlohmann::json::array({
        nlohmann::json{
            {"type", type},
            {"loc", nlohmann::json::array({"body", key})},
            {"msg", msg},
            {"input", input},
            {"ctx", nlohmann::json{{bound_key, bound}}},
        }})}});
}

/// FastAPI 在**查询参数**缺失时回的那种 422。
///
/// loc 是 ["query", <参数名>]，和请求体那两个（["body", ...]）不是一回事。
///
/// 这一条是实时对拍抓出来的：C++ 侧原先直接进处理函数，缺 episode_id 时
/// 回的是 404「没有章节 」（注意末尾那个空格，参数是空的）。而 FastAPI
/// 在处理函数跑之前就把请求拦下来了，回 422 加一个结构化的 detail。
/// 状态码和 detail 的形状都是契约的一部分。
///
/// **"给了但是空的"不算缺失**：`?path=` 在 FastAPI 那边是一个合法的空字符串，
/// 会进处理函数然后回 400「没有指定项目目录」。语料里有一条专门测这个。
inline ApiError unprocessable_query(const std::string& name) {
    return ApiError::with_body(422, nlohmann::json{{"detail", nlohmann::json::array({
        nlohmann::json{
            {"type", "missing"},
            {"loc", nlohmann::json::array({"query", name})},
            {"msg", "Field required"},
            {"input", nullptr},
        }})}});
}

// 每个接口一个纯函数。参数就是 Python 那边的查询参数。

ApiResult get_hardware(const config::Settings& settings);
ApiResult get_settings(const config::Settings& settings);

/// 同上，但**画像由调用方给**。
///
/// 路由层要传 runtime().profile()——那份应用了 /api/settings 改过的
/// 画质档位。上面那两个自己 detect() 一遍，会把覆盖绕过去，
/// 表现是用户改了草稿分辨率，接口回"已应用"，但 /api/hardware 还是老值。
/// 这个坑实机验证时撞到过：单元测试直接调 runtime().profile() 所以没发现。
ApiResult get_hardware(const config::Settings& settings,
                       const models::HardwareProfile& profile);
ApiResult get_settings(const config::Settings& settings,
                       const models::HardwareProfile& profile);
ApiResult get_projects(const config::Settings& settings);
ApiResult get_project(const std::string& path);
ApiResult get_shots(const std::string& path, const std::string& episode_id);
ApiResult get_assets(const std::string& path);

/// 列一个目录下面有哪些子目录。**给"挑一个文件夹"用的，所以只回目录。**
///
/// `path` 留空时回几个起点（用户目录、项目库、当前模型目录），不然人得
/// 自己先敲一段路径才能开始点——而"不知道从哪儿开始"正是要浏览器的理由。
///
/// **不列文件**：这条接口唯一的用处是选一个放模型的地方，把几百个 .gguf
/// 也列出来只会把目录淹掉。
///
/// 只读。写在这个文件里是因为它和别的 get_* 一样不碰任何状态。
ApiResult get_dirs(const std::string& path, const config::Settings& settings);

/// 请求体解析不动时，按 **Python 真实行为** 分四种情况回。
///
/// 之前这里是 `json::parse(body, nullptr, false)`，失败回一个 discarded 值，
/// 被直接塞进 422 的 `input` 字段——而 discarded 的 dump() 是字面量
/// `<discarded>`，**那不是 JSON**，客户端 `JSON.parse` 直接抛，
/// 连错误信息都读不出来。而且报的是"Field required"，
/// 指着一个根本没读到的字段。
///
/// 起两个引擎逐条比出来的（对拍「坏请求体」那五条）：
///
/// | 请求体 | Python |
/// |---|---|
/// | 不是合法 UTF-8 | 400 `{"detail":"There was an error parsing the body"}` |
/// | 空 | 422 `missing`，loc `["body"]`，input `null` |
/// | 能解码但不是 JSON | 422 `json_invalid`，loc `["body", 出错位置]` |
/// | 是 JSON 但不是对象 | 422 `model_attributes_type` |
///
/// **第一版我把五种全回成 400**，因为只拿"非法 UTF-8"那一种验过就收工了。
/// 五分之四是错的。
inline bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        if (i + n >= s.size() + (n == 0 ? 1 : 0)) { if (n) return false; }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

/// 字节偏移换成字符偏移。
///
/// **Python 的 `loc` 里那个数是字符位置，不是字节位置**——它在 str 上解析。
/// 纯 ASCII 时两者一样，所以只拿英文试是试不出这个差别的。
inline std::size_t char_offset(const std::string& s, std::size_t byte_off) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < byte_off && i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++chars;
    }
    return chars;
}

inline nlohmann::json parse_body(const std::string& raw) {
    // 一、解码那一层就挂了。Starlette 在读 body 时就拦下，还没轮到 JSON。
    if (!valid_utf8(raw)) {
        throw ApiError(400, "There was an error parsing the body");
    }

    // 二、空 body。pydantic 当成"这个字段没给"。
    if (raw.empty()) {
        throw ApiError::with_body(422, nlohmann::json{{"detail",
            nlohmann::json::array({nlohmann::json{
                {"type", "missing"},
                {"loc", nlohmann::json::array({"body"})},
                {"msg", "Field required"},
                {"input", nullptr},
            }})}});
    }

    // 三、能解码，但不是 JSON。
    nlohmann::json v;
    try {
        v = nlohmann::json::parse(raw);
    } catch (const nlohmann::json::parse_error& e) {
        // nlohmann 的 byte 是 1 起的字节位置，Python 的 pos 是 0 起的字符位置
        const std::size_t off =
            char_offset(raw, e.byte > 0 ? e.byte - 1 : 0);
        throw ApiError::with_body(422, nlohmann::json{{"detail",
            nlohmann::json::array({nlohmann::json{
                {"type", "json_invalid"},
                {"loc", nlohmann::json::array({"body", off})},
                {"msg", "JSON decode error"},
                {"input", nlohmann::json::object()},
                // 文字不算契约（对拍有 /**/error 的忽略规则），
                // 但形状要在——前端会读它
                {"ctx", nlohmann::json{{"error", "Expecting value"}}},
            }})}});
    }

    // 四、是 JSON，但不是对象。请求模型都是对象。
    if (!v.is_object()) {
        throw ApiError::with_body(422, nlohmann::json{{"detail",
            nlohmann::json::array({nlohmann::json{
                {"type", "model_attributes_type"},
                {"loc", nlohmann::json::array({"body"})},
                {"msg", "Input should be a valid dictionary or object to "
                        "extract fields from"},
                {"input", v},
            }})}});
    }
    return v;
}

/// 把上面那些函数的异常翻成 ApiResult。路由层统一走它。
template <typename F>
ApiResult guard(F&& fn) {
    try {
        return fn();
    } catch (const ApiError& e) {
        // detail 已经是完整 body（422 那种）就原样返回，
        // 否则包成 {"detail": "..."}——HTTPException 的形状。
        const auto& d = e.detail();
        if (d.is_object() && d.contains("detail")) return {e.status(), d};
        return {e.status(), {{"detail", d}}};
    } catch (const std::exception& e) {
        // Python 那边未捕获的异常会被 FastAPI 变成 500 加一句
        // "Internal Server Error"。这里把真实信息带出来——
        // 那句话什么也没说，用户报障时只能贴一张没用的截图。
        return {500, {{"detail", SAYF("服务端出错：%1", e.what())}}};
    }
}

}  // namespace changji::http
