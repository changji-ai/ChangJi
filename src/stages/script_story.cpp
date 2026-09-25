#include "stages/script_story.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "stages/script.hpp"
// 画风那两句和正片那一份共用——同一部电影不该因为换了条写作路线就换语气。
#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

/// 和 script.cpp 的同名函数一致：秒数印成整数，60.0 印成 "60"。
/// std::to_string 对 double 给六位小数，会印成 "60.000000"。
std::string format_f0(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return std::string(buf);
}

/// 取 [from, to) 那一段，**按 UTF-8 字符**。
///
/// 按字节切会把汉字劈成三段，截出来的是非法 UTF-8——它会一路流到提示词、
/// 分镜和字幕，最后表现成「整轨字幕不显示」这种离得很远的故障。
std::string slice_chars(const std::string& s, int from, int to) {
    const std::vector<std::string> chars = text::utf8_chars(s);
    const int n = static_cast<int>(chars.size());
    const int a = std::clamp(from, 0, n);
    const int b = std::clamp(to, a, n);
    std::string out;
    for (int i = a; i < b; ++i) out += chars[i];
    return out;
}

/// 章节在 story.chapters 里的下标，找不到返回 -1。
int index_of(const Story& story, const std::string& chapter_id) {
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        if (story.chapters[i].chapter_id == chapter_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

std::vector<std::string> episode_chapters(const Story& story,
                                          const EpisodePlan& plan) {
    std::vector<std::string> out;
    const int a = index_of(story, plan.from_chapter);
    if (a < 0) return out;
    // 末章找不到时就只算起始那一章。章节计划和故事对不上是坏数据，
    // 但不该让写剧本这件事整个做不成。
    const int b0 = index_of(story, plan.to_chapter);
    const int b = b0 < a ? a : b0;
    for (int i = a; i <= b; ++i) out.push_back(story.chapters[i].chapter_id);
    return out;
}

std::vector<Scene> episode_scenes(const Story& story, const EpisodePlan& plan) {
    std::vector<Scene> out;
    const int a = index_of(story, plan.from_chapter);
    if (a < 0) return out;
    const int b0 = index_of(story, plan.to_chapter);
    const int b = b0 < a ? a : b0;
    for (int i = a; i <= b; ++i) {
        const Chapter& c = story.chapters[i];
        const int len = c.text_len();
        const int from = (i == a) ? plan.from_char : 0;
        const int to = (i == b) ? plan.to_char : len;
        for (const Scene& s : c.scenes) {
            // 真的压上了才算。**端点相碰不算**：上一条正好收在这一场开
            // 头的时候，两条会同时把它列出来，而下一条才是真要拍它的那个。
            if (s.to_char <= from || s.from_char >= to) continue;
            out.push_back(s);
        }
    }
    return out;
}

std::string episode_text(const Story& story, const EpisodePlan& plan) {
    const int a = index_of(story, plan.from_chapter);
    if (a < 0) return {};
    const int b0 = index_of(story, plan.to_chapter);
    const int b = b0 < a ? a : b0;

    std::string out;
    for (int i = a; i <= b; ++i) {
        const Chapter& c = story.chapters[i];
        const int len = c.text_len();
        // 头一章从 from_char 起；末章到 to_char 止；中间的整章都要。
        const int from = (i == a) ? plan.from_char : 0;
        const int to = (i == b) ? plan.to_char : len;
        const std::string part = slice_chars(c.text, from, to);
        if (text::strip_ws(part).empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += part;
    }
    return out;
}

std::string script_tail(const std::string& script) {
    const std::vector<std::string> chars = text::utf8_chars(script);
    const std::size_t n = chars.size();
    if (n <= prompt::script_story::kPrevTailMaxChars) return text::strip_ws(script);

    std::string tail;
    for (std::size_t i = n - prompt::script_story::kPrevTailMaxChars; i < n; ++i) {
        tail += chars[i];
    }
    // 从半行中间开始的话，模型会把那半句当成一个完整的拍子去接。
    // 有换行就从第一个换行之后起。
    const std::size_t nl = tail.find('\n');
    if (nl != std::string::npos && nl + 1 < tail.size()) {
        tail = tail.substr(nl + 1);
    }
    return text::strip_ws(tail);
}

EpisodePlan chapter_plan(const Story& story, const std::string& chapter_id,
                         double target_duration_s) {
    EpisodePlan p;
    p.from_chapter = chapter_id;
    p.to_chapter = chapter_id;
    p.from_char = 0;
    p.target_duration_s = target_duration_s;
    const Chapter* c = story.chapter_by_id(chapter_id);
    if (c == nullptr) return p;
    p.title = c->title;
    p.to_char = c->text_len();
    // 停在哪：最后一场的 turn 是这一章的收口；没有场（粘贴导入的故事）
    // 就拿最后一条有说法的钩子；再没有就空着，提示词写「最后一场的落点」。
    for (auto it = c->scenes.rbegin(); it != c->scenes.rend(); ++it) {
        if (!text::strip_ws(it->turn).empty()) {
            p.hook = it->turn;
            break;
        }
    }
    if (p.hook.empty()) {
        for (auto it = c->hooks.rbegin(); it != c->hooks.rend(); ++it) {
            if (!text::strip_ws(it->text).empty()) {
                p.hook = it->text;
                break;
            }
        }
    }
    return p;
}

std::vector<ScenePlan> chapter_scene_plan(const Story& story,
                                          const EpisodePlan& plan) {
    std::vector<std::pair<std::string, int>> scene_chars;
    // 每一场的原文和落点，和 scene_chars 一一对应（一场一次调用时单给它看）。
    std::vector<std::pair<std::string, std::string>> body_stop;
    const int a = index_of(story, plan.from_chapter);
    if (a >= 0) {
        const int b0 = index_of(story, plan.to_chapter);
        const int b = b0 < a ? a : b0;
        for (int i = a; i <= b; ++i) {
            const Chapter& c = story.chapters[static_cast<std::size_t>(i)];
            const int len = c.text_len();
            const int from = (i == a) ? plan.from_char : 0;
            const int to = (i == b) ? plan.to_char : len;
            const std::vector<std::string> glyphs = text::utf8_chars(c.text);
            for (const Scene& s : c.scenes) {
                if (s.to_char <= from || s.from_char >= to) continue;
                std::string where;
                if (!s.where.empty()) where += s.where;
                if (!s.pov.empty()) where += "。跟着" + s.pov + "走";
                if (!s.goal.empty()) where += "：他要" + s.goal;
                if (!s.obstacle.empty()) where += "；拦着他的是" + s.obstacle;
                if (!s.turn.empty()) where += "。收在：" + s.turn;
                const int lo = std::max(s.from_char, from);
                const int hi = std::min(s.to_char, to);
                scene_chars.emplace_back(where, std::max(0, hi - lo));
                std::string body;
                for (int k = std::max(0, lo);
                     k < hi && k < static_cast<int>(glyphs.size()); ++k) {
                    body += glyphs[static_cast<std::size_t>(k)];
                }
                body_stop.emplace_back(std::move(body), s.turn);
            }
        }
    }
    if (scene_chars.empty()) {
        // 没有场：整章一场，篇幅按正文（没正文按梗概）。
        std::string body = episode_text(story, plan);
        if (body.empty()) {
            for (const auto& id : episode_chapters(story, plan)) {
                const Chapter* c = story.chapter_by_id(id);
                if (c != nullptr) body += c->summary;
            }
        }
        scene_chars.emplace_back("", static_cast<int>(text::utf8_len(body)));
    }
    std::vector<ScenePlan> out = scene_plan_for_chapter(scene_chars);
    for (std::size_t i = 0; i < out.size() && i < body_stop.size(); ++i) {
        out[i].body = std::move(body_stop[i].first);
        out[i].stop = std::move(body_stop[i].second);
    }
    return out;
}

std::string truncate_middle(const std::string& body, std::size_t limit) {
    const std::vector<std::string> chars = text::utf8_chars(body);
    if (chars.size() <= limit || limit < 20) {
        return limit < 20 ? text::truncate_utf8(body, limit) : body;
    }
    const std::size_t head_n = limit * 6 / 10;
    const std::size_t tail_n = limit - head_n;
    std::string out;
    for (std::size_t i = 0; i < head_n; ++i) out += chars[i];
    out += "\n……（中间略）……\n";
    for (std::size_t i = chars.size() - tail_n; i < chars.size(); ++i) out += chars[i];
    return out;
}

// 这里每一处小标题都写死「这一章」。2026-09-16 只留章模式那天，这个函数里
// 按 `chapter_scenes == nullptr` 分岔的那一半（【这一集】【上一集是这么结束
// 的】【这一集的场】、正文按 kEpisodeMaxChars 截尾）没跟着删，留成了残肢：
// 生产上唯一的调用点 build_chapter_script_prompt 一直传着场次清单，走不进
// 去；只有单元测试靠默认参数摸得着它，于是测出来的是发不出去的字。
// 2026-09-18 定成电影平台时连参数的默认值一起拔掉——场次清单改成引用，
// 「有另一种模式」这件事从签名上就不成立了；喂那半边的
// `episode_max_chars` 同一天从 prompts.toml 里删掉，所以 kEpisodeMaxChars
// 今天已经不存在了，别照着这段话去找。
std::string render_script_context(const Story& story, const EpisodePlan& plan,
                                  const std::string& previous_tail,
                                  const std::vector<ScenePlan>& chapter_scenes) {
    std::string out;

    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.genre.empty()) out += "【题材】" + story.genre + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    if (!out.empty()) out += "\n";

    out += "【人物】\n";
    for (const auto& c : story.characters) {
        out += c.name;
        if (!c.identity.empty()) out += "：" + c.identity;
        out += "。";
        if (!c.want.empty()) out += "他要的是：" + c.want + "。";
        if (!c.fear.empty()) out += "他怕的是：" + c.fear + "。";
        if (!c.voice.empty()) out += "说话：" + c.voice + "。";
        if (!c.arc.empty()) out += "他会从" + c.arc + "。";
        out += "\n";
    }

    if (!story.relations.empty()) {
        out += "\n【关系】\n";
        for (const auto& r : story.relations) {
            out += r.a + " — " + r.b;
            if (!r.kind.empty()) out += "：" + r.kind;
            out += "。";
            if (!r.tension.empty()) out += r.tension + "。";
            out += "\n";
        }
    }

    // 本章在故事里的下标。前情提要和下面筛地点都要用。
    const int first = [&] {
        for (std::size_t i = 0; i < story.chapters.size(); ++i) {
            if (story.chapters[i].chapter_id == plan.from_chapter) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }();

    // ---- 地方：**只发这一章用得着的那几个** ----
    //
    // 同 stages/chapter_write.cpp 里【地方】那一段，理由一样：发全片清单
    // 等于请模型跑遍全城，然后只能拿话去拦。章自己带着 locations 名单，
    // 照它筛。筛不出来（大纲那条路上这一栏可空）就照旧给全份。
    if (!story.locations.empty()) {
        std::vector<const models::StoryLocation*> use;
        if (first >= 0 && first < static_cast<int>(story.chapters.size())) {
            const Chapter& me = story.chapters[static_cast<std::size_t>(first)];
            for (const auto& l : story.locations) {
                for (const auto& want : me.locations) {
                    if (l.name == want) { use.push_back(&l); break; }
                }
            }
        }
        const bool all = use.empty();
        if (all) {
            for (const auto& l : story.locations) use.push_back(&l);
        }
        out += all
                   ? "\n【地方】（整个故事的清单；场次头只能使用这里的名字，"
                     "一字不改，这一章用得着哪一两个就只写那一两个）\n"
                   : "\n【地方】（这一章用得着的；场次头只能使用这里的名字，"
                     "一字不改）\n";
        for (const models::StoryLocation* l : use) {
            out += l->name;
            if (!l->what.empty()) out += "：" + l->what;
            out += "。";
            if (!l->when.empty()) out += l->when + "。";
            out += "\n";
        }
    }

    // 前情提要：这一章**之前**那些章，每章一句。
    //
    // 这是治失忆的那一味药。老路线带的是前三章的原文，取三章、截 4000 字符，
    // 写第五章时第一章已经不在上下文里了。压缩成每章一句之后，二十章也塞得下。
    //
    // **只取之前的。** 把后面的章也塞进去，模型会把还没发生的事当成已经
    // 发生的写——这个错在成片里表现成"剧透了自己"。
    // **从最近的章往前攒，攒满为止。** 原来是从第 1 章往后拼再截尾——
    // 章一多，截掉的正是最近几章，而那几章恰恰是连贯最需要的。
    std::vector<std::string> recap_lines;
    for (int i = 0; i < first; ++i) {
        const Chapter& c = story.chapters[i];
        std::string line = std::to_string(i + 1) + " " + c.title;
        if (!c.summary.empty()) line += "：" + text::collapse_ws(c.summary);
        recap_lines.push_back(line + "\n");
    }
    std::string recap;
    {
        std::size_t used = 0;
        std::vector<std::string> kept;
        for (auto it = recap_lines.rbegin(); it != recap_lines.rend(); ++it) {
            const std::size_t n = text::utf8_len(*it);
            if (used + n > prompt::script_story::kRecapMaxChars && !kept.empty()) break;
            kept.push_back(*it);
            used += n;
        }
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) recap += *it;
        if (kept.size() < recap_lines.size()) {
            recap = "（更早的几章略）\n" + recap;
        }
    }
    if (!recap.empty()) {
        out += "\n【前情提要】（之前发生过的事，不要再演一遍）\n";
        out += text::truncate_utf8(recap, prompt::script_story::kRecapMaxChars + 20);
    }

    const std::string tail = text::strip_ws(previous_tail);
    if (!tail.empty()) {
        out += "\n【上一章是这么结束的】\n";
        out += text::truncate_utf8(tail, prompt::script_story::kPrevTailMaxChars);
        out += "\n";
    }

    // **这一章在哪、跟着谁。** 正文里这些是化在叙述里的，模型顺着读容易
    // 把地点写丢——而每一镜都要照着地点画，丢了就镜镜不一样。
    // 场次清单就是 JSON 里 scenes 的形状，一场一行，带地板。
    out += prompt::script_story::kScenesHead;
    for (std::size_t i = 0; i < chapter_scenes.size(); ++i) {
        const ScenePlan& p = chapter_scenes[i];
        // 场号用这一场自己的：一场一次调用时只传一场进来，下标永远是 0。
        const int number = p.number > 0 ? p.number : static_cast<int>(i + 1);
        out += prompt::script_story::kSceneLinePre + std::to_string(number) +
               prompt::script_story::kSceneLineMid;
        if (!p.where.empty()) out += p.where + "。";
        out += prompt::script_story::kSceneLineBeatsPre +
               std::to_string(p.min_beats) +
               prompt::script_story::kSceneLineBeatsPost + "\n";
    }

    // **一章拆成一场一次调用时，说清这一次只写哪一场。** 原来拆开时提示词一个
    // 字没动：清单上只剩一场、【这一章】还是整章、要求里写着「一章是一个完整
    // 的故事」——2026-09-25 实跑，只有一场的那一次把整章四场全写进了 s1，后面
    // 三次再各写一遍（见 prompts.toml 里 one_scene_pre 上那段）。
    const ScenePlan* one = chapter_scenes.size() == 1 && chapter_scenes[0].of > 1
                               ? &chapter_scenes[0]
                               : nullptr;
    if (one != nullptr) {
        out += prompt::script_story::kOneScenePre + std::to_string(one->of) +
               prompt::script_story::kOneSceneMid + std::to_string(one->number) +
               prompt::script_story::kOneScenePost;
    }

    // 这一章要拍的。有正文用正文，没有就退回章节梗概。
    //
    // ⚠️ **退回梗概那一支今天只剩老项目走得到**（盘上那份 story.plan 里的
    // 条目，章节记录上没挂章）。挂着章的那些在 http/scripting.cpp 就被挡下
    // 了：没正文不写剧本。这儿原来写的是「大纲阶段就能先把剧本写出来，
    // 不必等逐章展开」——那条路 2026-09-19 用户否掉了：「我一个字没写的章节
    // 哪来的剧本和分镜」。三五句梗概扩出来的三千字，是一份没人写过的戏。
    out += "\n【这一章】\n";
    const std::string body = episode_text(story, plan);
    if (!body.empty()) {
        // 掐中间不掐尾巴：尾巴是钩子所在。
        out += truncate_middle(body, prompt::script_story::kChapterMaxChars);
    } else {
        for (const auto& id : episode_chapters(story, plan)) {
            const Chapter* c = story.chapter_by_id(id);
            if (c == nullptr) continue;
            out += c->title;
            if (!c->summary.empty()) out += "：" + text::collapse_ws(c->summary);
            out += "\n";
        }
    }

    // 一场一次调用：这一场自己那一段单独再给一遍——要拍的是它。
    if (one != nullptr && !one->body.empty()) {
        out += prompt::script_story::kOneSceneBodyHead;
        out += truncate_middle(one->body, prompt::script_story::kChapterMaxChars);
    }

    // 停在哪。**这一条是老路线完全没有的**：原来模型不知道自己该停在
    // 什么地方，结尾全凭它自己找一个落点，下一章接不接得上看运气。
    // 没有钩子也要写一句，规则里说了「停在【要停在】写的地方」。
    //
    // 一场一次调用、又不是最后一场：停在**这一场**的落点。给整章的钩子的话，
    // 模型就一路写到整章的结尾。
    if (one != nullptr && one->number < one->of) {
        out += prompt::script_story::kSceneStopHead;
        out += text::strip_ws(one->stop).empty() ? prompt::script_story::kNoStopScene
                                                 : one->stop;
    } else {
        out += "\n【这一章要停在】";
        out += plan.hook.empty() ? prompt::script_story::kNoHookChapter : plan.hook;
    }
    out += "\n";

    return out;
}

std::string build_chapter_script_prompt(
    const Story& story, const EpisodePlan& plan, StyleLine style_line,
    const std::vector<std::string>& characters,
    const std::string& previous_tail, const std::vector<ScenePlan>& scenes) {
    const char* hint = style_line == StyleLine::ANIME
                           ? prompt::script::kHintAnime
                           : prompt::script::kHintRealistic;

    std::string out;
    out += prompt::script_story::kSeg0Chapter;
    out += hint;
    out += prompt::script_story::kSeg2Chapter;
    out += prompt::script_story::kRulesChapter;

    if (!characters.empty()) {
        out += prompt::script_story::kCharsPre;
        for (std::size_t i = 0; i < characters.size(); ++i) {
            if (i > 0) out += "、";
            out += characters[i];
        }
        out += prompt::script_story::kCharsPost;
    }
    out += prompt::script_story::kContextHeadChapter;
    out += render_script_context(story, plan, previous_tail, scenes);
    out += prompt::script_story::kTail;
    return out;
}

}  // namespace changji::stages
