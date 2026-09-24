#pragma once

// 两个长任务：连着写好几章、给还没分镜的章节批量出分镜。
//
// 和前面五个接口的根本区别：**它们跑几分钟，不是几十秒**。
// 所以走 job 表——立刻返回 {"started": true, ...}，进度靠
// GET /api/script/series 轮询或者 WebSocket 推。
//
// 两个共用同一个任务槽（JobKind::Write）。Python 那边也是同一个
// WriteState，所以"写全片的时候不能同时批量出分镜"这条限制是照抄的，
// 不是我加的。

#include <memory>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// POST /api/script/series —— 连着写好几章，边写边存。
///
/// 跟单章不同，这个**直接落库并建出章节**。一章一章手点新建再手点写，
/// 写到第五章人就放弃了，那量产就无从谈起。写完可以逐章再改。
///
/// 客户端用 shared_ptr 是因为任务在工作线程上跑，可能比这次请求活得久。
/// 传引用的话，调用方一不小心让它先析构，表现是工作线程访问已释放对象——
/// 而那时候栈上已经没有任何线索指向这里了。
ApiResult post_script_series(const nlohmann::json& body,
                             std::shared_ptr<llm::Client> client);

/// POST /api/story/chapters —— 把还没正文的章一口气全展开。
///
/// 十六章的故事逐章点十六次不像话，而每章要跑十几秒，加起来是分钟级的，
/// 所以和上面两个一样走 job 表。**共用同一个任务槽**，也就是说写全片、
/// 批量分镜、批量展开三件事同时只能做一件——它们都在跟同一个大模型排队。
///
/// **每写完一章就落库并重读**：下一章的提示词里「上一章是这么结束的」
/// 拿到的才是刚写完那一章。全写完再一次性存的话，中途停掉就全白干了，
/// 而且每一章都以为自己接的是空的上一章。
ApiResult post_story_chapters(const nlohmann::json& body,
                              std::shared_ptr<llm::Client> client);

/// POST /api/plan/all —— 把还没分镜的章节一次补齐。
///
/// 连着写了五章之后，每一章都还得单独点一次「重出分镜」。
/// 五次里漏掉一次，跑整个项目时那一章就被跳过去了。
/// POST /api/script/all —— 把所有挂着章、又还没剧本的章一次改编完。
/// 和「批量补分镜」对称：那一步是它后面那一步。
ApiResult post_script_all(const nlohmann::json& body,
                          std::shared_ptr<llm::Client> client);

ApiResult post_plan_all(const nlohmann::json& body,
                        std::shared_ptr<llm::Client> client);

/// POST /api/story/understand —— 理解故事：一件活，把整本书读一遍，拍片
/// 要的全出来。
///
/// 用户 2026-09-17：设定页只留一颗「理解故事」，跑完人物、关系、场景、长相
/// 和剧本都有了；「出图」只是画。理解和定妆合成一件（两件都是把整本书读一
/// 遍，第二遍的输入就是第一遍的输出）；剧本一章一章接着写——一次调用写不出
/// 整部，第二章要看着第一章写。
///
/// 三步：读一遍（post_story_understand_once：结构和长相一次出来）→ 对齐章节
/// （存故事时自动做，这儿再对一次）→ 逐章写剧本（同 script/all 那个循环）。
/// 跑在 JobKind::Write 那个槽上，进度走 GET /api/script/series；能停，停了
/// 已经出来的留着。
///
/// overwrite=false：故事里已有人物表就不再读，剧本只补缺的。
/// overwrite=true：全部重来。
/// body: {project, overwrite?}
ApiResult post_story_understand(const nlohmann::json& body,
                                std::shared_ptr<llm::Client> client);

/// 「理解故事」那件活的任务体：读一遍 → 逐章写剧本 → 逐章拆分镜。
///
/// **导出是为了只有一份。** 两个入口跑同一件事：设定页那颗「理解故事」和
/// 项目页那颗「一键成片」。抄一份的坏法是不报错的那种——这里后来补的每
/// 一条（读完才记账、单子要现读、总数中途收一次）都得在两处各补一遍。
///
/// 已经在一个 job 里了才能调它：进度、取消、思考流都走传进来的
/// `JobProgress`。`need_read` / `book_fp` 由调用方按"正文变没变"算好。
void run_understand_work(const models::ProjectStore& store,
                         const std::shared_ptr<llm::Client>& client,
                         pipeline::JobProgress& p, bool overwrite,
                         bool need_read, const std::string& book_fp, int total);

/// POST /api/story/from_web —— 从网上找热点，写眼前这一章，直接落盘。
///
/// 用户 2026-09-18：故事页右下角那颗「点击直接让大语言模型使用 tools 从网上
/// 获取热门内容改写成一个完整的故事」，接着定「这次写的就只是这一章内容」。
/// 一条带工具的对话（stages/story_from_web），写完只换这一章的正文；这一章
/// 已经有字的要带 overwrite。
/// body: {project, chapter_id, overwrite?}。`get` 是上网那一层，主程序给真的。
ApiResult post_story_from_web(const nlohmann::json& body,
                              std::shared_ptr<llm::Client> client, llm::HttpGet get);

/// GET /api/script/series?path=&lane= —— **这部片子、这一道**上写作那一件的快照。
///
/// 形状和不带参数时一样（Write 那几个键），外加 `project` / `lane` 原样回去。
/// **不带 `path` 是老客户端**：回最近起的那一件（原来全机器只有一件）。
///
/// 页面上的按钮派活不带 `lane`，所以页面带着自己的片子来问，问到的就是自己
/// 按的那一件——而不是另一部片子正在写的（原来 B 的故事页上挂着 A 的进度条，
/// B 的「写这一章」也跟着灰掉）。
ApiResult get_write_status(const std::string& project, const std::string& lane);

/// POST /api/script/series/stop —— 停写作。body `{project?, lane?}`，回 `{stopped}`。
///
/// 带 `project`：只停**这部片子、这一道**（`lane` 不带就是页面按钮那一道）；
/// 再带 `all: true` 就是这部片子所有道上的。
/// 不带：老客户端——**恰好只有一件在写**时停它，好几件的话谁也不停（回
/// false），不能替人猜是哪一件。
ApiResult post_write_stop(const nlohmann::json& body);

}  // namespace changji::http
