#pragma once

// 把模型吐出来的 token 流分成「思考」和「正文」两路。
//
// 现在的模型都要思考（2026-09-14 定的方向）。远端那条各家把思考放在单独的
// 字段里（智谱 reasoning_content），`SseDeltas` 按字段分；本地这条路上思考和
// 正文**混在同一条 token 流里**：Qwen3 先写 `<think>…</think>` 再写正文，
// DeepSeek-R1 那一族的模板会替它把 `<think>` 先写进提示词末尾，模型一开口
// 就在思考里。这儿要自己拆——不拆的话思考稿会直接流进用户的编辑器，正文
// 那头解 JSON 也会先撞上一大段中文。
//
// 2026-09-14 删掉进程内那条路的理由正是这件事：当年靠 GBNF 语法钉形状，
// 而语法把 `<think>` 判成非法，思考被挤进键名和字符串。今天没有语法层了
// （结构整个交给提示词，回来之后本地校验），思考自由流，拆开就行。
//
// **标签可能被 token 边界切成两半**（"<thi" + "nk>"），所以尾巴上可能是半个
// 标签的那几个字节先攒着，下一段来了再判。流结束时 `finish()` 把攒着的按
// 当前状态吐出去。
//
// 纯逻辑，不碰 llama.cpp，测试目标里也编。

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace changji::infer {

class ThinkSplitter {
public:
    using Sink = std::function<void(const std::string&)>;

    /// `already_open`：模板已经把起始标签写进提示词末尾，模型一开口就在思考里。
    /// 标签给空串 = 这个模型不思考，来什么都当正文。
    ThinkSplitter(std::string start_tag, std::string end_tag, bool already_open)
        : start_(std::move(start_tag)),
          end_(std::move(end_tag)),
          thinking_(already_open && !end_.empty()) {}

    void on_thinking(Sink s) { think_sink_ = std::move(s); }
    void on_content(Sink s) { content_sink_ = std::move(s); }

    /// 每来一段叫一次。
    void feed(const std::string& piece) {
        pending_ += piece;
        while (true) {
            const std::string& tag = thinking_ ? end_ : start_;
            if (tag.empty()) {   // 这个方向不认标签：整段按当前状态走
                flush(pending_);
                pending_.clear();
                return;
            }
            const auto at = pending_.find(tag);
            if (at != std::string::npos) {
                flush(pending_.substr(0, at));
                pending_.erase(0, at + tag.size());
                thinking_ = !thinking_;
                // 刚从思考里出来：Qwen3 在 </think> 后面跟两个换行，那不是正文。
                if (!thinking_) skip_ws_ = true;
                continue;
            }
            // 没有整个标签。尾巴上要是像半个标签就留着，别的都放出去。
            const std::size_t keep = partial_suffix(pending_, tag);
            flush(pending_.substr(0, pending_.size() - keep));
            pending_.erase(0, pending_.size() - keep);
            return;
        }
    }

    /// 流结束：攒着的半个标签按当前状态吐出去。
    void finish() {
        flush(pending_);
        pending_.clear();
    }

    /// 到此为止收到的两路，各自接起来的全文。
    const std::string& thinking() const { return think_; }
    const std::string& content() const { return content_; }
    bool in_thinking() const { return thinking_; }

private:
    /// `s` 的尾巴有多长和 `tag` 的开头一样（"abc<th" 对 "<think>" 是 3）。
    static std::size_t partial_suffix(const std::string& s, const std::string& tag) {
        const std::size_t max = std::min(s.size(), tag.size() - 1);
        for (std::size_t n = max; n > 0; --n) {
            if (s.compare(s.size() - n, n, tag, 0, n) == 0) return n;
        }
        return 0;
    }

    void flush(const std::string& piece) {
        if (piece.empty()) return;
        if (thinking_) {
            think_ += piece;
            if (think_sink_) think_sink_(piece);
            return;
        }
        std::string out = piece;
        if (skip_ws_) {
            const auto first = out.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return;   // 全是空白，还在跳
            out.erase(0, first);
            skip_ws_ = false;
        }
        content_ += out;
        if (content_sink_) content_sink_(out);
    }

    std::string start_;
    std::string end_;
    bool thinking_ = false;
    bool skip_ws_ = false;
    std::string pending_;
    std::string think_;
    std::string content_;
    Sink think_sink_;
    Sink content_sink_;
};

}  // namespace changji::infer
