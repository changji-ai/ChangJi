"""一个会录音的假大模型。

它做两件事：
  **把收到的提示词原样记下来**（每条一行，写进 prompts.jsonl）；
  **回一个事先放好的答案**（读 reply.txt，不消耗，同一个答案能发两遍）。

原来是给对拍用的：夹在两个后端和模型之间，同一个请求让两边各发一次，
然后逐字节比两条提示词。对拍随 Python 引擎一起删了，它留下来是因为
**手工看"这一版到底发了什么出去"仍然只有它做得到**——
`[llm].backend = "remote"` 加上一个 base_url 指到它，跑一遍
就能把真实请求体捞出来。单元测试比的是录好的串，看不见当下发的。

跑法（在 changji/ 下）：
    python cpp/tools/fake_llm.py [端口] [工作目录]
"""

from __future__ import annotations

import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

# **Windows 上标准输出默认不是 UTF-8。**
#
# GitHub 的 windows runner 跑 Python 时控制台编码是 cp1252，而下面那些进度
# 是中文——print 到一半直接 UnicodeEncodeError，脚本非零退出。CMake 那边
# 报的是"patch step 失败"、MSBuild 报 MSB8066，**和真正的原因（编码）
# 差着十万八千里**，日志里要翻到最底下才看得见那行 UnicodeEncodeError。
# 2026-09-11 六平台流水线头一次跑，windows-x64 和 windows-arm64 两格就
# 挂在这儿，另外四个平台全过。
#
# errors="replace"：宁可某个字打成问号，也不能因为一个字让整个构建挂掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # 老 Python 或者被重定向成不支持的对象
        pass


WORK = Path("fake_llm")


class Handler(BaseHTTPRequestHandler):
    # 默认的日志会往 stderr 刷每一条请求，对拍时那是纯噪音。
    def log_message(self, fmt, *args):  # noqa: A003
        pass

    def _json(self, code: int, payload) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        # /v1/models：两个后端的"连不连得上"检查都问它。
        if self.path.rstrip("/").endswith("/models"):
            self._json(200, {"data": [{"id": "duiping-fake"}]})
            return
        self._json(404, {"error": "not found"})

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            req = {"_unparsed": raw}

        # 把提示词单独记一行。**记的是拼好的完整串**，不是整个请求体——
        # 请求体里还有 model、temperature 之类，那些两边本来就可能不同，
        # 混在一起比会淹掉真正要看的东西。
        prompt = ""
        for msg in req.get("messages", []) or []:
            # **content 可能是 null**（assistant 那条只带 tool_calls 时就是），
            # `or ""` 不能省：不加的话这儿 TypeError，而假模型是在另一个进程
            # 里崩的——引擎那头看到的是"连上了但没读完"，指向完全错误的方向。
            prompt += msg.get("content") or ""
        with (WORK / "prompts.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps({"path": self.path, "prompt": prompt,
                                "request": req}, ensure_ascii=False) + "\n")

        # **慢一点**（`FAKE_LLM_DELAY=秒`）。真模型要想十几秒到几分钟，而
        # 界面上"在跑的时候长什么样"只有在那段时间里才看得见——秒回的假模型
        # 把那一段整个跳过去了，进度条、停止按钮、那行「在想…」全验不到。
        delay = float(os.environ.get("FAKE_LLM_DELAY", "0") or 0)
        if delay > 0:
            time.sleep(delay)

        reply = self._next_reply()

        # **录的那条要是 `{"tool_calls": [...]}`，就当模型要调工具。**
        #
        # 带工具的那几条路（代理那条对话、上网写一章）没有这一支就验不了：
        # 假模型只会回一段文字，而那条路上真正容易坏的是"工具结果有没有喂
        # 回去"。和 llm::ReplayClient::chat 认的是同一种形状，录一次两处都能用。
        # 录的那条里带 `"thinking"` 就先"想"一段再说话。
        #
        # 真模型想那几分钟里，界面上那一行**只有这个数在动**。秒回的假模型
        # 把那一段整个跳过去，于是"在想的时候长什么样"从来没被看见过——
        # 而 2026-09-21 之前那条路干脆就没接上（思考整段落地，一声不响）。
        # replies.jsonl **一行一个 JSON 值**。原来只认"一行是一个 JSON 字符串"
        # （字符串里面再放一份 JSON），直接写一个对象上去的话这儿认不出来，
        # 一路带着 dict 往下走，到分片那一句才 `KeyError: slice(...)` ——
        # 而那是在假模型这个进程里崩的，引擎那头只看见一条断掉的连接。
        # 两种写法都收下。
        calls = None
        thinking = ""
        parsed = reply if isinstance(reply, dict) else None
        if parsed is None and isinstance(reply, str):
            try:
                got = json.loads(reply)
                if isinstance(got, dict):
                    parsed = got
            except json.JSONDecodeError:
                pass
        if parsed is not None:
            if isinstance(parsed.get("tool_calls"), list):
                calls = parsed["tool_calls"]
            if isinstance(parsed.get("thinking"), str):
                thinking = parsed["thinking"]
            if calls is None and ("thinking" in parsed or "content" in parsed):
                reply = parsed.get("content") or ""
        if not isinstance(reply, str):
            reply = json.dumps(reply, ensure_ascii=False)

        if calls is not None:
            message = {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "id": c.get("id", f"call_{i + 1}"),
                        "type": "function",
                        "function": {
                            "name": c.get("name", ""),
                            "arguments": c.get("arguments", "{}"),
                        },
                    }
                    for i, c in enumerate(calls)
                ],
            }
            finish = "tool_calls"
        else:
            message = {"role": "assistant", "content": reply}
            finish = "stop"

        # **要了流式就得回流式。**
        #
        # 不回的话对面那个 SSE 解析器一个字都抠不出来，而它**不报错**——
        # 回来是一条空回复，界面上像是模型什么都没说。2026-09-21 验代理那条
        # 对话时就撞上了：assistant 那条是空的，项目也没建出来，日志里干净
        # 得很。台架要能验的恰恰是这条路，因为界面真正走的就是它。
        if req.get("stream"):
            self._sse(message, finish, thinking)
            return

        # 非流式那条也带上：智谱那套把整段思考放在同一个字段里一次给全，
        # 引擎那头 `extract_thinking` 认的就是它。
        if thinking:
            message = dict(message, reasoning_content=thinking)
        self._json(200, {
            "id": "duiping",
            "object": "chat.completion",
            "choices": [{"index": 0, "message": message, "finish_reason": finish}],
        })

    def _sse(self, message, finish: str, thinking: str = "") -> None:
        """照 OpenAI 那套把一条回复拆成几帧发出去。

        工具调用在流里是**按 index 拆开**的（名字在第一帧，参数可能分几帧），
        收流那头按 index 合回去。这里一帧发完整的一份——合并逻辑那头本来就
        要能处理"一次就给全"，拆得太碎反而验不到真实形状。
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        def frame(delta, reason=None):
            payload = {"choices": [{"index": 0, "delta": delta, "finish_reason": reason}]}
            self.wfile.write(("data: " + json.dumps(payload, ensure_ascii=False) + "\n\n").encode("utf-8"))

        frame({"role": "assistant"})

        # **思考先发、一小段一小段发。** 一次发完的话那个数从 0 直接跳到终值，
        # 界面上"它还活着"这件事照样没验到。间隔用 FAKE_LLM_THINK_MS（默认
        # 120 毫秒一段）。字段名 `reasoning_content` 是智谱那套，引擎认它。
        if thinking:
            step = int(os.environ.get("FAKE_LLM_THINK_MS", "120") or 120)
            size = max(1, len(thinking) // 12)
            for i in range(0, len(thinking), size):
                frame({"reasoning_content": thinking[i:i + size]})
                self.wfile.flush()
                if step > 0:
                    time.sleep(step / 1000.0)

        if message.get("tool_calls"):
            for i, c in enumerate(message["tool_calls"]):
                frame({"tool_calls": [{
                    "index": i,
                    "id": c["id"],
                    "type": "function",
                    "function": c["function"],
                }]})
        elif message.get("content"):
            # 正文一段一段发，**为的是验增量拼接**：一次发完的话，
            # "把每一段接起来"那一层根本没跑到。
            #
            # 给一档节奏（`FAKE_LLM_TOKEN_MS`，默认 0 = 不等）。真模型是一个
            # 字一个字往外吐的，而"字在长"这件事**只能在界面上看出来**——
            # 秒回的假模型把那一段整个跳过去，于是"两张图之间字变多了没有"
            # 根本没法验（方案文档里那条"会动的东西要截两张"）。
            text = message["content"]
            step = int(os.environ.get("FAKE_LLM_TOKEN_MS", "0") or 0)
            pieces = max(2, min(24, len(text) // 4)) if step > 0 else 2
            size = max(1, -(-len(text) // pieces))
            for i in range(0, len(text), size):
                frame({"content": text[i:i + size]})
                self.wfile.flush()
                if step > 0:
                    time.sleep(step / 1000.0)

        frame({}, finish)
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


    def _next_reply(self) -> str:
        """按顺序取下一个答案。

        replies.jsonl 一行一个（JSON 字符串）。**用光了重复最后一条**，
        不是报错：某一侧可能比另一侧多问一次模型（比如它在别的地方
        又校验了一遍），那时候报错会把一次正常的对拍变成失败。

        /api/plan 一次请求要问两遍（圣经 + 分镜），所以必须排队；
        而**每一侧各起一个假模型**，两边不共用队列——共用的话
        第二个后端拿到的是第一个后端剩下的，永远错位。
        """
        queue = WORK / "replies.jsonl"
        if not queue.is_file():
            single = WORK / "reply.txt"
            return single.read_text(encoding="utf-8") if single.is_file() else "{}"

        lines = [ln for ln in queue.read_text(encoding="utf-8").splitlines() if ln]
        if not lines:
            return "{}"
        cursor_file = WORK / "cursor.txt"
        try:
            cursor = int(cursor_file.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            cursor = 0
        index = min(cursor, len(lines) - 1)
        cursor_file.write_text(str(cursor + 1), encoding="utf-8")
        try:
            return json.loads(lines[index])
        except json.JSONDecodeError:
            return lines[index]


def main() -> int:
    global WORK
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8125
    if len(sys.argv) > 2:
        WORK = Path(sys.argv[2])
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "prompts.jsonl").write_text("", encoding="utf-8")
    (WORK / "cursor.txt").write_text("0", encoding="utf-8")
    print(f"假大模型在 127.0.0.1:{port}，工作目录 {WORK.resolve()}", flush=True)
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
