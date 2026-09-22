#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "pipeline/jobs.hpp"

namespace changji::infer {

/// 有没有把进程内文本大模型编进来（`CHANGJI_LLAMA=ON`）。
bool llama_chat_available();

/// 一次补全要怎么跑。
struct ChatRun {
    double temperature = 0.7;
    /// ≤ 0 = 上下文里剩多少就写多少（见 LlamaChat::complete 里那段）。
    int max_tokens = 0;
    /// 让模型先想再写。**默认开**——2026-09-14 定的方向是「现在的模型都要
    /// 思考」，远端那条关都关不掉。本地这条能关（`[llm].thinking`），小卡上
    /// 跑 8B 时省下的是一半时间。模板不认这一项的模型（`supports_thinking()`
    /// 为假）填什么都没用。
    bool thinking = true;
    /// 正文每来一段回调一次（增量）。跑在生成循环里，每个 token 一次，别做慢活。
    std::function<void(const std::string&)> on_piece;
    /// 思考稿每来一段回调一次（增量）。同上。
    std::function<void(const std::string&)> on_thinking;
};

/// 进程内跑文本大模型。写正文、拆分镜用它。
///
/// **存在的理由是"一个程序跑所有"**（CLAUDE.md 第一行）。以前 LLM 是外面
/// 一个 llama-server，于是有两个进程要起、两份显存要算，而且**调度器管不着
/// 它**——出片要显存时没法让它先让开，只能靠人去把那个服务停掉。做成进程内
/// 的槽之后，它和出图出片一样归调度器管：平时常驻，显存真不够时被驱逐，
/// 够就不动（见 Scheduler::set_free_vram_probe）。
///
/// **2026-09-14 删、2026-09-19 接回来，中间变了一件事：没有语法层了。**
/// 当年这条路的独门武器是 GBNF 语法采样（schema 转成语法钉住形状），而它
/// 和思考是冲突的——语法把 `<think>` 判成非法，思考被挤进键名和字符串，
/// 于是整条路删了。接回来的这一版**和远端同一个做法**：schema 当文字贴在
/// 提示词后面（llm::schema_as_prompt），回来之后本地校验（checked_output）；
/// 思考自由流，按 `<think>…</think>` 拆开（think_split.hpp）。本地和远端
/// 于是只差"谁在算"，不差"怎么约束"。
class LlamaChat {
public:
    /// 载模型。失败回 nullptr 并把原因写进 `why`。
    ///
    /// `use_gpu` 传 false 就是纯 CPU 跑。显存紧张时调用方可以这么退。
    ///
    /// `parallel` 是**最多**开几个上下文，也就是最多同时跑几路。权重只载
    /// 一份，每个上下文自己一份 KV cache——花的是那几份的显存。
    /// **开不出来就少开一个**：第一个就开不出来才算失败，后面的开不出来
    /// 只是并发度低一档。所以显存决定实际并发度，这个参数只是上限。
    /// `n_ctx` 是开多长的上下文（token），**0 = 用模型训练时的长度**。
    /// 这一项是显存账上最大的一笔：一路 KV cache 的大小和它成正比，而
    /// `parallel` 路就是这个数乘以路数。见 config::LLMConfig::context_tokens。
    static std::unique_ptr<LlamaChat> load(const std::filesystem::path& model,
                                           bool use_gpu, std::string& why,
                                           int parallel = 1, int n_ctx = 0);

    ~LlamaChat();
    LlamaChat(const LlamaChat&) = delete;
    LlamaChat& operator=(const LlamaChat&) = delete;

    /// 跑一次补全。
    ///
    /// `out` 只有**正文**：思考那一段已经拆走，从 `run.on_thinking` 走了。
    /// `tok` 被取消时提前收工，返回已经生成的部分并返回 true（取消不是失败）。
    /// 失败返回 false 并填 `why`。`truncated` = 写到 token 上限还没收尾
    /// （远端那条叫 finish_reason = length）：那份正文多半是半截 JSON，
    /// 调用方要当失败处理，别拿去解析。
    bool complete(const std::string& prompt, const ChatRun& run,
                  pipeline::CancelToken& tok, std::string& out, std::string& why,
                  bool& truncated);

    /// 这个模型的上下文长度。提示词超了要先知道，别等它自己截断。
    int context_tokens() const;

    /// 实际开出来几个上下文，也就是能同时跑几路。**可能比要的少**。
    int slots() const;

    /// 这个模型的对话模板认不认「开/关思考」。不认的话 `ChatRun::thinking`
    /// 是句空话——设置页要把这件事说出来，别让人以为关了。
    bool supports_thinking() const;

private:
    LlamaChat();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace changji::infer
