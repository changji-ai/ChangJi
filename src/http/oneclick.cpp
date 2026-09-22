#include "http/oneclick.hpp"

#include <chrono>
#include <set>
#include <string>
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

/// 等这一批参考图画完。
///
/// **问引擎那本队列**（ref_gen.cpp 的 RefQueue），不是定时器猜——它自己
/// 知道还剩几张。和 EpShots 里 `waitRefs` 同一条规矩：读砸了不当成画完了，
/// 当成画完了的话下一步会在图还没出齐时发出去，又被那道闸挡回来。
///
/// 返回假 = 人按了停。
bool wait_refs(const std::string& root, pipeline::JobProgress& p) {
    for (;;) {
        if (p.cancelled()) return false;
        try {
            const ApiResult r = get_references_queue(root);
            if (!r.body.value("active", false)) return true;
            const int done = r.body.value("done", 0);
            const int total = r.body.value("total", 0);
            if (total > 0) {
                say(p, 5,
                    SAYF("还剩 %1 张（共 %2）", std::to_string(total - done),
                         std::to_string(total)));
            }
        } catch (const std::exception&) {
            // 引擎自己打嗝那几拍。不当成画完了。
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
}

}  // namespace

ApiResult post_oneclick(const json& body, std::shared_ptr<llm::Client> client,
                        const RunDeps& deps) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体要是一个对象"));
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
    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        throw ApiError(409, SAY("剧本那边还在忙"));
    }
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, SAY("已经在出片了。等这一轮跑完再按"));
    }
    // 已经写过正文的项目：第一步就会把整个故事换掉，那是人几个钟头的东西。
    refuse_to_clobber(story_or_empty(store), body);

    const std::string root = paths::to_utf8(store.root());
    std::string premise = text::strip_ws(body.value("premise", std::string{}));
    if (premise.empty()) premise = text::strip_ws(project.premise);
    const std::string keywords = body.value("keywords", std::string{});
    // 长度不给就用这部电影自己的设置——分镜页那颗按钮改的是同一个值。
    double preview_s = body.value("preview_s", 0.0);
    if (!(preview_s > 0.0)) {
        preview_s = config::load_settings(store.root()).preview.seconds;
    }
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, root, premise, keywords, preview_s,
         deps](pipeline::JobProgress& p) {
            const JobScope scope{
                pipeline::jobs().job_id(pipeline::JobKind::Write), p.token()};
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
            // 不画的话第 6 步会被 400 挡回来，而那句话是"去设定页按两个
            // 按钮再回来"——一颗一键把人支去别的页面，正是要治的那个形状。
            say(p, 5);
            try {
                const ApiResult r = post_references_generate_all(
                    json{{"project", root}, {"force", false}});
                if (r.body.value("started", false)) {
                    if (!wait_refs(root, p)) return;
                }
            } catch (const std::exception& e) {
                // 画不出来不在这儿断：出片那道闸会把"哪几镜拿不到参考图"
                // 说得比这儿准（它按真要跑的那几镜判）。
                p.set_message(
                    SAYF("参考图这一步没跑成：%1。接着试出片", e.what()));
            }
            if (p.cancelled()) return;

            // ---- 6. 出前 n 分钟 ----
            //
            // **起起来就收手。** 出片在另一个槽（JobKind::Run），跑一个
            // 钟头；在这儿等的话「写」这个槽被占着，期间任何写作都 409。
            const Project final_project = store.load_project();
            std::string episode_id;
            for (const auto& ep : final_project.episodes) {
                if (!ep.shots.empty()) {
                    episode_id = ep.episode_id;
                    break;
                }
            }
            if (episode_id.empty()) {
                throw std::runtime_error(
                    SAY("一章分镜都没拆出来，出不了片。上面几步的提示里写着"
                        "卡在哪儿了"));
            }
            say(p, 6, util::human_time(preview_s));
            post_run(json{{"project", root},
                          {"episode_id", episode_id},
                          {"preview_s", preview_s}},
                     deps);
            p.set_done(kStepCount);
            p.set_message(SAYF("前 %1 的片子在出了——跟着出片那条进度看",
                               util::human_time(preview_s)));
        },
        SAY("已手动停止。已经写出来的故事、剧本、分镜都留着。"), root,
        SAYF("一键成片 · 前 %1", util::human_time(preview_s)));
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
