#pragma once

// 剧本读写和章节增删改。
//
// 这几个接口属于阶段 3（编辑接口），当时漏了。漏的原因值得记一笔：
// 阶段 3 是按 test_web_editing.py 的覆盖面移植的，而那个文件不测这几个。
// **对拍语料的覆盖面成了移植的覆盖面**——只要拿测试文件当清单，
// 测试没覆盖的就会整片漏掉，而且漏得很干净，没有任何编译或运行迹象。
// 是阶段 2 判据的实机验证（前端调了 404）才把它们暴露出来的。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// 下一个没被占用的章节编号。
///
/// **只数 epNN 那些。** 预告片挂在 trailer 上，把它也数进去的话，
/// 有了预告之后新建的第二章会跳号变成 ep03。
std::string next_episode_id(const models::Project& project);

/// GET /api/script —— 读某一章的剧本。
ApiResult get_script(const std::string& path, const std::string& episode_id);

/// GET /api/script/context —— 这一章的原料，给人看的那份。
///
/// 和 AI 改编时拿到的是同一批东西：章节计划压着的场（在哪、跟着谁、要什么、
/// 谁拦着）、原文切片、停在什么钩子上、上一章的结尾、四段按秒的排法、
/// 对白字数预算。以前这些只进提示词，剧本页上一样都看不到——一章在设定里
/// 切好了 900 字正文，到剧本页看到的是「还没有剧本」。
///
/// 不在章节计划上的章（老项目、手动加的、预告片）也有答案，只是场和原文
/// 是空的，source 是 premise。
///
/// `chapter_blank`：挂着故事里的某一章、而那一章一个字都还没写。剧本页
/// 拿它关掉「改编成剧本」并挂牌子说清已有的那份剧本是照什么写的，理由在
/// 那个字段旁边。
ApiResult get_script_context(const std::string& path,
                             const std::string& episode_id);

/// POST /api/script —— 改剧本，可以顺便重出分镜。
///
/// 重出会**覆盖整张分镜表**，人工改过的镜头会丢，所以要显式勾 regenerate。
ApiResult post_script(const nlohmann::json& body, llm::Client& client,
                      pipeline::CancelToken& tok);

/// POST /api/episode —— 新建一章。id 留空就自动往后编号。
ApiResult post_episode(const nlohmann::json& body);

/// POST /api/episode/action —— 删除、复制或改名一章。
///
/// 复制只带走剧本和分镜的文案，**不带产出物和状态**。复制出来的一章
/// 是要重跑的，把状态也带过去会让它显示成已完成但没有文件。
ApiResult post_episode_action(const nlohmann::json& body);

}  // namespace changji::http
