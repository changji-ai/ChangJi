#include "http/story_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/runtime.hpp"
#include "config/settings.hpp"
#include "llm/call_log.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/chapter_write.hpp"
#include "stages/script.hpp"   // random_shape
#include "stages/story_analyze.hpp"
#include "stages/story_understand.hpp"
#include "stages/bible.hpp"
#include "http/planning.hpp"
#include "stages/story_import.hpp"
#include "stages/story_outline.hpp"
#include "stages/story_plan.hpp"
#include "stages/story_reverse.hpp"
#include "http/job_stream.hpp"
#include "http/offload.hpp"
#include "http/prompt_peek.hpp"
#include "pipeline/activity.hpp"
#include "stages/json_partial.hpp"
#include "stages/json_stream.hpp"
#include "stages/story_revise.hpp"
#include "util/cancel_words.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace changji::http {

namespace {

using namespace changji::models;

/// 校验请求体里没有多余字段。和 scripting.cpp 的同名函数一致：
/// 对应 pydantic 的 extra="forbid"，**422 不是 400**。
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
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(kv.key(), "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        throw unprocessable_top(key, "Field required", body, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

std::string opt_str(const json& body, const char* key,
                    const std::string& def = "") {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
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

/// 体量。**不能直接 get<StoryScale>()**：NLOHMANN_JSON_SERIALIZE_ENUM
/// 生成的 from_json 遇到不认识的值会**悄悄退回第一项**（也就是 short），
/// 前端拼错一个字母就变成写短篇，而且不报错。
StoryScale opt_scale(const json& body, const char* key, StoryScale def) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    const std::string v = it->get<std::string>();
    if (v == "short") return StoryScale::SHORT;
    if (v == "medium") return StoryScale::MEDIUM;
    if (v == "long") return StoryScale::LONG;
    throw unprocessable_top(key, "Input should be 'short', 'medium' or 'long'",
                            *it, "enum");
}

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

ProjectStore open_project(const json& body) {
    return open_project(need_str(body, "project"));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

Story load_story_or_400(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 回给前端的形状。故事本体之外多带几个数，免得每个页面各数一遍。
json story_response(const Story& story) {
    return json{
        {"story", json(story)},
        {"chapters", story.chapters.size()},
        {"written", story.written_chapters()},
        {"episodes", story.plan.size()},
        {"empty", story.empty()},
    };
}

/// 落库前统一走一遍校验。坏数据进了 story.json，下次打开这个项目就废了。
///
/// 回 400 而不是 422：422 那条路的 detail 是 pydantic 形状的结构化数组，
/// 前端按那个形状去高亮对应的输入框。这里的错是「故事内部对不上」
/// （关系指向不存在的人、章节计划指向不存在的章），没有哪个输入框对得上，
/// 挂成 422 前端只会显示成 [object Object]。
void validate_or_400(const Story& story) {
    const auto errs = story.validate();
    if (errs.empty()) return;
    std::string msg = SAY("这份故事里有对不上的地方：");
    for (std::size_t i = 0; i < errs.size(); ++i) {
        // 两头的话都翻了，中间这个标点也得跟着换。
        msg += (i == 0 ? std::string{} : SAY("；"));
        msg += errs[i];
    }
    throw ApiError(400, msg);
}

/// 哪个项目上正跑着一份大纲（项目目录 → stream_id）。
///
/// **为什么要这本账。** 出一份大纲要一分多钟，而人会在这一分钟里刷新
/// （用户 2026-09-13 实测：点下去 8 秒就刷新了）。刷新之后浏览器里那条
/// socket 没了，页面不知道后台还有活在跑，于是一片空白——草稿要等写完才
/// 落盘，那之前 GET /api/story 什么都带不回来。有了这本账，页面一进来就
/// 知道"有一轮在跑、听哪条流"，把「正在写」的板子摆回去，写完了照样能拿到
/// 结果。
///
/// 只记 stream_id，不记别的：进度本身走 ws，这里只负责"接头"。
class OutlineRegistry {
public:
    static OutlineRegistry& instance() {
        static OutlineRegistry r;
        return r;
    }
    void started(const std::string& project, const std::string& stream) {
        std::lock_guard<std::mutex> g(mu_);
        running_[key(project)] = stream;
        // 新的一轮起了，上一轮的错就过期了——不然写成之后页面还弹一句旧错。
        errors_.erase(key(project));
    }
    /// 只擦自己那一笔：同一个项目上要是又起了一轮（旧的还没退干净），
    /// 旧的收尾不能把新的那一笔擦掉。
    void finished(const std::string& project, const std::string& stream) {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = running_.find(key(project));
        if (it != running_.end() && it->second == stream) running_.erase(it);
    }
    /// 写砸了。**必须记下来。** job_error 只往那条流上广播一次，页面要是
    /// 已经刷新过（这正是用户会做的事），那句话就没人听见——盘上没草稿、
    /// 账也擦了，页面上一片空白，连"为什么"都没有。2026-09-13 实测就是
    /// 这样："还是不行"。
    void failed(const std::string& project, const std::string& message) {
        std::lock_guard<std::mutex> g(mu_);
        errors_[key(project)] = message;
    }
    std::string running_for(const std::string& project) {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = running_.find(key(project));
        return it == running_.end() ? std::string() : it->second;
    }
    /// 上一轮的错。**不清**：用户两台设备同时开着，"读一次就清"只有先
    /// 问到的那台看得见。留到下一轮 started() 或草稿被采用/丢弃再清；
    /// 页面自己记着上次弹过哪句，同一句不弹第二次。
    std::string error_for(const std::string& project) {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = errors_.find(key(project));
        return it == errors_.end() ? std::string() : it->second;
    }
    void clear_error(const std::string& project) {
        std::lock_guard<std::mutex> g(mu_);
        errors_.erase(key(project));
    }

private:
    /// **按真实路径记，不按字符串。** 同一个项目在这台机器上有两种写法
    /// （`AppData\Local\changji\…` 和它在 `Packages\…\LocalCache\Local\`
    /// 下的镜像，Windows 的应用容器干的），两台设备各用一种；按字符串记的话
    /// 一台设备起的那一轮另一台永远查不到。解不出真实路径就退回原字符串。
    static std::string key(const std::string& project) {
        std::error_code ec;
        const auto canon = fs::weakly_canonical(paths::from_utf8(project), ec);
        return ec ? project : paths::to_utf8(canon);
    }

    std::mutex mu_;
    std::map<std::string, std::string> running_;
    std::map<std::string, std::string> errors_;
};

}  // namespace

/// 已经展开过正文的故事被一份新大纲整份顶掉，那些正文就没了。
/// 章节 id 是按位置生成的，没法靠合并保住——只能挡在这里。
///
/// **出大纲那条要在开跑之前挡**：大纲直接落盘（见 write_outline 上那段），
/// 八分钟之后再 409 等于白跑一趟模型。
void refuse_to_clobber(const Story& existing, const json& body) {
    const int written = existing.written_chapters();
    if (written > 0 && !opt_bool(body, "overwrite", false)) {
        throw ApiError(409,
                       SAYN("这个项目里已经有 %n 章写好了正文，换一份大纲会把"
                            "它们顶掉。确认要换的话带上 overwrite。",
                            written));
    }
}

/// 把一份故事**落成正式的那份**：算章节计划、存盘、章节对齐、梗概同步回
/// project.json。
///
/// 出大纲、粘一份、提人物、采用——四条路的收尾是同一段。2026-09-17 之前
/// 前三条只回"草稿"，人点了「采用」才走到这儿；草稿那一屏当天退役（理由
/// 在 write_outline 上），它们直接落盘。四处各写一遍的话迟早只改一边。
json commit_story(const ProjectStore& store, Project project, Story story,
                  const Story& existing) {
    if (story.episode_duration_s <= 0.0) {
        story.episode_duration_s = existing.episode_duration_s;
    }
    story.plan = stages::plan_episodes(story, story.episode_duration_s);
    validate_or_400(story);
    store.save_story(story);
    // **章节动了，章节记录跟着对齐。** 一章一条是机械映射，用户 2026-09-16
    // 选的是「自动做，不要按钮」。
    sync_episodes_to_chapters(store, story);
    // 上一轮写砸的那句话到此为止：新的一份已经进去了。
    OutlineRegistry::instance().clear_error(paths::to_utf8(store.root()));
    // **梗概要写回 project.json 的话，先把项目重读一遍。** 上面那句 sync 刚
    // 把章节记录写进去了，拿进来时那份旧的盖回去，章节记录就没了——2026-09-18
    // 从网上写故事那条用例抓到的：故事两章、章节零条。（老的 adopt 也是这个
    // 顺序，只是走它的路梗概多半没变，没露出来。）
    if (!story.premise.empty()) {
        Project fresh = load_or_400(store);
        if (fresh.premise != story.premise) {
            fresh.premise = story.premise;
            store.save_project(fresh);
        }
    }
    (void)project;
    json out = story_response(story);
    out["adopted"] = true;
    return out;
}

ApiResult get_story(const std::string& path) {
    ProjectStore store = open_project(path);
    load_or_400(store); // 只为了验证这是个项目目录，结果不用
    json out = story_response(load_story_or_400(store));
    // 正跑着一份大纲的话把那条流的 id 带回去，页面好重新接上。
    // 没有就不带这个键——"有没有"靠键在不在表达，带个 null 回去前端还得
    // 再分一层。
    const std::string running = OutlineRegistry::instance().running_for(
        paths::to_utf8(store.root()));
    if (!running.empty()) out["outline_running"] = running;
    // 上一轮写砸了的话把那句话带回去。没有就不带这个键。
    const std::string err =
        OutlineRegistry::instance().error_for(paths::to_utf8(store.root()));
    if (!err.empty()) out["outline_error"] = err;
    return {200, std::move(out)};
}

ApiResult post_story(const json& body) {
    forbid_extra(body, {"project", "premise", "scale", "episode_duration_s"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    Story story = load_story_or_400(store);

    if (body.contains("premise")) {
        const std::string p = text::strip_ws(need_str(body, "premise"));
        story.premise = text::truncate_utf8(p, 2000);
        // 老流程的写剧本提示词读的是 Project::premise。两边各存一份的话，
        // 在故事页改完梗概、去写剧本用的还是旧的那句。
        if (project.premise != story.premise) {
            project.premise = story.premise;
            store.save_project(project);
        }
    }
    story.scale = opt_scale(body, "scale", story.scale);
    story.episode_duration_s =
        num_in_range(body, "episode_duration_s", story.episode_duration_s, 0.0,
                     1800.0);

    validate_or_400(story);
    store.save_story(story);
    // **章节动了，章节记录跟着对齐。** 一章一条是机械映射，用户 2026-09-16
    // 选的是「自动做，不要按钮」。
    sync_episodes_to_chapters(store, story);
    return {200, story_response(story)};
}

// 这个函数在头文件里声明了（导出只为了能测），所以不能放进匿名 namespace。
/// 从**补齐过的半份大纲**里挑界面要显示的那几样。
///
/// ⚠️ **每一项都要先问类型。** 这份 JSON 是半路截下来补出来的，任何一个
/// 字段都可能是 null（比如刚写到 `"logline":` 还没开始写值）。拿
/// `value("logline", "")` 去取的话，nlohmann 在 null 上转字符串抛的是
/// type_error——而那会把整条生成搞挂，就为了推一帧进度。
json outline_progress_payload(const json& snap) {
    const auto str_of = [](const json& j, const char* key) -> std::string {
        const auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>()
                                                  : std::string();
    };

    json chapters = json::array();
    if (const auto it = snap.find("chapters");
        it != snap.end() && it->is_array()) {
        for (const auto& c : *it) {
            if (!c.is_object()) continue;
            chapters.push_back({{"title", str_of(c, "title")},
                                {"summary", str_of(c, "summary")},
                                {"hook", str_of(c, "hook")}});
        }
    }
    json people = json::array();
    if (const auto it = snap.find("characters");
        it != snap.end() && it->is_array()) {
        for (const auto& c : *it) {
            if (!c.is_object()) continue;
            people.push_back({{"name", str_of(c, "name")},
                              {"identity", str_of(c, "identity")}});
        }
    }
    return {{"premise", str_of(snap, "premise")},
            {"logline", str_of(snap, "logline")},
            {"genre", str_of(snap, "genre")},
            {"tone", str_of(snap, "tone")},
            {"characters", people},
            {"chapters", chapters}};
}

// 声明在 story_api.hpp 上（一键成片那条也调它）。
json write_outline(ProjectStore& store, const Project& project,
                   const Story& existing, std::string premise, StoryScale scale,
                   const std::string& keywords, const std::string& stream_id,
                   std::uint32_t variation, llm::Client& client,
                   pipeline::CancelToken& tok, bool peek,
                   const std::string& pasted, int chapters) {
    pipeline::Activity act{"outline", paths::to_utf8(store.root()), "",
                           SAY("正在出大纲")};
    const pipeline::CancelLink stop_here{tok, act};

    // **账记在这儿，不记在 post_story_outline 的异步分支里。**
    //
    // 2026-09-13 栽过：真实服务走的是 server.cpp 的 script_route，它先
    // `take_async(body)` 把 async 剥掉、再把处理函数整个扔到后台——于是
    // post_story_outline 里"带 async 才记账"的那一支在服务里从来没走过，
    // 只有单测直接调才走。页面刷新之后问 GET /api/story，账上永远是空的。
    // 这个函数是两条路的汇合点，只有记在这儿两边才都算数。
    // 析构擦账：写砸了、抛了，账都不能留着，不然页面会永远显示"正在写…"。
    struct Bookkeeping {
        std::string project, stream;
        Bookkeeping(std::string p, std::string s)
            : project(std::move(p)), stream(std::move(s)) {
            if (!stream.empty()) OutlineRegistry::instance().started(project, stream);
        }
        ~Bookkeeping() {
            if (!stream.empty()) OutlineRegistry::instance().finished(project, stream);
        }
    } book{paths::to_utf8(store.root()), stream_id};

    llm::Request req;
    // 这一次的底子由调用方定（见 post_story_outline 里那段）：不给就现摇，
    // 给了就用给的，0 等于回到改之前。**同步和异步两条路都从这儿过**，
    // 所以两边摇的是同一个数——摇两次的话页面上看到的和存下来的对不上。
    req.prompt = stages::build_outline_prompt(premise, scale, project.style_line,
                                              keywords, variation, chapters);
    req.schema = stages::outline_schema();
    req.schema_name = "story_outline";
    req.on_thinking = thinking_sink();
    // **摆在 Activity 之后、调模型之前。** 摆在前面的话账本上会多一行
    // 秒开秒关的"正在出大纲"，而这一下根本没干活。
    if (peek) return peek_prompt(req).body;

    Story story;
    try {
        std::string raw;
        if (!pasted.empty()) {
            // **粘回来的那一份。** 走下面同一套解析和守卫——不是绕过检查的
            // 后门：在别处跑出来的东西一样要过 parse_outline。
            raw = pasted;
        } else if (stream_id.empty()) {
            raw = client.complete(req, tok);
        } else {
            // **边写边推。** 出一份大纲三四十秒，攒齐了再蹦出来的话那几十秒
            // 界面上一个字都没有——而"它在想什么"正是这一步用户要看的东西。
            //
            // 章节正文那条抠的是一个字段（JsonFieldStreamer），这儿不行：
            // 大纲是一整个对象，章节还是一串对象。所以走"补齐再解析"，
            // 每一帧把能解出来的那一份挑几样推过去，见 stages/json_partial。
            stages::PartialJson partial;
            int seq = 0;
            // 节流。一帧要把前缀整个解一遍，而 token 是几十毫秒一个；
            // 不节流的话这条回调自己就成了负载，而人眼也看不出区别。
            //
            // ⚠️ **起点要往前推一个节流窗口**，不然第一帧会被自己吞掉——
            // 而有的后端是**整段一次回调**（Client::complete 的默认实现就是
            // 这样，回放后端走的正是它），那种情况下"第一帧"也是唯一一帧，
            // 吞掉就等于一帧都没推。
            auto last = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds(200);
            std::size_t last_size = 0;
            raw = client.complete(req, tok, [&](const std::string& piece) {
                partial.feed(piece);
                const auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::milliseconds(200)) return;
                if (partial.size() == last_size) return;   // 没长就别重解
                last = now;
                last_size = partial.size();
                const json snap = partial.snapshot();
                if (!snap.is_object()) return;   // 这一帧补不出来，跳过
                json msg = outline_progress_payload(snap);
                // **到此为止收了多少字。空窗期唯一看得见的活口。**
                //
                // 上面挑的那几栏（logline / 人物 / 章节）要等模型写到那一栏
                // 才有东西，而它写这份 JSON 的键序每次都不一样：赶上先写
                // 章节摘要的时候，二三十秒里那几栏全是空的，板子上只剩一句
                // 「正在写…」和一个转着的点——跟卡死了长得一模一样。
                // 用户 2026-09-13 报的「ai 正在写的内容也不显示」就是这一段。
                //
                // 这个数不挑栏目、一直在涨，是"它确实在动"的硬证据。
                msg["raw_chars"] =
                    static_cast<int>(text::utf8_len(partial.raw()));
                msg["type"] = "outline_progress";
                msg["job_id"] = stream_id;
                msg["seq"] = seq++;
                job_relay(stream_id, std::move(msg));
            });
        }
        story = stages::parse_outline(raw, premise, scale);
    } catch (const stages::StoryError& e) {
        const std::string msg = SAYF("大模型没写出能用的大纲：%1", e.what());
        // job_error 只往流上广播一次，页面刷新过就没人听见——记下来，
        // 下一次 GET /api/story 带回去。
        if (!stream_id.empty()) OutlineRegistry::instance().failed(book.project, msg);
        throw ApiError(502, msg);
    } catch (const std::exception& e) {
        if (!stream_id.empty()) OutlineRegistry::instance().failed(book.project, e.what());
        throw ApiError(502, e.what());
    }

    // **直接落盘，不再经过草稿。**
    //
    // 2026-09-13 到 09-17 之间这儿只存 story_draft.json，人在页面上点了
    // 「采用」才进 story.json——为的是"源头没人审过就落库，后面几十分钟
    // 渲染全是白跑"。用户 09-17 拍板退掉那一屏：
    //
    //   · 对刚建的项目，「采用」什么都不保护。它那两道闸（写过正文才 409、
    //     有故事才弹确认）在新项目上都是假的，点下去发生的事和直接落盘
    //     一模一样。
    //   · 对已经有正文的项目，那道闸挪到**按下去之前**（post_story_outline
    //     里的 refuse_to_clobber）。八分钟之后再问，问的是一件已经花掉
    //     的事。
    //   · 落盘之后能做的事（展开正文、删章、加章、再来一份）比草稿态多，
    //     不比它少；"看一眼再决定"这件事在正式那份上照样能做。
    //
    // **要一章就只留一章。** 提示词里那一段（`kOneChapter`）已经把「章数按
    // 需要来」那条收回去了，但**提示词不是语法层**（CLAUDE.md 第一条）：
    // 模型照样可能给两三章。多出来的留着比截掉更糟——一键成片接着只做第一
    // 章，剩下那两章会永远挂在故事页上没有正文，而人从没要过它们。
    //
    // 截掉之后这一章的 hook 可能还指着下一章（"她推开了那扇门"）。那是模型
    // 没听话的代价，不是这一刀的代价：不截的话同一句话照样在，只是后面多挂
    // 两条空章。
    if (chapters == 1 && story.chapters.size() > 1) {
        story.chapters.resize(1);
        story.plan.clear();   // 章节计划按章算，截了就得重算（commit_story 会）
    }
    // 刷新、切页、换台机器看，都从 story.json 读——不用再为草稿另存一份。
    return commit_story(store, project, std::move(story), existing);
}

ApiResult post_story_outline(const json& body_in, llm::Client& client,
                             pipeline::CancelToken& tok) {
    json body = body_in;
    // 「只看不发」。**要在起异步那一支之前判**：这一下不调模型，几毫秒就回，
    // 起个后台活再回 202 的话，界面还得去轮询一段根本不会变的进度。
    const bool peek = take_peek(body);
    const std::string pasted = take_paste(body);
    forbid_extra(body, {"project", "premise", "scale", "keywords", "stream",
                        "async", "variation", "overwrite"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);
    // 大纲直接落盘（见 write_outline），所以这道闸只能在这儿。
    refuse_to_clobber(existing, body);

    // 梗概没给就用存着的那份。隔天回来接着写大纲时不用重打一遍。
    std::string premise = text::strip_ws(opt_str(body, "premise", ""));
    if (premise.empty()) premise = existing.premise;
    if (premise.empty()) premise = text::strip_ws(project.premise);
    // **一个字都没有也照写。** 选题是整条流水线上最难从零开始的一步，
    // 把它做成必填门槛就是把人摁在空白框前面发呆；这时候让模型连选题带
    // 大纲一起出，人再挑。给了关键词的话它会往那个方向想。

    const StoryScale scale = opt_scale(body, "scale", existing.scale);
    const std::string keywords = opt_str(body, "keywords", "");
    const std::string stream_id = text::strip_ws(opt_str(body, "stream"));

    // 这一次的底子：几个姓、一种名字形状，什么都没填时再加一组
    // 场域/关系/压力/调子（见 stages::build_outline_names / _spark）。
    //
    // **不给就现摇一个**，给了就用给的——和 /api/script/write 的 variation、
    // 出图那边的 seed 同一规矩（见 ref_gen.hpp）。界面上"再来一个"就是不送
    // 这个字段，"还要刚才那一版的底子"就是把上次的数送回来。
    //
    // **送 0 等于回到改之前**：两段都不拼，提示词逐字节还是老样子。
    // 量"改完到底有没有变"时那就是对照组——没有对照的话，十次都不一样也
    // 说明不了是这次改的功劳。
    //
    // 下界写 -1 不是 0：num_in_range 的下界是开区间（`v <= gt` 就报 422），
    // 而 0 是合法值。
    const std::uint32_t variation =
        body.is_object() && body.contains("variation")
            ? static_cast<std::uint32_t>(
                  num_in_range(body, "variation", 0.0, -1.0, 4294967295.0))
            : stages::random_shape();

    // 自己挪到后台的那一条。理由和别的长活一样（那一整段在 server.cpp 的
    // `start_async` 头上）：这个 handler 占着 Crow 的一条 I/O 线程，而出一份
    // 大纲要三四十秒，落在同一条线程上的连接会跟着冻住——顶栏那块表首当
    // 其冲。（这里原来写着"见 post_story_chapter 里那段"，而那个函数里
    // 没有这样一段：它压根没有自己的异步分支，整件事交给 script_route。）
    //
    // ⚠️ **真实服务走的不是这一条。** `/api/story/outline` 挂在
    // `script_route` 上，而它 `take_async(body)` 会把 `async` 这个键**删掉**
    // 再把处理函数扔进 `start_async`——所以到这儿时 body 里只剩 stream，
    // 下面这个 if 永远是假。走到这里的只有直接调用（用例就是这么测的，见
    // test_story_outline.cpp 里那两条）。
    //
    // 这也是下面 `own` 那个令牌够不着的原因：`start_async` 会挂一个
    // JobScope 把令牌按 stream_id 登记进表里，顶栏「停下」按的就是它；
    // 这一条没有，所以这里的令牌谁也触发不了。**要是哪天让这条上生产
    // （比如给它换一条不剥 async 的路由），得照 start_async 先挂
    // `const JobScope scope{stream_id};`**，否则那个按钮会安静地失效。
    if (opt_bool(body, "async", false) && !stream_id.empty()) {
        const std::string project_path = paths::to_utf8(store.root());
        // **回 202 之前就先登记一笔**（write_outline 进去还会登记一次，
        // 幂等）。直接调这条路时页面拿到 202 马上就会去问，后台线程可能
        // 还没跑到 write_outline。擦账和记错都在 write_outline 里，这儿不管。
        OutlineRegistry::instance().started(project_path, stream_id);
        Offload::instance().post([project_path, premise, scale, keywords,
                                  stream_id, variation, &client] {
            try {
                ProjectStore st = open_project(project_path);
                const Project pj = load_or_400(st);
                const Story ex = load_story_or_400(st);
                pipeline::CancelToken own;
                job_done(stream_id, write_outline(st, pj, ex, premise, scale,
                                                  keywords, stream_id, variation,
                                                  client, own, /*peek=*/false,
                                                  /*pasted=*/std::string()));
            } catch (const ApiError& e) {
                job_error(stream_id, e.what());
            } catch (const std::exception& e) {
                job_error(stream_id, e.what());
            }
        });
        return {202, {{"started", true}, {"stream", stream_id}}};
    }

    return {200, write_outline(store, project, existing, premise, scale,
                               keywords, stream_id, variation, client, tok, peek,
                               pasted)};
}

ApiResult post_story_chapter_delete(const json& body) {
    forbid_extra(body, {"project", "chapter_id"});
    ProjectStore store = open_project(body);
    load_or_400(store);
    const std::string chapter_id = opt_str(body, "chapter_id", "");
    if (chapter_id.empty()) {
        throw unprocessable_top("chapter_id", "Field required", body, "missing");
    }
    Story story = load_story_or_400(store);
    const Story::ChapterRemoval r = story.remove_chapter(chapter_id);
    if (!r.removed) throw ApiError(404, SAYF("没有章节 %1", chapter_id));
    store.save_story(story);
    // **章节动了，章节记录跟着对齐。** 一章一条是机械映射，用户 2026-09-16
    // 选的是「自动做，不要按钮」。
    sync_episodes_to_chapters(store, story);

    json out = story_response(story);
    out["deleted"] = chapter_id;
    out["plan_dropped"] = r.plan_dropped;
    out["plan_moved"] = r.plan_moved;
    return {200, std::move(out)};
}

ApiResult post_story_adopt(const json& body) {
    forbid_extra(body, {"project", "story", "overwrite"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);

    const auto it = body.find("story");
    if (it == body.end() || !it->is_object()) {
        throw unprocessable_top("story", "Field required", body, "missing");
    }

    Story story;
    try {
        story = it->get<Story>();
    } catch (const std::exception& e) {
        throw ApiError(400, SAYF("这份大纲读不了：%1", e.what()));
    }

    refuse_to_clobber(existing, body);
    return {200, commit_story(store, project, std::move(story), existing)};
}

ApiResult post_story_import(const json& body) {
    forbid_extra(body, {"project", "text", "scale", "title", "overwrite"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);
    // 切出来的章节会把现在这几章整份换掉，和出大纲一样先挡。
    refuse_to_clobber(existing, body);

    const std::string raw = need_str(body, "text");
    if (text::strip_ws(raw).empty()) throw ApiError(400, SAY("粘进来的是空的"));

    Story story;
    story.source = StorySource::PASTED;
    story.scale = opt_scale(body, "scale", existing.scale);
    story.premise = existing.premise;
    story.logline = text::clean_field(opt_str(body, "title", ""));
    story.chapters = stages::split_pasted(raw);
    if (story.chapters.empty()) throw ApiError(400, SAY("这段文本切不出章节来"));

    // 人物、关系、地点都还是空的——那些要读懂内容才提得出来。前端靠这个
    // 数提醒人「下一步让 AI 读一遍」，不然会一路走到分镜才发现资产库是空的。
    const bool needs_analysis = story.characters.empty();
    json out = commit_story(store, project, std::move(story), existing);
    out["needs_analysis"] = needs_analysis;
    return {200, out};
}

ApiResult post_story_analyze(const json& body_in, llm::Client& client,
                             pipeline::CancelToken& tok) {
    json body = body_in;
    // 「只看不发」：带了就把这一步的提示词原样回去，不调模型。见 prompt_peek。
    const bool peek = take_peek(body);
    const std::string pasted = take_paste(body);
    forbid_extra(body, {"project", "stream"});   // stream 同上，只为异步外壳
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = load_story_or_400(store);

    if (story.chapters.empty()) {
        throw ApiError(400, SAY("还没有故事。先写一份大纲，或者粘一段进来"));
    }
    if (story.written_chapters() == 0) {
        throw ApiError(400,
                       SAY("章节都还没有正文，没什么可读的。"
                           "大纲写出来的故事本来就带人物表，不用走这一步"));
    }

    pipeline::Activity act{"analyze", paths::to_utf8(store.root()), "",
                           SAY("正在读这个故事")};
    const pipeline::CancelLink stop_here{tok, act};

    llm::Request req;
    req.prompt = stages::build_analyze_prompt(story, project.style_line);
    req.schema = stages::analyze_schema();
    req.schema_name = "story_analysis";
    req.on_thinking = thinking_sink();
    if (peek) return peek_prompt(req);

    Story read;
    try {
        // 这条口子上没有那个勾（界面不走它，见 server.cpp 的路由表）。
        // **照旧全部重出**：显式点「读一遍」要的就是整份重读。
        read = stages::apply_analysis(
            story, pasted.empty() ? client.complete(req, tok) : pasted,
            /*overwrite=*/true);
    } catch (const stages::StoryError& e) {
        throw ApiError(502, SAYF("大模型没读出能用的结构：%1", e.what()));
    } catch (const std::exception& e) {
        throw ApiError(502, e.what());
    }

    // 正文一个字不动，只多了人物、关系、地点和钩子——不用挡，直接落。
    // 钩子变了切点就变了，commit_story 里会重算章节计划：这正是这一步的
    // 价值，机械切点只保证不切在半句话中间，现在能切在真正的悬念上了。
    const bool needs_analysis = read.characters.empty();
    json out = commit_story(store, project, std::move(read), story);
    out["needs_analysis"] = needs_analysis;
    return {200, out};
}

ApiResult post_story_understand_once(const json& body_in, llm::Client& client,
                                     pipeline::CancelToken& tok) {
    json body = body_in;
    const bool peek = take_peek(body);
    const std::string pasted = take_paste(body);
    forbid_extra(body, {"project", "overwrite", "stream"});
    const bool overwrite = opt_bool(body, "overwrite", false);
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = load_story_or_400(store);
    if (story.written_chapters() == 0) {
        throw ApiError(400,
                       SAY("故事还没有正文，没什么可理解的。先去故事页写"));
    }

    pipeline::Activity act{"understand", paths::to_utf8(store.root()), "",
                           SAY("正在理解这个故事")};
    const pipeline::CancelLink stop_here{tok, act};

    llm::Request req;
    req.prompt = stages::build_understand_prompt(story, project.style_line);
    req.schema = stages::understand_schema();
    req.schema_name = "story_understanding";
    req.on_thinking = thinking_sink();
    if (peek) return peek_prompt(req);

    const std::string ratio =
        config::load_settings(store.root()).video.aspect_ratio();
    Story read;
    AssetLibrary looks;
    try {
        const std::string raw = pasted.empty() ? client.complete(req, tok) : pasted;
        // **同一份回答喂两个解析器。** 结构那半和长相那半是同一批人、同一批
        // 地方——不需要再比一遍名单。
        // 那个勾一路传到底：不勾只补缺的那一档，**读这一步也只补不顶**
        // ——和下面 merge_assets 那半、和补剧本补分镜那两步同一条规矩。
        read = stages::apply_analysis(story, raw, overwrite);
        looks = stages::parse_bible(raw, project.style_line, ratio);
    } catch (const stages::StoryError& e) {
        throw ApiError(502, SAYF("大模型没读出能用的结构：%1", e.what()));
    } catch (const stages::BibleError& e) {
        throw ApiError(502, SAYF("大模型没给出能用的长相：%1", e.what()));
    } catch (const std::exception& e) {
        throw ApiError(502, e.what());
    }

    json out = commit_story(store, project, std::move(read), story);
    const ApiResult merged = merge_assets(store, looks, overwrite, "story");
    out["assets"] = merged.body;
    return {200, out};
}

void note_chapter_gate(const std::string& call_id, const std::string& chapter_id,
                       int attempt, int attempts, bool strict,
                       const std::exception* e, bool revision) {
    // **不是闸门的就不记**，理由写在 story_api.hpp 上那一段。
    const auto* gate = dynamic_cast<const stages::StoryError*>(e);
    if (e != nullptr && gate == nullptr) return;

    llm::GateVerdict v;
    v.call_id = call_id;
    // 和 `Request::schema_name` 同一个词——两份 jsonl 按这一列分组，
    // 分出来的得是同一批东西。
    v.stage = "chapter";
    v.subject = chapter_id;
    v.attempt = attempt;
    v.attempts = attempts;
    v.strict = strict;
    v.revision = revision;
    v.ok = (e == nullptr);
    if (gate != nullptr) {
        v.gate = gate->code();
        v.reason = gate->what();
    }
    llm::note_gate(v, llm::call_log_options(config::runtime().snapshot().llm));
}

llm::Request chapter_request(const Story& story, const std::string& chapter_id,
                             models::StyleLine style) {
    llm::Request req;
    req.prompt = stages::build_chapter_prompt(story, chapter_id, style);
    req.schema = stages::chapter_schema(stages::chapter_target_scenes(story),
                                       stages::chapter_scene_paras(story));
    req.schema_name = "chapter";
    req.on_thinking = thinking_sink();
    req.temperature = stages::kChapterTemperature;
    // **想多久不在这儿写死了**（2026-09-19）：原来这一行是 `= "high"`，治的是
    // glm-5.3 把整章写在思考里那个病（977 秒、思考 169394 字、正文 0 字）。
    // 病和药方都还在，只是搬进了配置——`[llm] reasoning_effort`，默认仍是
    // high，模型窗里能调。为什么是 high 不是 low、撤掉会怎样，见
    // `LLMConfig::reasoning_effort` 上那段。
    //
    // 这儿留空 = 按配置来（`build_payload` 里 `effort_for(schema_name)`）。
    return req;
}

llm::Request chapter_revision_request(const Story& story, const std::string& chapter_id,
                                      models::StyleLine style,
                                      const stages::ChapterRejected& rejected) {
    llm::Request req = chapter_request(story, chapter_id, style);
    req.prompt = stages::build_chapter_revision_prompt(story, chapter_id, style,
                                                       rejected.draft(),
                                                       rejected.problems());
    return req;
}

/// 真正写这一章：借大模型、边写边推、解析、落库、重算章节计划。
///
/// **从接口里抽出来的，因为它有两条调用路**：同步那条（老客户端）直接在
/// Crow 的线程上跑完；异步那条在后台线程上跑，接口早就回过"开始了"。
/// 两条路跑的必须是同一段代码——抄一份的话，两边迟早只改一边，而不一致的
/// 表现是"用新版界面写出来的和用 curl 写出来的不一样"。
json write_one_chapter(ProjectStore& store, const Project& project, Story story,
                       const std::string& chapter_id,
                       const std::string& stream_id, llm::Client& client,
                       pipeline::CancelToken& tok, bool peek,
                       const std::string& pasted) {
    const Chapter* me = story.chapter_by_id(chapter_id);
    if (me == nullptr) throw ApiError(404, SAYF("没有这一章：%1", chapter_id));

    // 登记到顶栏那本账上。不登记的话这一两分钟里引擎在界面上看着是闲着的
    // ——而它正占着大模型那一槽，别的活全得等。
    //
    // **第三个参数是 episode_id，不能塞 chapter_id。** 这两个是两套命名
    // （`^ch[0-9]+$` 对 `episode_id`），而前端拿这个字段干的事是
    // `session.selectEpisode(row.episode_id)`（JobBadge 的 go()，点这一行
    // 就跳过去）。塞进去之后：章号变成 "ch03" → 写进 localStorage →
    // `/bff/flow` 那边只在**空串**时才回落到第一章，非空但不认识的原样
    // 回给你 → 前端看见 `data.episodeId === episodeId.value` 就不改了。
    // 于是章号永久卡在一个不存在的章上，刷新也还在：顶栏那个下拉空着、
    // 「这一章」整页没东西。章是故事那一层的，本来就不属于某一条章节记录，留空。
    pipeline::Activity act{
        "write_one", paths::to_utf8(store.root()), "",
        SAYF("正在写 %1", me->title.empty() ? chapter_id : me->title)};
    const pipeline::CancelLink stop_here{tok, act};

    const llm::Request req = chapter_request(story, chapter_id, project.style_line);
    if (peek) return peek_prompt(req).body;

    // 给了 stream_id 就**边写边推**。写一章要一两分钟，攒齐了再蹦出来的话
    // 那一两分钟界面上什么都没有——而那正是用户要看的"写作的过程"。
    //
    // **这一步不能像改稿那样退回大白话。** 除了正文还要模型标出这一章里
    // 哪几个地方可以收一章（hooks），而那些钩子是一章停在真悬念上的全部
    // 依据（实跑里把比例从 25% 抬到 56%）。为了能流式砍掉 hooks，等于拿
    // 切章的质量换一个动画。所以照旧约束成 JSON，只在 token 流上顺手把正文
    // 那个字段解出来推给编辑器——见 stages/json_stream。
    Story next;
    // 落地的时候这一章还在不在。见下面那段"接在刚读回来的那一份上"。
    bool chapter_gone = false;
    // 闸门那一行要对回这一次调用。**在 `complete()` 回来之后才取**，而且
    // 粘回来的那一支根本不取——见 `llm::take_last_call_id()` 上那段（线程是
    // 池子里轮着用的，读一眼会捡到上一个请求留下的 id）。
    std::string call_id;

    // **一稿，打回了改一稿。** 单章这条原来只有一轮，理由是「前面站着一个
    // 人，他看见报错自己会再按一下」——可再按一下发的还是同一份提示词，
    // 而闸门手里明明有坏在第几场、是哪句。2026-09-19 起打回之后先把那份清单
    // 连上一稿一起回给模型改（chapter_revision_request），改完还不过才报错。
    // 手里没稿的那几种（JSON 解析不动）没法改，第二轮照旧重掷。
    //
    // 粘回来的那一份只有一轮：没有模型可以改。
    //
    // 最后一轮把软闸关掉，和批量那条同一个理由（batch.cpp）：0 字比「写得
    // 一般」差得多。
    constexpr int kAttempts = 2;
    const int attempts = pasted.empty() ? kAttempts : 1;
    std::optional<stages::ChapterRejected> rejected;   // 上一轮被打回的稿
    std::string last_error;
    const int floor_chars = static_cast<int>(
        stages::chapter_target_chars(story) * stages::kChapterMinRatio);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const bool revising = attempt > 0 && rejected.has_value();
        const bool strict = pasted.empty() ? attempt + 1 < attempts : true;
        if (attempt > 0) {
            // 顶栏那一行说清这一轮在干什么，人才知道为什么又等一两分钟。
            act.set_note((revising ? SAY("改稿：上一稿") : SAY("重写：上一稿")) +
                         text::truncate_utf8(last_error, 60));
        }
        const llm::Request use =
            revising ? chapter_revision_request(story, chapter_id, project.style_line,
                                                *rejected)
                     : req;
        call_id.clear();
        try {
            std::string raw;
            if (!pasted.empty()) {
                // 粘回来的那一份。守卫一道不少：整章没对白、正文复读、贴情绪
                // 标签，在别处跑出来的一样打回。
                raw = pasted;
            } else if (stream_id.empty()) {
                raw = client.complete(use, tok);
            } else {
                // **抠的是 paragraphs，不是 text。** c41821d 把章节正文从一个
                // 字符串改成了一段一项的数组，而这里没跟着改——于是流式一个字
                // 都抠不出来，界面上就是"AI 写作没有热更新"，后端不报任何错。
                stages::JsonFieldStreamer field(stages::kChapterBodyField, true);
                // **每一轮从 0 数起。** 编辑器按 seq 归零判"重头来"（StoryView
                // 那条），改稿那一轮的字才不会接在被打回的那半截后面。
                int seq = 0;
                raw = client.complete(use, tok, [&](const std::string& piece) {
                    const std::string fresh = field.feed(piece);
                    if (fresh.empty()) return;  // JSON 外壳和 hooks 那一串不推
                    job_relay(stream_id, {{"type", "story_token"},
                                          {"job_id", stream_id},
                                          {"seq", seq++},
                                          {"text", fresh}});
                });
            }
            if (pasted.empty()) call_id = llm::take_last_call_id();
            const stages::ChapterDraft d = stages::parse_chapter(raw, floor_chars, strict);
            // **接在刚读回来的那一份上，不是进函数时那份。**
            //
            // 上面这一趟要跑一两分钟，而这一两分钟里用户完全可能在编辑器里改
            // **别的章**——故事页就是这么设计的：一边看 AI 写，一边还能读能改，
            // 只有正在写的那一章是锁着的，改完 1.5 秒自动保存直接落 story.json。
            // 拿进函数时那份快照整份写回去，那些字就被悄悄吞掉了：不报错，人是
            // 过几分钟翻回那一章才发现自己白改了。
            //
            // 批量展开那条（batch.cpp 里 `apply_chapter(store.load_story(), …)`）
            // 早就这么写了，注释也在那儿——单章这条是同一个函数、同一种坏法，
            // 只是漏了。重读一次的代价是一个文件。
            Story latest = store.load_story();
            if (latest.chapter_by_id(chapter_id) == nullptr) {
                // 这一两分钟里这一章被删了。**不能混进下面那句 502 里说**
                // ——「大模型没写出能用的正文」是假话，正文写出来了，只是没
                // 地方放了。
                chapter_gone = true;
            } else {
                next = stages::apply_chapter(latest, chapter_id, d);
                // **等 apply_chapter 也过了才算过**：「又停在上一章停过的地方」
                // 那道闸在它里面。这一章被删了的那一支（chapter_gone）不记——
                // 那道闸压根没跑，记成"过了"是假的。
                note_chapter_gate(call_id, chapter_id, attempt + 1, attempts, strict,
                                  nullptr, revising);
            }
            last_error.clear();
            break;
        } catch (const stages::ChapterRejected& e) {
            note_chapter_gate(call_id, chapter_id, attempt + 1, attempts, strict, &e,
                              revising);
            last_error = e.what();
            rejected = e;   // 稿子还在，下一轮改它
        } catch (const stages::StoryError& e) {
            note_chapter_gate(call_id, chapter_id, attempt + 1, attempts, strict, &e,
                              revising);
            last_error = e.what();
            rejected.reset();   // 没稿可改，下一轮只能重掷
        } catch (const std::exception& e) {
            // **配错了、人按了停：不重来，照实报。** 密钥不对换一轮还是不对，
            // 取消更不该再发一次。别的——JSON 没收好、限流、连接抖了一下——
            // 和批量那条同一个判据（hopeless），还有轮次就再掷一次：2026-09-19
            // 拿 Qwen3-4B 实跑，第一稿只是漏了一个闭引号，原来这儿当场 502，
            // 人只能自己再按一下，而按一下发的还是同一份提示词。
            const auto* llm_err = dynamic_cast<const llm::LlmError*>(&e);
            const bool cancelled = std::string(e.what()).find(util::kCancelled) !=
                                   std::string::npos;
            const bool give_up = cancelled || attempt + 1 >= attempts ||
                                 (llm_err != nullptr && llm_err->is_config_error());
            if (give_up) {
                // story_error 是给编辑器用的（把流了一半的字撤掉）；异步那条路
                // 上还有个人在等最终结果，那条 job_error 由 server.cpp 的
                // start_async 播——两条各管各的，别合并。
                if (!stream_id.empty()) {
                    job_relay(stream_id, {{"type", "story_error"},
                                          {"job_id", stream_id},
                                          {"message", e.what()}});
                }
                throw ApiError(502, e.what());
            }
            last_error = e.what();
            rejected.reset();   // 没稿可改，下一轮重掷
        }
        if (chapter_gone) break;
    }
    if (!last_error.empty()) {
        if (!stream_id.empty()) {
            job_relay(stream_id, {{"type", "story_error"},
                                  {"job_id", stream_id},
                                  {"message", last_error}});
        }
        throw ApiError(502, SAYF("大模型没写出能用的正文：%1", last_error));
    }
    if (chapter_gone) {
        throw ApiError(409,
                       SAYF("写完的时候「%1」已经不在这个故事里了（写的这一两"
                            "分钟里被删掉了），这一章的正文没处放。",
                            chapter_id));
    }

    // 钩子换了，切点就换了。正文落进去之前那些候选是对着空正文算的。
    next.plan = stages::plan_episodes(next, next.episode_duration_s);
    validate_or_400(next);
    store.save_story(next);

    json out = story_response(next);
    out["chapter_id"] = chapter_id;
    const Chapter* done = next.chapter_by_id(chapter_id);
    out["chars"] = done != nullptr ? done->text_len() : 0;
    out["target_chars"] = stages::chapter_target_chars(story);
    return out;
}

ApiResult post_story_chapter(const json& body_in, llm::Client& client,
                             pipeline::CancelToken& tok) {
    json body = body_in;
    const bool peek = take_peek(body);
    const std::string pasted = take_paste(body);
    forbid_extra(body, {"project", "chapter_id", "overwrite", "stream"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    Story story = load_story_or_400(store);

    const std::string chapter_id = need_str(body, "chapter_id");
    const Chapter* me = story.chapter_by_id(chapter_id);
    if (me == nullptr) throw ApiError(404, SAYF("没有这一章：%1", chapter_id));
    if (!peek && !text::strip_ws(me->text).empty() &&
        !opt_bool(body, "overwrite", false)) {
        throw ApiError(409,
                       SAY("这一章已经有正文了。要重写就带上 overwrite"));
    }

    const std::string stream_id = text::strip_ws(opt_str(body, "stream"));

    // **只看不发那一下要绕过"已经有正文了"那道 409。** 想复制提示词的人
    // 多半正是因为这一章写砸了要去别处重跑，而那时候正文是有的。
    return {200, write_one_chapter(store, project, std::move(story), chapter_id,
                                   stream_id, client, tok, peek, pasted)};
}

/// 把章节记录对齐到故事的章：一章一条。
///
/// 用户 2026-09-16 选的是「自动做，不要按钮」——一章一条是机械映射，
/// 不该让人去按一下。**所以每一处改动章节的接口，存完 story 都要叫它。**
///
/// 返回这一轮建了几个、更新了几个、还有哪几个落了单（对不上任何一章）。
/// **落单的不删**：它们可能已经出过片，删了就是把成片连着记录一起抹掉。
EpisodeSync sync_episodes_to_chapters(const ProjectStore& store, const Story& story) {
    EpisodeSync out;
    if (story.chapters.empty()) return out;                 // 还没有章节

    Project project = load_or_400(store);
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        const Chapter& c = story.chapters[i];
        if (c.chapter_id.empty()) continue;

        // 一章一个 id：ch07 → ep07。章节计划（plan_episodes）发 id 用的是
        // 同一条，两边对得上。
        const std::string ep_id = stages::episode_id_for_chapter(c.chapter_id, i);

        // 这一章值多长。**按正文字数估**（story_plan 那个每秒消化多少字的
        // 系数），没正文按梗概的章数占比。原来数「结束在这一章」的计划条目，
        // 短章被并进同一条时一条都数不到、跨章的条目把整段时长记到后一章
        // 头上（2026-09-16 查出）。这个数不卡长度（分镜、配音那两次
        // rebalance 都跳过），它只是拆镜头的目标量。
        //
        // 没正文时的回落 2026-09-18 之前是 `[assembly].episode_s`；那一项的
        // 正业（成片按每集 N 秒切）随电影平台一起拔掉了，回落改成代码里的
        // 常量 stages::kDefaultChapterS。
        double dur = 0.0;
        if (c.text_len() > 0) {
            dur = static_cast<double>(c.text_len()) / stages::kProseCharsPerSecond;
        }
        if (!(dur > 0.0)) dur = stages::kDefaultChapterS;

        const std::string synopsis =
            text::truncate_utf8(text::collapse_ws(c.summary), 120);

        Episode* existing = project.episode_by_id(ep_id);
        if (existing != nullptr) {
            // **只补元数据。** script 和 shots 一个字不动——章节一改就把写好
            // 的剧本和出过的片冲掉，那是没法接受的。
            if (!c.title.empty()) existing->title = c.title;
            existing->target_duration_s = dur;
            existing->chapter_refs = {c.chapter_id};
            if (existing->synopsis.empty()) existing->synopsis = synopsis;
            out.updated.push_back(ep_id);
            continue;
        }

        Episode ep;
        ep.episode_id = ep_id;
        ep.title = c.title;
        ep.synopsis = synopsis;
        ep.target_duration_s = dur;
        ep.chapter_refs = {c.chapter_id};
        project.episodes.push_back(std::move(ep));
        out.created.push_back(ep_id);
    }

    for (const Episode& e : project.episodes) {
        const bool mine =
            std::find(out.created.begin(), out.created.end(), e.episode_id) !=
                out.created.end() ||
            std::find(out.updated.begin(), out.updated.end(), e.episode_id) !=
                out.updated.end();
        if (!mine) out.orphans.push_back(e.episode_id);
    }

    store.save_project(project);
    return out;
}

ApiResult post_story_episodes(const json& body) {
    forbid_extra(body, {"project"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    const Story story = load_story_or_400(store);

    // **一章落一条章节记录。** 实现在 sync_episodes_to_chapters 里——那件事
    // 现在是自动做的（每一处改动章节的接口存完 story 都会叫它），这个接口
    // 留着是为了让老客户端和对拍脚本还能按一下。


    // **一章一条，没有第二条路。** 这儿原来是个 `if (chapter_mode)`：
    // 假那一边按 story.plan（章节计划）一条落一条。按计划落和一章一条是两种
    // 落法，2026-09-16 用户定了只留前者，那一大块跟着删——留着一条走不到的
    // 等于留一份会跟着腐烂的第二真相。
    if (story.chapters.empty()) {
        throw ApiError(400,
                       SAY("这部电影还没有章节。先去故事页写一份大纲"));
    }
    const EpisodeSync r = sync_episodes_to_chapters(store, story);
    json out = story_response(story);
    out["created"] = r.created;
    out["updated"] = r.updated;
    out["orphans"] = r.orphans;
    return {200, out};
}

namespace {

/// 从请求体里取出"选中的哪一段"，顺手校验。
stages::Span need_span(const json& body, const Story& story) {
    stages::Span span;
    span.chapter_id = need_str(body, "chapter_id");
    const Chapter* c = story.chapter_by_id(span.chapter_id);
    if (c == nullptr) throw ApiError(404, SAYF("没有这一章：%1", span.chapter_id));

    const int len = c->text_len();
    span.from_char = static_cast<int>(
        num_in_range(body, "from_char", 0.0, -1.0, 1e9));
    span.to_char = static_cast<int>(
        num_in_range(body, "to_char", static_cast<double>(len), -1.0, 1e9));
    // **空区间 [k, k) 是合法的：那是"在这儿写一段"**，章还是空的时候就是
    // [0, 0)。故事页 2026-09-17 起只有这一条 AI 路（选中了改选中的，没选就
    // 整章，空章从头写），三种都走这儿，只差区间。
    if (span.from_char < 0 || span.to_char > len || span.from_char > span.to_char) {
        // **位置对不上就拒**，别夹到合法范围里硬改。夹过之后改的是另一段
        // 字，而用户看到的是"改好了"——他得自己一段段核对才发现改错了地方。
        throw ApiError(400, SAYN("选中的范围不对：这一章有 %n 个字，"
                                 "而选的是 [%1, %2)",
                                 static_cast<long long>(len),
                                 std::to_string(span.from_char),
                                 std::to_string(span.to_char)));
    }
    return span;
}

std::vector<stages::ReviseTurn> read_history(const json& body) {
    std::vector<stages::ReviseTurn> out;
    const auto it = body.find("history");
    if (it == body.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (!v.is_object()) continue;
        stages::ReviseTurn t;
        t.role = v.value("role", std::string("user"));
        t.text = text::strip_ws(v.value("text", std::string()));
        if (t.text.empty()) continue;
        if (t.role != "assistant") t.role = "user";
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace

ApiResult post_story_revise(const json& body, llm::Client& client,
                            pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "chapter_id", "from_char", "to_char",
                        "instruction", "history", "stream"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = load_story_or_400(store);

    const stages::Span span = need_span(body, story);
    const std::string instruction = text::strip_ws(opt_str(body, "instruction"));
    const auto history = read_history(body);
    if (instruction.empty() && history.empty()) {
        throw ApiError(400,
                       SAY("没说要改成什么样。选中一段之后说一句，比如"
                           "「这儿太赶了，铺一下情绪」"));
    }

    const std::string before = stages::span_text(story, span);
    const int span_chars = static_cast<int>(text::utf8_len(before));
    // 整章多长也要给：上限取"选中的六倍"和"整章的六成"里松的那条，而后者
    // 才是"抄整章"的真判据。只看倍数的话，短选区上一个正当的「拉长」会被
    // 打回——实跑撞过。
    const Chapter* whole = story.chapter_by_id(span.chapter_id);
    const int whole_chars = whole != nullptr ? whole->text_len() : 0;

    // 给了 stream_id 就**边生边推**：写一段话要十几秒，攒齐了再一次性蹦
    // 出来的话，中间那十几秒界面上什么都没有——而那正是用户要看的"写作的
    // 过程"。字走 WebSocket，这个请求照样在最后回完整的一份（前端拿它对
    // 一遍，也让丢包的连接有个兜底）。
    const std::string stream_id = text::strip_ws(opt_str(body, "stream"));
    const bool streaming = !stream_id.empty();

    // 同 write_one：第三个参数是 episode_id，`span.chapter_id` 不是章号，
    // 塞进去会把前端的章号卡死在一个不存在的章上。
    pipeline::Activity act{"revise", paths::to_utf8(store.root()), "",
                           SAY("正在改这一段")};
    const pipeline::CancelLink stop_here{tok, act};

    llm::Request req;
    // **流式那条不要 JSON。** 逐字插进编辑器的话，用户先看到的会是
    // `{"text":"` 这几个字符。代价是没有 note，那本来也只是锦上添花。
    req.prompt = stages::build_revise_prompt(story, span, instruction, history,
                                             project.style_line, streaming);
    if (!streaming) {
        req.schema = stages::revise_schema();
        req.schema_name = "story_revision";
        req.on_thinking = thinking_sink();
    }

    stages::Revision rev;
    try {
        if (streaming) {
            int seq = 0;
            const std::string raw = client.complete(
                req, tok, [&](const std::string& piece) {
                    // **不节流。** 逐字推正是这件事的全部意义；而 Hub 的
                    // 节流是按 (job, type) 分桶的，type 用 progress 的话
                    // 会被 200ms 一桶压掉九成。
                    job_relay(stream_id, {{"type", "story_token"},
                                          {"job_id", stream_id},
                                          {"seq", seq++},
                                          {"text", piece}});
                });
            rev = stages::parse_plain_revision(raw, span_chars, whole_chars);
        } else {
            rev = stages::parse_revision(client.complete(req, tok), span_chars,
                                         whole_chars);
        }
    } catch (const std::exception& e) {
        if (streaming) {
            // 报错也要推一条：前端那边正等着字，不推的话它一直显示"改着…"
            job_relay(stream_id, {{"type", "story_error"},
                                  {"job_id", stream_id},
                                  {"message", e.what()}});
        }
        throw ApiError(502, SAYF("改稿没改出能用的东西：%1", e.what()));
    }
    if (streaming) {
        job_relay(stream_id, {{"type", "story_done"},
                              {"job_id", stream_id},
                              {"text", rev.text}});
    }

    // **只回草稿，不落库。** 和写大纲同一条规矩，而且这里更要紧：大纲落错了
    // 重写一份就是，改稿落错了盖掉的是作者自己写的字。
    return {200, {
        {"chapter_id", span.chapter_id},
        {"from_char", span.from_char},
        {"to_char", span.to_char},
        {"before", before},
        {"text", rev.text},
        {"note", rev.note},
    }};
}

ApiResult post_story_revise_apply(const json& body) {
    forbid_extra(body, {"project", "chapter_id", "from_char", "to_char", "text"});
    ProjectStore store = open_project(body);
    load_or_400(store);
    const Story story = load_story_or_400(store);

    const stages::Span span = need_span(body, story);
    const std::string text_in = text::strip_ws(need_str(body, "text"));
    if (text_in.empty()) throw ApiError(400, SAY("要写回去的那一段是空的"));

    Story next = stages::apply_revision(story, span, text_in);
    // **章节计划跟着重算。** 正文长度变了，后面每一条的字符区间都错位了；
    // 不重算的话切线会落在句子中间，而这件事不报错，只在成片里表现成
    // "这一章从半句话开始"。
    next.plan = stages::plan_episodes(next, next.episode_duration_s);

    validate_or_400(next);
    store.save_story(next);
    // **章节动了，章节记录跟着对齐。** 一章一条是机械映射，用户 2026-09-16
    // 选的是「自动做，不要按钮」。
    sync_episodes_to_chapters(store, next);

    json out = story_response(next);
    const Chapter* c = next.chapter_by_id(span.chapter_id);
    out["chapter_id"] = span.chapter_id;
    out["chars"] = c != nullptr ? c->text_len() : 0;
    return {200, out};
}

ApiResult post_story_from_episodes(const json& body) {
    forbid_extra(body, {"project", "overwrite"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);

    // 已经有故事了还反推，反推出来的那份会把它整份顶掉——而那一份里可能
    // 有人工改过的人物关系和切章的线。和采用大纲同一条规矩。
    if (!existing.empty() && !opt_bool(body, "overwrite", false)) {
        throw ApiError(409,
                       SAY("这个项目已经有故事了，反推会把它整份顶掉。"
                           "确认要换的话带上 overwrite。"));
    }

    Story story = stages::story_from_episodes(project);
    if (story.chapters.empty()) {
        throw ApiError(400,
                       SAY("这个项目里一章剧本都没有，反推不出东西来。"
                           "先写一章，或者直接在故事那一页写大纲"));
    }

    validate_or_400(story);
    store.save_story(story);
    // **章节动了，章节记录跟着对齐。** 一章一条是机械映射，用户 2026-09-16
    // 选的是「自动做，不要按钮」。
    sync_episodes_to_chapters(store, story);

    // **顺手把章节记录和章接上。** 不接的话故事在这儿、章节记录在那儿，两边
    // 看着都齐全，只有写下一章时才发现它拿不到前情——而那时候没有任何报错。
    Project linked = project;
    for (const auto& p : story.plan) {
        Episode* ep = linked.episode_by_id(p.episode_id);
        if (ep != nullptr) ep->chapter_refs = {p.from_chapter};
    }
    store.save_project(linked);

    json out = story_response(story);
    // 反推是机械的，人物关系一个都没有——这里说清楚下一步该点哪儿，
    // 不然用户会以为反推完就齐了，而故事页上人物那一栏是空的。
    out["next"] = SAY("读现成正文提结构");
    return {200, out};
}

ApiResult post_story_plan(const json& body) {
    forbid_extra(body, {"project", "duration_s"});
    ProjectStore store = open_project(body);
    load_or_400(store);
    Story story = load_story_or_400(store);

    if (story.chapters.empty()) {
        throw ApiError(400, SAY("还没有故事，先写一份大纲"));
    }

    story.episode_duration_s =
        num_in_range(body, "duration_s", story.episode_duration_s, 0.0, 1800.0);
    story.plan = stages::plan_episodes(story, story.episode_duration_s);

    validate_or_400(story);
    store.save_story(story);
    return {200, story_response(story)};
}

}  // namespace changji::http
