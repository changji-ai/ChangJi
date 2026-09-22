#pragma once

#include <functional>
#include <memory>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "models/hardware.hpp"

namespace changji::llm {

/// 进程内跑大模型。和 `RemoteClient` 同一个接口，换掉它就行。
///
/// **和远端那条的差别只有"谁在算"**：schema 一样是当文字贴在提示词后面
/// （`schema_as_prompt`），回来之后一样过本地校验（`checked_output`），
/// 思考一样从 `Request::on_thinking` 走，日志一样落 `llm_log/`。
/// 多出来的一件事是它归调度器管：借槽（`Slot::LLM`）才拿得到模型，出图
/// 出片要显存时调度器会按**实时空闲显存**决定要不要把它卸掉——够就不动，
/// 一次重装是几十秒。
///
/// 权重路径在 `[models].llm`，`[llm].backend = "local"` 打开。
///
/// 2026-09-14 删、2026-09-19 接回来。为什么删、为什么回来、中间变了什么，
/// 见 infer/llama_chat.hpp 上那段。一句话：这是产品要给别人用的——图片、
/// 视频、配音全在本地，编剧不该是唯一要人自己去申请密钥、自己撞 429 的一环。
class LocalClient : public Client {
public:
    explicit LocalClient(ConfigProvider cfg);
    std::string complete(const Request& req, pipeline::CancelToken& tok) override;
    /// 边生边给。进程内这条路能逐 token 拿到，所以真流式的就是它。
    std::string complete(const Request& req, pipeline::CancelToken& tok,
                         const OnToken& on_token) override;

private:
    ConfigProvider cfg_;
};

/// 进程内那条现在是什么状态。设置页的引擎卡读它。
///
/// **并发度是算不出来的，只能问。** 配置里 `[llm].parallel` 是个上限，
/// 实际开出来几个上下文由显存说了算（见 LlamaChat::load）。界面上不把
/// 这两个数分开摆，用户会以为自己配了 4 就是 4 路，而实际可能只有 1 路。
struct LocalLlmStatus {
    bool loaded = false;
    int slots = 0;           ///< 实际开出来的上下文数，也就是能同时跑几路
    int context_tokens = 0;  ///< 每个上下文多长
    bool supports_thinking = false;  ///< 模板认不认「开/关思考」
};

LocalLlmStatus local_llm_status();

/// 把大模型注册成调度器的一个槽。
///
/// **`[llm].backend != "local"` 时什么都不做**：远端那条没有本地权重，
/// 注册一个装不上的槽只会在借它的时候抛一句没意义的错。
///
/// 注册之后它是常驻的（`Residency::Cached`）：装上就不主动卸，显存真不够
/// 时才被驱逐。驱逐优先级设得比出图出片低，腾地方时先卸它——写一章只跑
/// 一次，而出图出片每镜都要。
///
/// **注册不等于加载。** 调度器是借出时才装的——用户定的
/// 「用的时候才加载，不做启动预载」。
void register_llm_slot(std::function<config::Settings()> provider,
                       const models::HardwareProfile& profile);

/// 造进程内那条客户端。**这个二进制没编进 llama.cpp 时回 nullptr**，
/// 由 `make_client` 那头退回远端并在 stderr 上说一声——用户多半只是拿了个
/// 不带 llama 的构建，而远端那条只要地址填了就能用。
std::shared_ptr<Client> make_local_client(ConfigProvider cfg);

}  // namespace changji::llm
