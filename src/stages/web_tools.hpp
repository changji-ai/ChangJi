#pragma once

// 从网上找热点那几个工具。给大模型的工具表 + 引擎这头真跑的实现。
//
//   hot_topics   百度热搜、今日头条热榜、微博热搜合在一起，几十条标题
//   web_search   必应搜一个词，前几条的标题、链接、摘要
//   fetch_page   打开一个网址，正文去掉标签，最多六千字
//
// 上网走一个 HttpGet（llm/client.hpp），主程序给真的，测试塞假的。解析都是
// 纯函数，单测钉着：热榜的 JSON 长什么样、必应的结果页长什么样，改了这儿
// 先红。

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/client.hpp"

namespace changji::stages {

struct HotItem {
    std::string title;
    std::string note;
    std::string url;
    std::string source;   ///< 百度 / 头条 / 微博
};

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

struct WebTools {
    llm::HttpGet get;
    double timeout_s = 20.0;
};

/// 给模型的工具表（OpenAI 那套 function 格式）。
nlohmann::ordered_json web_tool_specs();

/// 跑一个工具，回给模型看的文字。名字不认识、参数不对都回一句话，不抛。
std::string run_web_tool(const WebTools& web, const std::string& name,
                         const std::string& arguments_json);

/// 场记调这三个工具时，界面上（和任务账本上）那一行说什么：「在看网上什么热」
/// 「在网上搜「…」」「在读网页」。**给人看的，翻。** 不是这三个就回空串。
///
/// 摆在这儿、不摆在代理那头：代理的源码在外层产品仓库里（2026-09-23 起），
/// 而多语言那张表在这个仓库；句子和它的工具放在一起，两边才对得上。
std::string web_tool_label(const std::string& name, const std::string& arguments_json);

// ---- 可测的纯解析 ----
std::vector<HotItem> parse_baidu_hot(const std::string& body);
std::vector<HotItem> parse_toutiao_hot(const std::string& body);
std::vector<HotItem> parse_weibo_hot(const std::string& body);
std::vector<SearchHit> parse_bing_results(const std::string& html);
/// 去脚本、去样式、去标签、解几个实体、并空白。
std::string html_to_text(const std::string& html);
std::string url_encode(const std::string& s);

}  // namespace changji::stages
