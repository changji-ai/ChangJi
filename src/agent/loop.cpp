#include "util/say.hpp"
#include "agent/loop.hpp"
#include "util/tool_slot.hpp"

#include <exception>
#include <utility>

#include "http/readonly.hpp"
#include "http/story_api.hpp"
#include "stages/prompts.inc.hpp"
#include "util/cancel_words.hpp"

using json = nlohmann::json;

namespace changji::agent {

namespace {

/// 说一句话：**落一条 + 回那句话**。
///
/// 做成一个出口是有理由的。原来四个 return 里只有两个落了 Turn，于是"模型
/// 调不通"那条路回了话却一条都没落——界面上停在「在想」，账本上那件活却已经
/// 结了，而日志里一个字都没有。2026-09-21 真连一次就撞上了。
/// 场记开口说一句，这一轮到此为止。`level` 见 `Turn::level`：出错收场的那几句
/// 要标出来，界面才画得出"这一轮砸了"。
std::string say(const LoopHooks& hooks, std::string text, std::string level = {}) {
    Turn t;
    t.role = "assistant";
    t.text = text;
    t.level = std::move(level);
    t.at = now_ms();
    if (hooks.on_turn) hooks.on_turn(t);
    return text;
}

}  // namespace

/// 工具名 → 界面上那一行在干什么。
///
/// **说清干什么**（CLAUDE.md 第十条）：一行「在跑工具」和二十行一模一样的
/// 「在跑工具」，那一栏等于没有。
///
/// ⚠️ **每一个工具都要有一句。** 2026-09-23 之前这儿只认九个「读」的，派活
/// 那几个全落到兜底，任务行上挂着「在跑 assets_understand（{}）」——而模型
/// 调 tasks_read 读到的正是这一行，转头就把 `assets_understand` 原样说给了人。
/// `test_agent.cpp` 里有一条拿 `tool_specs()` 挨个查的守卫。
///
/// ⚠️ **这一族是给人看的，所以翻。** 同一个文件里 `current_state()` 和
/// `to_messages()` 那几句是**给模型看的**——一个字都不许动，翻了就是让界面
/// 语言决定模型收到什么（`util/say.hpp` 头上那张表）。
std::string tool_label(const std::string& name, const std::string& args) {
    if (name == "project_state") return SAY("在看这部片子现在什么样");
    if (name == "list_projects") return SAY("在看有哪些片子");
    if (name == "create_project") return SAY("在建项目");
    if (name == "story_read") return SAY("在读故事");
    if (name == "script_read") return SAY("在读剧本");
    if (name == "shots_read") return SAY("在读分镜");
    if (name == "assets_read") return SAY("在看设定");
    if (name == "outputs_read") return SAY("在看出了哪些片");
    if (name == "tasks_read") return SAY("在看引擎在忙什么");
    if (name == "shot_edit") return SAY("在改一镜");
    if (name == "assets_set_reference") return SAY("在换参考图");
    if (name == "story_outline") return SAY("在写大纲");
    if (name == "story_write_chapters") return SAY("在写正文");
    if (name == "assets_understand") return SAY("在读故事、提人物和场景");
    if (name == "refs_make") return SAY("在画参考图");
    if (name == "script_write_all") return SAY("在写剧本");
    if (name == "storyboard_plan_all") return SAY("在拆镜头");
    if (name == "render_run") return SAY("在出片");
    if (name == "film_join") return SAY("在接成一部");
    if (name == "task_cancel") return SAY("在停一件活");
    if (name == "ask_user") return SAY("在问你");
    // 认不出的照实报名字，别编一句好听的。参数是空的（`{}`）就不挂那对括号：
    // 挂上也只是一对花括号，什么都没说。
    const bool no_args = args.find_first_not_of(" \t\r\n{}") == std::string::npos;
    return no_args ? SAYF("在跑 %1", name) : SAYF("在跑 %1（%2）", name, args);
}

/// ⚠️ **这一族是给模型看的，一个字都不许包进 `SAY()`。** 这几句话原样进
/// 提示词，翻了就是让界面语言决定模型收到什么——而这个项目里「改一句描述」
/// 和「改约束本身」的区别是有血的教训的（CLAUDE.md 第二条、第五条）。
std::string current_state(const ToolContext& ctx) {
    if (ctx.project.empty()) {
        return SAY_NEVER(
            "现在还没有项目——人要是说了想拍什么，先 create_project 建一部。");
    }
    try {
        const auto pr = http::get_project(ctx.project);
        if (pr.status < 200 || pr.status >= 300) return SAY_NEVER("项目读不出来。");
        json story;
        const auto sr = http::get_story(ctx.project);
        if (sr.status >= 200 && sr.status < 300) story = sr.body;
        return describe_project(pr.body, story);
    } catch (const std::exception& e) {
        // 状态读不出来**不是致命的**：模型照样可以先答话、或者自己调工具再问
        // 一遍。把这句实话交给它，比抛一个异常把整轮掐掉强。
        return std::string(SAY_NEVER("项目状态读不出来：")) + e.what();
    }
}

std::vector<llm::Message> build_messages(const std::string& state,
                                         const std::vector<Turn>& history,
                                         const std::string& here) {
    std::vector<llm::Message> msgs;

    llm::Message sys;
    sys.role = "system";
    sys.content = stages::prompt::agent::kSystem;
    // 状态贴在系统提示后面，**每轮都是新的那一份**。
    sys.content += SAY_NEVER("\n\n【这部片子现在什么样】\n");
    sys.content += state;
    // 人这会儿把话说在哪一章上。**摆在状态后面**：先让它知道盘上是什么样，
    // 再说人的眼睛落在哪儿。
    //
    // ⚠️ **这儿写的是 `ep01` 这个键，不是「第 1 章」。** 上面那张「各章：」
    // 表用的就是键，模型要从那儿学会该往 `episode_id` 里填什么；这一句换成
    // 中文说法的话它就得自己做一次翻译，而它可能翻错（见 chapter_word.hpp
    // 开头那段）。说给人听的那一头在界面上，不在这儿。
    //
    // 没指的时候整段不发，见 loop.hpp 上那段。
    if (!here.empty()) {
        sys.content += SAY_NEVER("\n\n【人这会儿在说哪一章】\n");
        sys.content += here;
        sys.content += SAY_NEVER(
            "\n他说「这一章」「改一下」而没点名是哪一章时，指的就是这一章。"
            "点了名的照他说的办。");
    }
    msgs.push_back(std::move(sys));

    for (const auto& t : history) {
        llm::Message m;
        if (t.role == "user") {
            m.role = "user";
            m.content = t.text;
        } else if (t.role == "assistant") {
            m.role = "assistant";
            m.content = t.text;
            if (t.tool_calls.is_array()) {
                for (const auto& c : t.tool_calls) {
                    llm::ToolCall tc;
                    tc.id = c.value("id", "");
                    tc.name = c.value("name", "");
                    tc.arguments = c.value("arguments", "");
                    m.tool_calls.push_back(std::move(tc));
                }
            }
        } else if (t.role == "tool") {
            m.role = "tool";
            m.tool_call_id = t.tool_id;
            m.content = t.text;
        } else {
            // 引擎自己插的话（「出片做完了」）**当 user 发**。
            //
            // 各家对 system 出现在对话中段的处理不一样，有的直接忽略；
            // 而这句话恰恰是要模型接着往下做的依据，忽略了它就会停在那儿
            // 等一个永远不来的消息。
            m.role = "user";
            m.content = std::string(SAY_NEVER("【引擎】")) + t.text;
        }
        msgs.push_back(std::move(m));
    }
    return msgs;
}

std::string run_turn(llm::Client& client, ToolContext& ctx,
                     const std::vector<Turn>& history, pipeline::CancelToken& tok,
                     const LoopHooks& hooks, int max_rounds) {
    std::vector<llm::Message> msgs =
        build_messages(current_state(ctx), history, ctx.here);

    llm::Request opts;
    opts.schema_name = "agent";   // 挑模型和温度按这个名字分流
    opts.on_thinking = hooks.on_thinking;
    opts.on_token = hooks.on_delta;

    const auto specs = tool_specs();

    for (int round = 0; round < max_rounds; ++round) {
        if (tok.cancelled()) return say(hooks, util::kCancelled);

        llm::ChatReply r;
        try {
            r = client.chat(msgs, specs, opts, tok);
        } catch (const std::exception& e) {
            // **人按的停不是出错。** 停是在这一句里打断的（上面那个
            // `tok.cancelled()` 只在每一轮开头查一次，一轮里等着模型吐字的
            // 那几十秒全在这条 catch 底下），于是取消也从这儿出来——包成
            // 「大模型那头出错了：已取消」的话，界面上人自己按的那一下变成
            // 一句报错。收在 util/cancel_words.hpp 那一处，两边一个判据。
            if (tok.cancelled() || util::is_cancel_word(e.what()))
                return say(hooks, util::kCancelled);
            // **不抛。** 模型调不通是这条对话里的一件事，界面上该看得见这句
            // 话；抛出去的话它变成一个红框，而红框里说不清刚才做到哪儿了。
            return say(hooks, SAYF("大模型那头出错了：%1", e.what()), "error");
        }

        if (r.tool_calls.empty()) {
            // 开口说话了，这一轮到此为止。
            return say(hooks, r.content);
        }

        // 要调工具：先把它这一步落一条，再逐个跑。
        {
            Turn t;
            t.role = "assistant";
            t.text = r.content;
            t.at = now_ms();
            t.tool_calls = json::array();
            for (const auto& c : r.tool_calls) {
                t.tool_calls.push_back({{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
            }
            if (hooks.on_turn) hooks.on_turn(t);

            llm::Message a;
            a.role = "assistant";
            a.content = r.content;
            a.tool_calls = r.tool_calls;
            msgs.push_back(std::move(a));
        }

        for (const auto& c : r.tool_calls) {
            if (tok.cancelled()) return say(hooks, util::kCancelled);
            if (hooks.on_tool) {
                hooks.on_tool(tool_label(c.name, c.arguments), c.name, c.arguments);
            }

            std::string out = run_tool(ctx, c.name, c.arguments);

            // **`ask_user` 是这一轮的终点。**
            //
            // 它回的那句话前面挂着一个记号（`kAskUserMark`）。不认这个记号
            // 的话，模型收到工具结果会接着自己往下猜——而它本来就是因为
            // 猜不准才问的。
            const std::string mark(kAskUserMark);
            if (out.rfind(mark, 0) == 0) {
                const std::string question = out.substr(mark.size());
                // 这一条还是要落：assistant 说了要调工具，就得有个 tool 回它，
                // 不然下一轮发出去的那一串里少一半，有的服务直接 400。
                Turn asked;
                asked.role = "tool";
                asked.tool_name = c.name;
                asked.tool_id = c.id;
                // **别把问题原样再写一遍。** 紧接着那一条 assistant 就是这句
                // 问话，两条挨着摆在界面上就是同一句话说两遍（2026-09-21
                // 真跑一轮截出来的）。这一条存在的理由是"assistant 说了要调
                // 工具就得有个 tool 回它"，不是为了复述内容。
                asked.text = SAY("（问了人）");
                asked.at = now_ms();
                if (hooks.on_turn) hooks.on_turn(asked);

                llm::Message m;
                m.role = "tool";
                m.tool_call_id = c.id;
                m.content = asked.text;
                msgs.push_back(std::move(m));

                return say(hooks, question);
            }

            Turn t;
            t.role = "tool";
            t.tool_name = c.name;
            t.tool_id = c.id;
            t.text = out;
            // 这一次动的是哪一章。**给"画布跟着对话走"用**：代理刚拆完第 3
            // 章的分镜，而人眼前那面墙还停在第 1 章。args 是模型填的，
            // 抠不出来就是空的（`episode_of_args` 不抛，理由在它头上）。
            t.episode = util::episode_of_args(c.arguments);
            // 这一次带回来的图、片、改动（见 `ToolContext::media`）。**收走**，
            // 不收的话下一个工具会把这一份也扛上。
            t.media = std::exchange(ctx.media, json::array());
            // 没做成就标出来（见 `ToolContext::failed`）。
            t.level = std::exchange(ctx.failed, false) ? "error" : "";
            t.at = now_ms();
            if (hooks.on_turn) hooks.on_turn(t);

            llm::Message m;
            m.role = "tool";
            m.tool_call_id = c.id;
            m.content = out;
            msgs.push_back(std::move(m));
        }
    }

    // 转满了还在调工具。**照实说**，别装作做完了。
    return say(hooks, SAYF("我连着查了 %1 轮还没想清楚，先停在这儿。"
                           "你说得再具体一点？",
                           std::to_string(max_rounds)),
               "warn");
}

}  // namespace changji::agent
