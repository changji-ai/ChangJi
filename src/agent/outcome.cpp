#include "agent/outcome.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <exception>
#include <functional>
#include <system_error>
#include <vector>

#include "agent/tools.hpp"
#include "agent/transcript.hpp"
#include "media/assemble.hpp"
#include "models/project.hpp"
#include "pipeline/task_board.hpp"
#include "util/chapter_word.hpp"
#include "util/fs_time.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace changji::agent {

namespace {

/// 界面上这几样东西各摆多少。**截的是最旧的那几件**：一章出了十七张首帧，
/// 人最想看的是刚出的那几张。
constexpr std::size_t kMaxImages = 24;
constexpr std::size_t kMaxVideos = 10;
constexpr std::size_t kMaxTexts = 12;
/// 一段字最多留多少个**字**。一章正文三五千字，二十章就是十万——这份东西
/// 要落进 chat.jsonl，而那个文件每次打开这条对话都整条读回来。
/// 截掉的那半在那一格里看得全（消息末尾那颗「看看… →」）。
constexpr std::size_t kTextKeep = 6000;

const i18n::Audience kHuman = i18n::Audience::human();

/// 一个相对路径的文件在不在、mtime 是多少。不在回 false。
bool stat_file(const fs::path& root, const std::string& rel, double& mtime) {
    if (rel.empty()) return false;
    std::error_code ec;
    const fs::path p = root / paths::from_utf8(rel);
    if (!fs::is_regular_file(p, ec)) return false;
    mtime = util::file_mtime_unix(p);
    return true;
}

void put_file(Baseline& b, const fs::path& root, const std::string& key,
              const std::optional<std::string>& rel) {
    if (!rel.has_value()) return;
    double mt = 0.0;
    if (!stat_file(root, *rel, mt)) return;
    b.pieces[key] = Piece{*rel, mt, 0};
}

void put_text(Baseline& b, const std::string& key, const std::string& content) {
    // **空的不进盘面。** 一章还没正文和"这一章不在"是一回事——做完了它有了，
    // 就算新出的；反过来从有到没（删了），不是这儿要报的"做出了什么"。
    if (content.empty()) return;
    b.pieces[key] = Piece{{}, 0.0, std::hash<std::string>{}(content)};
}

/// 分镜表的指纹。**只认人会当成"分镜改了"的那几栏**：状态、路径、配音
/// 回填的实长每出一次片都变，算进来的话出完片那一条会莫名其妙地多一张
///「分镜 · 第 1 章」，而人一个字都没改。时长也不算——配音会把镜头压短
///（CLAUDE.md 第十½条），那是出片的副作用，不是改了分镜。
std::string board_sig(const models::Episode& e) {
    std::string s;
    for (const auto& shot : e.shots) {
        const json j = shot;
        s += shot.shot_id;
        s += '|';
        s += j.value("shot_size", std::string());
        s += '|';
        s += j.value("camera_move", std::string());
        s += '|';
        s += shot.visual_desc;
        s += '|';
        s += shot.first_frame_prompt;
        for (const auto& d : shot.dialogue) {
            s += '|';
            s += d.text;
        }
        s += '\n';
    }
    return s;
}

/// 一格参考图的名字：「董平 · 正面」。
std::string ref_title(const std::string& name, const std::string& slot) {
    std::string word = slot == "front"           ? SAY("正面")
                       : slot == "three_quarter" ? SAY("四分之三侧面")
                       : slot == "back"          ? SAY("背面")
                                                 : SAY("空景图");
    return name + " · " + word;
}

/// 一镜的名字：「第 1 章 sh003」。
std::string shot_title(const std::string& episode_id, const std::string& shot_id) {
    return util::chapter_word(episode_id, kHuman) + " " + util::short_shot_id(shot_id);
}

/// output/ 底下一条片子的名字。
std::string output_title(const std::string& rel) {
    const fs::path p = paths::from_utf8(rel);
    const std::string stem = paths::to_utf8(p.stem());
    const std::string dir = paths::to_utf8(p.parent_path().filename());
    // 整部电影落在 output/final/（`pipeline/film_join.cpp`）。
    if (dir == "final") return SAY("整部电影");
    // 前 n 分钟落在 output/preview/<章号>.mp4（`pipeline/preview.cpp`）。
    if (dir == "preview") return util::chapter_word(stem, kHuman) + " · " + SAY("预告");
    // 章成片：**认领规则只有一份**（`media::episode_of_output`，CLAUDE.md
    // 第八条那张表第一行），这儿照调，不自己猜。
    if (const auto ep = media::episode_of_output(stem)) {
        return util::chapter_word(*ep, kHuman) + " · " + SAY("成片");
    }
    return paths::to_utf8(p.filename());
}

/// 截到 `kTextKeep` 个字。
std::string keep_text(const std::string& s) {
    if (text::utf8_len(s) <= kTextKeep) return s;
    return text::truncate_utf8(s, kTextKeep) + "\n…";
}

/// 那段字**现在**是什么样——走和 `*_read` 那几个工具同一份（给人看的那一遍）。
std::string read_now(const fs::path& root, const std::string& tool, const json& args) {
    ToolContext ctx;
    ctx.project = paths::to_utf8(root);
    return run_tool(ctx, tool, args.dump(), kHuman);
}

}  // namespace

json media_item(const std::string& kind, const std::string& rel,
                const std::string& title, const std::string& text,
                const std::string& poster) {
    json j;
    j["kind"] = kind;
    if (!rel.empty()) j["rel"] = rel;
    if (!title.empty()) j["title"] = title;
    if (!text.empty()) j["text"] = text;
    if (!poster.empty()) j["poster"] = poster;
    return j;
}

// 盘面里的键：
//
//     t:outline              大纲（一句话 + 章节表）
//     t:cast                 设定（人物、场景的文字那一半）
//     t:chapter:<ch01>       一章正文
//     t:script:<ep01>        一章剧本
//     t:board:<ep01>         一章分镜表
//     i:ref:<c_id>:<slot>    一张参考图（场景的 slot 是 empty）
//     i:frame:<shot_id>      一镜首帧
//     v:out:<rel>            output/ 底下一条片子（章成片、预告、整部）
//     v:clip:<shot_id>       一镜的片子
//
// 前缀那一个字母就是它归哪一族（字 / 图 / 片），`changed_media` 按它分。
Baseline take_baseline(const fs::path& root) {
    Baseline b;
    b.task_floor = task_floor_now();
    b.at = now_ms();
    const models::ProjectStore store(root);

    try {
        const models::Story st = store.load_story();
        std::string outline = st.logline;
        for (const auto& c : st.chapters) {
            outline += "\n" + c.chapter_id + c.title + c.summary;
            put_text(b, "t:chapter:" + c.chapter_id, c.text);
        }
        if (!st.chapters.empty()) put_text(b, "t:outline", outline);
    } catch (const std::exception&) {}

    try {
        const models::AssetLibrary assets = store.load_assets();
        std::string cast;
        for (const auto& kv : assets.characters) {
            const auto& c = kv.second;
            cast += c.name + c.appearance.identity + c.appearance.face +
                    c.appearance.attire + "\n";
            put_file(b, root, "i:ref:" + c.char_id + ":front", c.ref_front);
            put_file(b, root, "i:ref:" + c.char_id + ":three_quarter", c.ref_three_quarter);
            put_file(b, root, "i:ref:" + c.char_id + ":back", c.ref_back);
        }
        for (const auto& kv : assets.locations) {
            const auto& l = kv.second;
            cast += l.name + l.space + "\n";
            put_file(b, root, "i:ref:" + l.location_id + ":empty", l.ref_empty);
        }
        put_text(b, "t:cast", cast);
    } catch (const std::exception&) {}

    try {
        const models::Project p = store.load_project();
        for (const auto& e : p.episodes) {
            put_text(b, "t:script:" + e.episode_id, e.script);
            put_text(b, "t:board:" + e.episode_id, board_sig(e));
            for (const auto& s : e.shots) {
                put_file(b, root, "i:frame:" + s.shot_id, s.frame_path);
                put_file(b, root, "v:clip:" + s.shot_id, s.video_path);
            }
        }
    } catch (const std::exception&) {}

    // 成片目录。三处：章成片在顶层，前 n 分钟在 preview/，整部在 final/。
    const fs::path out = store.paths().output();
    for (const fs::path& dir : {out, out / "preview", out / "final"}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            if (!f.is_regular_file(ec)) continue;
            std::string ext = paths::to_utf8(f.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".mp4") continue;
            const std::string rel = store.paths().rel(f.path());
            b.pieces["v:out:" + rel] = Piece{rel, util::file_mtime_unix(f.path()), 0};
        }
    }
    return b;
}

namespace {

/// `changed_media` 的本体。`only` 非空时只看键以它开头的那几样——
/// `outputs_media` 要的只是成片目录，别为它把整本正文和剧本现读一遍。
json media_since(const fs::path& root, const Baseline& before, const std::string& only) {
    const Baseline after = take_baseline(root);
    const models::ProjectStore store(root);

    // 名字要从模型里取（谁的参考图、哪一章的哪一镜），先读一遍。
    models::AssetLibrary assets;
    models::Project project;
    try { assets = store.load_assets(); } catch (const std::exception&) {}
    try { project = store.load_project(); } catch (const std::exception&) {}

    const auto name_of = [&](const std::string& id) {
        if (const auto it = assets.characters.find(id); it != assets.characters.end())
            return it->second.name;
        if (const auto it = assets.locations.find(id); it != assets.locations.end())
            return it->second.name;
        return id;
    };
    // 一镜在哪一章、首帧在哪儿。
    const auto shot_of = [&](const std::string& shot_id) -> const models::Shot* {
        for (const auto& e : project.episodes) {
            for (const auto& s : e.shots) {
                if (s.shot_id == shot_id) return &s;
            }
        }
        return nullptr;
    };
    const auto episode_of_shot = [&](const std::string& shot_id) {
        for (const auto& e : project.episodes) {
            for (const auto& s : e.shots) {
                if (s.shot_id == shot_id) return e.episode_id;
            }
        }
        return std::string();
    };
    // 封面：那一镜的首帧；章成片取那一章第一张有的首帧。
    const auto frame_of = [&](const std::string& shot_id) {
        const models::Shot* s = shot_of(shot_id);
        double mt = 0.0;
        if (s && s->frame_path && stat_file(root, *s->frame_path, mt)) return *s->frame_path;
        return std::string();
    };
    const auto first_frame_of = [&](const std::string& episode_id) {
        for (const auto& e : project.episodes) {
            if (!episode_id.empty() && e.episode_id != episode_id) continue;
            for (const auto& s : e.shots) {
                double mt = 0.0;
                if (s.frame_path && stat_file(root, *s.frame_path, mt)) return *s.frame_path;
            }
            if (!episode_id.empty()) break;
        }
        return std::string();
    };

    struct Found {
        std::string key;
        const Piece* piece;
    };
    std::vector<Found> texts, images, videos;
    for (const auto& [key, piece] : after.pieces) {
        if (!only.empty() && key.rfind(only, 0) != 0) continue;
        const auto was = before.pieces.find(key);
        const bool changed =
            was == before.pieces.end() ||
            (key[0] == 't' ? was->second.sig != piece.sig
                           : was->second.rel != piece.rel ||
                                 was->second.mtime != piece.mtime);
        if (!changed) continue;
        (key[0] == 't' ? texts : key[0] == 'i' ? images : videos)
            .push_back({key, &piece});
    }

    // 图和片**新的在前**，截掉的是最旧的那几件。字按键排（章序）就对。
    const auto newest_first = [](const Found& a, const Found& b) {
        return a.piece->mtime > b.piece->mtime;
    };
    std::stable_sort(images.begin(), images.end(), newest_first);
    // 片子：成片目录那几条排在一镜一镜的前面——那是"这一回的产出"本身。
    std::stable_sort(videos.begin(), videos.end(), [](const Found& a, const Found& b) {
        const bool ao = a.key.rfind("v:out:", 0) == 0;
        const bool bo = b.key.rfind("v:out:", 0) == 0;
        if (ao != bo) return ao;
        return a.piece->mtime > b.piece->mtime;
    });
    // 字：大纲、设定排最前，然后一章一章。
    std::stable_sort(texts.begin(), texts.end(), [](const Found& a, const Found& b) {
        const auto rank = [](const std::string& k) {
            return k == "t:outline" ? 0 : k == "t:cast" ? 1
                 : k.rfind("t:chapter:", 0) == 0 ? 2
                 : k.rfind("t:script:", 0) == 0 ? 3 : 4;
        };
        return rank(a.key) < rank(b.key);
    });

    json out = json::array();

    std::size_t n = 0;
    for (const auto& f : texts) {
        if (n++ >= kMaxTexts) break;
        const std::string& k = f.key;
        const auto tail = [&](const char* prefix) {
            return k.substr(std::string(prefix).size());
        };
        std::string title, body;
        if (k == "t:outline") {
            title = SAY("大纲");
            body = read_now(root, "story_read", json::object());
        } else if (k == "t:cast") {
            title = SAY("设定");
            // 设定没有"给人看的那一段"现成可调（`assets_read` 只报有没有图），
            // 在这儿摊开：名字，后面是长相那几栏。
            for (const auto& kv : assets.characters) {
                const auto& a = kv.second.appearance;
                std::string line;
                for (const std::string* s : {&a.identity, &a.face, &a.body, &a.attire}) {
                    if (s->empty()) continue;
                    if (!line.empty()) line += " ";
                    line += *s;
                }
                body += SAYF("%1：%2", kv.second.name, line) + "\n";
            }
            for (const auto& kv : assets.locations) {
                body += SAYF("%1：%2", kv.second.name, kv.second.space) + "\n";
            }
        } else if (k.rfind("t:chapter:", 0) == 0) {
            const std::string id = tail("t:chapter:");
            title = SAY("正文") + " · " + util::chapter_word(id, kHuman);
            body = read_now(root, "story_read", {{"chapter_id", id}});
        } else if (k.rfind("t:script:", 0) == 0) {
            const std::string id = tail("t:script:");
            title = SAY("剧本") + " · " + util::chapter_word(id, kHuman);
            body = read_now(root, "script_read", {{"episode_id", id}});
        } else {
            const std::string id = tail("t:board:");
            title = SAY("分镜") + " · " + util::chapter_word(id, kHuman);
            body = read_now(root, "shots_read", {{"episode_id", id}});
        }
        out.push_back(media_item("text", {}, title, keep_text(text::strip_ws(body))));
    }

    n = 0;
    for (const auto& f : images) {
        if (n++ >= kMaxImages) break;
        const std::string& k = f.key;
        std::string title;
        if (k.rfind("i:ref:", 0) == 0) {
            const std::string rest = k.substr(6);            // <id>:<slot>
            const auto colon = rest.rfind(':');
            title = ref_title(name_of(rest.substr(0, colon)), rest.substr(colon + 1));
        } else {
            const std::string sid = k.substr(8);             // i:frame:
            title = shot_title(episode_of_shot(sid), sid) + " · " + SAY("首帧");
        }
        out.push_back(media_item("image", f.piece->rel, title));
    }

    n = 0;
    for (const auto& f : videos) {
        if (n++ >= kMaxVideos) break;
        const std::string& k = f.key;
        if (k.rfind("v:out:", 0) == 0) {
            const std::string rel = f.piece->rel;
            const std::string stem = paths::to_utf8(paths::from_utf8(rel).stem());
            const std::string dir =
                paths::to_utf8(paths::from_utf8(rel).parent_path().filename());
            std::string ep;
            if (dir == "preview") ep = stem;
            else if (const auto e = media::episode_of_output(stem)) ep = *e;
            // 整部电影（final/）ep 是空的：封面取全片第一张。
            out.push_back(media_item("video", rel, output_title(rel), {},
                                     first_frame_of(ep)));
        } else {
            const std::string sid = k.substr(7);             // v:clip:
            out.push_back(media_item("video", f.piece->rel,
                                     shot_title(episode_of_shot(sid), sid),
                                     {}, frame_of(sid)));
        }
    }
    return out;
}

}  // namespace

json changed_media(const fs::path& root, const Baseline& before) {
    return media_since(root, before, {});
}

json reference_media(const fs::path& root, const std::string& id,
                     const std::string& slot) {
    try {
        const models::AssetLibrary lib = models::ProjectStore(root).load_assets();
        std::optional<std::string> rel;
        std::string name = id;
        if (const auto it = lib.locations.find(id); it != lib.locations.end()) {
            rel = it->second.ref_empty;
            name = it->second.name;
        } else if (const auto ic = lib.characters.find(id); ic != lib.characters.end()) {
            const auto& c = ic->second;
            rel = slot == "three_quarter" ? c.ref_three_quarter
                : slot == "back"          ? c.ref_back
                                          : c.ref_front;
            name = c.name;
        }
        double mt = 0.0;
        if (!rel || !stat_file(root, *rel, mt)) return nullptr;
        return media_item("image", *rel, ref_title(name, slot));
    } catch (const std::exception&) {
        return nullptr;
    }
}

json references_media(const fs::path& root) {
    json out = json::array();
    try {
        const models::AssetLibrary lib = models::ProjectStore(root).load_assets();
        double mt = 0.0;
        for (const auto& kv : lib.characters) {
            const auto& c = kv.second;
            if (out.size() >= kMaxImages) break;
            if (c.ref_front && stat_file(root, *c.ref_front, mt)) {
                out.push_back(media_item("image", *c.ref_front, ref_title(c.name, "front")));
            }
        }
        for (const auto& kv : lib.locations) {
            const auto& l = kv.second;
            if (out.size() >= kMaxImages) break;
            if (l.ref_empty && stat_file(root, *l.ref_empty, mt)) {
                out.push_back(media_item("image", *l.ref_empty, ref_title(l.name, "empty")));
            }
        }
    } catch (const std::exception&) {}
    return out;
}

json frames_media(const fs::path& root, const std::string& episode_id) {
    json out = json::array();
    try {
        const models::Project p = models::ProjectStore(root).load_project();
        const models::Episode* e = p.episode_by_id(episode_id);
        if (!e) return out;
        double mt = 0.0;
        for (const auto& s : e->shots) {
            if (out.size() >= kMaxImages) break;
            if (!s.frame_path || !stat_file(root, *s.frame_path, mt)) continue;
            out.push_back(media_item("image", *s.frame_path,
                                     shot_title(episode_id, s.shot_id)));
        }
    } catch (const std::exception&) {}
    return out;
}

json outputs_media(const fs::path& root) {
    // 和做完那一下比出来的是同一份（同一套名字、同一个封面），只是不比：
    // 拿一张空盘面去比，盘上有的就全是"新的"。
    // 一镜一镜的那些不摆：人问「出了哪些片」要的是成片目录那几条。
    return media_since(root, Baseline{}, "v:out:");
}

json shot_json(const fs::path& root, const std::string& episode_id,
               const std::string& shot_id) {
    try {
        const models::Project p = models::ProjectStore(root).load_project();
        const models::Episode* e = p.episode_by_id(episode_id);
        if (!e) return nullptr;
        for (const auto& s : e->shots) {
            if (s.shot_id == shot_id) return json(s);
        }
    } catch (const std::exception&) {}
    return nullptr;
}

namespace {

/// 一栏的值说成一句话。台词是数组，拼起来。
std::string field_word(const json& shot, const char* key) {
    if (!shot.is_object() || !shot.contains(key)) return {};
    const json& v = shot.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) {
        // 时长。「5.000000」没人要看，一位小数就够。
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1fs", v.get<double>());
        return buf;
    }
    if (v.is_array()) {
        std::string s;
        for (const auto& line : v) {
            const std::string t =
                line.is_object() ? line.value("text", std::string()) : std::string();
            if (t.empty()) continue;
            if (!s.empty()) s += " / ";
            s += t;
        }
        return s;
    }
    return {};
}

}  // namespace

json shot_change_media(const fs::path& root, const std::string& episode_id,
                       const std::string& shot_id, const json& before,
                       const json& after) {
    json out = json::array();
    if (!after.is_object()) return out;

    // 只列 `shot_edit` 改得动的那几栏（`agent/tools.cpp` 里那张 patch）。
    struct Field {
        const char* key;
        std::string label;
    };
    const Field fields[] = {
        {"shot_size", SAY("景别")},
        {"camera_move", SAY("运镜")},
        {"duration_s", SAY("时长")},
        {"dialogue", SAY("台词")},
        {"first_frame_prompt", SAY("首帧提示词")},
    };
    std::string body;
    for (const Field& f : fields) {
        const std::string was = field_word(before, f.key);
        const std::string now = field_word(after, f.key);
        if (was == now) continue;
        if (!body.empty()) body += '\n';
        body += SAYF("%1：%2 → %3", f.label, was.empty() ? std::string("—") : was,
                     now.empty() ? std::string("—") : now);
    }
    const std::string title = shot_title(episode_id, shot_id);
    if (!body.empty()) {
        out.push_back(media_item("text", {}, title + " · " + SAY("改动"), body));
    }
    // 那一镜的首帧：**看得出改的是哪一镜**。改完还没重出，所以这是旧画面，
    // 人拿它对着上面那几行想"改完会变成什么样"。
    const std::string frame = field_word(after, "frame_path");
    double mt = 0.0;
    if (!frame.empty() && stat_file(root, frame, mt)) {
        out.push_back(media_item("image", frame, title));
    }
    return out;
}

std::uint64_t task_floor_now() {
    const json b = pipeline::task_board();
    std::uint64_t top = 0;
    for (const char* sec : {"running", "queued", "done"}) {
        if (!b.contains(sec) || !b.at(sec).is_array()) continue;
        for (const auto& t : b.at(sec)) {
            top = std::max<std::uint64_t>(top, t.value("id", std::uint64_t{0}));
        }
    }
    return top;
}

namespace {

/// 两串路径是不是同一部片子。**规范化再比**（CLAUDE.md 第十一条）：对话认的
/// 那一串是界面递过来的，账本上那一串是 `ProjectStore` 记的，同一个目录
/// 两种写法（斜杠方向、软链）直接比就是"这一回什么都没做"。
///
/// 账本上没记项目的那几件算进来——同 `pipeline::task_board` 里 `mine` 的规矩；
/// 上面按 id 已经卡过"这一回"，漏进来的只能是这一回里没标项目的活。
bool same_project(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a == b) return true;
    std::error_code ea, eb;
    const fs::path pa = fs::weakly_canonical(paths::from_utf8(a), ea);
    const fs::path pb = fs::weakly_canonical(paths::from_utf8(b), eb);
    return !ea && !eb && pa == pb;
}

}  // namespace

WorkReport work_report(const std::string& project, std::uint64_t task_floor) {
    const json b = pipeline::task_board();
    struct Row {
        std::string title;
        std::string why;
    };
    std::vector<Row> ok, bad, stopped;
    if (b.contains("done") && b.at("done").is_array()) {
        // 账本上"做完的"是倒着排的（刚干完的在最上面）；报的时候按先后。
        for (auto it = b.at("done").rbegin(); it != b.at("done").rend(); ++it) {
            const json& t = *it;
            if (t.value("id", std::uint64_t{0}) <= task_floor) continue;
            if (!same_project(project, t.value("project", std::string()))) continue;
            const std::string title = t.value("title", std::string());
            const std::string state = t.value("state", std::string());
            if (state == "failed") {
                // 原话要带上：模型靠它决定重试还是去问人。**截一截**——
                // 一段 ffmpeg 的报错能有几千字。
                std::string why = text::strip_ws(t.value("error", std::string()));
                why = text::truncate_utf8(why, 200);
                bad.push_back({title, why});
            } else if (state == "cancelled") {
                stopped.push_back({title, {}});
            } else {
                ok.push_back({title, {}});
            }
        }
    }
    WorkReport out;
    if (ok.empty() && bad.empty() && stopped.empty()) return out;
    out.failed = !bad.empty();

    json sections = json::array();
    const auto section = [&out, &sections](const char* kind, const std::string& head,
                                           const std::vector<Row>& rows) {
        if (rows.empty()) return;
        // **一节最多八行。** 一章出片底下是几十件（每镜配音、首帧、片子），
        // 全列出来这一条就是一整屏，而人要看的是没成的那几行。
        constexpr std::size_t kRows = 8;
        json list = json::array();
        out.text += "\n" + head;
        for (std::size_t i = 0; i < rows.size() && i < kRows; ++i) {
            const Row& r = rows[i];
            out.text += "\n· " + (r.why.empty() ? r.title : SAYF("%1：%2", r.title, r.why));
            json one = {{"title", r.title}};
            if (!r.why.empty()) one["why"] = r.why;
            list.push_back(std::move(one));
        }
        if (rows.size() > kRows) out.text += "\n· …";
        sections.push_back({{"kind", kind}, {"rows", list}, {"more", rows.size() > kRows}});
    };
    section("failed", SAY("没成的："), bad);
    section("stopped", SAY("停下的："), stopped);
    section("done", SAY("做完的："), ok);
    out.report = {{"sections", sections}};
    return out;
}

}  // namespace changji::agent
