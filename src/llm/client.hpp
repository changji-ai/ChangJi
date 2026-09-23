#pragma once

// 大模型客户端。
//
// 远端兼容 OpenAI 接口的服务（Ollama、云端）。对应 [llm] 配置段。
// Replay 后端只给测试和回放模式用。文本生成已经没有进程内 Local 后端；
// llama.cpp 仍用于进程内配音。
//
// ---
//
// HTTP 是**注入**的，这个文件不 include httplib。
// 一是分层，二是很实际：请求怎么拼、错误怎么翻成人话、返回怎么抽内容，
// 这三件事全是纯逻辑，能测死；混进真实网络之后就只能靠手工验了。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "pipeline/jobs.hpp"

namespace changji::llm {

/// 调不通大模型时抛这个。消息是给用户看的，不是给开发看的。
class LlmError : public std::runtime_error {
public:
    explicit LlmError(const std::string& what) : std::runtime_error(what) {}
    LlmError(const std::string& what, int http_status)
        : std::runtime_error(what), status_(http_status) {}

    /// 对面回的 HTTP 状态码。0 = 不是因为状态码抛的（解析失败、被取消…）。
    int status() const { return status_; }

    /// **这个错换一件活重来也一样。**
    ///
    /// 判据是"配的东西不对"，不是"这一次不巧"：密钥不对（401）、这把密钥
    /// 没有这个权限（403）、地址或模型名不存在（404）。
    ///
    /// ⚠️ **批量那几条要看它。** 2026-09-17 实撞：一次补七章分镜，第三章
    /// 开始密钥失效，而「一章砸了不拖垮整批」这条规矩让它**又试了五章，
    /// 每章同一个 401**——人白等二十分钟，回来看到五条一模一样的报错。
    /// 「一章砸了不拖垮整批」针对的是内容问题（这一章模型没写好），
    /// 而配错了的东西不会在下一章自己变好。
    ///
    /// 429（限流）和 5xx **不算**：那两种换个时间真会好，接着跑是对的。
    bool is_config_error() const {
        return status_ == 401 || status_ == 403 || status_ == 404;
    }

private:
    int status_ = 0;
};

/// 一次请求。
struct Request {
    std::string prompt;
    /// 结构化输出会在收到完整正文后按 schema 做本地校验；不符合时抛 LlmError。
    nlohmann::ordered_json schema;
    /// schema 的任务名，用于模型和温度分流。
    std::string schema_name;

    /// 这一步专用的温度。**不填就用 `[llm].temperature`**，也就是用户在
    /// 设置页上定的那个全局值。
    ///
    /// ⚠️ **做成 optional 而不是给个默认数。** 原来是 `double = 0.7`，
    /// 而远端那条路 build_payload 根本没读它（发的一直是 cfg.temperature）——
    /// 于是 kChapterTemperature 那个"写正文用 0.5"在默认后端上空转了很久，
    /// 谁也没发现，因为两条路都返回 200。接上线之后要是还留着默认数，
    /// 病就换个方向犯：**任何一个忘了填的调用点都会悄悄盖掉用户的设置**，
    /// 他把温度调到 0.3，写分镜那一步照样跑 0.7，而界面上显示的是 0.3。
    /// 空的才是"没意见"，有值才是"这一步我有意见"。
    std::optional<double> temperature;

    /// 模型"先想再写"的那一段，每收到一点回调一次。
    ///
    /// **和正文分两路，这是有意的。** 现在的模型都要思考，而各家都把思考
    /// 放在单独的字段里（智谱 reasoning_content、OpenRouter reasoning）。
    /// 并进正文的话，思考稿会直接流进用户的编辑器。
    ///
    /// 放在 Request 上而不是给 complete 多加一个参数：这条流是**每一步**
    /// 都要有的（写大纲、写正文、写剧本、拆分镜），而那些调用点散在六七个
    /// 文件里。放在这儿的话，有 stream_id 的调用点加一行就接上了，
    /// 没有的（对拍、回放、后台批处理）留空什么都不发生。
    ///
    /// ⚠️ 回调跑在**收流那条线程**上，每来一小段就一次。别在里面做慢活，
    /// 也别在里面碰界面状态以外的东西。
    std::function<void(const std::string& piece)> on_thinking;

    /// 正文那一段，每收到一点回调一次。给的是**增量**。
    ///
    /// **只有 `chat()` 那条路读它。** `complete()` 的逐字回调是函数参数
    /// （`OnToken`），因为那条路上"要不要逐字"是每个调用点自己的事；而
    /// `chat()` 的签名里 `opts` 就是那一袋子选项，再加一个参数的话，
    /// 四个实现和一串用例都要跟着改，而其中三个根本不流式。
    ///
    /// 2026-09-19 加的：右下角那颗「上网看热点写这一章」走的是 chat()，
    /// 而那条路整段 POST——一百多秒里编辑器一个字都不动，思考也要等这一轮
    /// 结束才一次性蹦出来。用户报的就是它。
    ///
    /// ⚠️ 回调跑在**收流那条线程**上，注意事项同 `on_thinking`。
    std::function<void(const std::string& piece)> on_token;

    /// 让模型少想一点。空串 = 不提意见，按服务端默认来（**今天就是这样**）。
    ///
    /// **为什么要有这一项。** 2026-09-17 把拆分镜那一步的思考捞出来读了：
    /// 它在思考里**把整张分镜表逐镜写完了**——Shot 11 的 first_frame、
    /// dialogue、motion、characters 全是成品——然后才用 JSON 再写一遍。
    /// 同一份东西写两遍，而这一步是全流水线最贵的（一章七八分钟，采样下来
    /// 思考每秒涨约 190 字，一场跑了 410 秒还一个字正文都没落）。
    ///
    /// 智谱的文档写着 `reasoning_effort` **默认就是 max**，GLM-5.3 / 5.3-Flash
    /// 支持 low / high / max。所以这不是猜的旋钮，是我们一直没说话而已。
    ///
    /// ⚠️ **只发给认得它的模型**（见 build_payload 里那段）：这是智谱家的
    /// 参数，别家收到不认识的字段可能直接 400，而那会让**所有**生成一起挂。
    std::string reasoning_effort;
};


/// HTTP 响应。刻意只留用得上的这几样。
struct HttpResponse {
    int status = 0;
    std::string body;
    /// 传输层就失败了（连不上、超时），这时 status 是 0。
    std::optional<std::string> transport_error;
    /// 响应头，**键一律小写**（HTTP 头不分大小写，各家服务写法不一）。
    /// 2026-09-24 为远程 MCP 加的：会话号 `mcp-session-id` 在响应头里，
    /// 拿不到就续不上那一次会话。
    std::map<std::string, std::string> headers;
};

/// 发一次 POST。由调用方注入。
using HttpPost = std::function<HttpResponse(
    const std::string& url, const std::string& body,
    const std::map<std::string, std::string>& headers, double timeout_s)>;

/// 响应体到了一段。返回 false = 别发了（取消），传输层会断开连接。
using OnChunk = std::function<bool(const char* data, std::size_t len)>;

/// 发一次 POST，**响应边到边给**。同样由调用方注入。
///
/// 和 HttpPost 分成两个而不是加个参数：整段那条有一堆地方在用（TTS、
/// 模型列表、doctor），给它们全加一个用不上的参数不值当。
///
/// 回来的 HttpResponse 里 body 只在**出错时**有东西（状态码 >= 400 的
/// 那份错误体，要拿它翻译成人话）；正常那条的内容已经从 on_chunk 走了。
using HttpPostStream = std::function<HttpResponse(
    const std::string& url, const std::string& body,
    const std::map<std::string, std::string>& headers, double timeout_s,
    const OnChunk& on_chunk)>;

/// 生成到一段文字时回调一次。给的是**增量**，不是累计。
///
/// 增量而不是累计：一段几千字的正文，回调几百次、每次带全文的话，
/// 光是这些字符串拷贝就比生成本身还贵。
using OnToken = std::function<void(const std::string& piece)>;

/// 客户端接口。
/// GET 一个网址。给"从网上找热点"那几个工具用（stages/web_tools）。
using HttpGet = std::function<HttpResponse(
    const std::string& url, const std::map<std::string, std::string>& headers,
    double timeout_s)>;

/// 模型要调的一个工具（OpenAI 那套 tool_calls 的一项）。arguments 是 JSON 文本。
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

/// 对话里的一条。role 是 system / user / assistant / tool。
/// assistant 那条可能带 tool_calls；tool 那条要带 tool_call_id 说它回的是哪一次。
struct Message {
    std::string role;
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
};

/// 一轮对话的回复：要么是话（content），要么是"帮我调这几个工具"。
struct ChatReply {
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
};

class Client {
public:
    virtual ~Client() = default;

    /// 同步生成，返回模型吐的原始文本。
    ///
    /// 取消令牌要在耗时点上查。远端后端能查的只有请求前后两个点——
    /// httplib 的同步调用中途打不断；进程内后端可以在每个 token 上查。
    virtual std::string complete(const Request& req,
                                 pipeline::CancelToken& tok) = 0;

    /// 同上，但**边生边给**。
    ///
    /// 用户 2026-09-11 要的"AI 生成在编辑器里流式插入"：写一段话要十几秒，
    /// 攒齐了再一次性蹦出来，中间那十几秒界面上什么都没有——而那正是他要
    /// 看的"写作的过程"。
    ///
    /// **默认实现就是跑一遍同步的然后整段回调一次。** 这不是敷衍：远端
    /// 那条路要不要上 SSE 是另一笔账，而这里的约定只是"能拿到就早点给"。
    /// 拿不到的后端照样能用，只是那一下是整段到的。
    virtual std::string complete(const Request& req, pipeline::CancelToken& tok,
                                 const OnToken& on_token);

    /// **带工具的多轮对话。** `messages` 是到此为止的整段来回，`tools` 是
    /// 给模型的工具表（OpenAI 那套 function 格式）。`opts` 只用 schema_name
    /// （挑模型）、temperature、reasoning_effort、on_thinking、on_token。
    /// 回来的要么是话，要么是一批 tool_calls——调用方自己跑工具、把结果
    /// 以 role=tool 追加进 messages、再来一轮。默认实现不支持，抛 LlmError。
    virtual ChatReply chat(const std::vector<Message>& messages,
                           const nlohmann::ordered_json& tools, const Request& opts,
                           pipeline::CancelToken& tok);
};

/// 拼请求体。
///
/// **schema 不走 response_format，以文字贴在提示词后面**（见
/// schema_as_prompt）。2026-09-14 起只有这一种发法，没有档位、没有退路——
/// 理由写在 client.cpp 的 build_payload 里，一句话是：那层"硬约束"各家
/// 支持得七零八落，而为了兜住差异挂的退档梯子会被别的 400 误触发，
/// 悄悄把结构退没。
///
/// 两处"没填"的兜底：`req.temperature` 空着落到 `cfg.temperature`；
/// 空 schema 就只发提示词本身。
nlohmann::ordered_json build_payload(const config::LLMConfig& cfg,
                                     const Request& req);

/// 把 schema 抄进提示词里。退回 `json_object` 那一下用。
///
/// **退回之后 schema 不会再出现在请求体里的任何地方**——`response_format`
/// 只剩一个 `{"type":"json_object"}`，而我们的提示词里从来没写过字段名
/// （分镜那份光 schema 就几十个字段和一串枚举）。也就是说这一下模型是
/// 在「随便吐个 JSON」，而调用方在按 Shot 的字段去解析它。原来这条退路
/// 的注释写着"全靠提示词里那句『只输出 JSON』"——那句话保不住任何东西。
///
/// 抄的是**原始** schema，不是削过的：这里它是给人（模型）读的文字，
/// minItems 那些数读得懂就有用。
std::string schema_as_prompt(const std::string& prompt,
                             const nlohmann::ordered_json& schema);

/// 从返回里抽出内容。抽不到抛 LlmError。
///
/// 对应 Python 的 body["choices"][0]["message"]["content"]，
/// 那边 KeyError/IndexError 都翻成"大模型返回格式异常"。
std::string extract_content(const std::string& raw_body);

/// 把 HTTP 状态码翻成一句能照着做的话。
///
/// 对应 Python 的 stages/_llm.py explain()。那个模块存在的理由是：
/// 三个阶段本来都是 raise_for_status() 了事，抛出的 httpx 异常一路穿到
/// 最外面变成一句「Internal Server Error」。而这恰恰是最常见的一类失败——
/// 地址填错了、模型名写错了、密钥过期了。用户该看到的是"去设置里改地址"。
std::string explain_status(const config::LLMConfig& cfg, int status,
                           const std::string& body);

/// **我们认识的这家有哪些模型**，每个配一句「选它还是不选它」的话。
/// 返回 `{模型名, 一句话}`，认不出的地址回空。
///
/// 两处在用，两处的理由不一样，但都绕不开同一件事——
/// **智谱的 `/models` 不列免费模型**（2026-09-13 实测：z.ai 和
/// bigmodel.cn 都只回 glm-4.5 ~ glm-5.3-flash 这些收费的，
/// **glm-4.7-flash 不在里面而它能用**，服务端回的 `model` 字段就是它），
/// 而它正好是我们的默认：
///
///   * `/api/llm/models` —— 只照 `/models` 渲染下拉的话，默认那个模型
///     在自己的下拉里是找不到的。顺带给每个模型一句话：一串
///     glm-4.5/4.6/4.7/5/5.1/5.2/5.3 摆在那儿，要紧的两件事
///     （哪个不要钱、哪个会写）名字上一个字都看不出来。
///   * 体检 —— 「这台服务上没有 X」那句话对着默认配置误报。
///
/// ⚠️ 这是本**我们自己维护的小抄，不是模型总表**。真实能用什么以
/// `/models` 拉回来的为准；小抄只用来补它漏掉的那些、和给一句说明。
/// 分数来自 EQ-Bench 长文创作榜（2026-09-14 抓的）。
/// **不写具体单价**：那些数只在转售的网关上核过，各家官网价会变，
/// 写进界面就是在替服务商报价。
std::vector<std::pair<std::string, std::string>> known_models(
    const std::string& base_url);

/// 每次调用时取一份当前配置。
///
/// **不能在构造时拷一份。** /api/connections 能在运行期换大模型地址，
/// 拷一份的话改完之后剧本接口还在往老地址发，而界面已经显示"已应用"了。
using ConfigProvider = std::function<config::LLMConfig()>;

/// 远端 OpenAI 兼容服务。
class RemoteClient : public Client {
public:
    /// `stream_post` 给了就走 SSE（边生边回调）；不给就只有整段那条。
    /// **做成可选的**：TTS 那边和一堆测试拿 RemoteClient 当普通客户端用，
    /// 它们不需要流式，也不该被迫再注入一个函数。
    explicit RemoteClient(ConfigProvider cfg, HttpPost post,
                          HttpPostStream stream_post = {});
    /// 配置固定不变的版本。测试用，生产代码应该传 provider。
    RemoteClient(config::LLMConfig cfg, HttpPost post,
                 HttpPostStream stream_post = {});

    std::string complete(const Request& req, pipeline::CancelToken& tok) override;

    /// 走 SSE。服务不支持流式但返回普通 OpenAI JSON 时会就地解析；
    /// 不会为同一请求自动再发一次，避免重复生成和重复计费。
    std::string complete(const Request& req, pipeline::CancelToken& tok,
                         const OnToken& on_token) override;

    /// **有人要看思考或者要逐字，就走 SSE**（和上面那两条一个规矩）。
    /// 工具调用在流里是拆开来的，收流那头按 index 合回去（见 SseDeltas）。
    /// 两样都没人要，或者没注入流式发送函数，照旧整段 POST。
    ChatReply chat(const std::vector<Message>& messages,
                   const nlohmann::ordered_json& tools, const Request& opts,
                   pipeline::CancelToken& tok) override;

private:
    ConfigProvider cfg_;
    HttpPost post_;
    HttpPostStream stream_post_;
};

/// **把本机装着的大模型命令行当后端。** `[llm].backend = "command"`。
///
/// 用户 2026-09-18：「增加对 claudecode 和 codex 命令版的支持」。
/// 那两个命令行按订阅算钱，而这条流水线最费的就是文字——手上已经有订阅的
/// 人，走它等于这部分不再按 token 花钱。账和限制都写在
/// `config::LLMConfig::command` 那一段。
///
/// 做的事只有一件：把提示词从**标准输入**喂给那个程序，把它印出来的东西
/// 当模型的回答。schema 照旧是贴在提示词后面的文字（`schema_as_prompt`
/// 那条路远端和这条共用），所以结构化输出这边一个字都不用另写。
class CommandClient : public Client {
public:
    explicit CommandClient(ConfigProvider cfg);
    /// 配置固定不变的版本。测试用。
    explicit CommandClient(config::LLMConfig cfg);

    std::string complete(const Request& req, pipeline::CancelToken& tok) override;

private:
    ConfigProvider cfg_;
};

/// 这个命令名对应的默认参数。认不出来回空。
///
/// **摆成一张表而不是写死在调用处**：两个命令行都在快速改版，而认不出的
/// 名字照样能用——用户在 `[llm].command_args` 里自己写就是了。
std::vector<std::string> default_command_args(const std::string& command);

/// 回放。按调用顺序吐出预先录好的返回。
///
/// 给测试和阶段 4 的"回放模式"用：同一段提示词，Python 和 C++ 各跑一遍
/// 解析，比对结果。真调模型的话每次输出都不一样，什么都比不了。
class ReplayClient : public Client {
public:
    explicit ReplayClient(std::vector<std::string> responses);
    std::string complete(const Request& req, pipeline::CancelToken& tok) override;

    /// 录下的每一次请求。测试要拿它检查提示词拼对没有。
    const std::vector<Request>& calls() const { return calls_; }

    /// 回放也能演工具：录的那条要是个带 tool_calls 数组的 JSON 对象，就当模型
    /// 要调工具；不然整条当它说的话。calls() 里记的 prompt 是最后一条消息。
    ChatReply chat(const std::vector<Message>& messages,
                   const nlohmann::ordered_json& tools, const Request& opts,
                   pipeline::CancelToken& tok) override;
    /// chat 那条路最后一次收到的整段来回。用例拿它看工具结果有没有喂回去。
    const std::vector<Message>& last_messages() const { return last_messages_; }

private:
    std::vector<std::string> responses_;
    std::vector<Request> calls_;
    std::vector<Message> last_messages_;
    std::size_t next_ = 0;
};

/// 用 cpp-httplib 发请求。定义在 client_http.cpp 里，那个文件才 include httplib。
HttpPost default_http_post();

/// 同上，但响应边到边给。SSE 那条走它。
HttpPostStream default_http_post_stream();

/// 真上网的 GET（httplib，跟着跳转，带 UA）。只在主程序里有（测试目标不链
/// 这一层）；工具那头收一个 HttpGet，测试塞假的。
///
/// **正文超过 `kGetBodyMax` 当场断开**，回 `transport_error`：网址是模型、网页、人
/// 贴进来的，一个回无底正文的地址不该能吃光引擎的内存。
HttpGet default_http_get();
inline constexpr std::size_t kGetBodyMax = 32 * 1024 * 1024;

/// 造一个大模型客户端。
///
/// 造一个按 `[llm].backend` 分派的客户端。远端和命令行两条，理由见
/// client.cpp 里的实现。
///
/// ⚠️ **回来的这个对象一直有效，换后端不用重造。** 它每次调用现读一遍配置
/// 挑一条，所以调用点把它存进 `static` 是对的（`http/server.cpp` 就是这么
/// 存的）——**而且必须这样**：批量那几条活比起它的那次请求活得久。
///
/// `post` 是发送函数，由调用方注入（生产里传 `default_http_post()`）。
/// **做成参数而不是在这里直接调**：那个函数只链进主目标，测试目标里没有，
/// 写死会让测试链不过。
///
/// `stream_post` 是走 SSE 用的。不给的话仍然能用，只是"边写边看"退回整段
/// 到——写一章、写大纲在界面上就是干等到最后一下子出来。
std::shared_ptr<Client> make_client(HttpPost post, HttpPostStream stream_post = {});

/// 同上，但配置从哪儿读由调用方给。**给测试用的**：生产那个重载读的是
/// `config::runtime()`，而用例要能在两次调用之间把 `backend` 换掉，
/// 才钉得住"换后端不用重启"这件事。
std::shared_ptr<Client> make_client(ConfigProvider cfg, HttpPost post,
                                    HttpPostStream stream_post = {});

}  // namespace changji::llm
