#pragma once

// 大模型的接入信息：常见平台的地址清单、可用模型列表。
//
// ---
//
// 方案的破契约白名单里原本有这两条，理由是"进程内推理之后语义重定义"。
// **决策 4 之后这个理由不成立了。**
//
// 那条决策定的是"Pi 保留、模型文件走配置"，而它的一个直接后果是：
// 用远端大模型不再是过渡状态，是长期形态之一——树莓派没有跑 14B 的内存，
// 它只能打到局域网的 Windows 机器或者云端。所以：
//
//   /api/llm/providers  原样保留。那份"省得查文档"的地址清单照样有用。
//   /api/llm/models     **扩展**不是替换。远端有哪些模型照旧列，
//                       另外加一个字段列本地的 gguf。
//
// 加字段是向后兼容的：前端现在只读 .models 和 .error，多出来的它不看。
// 换成"改成列本地 gguf"就真的破契约了——设置页那个下拉框会突然从
// "远端服务上的模型"变成"本机文件"，而用户配的是远端服务。

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "llm/chat_pick.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// GET /api/llm/providers —— 常见大模型平台的接入地址。
///
/// 这些平台都提供 OpenAI 兼容接口，所以不用为每一家写适配器——
/// 填对地址和密钥就能用。列出来只是免得用户去翻各家文档找那一行 base_url，
/// 选完仍然可以手改，这里不锁死任何东西。
ApiResult get_llm_providers();

/// 发一次 GET。和 llm::HttpPost 一样是注入的，理由也一样。
using HttpGet = std::function<llm::HttpResponse(
    const std::string& url, const std::map<std::string, std::string>& headers,
    double timeout_s)>;

/// GET /api/llm/models —— 那台大模型服务上都有哪些模型，外加本机的 gguf。
///
/// 模型名以前只能手打。打错了要跑到写剧本那一步才报错，而报出来的是一个
/// 404——用户看不出是地址错了还是名字错了。列表拉过来给人选，
/// 这类错就没机会发生。
///
/// 问不到就返回空列表并说明原因，界面退回手打，不至于因为列不出来就没法填。
ApiResult get_llm_models(const config::Settings& settings, const HttpGet& fetch);

/// 同上，但**问指定的那一家**，不是配置里存着的那一家。
///
/// ⚠️ **换平台那一下必须用它。** 模型窗里挑了另一家之后，界面上的地址已经
/// 换了，而配置里还是旧的——照旧问 `get_llm_models` 的话，回来的是**上一家
/// 的模型列表**，而且一声不响（用户 2026-09-17：「大语言模型选择平台后无法
/// 立即刷新模型列表」）。
///
/// `base_url` 留空就用配置里那个；`api_key` 留空也用配置里那把——刚换一家、
/// 密钥还没填的时候，回来多半是 401，而那条路本来就返回"空列表加一句原因"。
///
/// **密钥只走请求体，不进查询串**：查询串会落进访问日志和浏览器历史。
ApiResult post_llm_models(const nlohmann::json& body,
                          const config::Settings& settings,
                          const HttpGet& fetch);

/// 用 cpp-httplib 发 GET。定义在 client_http.cpp 里。
HttpGet default_http_get();

// ---- 自己加的服务（用户 2026-09-26） ----
//
// 「大模型里通过 api 和 key 和名字添加一个新大模型使用路径，在输入框下面列表
// 出来」。原来厂商单子上摆的是**填过密钥的那几家**——没有地方加一家清单上没有的
// （公司自建的、代理转发的），本机那几家（Ollama）不要密钥，于是永远摆着、不管
// 起没起。现在单子上摆的是**人加过的**：
//
//   · 清单上那几家：填过密钥的算加过（老用户一家都不会少），另外按「加」加进来的；
//   · 清单上没有的：名字 + 地址存在 `<配置目录>/llm_providers.json`；
//   · 密钥**照旧按地址存**（`config::write_api_key_for`），这份文件里一个字都没有。
//
// `/api/llm/providers` 每一家多报两栏：`added`（进不进输入框底下那张单子）、
// `custom`（是不是人自己加的——那几家删掉就没了，清单上的删掉只是不摆）。

/// 这份文件在哪。给用例用。
std::filesystem::path custom_providers_path();

/// POST /api/llm/providers/add {name, base_url, api_key?}
///
/// 地址和清单上某一家一样的，不另起一条：记成「加过了」，名字照清单上的。
/// 同一个地址加第二次是改名字、换密钥。
ApiResult post_llm_provider_add(const nlohmann::json& body);

/// POST /api/llm/providers/update {id, name?, base_url?, api_key?} —— 改一家。
///
/// 用户 2026-09-27：「增加添加的大模型修改功能，修改 api 和 key」。
/// - **清单上那几家只换密钥**：地址就是它是谁，名字跟着界面语言走。
/// - 自己加的：名字、地址、密钥都能改。`api_key` 空着 = 不动它。
/// - 换了地址：没给新密钥就把旧的那把搬过去（旧地址名下那把删掉）；记下的模型单子
///   跟着搬；**默认指着旧地址的话跟着换**。撞上另一家的地址回 409。
///   已经单独挑过这一家的对话（`chats/<编号>.model.json`）还记着旧地址——那几份
///   散在各部片子里，这儿不去翻；那条对话里重挑一下就好。
ApiResult post_llm_provider_update(const nlohmann::json& body);

/// POST /api/llm/providers/remove {id}
///
/// 从单子上拿掉，**密钥一起删**（不删的话那一家还算「填过密钥」，下次又摆回来）。
ApiResult post_llm_provider_remove(const nlohmann::json& body);

/// GET /api/llm/local —— 本机模型目录里能当编剧用的那几份 gguf。
///
/// 「本地模型直接在大模型里显示和下载，下载一个之后……厂商增加一个本地，模型名
/// 就是本地模型名」（用户 2026-09-26）。**判据是盘上有没有**，不是配置里填没填：
/// 下完一份就出现，人自己拷进 `llm/` 的也算（`llm::is_llm_weights`）。
///
/// `available`：这个二进制编没编进 llama.cpp。没编进的话列出来也跑不了，界面照实说。
/// `POST /api/llm/local/release`：现在就把本地模型的显存还回去（正在用时 409）。
ApiResult post_llm_local_release();

ApiResult get_llm_local(const config::Settings& settings);

// ---- 每一家的模型单子记在盘上（用户 2026-09-26：「模型列表应该持久化保存，如果有
// 新模型的话前面可以显示个点」） ----
//
// `<配置目录>/llm_models/<地址短名>.json`：上一次问到的那一串（`models`）和人看过的
// （`seen`）。问到了就换上新的；问不到就拿上一次的顶上（`cached: true`）。
// `new` = 问到了、人还没看过的那几个——界面上前面摆个点。**头一回问到一家全算看过**。

std::filesystem::path models_cache_path(const std::string& base_url);

/// 给 `/api/llm/models` 的回包记账、补上 `new`（问不到时补上上一次的 `models`）。
/// **只在路由那一层叫**：`get_llm_models` 本身不碰盘，用例拿假的 GET 测它时不往真
/// 配置目录里写东西。
nlohmann::json remember_models(const std::string& base_url, nlohmann::json result);

/// POST /api/llm/models/seen {base_url} —— 人看过这一家的单子了，点都消掉。
ApiResult post_llm_models_seen(const nlohmann::json& body);

// ---- 新对话用哪个 ----
//
// 「默认」只有一份：整台机器的 `[llm]`（本地的话再加 `[models].llm`）。两处会改它：
// 设置 ▸ 大模型那颗「设为默认」（`/api/llm/default`），和人在对话里挑了模型
// （`post_chat_model`——最后一次挑的就是下一条新对话用的）。没单独挑过的对话跟着它；
// 为了它动的时候老对话不跟着换，一条对话说第一句话时把当时的默认记成它自己的
// （`agent/chat_api.cpp`）。

/// 把默认换成 `p`（只动带着的那几栏）。写配置、换运行时那一份。
void remember_default(const llm::ChatPick& p);

/// POST /api/llm/default {backend: remote, base_url, model} 或 {backend: local, file}
ApiResult post_llm_default(const nlohmann::json& body);

// ---- 这一条对话用哪个模型（`llm/chat_pick.hpp`） ----

/// GET /api/chat/model?project=&chat=
///
/// 回 `pick`（这条对话自己挑的，没挑是 null）和 `effective`（叠上全局之后实际会
/// 用的：backend / base_url / model / file / effort）。输入框底下那一条显示后者。
ApiResult get_chat_model(const std::string& project, const std::string& chat,
                         const config::Settings& settings);

/// POST /api/chat/model {project, chat, backend?, base_url?, model?, file?, effort?, reset?}
///
/// 只改带来的那几栏。换了家（地址变了）又没带模型名的，模型名清空——上一家的
/// 名字在新的一家多半不存在，留着就是发一个必 404 的名字出去。
/// `backend: "local"` 要带 `file`，而且那份得在模型目录里、是一份编剧模型。
ApiResult post_chat_model(const nlohmann::json& body, const config::Settings& settings);

}  // namespace changji::http
