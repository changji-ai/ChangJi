// 常见大模型平台的接入地址。项目页点编剧模型名弹出来那个窗口里，
// 「服务」那个下拉就是它（「大模型」那一节 2026-09-14 从设置页搬走了）。
//
// 原来这份由 cpp/tools/gen_prompts.py 从 web/server.py 的 LLM_PROVIDERS
// 导出，**那两个文件都已经不在了**（Python 侧删干净了），所以现在直接改
// 这里。各家地址会变，发现不对就改。
//
// 这只是一本**地址簿**——填了地址还要自己填密钥、选模型，列在这儿不等于
// 推荐、更不等于接通过。2026-09-14 按用户要求删掉了 OpenRouter 那一项。

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kLlmProvidersJson =
    R"CJ([
 {
  "id": "ollama",
  "name": "Ollama（本机）",
  "base_url": "http://127.0.0.1:11434/v1",
  "local": true,
  "note": "本机跑模型，不要密钥。先 ollama pull 一个模型"
 },
 {
  "id": "lmstudio",
  "name": "LM Studio（本机）",
  "base_url": "http://127.0.0.1:1234/v1",
  "local": true,
  "note": "在 LM Studio 里开启本地服务器"
 },
 {
  "id": "vllm",
  "name": "vLLM（本机或局域网）",
  "base_url": "http://127.0.0.1:8000/v1",
  "local": true,
  "note": "自建推理服务，地址按实际部署改"
 },
 {
  "id": "deepseek",
  "name": "DeepSeek 深度求索",
  "base_url": "https://api.deepseek.com/v1",
  "local": false,
  "note": "platform.deepseek.com 申请密钥"
 },
 {
  "id": "siliconflow",
  "name": "硅基流动 SiliconFlow",
  "base_url": "https://api.siliconflow.cn/v1",
  "local": false,
  "note": "聚合了很多开源模型，一个密钥都能用"
 },
 {
  "id": "dashscope",
  "name": "阿里云百炼（通义千问）",
  "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
  "local": false,
  "note": "注意是 compatible-mode 那条地址"
 },
 {
  "id": "moonshot",
  "name": "月之暗面 Kimi",
  "base_url": "https://api.moonshot.cn/v1",
  "local": false,
  "note": "platform.moonshot.cn 申请密钥"
 },
 {
  "id": "zai",
  "name": "智谱 z.ai",
  "base_url": "https://api.z.ai/api/paas/v4",
  "local": false,
  "note": "glm-4.7-flash 免费但限流很紧；国内站 open.bigmodel.cn 是同一套后端"
 },
 {
  "id": "zhipu",
  "name": "智谱 GLM（国内站 · 默认）",
  "base_url": "https://open.bigmodel.cn/api/paas/v4",
  "local": false,
  "note": "默认就是这家。glm-4.7-flash 不要钱；写作要好换 glm-5.3。和 z.ai 同一套后端、同一把密钥"
 },
 {
  "id": "ark",
  "name": "火山方舟（豆包）",
  "base_url": "https://ark.cn-beijing.volces.com/api/v3",
  "local": false,
  "note": "模型名填推理接入点 ID，不是模型名字"
 },
 {
  "id": "hunyuan",
  "name": "腾讯混元",
  "base_url": "https://api.hunyuan.cloud.tencent.com/v1",
  "local": false,
  "note": "腾讯云控制台申请密钥"
 },
 {
  "id": "minimax",
  "name": "MiniMax",
  "base_url": "https://api.minimax.chat/v1",
  "local": false,
  "note": "platform.minimaxi.com 申请密钥"
 },
 {
  "id": "stepfun",
  "name": "阶跃星辰 StepFun",
  "base_url": "https://api.stepfun.com/v1",
  "local": false,
  "note": "platform.stepfun.com 申请密钥"
 },
 {
  "id": "lingyi",
  "name": "零一万物 Yi",
  "base_url": "https://api.lingyiwanwu.com/v1",
  "local": false,
  "note": "platform.lingyiwanwu.com 申请密钥"
 },
 {
  "id": "openai",
  "name": "OpenAI",
  "base_url": "https://api.openai.com/v1",
  "local": false,
  "note": "国内直连多半要自备网络"
 }
])CJ";

}  // namespace changji::stages::prompt
