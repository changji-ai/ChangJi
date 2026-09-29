#include "http/oneclick.hpp"

#include <chrono>
#include <set>
#include <algorithm>
#include <string>
#include <map>
#include <thread>
#include <vector>

#include "config/settings.hpp"
#include "http/batch.hpp"
#include "http/job_stream.hpp"
#include "http/ref_gen.hpp"
#include "http/story_api.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/jobs.hpp"
#include "stages/script.hpp"
#include "stages/story_outline.hpp"
#include "util/human_time.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

ProjectStore open_project(const json& body) {
    const auto it = body.find("project");
    if (it == body.end() || !it->is_string() || it->get<std::string>().empty()) {
        throw ApiError(400, SAY("没有指定项目目录"));
    }
    return ProjectStore(paths::from_utf8(it->get<std::string>()));
}

Story story_or_empty(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception&) {
        return Story{};
    }
}

/// 这条链跑到哪一步了。**名字要说清干什么**（CLAUDE.md 第十条）：
/// 「第 3 步 / 共 6 步 · 理解这一章」，不是「批量写作」。
//
// **表里存中文原话，翻译推迟到用它的那几行。** 静态表就地 `SAY()` 会把
// 语言冻在静态初始化那一刻，见 `util/say.hpp` 里 `SAY_NOOP` 那段。
const char* const kSteps[] = {
    SAY_NOOP("出大纲（只要 1 章）"),
    SAY_NOOP("写这一章的正文"),
    SAY_NOOP("理解：人物、场景、长相、剧本"),
    SAY_NOOP("拆这一章的分镜"),
    SAY_NOOP("把缺的参考图画齐"),
    SAY_NOOP("出前 n 分钟"),
};
constexpr int kStepCount = 6;

void say(pipeline::JobProgress& p, int step, const std::string& extra = "") {
    std::string m = SAYF("第 %1 步 / 共 %2 步 · %3", std::to_string(step),
                         std::to_string(kStepCount), SAY(kSteps[step - 1]));
    if (!extra.empty()) m = SAYF("%1：%2", m, extra);
    p.set_message(m);
    p.set_done(step - 1);
}

}  // namespace

ApiResult post_oneclick(const json& body, std::shared_ptr<llm::Client> client,
                        const RunDeps& deps) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体须为 JSON 对象"));
    // 下面那几句 `body.value(...)` 类型不对是抛 type_error 的，落出去就是 500
    // 而不是一句人话。进门先挡（同 post_run）。
    for (const char* k : {"project", "premise", "keywords", "lane"}) {
        if (const auto it = body.find(k); it != body.end() && !it->is_string()) {
            throw ApiError(400, SAYF("%1 要是字符串", k));
        }
    }
    if (const auto it = body.find("preview_s"); it != body.end() && !it->is_number()) {
        throw ApiError(400, SAYF("%1 要是数字", "preview_s"));
    }
    ProjectStore store = open_project(body);
    Project project;
    try {
        project = store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }

    // ---- 三道闸，全在起活之前 ----
    //
    // **都要在这儿判。** 起了活再抛的话，那句话落进任务状态的 error 里，
    // 页面上是一条"失败的任务"，而人按下去那一刻什么提示都没有。
    //
    // **只挡这部片子、这个派活的人手上那一件**（`JobTable::start` 的 `lane`）。
    // 原来挡的是全机器：别的片子在写，这边一键成片按不下去。
    //
    // 出片那道闸拿掉了：它是 RunQueue 之前的东西（那时候第六步撞上别的片子
    // 在出片就是一个 409）。现在第二件出片进队列排着，轮到了自己开始。
    {
        const std::string lane = field_str(body, "lane");
        if (pipeline::jobs().running(pipeline::JobKind::Write,
                                     field_str(body, "project"), lane)) {
            throw ApiError(409, SAY("剧本那边还在忙"));
        }
    }
    // 已经写过正文的项目：第一步就会把整个故事换掉，那是人几个钟头的东西。
    refuse_to_clobber(story_or_empty(store), body);

    const std::string root = paths::to_utf8(store.root());
    std::string premise = text::strip_ws(field_str(body, "premise"));
    if (premise.empty()) premise = text::strip_ws(project.premise);
    const std::string keywords = field_str(body, "keywords");
    // 长度不给就用这部电影自己的设置——分镜页那颗按钮改的是同一个值。
    double preview_s = field_num(body, "preview_s", 0.0);
    if (!(preview_s > 0.0)) {
        preview_s = config::load_settings(store.root()).preview.seconds;
    }
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, root, premise, keywords, preview_s,
         deps](pipeline::JobProgress& p) {
            const JobScope scope{
                p.job_id(), p.token()};
            pipeline::CancelToken& tok = p.token();
            p.set_total(kStepCount);

            // ---- 1. 大纲，只要一章 ----
            say(p, 1);
            // `write_outline` 要一个能改的 store（它自己要落盘），而 lambda
            // 捕的那份是 const。拷一份出来，指的是同一个目录。
            ProjectStore st = store;
            Project pj = st.load_project();
            const Story before = story_or_empty(store);
            write_outline(st, pj, before, premise, StoryScale::SHORT,
                          keywords, /*stream_id=*/"", stages::random_shape(),
                          *client, tok, /*peek=*/false, /*pasted=*/"",
                          /*chapters=*/1);
            if (p.cancelled()) return;

            // ---- 2. 写这一章的正文 ----
            const Story after_outline = story_or_empty(store);
            if (after_outline.chapters.empty()) {
                throw std::runtime_error(
                    SAY("大纲里一章都没有，后面几步没得做"));
            }
            const std::string chapter_id = after_outline.chapters.front().chapter_id;
            say(p, 2, after_outline.chapters.front().title);
            post_story_chapter(json{{"project", root},
                                    {"chapter_id", chapter_id},
                                    {"overwrite", true}},
                               *client, tok);
            if (p.cancelled()) return;

            // ---- 3 + 4. 理解（含剧本）和分镜 ----
            //
            // **一件活，不是两件**：`run_understand_work` 里第二步逐章写
            // 剧本、第三步逐章拆分镜，正是这条链的第 3、4 步。在这儿再抄
            // 一遍循环的话，那边后来补的每一条（单子要现读、总数中途收一
            // 次）都得在两处各补一遍，漏一处就是那条静悄悄失效。
            //
            // 步骤号交给它那句 message 去说——它自己会写「写剧本 · 第一章」
            // 「拆分镜 · 第一章」，比这儿印一个笼统的"第 3 步"准。
            // **这一轮之前每一章的剧本和分镜长什么样**，记一个指纹。理解那一步逐章写
            // 剧本、拆分镜，**哪一章砸了只记一句、不抛**；而拿 overwrite 重来的老片子里
            // ep01 本来就挂着上一个故事的剧本和镜头——原来第 6 步挑「挂着这一章、有镜头
            // 能用」的那一章，挑中的正是那份老分镜，出的是上一个故事的片子，还报成功。
            std::map<std::string, std::string> board_before;
            for (const auto& ep : store.load_project().episodes) {
                board_before[ep.episode_id] = ep.script + "\n" + json(ep.shots).dump();
            }
            const auto rebuilt = [&board_before](const Episode& ep) {
                const auto it = board_before.find(ep.episode_id);
                return it == board_before.end() ||
                       it->second != ep.script + "\n" + json(ep.shots).dump();
            };

            say(p, 3);
            const Story now = story_or_empty(store);
            run_understand_work(store, client, p, /*overwrite=*/true,
                                /*need_read=*/true, story_text_fingerprint(now),
                                /*total=*/kStepCount);
            if (p.cancelled()) return;

            // 这几步是跑在同一个 JobProgress 上的，它把 total 改成了自己的
            // 那个数。收回来，不然下面两步的进度条读数是另一套刻度。
            p.set_total(kStepCount);

            // ---- 5. 把缺的参考图画齐 ----
            //
            // 出片那一件开头自己也会补画（`post_run`），这儿先画是为了进度条上
            // 这一步说得清「还剩几张」，而不是出片那一步卡着不动。
            say(p, 5);
            // **已经有一批在画（409）不算「这一步过了」**：原来接着就去出片，参考图还在
            // 画着——换图模型当场 400「缺参考图」，别的走没参考图的就出一章没长相的片子。
            // 这部片子自己那一批（人按了按钮、对话里起的）可能不包括这次新写出来的角色，
            // 所以等它画完再补一遍；别的片子那一批就等它空出来再来。最多来三遍。
            // 画、等、409 重来那一套和出片那一件开头是同一个（`ensure_refs`，ref_gen.hpp）。
            if (!ensure_refs(
                    root, p.lane(), [&p] { return p.cancelled(); },
                    [&p](const std::string& extra) { say(p, 5, extra); })) {
                return;
            }
            if (p.cancelled()) return;

            // ---- 6. 出前 n 分钟 ----
            //
            // **起起来就收手。** 出片在另一个槽（JobKind::Run），跑一个
            // 钟头；在这儿等的话「写」这个槽被占着，期间任何写作都 409。
            const Project final_project = store.load_project();
            // **挑刚写的那一章**（挂着这一章的那条记录），不是"第一个有镜头
            // 的"：片子里原来就有老章的话，原来挑中的是它——一键成片出的是
            // 一章老片子。判据也是正面的「有一镜能用」（CLAUDE.md 第四条），
            // 空壳章不算。挂不上的（老项目那种）才退回第一章能用的。
            std::string episode_id;
            // **只认这一轮重拆过的**（rebuilt）：没重拆成的那一章挂着的是老分镜。
            for (const auto& ep : final_project.episodes) {
                if (ep.has_usable_shots() && rebuilt(ep) &&
                    std::find(ep.chapter_refs.begin(), ep.chapter_refs.end(),
                              chapter_id) != ep.chapter_refs.end()) {
                    episode_id = ep.episode_id;
                    break;
                }
            }
            for (const auto& ep : final_project.episodes) {
                if (!episode_id.empty()) break;
                if (ep.has_usable_shots() && rebuilt(ep)) episode_id = ep.episode_id;
            }
            if (episode_id.empty()) {
                throw std::runtime_error(
                    SAY("一章分镜都没拆出来，出不了片。上面几步的提示里写着"
                        "卡在哪儿了"));
            }
            say(p, 6, util::human_time(preview_s));
            // **带上这一道**：这条链是哪条对话派的，出片那件也记在它名下——
            // 人在那条对话里说「停」，排着、跑着的片子要跟着停。
            post_run(json{{"project", root},
                          {"episode_id", episode_id},
                          {"preview_s", preview_s},
                          {"lane", p.lane()}},
                     deps);
            p.set_done(kStepCount);
            p.set_message(SAYF("前 %1 的片子在出了——跟着出片那条进度看",
                               util::human_time(preview_s)));
        },
        SAY("已手动停止。已经写出来的故事、剧本、分镜都留着。"), root,
        SAYF("一键成片 · 前 %1", util::human_time(preview_s)),
        field_str(body, "lane"));
    if (!started) throw ApiError(409, SAY("剧本那边还在忙"));
    return {202,
            {{"started", true},
             // 这一栏画在页面上，翻。
             {"steps", json::array({SAY(kSteps[0]), SAY(kSteps[1]),
                                    SAY(kSteps[2]), SAY(kSteps[3]),
                                    SAY(kSteps[4]), SAY(kSteps[5])})},
             {"preview_s", preview_s}}};
}

}  // namespace changji::http
