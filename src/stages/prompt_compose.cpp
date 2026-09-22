#include "stages/prompt_compose.hpp"

#include "stages/storyboard.hpp"  // motion_covering

#include <cstdio>
#include <map>
#include <utility>

#include "stages/prompts.inc.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

const std::string kEmpty;

/// 表里查不到就返回空串。
///
/// 返回空串而不是抛异常，是照抄 Python 的 dict.get(k, "")。
/// 抛异常的话，加一个新枚举值忘了补表会让整条流水线挂掉；
/// 返回空串只是那一层少一个词。前者更响亮但也更容易在半夜炸掉一整晚的活。
template <typename K>
const std::string& lookup(const std::map<K, std::string>& m, K key) {
    const auto it = m.find(key);
    return it == m.end() ? kEmpty : it->second;
}

/// 用分隔符连接，**跳过空串，并且削掉每一段尾巴上的断句标点**。
///
/// 跳空是必须的：不跳的话缺一层就多一个"，，"，
/// 而那个双逗号会被模型当成一个停顿信号。
///
/// **削尾标点是 2026-09-13 补的，治的是同一个病的另一半。** 大模型写完一段
/// 描述习惯性地点一个句号，而这里紧跟着又接一个「，」，拼出来是"。，"——
/// 和双逗号一样是个坏掉的分隔符，而且它一直在那儿：
///
/// | | 以句号结尾的 |
/// |---|---|
/// | walk_c 的 `first_frame_prompt` | 61 / 61 |
/// | 新出的那份 `first_frame_prompt` | 46 / 47 |
/// | 新出的那份 `motion_prompt` | 51 / 52 |
///
/// 也就是说**每一张首帧的提示词里都有一个"。，"**，从来没人看见——因为
/// 它不报错，只是让模型多读到一个停顿。运动描述那一栏以前是空的，
/// 所以这半边直到 `motion_prompt` 变成必填才露出来。
///
/// 削的是 `rstrip_punct` 那一套（。.；;，,、和空格），**不含 ！？ 和引号**：
/// "他说：「走！」"那种结尾是内容，不是断句。段落中间的句号一个不动——
/// 那是真的句子结构，只有贴着分隔符的那个是噪声。
std::string join_nonempty(const std::vector<std::string>& parts,
                          const std::string& sep) {
    std::string out;
    bool first = true;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        const std::string clean = text::rstrip_punct(p);
        if (clean.empty()) continue;
        if (!first) out += sep;
        out += clean;
        first = false;
    }
    return out;
}

/// prompts.toml 里 [compose.*] 那几张词表读成 map。键是枚举名（和 to_string
/// 出来的一样），所以下面三个函数按名字查，查不到还是空串——理由见 lookup。
template <std::size_t N>
std::map<std::string, std::string> zh_table(
    const std::pair<const char*, const char*> (&rows)[N]) {
    std::map<std::string, std::string> m;
    for (const auto& [key, zh] : rows) m.emplace(key, zh);
    return m;
}

}  // namespace

const std::string& shot_size_zh(ShotSize v) {
    static const auto m = zh_table(prompt::compose::kShotSize);
    return lookup(m, std::string(to_string(v)));
}

const std::string& angle_zh(CameraAngle v) {
    static const auto m = zh_table(prompt::compose::kCameraAngle);
    return lookup(m, std::string(to_string(v)));
}

const std::string& move_zh(CameraMove v) {
    static const auto m = zh_table(prompt::compose::kCameraMove);
    return lookup(m, std::string(to_string(v)));
}

const std::string& lens_zh(Lens v) {
    static const auto m = zh_table(prompt::compose::kLens);
    return lookup(m, std::string(to_string(v)));
}

PromptComposer::PromptComposer(AssetLibrary assets)
    : assets_(std::move(assets)), style_line_(assets_.style.style_line) {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，
    // 用中文逗号。两条线的分隔符不一样，混用会让标签串里冒出中文标点，
    // 而那对 Danbooru 词表训练出来的模型是噪声。
    sep_ = style_line_ == StyleLine::ANIME ? ", " : "，";
}

PromptBundle PromptComposer::compose(const Shot& shot) const {
    return compose_with(shot, shot.first_frame_prompt);
}

std::string PromptComposer::project_negative_extras() const {
    // 读资产库那一步会把默认负向词补进 negative_prompt（图像那套）。
    // 把默认那截削掉，剩下的才是用户自己加的。
    std::string v = text::strip_ws(assets_.style.negative_prompt);
    const auto strip_prefix = [&](const std::string& p) {
        if (p.empty() || v.compare(0, p.size(), p) != 0) return;
        v = v.substr(p.size());
        // 紧跟的分隔符一起削
        for (const char* sep : {"，", ", ", ","}) {
            const std::string s = sep;
            if (v.compare(0, s.size(), s) == 0) {
                v = v.substr(s.size());
                break;
            }
        }
        v = text::strip_ws(v);
    };
    if (style_line_ == StyleLine::ANIME) {
        strip_prefix(std::string(prompt::style::kNegativeBase) +
                     prompt::style::kNegativeAnimeExtra);
    }
    strip_prefix(prompt::style::kNegativeBase);
    return v;
}

PromptBundle PromptComposer::compose_end(const Shot& shot) const {
    if (!shot.last_frame_prompt.has_value() ||
        text::strip_ws(*shot.last_frame_prompt).empty()) {
        return compose(shot);
    }
    return compose_with(shot, *shot.last_frame_prompt);
}

PromptBundle PromptComposer::compose_with(const Shot& shot,
                                          const std::string& picture) const {
    std::vector<std::string> layers;
    std::vector<std::string> refs;

    // ---- 身份层。逐字节从资产库拼出来，模型碰不到。----
    //
    // **大特写（ECU）不带身份层。** 2026-09-13 q4_full sh004 实见：分镜要的
    // 是「豪华卧室床头柜特写。一部智能手机屏幕亮起……只能看到李浩然模糊的
    // 手部阴影」，角色列表里挂着他，身份层（名字、脸、身材、衣着）排在最前，
    // 出图模型就画了一个拿手机站在床边的全身人像；换种子重出还是。出片模型
    // 拿到这张和提示词矛盾的首帧，半路硬切到手机特写。同一镜四种拼法各出
    // 一遍：身份层在前→全身人像；挪到画面描述后面→还是全身人像；只留脸的
    // 描述放后面→一张脸浮在镜子里；**整个去掉→前景手机、背景虚化的人**，
    // 唯一对得上分镜的一张。
    //
    // 大特写本来就只装得下一个局部（手、眼、物件），角色的全身描述在这一
    // 档里没有位置；画面描述会点名是谁的眼、谁的手，够了。代价是脸的大特写
    // 少了那几个字的长相描述——两秒的插入镜头，可以接受。
    //
    // **场景层同样不带。** 去掉身份层之后同一镜再出：前景是手机，背景还是
    // 整间卧室加一个虚化的人——场景层那句「豪华卧室，柔和自然光……」把
    // 整个房间拉了进来，出片模型在 1.7 秒处又切到真正的手机大特写。连场景
    // 层也去掉之后才是分镜要的那张：手、手机、床头柜、床的一角。画面描述
    // 里本来就写着"只能看到床的一角"，大特写要的环境就那么一点，靠它够了。
    const bool insert_shot = shot.shot_size == ShotSize::ECU;
    for (const CharacterInShot& in_shot : shot.characters) {
        const auto it = assets_.characters.find(in_shot.char_id);
        if (it == assets_.characters.end()) {
            throw RenderError("镜头 " + shot.shot_id + " 引用了未注册角色 " +
                              in_shot.char_id);
        }
        if (insert_shot) continue;   // 校验照做，层不拼、参考图不带
        const Character& c = it->second;
        std::vector<std::string> beats = {
            c.render_prompt(style_line_, in_shot.wardrobe_state)};
        if (!in_shot.expression.empty()) beats.push_back(in_shot.expression);
        if (!in_shot.action.empty()) beats.push_back(in_shot.action);
        layers.push_back(join_nonempty(beats, sep_));

        const auto ref = c.ref_for_pose(to_string(in_shot.face_pose));
        if (ref.has_value() && !ref->empty()) refs.push_back(*ref);
    }

    // ---- 场景层 ----
    if (shot.location_id.has_value() && !shot.location_id->empty()) {
        const auto it = assets_.locations.find(*shot.location_id);
        if (it == assets_.locations.end()) {
            throw RenderError("镜头 " + shot.shot_id + " 引用了未注册场景 " +
                              *shot.location_id);
        }
    }
    if (!insert_shot && shot.location_id.has_value() && !shot.location_id->empty()) {
        const auto it = assets_.locations.find(*shot.location_id);
        // 这一镜自己写了光就不带场景那句光：两句光同时在，模型两句都读。
        const bool shot_has_light = !text::strip_ws(shot.lighting).empty();
        layers.push_back(it->second.render_prompt(style_line_, !shot_has_light));
        // **时段对不上就别喂那张空景图。**
        //
        // 空景图一个场景只有一张，而同一个场景会有日夜两场戏。
        // 2026-09-16 实测 ep04_sh001：场景资产写着「白天，散射光从窗户来」、
        // 空景图是大白天的，而这一镜的光是「夜晚，光从窗户来，朝内打，硬光」
        // ——Edit 模型拿参考图压过文字，首帧出来是大白天；出片模型再拿这张
        // 白天首帧配上「夜晚」的提示词，两秒里把画面从白天拉成了夜晚。
        // 闸门那句「首帧和提示词对不上，模型半路切到了提示词要的画面」
        // 说的正是这件事。
        //
        // 这时候宁可只靠文字描述环境：一张白天的照片会把整场戏拽错时段，
        // 而文字至少和这一镜的光是一致的。人物参考图不受影响——脸不分昼夜。
        // 判据只认「明说了而且不是同一个时段」，说不准的一律照喂（见
        // lighting_clashes）。
        //
        // **试过「近景以紧不喂这张图」，没用，而且有害。**
        //
        // 起因是 2026-09-16 的 ep06：宋律师办公室那一场八镜，分镜排的是
        // LS / MS / MCU / CU 四种景别，出来的八张首帧机位和空景图分毫不差
        // ——同一张桌子、同一扇窗、墙上同样三个镜框，只是人站的位置不同。
        // 当时的判断是 Edit 模型照着这张图摆机位，把景别抹平了。
        //
        // 于是让 MCU / CU / ECU 不带这张图，重出 sh018(MCU)、sh019(CU)：
        // **人还是站着的全身，一点没收紧**，只是屋子换了——sh019 出成了
        // 一间带拱窗和书柜的书房，和同一场别的镜头根本不是一个地方。
        // 也就是说：景别收不收紧这件事，那张图说了不算；而没有它，场景
        // 一致性当场就丢。两头都输，改回来了。
        //
        // 真正的原因在别处：出图模型对「近景」「特写」这两个词本身就不
        // 敏感，给人物一律画全身。下次要动这里，先拿同一段提示词只改景别
        // 那两个字出几张，确认模型认不认，别再从参考图下手。
        const bool clash =
            lighting_clashes(it->second.lighting, shot.lighting);
        if (!clash && it->second.ref_empty.has_value() &&
            !it->second.ref_empty->empty()) {
            refs.push_back(*it->second.ref_empty);
        }
    }

    // ---- 镜头层 ----
    //
    // 景别、机位、焦段一句；然后是这一镜的光；然后是画面描述。
    // 焦段和光是 2026-09-14 加的（docs/电影质感方案.md）：没填时
    // （老分镜表）一个字不加，语料照旧。这一镜的光填了的话，场景层那句
    // 光就不拼（见上面），所以这里不存在「谁盖过谁」的问题。
    std::vector<std::string> shot_layers;
    shot_layers.push_back(join_nonempty(
        {shot_size_zh(shot.shot_size), angle_zh(shot.camera_angle),
         lens_zh(shot.lens)},
        sep_));
    if (!text::strip_ws(shot.lighting).empty()) shot_layers.push_back(shot.lighting);
    if (!picture.empty()) shot_layers.push_back(picture);
    for (const auto& l : shot_layers) layers.push_back(l);

    // ---- 风格层 ----
    //
    // **这里不给默认值，补默认是读资产库那一步的事**（见
    // ProjectStore::load_assets）。在这儿补的话，同一份 assets.json 走
    // 出图和走界面显示会得到两种画风，而用户在项目页的「画风」框里看到的
    // 是空的——他改不了一个看不见的东西。
    // 这一段还和 Python 逐字节对拍，见本文件开头。
    if (!assets_.style.global_style.empty()) {
        layers.push_back(assets_.style.global_style);
    }

    // 注意最后这一步过滤的是 **strip 之后为空**，不是原串为空。
    // 对应 Python 的 `if p.strip()`。一层里只有空格的话要整层丢掉，
    // 留着就是一个孤零零的分隔符。
    std::vector<std::string> kept;
    for (const auto& l : layers) {
        if (!text::strip_ws(l).empty()) kept.push_back(l);
    }

    PromptBundle out;
    {
        std::vector<std::string> vs;
        for (const auto& l : shot_layers) {
            if (!text::strip_ws(l).empty()) vs.push_back(l);
        }
        out.video_scene = join_nonempty(vs, sep_);
        out.style_layer = assets_.style.global_style;
    }
    // **大特写走基础文生图权重。** 这一档故意不带身份层、场景层和参考图
    // （上面那段），而首帧那一族是 Edit 模型：没有编辑源它退化成文生图，
    // 出来多半是彩色噪点，闸门那条「不是空图」拦不住（sd_image.cpp）。
    // 基础版没配时 frames.cpp 会退回 Edit——那时至少带上这个场景的空景图
    // 当编辑源（下面），一张桌子上的杯子照着空景画，比噪点强。
    if (insert_shot) {
        out.base_model = true;
        if (shot.location_id.has_value() && !shot.location_id->empty()) {
            const auto it = assets_.locations.find(*shot.location_id);
            if (it != assets_.locations.end() && it->second.ref_empty.has_value() &&
                !it->second.ref_empty->empty() &&
                !lighting_clashes(it->second.lighting, shot.lighting)) {
                refs.push_back(*it->second.ref_empty);
            }
        }
    }
    // **ASCII 圆括号和方括号在 sd.cpp 里是权重语法，这里没有转义。**
    //
    // stable-diffusion.cpp 的 parse_prompt_attention 把 ( ) 当加权、
    // [ ] 当减权，\( 才是字面的括号。它自己的注释里有例子：
    //     parse_prompt_attention("(unnecessary)(parens)")
    //       -> [["unnecessaryparens", 1.1]]
    // 括号被吃掉、里面的词被加权。更糟的是不配对的那种：
    //     parse_prompt_attention("(unbalanced") -> [["unbalanced", 1.1]]
    // 一个孤零零的左括号会把它后面的词全部加权。
    //
    // 提示词里混着大模型写的分镜描述，它偶尔会吐 ASCII 括号——那时候
    // 出来的图会悄悄变一点，**不报错也不好察觉**。
    //
    // **为什么先不转义。** 转了的话，有人故意在资产库里写 "(红裙:1.3)"
    // 调权重的能力就没了。这个能力没写进文档、界面上也没有，但它是通的。
    // 两边都是"悄悄地不按预期工作"，而这个项目自己从不使用权重语法
    // （2026-09-11 查过：stages/ 和 models/ 下一处都没有，金语料里的
    // 提示词是纯中文全角标点）。
    //
    // 要动的话判据是这个：**用户想不想要权重这个能力**。
    //   想要 -> 保持现状，顶多在拼完之后检一遍不配对的括号并告警；
    //   不想要 -> 把 ( ) [ ] 全转义成 \( 那种字面形式，一了百了。
    out.positive = join_nonempty(kept, sep_);
    // 负向提示词是镜头自己的加上全片的。镜头级的在前——
    // 它是针对这一镜的具体问题加的，权重该更高。
    out.negative = join_nonempty(
        {shot.negative_prompt, assets_.style.negative_prompt}, sep_);
    // 视频那份：镜头自己的 + 项目页「负向」框里用户自己加的 + 全片的视频
    // 负向词（prompts.toml [style]）。图像那份的默认串（「多余的手指」）
    // 不带过去，理由见 PromptBundle::negative_video；但用户在那个框里
    // **自己加的**要带——文件里推荐它「再压一道」，而 2026-09-16 之前它一个
    // 字都进不了视频。
    out.negative_video = join_nonempty(
        {shot.negative_prompt, project_negative_extras(), prompt::style::kNegativeVideo},
        sep_);
    out.reference_images = std::move(refs);
    return out;
}

bool lighting_clashes(const std::string& location_light,
                      const std::string& shot_light) {
    // 只认明写出来的时段词。**说不准的一律算不冲突**：宁可喂一张可能
    // 不对的参考图，也不要因为一句没写时段的光把场景的样子整个丢掉。
    const auto era = [](const std::string& s) -> int {
        static const char* kNight[] = {"夜", "晚", "凌晨", "深夜", "午夜"};
        static const char* kDay[] = {"白天", "日间", "上午", "下午", "中午", "正午"};
        bool night = false, day = false;
        for (const char* w : kNight) {
            if (s.find(w) != std::string::npos) { night = true; break; }
        }
        for (const char* w : kDay) {
            if (s.find(w) != std::string::npos) { day = true; break; }
        }
        if (night == day) return 0;   // 都没有、或者两样都提了：说不准
        return night ? 1 : 2;
    };
    const int a = era(location_light);
    const int b = era(shot_light);
    return a != 0 && b != 0 && a != b;
}

std::string PromptComposer::motion_prompt(const Shot& shot) const {
    std::vector<std::string> actions;
    for (const CharacterInShot& in_shot : shot.characters) {
        if (!in_shot.action.empty()) actions.push_back(in_shot.action);
    }
    // **用之前再夹一次时长。**
    //
    // 排分镜和改时长那两处已经对齐过（storyboard.cpp 的 motion_covering），
    // 但**盘上的老分镜表不会自愈**：2026-09-16 实测，修复之前排的那一章里
    // 一个 6 秒的镜头挂着 `[0-15秒]`、一个 2 秒的挂着 `[0-5秒]`。那些表
    // 不重排就一直是错的，而重排会把人手改过的东西一起冲掉。
    //
    // 夹在**用**的这一刻，老表照样出对的片子，一个字都不用人管。
    // 对已经对齐的表这一步是恒等变换。
    const std::string motion =
        motion_covering(text::strip_ws(shot.motion_prompt), shot.duration_s);

    // 分镜模型已经按时间码分段：运镜它自己写在段里了（规则第 6 条要它写
    // 名字、幅度、速度），不再前插枚举词——前插就是「镜头缓慢推近, 缓慢推近」。
    // 角色动作并进**第一段**：它是这一镜的起始状态；接在整串末尾的话，
    // 三段的镜头里所有动作都被读成最后一段的事（2026-09-16 查出）。
    if (!motion.empty() && motion.front() == '[') {
        const std::size_t close = motion.find(']');
        if (close != std::string::npos) {
            const std::string head = motion.substr(0, close + 1);
            std::string rest = text::strip_ws(motion.substr(close + 1));
            // 第一段到下一个时间码为止
            const std::size_t next = rest.find('[');
            std::string first = next == std::string::npos ? rest : rest.substr(0, next);
            std::string later = next == std::string::npos ? "" : rest.substr(next);
            first = text::strip_ws(first);
            std::vector<std::string> parts = {first};
            parts.insert(parts.end(), actions.begin(), actions.end());
            std::string out = head + " " + join_nonempty(parts, sep_);
            if (!later.empty()) out += " " + text::strip_ws(later);
            return out;
        }
    }

    // 没分段：整镜当一段。
    std::vector<std::string> parts = {move_zh(shot.camera_move)};
    if (!motion.empty()) parts.push_back(motion);
    parts.insert(parts.end(), actions.begin(), actions.end());
    const std::string body = join_nonempty(parts, sep_);
    if (body.empty()) return body;
    char dur[32];
    std::snprintf(dur, sizeof(dur), "%g", shot.duration_s);
    return std::string("[0-") + dur + "秒] " + body;
}

std::vector<std::string> shots_without_refs(
    const std::vector<models::Shot>& shots,
    const models::AssetLibrary& assets) {
    // **走 compose 本身，不另写一份判断。** 参考图怎么挑（每个在场角色按
    // face_pose 取一张、场景取空景图、大特写整个不带）全在 compose 里，
    // 照着再写一遍就是第二份，而两份漂开的时候这一条会**放行本该拦住的**。
    const PromptComposer composer(assets);
    std::vector<std::string> out;
    for (const models::Shot& shot : shots) {
        if (shot.shot_size == models::ShotSize::ECU) continue;
        try {
            if (composer.compose(shot).reference_images.empty()) {
                out.push_back(shot.shot_id);
            }
        } catch (const RenderError&) {
            // 引用了没注册的角色/场景。那是另一条错，让出图那一步去说。
        }
    }
    return out;
}

}  // namespace changji::stages
