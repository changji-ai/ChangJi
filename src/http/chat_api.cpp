#include "http/chat_api.hpp"
#include "util/say.hpp"
#include "util/chapter_word.hpp"
#include "util/chat_id.hpp"
#include "util/text.hpp"
#include "util/tool_slot.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "agent/attachments.hpp"
#include "agent/loop.hpp"
#include "agent/tools.hpp"
#include "agent/transcript.hpp"
#include "config/runtime.hpp"
#include "http/run.hpp"
#include "http/ws.hpp"
#include "pipeline/activity.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/task_board.hpp"   // Activity::task() 回的 Task
#include "util/paths.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace changji::http {

namespace {

/// 还没有项目那会儿，对话落在哪儿。
///
/// 建出项目之后整条搬进项目目录（`write_transcript`）——**对话是这部片子的
/// 一部分**，拷走目录就该把它一起拷走。
fs::path homeless_dir() {
    return paths::user_data_dir("changji") / "chat";
}

fs::path dir_for(const std::string& project) {
    return project.empty() ? homeless_dir() : paths::from_utf8(project);
}

/// 一个项目一轮。
///
/// **不排队，正在跑就回 409。** 两轮并着跑会往同一个 chat.jsonl 里交叉写，
/// 而且各自拼的状态摘要都是半截的——那种错不会报，只会让对话越说越不对。
struct Session {
    std::atomic<bool> busy{false};
    std::shared_ptr<pipeline::CancelToken> tok;

    /// 还能**自动接续**几轮。
    ///
    /// 派出去的活干完了，引擎往对话里追一条「做完了」，再让代理跑一轮去
    /// 反应（报账、或者接着做下一步）。那一轮里它可能又派一件活——
    /// **没有上限的话一条对话能自己跑一整晚**，而每一轮都在花钱。
    ///
    /// 人一说话就把它填满：人在旁边盯着的时候，接着往下做是他要的。
    std::atomic<int> auto_left{0};

    /// **权限那一档**：`auto`（放手做）/ `ask`（动东西之前先问）/ `read`（只看）。
    ///
    /// 跟着人说的那一句一起来（桌面端输入框右下角那颗），**记在这儿**是因为活
    /// 干完之后自动接续的那几轮（`watch_and_react`）也得照这一档办——那会儿
    /// 没有人说话，拿不到新的一档。
    std::mutex mu;
    std::string permission = "auto";

    /// 「每步问我」那一档：正在等人点头的那一问。0 = 没在等。
    std::uint64_t asking = 0;
    /// 人点了没有、点的是什么。
    bool answered = false;
    bool allowed = false;
    std::condition_variable answer_cv;
    std::uint64_t next_ask = 1;
};

std::mutex g_mu;
std::map<std::string, std::shared_ptr<Session>> g_sessions;

std::shared_ptr<Session> session_for(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sessions.find(key);
    if (it != g_sessions.end()) return it->second;
    auto s = std::make_shared<Session>();
    g_sessions[key] = s;
    return s;
}

/// 推一条给界面。频道就叫 "chat"。
///
/// **按类订阅，不按具体 id**（`http/ws.hpp` 开头那段）：具体的 job id 从来
/// 不从任何接口露出来，只认它的话客户端永远订不上。
void push(const json& msg) {
    json m = msg;
    m["type"] = "chat";
    m["job_id"] = "chat";
    ws::hub().broadcast("chat", m);
}

void push_turn(const std::string& project, const std::string& chat,
               const agent::Turn& t) {
    json m = agent::to_json(t);
    m["event"] = "turn";
    m["project"] = project;
    // **带上是哪一条对话。** 一部片子可以开好几条，界面得知道这一条该贴到
    // 哪一条上——不带的话它会贴到眼前那一条上，而那多半不是它。
    m["chat"] = chat;
    push(m);
}

}  // namespace

ApiResult get_chat_history(const std::string& project, const std::string& chat) {
    if (!chat.empty() && !util::chat_id_ok(chat)) {
        throw ApiError(400,
                       SAYF("对话编号里只能有字母、数字、`_` 和 `-`：%1",
                            chat));
    }
    const auto turns = agent::load_transcript(dir_for(project), chat);
    json arr = json::array();
    for (const auto& t : turns) arr.push_back(agent::to_json(t));
    return {200, {{"turns", arr}, {"project", project}, {"chat", chat}}};
}

ApiResult list_chats(const std::string& project) {
    const auto root = dir_for(project);
    json arr = json::array();

    /// 一条对话在表上长什么样。**名字取第一句人说的话**——那句话就是这条
    /// 对话的来由，而让人另取一个名字是给他加一件事。
    const auto row = [&](const std::string& id) {
        const auto turns = agent::load_transcript(root, id);
        std::string title;
        std::int64_t at = 0;
        for (const auto& t : turns) {
            if (at == 0) at = t.at;
            if (t.role != "user" || t.text.empty()) continue;
            // ⚠️ **截短归界面，不归这儿。** 原来这儿截成十八个字加「…」，于是
            // 顶上那一行明明空着大半条，标题还是「Turn one of today'…」
            // ——截断是在字到界面之前就做完的，界面按宽度 elide 也救不回来
            //（2026-09-23 用户说的）。侧栏、顶上那一行各按自己的宽度截。
            //
            // 这儿只做两件事：换行和连着的空白**并成一个空格**（界面那几处
            // 都是单行字，带着换行会摞成两行），再**夹一道 120 个字**——人
            // 贴进来一整章的话，表上不该拖着几千字。按**字**夹，不按字节：
            // 中文一个字三字节，按字节截会把最后那个字劈成两半。
            bool gap = false;
            std::size_t kept = 0;
            const auto chars = text::utf8_chars(t.text);
            for (const auto& c : chars) {
                const bool blank = c == " " || c == "\n" || c == "\r" || c == "\t" ||
                                   c == "　";
                if (blank) {
                    gap = !title.empty();
                    continue;
                }
                if (kept == 120) {
                    title += "…";
                    break;
                }
                if (gap) title += ' ';
                gap = false;
                title += c;
                ++kept;
            }
            break;
        }
        return json{{"id", id},
                    {"title", title},
                    {"at", turns.empty() ? 0 : turns.back().at},
                    {"turns", static_cast<int>(turns.size())}};
    };

    // 一直以来那一条（空 id）**排在最前面**：它是这部片子说的第一句话。
    // 文件不在就不摆——新建的项目没有它。
    std::error_code ec;
    if (fs::exists(agent::transcript_path(root), ec)) arr.push_back(row(std::string{}));
    for (const auto& id : agent::list_chat_ids(root)) arr.push_back(row(id));
    return {200, {{"chats", arr}, {"project", project}}};
}

ApiResult delete_chat(const std::string& project, const std::string& chat) {
    if (!chat.empty() && !util::chat_id_ok(chat)) {
        throw ApiError(400,
                       SAYF("对话编号里只能有字母、数字、`_` 和 `-`：%1",
                            chat));
    }
    // 正在跑就别删：那一轮还在往这个文件里写，删完它接着写，等于又建了一个
    // 半截的——而人以为自己删掉了。
    if (session_for(project)->busy.load()) {
        throw ApiError(409, SAY("这部片子正在跑一轮，等它说完再删"));
    }
    const bool gone = agent::remove_transcript(dir_for(project), chat);
    return {200, {{"ok", true}, {"gone", gone}}};
}

ApiResult post_chat_fork(const json& body) {
    const std::string project = body.value("project", std::string());
    const std::string from = body.value("chat", std::string());
    const std::string to = body.value("to", std::string());
    const std::int64_t upto = body.value("upto", static_cast<std::int64_t>(0));

    if (!from.empty() && !util::chat_id_ok(from)) {
        throw ApiError(400,
                       SAYF("对话编号里只能有字母、数字、`_` 和 `-`：%1",
                            from));
    }
    // **新的那条不许是空 id。** 空 id 是「一直以来那一条」，分过去就把它盖了。
    if (to.empty() || !util::chat_id_ok(to)) {
        throw ApiError(400,
                       SAYF("新对话的编号里只能有字母、数字、`_` 和 `-`：%1",
                            to));
    }
    if (from == to) throw ApiError(400, SAY("分出来的那条不能还是它自己"));
    if (upto <= 0) throw ApiError(400, SAY("这一条没有时间戳，分不出来"));

    const auto root = dir_for(project);

    // **已经有了就别盖。** 编号是界面按毫秒挑的，撞上的可能性很小，但盖掉
    // 的是一整条说过的话，而它没有回头路。
    std::error_code ec;
    if (fs::exists(agent::transcript_path(root, to), ec)) {
        throw ApiError(409, SAYF("这个编号已经有一条对话了：%1", to));
    }

    const auto turns = agent::load_transcript(root, from);
    std::vector<agent::Turn> kept;
    for (const auto& t : turns) {
        if (t.at > 0 && t.at > upto) break;
        kept.push_back(t);
    }
    if (kept.empty()) {
        throw ApiError(400, SAY("这一条之前什么都没有，分不出来"));
    }

    agent::write_transcript(root, kept, to);
    return {200, {{"chat", to}, {"turns", static_cast<int>(kept.size())}}};
}

ApiResult post_chat_stop(const json& body) {
    const std::string project = body.value("project", std::string());
    auto s = session_for(project);
    if (s->tok) s->tok->request();
    return {200, {{"ok", true}}};
}

ApiResult post_chat_permit(const json& body) {
    const std::string project = body.value("project", std::string());
    const std::uint64_t id = body.value("id", std::uint64_t{0});
    const bool allow = body.value("allow", false);
    auto s = session_for(project);
    {
        std::lock_guard<std::mutex> lk(s->mu);
        // **只认正在等的那一问。** 晚到的一下（上一问早过去了）照实回 409，
        // 别把它算到下一问头上——那等于替人允许了一件他没看见的事。
        if (id == 0 || s->asking != id) {
            throw ApiError(409, SAY("这一问已经过去了"));
        }
        s->answered = true;
        s->allowed = allow;
    }
    s->answer_cv.notify_all();
    return {200, {{"ok", true}}};
}

/// 守着刚派出去的那几件活，干完了往对话里追一条，再让代理跑一轮。
///
/// **判据挂在账本上**（`pipeline::running_work()`），不挂在"我派了什么"上：
/// 一件活可能牵出好几件（出片那一件底下是每一镜），而账本是唯一一本
/// （CLAUDE.md 第十条）。连着几拍都空才算完——两件活之间有空档，只看一拍
/// 会把中间那一下当成"全干完了"。
void watch_and_react(const std::string& project, const std::string& chat,
                     std::shared_ptr<llm::Client> client,
                     std::function<RunDeps()> run_deps, std::shared_ptr<Session> s,
                     const std::string& here, std::shared_ptr<agent::Baseline> base);

/// 这一下让不让做（`LoopHooks::on_permit`）。回空串放行，回一句话就是不让。
///
/// 三档（`Session::permission`）：
///
///   · `auto`  都放行。**默认这一档**：人说"做吧"的时候要的就是它做。
///   · `read`  只放「只看」的那几个（`util::tool_is_read_only`）。别的回一句
///             话告诉模型眼下是只看——它会照实跟人说，而不是假装做了。
///   · `ask`   「只看」的放行；别的**停下来问人**：推一条 `permit` 给界面，
///             在这儿等人点「允许」或「不」（`post_chat_permit`）。人按了停
///             就算不允许，这一轮照常收尾。
///
/// ⚠️ **等的时候这一轮还算"在跑"**（`busy` 还是真的）。不这样的话人在问的
/// 那几秒里又说一句，两轮并着跑——那正是 409 那条规矩要挡的事。
std::string permit(const std::string& project, const std::string& chat,
                   const std::shared_ptr<Session>& s, const std::string& name,
                   const std::string& args) {
    if (util::tool_is_read_only(name)) return {};
    std::string mode;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        mode = s->permission;
    }
    const std::string what = agent::tool_ask(name, args);
    if (mode == "read") {
        return SAYF("眼下是「只看」：不改东西、不派活。这一步（%1）没做——"
                    "要做的话，把输入框右下角的权限换成「自动」或「每步问我」。",
                    what);
    }
    if (mode != "ask") return {};

    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        id = s->next_ask++;
        s->asking = id;
        s->answered = false;
        s->allowed = false;
    }
    push({{"event", "permit"},
          {"project", project},
          {"chat", chat},
          {"id", id},
          {"what", what},
          {"tool", name},
          {"args", args}});

    bool allowed = false;
    {
        std::unique_lock<std::mutex> lk(s->mu);
        // 醒一醒看看人是不是按了停：停是另一个令牌，不走这个条件变量。
        while (!s->answered) {
            s->answer_cv.wait_for(lk, std::chrono::milliseconds(200));
            if (s->tok && s->tok->cancelled()) break;
        }
        allowed = s->answered && s->allowed;
        s->asking = 0;
    }
    push({{"event", "permit_done"}, {"project", project}, {"chat", chat}, {"id", id}});
    if (allowed) return {};
    return SAYF("人没同意这一步（%1），没做。", what);
}

/// 跑一轮对话。`kickoff` 的正文非空时先把它当引擎说的话落一条（做完了、
/// 出错了），带着它身上的东西（这一回做出来的图、片、字，几件成几件没成）。
///
/// `here` 是人这会儿把话说在哪一章上，**一路带下去**：活跑完之后那几轮自动
/// 接续说的还是同一件事，他的眼睛也还落在同一章上。
void run_round(const std::string& project, const std::string& chat,
               std::shared_ptr<llm::Client> client,
               std::function<RunDeps()> run_deps, std::shared_ptr<Session> s,
               const agent::Turn& kickoff, const std::string& here) {
    pipeline::Activity act("llm", project, "", SAY("在想"));

    agent::ToolContext ctx;
    ctx.project = project;
    ctx.here = here;
    ctx.settings = config::runtime().snapshot();
    ctx.client = client;
    ctx.run_deps = run_deps;

    std::string moved_to;
    ctx.on_project_created = [&](const std::string& root) {
        const auto from = dir_for(project);
        const auto to = paths::from_utf8(root);
        agent::write_transcript(to, agent::load_transcript(from, chat), chat);
        std::error_code ec;
        fs::remove(agent::transcript_path(from, chat), ec);
        moved_to = root;
        push({{"event", "project"}, {"project", root}});
    };

    if (!kickoff.text.empty()) {
        agent::Turn t = kickoff;
        t.role = "system";
        t.at = agent::now_ms();
        agent::append_turn(dir_for(project), t, chat);
        push_turn(project, chat, t);
    }

    const auto history = agent::load_transcript(dir_for(project), chat);

    // 已经开口了没有。**一轮里只改一次口**，不是每个字都去设一遍。
    bool speaking = false;

    // 这一句话说出口之前它想了什么。**攒在这儿，落在那条 `Turn` 上**
    //（见 transcript.hpp 上那段：一轮跑完账本就退休了，只走
    // `/api/task/thinking` 的话这几千字再也问不出来）。
    //
    // ⚠️ **一轮里要清好几次。** 一次代理对话是"想→调工具→再想→说"，
    // 每一段思考归它后面那一条 assistant，不清的话第二条会把第一条的思考
    // 也扛上，越滚越长。
    std::string thought;

    agent::LoopHooks hooks;
    hooks.on_turn = [&](const agent::Turn& one) {
        const std::string where = moved_to.empty() ? project : moved_to;
        agent::Turn t = one;
        // 只有场记说的话挂思考。工具回的那条、引擎插的那句都不是它想的。
        //
        // ⚠️ **正文是空的那一条不收账**（那种 assistant 只带 tool_calls，
        // 界面上根本不摆）。收了的话那几段思考跟着一条看不见的行落库，
        // 而人看见的那句回话底下写着「想了 0 字」——它明明想了两分钟。
        // 留着继续攒，归后面那条真说了话的。
        if (t.role == "assistant" && !t.text.empty()) {
            t.thinking = std::move(thought);
            thought.clear();
        }
        agent::append_turn(dir_for(where), t, chat);
        push_turn(where, chat, t);
    };
    // 权限那一关。**对话挪了家（建出项目）之后按新家推**，界面按项目认消息。
    hooks.on_permit = [&](const std::string& name, const std::string& args) {
        return permit(moved_to.empty() ? project : moved_to, chat, s, name, args);
    };
    hooks.on_delta = [&](const std::string& piece) {
        // **第一个字出来就改口。** 在这之前那一行写着「在想」，而人已经看见
        // 正文一个字一个字在往外流了——那一行就成了一句过期的话。
        // 2026-09-21 拿会流的假模型截两张图时看见的。
        if (!speaking) {
            speaking = true;
            act.set_message(SAY("在说"));
        }
        push({{"event", "delta"}, {"text", piece}});
    };
    hooks.on_tool = [&](const std::string& what, const std::string& name,
                        const std::string& args) {
        // 一轮里可能说一句、再调一次工具、再说——所以改回来。
        speaking = false;
        act.set_message(what);
        // **这会儿在动哪一格的哪一章。** 界面拿它在那块面板上说一声
        //「正在动」——不说的话人正盯着一份马上要被顶掉的稿子，稿纸那一格
        // 他还可能正往里写字（方案第四节「别打扰」最后一条）。
        // 章为空的意思是"整格"（比如一次把所有章都写了）。
        push({{"event", "tool"},
              {"text", what},
              {"slot", util::slot_of_tool(name)},
              {"episode", util::episode_of_args(args)},
              // 读一眼还是真要改。**只有"改"那一档才把稿纸锁住**——
              // 为"在读故事"锁一道是白拦。
              {"writes", util::tool_rewrites(name)}});
    };
    // 「先想再说」那一段**接进账本这一件**，不另攒一份。
    //
    // 不接的话它整段丢地上：一个爱想的模型能想上几分钟，而这几分钟里界面
    // 那一行只写着「在想…」，一动不动——看着就是卡死了。账本本来就会报
    // 「想了多少字」和正文（`/api/task/thinking`），接上去那一行自己就会动。
    //
    // **只接不数**：字数由账本那头算（`text::utf8_len`，按字不按字节），
    // 这儿再数一遍就是第二个计数器（CLAUDE.md 第八条）。
    hooks.on_thinking = [&](const std::string& piece) {
        act.task().append_thinking(piece);
        // 攒着，等这一句说完落到那条 `Turn` 上（见上面 `thought`）。
        thought += piece;
        // **从头上截。** 留最近那一截——人回头翻的是"它最后是怎么想的"。
        if (thought.size() > agent::kThinkingKeep) {
            thought.erase(0, thought.size() - agent::kThinkingKeep);
        }
        // **一段一段推上去**，和正文那条 `delta` 一个形状：界面不等这一轮
        // 结束就能把它摆出来，而一个爱想的模型能想上几分钟。
        //
        // ⚠️ **推的是增量不是全文。** 推全文的话，想到第五千字时每一段都要
        // 把前面五千字再发一遍——那条通道会被同一份东西塞满。
        push({{"event", "thinking"}, {"text", piece}});
    };

    try {
        agent::run_turn(*client, ctx, history, *s->tok, hooks);
    } catch (const std::exception& e) {
        agent::Turn t;
        t.role = "assistant";
        t.text = SAYF("出错了：%1", e.what());
        t.level = "error";
        t.at = agent::now_ms();
        agent::append_turn(dir_for(moved_to.empty() ? project : moved_to), t, chat);
        push_turn(moved_to.empty() ? project : moved_to, chat, t);
    }

    const std::string where = moved_to.empty() ? project : moved_to;
    push({{"event", "idle"}, {"project", where}});
    s->busy = false;

    // 这一轮真派出去了活 → 守着它。
    if (!ctx.dispatched.empty() && s->auto_left.load() > 0) {
        std::thread(watch_and_react, where, chat, client, run_deps, s, here,
                    ctx.baseline).detach();
    }
}

void watch_and_react(const std::string& project, const std::string& chat,
                     std::shared_ptr<llm::Client> client,
                     std::function<RunDeps()> run_deps, std::shared_ptr<Session> s,
                     const std::string& here, std::shared_ptr<agent::Baseline> base) {
    using namespace std::chrono_literals;

    int quiet = 0;
    // 兜一个上限：一章出片一个钟头起，四个钟头还没停多半是卡住了，
    // 那时候接着等下去不如把这条守望放掉——人自己看得见任务条。
    const auto deadline = std::chrono::steady_clock::now() + 4h;

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2s);
        if (s->tok && s->tok->cancelled()) return;

        // 人又说话了：这条守望作废——新那一轮自己会守自己的。
        if (s->busy.load()) return;

        const json work = pipeline::running_work();
        bool mine = false;
        for (const auto& w : work) {
            if (w.value("project", std::string()) == project) mine = true;
        }
        quiet = mine ? 0 : quiet + 1;
        // **连着三拍都空才算完。** 两件活之间有空档，只看一拍会把中间那
        // 一下当成"全干完了"，于是报一句"做完了"而它才跑到一半。
        if (quiet >= 3) break;
    }

    if (s->busy.load()) return;
    bool expected = false;
    if (!s->busy.compare_exchange_strong(expected, true)) return;

    const int left = s->auto_left.fetch_sub(1) - 1;
    if (left < 0) {
        s->busy = false;
        return;
    }

    // **这句话是给人看的。** 「先看结果再说话」那半句是规矩，收在提示词里
    // （`[agent] system`）——写在这儿的话它会原样出现在对话里，而那是一句
    // 对着模型说的话，人读到只会觉得莫名其妙。
    // **这一句两头都看得见**：它进对话（人读）也进这一轮的输入（模型读）。
    // 翻了它，德语用户的对话里就不会突兀地冒出一句中文；模型这边不挑语言，
    // 而人这会儿多半正用德语跟它说话。和 `stages/` 那些**只**给模型看的
    // 提示词不是一回事。
    //
    // ⚠️ **做成了什么、没成什么，跟着这一句一起说**（2026-09-23）。原来只有
    // 这一句——没成也是它：「旧手机故事」写大纲派了二十多次，每一次做完都是
    //「跑完了」，而故事一章都没有，人和模型都看不出来。没成的那几件带着原话
    // 接在后面（`work_report`），做出来的图、片、字挂在这一条上（`changed_media`）。
    agent::Turn said;
    said.text = SAY("刚才派出去的活跑完了。");
    if (base) {
        const agent::WorkReport rep = agent::work_report(project, base->task_floor);
        said.text += rep.text;
        said.report = rep.report;
        // 有没成的：这一条标成「做了但没做完」，界面画得出来。
        if (rep.failed) said.level = "warn";
        try {
            said.media = agent::changed_media(paths::from_utf8(project), *base);
        } catch (const std::exception&) {
            // 比不出来就不挂：这一条照样要落，模型照样要接着往下做。
        }
    }
    run_round(project, chat, client, run_deps, s, said, here);
}

ApiResult post_chat(const json& body, std::shared_ptr<llm::Client> client,
                    std::function<RunDeps()> run_deps) {
    const std::string text = body.value("text", std::string());

    // 带进来的文件（`agent/attachments.hpp`）：一件一个路径。**读不懂的当场拒**，
    // 别等模型拿到一个它什么都做不了的路径再来问。
    std::vector<std::string> files;
    if (body.contains("attachments") && body.at("attachments").is_array()) {
        for (const auto& a : body.at("attachments")) {
            const std::string p = a.is_string() ? a.get<std::string>()
                                : a.is_object() ? a.value("path", std::string())
                                                : std::string();
            if (p.empty()) continue;
            if (agent::attachment_kind(p).empty()) {
                throw ApiError(400, SAYF("这种文件还读不了：%1", p));
            }
            files.push_back(p);
        }
    }
    // **只带文件不说话也行**：人丢一份稿子进来，这就是那一句。
    if (text.empty() && files.empty()) throw ApiError(400, SAY("没有要说的话"));

    // 权限那一档。认不出的按默认（自动）——别因为一个拼错的词把人挡在门外。
    std::string permission = body.value("permission", std::string("auto"));
    if (permission != "ask" && permission != "read") permission = "auto";

    const std::string project = body.value("project", std::string());
    // 说给哪一条对话听。**空的就是一直以来那一条**（`<项目目录>/chat.jsonl`）。
    // 带了就得过一道——它会变成文件名，见 `util/chat_id.hpp`。
    const std::string chat = body.value("chat", std::string());
    if (!chat.empty() && !util::chat_id_ok(chat)) {
        throw ApiError(400,
                       SAYF("对话编号里只能有字母、数字、`_` 和 `-`：%1",
                            chat));
    }
    // 人这会儿把话说在哪一章上（输入框顶上那一行写着的那一章）。
    // **空 = 他没指**，那时候按上下文办，和以前一样。
    //
    // 拒掉、不洗：它会**原样贴进系统提示**，而洗出来的那一个未必是他要的
    // 那一章，他还看不出来（同 `chat` 那一条，同 `util/chat_id.hpp`）。
    const std::string here = body.value("here", std::string());
    if (!here.empty() && !util::chapter_key_ok(here)) {
        throw ApiError(400,
                       SAYF("说的是哪一章，得是 `ep01` 那样的键：%1", here));
    }
    // **一部片子还是只跑一轮**，哪怕开着好几条对话：两轮并着跑，各自拼的
    // 状态摘要都是半截的，而那种错不会报，只会让对话越说越不对。
    auto s = session_for(project);

    bool expected = false;
    if (!s->busy.compare_exchange_strong(expected, true)) {
        throw ApiError(409, SAY("这条对话正在跑上一句，等它说完"));
    }

    // 人说的那一条**先落库再开线程**：线程起不来、进程被杀，人说过的话都
    // 不该丢。
    agent::Turn user;
    user.role = "user";
    user.text = text;
    // 界面上那几张缩略图，和模型读的那一段（见 `Turn::for_model`）。
    for (const auto& p : files) {
        user.media.push_back(agent::attachment_media(p));
        if (!user.for_model.empty()) user.for_model += "\n\n";
        user.for_model += agent::attachment_for_model(p);
    }
    user.at = agent::now_ms();
    agent::append_turn(dir_for(project), user, chat);
    push_turn(project, chat, user);

    s->tok = std::make_shared<pipeline::CancelToken>();
    {
        std::lock_guard<std::mutex> lk(s->mu);
        s->permission = permission;
    }

    // **人一说话就把预算填满。** 人在旁边盯着的时候，活干完了接着往下做
    // 是他要的；而他走开之后那几轮自动接续用完就停下来等。
    // `auto` 那一档给得多——那正是"放手跑"的意思。
    s->auto_left = body.value("mode", std::string()) == "auto" ? 30 : 4;

    std::thread(run_round, project, chat, client, run_deps, s, agent::Turn{}, here)
        .detach();

    return {202, {{"started", true}, {"project", project}}};
}

}  // namespace changji::http
