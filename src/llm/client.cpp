#include "util/say.hpp"
#include "llm/client.hpp"
#include "llm/thinking.hpp"
#include "llm/local_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <functional>
#include <string>
#include <utility>

#include "config/runtime.hpp"
#include "llm/call_log.hpp"
#include "llm/schema_validate.hpp"
#include "llm/sse.hpp"
#include "stages/prompts.inc.hpp"
#include "util/cancel_words.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::llm {

namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return s;
}

/// 从错误响应里挖出服务说了什么。
///
/// 对应 Python 的
///   detail = str(body.get("error") or body.get("message") or "")[:200]
///   except: detail = response.text[:200]
std::string error_detail(const std::string& body) {
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    std::string detail;
    if (!parsed.is_discarded() && parsed.is_object()) {
        for (const char* key : {"error", "message"}) {
            const auto it = parsed.find(key);
            if (it == parsed.end() || it->is_null()) continue;
            // 字符串直接用；别的类型 dump 成 JSON。
            //
            // 和 Python 有出入：那边是 str(dict)，吐的是 Python 的字典
            // repr（单引号、True/False）。在 C++ 里复刻那个格式既荒唐又没用——
            // 这段文字只是原样显示给用户看的，不解析。
            detail = it->is_string() ? it->get<std::string>() : it->dump();
            if (!detail.empty()) break;
        }
    } else {
        detail = body;
    }
    return text::truncate_utf8(detail, 200);
}

/// 服务说的是"这个模型不让你用"吗？是的话返回它那句原话，否则空串。
///
/// **只认几个说法明确的短语**，宁可漏判。漏判的代价是退回原来那句
/// 「八成是 API Key 不对」——不理想但不致命，而且服务的原话本来就跟在
/// 后面（explain_status 末尾会附「服务说：…」）。误判的代价大得多：
/// 真的密钥过期时告诉人家"换个模型试试"，那才是把人支到沟里。
std::string model_not_allowed(const std::string& detail) {
    static const char* kMarks[] = {
        "only available on",      // OpenRouter: only available on agentic harnesses
        "not available to you",
        "no access to",
        "does not have access",
        "requires a paid",
        "not allowed to use",
    };
    const std::string low = lower_ascii(detail);
    for (const char* m : kMarks) {
        if (low.find(m) != std::string::npos) return detail;
    }
    return {};
}

std::string completion_reason(const std::string& raw_body) {
    const json body = json::parse(raw_body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) return {};
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty() ||
        !(*choices)[0].is_object()) {
        return {};
    }
    const auto finish = (*choices)[0].find("finish_reason");
    return finish != (*choices)[0].end() && finish->is_string()
               ? finish->get<std::string>()
               : std::string();
}

void require_complete_reason(const std::string& reason) {
    if (reason.empty() || reason == "stop" || reason == "tool_calls") return;
    if (reason == "length") {
        throw LlmError(SAY("大模型输出达到长度上限，返回内容被截断。"
                           "请提高该模型的输出上限，或缩短输入后重试"));
    }
    if (reason == "content_filter") {
        throw LlmError(SAY("大模型服务的内容过滤器中止了生成"));
    }
    throw LlmError(SAYF("大模型没有正常完成生成（finish_reason=%1）", reason));
}

std::string checked_output(const Request& req, std::string out) {
    if (const auto err = validate_structured_output(out, req.schema)) {
        throw LlmError(SAYF("大模型输出不符合 %1：%2",
                            req.schema_name.empty()
                                ? std::string("JSON Schema")
                                : req.schema_name + " Schema",
                            *err));
    }
    return out;
}

/// 开一份记录器。**构造这一下抛了，也不许把这一次生成带走。**
///
/// 构造真的会抛：`call_log_options` 里那句
/// `paths::from_utf8(paths::env("CHANGJI_LLM_LOG_DIR"))` 走
/// `std::filesystem::u8path`，MSVC 上转不动的串当场是 `filesystem_error`；
/// 记录器自己那头还要取一次任务账本的锁、拷几个字符串。
///
/// 后果不是"少记一次"：`filesystem_error` **不是 `LlmError`**，而批量那几条
/// 只 `catch (const LlmError&)`，一整批当场终止——等于为了记日志把本来会成功
/// 的那一次生成弄砸了。call_log.hpp 上那条「写盘出问题不许影响生成」原来只落
/// 在析构那一半，构造这一半是漏的。
///
/// 兜底那一支传的是**关掉的** `CallLogOptions`：记录器于是整个不动手（不建
/// 目录、不取 id、不读线程局部的那两样），只剩几个字符串的拷贝。它再抛就是
/// 这台机器没内存了，和记不记日志无关。
///
/// 返回 prvalue 是**有意的**：`CallLog` 既不可拷也不可移，C++17 保证就地
/// 构造，所以这儿不能改成"先声明再 return 那个变量"。
std::optional<CallLog> open_call_log(const char* backend, const char* kind,
                                     const Request& req,
                                     const config::LLMConfig& cfg) {
    try {
        return std::optional<CallLog>(std::in_place, backend, kind, req,
                                      call_log_options(cfg));
    } catch (...) {
        CallLogOptions off;
        off.enabled = false;
        return std::optional<CallLog>(std::in_place, backend, kind, req, off);
    }
}

}  // namespace

namespace {

/// 把 pydantic 自动生成的 `title` 摘掉。
///
/// **它是纯噪声。** 那一栏的值就是键名换个写法——`shot_id` 配
/// 「"title": "Shot Id"」、`visual_desc` 配「"title": "Visual Desc"」，
/// 模型从键名已经知道的东西再说一遍。2026-09-17 量的：分镜那份里 17 个，
/// 369 字符；这份 schema 是整条提示词里最大的一块。
///
/// 仓库里**没有一个手写的 title 注解**（`{"title", …}` 在各阶段的
/// 构造里一次都没出现），所以摘掉不会丢掉谁写的话。
///
/// ⚠️ **`properties` 底下那一层的键是字段名，不是关键字。** 剧本那份
/// schema 里就真有一个叫 `title` 的字段（`props["title"]`，一章的标题）。
/// 不分这一层的话，递归摘 title 会把那个字段整个删掉——模型于是再也不
/// 会填标题，而且一声不响。`required` 数组里的 "title" 是值不是键，
/// 下面数组那一支原样留着。
///
/// 只摘文字这一份，校验那边拿到的还是原样。
ordered strip_titles(const ordered& node) {
    if (node.is_array()) {
        ordered out = ordered::array();
        for (const auto& v : node) out.push_back(strip_titles(v));
        return out;
    }
    if (!node.is_object()) return node;
    ordered out = ordered::object();
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string& k = it.key();
        if (k == "title" && it.value().is_string()) continue;
        // 这几个底下一层是名字，原样留着，只往各自的值里走。
        if (k == "properties" || k == "$defs" || k == "definitions" ||
            k == "patternProperties") {
            ordered kids = ordered::object();
            for (auto c = it.value().begin(); c != it.value().end(); ++c) {
                kids[c.key()] = strip_titles(c.value());
            }
            out[k] = std::move(kids);
            continue;
        }
        out[k] = strip_titles(it.value());
    }
    return out;
}

/// 把没人 $ref 的 $defs 摘掉。
///
/// **这些定义是随模型的 JSON Schema 整份带进来的**，而各阶段只挑用得上的
/// 那几个字段、还把枚举内联在属性上（storyboard.cpp 里
/// `defs["CameraMove"]["enum"]` 读出来再写进 kept）。于是定义本身成了没人
/// 指向的孤儿，却照样贴给模型读。2026-09-17 量的分镜那份：CameraAngle、
/// CameraMove、ShotStatus、Transition、Lens 五个一次都没被引用，合计 635
/// 字符（带缩进一千出头），其中 CameraMove 那串枚举还和属性上内联的那份
/// 一模一样——**同一串东西模型读两遍**。
///
/// 只摘文字这一份，校验那边拿到的还是原样（它也只解析引用得到的那些）。
/// 传递闭包：被引用的定义自己再引用别的，那些也得留。
ordered prune_unused_defs(const ordered& schema) {
    const auto defs = schema.find("$defs");
    if (defs == schema.end() || !defs->is_object()) return schema;

    const auto refs_in = [](const ordered& node, std::set<std::string>& out) {
        const std::function<void(const ordered&)> walk = [&](const ordered& n) {
            if (n.is_object()) {
                for (auto it = n.begin(); it != n.end(); ++it) {
                    if (it.key() == "$ref" && it.value().is_string()) {
                        const std::string r = it.value().get<std::string>();
                        const std::string head = "#/$defs/";
                        if (r.rfind(head, 0) == 0) out.insert(r.substr(head.size()));
                    }
                    walk(it.value());
                }
            } else if (n.is_array()) {
                for (const auto& v : n) walk(v);
            }
        };
        walk(node);
    };

    // 先看正文引用了谁（不含 $defs 自己）
    ordered body = schema;
    body.erase("$defs");
    std::set<std::string> want;
    refs_in(body, want);
    // 传递闭包
    for (bool grew = true; grew;) {
        grew = false;
        const std::set<std::string> seen = want;
        for (const auto& name : seen) {
            const auto it = defs->find(name);
            if (it == defs->end()) continue;
            std::set<std::string> more;
            refs_in(*it, more);
            for (const auto& m : more) {
                if (want.insert(m).second) grew = true;
            }
        }
    }
    if (want.size() == defs->size()) return schema;

    ordered out = schema;
    ordered kept = ordered::object();
    for (auto it = defs->begin(); it != defs->end(); ++it) {
        if (want.count(it.key()) != 0) kept[it.key()] = it.value();
    }
    if (kept.empty()) {
        out.erase("$defs");
    } else {
        out["$defs"] = std::move(kept);
    }
    return out;
}

}  // namespace

std::string schema_as_prompt(const std::string& prompt, const ordered& schema) {
    if (schema.is_null() || schema.empty()) return prompt;
    // **换行留着，缩进的空格不留。**
    //
    // 这份东西是给模型读的，分镜那份有几十个字段；压成一行之后连括号配对
    // 都要一个一个数，所以换行要留。但**缩进的空格一个字都不告诉人任何
    // 事**——它是这份提示词里最大的一块里最没用的那一层：
    //
    //   分镜那份 schema，真正发出去的（剪过没人引用的 $defs）
    //     缩进 2   ~9300 字符                      ← 2026-09-17 之前
    //     缩进 1    7438 字符（空白 2000 上下）    ← 上一版
    //     缩进 0    5600 上下（只剩换行）          ← 现在
    //     压成一行  5000 上下
    //
    // 同一份提示词里那张硬性要求表才 859 字符。**真正臃肿的是 schema，
    // 而且一大半是排版。** 每一个带 schema 的阶段都省这一份（2026-09-14
    // 起 schema 只有这一种发法，见下面 build_payload）。
    //
    // 一个约束都没动：摘掉的是缩进和 pydantic 自动生成的 title，
    // enum / required / minItems / description 一条不少。
    return prompt + stages::prompt::llm::kSchemaSuffix +
           strip_titles(prune_unused_defs(schema)).dump(0);
}

ordered build_payload(const config::LLMConfig& cfg, const Request& req) {
    ordered messages = ordered::array();
    // **schema 以文字贴在提示词后面，不走 response_format。**
    //
    // 2026-09-14 决定：结构约束整个交给提示词。理由是那层"硬约束"早就不硬了——
    // 本地那条的 GBNF 随本地后端一起删了；远端严格模式各家支持得七零八落
    // （OpenRouter 上一大半模型压根不收 response_format，智谱那个免费档收下
    // 之后既不报错也不照做，直接回一段散文），为了兜住这些差异原来挂着一部
    // 四档退档梯子，而梯子本身又会被别的 400 误触发——2026-09-14 glm-5.3
    // 那次就是：它只是不收 thinking 的关闭值，结果十次生成全被退到最底一档。
    //
    // 一档到底反而实在：每次发的都一样，没有"这次到底走的哪一档"这个问题。
    messages.push_back(ordered{
        {"role", "user"}, {"content", schema_as_prompt(req.prompt, req.schema)}});

    ordered payload = ordered::object();
    payload["model"] = cfg.model;
    payload["messages"] = messages;
    // **这里原来发的是 cfg.temperature，req 里那个从来没人读。** 于是
    // kChapterTemperature（写正文 0.5）在远端那条路上一直空转——而远端
    // 正是现在的默认后端。两条路都返回 200，所以这件事只能靠读代码发现。
    // 见 Request::temperature 上那段。
    payload["temperature"] = req.temperature.value_or(cfg.temperature);

    // **少想一点，只对认得这个参数的模型说。**
    //
    // 智谱的 `reasoning_effort` 默认是 max；文档写着 GLM-4.5 及以上支持
    // `thinking`，GLM-5.2 及以上支持 `reasoning_effort`（5.3 / 5.3-Flash
    // 只收 low / high / max）。这儿按**模型名**认，不按地址：同一个网关
    // 后面可以挂别家的模型，而认错的代价是别家收到不认识的字段直接 400——
    // 那会让所有生成一起挂，比"想得久"严重得多。
    //
    // 认不出就一个字段都不多发，和这一行加进来之前完全一样。
    //
    // **档位从配置来，不再各步各写死一行。** 2026-09-19 之前是 chapter 和
    // storyboard 两处代码里写死 "high"，而出大纲、读正文提结构这几步没人
    // 说话——于是那几步走的是智谱的默认 max，一趟十几分钟不回。写死还有
    // 第二个毛病：模型窗里那个档位调了也不算数。现在 `req` 里填了就用填的
    // （给单测和特例留的口子），没填就问配置（`[llm] reasoning_effort` /
    // `[llm.effort] <任务>`）。
    const std::string effort = req.reasoning_effort.empty()
                                   ? cfg.effort_for(req.schema_name)
                                   : req.reasoning_effort;
    // 哪一家认哪几档、字段叫什么，**全在 `llm/thinking.hpp` 那一张表里**
    // ——这儿一个模型名都不判。原来这儿和带工具那条各写了一份一模一样的
    // `glm-5` 判断，界面那头还有第三份写死的"都不能选"（2026-09-21 收的）。
    apply_thinking(payload, cfg.base_url, cfg.model, effort);
    return payload;
}

/// 整段那条回来的思考。**没有就是空串。**
///
/// 整段那条拿不到过程，只能在收完之后一次性把整段思考给出去——界面上
/// 那个浮层于是"想完了才有内容"。比没有强：用户至少能回头看它想了什么。
std::string extract_thinking(const std::string& raw_body) {
    const json body = json::parse(raw_body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) return {};
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty()) return {};
    const auto& first = (*choices)[0];
    if (!first.is_object()) return {};
    const auto msg = first.find("message");
    if (msg == first.end() || !msg->is_object()) return {};
    for (const char* k : {"reasoning_content", "reasoning"}) {
        const auto r = msg->find(k);
        if (r != msg->end() && r->is_string()) return r->get<std::string>();
    }
    return {};
}

std::string extract_content(const std::string& raw_body) {
    const json body = json::parse(raw_body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
        throw LlmError(SAYF("大模型返回的不是 JSON：%1",
                            text::truncate_utf8(raw_body, 200)));
    }
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty()) {
        throw LlmError(SAYF("大模型返回格式异常：%1",
                            text::truncate_utf8(body.dump(), 400)));
    }
    const auto& first = (*choices)[0];
    if (!first.is_object()) {
        throw LlmError(SAYF("大模型返回格式异常：%1",
                            text::truncate_utf8(body.dump(), 400)));
    }
    const auto msg = first.find("message");
    if (msg == first.end() || !msg->is_object()) {
        throw LlmError(SAYF("大模型返回格式异常：%1",
                            text::truncate_utf8(body.dump(), 400)));
    }
    const auto content = msg->find("content");
    if (content == msg->end() || !content->is_string()) {
        throw LlmError(SAYF("大模型返回格式异常：%1",
                            text::truncate_utf8(body.dump(), 400)));
    }
    return content->get<std::string>();
}

std::string explain_status(const config::LLMConfig& cfg, int status,
                           const std::string& body) {
    const std::string url = cfg.base_url + "/chat/completions";
    const std::string detail = error_detail(body);

    std::string hint;
    if (status == 404) {
        // 404 有两种：地址不对，和模型没拉下来。Ollama 两种都回 404，
        // 一律说「地址填错了」会把人支到错误的地方去查。
        const std::string low = lower_ascii(detail);
        const bool about_model =
            low.find("model") != std::string::npos &&
            (low.find("not found") != std::string::npos ||
             low.find("not exist") != std::string::npos);
        if (about_model) {
            hint = SAYF("大模型服务在，但它没有 %1 这个模型。"
                        "本地 Ollama 的话先 ollama pull %1，"
                        "或者去项目页「模型」那一行点一下编剧模型的名字，"
                        "在弹出来的窗口里换一个已经有的。", cfg.model);
        } else {
            hint = SAYF("大模型服务在 %1 上没有这个接口。"
                        "多半是地址填错了——地址要带 /v1 结尾，"
                        "而且那台机器上的服务得真的起着。"
                        "改它：桌面端在「设置 ▸ 大模型 ▸ 地址」，"
                        "网页那一套在项目页「模型」那一行点一下编剧模型的名字。",
                        url);
        }
    } else if (status == 401 || status == 403) {
        // ⚠️ **这几句话两个界面都会原样显示，所以别报路**（2026-09-21）。
        //
        // 原来写的是「去项目页「模型」那一行点一下编剧模型的名字」——那是
        // 网页那一套的走法。桌面端上根本没有「项目页」，而它自己**有**一个
        // 填密钥的地方（设置 ▸ 大模型 ▸ 密钥）。照那句话做的人会在桌面端里
        // 到处找一个不存在的东西。
        //
        // 改法是**两头都说一句**。想过只说「去设置里填」不说路——可那两头
        // 的路是真不一样（桌面端有一张自己的设置纸，网页那一套是在项目页
        // 点开一个对话框），含糊一句等于两头都没说清。多一句话换一个人不用
        // 到处找，值。
        //
        // ⚠️ **改这两句的时候两头一起改**，别只顾一头（CLAUDE.md 第八条）。
        //
        // **三种情况要说三句不同的话。** 都说成「API Key 不对」的话，
        // 前两种会把人支去检查一个他根本没填过的东西。
        if (cfg.api_key.empty()) {
            hint = SAYF("还没填 API Key——桌面端在「设置 ▸ 大模型 ▸ 密钥」，"
                        "网页那一套在项目页「模型」那一行点一下编剧模型的名字。"
                        "当前地址是 %1。", cfg.base_url);
            if (cfg.base_url.find("bigmodel.cn") != std::string::npos ||
                cfg.base_url.find("z.ai") != std::string::npos) {
                // ⚠️ **两句话接在一起，接缝归被接的那一句管。** 中文
                // 「。」后面不空格，西文句号后面要空一格——所以那几种语言的
                // 译文自己带一个前导空格，中日韩的不带。在这儿写死一个
                // `" "` 的话，中文那份就多出一个空格。
                hint += SAY("去 bigmodel.cn 控制台领一把——默认挑的 "
                            "glm-4.7-flash 本身不要钱，但服务仍然要认人。");
            }
        } else if (!model_not_allowed(detail).empty()) {
            // **403 不都是密钥问题。** 2026-09-13 实测：OpenRouter 上
            // thinkingmachines/inkling:free 回的是
            //   "only available on agentic harnesses. Try plugging it into
            //    a coding agent or productivity app listed on …/apps"
            // ——密钥完全正常（同一把密钥列得出 445 个模型），是**这个模型
            // 的免费档限定入口**，只给它登记过的那些应用用。
            //
            // 照老话术报的话，用户会去反复换一把其实没问题的密钥，而真正
            // 该做的是换一个模型。**服务已经把原因说清楚了，照抄就是**——
            // 我们猜的那句反而盖住了它。
            hint = SAYF("这个模型不让我们用（%1）：%2"
                        "\n密钥本身多半没问题——去项目页那个模型窗口里换一个"
                        "模型试试。",
                        std::to_string(status), model_not_allowed(detail));
        } else {
            hint = SAYF("大模型服务拒绝了这次请求（%1），八成是 API Key 不对。"
                        "去项目页「模型」那一行点一下编剧模型的名字，"
                        "在那个窗口里换一把。", std::to_string(status));
        }
    } else if (status == 429) {
        // **429 不一定是"太频繁"。** 智谱把"余额不足/没有可用资源包"也回
        // 429（code 1113），2026-09-13 实测：拿免费密钥调 glm-4.7 这类收费
        // 模型就是这个。报成"等一会儿再试"的话，用户会一直等一件永远不会
        // 自己好起来的事。
        if (detail.find("1113") != std::string::npos ||
            // ⚠️ **这两个字是拿去比服务商回的原话的，不是给人看的。**
            // 智谱那句 429 里就带着「余额」，翻了就再也对不上，而且不报错
            // ——人看到的会是一句"等一会儿再试"，等一件永远不会好的事。
            detail.find(SAY_NEVER("余额")) != std::string::npos) {
            hint = SAY("这个模型要钱，而账上没余额（服务回的是 429 / 1113）。"
                       "换一个免费模型（智谱这边是 glm-4.7-flash），"
                       "或者去服务商那边充值。改在项目页那个模型窗口里。");
            // **手上有 GLM Coding Plan 订阅的人会撞在这儿，而且想不明白。**
            // 那份额度只认智谱登记在册的编程工具（Claude Code、Cline、
            // Cursor 那些），官方原话是"在除规定工具外调用 API，不可享用
            // Coding 套餐的额度"——我们这个程序不在册，于是同一把密钥
            // 调过来要么报余额不足，要么**直接去扣按量余额**。
            // 不说这一句的话，用户会盯着一个明明还有额度的订阅反复怀疑
            // 是自己填错了。
            if (cfg.base_url.find("bigmodel.cn") != std::string::npos ||
                cfg.base_url.find("z.ai") != std::string::npos) {
                hint += SAY(
                    "\n⚠️ 有 GLM Coding Plan 订阅也一样：那份额度只认智谱"
                    "登记在册的编程工具，自己写的程序调不到，正是这个报错。"
                    "订阅之外另充一点按量余额，或者就用免费那个。");
            }
        } else {
            hint = SAY("大模型服务说请求太频繁了，等一会儿再试。"
                       "免费档限流很紧，隔十几秒再点一次多半就过了。");
        }
    } else if (status >= 500) {
        hint = SAYF("大模型服务自己出错了（%1）。"
                    "本地服务的话看一眼它的日志，云服务的话过一会儿再试。",
                    std::to_string(status));
    } else {
        hint = SAYF("大模型服务返回 %1。", std::to_string(status));
    }

    std::string out = hint;
    if (!cfg.model.empty()) out += SAYF("\n当前模型：%1", cfg.model);
    if (!detail.empty()) out += SAYF("\n服务说：%1", detail);
    return out;
}

std::vector<std::pair<std::string, std::string>> known_models(
    const std::string& base_url) {
    const bool zhipu = base_url.find("bigmodel.cn") != std::string::npos ||
                       base_url.find("z.ai") != std::string::npos;
    if (!zhipu) return {};
    // 顺序是**推荐顺序**，不是字母序：设置页换家时挑的就是头一个，
    // 而字母序头一个是 glm-4.5，谁也不该先看见它。改顺序前想一下这件事。
    return {
        // 「写作榜 XX.X」原来五处各写一遍，只差一个数——翻译要翻五遍。
        {"glm-4.7-flash",
         SAYF("免费 · 默认。限流很紧，成批写会慢；写作榜 %1", "47.8")},
        {"glm-5.3",
         SAYF("这一档最会写：写作榜 %1、套话 %2，八章几乎不降", "81.8", "7.09")},
        {"glm-5.3-flash", SAY("便宜档。⚠️ 写作榜上没测过，别照 5.3 的分想当然")},
        {"glm-5.2", SAYF("写作榜 %1", "77.9")},
        {"glm-5", SAYF("写作榜 %1", "70.9")},
        {"glm-4.7", SAYF("写作榜 %1", "66.0")},
        {"glm-4.6", SAYF("写作榜 %1", "57.3")},
        {"glm-4.5", SAYF("写作榜 %1", "55.5")},
    };
}

namespace {

/// 这一趟没走通时那句话。三处在用，措辞得一样。
///
/// **「连不上」和「连上了但没读完」是两回事，不能都说成连不上。**
///
/// 2026-09-16 实撞：拆分镜连砸三次，日志里写着「连不上大模型服务
/// （https://open.bigmodel.cn/…）」，而同一台机器 curl 那个地址秒回。
/// 照着这句话查了半小时网络、代理、重复进程，全是好的——真正的原因在
/// **第二行**：`Failed to read connection`，也就是连接建起来了、请求发出去
/// 了，读响应的时候断的。同一个服务几十分钟前写大纲还是好的，只有分镜这
/// 一步砸——那是这条流水线上最大的一个请求。
///
/// 两种说法指向两个完全不同的地方：「连不上」让人去查网络和地址，
/// 「读一半断了」让人去看请求是不是太大、超时够不够、服务端掐没掐。
/// 所以按 why 里的措辞分开说。
/// 「连上了但没读完」那一类：请求发出去了，断在读回来的路上。
bool broke_midway(const std::string& why) {
    // cpp-httplib 的错误字样：连接阶段是 Connection/Connect，读写阶段是
    // Read/Write，超时是 Timeout。只要不是"压根没连上"，就别说连不上。
    const auto has = [&why](const char* w) {
        return why.find(w) != std::string::npos;
    };
    return has("read") || has("Read") || has("write") || has("Write") ||
           has("timeout") || has("Timeout");
}

std::string connect_failed(const config::LLMConfig& cfg,
                           const std::string& why, bool resent = false) {
    if (broke_midway(why) && resent) {
        // 重发过一次还是断，说"调大超时"就指歪了：值得重发的那一趟本来就
        // 远没到超时（见 worth_resending）。
        return SAYF("和大模型服务连上了，但连着两趟都断在半路（%1）。\n%2"
                    "\n第一趟断了之后已经自动重发过一次。离超时还远就断，多半是"
                    "服务端或中间的代理掐掉了这条长连接：过一会儿再试；老是这样"
                    "的话换一个模型或服务。**不是网络不通**——网络不通的话下面"
                    "那行写的是连接失败。",
                    cfg.base_url, why);
    }
    if (broke_midway(why)) {
        return SAYF("和大模型服务连上了，但这一趟没走完（%1）。\n%2"
                    "\n请求发出去了、响应没读回来。多半是这一趟太大或太慢："
                    "把 [llm].timeout_s 调大，或者换一个上下文更长的模型；"
                    "服务端限流时也会这样。**不是网络不通**——网络不通的话"
                    "下面那行写的是连接失败。",
                    cfg.base_url, why);
    }
    // **这一支也要说清下一步。** 上面那一支 2026-09-16 debug 过一次，
    // 所以写全了；这一支一直只挂着 httplib 那句英文
    //（`Could not establish connection`），中文界面上就是一句生英文，
    // 而且没说该去看哪儿。2026-09-21 把假模型停掉发一句话时看见的。
    return SAYF("连不上大模型服务（%1）。\n%2"
                "\n请求根本没发出去。按这个顺序看：地址对不对（少一截 `/v1`、"
                "http 写成 https 都算）、那个服务起没起、要不要走代理。"
                "地址和密钥在设置里改。**不是超时**——超时的话上面那行写的是"
                "「连上了但这一趟没走完」。",
                cfg.base_url, why);
}

/// 这一趟断在半路，值不值得**原样再发一趟**（只一趟）。
///
/// 「只发一次」是这个客户端的规矩（重复生成、重复计费），这是它唯一一处
/// 为传输层开的口子。2026-09-25 实测：智谱 glm-5.3 连着五次调用里断了四次，
/// 全是 `Failed to read connection`、全断在**思考**那一段（正文 0 字），
/// 离 900 秒的超时远得很（92 ~ 212 秒）。不重发的话，对话那一轮整个作废、
/// 一次补分镜白跑三分钟——而人接下来做的就是**原话再发一遍**，花的是同一份
/// 钱。能自动解决的就别报错。
///
/// 三道判据，缺一道都不重发：
///   1. **断在读的路上**（`broke_midway`）。压根没连上的不算——那是地址或
///      网络的事，重发只是让人多等一个超时（「连不上的时候不重试」那条用例）；
///   2. **正文、工具调用一个字都还没来**，由调用方判。来过的话界面上已经
///      出了半句，再发一趟会把同样的话再冒一遍；
///   3. **离超时还远**（不到 timeout_s 的一半）。真是等满了超时的，再等一个
///      超时也一样。
bool worth_resending(const std::optional<std::string>& transport_error,
                     double secs, double timeout_s) {
    return transport_error.has_value() && broke_midway(*transport_error) &&
           secs < timeout_s * 0.5;
}

/// 重发之前往思考那一栏里说一句。**不说的话**，那一栏的字会在半截上
/// 从头再想一遍，看着像模型犯了糊涂。
std::string resend_notice() {
    return "\n\n" + SAY("（和大模型服务的连接断在半路，已经原样重发一次。）") + "\n\n";
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// ---- RemoteClient ----

RemoteClient::RemoteClient(ConfigProvider cfg, HttpPost post,
                           HttpPostStream stream_post)
    : cfg_(std::move(cfg)),
      post_(std::move(post)),
      stream_post_(std::move(stream_post)) {}

RemoteClient::RemoteClient(config::LLMConfig cfg, HttpPost post,
                           HttpPostStream stream_post)
    : cfg_([cfg] { return cfg; }),
      post_(std::move(post)),
      stream_post_(std::move(stream_post)) {}

std::string RemoteClient::complete(const Request& req,
                                   pipeline::CancelToken& tok,
                                   const OnToken& on_token) {
    // 没人要逐字、也没人要看思考，或者没注入流式发送函数，就走整段那条。
    // 基类那个默认实现会把整段回调一次，形状是一样的。
    if ((!on_token && !req.on_thinking) || !stream_post_) {
        return Client::complete(req, tok, on_token);
    }

    // ⚠️ **记录器摆在上面那句转手之后，不能摆到它前面。** 摆前面的话，
    // 转手出去的那一次会在账上出现两行——这儿一行、整段那条再一行，
    // 而这儿那行的 ms 是另一行的外壳，统计出来的耗时凭空翻倍。
    //
    // cfg_() 也提到取消检查之前：它只是取一份配置快照，没有副作用。这么排
    // 是为了让**「已取消」也记得下来**——取消是一次调用真实的结束方式，
    // 漏掉的话账上会凭空少一次。
    config::LLMConfig cfg = cfg_();
    // 构造整个包一层，见 open_call_log 上那段「构造这一下抛了，也不许把这一次
    // 生成带走」。
    auto log_box = open_call_log("remote", "complete_stream", req, cfg);
    CallLog& log = *log_box;
    try {
        if (tok.cancelled()) throw LlmError(util::kCancelled);

        // **按任务分流。** 哪一步用哪个模型由 [llm.models] 定，见
        // config::LLMConfig::task_models：写正文那几步走文采好的，分镜人物
        // 那几步走听话的。认不出的任务落到 cfg.model。
        cfg.model = cfg.model_for(req.schema_name);
        // **温度也按任务分。** 编东西那几步要发散，拆分镜那几步要听话，
        // 见 LLMConfig::temperature_for。放在这儿而不是 build_payload 里：
        // 那个函数被一条「和 Python 逐字段一样」的语料钉着，加工混进去就分不清
        // 差异是谁造成的——和 model_for 当初放在这儿是同一个理由。
        // req 里自己填了温度的（比如写正文那步）照样盖过它，见 build_payload。
        cfg.temperature = cfg.temperature_for(req.schema_name);
        // **只记地址，整个请求头一个字节都不记**——密钥在 Authorization 里。
        log.set_endpoint(cfg.base_url);
        // ⚠️ **分流之后那三样记在下面 run() 里**，payload 拼好之后——理由见
        // 那儿那段「记的是 payload 里真有的那一份」。挪回这儿就又错了。
        const std::string url = cfg.base_url + "/chat/completions";
        const std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + cfg.api_key},
            {"Content-Type", "application/json"},
            // 有的网关看这个头决定要不要给你加缓冲。加了缓冲就等于没有流式。
            {"Accept", "text/event-stream"},
        };

        // 跑一趟 SSE。回来的是「拿到了多少正文 / 出了什么事」。
        struct Attempt {
            std::string text;
            std::string sse_error;   ///< 服务端在流里塞的 error
            std::string response_body;
            std::string finish_reason;
            int status = 0;
            std::optional<std::string> transport_error;
            bool canceled = false;
            double secs = 0.0;       ///< 这一趟花了多久，见 worth_resending
        };
        const auto build = [&] {
            nlohmann::ordered_json payload = build_payload(cfg, req);
            payload["stream"] = true;
            return payload;
        };
        const auto run = [&](const nlohmann::ordered_json& payload) {
            Attempt a;
            // **记的是 payload 里真有的那一份**，不是 cfg / req 里那份。
            //
            // `reasoning_effort` 尤其不能照 `req` 抄：build_payload 只在模型名
            // 以 glm-5 打头时才真把它写进请求体（见那儿「只对认得这个参数的
            // 模型说」那段），而上游是**无条件**填的——storyboard_run.cpp 每次
            // 都写 high，落到兜底的 glm-4.7-flash 时请求体里一个
            // reasoning_effort 都没有，账上却写着 "high"。用户按这一栏分两桶
            // 比分镜质量，两桶发的其实是同一份请求，差异全是噪声——而这正是
            // 这份日志存在的目的。docs/提示词日志.md 上那句「真发出去才有值」
            // 说的也是这件事。
            //
            // model 和 temperature 一并从 payload 取：它俩本来就对得上
            // （分流、req 覆盖都在 build_payload 之前算完），从同一处取只是
            // 不留下第二个"可能对不上"的口子。
            log.set_model(payload.value("model", std::string()),
                          payload.value("temperature", 0.0),
                          payload.value("reasoning_effort", std::string()));
            // 记的是**真发出去的那一份**，不是照着 cfg / req 再拼一遍。
            // build_payload 是纯函数不假，可多跑一遍就多出一处"日志里那份和
            // 真发出去的那份可能不一样"的风险，而这次要研究的正是那段字。
            log.set_prompt_from_payload(payload);

            SseDeltas sse;
            const auto t0 = std::chrono::steady_clock::now();
            HttpResponse r = stream_post_(
                url, payload.dump(), headers, cfg.timeout_s,
                [&](const char* data, std::size_t len) {
                    if (tok.cancelled()) {
                        a.canceled = true;
                        return false;   // 断掉，别让它继续生成
                    }
                    const std::string piece = sse.feed(data, len);
                    // **思考先推，正文后推。** 顺序反了的话，界面上会先冒出
                    // 第一句正文、再冒出"它正在想"，看着像倒放。
                    //
                    // **抽思考这一下无条件做。** 原来整段包在
                    // `if (req.on_thinking)` 里，没人盯着的那些批处理跑出来的
                    // 思考被直接丢掉——而研究最想看的正是那些。
                    // take_thinking() 本来每块都在调，改的只是"结果给谁"。
                    //
                    // ⚠️ **这个回调里一个字节都不许落盘。** 它每来一小段就跑
                    // 一次、跑在收流线程上，在这儿开文件等于把生成拖慢。下面
                    // 那两句 log.* 一个是往内存里接、一个是看一眼表，都是 O(1)；
                    // 真正的落盘在 log 析构那一下，也就是这一趟结束之后。
                    const std::string think = sse.take_thinking();
                    if (!think.empty()) {
                        if (req.on_thinking) req.on_thinking(think);
                        log.append_thinking(think);
                    }
                    if (piece.empty()) return true;
                    log.mark_first_token();
                    a.text += piece;
                    if (on_token) on_token(piece);
                    return true;
                });
            a.secs = seconds_since(t0);
            a.status = r.status;
            a.response_body = r.body;
            a.transport_error = r.transport_error;
            a.sse_error = sse.error();
            a.finish_reason = sse.finish_reason();
            // **服务端没理会 stream 的情况**：它回了一份普通的 JSON，SSE 解不
            // 出任何东西。那份 body 在 r.body 里（流式那条只在出错时收 body，
            // 但"整份 JSON"和"错误体"在传输上没区别），试着按整段解一次。
            if (a.text.empty() && a.status < 400 && !r.body.empty()) {
                try {
                    a.finish_reason = completion_reason(r.body);
                    // **思考无条件抽一次**，和另外两条路一个写法：网关收下
                    // stream 却回一份普通 JSON 时，正文能从 r.body 里救回来，
                    // 思考原来是整段丢掉的（thinking.txt 不写、thinking_chars
                    // 是 0）。抽在 extract_content 之前：那一句会抛，抛了思考
                    // 就白捞了。
                    //
                    // 用 append 不用 set：这条路上思考是一段一段接进来的，
                    // set 会把已经接到的那几段抹掉。
                    const std::string think = extract_thinking(r.body);
                    if (!think.empty()) {
                        if (req.on_thinking) req.on_thinking(think);
                        log.append_thinking(think);
                    }
                    a.text = extract_content(r.body);
                    if (on_token && !a.text.empty()) on_token(a.text);
                } catch (const std::exception&) {
                    // 解不出来就由下面按本次响应直接报错；不重复发送同一个请求。
                }
            }
            return a;
        };

        // **只发一次。** 不支持 SSE 但直接返回普通 OpenAI JSON 的服务会在
        // run() 里就地解析同一份响应；空响应和错误响应都不会自动再发一次，
        // 避免重复生成和重复计费。
        //
        // 例外有两个：「关掉思考」被拒（`learn_thinking_always_on`）：那是
        // 400，请求根本没被接下，换 low 再发一趟不算重复；另一个是断在半路、
        // 正文一个字还没来，见 worth_resending。
        nlohmann::ordered_json payload = build();
        Attempt a = run(payload);
        if (learn_thinking_always_on(payload, a.status, a.response_body,
                                     cfg.base_url, cfg.model)) {
            payload = build();
            a = run(payload);
        }
        bool resent = false;
        if (!a.canceled && a.text.empty() &&
            worth_resending(a.transport_error, a.secs, cfg.timeout_s)) {
            const std::string notice = resend_notice();
            if (req.on_thinking) req.on_thinking(notice);
            log.append_thinking(notice);
            resent = true;
            a = run(payload);
        }
        log.note_response(a.status, a.response_body);
        log.set_finish_reason(a.finish_reason);
        // ⚠️ **正文赶在下面那几条 throw 之前交出去。**「服务在流里报错，而
        // 前面已经吐过半份正文」正是要研究的情形（下面那段注释说的就是它）。
        // 留到最后一句才交的话，最想看的那几次 reply.txt 恰好是空的。
        log.set_reply(a.text);
        if (a.canceled) throw LlmError(util::kCancelled);
        if (a.transport_error.has_value()) {
            throw LlmError(connect_failed(cfg, *a.transport_error, resent));
        }
        if (a.status >= 400) {
            throw LlmError(explain_status(cfg, a.status, a.response_body), a.status);
        }

        // 服务在流里报错时，即使前面已经吐过半份正文也必须报真实错误。
        // 把半份正文交给解析器，只会把它伪装成“JSON 格式错误”。
        if (!a.sse_error.empty()) {
            throw LlmError(SAYF("大模型服务报错：%1", a.sse_error));
        }
        require_complete_reason(a.finish_reason);
        if (a.text.empty()) {
            throw LlmError(SAY("大模型服务没有返回正文；本次请求不会自动重发，"
                            "以免重复生成或重复计费"));
        }
        return checked_output(req, std::move(a.text));
    } catch (const LlmError& e) {
        // ⚠️ **裸 `throw;`，异常对象原样往外走。** 写成 `throw e;` 会切片成
        // std::exception，LlmError::status() / is_config_error() 当场失效——
        // 批量那几条靠 is_config_error 止损（见 client.hpp 上那段
        //「一次补七章分镜，第三章开始密钥失效，又试了五章」）。
        log.fail(e.what(), e.status());
        throw;
    } catch (const std::exception& e) {
        log.fail(e.what(), 0);
        throw;
    }
}

std::string Client::complete(const Request& req, pipeline::CancelToken& tok,
                             const OnToken& on_token) {
    // 拿不到逐字的后端就整段给一次。**不是不回调**——调用方按"回调拼出来
    // 的就是全文"写，少这一下的话流式那条路上什么都收不到，而它不会报错，
    // 只是编辑器里一直空着。
    std::string out = complete(req, tok);
    if (on_token && !out.empty()) on_token(out);
    return out;
}

std::string RemoteClient::complete(const Request& req,
                                   pipeline::CancelToken& tok) {
    // ⚠️ **有人要看思考，就走流式那条，哪怕没人要逐字。**
    //
    // 这一条**只能写在这儿**：不带 `on_token` 的调用方（改编成剧本、拆
    // 分镜、定妆）走的就是这个两参重载，根本到不了上面那个三参的分流点。
    // 我 2026-09-17 第一次就改错了地方，用例当场红。
    //
    // 为什么非流式不行：整段那条只能在**收完之后**把整段思考一次性给出去
    // （见下面那段）。而「改编成剧本」实测跑 11 分半——这 11 分半里任务
    // 页面上那一行一个字都没有，跑完那一下才蹦出 19 万字。用户
    // 2026-09-17：「思考的内容还是看不到」。**思考的用处全在跑的过程里**，
    // 它是唯一能回答"它还活着吗、在想什么"的东西，跑完再给等于没给。
    //
    // 走流式不多花什么：除了 `stream: true`，发的是同一份；服务端不理会
    // stream 的情况那条路上本来就有退路。
    if (stream_post_ && req.on_thinking) return complete(req, tok, OnToken{});

    // ⚠️ **记录器摆在上面那句转手之后，不能摆到它前面。** 摆前面的话，
    // 转手出去的那一次会在账上出现两行——这儿一行、SSE 那条再一行，
    // 而这儿那行的 ms 是另一行的外壳，统计出来的耗时凭空翻倍。
    //
    // cfg_() 也提到取消检查之前：它只是取一份配置快照，没有副作用。这么排
    // 是为了让**「已取消」也记得下来**——取消是一次调用真实的结束方式，
    // 漏掉的话账上会凭空少一次。
    //
    // 每次取一份当前配置。中途 /api/connections 换了地址的话，
    // 下一次调用就走新地址——这正是那个接口的意义。
    config::LLMConfig cfg = cfg_();
    // 构造整个包一层，见 open_call_log 上那段「构造这一下抛了，也不许把这一次
    // 生成带走」。
    auto log_box = open_call_log("remote", "complete", req, cfg);
    CallLog& log = *log_box;
    try {
        if (tok.cancelled()) throw LlmError(util::kCancelled);

        // **按任务分流。** 哪一步用哪个模型由 [llm.models] 定，见
        // config::LLMConfig::task_models：写正文那几步走文采好的，分镜人物
        // 那几步走听话的。认不出的任务落到 cfg.model。
        cfg.model = cfg.model_for(req.schema_name);
        // **温度也按任务分。** 编东西那几步要发散，拆分镜那几步要听话，
        // 见 LLMConfig::temperature_for。放在这儿而不是 build_payload 里：
        // 那个函数被一条「和 Python 逐字段一样」的语料钉着，加工混进去就分不清
        // 差异是谁造成的——和 model_for 当初放在这儿是同一个理由。
        // req 里自己填了温度的（比如写正文那步）照样盖过它，见 build_payload。
        cfg.temperature = cfg.temperature_for(req.schema_name);
        // **只记地址，整个请求头一个字节都不记**——密钥在 Authorization 里。
        log.set_endpoint(cfg.base_url);
        // ⚠️ **分流之后那三样记在下面**，payload 拼好之后——理由见
        // complete_stream 里那段「记的是 payload 里真有的那一份」。
        const std::string url = cfg.base_url + "/chat/completions";
        const std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + cfg.api_key},
            {"Content-Type", "application/json"},
        };

        // **只发一次。** 退档梯子随 response_format 一起删了，理由见
        // build_payload 里那段。
        // 例外两种：「关掉思考」被拒、断在半路，见 complete_stream 里那段。
        const auto send = [&](const nlohmann::ordered_json& payload) {
            // 分流之后那三样，从 payload 里取，**所以 set_model 在
            // build_payload 后面**，不能挪回前面去。理由见 complete_stream
            // 里那段「记的是 payload 里真有的那一份」。
            log.set_model(payload.value("model", std::string()),
                          payload.value("temperature", 0.0),
                          payload.value("reasoning_effort", std::string()));
            // 记的是**真发出去的那一份**，不是照着 cfg / req 再拼一遍。
            // build_payload 是纯函数不假，可多跑一遍就多出一处"日志里那份和
            // 真发出去的那份可能不一样"的风险，而这次要研究的正是那段字。
            log.set_prompt_from_payload(payload);
            return post_(url, payload.dump(), headers, cfg.timeout_s);
        };
        nlohmann::ordered_json payload = build_payload(cfg, req);
        auto t0 = std::chrono::steady_clock::now();
        HttpResponse r = send(payload);
        if (learn_thinking_always_on(payload, r.status, r.body, cfg.base_url,
                                     cfg.model)) {
            payload = build_payload(cfg, req);
            t0 = std::chrono::steady_clock::now();
            r = send(payload);
        }
        // 断在半路那一种原样再发一趟，见 worth_resending。整段这条一个字都
        // 没交出去过，"正文还没来"天然成立。
        bool resent = false;
        if (!tok.cancelled() &&
            worth_resending(r.transport_error, seconds_since(t0), cfg.timeout_s)) {
            resent = true;
            r = send(payload);
        }
        // **拿到回包就立刻记，放在下面那句 transport_error 之前。** 连不上
        // 时 r.status 是 0、r.body 是空，记下来无害；放到后面的话 400 那一支
        // 就整个漏了，而"什么提示词换来一个 400"正是最想研究的。
        log.note_response(r.status, r.body);
        if (r.transport_error.has_value()) {
            throw LlmError(connect_failed(cfg, *r.transport_error, resent));
        }

        if (r.status >= 400) {
            throw LlmError(explain_status(cfg, r.status, r.body), r.status);
        }
        if (tok.cancelled()) throw LlmError(util::kCancelled);
        // **思考无条件抽一次。** 原来整段包在 `if (req.on_thinking)` 里，
        // 没人盯着的那些批处理跑出来的思考一个字都留不下——而研究要的正是
        // 那些。多的开销只是把一份已经在手里的 JSON 再解一次，不多跑一趟网络。
        const std::string think = extract_thinking(r.body);
        if (req.on_thinking && !think.empty()) req.on_thinking(think);
        log.set_thinking(think);
        // 拆成两句：先算出来、记上、再校验。**require_complete_reason 会抛**
        // （截断、内容过滤都从这儿抛），抛之前得先把 finish_reason 记上，
        // 不然账上最该看的那几行这一栏恰好是空的。
        const std::string reason = completion_reason(r.body);
        log.set_finish_reason(reason);
        require_complete_reason(reason);
        // ⚠️ **抽出来的正文要赶在 checked_output 之前交给记录器**，否则
        //「schema 本地校验没过」那一次 reply.txt 是空的——而那正是最想看的
        // 一次（对面好好地回了 200，是我们这头没认）。
        std::string raw = extract_content(r.body);
        log.set_reply(raw);
        return checked_output(req, std::move(raw));
    } catch (const LlmError& e) {
        // ⚠️ **裸 `throw;`，异常对象原样往外走。** 写成 `throw e;` 会切片成
        // std::exception，LlmError::status() / is_config_error() 当场失效——
        // 批量那几条靠 is_config_error 止损（见 client.hpp 上那段
        //「一次补七章分镜，第三章开始密钥失效，又试了五章」）。
        log.fail(e.what(), e.status());
        throw;
    } catch (const std::exception& e) {
        log.fail(e.what(), 0);
        throw;
    }
}

// ---- ReplayClient ----

namespace {

ordered message_json(const Message& m) {
    ordered j = ordered::object();
    j["role"] = m.role;
    if (m.role == "assistant" && !m.tool_calls.empty()) {
        // OpenAI 那套：带 tool_calls 的那条 content 可以是 null
        if (m.content.empty()) j["content"] = nullptr;
        else j["content"] = m.content;
        ordered calls = ordered::array();
        for (const auto& c : m.tool_calls) {
            calls.push_back(ordered{{"id", c.id},
                                    {"type", "function"},
                                    {"function", ordered{{"name", c.name},
                                                         {"arguments", c.arguments}}}});
        }
        j["tool_calls"] = calls;
    } else {
        j["content"] = m.content;
    }
    if (m.role == "tool") j["tool_call_id"] = m.tool_call_id;
    return j;
}

ChatReply parse_chat_reply(const std::string& raw_body) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(raw_body);
    } catch (const std::exception&) {
        throw LlmError(SAYF("大模型返回的不是 JSON：%1",
                            text::truncate_utf8(raw_body, 400)));
    }
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty()) {
        throw LlmError(SAYF("大模型返回格式异常：%1",
                            text::truncate_utf8(body.dump(), 400)));
    }
    const auto& first = (*choices)[0];
    const auto msg = first.find("message");
    if (msg == first.end() || !msg->is_object()) {
        throw LlmError(SAYF("大模型返回格式异常：%1",
                            text::truncate_utf8(body.dump(), 400)));
    }
    ChatReply r;
    if (const auto c = msg->find("content"); c != msg->end() && c->is_string()) {
        r.content = c->get<std::string>();
    }
    if (const auto fr = first.find("finish_reason"); fr != first.end() && fr->is_string()) {
        r.finish_reason = fr->get<std::string>();
    }
    if (const auto tc = msg->find("tool_calls"); tc != msg->end() && tc->is_array()) {
        for (const auto& item : *tc) {
            if (!item.is_object()) continue;
            ToolCall call;
            call.id = item.value("id", std::string());
            const auto fn = item.find("function");
            if (fn != item.end() && fn->is_object()) {
                call.name = fn->value("name", std::string());
                const auto args = fn->find("arguments");
                if (args != fn->end()) {
                    call.arguments = args->is_string() ? args->get<std::string>() : args->dump();
                }
            }
            if (!call.name.empty()) r.tool_calls.push_back(std::move(call));
        }
    }
    return r;
}

}  // namespace

ChatReply Client::chat(const std::vector<Message>&, const ordered&, const Request&,
                       pipeline::CancelToken&) {
    throw LlmError(SAY("这个大模型后端不带工具调用"));
}

ChatReply RemoteClient::chat(const std::vector<Message>& messages, const ordered& tools,
                             const Request& opts, pipeline::CancelToken& tok) {
    // 这一条不转手，记录器直接摆在 cfg_() 之后。cfg_() 提到取消检查之前：
    // 它只是取一份配置快照，没有副作用，这么排是为了让**「已取消」也记得
    // 下来**——取消是一次调用真实的结束方式，漏掉的话账上会凭空少一次。
    config::LLMConfig cfg = cfg_();
    // **有人要看思考、或者要逐字，就走 SSE。**
    //
    // 理由和上面两条 complete 一样，只是这条路上"看不见"更重：一趟对话
    // 十轮起，整段 POST 的话每一轮的思考要等这一轮收完才一次性给，而正文
    // 压根没有出口。2026-09-19 用户报的就是它——右下角那颗按下去，编辑器
    // 一百多秒一个字不动，末了整章一次性蹦出来。
    const bool live = stream_post_ && (opts.on_thinking || opts.on_token);
    // 构造整个包一层，见 open_call_log 上那段「构造这一下抛了，也不许把这一次
    // 生成带走」。**账上两条路分得开**，和 complete / complete_stream 一样。
    auto log_box = open_call_log("remote", live ? "chat_stream" : "chat", opts, cfg);
    CallLog& log = *log_box;
    try {
        if (tok.cancelled()) throw LlmError(util::kCancelled);

        cfg.model = cfg.model_for(opts.schema_name);
        cfg.temperature = cfg.temperature_for(opts.schema_name);
        // **只记地址，整个请求头一个字节都不记**——密钥在 Authorization 里。
        log.set_endpoint(cfg.base_url);
        // ⚠️ **分流之后那三样记在下面**，payload 拼好之后——理由见
        // complete_stream 里那段「记的是 payload 里真有的那一份」。
        const std::string url = cfg.base_url + "/chat/completions";
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + cfg.api_key},
            {"Content-Type", "application/json"},
        };
        // 有的网关看这个头决定要不要给你加缓冲。加了缓冲就等于没有流式。
        if (live) headers["Accept"] = "text/event-stream";

        // 和 build_payload 同一套：模型、温度、GLM-5 的 thinking。多的只是
        // messages 是整段来回、外加 tools。
        ordered payload = ordered::object();
        payload["model"] = cfg.model;
        ordered msgs = ordered::array();
        for (const auto& m : messages) msgs.push_back(message_json(m));
        payload["messages"] = msgs;
        payload["temperature"] = opts.temperature.value_or(cfg.temperature);
        if (tools.is_array() && !tools.empty()) {
            payload["tools"] = tools;
            payload["tool_choice"] = "auto";
        }
        // 同 build_payload：查同一张表（`llm/thinking.hpp`）。
        apply_thinking(payload, cfg.base_url, cfg.model, opts.reasoning_effort);
        if (live) payload["stream"] = true;
        // 分流之后那三样，从 payload 里取，**所以这一句在 payload 拼完之后**。
        // 认不出的模型一个 reasoning_effort 都不发（那张表管着），
        // 账上就不该写着有。
        const auto note_payload = [&] {
            log.set_model(payload.value("model", std::string()),
                          payload.value("temperature", 0.0),
                          payload.value("reasoning_effort", std::string()));
            // 记的是**真发出去的那一份**（下一句 dump 的就是它），整段来回
            // 渲染成 `=== role ===` 那个形状。
            log.set_prompt_from_payload(payload);
        };
        note_payload();
        // 工具表另存一份。
        log.set_tools(tools);
        // 「关掉思考」被拒：记下来，把那一档重填一遍（这回填的是 low），
        // 回 true 让下面再发一趟。理由见 complete_stream 里那段。
        const auto thinking_refused = [&](const HttpResponse& r) {
            if (!learn_thinking_always_on(payload, r.status, r.body, cfg.base_url,
                                          cfg.model)) {
                return false;
            }
            payload.erase("thinking");
            payload.erase("reasoning_effort");
            apply_thinking(payload, cfg.base_url, cfg.model, opts.reasoning_effort);
            note_payload();
            return true;
        };

        if (!live) {
            auto t0 = std::chrono::steady_clock::now();
            HttpResponse r = post_(url, payload.dump(), headers, cfg.timeout_s);
            if (thinking_refused(r)) {
                t0 = std::chrono::steady_clock::now();
                r = post_(url, payload.dump(), headers, cfg.timeout_s);
            }
            // 断在半路原样再发一趟，见 worth_resending。
            bool resent = false;
            if (!tok.cancelled() &&
                worth_resending(r.transport_error, seconds_since(t0), cfg.timeout_s)) {
                resent = true;
                r = post_(url, payload.dump(), headers, cfg.timeout_s);
            }
            // 拿到回包就立刻记，理由同整段那条：放到下面去就把 400 那一支漏了。
            log.note_response(r.status, r.body);
            if (r.transport_error.has_value()) {
                throw LlmError(connect_failed(cfg, *r.transport_error, resent));
            }
            if (r.status >= 400) {
                throw LlmError(explain_status(cfg, r.status, r.body), r.status);
            }
            if (tok.cancelled()) throw LlmError(util::kCancelled);
            // **思考无条件抽一次**，理由同整段那条：研究要的正是没人盯着的那些。
            const std::string think = extract_thinking(r.body);
            if (opts.on_thinking && !think.empty()) opts.on_thinking(think);
            log.set_thinking(think);
            // ⚠️ **parse_chat_reply 抛的时候 reply.txt 会是空的，这是对的**，
            // 不是漏了：那一次「大模型返回的不是 JSON」的整份回包已经原样躺在
            // error.txt 的第二段里（上面 note_response 把 r.body 留住了）。
            //
            // 另外，模型这一轮改成调工具时 reply.content 常常是空串、
            // reply_chars 就是 0——也正常，它这一轮说的话在 tools_count
            // 和下一轮的 prompt 里。
            ChatReply reply = parse_chat_reply(r.body);
            log.set_finish_reason(reply.finish_reason);
            log.set_reply(reply.content);
            return reply;
        }

        // ---- 边收边给 ----
        SseDeltas sse;
        std::string content;
        bool canceled = false;
        const OnChunk on_chunk = [&](const char* data, std::size_t len) {
            if (tok.cancelled()) {
                canceled = true;
                return false;   // 断掉，别让它继续生成
            }
            const std::string piece = sse.feed(data, len);
            // **思考先推，正文后推**，理由同 complete_stream：反了的话
            // 界面上会先冒出第一句正文、再冒出"它正在想"，像倒放。
            // 落盘的规矩也一样：这个回调里一个字节都不许落盘。
            const std::string think = sse.take_thinking();
            if (!think.empty()) {
                if (opts.on_thinking) opts.on_thinking(think);
                log.append_thinking(think);
            }
            if (piece.empty()) return true;
            log.mark_first_token();
            content += piece;
            if (opts.on_token) opts.on_token(piece);
            return true;
        };
        auto t0 = std::chrono::steady_clock::now();
        HttpResponse r =
            stream_post_(url, payload.dump(), headers, cfg.timeout_s, on_chunk);
        // 400 时流里一个字都没来过，sse / content 还是空的，接着用就是。
        if (thinking_refused(r)) {
            t0 = std::chrono::steady_clock::now();
            r = stream_post_(url, payload.dump(), headers, cfg.timeout_s, on_chunk);
        }
        // 断在半路、正文和工具调用一个字都还没来：原样再发一趟，见
        // worth_resending。**sse 要换一个新的**——断掉那一趟可能留着半行没
        // 凑完的字节，接着喂的话第二趟的第一行会被拼坏。
        bool resent = false;
        if (!canceled && content.empty() && sse.tool_calls().empty() &&
            worth_resending(r.transport_error, seconds_since(t0), cfg.timeout_s)) {
            const std::string notice = resend_notice();
            if (opts.on_thinking) opts.on_thinking(notice);
            log.append_thinking(notice);
            sse = SseDeltas{};
            resent = true;
            r = stream_post_(url, payload.dump(), headers, cfg.timeout_s, on_chunk);
        }
        log.note_response(r.status, r.body);
        if (canceled) throw LlmError(util::kCancelled);
        if (r.transport_error.has_value()) {
            throw LlmError(connect_failed(cfg, *r.transport_error, resent));
        }
        if (r.status >= 400) {
            throw LlmError(explain_status(cfg, r.status, r.body), r.status);
        }
        // 服务在流里报错时，即使前面已经吐过半份话也必须报真实错误——
        // 理由同 complete_stream 那处。
        if (!sse.error().empty()) {
            throw LlmError(SAYF("大模型服务报错：%1", sse.error()));
        }
        // **服务端没理会 stream 的情况**：它回了一份普通的 JSON，SSE 解不出
        // 任何东西。那份 body 在 r.body 里，按整段解一次；**不为同一个请求
        // 再发一次**（重复生成、重复计费），和 complete_stream 同一条规矩。
        if (content.empty() && sse.tool_calls().empty() && !r.body.empty()) {
            const std::string think = extract_thinking(r.body);
            if (!think.empty()) {
                if (opts.on_thinking) opts.on_thinking(think);
                log.append_thinking(think);
            }
            ChatReply whole = parse_chat_reply(r.body);
            if (opts.on_token && !whole.content.empty()) opts.on_token(whole.content);
            log.set_finish_reason(whole.finish_reason);
            log.set_reply(whole.content);
            return whole;
        }
        ChatReply reply;
        reply.content = content;
        reply.finish_reason = sse.finish_reason();
        for (const auto& c : sse.tool_calls()) {
            // **没名字的那一格丢掉。** 攒的时候是按 index 开格子的，服务端
            // 跳号（只发了 0 和 2）就会在中间留一个空壳，照单交上去的话
            // 上层会去跑一个名字是空串的工具。
            if (c.name.empty()) continue;
            ToolCall out;
            out.id = c.id;
            out.name = c.name;
            out.arguments = c.arguments;
            reply.tool_calls.push_back(std::move(out));
        }
        log.set_finish_reason(reply.finish_reason);
        log.set_reply(reply.content);
        return reply;
    } catch (const LlmError& e) {
        // ⚠️ **裸 `throw;`，异常对象原样往外走**（理由见上面两条 complete）。
        log.fail(e.what(), e.status());
        throw;
    } catch (const std::exception& e) {
        log.fail(e.what(), 0);
        throw;
    }
}

ReplayClient::ReplayClient(std::vector<std::string> responses)
    : responses_(std::move(responses)) {}

/// ⚠️ **ReplayClient 的这两条一行日志都不接，这是有意的。**
///
/// 它只给用例和回放模式用。接上去的话，跑一次单元测试就往用户真正的数据
/// 目录里写一千多份提示词和回复——账上那些行还全是假的（回放录好的），
/// 混在真调用里只会把研究的那份数据弄脏。
ChatReply ReplayClient::chat(const std::vector<Message>& messages, const ordered&,
                            const Request& opts, pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError(util::kCancelled);
    Request seen = opts;
    seen.prompt = messages.empty() ? std::string() : messages.back().content;
    calls_.push_back(seen);
    last_messages_ = messages;
    if (next_ >= responses_.size()) {
        throw LlmError(SAYF("回放录到头了：这是第 %1 次调用，只录了 %2 条",
                            std::to_string(next_ + 1),
                            std::to_string(responses_.size())));
    }
    const std::string raw = responses_[next_++];
    ChatReply r;
    try {
        const nlohmann::json j = nlohmann::json::parse(raw);
        // 录的那条里带 `thinking` 就先把那段"想"出来，再照常往下走。
        //
        // **为的是让"接没接思考"这件事验得着。** 真模型那头思考走的是
        // `opts.on_thinking`，而一个调用点忘了给这个回调**一声不响**——
        // 思考整段落地，界面上那一行一动不动，看着就是卡死了
        // （2026-09-21 实撞：对话那条从头到尾没接）。
        if (j.is_object() && j.contains("thinking")) {
            if (opts.on_thinking) opts.on_thinking(j["thinking"].get<std::string>());
            // 带了 `thinking` 就说明这条是**结构化**的那种录法，说的那句话在
            // `content` 里。不这么分的话下面那句 `r.content = raw` 会把整段
            // JSON 当成模型说的话，界面上就是一行花括号。
            if (!j.contains("tool_calls")) {
                r.content = j.value("content", std::string());
                r.finish_reason = "stop";
                return r;
            }
        }
        if (j.is_object() && j.contains("tool_calls") && j["tool_calls"].is_array()) {
            int k = 0;
            for (const auto& item : j["tool_calls"]) {
                ToolCall c;
                c.id = item.value("id", "call_" + std::to_string(++k));
                c.name = item.value("name", std::string());
                const auto a = item.find("arguments");
                if (a != item.end()) c.arguments = a->is_string() ? a->get<std::string>() : a->dump();
                r.tool_calls.push_back(std::move(c));
            }
            // 真模型可以在同一条回话里先说一段、再调工具（OpenAI 那套的
            // `content` + `tool_calls`）。录的时候带着 `content` 就照样给，
            // 「先说再做」那道关（agent/loop.cpp）才验得着。
            if (const auto c = j.find("content"); c != j.end() && c->is_string()) {
                r.content = c->get<std::string>();
            }
            r.finish_reason = "tool_calls";
            return r;
        }
    } catch (const std::exception&) {
        // 不是 JSON：就是一段话
    }
    r.content = raw;
    r.finish_reason = "stop";
    return r;
}

/// 同上：**不接日志**，理由见 ReplayClient::chat 上方那段。
std::string ReplayClient::complete(const Request& req,
                                   pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError(util::kCancelled);
    calls_.push_back(req);
    if (next_ >= responses_.size()) {
        throw LlmError(SAYF("回放录到头了：这是第 %1 次调用，只录了 %2 条",
                            std::to_string(next_ + 1),
                            std::to_string(responses_.size())));
    }
    return responses_[next_++];
}

// ---- 造一个客户端 ----

namespace {

/// 每次调用都按**当前**配置挑一条后端。
///
/// **"选哪条后端"必须和 `ConfigProvider` 一样是每次现读的。** 这儿原来是在
/// 工厂里 `if` 一次、把选中的那条造出来返回，而调用点把它存进函数局部
/// `static`（`http/server.cpp` 的 `script_client` / `batch_client`）——于是
/// 那个 `if` 跟着冻在进程第一次叫模型那一刻。
///
/// 2026-09-18 实测：`POST /api/connections` 把 `llm_backend` 从 remote 改成
/// command，回执的 `changed` 里有它、`config.toml` 也真写进去了，紧接着那次
/// 调用落进日志的还是 `"backend":"remote"`，重启才变。**地址、模型、密钥换了
/// 都立刻生效**（那几样走 `ConfigProvider`），偏偏"走哪条后端"不生效，
/// 而界面上已经显示"已应用"了——错得最难查的那一种。
///
/// 两条后端造起来都不花钱（各自只揣一个 `ConfigProvider` 和两个
/// `std::function`），所以两条都先造好，每次调用只是挑一个。
/// 原来在调用点上写的理由是「每个请求选一次的话，local 那条每次都要重新借槽」
/// ——**那条路 2026-09-14 就删了**，理由跟着一起没了。
class SwitchingClient : public Client {
public:
    SwitchingClient(ConfigProvider cfg, HttpPost post, HttpPostStream stream_post)
        : cfg_(cfg),
          remote_(std::make_shared<RemoteClient>(cfg, std::move(post),
                                                 std::move(stream_post))),
          command_(std::make_shared<CommandClient>(cfg)),
          // 没编进 llama.cpp 的构建里是 nullptr，pick() 退回远端。
          local_(make_local_client(cfg)) {}

    std::string complete(const Request& req, pipeline::CancelToken& tok) override {
        return pick()->complete(req, tok);
    }

    std::string complete(const Request& req, pipeline::CancelToken& tok,
                         const OnToken& on_token) override {
        return pick()->complete(req, tok, on_token);
    }

    ChatReply chat(const std::vector<Message>& messages, const ordered& tools,
                   const Request& opts, pipeline::CancelToken& tok) override {
        return pick()->chat(messages, tools, opts, tok);
    }

private:
    /// 这一次该走哪条。**认的是 `backend` 这一个值**，认不出的一律当远端——
    /// 配置里写了个错别字时，宁可按默认那条跑通，也不要当场抛一句
    /// 「不认识的后端」把整条流水线堵死。填错了会在 `Settings::validate`
    /// 那头说。
    const std::shared_ptr<Client>& pick() const {
        const std::string backend = cfg_().backend;
        if (backend == "command") return command_;
        if (backend == "local") {
            if (local_) return local_;
            // **说一声，然后走远端。** 用户多半只是拿了个不带 llama 的构建，
            // 而远端那条只要地址填了就能用。只说一次，不然一部电影几百次。
            static const bool said = [] {
                std::fputs(
                    SAY_NEVER(
                        "[llm] 配的是 backend = \"local\"，但这个二进制没编进程内"
                        "大模型（构建时 CHANGJI_LLAMA=OFF）。"
                        "退回 [llm].base_url 那条。\n"),
                    stderr);
                return true;
            }();
            (void)said;
        }
        return remote_;
    }

    ConfigProvider cfg_;
    std::shared_ptr<Client> remote_;
    std::shared_ptr<Client> command_;
    std::shared_ptr<Client> local_;
};

}  // namespace

std::shared_ptr<Client> make_client(ConfigProvider cfg, HttpPost post,
                                    HttpPostStream stream_post) {
    // 三条后端：远端（打 API）、命令行（2026-09-18 加的，claude / codex）、
    // 进程内（2026-09-14 删、2026-09-19 接回来，理由见 llm/local_client.hpp）。
    // **在这儿分叉，不在每个调用点**——叫模型的地方散在六七个文件里，
    // 它们只认 `Client` 这个接口。
    //
    // 分派为什么收在 `SwitchingClient` 里而不是在这儿 `if` 一次：见它上面那段。
    return std::make_shared<SwitchingClient>(std::move(cfg), std::move(post),
                                             std::move(stream_post));
}

std::shared_ptr<Client> make_client(HttpPost post, HttpPostStream stream_post) {
    return make_client(ConfigProvider([] { return config::runtime().snapshot().llm; }),
                       std::move(post), std::move(stream_post));
}

}  // namespace changji::llm
