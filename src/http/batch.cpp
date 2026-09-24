#include "util/say.hpp"
#include <algorithm>
#include "http/batch.hpp"

#include "http/story_api.hpp"
#include "config/runtime.hpp"
#include "llm/call_log.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "http/job_stream.hpp"
#include "http/episodes.hpp"
#include "http/planning.hpp"
#include "pipeline/activity.hpp"
#include "pipeline/task_board.hpp"
#include "stages/story_from_web.hpp"
#include "stages/web_tools.hpp"
#include "http/scripting.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/bible.hpp"
#include "models/story.hpp"
#include "http/ws.hpp"
#include "stages/chapter_write.hpp"
#include "stages/json_stream.hpp"
#include "stages/script.hpp"
#include "stages/story_plan.hpp"
#include "pipeline/storyboard_run.hpp"
#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(400, SAY("请求体要是一个对象"));
    for (const auto& kv : body.items()) {
        // **`stream` 一律放行。** 它是传输层的信封字段，不是业务字段：
        // 路由那一层（script_route / batch_route）拿它决定这件活挪不挪到
        // 后台、结果往哪条 WebSocket 送，处理函数多半根本不看它。
        //
        // 原来是各家自己往白名单里加，2026-09-14 栽了：给「照故事定妆」
        // 接上思考流之后前端开始发 stream，而 post_bible 的白名单里没有，
        // 一按就是 422 `Extra inputs are not permitted`。十几个处理函数
        // 挨个加，漏一个的表现就是那一步整个不能用。
        //
        // `async` 不在这儿放行是因为它在更上面就被 take_async 摘掉了
        // （见 server.cpp），到这儿本来就没有。
        if (kv.key() == "stream") continue;
        // **`lane` 同理**：谁派的（`pipeline::chat_lane`），任务表拿它分槽，
        // 处理函数自己不看。
        if (kv.key() == "lane") continue;
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(kv.key(), "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

/// 谁派的这一件（`pipeline::JobTable::start` 的 `lane`）。不带 = 页面上的按钮。
std::string lane_of(const json& body) {
    const auto it = body.find("lane");
    return it != body.end() && it->is_string() ? it->get<std::string>()
                                               : std::string();
}

/// 请求里写的那部片子，原样（判 409 用；键由任务表自己规范化）。
std::string project_of(const json& body) {
    const auto it = body.find("project");
    return it != body.end() && it->is_string() ? it->get<std::string>()
                                               : std::string();
}

/// **这一道上**是不是已经有一件在写。
///
/// 原来判的是"全机器有没有一件在写"，于是 A 片子在写正文，B 片子的对话
/// 派什么都 409（2026-09-24 用户撞到的那一次，就是这一句把 B 的场记骗成
/// 「卡住了」、转手把 A 停了）。现在只挡同一部片子、同一个派活的人。
bool lane_busy(const json& body) {
    return pipeline::jobs().running(pipeline::JobKind::Write, project_of(body),
                                    lane_of(body));
}

std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        // **input 是整个请求体，不是 null。** FastAPI 报缺字段时把父对象
        // 放进 input，前端拿它回显"你提交的是这些"。写 null 的话那一栏是空的。
        // 实时对拍抓出来的（写接口那一轮）。
        throw unprocessable_top(key, "Field required", body, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

double num_in_range(const json& body, const char* key, double def, double gt,
                    double le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", *it,
                                "float_type");
    }
    const double v = it->get<double>();
    // 边界值用 bound_text 印，不用 std::to_string——后者给六位小数，
    // 1800.0 会变成 "1800.000000"，而 pydantic 的消息里是 "1800"。
    // 而且要带 ctx，前端靠它填出"最大 1800"这种中文提示。
    if (v <= gt) {
        throw out_of_range(key, "Input should be greater than " + bound_text(gt),
                           *it, "greater_than", "gt", gt);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + bound_text(le), *it,
            "less_than_equal", "le", le);
    }
    return v;
}

int int_in_range(const json& body, const char* key, int def, int ge, int le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number_integer()) {
        throw unprocessable_top(key, "Input should be a valid integer", *it,
                                "int_type");
    }
    const int v = it->get<int>();
    if (v < ge) {
        throw out_of_range(
            key, "Input should be greater than or equal to " + std::to_string(ge),
            *it, "greater_than_equal", "ge", ge);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + std::to_string(le),
            *it, "less_than_equal", "le", le);
    }
    return v;
}

ProjectStore open_project(const json& body) {
    const std::string path = need_str(body, "project");
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

std::vector<std::string> character_names(const AssetLibrary& assets) {
    std::vector<std::string> names;
    for (const auto& kv : assets.characters) names.push_back(kv.second.name);
    return names;
}

double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

/// 这个错是不是"配错了"——换一件活重来还是一样。
///
/// ⚠️ **批量那几条都要问它一句。** 2026-09-17 实撞：一次补七章分镜，第三章
/// 开始密钥失效，而「一章砸了不拖垮后面几章」这条让它**又试了五章，每章
/// 同一个 401**——人白等二十分钟，回来看到五条一模一样的报错。
///
/// 那条规矩针对的是**内容**问题（这一章模型没写好，下一章换个剧本也许就
/// 好了）；而密钥不对、地址不对、模型名不存在，不会在下一章自己变好。
/// 限流（429）和 5xx 不算——那两种换个时间真会好，接着跑是对的。
bool hopeless(const std::exception& e) {
    const auto* le = dynamic_cast<const llm::LlmError*>(&e);
    return le != nullptr && le->is_config_error();
}

/// 整批停在这儿的那句话。**要说清"停了"和"还剩几件"**——不然人看到的是一
/// 条孤零零的报错，不知道后面那几件是跑过了还是压根没跑。
std::string stopped_because(const std::exception& e, std::size_t left) {
    std::string out = e.what();
    out += SAY("\n\n这一条换一件活重来也一样，所以整批停在这儿了");
    if (left > 0) out += SAYN("——还有 %n 件没做", left);
    out += SAY("。改完再按一次，做过的不会重做。");
    return out;
}


}  // namespace

ApiResult post_story_chapters(const json& body,
                              std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    // 409 在建 store 之前判，和 post_script_series 一个顺序：
    // 两个都错时回哪一个是可观测的。
    if (lane_busy(body)) {
        throw ApiError(409, SAY("已经在写了"));
    }
    ProjectStore store = open_project(body);
    // **回给界面的项目串，用客户端自己发来的那一份。**
    //
    // 不能用 `store.root()`：ProjectPaths 的构造会做 weakly_canonical
    // （见 project.cpp），而界面手里那串来自项目列表、不保证规范化过
    // ——macOS 上 /tmp 会解成 /private/tmp 这类符号链接，两串就对不上。
    // 下面那条流式正文要靠这串让界面认"这是不是我这部电影的"；对不上的话
    // 收到的正文会被整个丢掉，而且是静默的。
    const std::string client_project = need_str(body, "project");
    const Project project = load_or_400(store);

    Story story;
    try {
        story = store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
    if (story.chapters.empty()) {
        throw ApiError(400, SAY("还没有故事。先写一份大纲，或者粘一段进来"));
    }

    std::vector<std::string> todo;
    for (const auto& c : story.chapters) {
        if (overwrite || text::strip_ws(c.text).empty()) {
            todo.push_back(c.chapter_id);
        }
    }
    if (todo.empty()) throw ApiError(400, SAY("每一章都有正文了"));

    const models::StyleLine style = project.style_line;
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo, style, client_project](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // 推流式正文要用它。**按类订阅也收得到**：Hub 把 job_id 里第一个
            // '-' 之前的部分当类名（"write-a3f…" → "write"），而客户端只订
            // 得到类名——具体 id 从来不从任何接口暴露出去。
            const std::string job_id =
                p.job_id();
            // 思考流挂到这条 job 的频道上。**令牌借 p 那个，不能让
            // JobScope 自带一个**：循环查的是 `p.cancelled()`、给大模型的
            // 是 `p.token()`，自带那个没人读——顶栏「停下」按下去接口回
            // `{"stopped": true}`，活一秒没停。见 JobScope 第二个构造函数。
            const JobScope scope{job_id, p.token()};
            int done = 0;
            // **三个数，三件事**：`done` 是走完了几章（进度条），`wrote` 是真写成了几章（最后那句总结），`failed` 是砸了几章。
            int wrote = 0;
            int failed = 0;
            for (const auto& id : todo) {
                if (p.cancelled()) return;

                // **每一轮重读。** 上一章写完已经落库了，这一章的提示词里
                // 「上一章是这么结束的」要拿到刚写的那一份。用循环外那个
                // 快照的话，每一章都以为自己接的是空的上一章。
                Story cur = store.load_story();
                const Chapter* me = cur.chapter_by_id(id);
                if (me == nullptr) {
                    p.set_done(++done);
                    continue;
                }
                p.set_message(SAYF("正在写 %1（%2）", id, me->title));

                llm::Request req;
                try {
                    req = chapter_request(cur, id, style);
                } catch (const std::exception& e) {
                    p.add_episode(json{{"chapter_id", id}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }

                // **砸了就再要一次——但不是再掷一次骰子。**
                //
                // 2026-09-19 起：闸门打回的那一稿还在手里（stages::ChapterRejected
                // 带着解析好的稿和整份毛病清单），下一轮发的是「上一稿 + 要改的
                // 地方」（chapter_revision_request），让模型只改那几处。原来
                // 这儿是把同一份提示词再发一遍，chapter_write.cpp 里自己写着
                // 「整章作废，而重试只是再掷一次骰子」。手里没稿的那几种
                // （JSON 解析不动）照旧重掷。
                //
                // 最常见的砸法是模型把章节标题填进了正文字段，于是正文只有
                // 十几个字（守卫按目标篇幅的两成拦下来）。采样带随机种子，
                // 再要一次通常就对了——2026-09-11 实跑四章砸了两章，而这
                // 两章的失败彼此无关。
                //
                // **只有批量这条路重试。** 单章那个接口前面站着一个人，他
                // 看见报错自己会再按一下；批量这条是十六章无人看管地跑，
                // 中间掉两章的话，等他回来时故事里有两个空洞，而进度条上
                // 写的是"写完了 16 章"。
                //
                // 就多要两次，不是要到成功为止：提示词真有毛病时，重试到底
                // 只会把一次失败变成一小时失败。
                //
                // **最后一次把软闸关掉**（parse_chapter 的 strict=false）。
                // 软闸拦的是「能用但不够好」——整章没对白、两场撞车、章尾
                // 点题。一直硬拦的话 14B 连着写不对，那一章落成 **0 字**，
                // 而 0 字比「写得一般」差得多：2026-09-12 三轮实跑里软闸
                // 每轮清掉 1~3 章。最后一次收下它，软闸就只提分不清零。
                Story next;
                std::string last_error;
                bool last_hopeless = false;
                // 上一轮被打回的稿；空 = 上一轮没留下能改的稿（或者还没跑）。
                std::optional<stages::ChapterRejected> rejected;
                // **读→改→存在一把锁里**（`ProjectStore::lock`）：从下面重读
                // story.json 那一下拿住，存完、对齐完章节记录才放。别的对话同一
                // 部片子也在写别的章，不锁的话两边各读各存，后存的那份把前一章
                // 冲回旧样子，一声不响。**大模型那一两分钟不在锁里**——每一轮
                // 开头先放掉。
                std::unique_lock<std::recursive_mutex> story_guard;
                constexpr int kAttempts = 3;
                for (int attempt = 0; attempt < kAttempts; ++attempt) {
                    if (story_guard.owns_lock()) story_guard.unlock();
                    // 点了停就别再来一次了——重试的那一次一样会当场被取消，
                    // 白跑一趟提示词。
                    if (p.cancelled()) break;
                    const bool revising = attempt > 0 && rejected.has_value();
                    if (attempt > 0) {
                        // **说上一次到底砸在哪。**
                        //
                        // 这句原来写死是"只写出几个字"。那是最常见的一种砸
                        // 法，不是唯一一种：守卫会因为整章没对白、两场撞车、
                        // 正文复读、贴情绪标签打回，每一种 StoryError 都自带
                        // 一句能直接给人看的话（"整章几乎没有对白（65 段里
                        // 0 段有人说话）"）。写死那句的代价是，一章要跑五分
                        // 钟，人盯着看的那五分钟里读到的是个假理由——而真理由
                        // 就在手边的 last_error 里。
                        const std::string why =
                            last_error.empty()
                                ? SAY("只写出几个字")
                                : text::truncate_utf8(last_error, 80);
                        // 「改稿」和「重写」是两件事，人看着进度条要分得出：
                        // 前者是拿着清单改那几处，后者是从头再来。
                        p.set_message(
                            revising
                                ? SAYF("改稿 %1（%2）——上一稿%3", id, me->title, why)
                                : SAYF("重写 %1（%2）——上一稿%3", id, me->title, why));
                    }
                    // 闸门那一行要对回这一次调用，而打回那一支（catch）也要
                    // 用它，所以摆在 try 外头。`complete()` 自己抛了的时候
                    // （429、连不上）它还是空串，`note_chapter_gate` 于是整个
                    // 不动手——对面砸了不是闸门打回，那一次在 index.jsonl 里
                    // 已经记着了。
                    std::string call_id;
                    const bool strict = attempt + 1 < kAttempts;
                    try {
                        const int floor_chars = static_cast<int>(
                            stages::chapter_target_chars(cur) *
                            stages::kChapterMinRatio);
                        // **批量这条也边写边推。** 十六章要跑一个多小时，
                        // 进度条上只有"正在写 ch07"的话，那一个多小时里
                        // 看不到一个字。推的是从 token 流里抠出来的正文，
                        // JSON 外壳和后面那串 hooks 不推（见 json_stream）。
                        // **抠的是 paragraphs，不是 text。** c41821d 把章节正文从一个字符串
            // 改成了一段一项的数组，而这里没跟着改——于是流式一个字都抠
            // 不出来，界面上就是"AI 写作没有热更新"，后端不报任何错。
            stages::JsonFieldStreamer field(stages::kChapterBodyField, true);
                        int seq = 0;
                        // **写到哪儿了也挂到这件活的账上**（`Task::set_output`），
                        // 桌面端不订那条 "write" 频道，它看的是账本。前面带一行
                        // 章名，人看得出是哪一章。
                        std::string shown;
                        {
                            const Chapter* ch = cur.chapter_by_id(id);
                            shown = "【" + (ch == nullptr || ch->title.empty() ? id : ch->title) +
                                    "】\n";
                        }
                        pipeline::Activity* on_board = pipeline::current_activity();
                        // ⚠️ **令牌要给这个任务真正的那一个（p.token()）。**
                        //
                        // 这里原来是一个当场新建的 `CancelToken dummy`——
                        // 它永远不会被点亮，于是"停"这个按钮只在**章与章
                        // 之间**那一下才生效（循环头上那句 p.cancelled()）。
                        // 表现就是用户点了停，AI 还在哗哗地写，要等这一章
                        // 写完才停——32B 上那是好几分钟。
                        //
                        // JobProgress::token() 上面那段注释早就写清楚了：
                        // "sd.cpp 的采样、llama.cpp 的生成、ffmpeg 子进程
                        // 都要拿到令牌本身才能被打断"。批量这条漏了。
                        const llm::Request use =
                            revising ? chapter_revision_request(cur, id, style, *rejected)
                                     : req;
                        const std::string raw = client->complete(
                            use, p.token(), [&](const std::string& piece) {
                                const std::string fresh = field.feed(piece);
                                if (fresh.empty()) return;
                                shown += fresh;
                                if (on_board != nullptr) on_board->task().set_output(shown);
                                // **带上项目。** 这条流广播在 "write" 这个
                                // 槽上（客户端只订得到类名），而槽是全局
                                // 的一个：界面那头常驻一条连接收它。人在
                                // 批量跑着的时候去看另一部电影，收到的还是
                                // 这一部的正文——不说清是谁的，那边就会把
                                // 它画进别人的编辑器里。章号是 ch01 这种，
                                // 两部电影都有，光靠它分不出来。
                                ws::hub().broadcast(
                                    job_id, {{"type", "story_token"},
                                             {"job_id", job_id},
                                             {"project", client_project},
                                             {"chapter_id", id},
                                             {"seq", seq++},
                                             {"text", fresh}});
                            });
                        // **接在刚读回来的那一份上，不是循环开头那份。**
                        //
                        // 上面这次生成要跑一两分钟，而这一两分钟里用户完全
                        // 可能在编辑器里改别的章——那一页就是这么设计的：
                        // 一边看 AI 写，一边还能读能改，而改完 1.5 秒自动
                        // 保存直接落 story.json。拿循环开头那份快照整份写
                        // 回去，那些字就被悄悄吞掉了，不报错，人是过几分钟
                        // 翻回去才发现。
                        //
                        // 重读一次的代价是一个文件；换来的是"丢一章手写的
                        // 正文"这种没法补救的事不会发生。
                        call_id = llm::take_last_call_id();
                        story_guard = store.lock();
                        next = stages::apply_chapter(
                            store.load_story(), id,
                            stages::parse_chapter(raw, floor_chars, strict));
                        note_chapter_gate(call_id, id, attempt + 1, kAttempts,
                                          strict, nullptr, revising);
                        last_error.clear();
                        break;
                    } catch (const stages::ChapterRejected& e) {
                        note_chapter_gate(call_id, id, attempt + 1, kAttempts,
                                          strict, &e, revising);
                        last_error = e.what();
                        rejected = e;   // 稿子还在，下一轮改它
                        last_hopeless = false;
                    } catch (const std::exception& e) {
                        note_chapter_gate(call_id, id, attempt + 1, kAttempts,
                                          strict, &e, revising);
                        last_error = e.what();
                        rejected.reset();   // 没稿可改（或者对面砸了），下一轮重掷
                        // 配错了的话连这一章的两次重试都不用跑完。
                        last_hopeless = hopeless(e);
                        if (last_hopeless) break;
                    }
                    if (p.cancelled()) break;
                }
                if (!last_error.empty()) {
                    if (story_guard.owns_lock()) story_guard.unlock();
                    // 一章写砸了不该让前面几章白写，记下来接着往下写。
                    // **但配错了的除外**，见 hopeless：那种重来还是一样，
                    // 接着写只是让人多等十几分钟再看到同一句话。
                    p.add_episode(json{{"chapter_id", id},
                                       {"error", SAYF("%1（重试过两次）", last_error)}});
                    // ⚠️ **`done` 是「走完了几章」，不是「写成了几章」。**
                    // 它管的是进度条（第 n / 共 m），写砸的那一章也走完了，
                    // 所以照加。**可最后那句总结不能拿它说话**——2026-09-22
                    // 拿假模型实跑撞见的：三次全被闸门退回、正文一个字没有，
                    // 任务条上却写着「写完了 1 章」，而 error 是空的。
                    // 每一章的原因确实记在上面那张表里，可人先看见的是那一句。
                    ++failed;
                    p.set_done(++done);
                    if (last_hopeless) {
                        p.set_error(stopped_because(
                            std::runtime_error(last_error),
                            todo.size() - static_cast<std::size_t>(done)));
                        return;
                    }
                    continue;
                }

                next.plan = stages::plan_episodes(next, next.episode_duration_s);
                store.save_story(next);
                // 正文扩写完，这一章的梗概和它值多长都变了——章节记录跟着对齐。
                sync_episodes_to_chapters(store, next);
                if (story_guard.owns_lock()) story_guard.unlock();

                const Chapter* written = next.chapter_by_id(id);
                p.add_episode(json{
                    {"chapter_id", id},
                    {"title", written != nullptr ? written->title : std::string()},
                    {"chars", written != nullptr ? written->text_len() : 0},
                    {"episodes", next.plan.size()},
                });
                ++wrote;
                p.set_done(++done);
            }
            // **数的是真写成的那几章。** 砸了的单说一句，别混进来。
            std::string sum = SAYN("写完了 %n 章", wrote);
            if (failed > 0) sum += SAYN("，另有 %n 章没写成（原因在每一章那一行上）",
                                        failed);
            p.set_message(sum);
        },
        // **不是 `kWriteStoppedMessage`。** 那是写作槽的兜底，眼下和这一条
        // 同字——同字是巧合，展开的是章**正文**、不是剧本，两件事分开放
        // （jobs.hpp 里那一族的注释说了为什么）。
        pipeline::kStoryChaptersStoppedMessage,
        // 顶栏那块"AI 作业中"要靠它说清是哪部电影、点了往哪儿跳。
        paths::to_utf8(store.root()),
        // 任务页面那一行。**`JobKind::Write` 一个槽里跑着六件活**（这个文件里
        // 六处 start），不各自报名字的话那一行只会写「批量」。
        SAYN("写正文 · 还缺的 %n 章", static_cast<long long>(todo.size())),
        lane_of(body));

    if (!started) throw ApiError(409, SAY("已经在写了"));
    return {200, {{"started", true}, {"chapters", todo.size()}}};
}

ApiResult post_script_series(const json& body,
                             std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "premise", "episodes", "duration_s",
                        "reuse_characters"});
    const std::string premise = need_str(body, "premise");
    const int episodes = int_in_range(body, "episodes", 3, 1, 20);
    const double duration_s = num_in_range(body, "duration_s", 60.0, 0.0, 1800.0);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    // 409 要在**建 store 之前**判断吗？不。Python 那边是先判 running
    // 再 load_project，所以SAY("已经在写了")优先于"项目不存在"。照抄这个顺序：
    // 两个都错时回哪一个是可观测的。
    if (lane_busy(body)) {
        throw ApiError(409, SAY("已经在写了"));
    }
    ProjectStore store = open_project(body);
    load_or_400(store);   // 只为了验证项目能读，结果不用

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, premise, episodes, duration_s,
         reuse_chars](pipeline::JobProgress& p) {
            p.set_total(episodes);
            // **思考流挂到这条 job 的频道上。** 批量这几条是全流水线上跑得
            // 最久的（整部电影几十分钟），最需要"它到底在想还是卡死了"这个
            // 信号；而它们不走 start_async，所以要自己挂一次。
            // 停这一族仍然走 /api/script/series/stop（JobKind::Write 那个槽），
            // 不是按 stream——这条 job 本来就只有一个。
            // 令牌借 p 那个，理由同上面展开正文那条。
            const JobScope scope{p.job_id(),
                                 p.token()};

            // 梗概先存下来。下次打开界面时回填，不用凭记忆重打。
            {
            const auto store_guard = store.lock();
            Project first = store.load_project();
            const std::string trimmed = text::strip_ws(premise);
            if (first.premise != trimmed) {
                first.premise = text::truncate_utf8(trimmed, 2000);
                store.save_project(first);
            }
            }

            int done = 0;
            for (int i = 0; i < episodes; ++i) {
                if (p.cancelled()) return;

                Project project = store.load_project();
                const AssetLibrary assets = store.load_assets();
                const std::vector<std::string> names =
                    reuse_chars ? character_names(assets)
                                : std::vector<std::string>{};

                p.set_message(SAYF("正在写第 %1 章", std::to_string(i + 1)));

                // **四段的 schema，和单章那条走同一份。**
                //
                // 这儿原来用的是 `script_schema()`——**平的那份**，只有
                // minItems 4 的地板，没有时间结构。那正是 2026-09-12 之前
                // 的行为，也正是「60 秒的章写出 13 秒的剧本」的来源：
                // 提示词里说"要凑够"模型不听，minItems 写 4 它就写 4 拍。
                //
                // 2026-09-13 实跑这条路（三章，每章目标 60 秒）：
                //     ep01   7 行 /  3 句台词 / 293 字
                //     ep02  19 行 /  8 句台词 / 477 字
                //     ep03   3 行 /  1 句台词 / 122 字
                // 而同一天走单章那条出的是 17 拍 / 对白 171 字 /「合适」。
                // **当初改四段时漏了这一条路。**
                //
                // 形状每章重摇一个（random_shape，ComfyUI 的 randomize 那个
                // 意思）：写死比例的话整部电影每章都是同一个模子，连着看就露馅。
                //
                // **三处都要带上同一个 variation：提示词、schema、解析。**
                // 2026-09-14 发现提示词那一处漏了——它当时还不收这个参数，
                // 于是提示词里写着「开场钩子（0–5 秒）」（固定的 8%），
                // schema 里写的却是摇出来的秒数。模型照提示词写、我们照
                // schema 解析，两边差几秒，**而且不报错**。
                const std::uint32_t variation = stages::random_shape();

                llm::Request req;
                req.prompt = stages::build_script_prompt(
                    premise, duration_s, project.style_line,
                    // 前情收在 scripting.hpp 的 previous_scripts 里。
                    // **这儿原来自己抄了一份**，两份的取法还不一样：这份取
                    // "最近三章写好的"、不管在这一章前面还是后面。写全片时
                    // 两者恰好相等（这一章是写完才建出来的，见下面
                    // next_episode_id——所以"已经写好的"就是"这一章之前
                    // 的"），所以谁也没发现。终点留空就是全部已写的。
                    previous_scripts(project), names, variation);
                req.schema =
                    stages::script_schema(duration_s, names, variation);
                req.schema_name = "script";
                req.on_thinking = thinking_sink();

                stages::ScriptDraft draft;
                try {
                    // 令牌给这个任务真正的那一个，理由同上面写章节那处：
                    // 给 dummy 的话，"停"要等这一章写完才生效。
                    draft = stages::parse_script(client->complete(req, p.token()),
                                                 duration_s, variation);
                } catch (const std::exception& e) {
                    // 一章写砸了不该让前面几章白写，记下来接着往下写。
                    // episode_id 留空——这一章根本没建出来。
                    p.add_episode(json{{"episode_id", ""}, {"error", e.what()}});
                    p.set_done(++done);
                    // **配错了就别再试**，见 hopeless。
                    if (hopeless(e)) {
                        p.set_error(stopped_because(
                            e, static_cast<std::size_t>(episodes - done)));
                        return;
                    }
                    continue;
                }

                // **写回去之前重新读一份。**
                //
                // 上面那次生成跑了一两分钟，而 `project` 是那一两分钟**之前**
                // 读的。把它整份写回去，这期间界面上改的东西全被悄悄吞掉：
                // 镜头抽屉存的那一笔、改过的章名、手动加的一章、删掉的一章。
                // 同 post_story_chapters 和 post_plan_all 那两处，理由一样。
                //
                // 章号也要按新那份算：这几分钟里手动加过一章的话，照旧那份
                // 算出来的号会撞上它。
                const auto store_guard = store.lock();   // 读→改→存一把锁
                Project latest = store.load_project();
                const std::string episode_id = next_episode_id(latest);
                Episode ep;
                ep.episode_id = episode_id;
                ep.title = draft.title;
                ep.synopsis = draft.logline;
                ep.script = draft.render();
                ep.target_duration_s = duration_s;
                latest.episodes.push_back(std::move(ep));
                store.save_project(latest);

                p.add_episode(json{
                    {"episode_id", episode_id},
                    {"title", draft.title},
                    {"logline", draft.logline},
                    {"speakers", draft.speakers()},
                    {"dialogue_chars", draft.dialogue_chars()},
                });
                p.set_done(++done);
            }
            p.set_message(SAYN("写完了 %n 章", done));
        },
        // 收在 `pipeline/jobs.hpp`：那边改了措辞，这儿跟着走。
        pipeline::kScriptSeriesStoppedMessage,
        paths::to_utf8(store.root()), SAYN("写全片 · %n 章", episodes),
        lane_of(body));

    if (!started) throw ApiError(409, SAY("已经在写了"));
    return {200, {{"started", true}, {"total", episodes}}};
}

namespace {

// ---------------------------------------------------------------------------
// 「还缺什么」——「理解故事」和两颗批量按钮共用的那两条判据。
//
// **勾不勾「覆盖已有」决定一切**（用户 2026-09-19 定的）：
//
//   不勾 = 只补缺。没剧本的写剧本，没分镜的拆分镜，已经有的一个字不动。
//   勾上 = 全部重出。
//
// 中间没有第三档。这一天里先试过一版"正文改过的也算缺"（照正文指纹比），
// 用户当场否掉：那样人按下去之前说不出会动哪几章，而这颗按钮一按就是
// 几十分钟、还可能顶掉手改过的稿子。**可预测比聪明重要。**
//
// 剧本改过之后那张旧分镜表怎么办？不在这儿自动重拆——镜头页会挂个牌子说
// 「这张分镜是照旧剧本拆的」（见 Episode::shots_stale），要不要重拆由人定。
// 自动重拆会把手改过的台词、调过的顺序、锁住的镜头全冲掉。
//
// 三处共用一份，不是省几行：抄三份的话改一处、漏两处，表现是「同一颗
// 按钮在设定页说有活、在剧本页说没有」。
//
// **还有一条压在所有档位上面：没正文的章一步都不走。**
//
// 用户 2026-09-19：「我一个字没写的章节哪来的剧本和分镜」。他那部八章的
// 片子只写了第一章正文，而 ep02–ep08 各有三千多字剧本、七八十个镜头、
// 五分多钟的片长——全是这几颗批量按钮照**三五句章节梗概**扩写出来的
// （写剧本那一步没正文时退回用梗概，见 stages/script_story.cpp 里
// 【这一章】那一段）。人回到那一章，看见的是一份自己从没写过的戏。
//
// 所以判据前面加一条正面条件：**这一章的正文非空**。它压在 overwrite
// 上面——「覆盖已有」是"重出"的开关，不是"凭空写"的开关。
//
// 没挂章的那些（老项目、手动加的章、预告片）不受这条管：它们本来就没有
// 正文可照，照梗概写是它们唯一的走法，而 `chapter_refs.empty()` 早就把
// 它们挡在这两条判据外面了。
// ---------------------------------------------------------------------------

// 「这一章有正文吗」那一句收在 models::chapter_written（models/project.hpp）
// ——三处在问它，抄一份就漏一处，见那儿的注释。

/// 这一章还缺剧本吗。
bool script_missing(const Story& story, const Episode& ep, bool overwrite) {
    if (ep.chapter_refs.empty()) return false;   // 手动加的章、预告片：没有章可照
    // 没正文的章缺的是**正文**，不是剧本。见上面那一段。
    if (!chapter_written(story, ep)) return false;
    if (overwrite) return true;
    return text::strip_ws(ep.script).empty();
}

/// 这一章还缺分镜吗。
///
/// **判据是"有没有能用的镜头"，不是"shots 数组非空"**——空壳镜头
/// （shot_id 是空串）指不到任何文件、进不了任何一步，可数组非空就把这一章
/// 挡在补分镜之外。整段理由在 post_plan_all 那儿。
bool shots_missing(const Story& story, const Episode& ep, bool overwrite) {
    // 同上：挂着章而那一章还没正文的，不拆。
    //
    // **这一条要单独写在这儿**，不能指望"没正文就没剧本"替它挡住——盘上
    // 已经躺着的那几份梗概剧本（这条规矩之前写出来的）正是反例：它们有
    // 剧本、没正文，照样一拆就是七八十个镜头。
    //
    // 没挂章的（老项目、手动加的章、预告片）照旧放行：它们本来就没有正文
    // 可照，有剧本就能拆。
    if (!ep.chapter_refs.empty() && !chapter_written(story, ep)) return false;
    // 没剧本就拆不了分镜。「理解故事」那条路上这一章可能刚刚才写出剧本，
    // 所以那边要**写完再算一遍**，不能拿起跑前那份快照挑。
    if (text::strip_ws(ep.script).empty()) return false;
    if (overwrite) return true;
    return !std::any_of(ep.shots.begin(), ep.shots.end(), [](const Shot& s) {
        return !text::strip_ws(s.shot_id).empty();
    });
}

/// 这本书里还没写正文的章有几章。
///
/// 三颗按钮"没有要做的"时都报它，**三处报的是同一个数**：不报的话，一部
/// 只写了一章的片子按下去得到的是「都齐了」，而那正是用户那一下想干的事
/// 没干成的样子。按故事数不按章节记录数：还没落成章节的那几章同样是
/// "缺正文"，而话是同一句——先去故事页写。
int blank_chapter_count(const Story& story) {
    return static_cast<int>(story.chapters.size()) - story.written_chapters();
}

/// 这部电影的故事。读不了就回一份空的——老项目本来就没有 story.json。
///
/// 空故事底下 `chapter_written` 一律是 false，也就是挂着章的那几章一步都
/// 不走。**这是有意的**：挂着章说明正文本该在 story.json 里，而它读不出来
/// 的时候，"照它写"这件事本身就无从谈起。
Story story_or_empty(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception&) {
        return Story{};
    }
}

/// 按上面那条挑出要写剧本的那几章，**保持 project.episodes 的顺序**：
/// 剧本是一章接一章写的（要接上一章的结尾语气），乱序写出来的语气会断。
std::vector<std::string> episodes_missing_script(const Project& project,
                                                 const Story& story,
                                                 bool overwrite) {
    std::vector<std::string> todo;
    for (const Episode& ep : project.episodes) {
        if (script_missing(story, ep, overwrite)) todo.push_back(ep.episode_id);
    }
    return todo;
}

/// 同上，挑要拆分镜的那几章。
std::vector<std::string> episodes_missing_shots(const Project& project,
                                                const Story& story,
                                                bool overwrite) {
    std::vector<std::string> todo;
    for (const Episode& ep : project.episodes) {
        if (shots_missing(story, ep, overwrite)) todo.push_back(ep.episode_id);
    }
    return todo;
}

}  // namespace

/// 把所有挂着章、又还没有剧本的章一次改编完。
///
/// 用户 2026-09-16：「交互过程也太繁琐。」原来一章要点两下、等一轮：
/// 「改编成剧本」等两三分钟，回来点「采用」，下一章再来一遍——八章就是
/// 十六下点击、八段等待。而**旁边「批量补分镜」早就是一次点完**，
/// 缺的就是它前面这一步。
///
/// **不复制单章那条路的逻辑**：它要挑上一章的结尾接语气、要按章排场、
/// 章模式和老路线还分岔（见 post_script_write）。这儿原样调它，出来的稿子
/// 直接存（post_script）——批量就是"我不逐篇看了"，没有草稿态。
ApiResult post_script_all(const json& body, std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    if (lane_busy(body)) {
        throw ApiError(409, SAY("剧本那边还在忙"));
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = story_or_empty(store);

    // 挑哪几章：`script_missing` 那一条，和「理解故事」用的是同一份。
    const std::vector<std::string> todo =
        episodes_missing_script(project, story, overwrite);
    // **"没有要做的"不是错。**
    //
    // 这两颗批量按钮的用法就是"隔一阵按一下，把新写的章补上"，而按下去
    // 十有八九本来就没有漏的——原来这一下回 400，界面上弹一个带警告角标
    // 的红框，说的却是"一切正常，你不用做任何事"。用户 2026-09-16：
    // 「交互过程也太繁琐。」报错该留给真出事的时候。
    //
    // 回 200 + `started: false` + 空 `episodes`，让调用方自己挑话说。
    // **`blank_chapters` 一起带上**：没活干有两种，"都齐了"和"还有几章
    // 没正文"，而后者要说的话完全不一样（指回故事页）。不分的话，一部
    // 只写了一章的片子按下去得到的是「每一章都已经有剧本了」。
    // 跑起来那一条仍然是 202（它是异步的）；这一条没有活要干，就是 200。
    if (todo.empty()) {
        return {200,
                {{"started", false},
                 {"episodes", json::array()},
                 {"blank_chapters", blank_chapter_count(story)}}};
    }

    const std::string root = paths::to_utf8(store.root());
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo, root](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // 思考流和取消令牌都挂在这条 job 上，理由同 post_plan_all。
            const JobScope scope{
                p.job_id(), p.token()};
            int done = 0;
            for (const std::string& episode_id : todo) {
                if (p.cancelled()) return;
                p.set_message(SAYF("正在改编 %1", episode_id));
                try {
                    pipeline::CancelToken& tok = p.token();
                    json req = json::object();
                    req["project"] = root;
                    req["episode_id"] = episode_id;
                    // 老路线那一支要这个键才不报"缺 premise"；章模式用不上。
                    req["premise"] = "";
                    const ApiResult wrote = post_script_write(req, *client, tok);
                    if (wrote.status != 200 || !wrote.body.is_object()) continue;
                    const auto sit = wrote.body.find("script");
                    if (sit == wrote.body.end() || !sit->is_string()) continue;

                    json save = json::object();
                    save["project"] = root;
                    save["episode_id"] = episode_id;
                    save["script"] = *sit;
                    if (const auto lit = wrote.body.find("logline");
                        lit != wrote.body.end() && lit->is_string()) {
                        save["synopsis"] = *lit;
                    }
                    post_script(save, *client, tok);
                } catch (const std::exception&) {
                    // **一章砸了不拖垮整批。** 后面那几章照跑，跑完人回来看
                    // 哪几章还是空的——和 post_plan_all 那条一个做法。
                }
                p.set_done(++done);
            }
        },
        // 收在 `pipeline/jobs.hpp`，和另外两件写作活各是一条。
        pipeline::kScriptAllStoppedMessage, root,
        SAYN("写剧本 · 还缺的 %n 章", static_cast<long long>(todo.size())),
        lane_of(body));
    if (!started) throw ApiError(409, SAY("剧本那边还在忙"));
    return {202, {{"started", true}, {"episodes", todo}}};
}

/// 「理解故事」那件活的**任务体本身**。
///
/// **单拎出来是为了只有一份。** 两个入口跑同一件事：设定页那颗「理解故事」
/// （`post_story_understand`）和项目页那颗「一键成片」（http/oneclick.cpp）。
/// 抄一份的坏法是不报错的那种——这里后来补的每一条（读完那一趟才记账、
/// 单子要现读、总数要在中途收一次）都得在两处各补一遍，漏一处就是那一条
/// 静悄悄失效。
///
/// `need_read` / `book_fp` 由调用方按"正文变没变"算好（见
/// `post_story_understand` 里那一段），因为那几个判据还要用来决定这件活
/// 起不起得来。
void run_understand_work(const ProjectStore& store,
                         const std::shared_ptr<llm::Client>& client,
                         pipeline::JobProgress& p, bool overwrite,
                         bool need_read, const std::string& book_fp,
                         int total) {
    const std::string root = paths::to_utf8(store.root());
    p.set_total(total);
    pipeline::CancelToken& tok = p.token();
    int done = 0;

    // 读那一趟砸了就整件活砸：没有人物表就没有剧本里能指得到的人。
    // 让它抛，JobTable 会把那句话记到 error 上。
    if (need_read) {
        if (p.cancelled()) return;
        p.set_message(SAY("读故事：人物、关系、场景、长相"));
        post_story_understand_once(
            json{{"project", root}, {"overwrite", overwrite}}, *client, tok);
        p.set_done(++done);
    }

    // 章节对齐是存故事时自动做的（commit_story → sync）；这儿再对一次，
    // 老项目、手改过 project.json 的都兜住。
    try {
        post_story_episodes(json{{"project", root}});
    } catch (const std::exception&) {
    }

    // **读完那一趟才记账。** 记在 sync 之后：上面那句刚把章节记录
    // 写进 project.json，先记的话会被它拿着的旧副本盖回去
    // （同 commit_story 里那段「先把项目重读一遍」的坑）。
    if (need_read) {
        try {
            const auto store_guard = store.lock();   // 读→改→存一把锁
            Project fresh = store.load_project();
            fresh.understood_from = book_fp;
            store.save_project(fresh);
        } catch (const std::exception&) {
            // 记不上就是下次多读一遍，不值得让整件活砸在这儿。
        }
    }

    // **单子现读**：上面读那一趟会新落几条章节记录出来，拿起跑前
    // 那份算的话，新出来的章一条剧本都写不上。
    const Project after_read = store.load_project();
    // **故事也要重读一遍。** 上面那一趟把模型读出来的结构整个写回了
    // story.json，而挑章的判据问的是"这一章有没有正文"——拿起跑前
    // 那份问，问的是一本已经被换掉的书。
    const Story story_now = story_or_empty(store);
    const std::vector<std::string> to_write =
        episodes_missing_script(after_read, story_now, overwrite);
    {
        std::set<std::string> u(to_write.begin(), to_write.end());
        for (const std::string& id :
             episodes_missing_shots(after_read, story_now, overwrite)) {
            u.insert(id);
        }
        p.set_total((need_read ? 1 : 0) + static_cast<int>(to_write.size()) +
                    static_cast<int>(u.size()));
    }

    // 第二步：逐章写剧本。**和 script/all 同一个循环，同一条规矩**：
    // 一章写砸了不拖垮后面的，跳过去接着写。
    for (const std::string& episode_id : to_write) {
        if (p.cancelled()) return;
        const Project cur = store.load_project();
        const Episode* ep = cur.episode_by_id(episode_id);
        if (ep == nullptr) continue;   // 这期间被删了
        p.set_message(SAYF("写剧本 · %1", ep->chapter_refs.empty()
                                             ? episode_id
                                             : ep->chapter_refs.front()));
        try {
            json req = json::object();
            req["project"] = root;
            req["episode_id"] = episode_id;
            req["premise"] = "";
            const ApiResult wrote = post_script_write(req, *client, tok);
            if (wrote.status == 200 && wrote.body.is_object()) {
                const auto sit = wrote.body.find("script");
                if (sit != wrote.body.end() && sit->is_string()) {
                    json save = json::object();
                    save["project"] = root;
                    save["episode_id"] = episode_id;
                    save["script"] = *sit;
                    if (const auto lit = wrote.body.find("logline");
                        lit != wrote.body.end() && lit->is_string()) {
                        save["synopsis"] = *lit;
                    }
                    post_script(save, *client, tok);
                }
            }
        } catch (const std::exception&) {
        }
        p.set_done(++done);
    }

    // 第三步：逐章拆分镜。
    //
    // **单子在这儿才算得准**：刚写出剧本的那几章这一刻才够格
    // （`shots_missing` 要求有剧本），而写砸的那几章自动落在单子外
    // ——它们还是空剧本，拆不了。
    //
    // 一章一趟同步的 post_plan，和上面调 post_script_write 一个做法：
    // 它自己会在角色设定空着的时候先补一次、自己记 shots_from、
    // 自己只把这一章的镜头写回去。**不在这儿另抄一份。**
    const Project after_write = store.load_project();
    const std::vector<std::string> to_plan =
        episodes_missing_shots(after_write, story_or_empty(store), overwrite);
    // **总数在这儿收一次。** 上面那个是预估：它把"即将写出剧本的
    // 章"都算成要拆分镜，而写砸的章拆不了（`shots_missing` 要求有
    // 剧本）。不收的话进度条停在 3/5，看着像没跑完。
    p.set_total((need_read ? 1 : 0) + static_cast<int>(to_write.size()) +
                static_cast<int>(to_plan.size()));
    for (const std::string& episode_id : to_plan) {
        if (p.cancelled()) return;
        const Project cur = store.load_project();
        const Episode* ep = cur.episode_by_id(episode_id);
        if (ep == nullptr) continue;
        p.set_message(SAYF("拆分镜 · %1", ep->chapter_refs.empty()
                                             ? episode_id
                                             : ep->chapter_refs.front()));
        try {
            json req = json::object();
            req["project"] = root;
            req["episode_id"] = episode_id;
            req["script"] = ep->script;
            req["duration_s"] = ep->target_duration_s;
            post_plan(req, *client, tok);
        } catch (const std::exception&) {
            // 同上：一章拆砸了不拖垮后面几章。
        }
        p.set_done(++done);
    }
    p.set_message(SAY("理解完了"));
}

ApiResult post_story_understand(const json& body,
                                std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    if (lane_busy(body)) {
        throw ApiError(409, SAY("剧本那边还在忙"));
    }
    ProjectStore store = open_project(body);
    const Project before = load_or_400(store);
    Story story;
    try {
        story = store.load_story();
    } catch (const std::exception&) {
        throw ApiError(400, SAY("这个项目还没有故事。先去故事页写"));
    }
    if (story.written_chapters() == 0) {
        throw ApiError(400, SAY("故事还没有正文，没什么可理解的。先去故事页写"));
    }

    const std::string root = paths::to_utf8(store.root());
    const std::string book_fp = story_text_fingerprint(story);

    // 一件活，三步：**读一遍 → 补剧本 → 补分镜**。
    //
    // 勾不勾「覆盖已有」决定每一步是"补缺"还是"重出"，整段规矩在上面
    // `script_missing` / `shots_missing` 那儿。2026-09-19 之前这儿只有前两
    // 步，而且第二步的判据把"有剧本"当"做过了"——用户八章的片子按下去
    // 0 秒跑完报「理解完了」，那一下本该去补 ep04–ep08 的分镜。
    //
    // 读那一趟单独一条：**正文变过就重读**（用户当天定的）。它和另外两步
    // 的"只补缺"不一样，因为新写一章会带出新人物、新地点，不重读就永远
    // 进不了设定库——那正是这一步的"缺"。不勾时是**只补不顶**
    // （merge_assets），手改过的设定不会丢。指纹见
    // models::story_text_fingerprint：没正文的章不参与，加删空章不触发。
    const bool need_read = overwrite || story.characters.empty() ||
                           before.understood_from != book_fp;

    // 这儿先算一遍只为两件事：决定这件活要不要起，以及进度条上那个总数。
    // **真正跑的时候三步都要重算**——读那一趟会新落几条章节记录出来，
    // 补完剧本又会让那几章变成"缺分镜"。
    const std::vector<std::string> will_write =
        episodes_missing_script(before, story, overwrite);
    const std::vector<std::string> will_plan =
        episodes_missing_shots(before, story, overwrite);

    // **"没有要做的"不是错，但也不能一声不吭。**
    //
    // 照 post_script_all 那条的规矩回 200 + `started: false`，让页面自己挑
    // 话说。原来这儿照样起一件活、0 秒跑完报「理解完了」——从人那头看，
    // 和"这颗按钮坏了"一模一样。
    if (!need_read && will_write.empty() && will_plan.empty()) {
        return {200,
                {{"started", false},
                 {"total", 0},
                 {"blank_chapters", blank_chapter_count(story)}}};
    }

    // 剧本写出来的那几章接着就要拆分镜，所以两张单子会重叠——总数按并集算，
    // 不然进度条会在中途超过 100%。
    std::set<std::string> plan_union(will_plan.begin(), will_plan.end());
    for (const std::string& id : will_write) plan_union.insert(id);
    const int total = (need_read ? 1 : 0) + static_cast<int>(will_write.size()) +
                      static_cast<int>(plan_union.size());

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, overwrite, need_read, total,
         book_fp](pipeline::JobProgress& p) {
            const JobScope scope{
                p.job_id(), p.token()};
            run_understand_work(store, client, p, overwrite, need_read, book_fp,
                                total);
        },
        SAY("已手动停止。已经理解出来的留着。"), root,
        // 账上那一行要说清干什么（CLAUDE.md 第十条）。**「0 章」等于没说**
        // ——没正文的章不写剧本也不拆分镜之后，"只读一遍"是常事，而那一行
        // 原来印的是「理解故事 · 0 章」。
        plan_union.empty()
            ? SAY("理解故事 · 读一遍")
            : SAYF("理解故事 · %1 章", std::to_string(plan_union.size())),
        lane_of(body));
    if (!started) throw ApiError(409, SAY("剧本那边还在忙"));
    return {202, {{"started", true}, {"total", total}}};
}

namespace {

/// 「第 3 章」「第三章」这种默认名才算默认；「第二章 五年前那把伞」不算——
/// 后半截是人起的。
bool is_default_chapter_title(const std::string& title_in) {
    const std::string t = text::strip_ws(title_in);
    if (t.empty()) return true;
    const std::string head = SAY_NEVER("第");
    const std::string tail = SAY_NEVER("章");
    if (t.size() < head.size() + tail.size()) return false;
    if (t.compare(0, head.size(), head) != 0) return false;
    if (t.compare(t.size() - tail.size(), tail.size(), tail) != 0) return false;
    std::string mid = t.substr(head.size(), t.size() - head.size() - tail.size());
    // 中间只许数字、空格、汉字数字。
    //
    // ⚠️ **这一族和上面那两个「第」「章」是拿来认字的，不是拿来显示的**
    // ——包进 `SAY()` 就是让界面语言决定"认不认得出中文章节名"，而章节名
    // 是用户写在故事里的中文。一个字都不许动。
    static const char* kNumerals[] = {
        SAY_NEVER("一"), SAY_NEVER("二"), SAY_NEVER("三"), SAY_NEVER("四"),
        SAY_NEVER("五"), SAY_NEVER("六"), SAY_NEVER("七"), SAY_NEVER("八"),
        SAY_NEVER("九"), SAY_NEVER("十"), SAY_NEVER("百"), SAY_NEVER("零"),
        SAY_NEVER("两")};
    while (!mid.empty()) {
        if (mid[0] == ' ' || (mid[0] >= '0' && mid[0] <= '9')) {
            mid.erase(0, 1);
            continue;
        }
        bool hit = false;
        for (const char* n : kNumerals) {
            const std::size_t len = std::strlen(n);
            if (mid.compare(0, len, n) == 0) {
                mid.erase(0, len);
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }
    return true;
}

}  // namespace

ApiResult post_story_from_web(const json& body, std::shared_ptr<llm::Client> client,
                              llm::HttpGet get) {
    forbid_extra(body, {"project", "chapter_id", "overwrite"});
    std::string chapter_id;
    if (const auto it = body.find("chapter_id"); it != body.end() && it->is_string()) {
        chapter_id = text::strip_ws(it->get<std::string>());
    }
    if (chapter_id.empty()) throw ApiError(422, SAY("缺 chapter_id：写的是哪一章"));
    const bool overwrite = opt_bool(body, "overwrite", false);
    if (lane_busy(body)) {
        throw ApiError(409, SAY("剧本那边还在忙"));
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    Story story;
    try {
        story = store.load_story();
    } catch (const std::exception&) {
        throw ApiError(400, SAY("这个项目还没有故事"));
    }
    const Chapter* mine = story.chapter_by_id(chapter_id);
    if (mine == nullptr) throw ApiError(404, SAYF("没有这一章：%1", chapter_id));
    if (!text::strip_ws(mine->text).empty() && !overwrite) {
        throw ApiError(409, SAY("这一章已经有正文了，换掉的话带上 overwrite"));
    }

    const std::string root = paths::to_utf8(store.root());
    // **推流式正文要用界面手里那串，不是规范化过的 root。** 理由整段写在
    // 批量展开那头（post_story_chapters 里 `client_project` 上面）：macOS 上
    // /tmp 会解成 /private/tmp，两串字不一样而指的是同一个地方，对不上的话
    // 那一头把收到的正文整个丢掉——**而且是静默的**。
    const std::string client_project = need_str(body, "project");
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, get, root, client_project, chapter_id,
         project](pipeline::JobProgress& p) {
            // 推正文要用它。按类订阅也收得到：Hub 把第一个 '-' 之前的部分
            // 当类名（"write-a3f…" → "write"），而界面只订得到类名。
            const std::string job_id =
                p.job_id();
            const JobScope scope{job_id, p.token()};
            pipeline::CancelToken& tok = p.token();
            pipeline::Activity act{"story_web", root, "", SAYF("从网上热点写 %1", chapter_id)};
            const pipeline::CancelLink stop_here{tok, act};
            p.set_total(1);
            p.set_message(SAY("在看网上什么热"));

            // 故事现读：从按下去到开跑之间人可能改了别的章
            const Story before = store.load_story();
            stages::WebTools web;
            web.get = get;
            stages::WebStoryHooks hooks;
            hooks.on_step = [&p](const std::string& s) { p.set_message(s); };
            hooks.on_thinking = thinking_sink();
            // **这一条也边写边推**，和批量展开走同一条流、同一个形状，所以
            // 故事页那头一个字都不用改。2026-09-18 这颗按钮从"打开输入框"
            // 换成"上网看热点写这一章"之后，编辑器就再没动过——那条输入框
            // 走的是流式改稿，而这条一百多秒里什么都不给，末了整章一次性
            // 落库（用户 2026-09-19 报的）。
            int seq = 0;
            hooks.on_text = [&](const std::string& piece, bool restart) {
                if (restart) seq = 0;   // 新一轮从头写，编辑器照 seq==0 接
                ws::hub().broadcast(job_id,
                                    {{"type", "story_token"},
                                     {"job_id", job_id},
                                     {"project", client_project},
                                     {"chapter_id", chapter_id},
                                     {"seq", seq++},
                                     {"text", piece}});
            };
            const stages::WebChapter wc = stages::write_chapter_from_web(
                *client, web, project.style_line, before, chapter_id, tok, hooks);

            // 只动这一章：正文换掉，标题只在原来是"第 N 章"这种默认名时才换。
            Story next = store.load_story();
            Chapter* c = next.chapter_by_id(chapter_id);
            if (c == nullptr) throw ApiError(404, SAYF("写着写着这一章没了：%1", chapter_id));
            c->text = wc.text;
            if (is_default_chapter_title(c->title) && !wc.title.empty()) c->title = wc.title;
            if (next.premise.empty() && next.logline.empty() && !wc.source.empty()) {
                // 这部电影还没一句话的话，先拿来源顶上——理解故事那一步会重写它
                next.logline = wc.source;
            }
            commit_story(store, project, std::move(next), before);
            p.set_done(1);
            p.set_message(
                SAYF("写好了 %1 · %2 字", chapter_id,
                     std::to_string(text::utf8_len(wc.text)))
                + (wc.source.empty() ? std::string()
                                     : SAYF(" · 来自「%1」", wc.source)));
        },
        SAY("已手动停止。这一章没写完，原来的留着。"), root, SAYF("从网上热点写 %1", chapter_id),
        lane_of(body));
    if (!started) throw ApiError(409, SAY("剧本那边还在忙"));
    return {202, {{"started", true}, {"chapter_id", chapter_id}}};
}

ApiResult post_plan_all(const json& body, std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    if (lane_busy(body)) {
        // 和写全片不是同一句话。用户看到SAY("已经在写了")会去找哪里在写剧本，
        // 而实际情况是那个槽被别的事占着。
        throw ApiError(409, SAY("剧本那边还在忙"));
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = story_or_empty(store);

    // 挑哪几章：`shots_missing` 那一条，和「理解故事」、批量改编共用一份。
    //
    // **2026-09-19 之前这儿是自己抄的一份**（"有剧本 + 没有能用的镜头"），
    // 而那份里没有"这一章得有正文"——同一天给另外两条加上的那一条，这儿
    // 静悄悄地没跟上。抄一份的代价就是这个：改了两处、漏了一处，而漏掉的
    // 那一处正是用户按得最多的「批量补分镜」。
    //
    // 判据里那句"有没有能用的镜头"（不是"shots 数组非空"）的理由留在
    // `shots_missing` 上：空壳镜头（shot_id 是空串）指不到任何文件、进不了
    // 任何一步，可数组非空就把这一章挡在补分镜之外——人看到的是「没有要
    // 补的」，而那一章明明是空的，一键跑完整部电影也救不回来。
    const std::vector<std::string> todo =
        episodes_missing_shots(project, story, overwrite);
    // **"没有要做的"不是错。**
    //
    // 这两颗批量按钮的用法就是"隔一阵按一下，把新写的章补上"，而按下去
    // 十有八九本来就没有漏的——原来这一下回 400，界面上弹一个带警告角标
    // 的红框，说的却是"一切正常，你不用做任何事"。用户 2026-09-16：
    // 「交互过程也太繁琐。」报错该留给真出事的时候。
    //
    // 回 200 + `started: false` + 空 `episodes`，让调用方自己挑话说。
    // `blank_chapters` 同 post_script_all：没活干的两种要分得开。
    // 跑起来那一条仍然是 202（它是异步的）；这一条没有活要干，就是 200。
    if (todo.empty()) {
        return {200,
                {{"started", false},
                 {"episodes", json::array()},
                 {"blank_chapters", blank_chapter_count(story)}}};
    }

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // **思考流挂到这条 job 的频道上。** 批量这几条是全流水线上跑得
            // 最久的（整部电影几十分钟），最需要"它到底在想还是卡死了"这个
            // 信号；而它们不走 start_async，所以要自己挂一次。
            // 停这一族仍然走 /api/script/series/stop（JobKind::Write 那个槽），
            // 不是按 stream——这条 job 本来就只有一个。
            // 令牌借 p 那个，理由同上面展开正文那条。**这一条尤其要紧**：
            // 批量补分镜今天是页面上那两条链子里的一步（镜头页「跑完整部
            // 电影」的第二步、故事页「一键处理」的最后一步），按下去那颗
            // 按钮自己变成「停下」，而那颗「停下」调的正是
            // /api/script/series/stop。令牌不借 p 那个的话，它就是一张
            // 空头支票：屏幕上那件看着停了，这一章的设定还在往下出。
            const JobScope scope{p.job_id(),
                                 p.token()};
            int done = 0;
            for (const std::string& episode_id : todo) {
                if (p.cancelled()) return;
                p.set_message(SAYF("正在给 %1 出分镜", episode_id));

                Project project = store.load_project();
                AssetLibrary assets = store.load_assets();
                Episode* ep = project.episode_by_id(episode_id);
                if (ep == nullptr) continue;

                try {
                    // 令牌同上：新建一个永远不会点亮的话，点了停要等这一章
                    // 的设定整套出完才有反应。
                    pipeline::CancelToken& tok = p.token();
                    // 角色设定全片共用，第一次缺的时候补一次就够。
                    // 每章都重出的话，同一个角色前后长得不一样。
                    if (assets.characters.empty()) {
                        llm::Request breq;
                        breq.prompt = stages::build_bible_prompt(
                            ep->script, project.style_line);
                        breq.schema = stages::bible_schema();
                        breq.schema_name = "bible";
                        breq.on_thinking = thinking_sink();
                        AssetLibrary made = stages::parse_bible(
                            client->complete(breq, tok), project.style_line,
                            config::load_settings(store.root())
                                .video.aspect_ratio());
                        // **写回之前看一眼：这一分钟里别处可能已经补上了。**
                        // 出这份圣经要跑一趟大模型，而这期间人完全可能在设
                        // 定页按了「照故事定妆」。那就用人家那份——它是照整
                        // 个故事出的，比这儿照一章剧本出的全，而且人正看着
                        // 它。反过来拿这一份顶掉，人刚定完的角色当场全换。
                        const auto assets_guard = store.lock();   // 读→改→存一把锁
                        AssetLibrary latest = store.load_assets();
                        if (latest.characters.empty()) {
                            assets = std::move(made);
                            store.save_assets(assets);
                        } else {
                            assets = std::move(latest);
                        }
                    }

                    // 单镜的时长档位是这部电影的属性（[video].max_shot_s），
                    // 按项目那份设置算一遍再拆镜头——**只挂在这条线程上**，
                    // 不去改全局那份（见 stages::ScopedVideoLimits）。
                    const stages::ScopedVideoLimits limits_here{
                        config::video_limits_for(config::load_settings(store.root()))};
                    // 切场、拆镜、补台词、查覆盖、重编号、拉回时长都在
                    // run_storyboard 里，和 post_plan 是同一份。
                    pipeline::StoryboardRunOptions sb;
                    sb.script = ep->script;
                    sb.assets = assets;
                    sb.episode_id = episode_id;
                    sb.duration_s = ep->target_duration_s;
                    sb.on_thinking = thinking_sink();
                    sb.on_progress = [&p, &episode_id](const std::string& m) {
                        // 两头都翻过了，中间这个标点也得跟着换——
                        // 见 `http/run_deps.cpp` 里那段。
                        p.set_message(SAYF("%1：%2", episode_id, m));
                    };
                    ep->shots = pipeline::run_storyboard(sb, *client, tok).shots;

                    // **写回之前重读一遍。** 理由同上面写全片那处：拆一章
                    // 镜头要跑几分钟，而这几分钟里界面可能在改别的章的镜头、
                    // 改章名、加一章——`project` 是循环开头那份快照，整份
                    // 写回去就把那些改动吞了。只把这一章的镜头放进新读的
                    // 那一份。
                    //
                    // 这一章在这期间被删了的话就别写了：拿旧快照写回去
                    // 等于把它从坟里刨出来。
                    const auto store_guard = store.lock();   // 读→改→存一把锁
                    Project latest = store.load_project();
                    if (Episode* target = latest.episode_by_id(episode_id)) {
                        target->shots = ep->shots;
                        // 照哪一版剧本拆的，见 Episode::shots_from。
                        // **算在 `sb.script` 上**：那是这一趟真正照着
                        // 拆的那份。`target->script` 是刚重读回来的，
                        // 而拆一章要跑几分钟，这几分钟里人完全可能在
                        // 剧本页改过了——拿它算就会记成"照新的拆的"，
                        // 而这张表其实照的是旧的。
                        target->shots_from = chapter_text_fingerprint(sb.script);
                        store.save_project(latest);
                    }
                } catch (const std::exception& e) {
                    // 一章出错不拖垮后面几章。跑一晚上，早上发现第二章挂了
                    // 导致后面十章都没动，那这一晚上就白熬了。
                    p.add_episode(
                        json{{"episode_id", episode_id}, {"error", e.what()}});
                    p.set_done(++done);
                    // **配错了就别再试**，见 hopeless：2026-09-17 就是这儿
                    // 连着报了五章一模一样的 401。
                    if (hopeless(e)) {
                        p.set_error(stopped_because(
                            e, todo.size() - static_cast<std::size_t>(done)));
                        return;
                    }
                    continue;
                }

                // 按成片长度报，不按名义值加——见 stages::real_total_s。
                const double total = stages::real_total_s(ep->shots);
                p.add_episode(json{{"episode_id", episode_id},
                                   {"title", ep->title},
                                   {"shots", ep->shots.size()},
                                   {"duration_s", round1(total)}});
                p.set_done(++done);
            }
            p.set_message(SAYN("出完了 %n 章的分镜", done));
        },
        SAY("已手动停止。已经出好的分镜留着。"),
        paths::to_utf8(store.root()),
        SAYN("补分镜 · 还缺的 %n 章", static_cast<long long>(todo.size())),
        lane_of(body));

    if (!started) throw ApiError(409, SAY("剧本那边还在忙"));
    return {200, {{"started", true}, {"episodes", todo}}};
}

ApiResult get_write_status(const std::string& project, const std::string& lane) {
    if (project.empty()) {
        return {200, pipeline::jobs().snapshot(pipeline::JobKind::Write)};
    }
    json out = pipeline::jobs().snapshot(pipeline::JobKind::Write, project, lane);
    out["project"] = project;
    out["lane"] = lane;
    // 这一道最近那件的 id。WebSocket 上 "write" 那一类同时有好几件在推，
    // 页面拿它认"哪几条是我这一件的"。
    out["job_id"] = pipeline::jobs().job_id(pipeline::JobKind::Write, project, lane);
    return {200, out};
}

ApiResult post_write_stop(const json& body) {
    const std::string project =
        body.is_object() ? body.value("project", std::string{}) : std::string{};
    if (project.empty()) {
        if (pipeline::jobs().running_projects(pipeline::JobKind::Write).size() != 1) {
            return {200, {{"stopped", false}}};
        }
        return {200, {{"stopped", pipeline::jobs().cancel(pipeline::JobKind::Write)}}};
    }
    // `all`：这部片子**所有道**上的都停（桌面端那颗停：它不知道对话派的活
    // 落在哪一道上，人按下去的意思是"这部片子别写了"）。
    if (body.value("all", false)) {
        return {200, {{"stopped", pipeline::jobs().cancel_project(
                                      pipeline::JobKind::Write, project) > 0}}};
    }
    const std::string lane = body.value("lane", std::string{});
    return {200, {{"stopped", pipeline::jobs().cancel(pipeline::JobKind::Write,
                                                      project, lane)}}};
}

}  // namespace changji::http
