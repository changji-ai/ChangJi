// 提示词组装的对拍测试。
//
// **这是阶段 5 里唯一算契约的东西。** 出图后端换成什么都无所谓——
// ComfyUI 也好、进程内 sd.cpp 也好，拿到的是同一份提示词。
// 但那串字必须和 Python 逐字节一致，否则同一个项目在两个后端上
// 出的画面不一样，而用户会以为是模型的问题。

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/shot.hpp"
#include "stages/prompt_compose.hpp"
#include "stages/prompts.inc.hpp"
#include "stages/render.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/prompt_compose.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

/// 和语料里那份一模一样的资产库。
models::AssetLibrary make_assets(models::StyleLine line) {
    models::AssetLibrary a;

    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性，外表冷静";
    lin.appearance.body = "偏瘦，中等身高";
    lin.appearance.face = "黑色长直发，鹅蛋脸，杏眼";
    lin.appearance.attire = "白色衬衫，深色西装裤";
    lin.ref_front = "refs/c_lin_wan_front.png";
    lin.ref_three_quarter = "refs/c_lin_wan_three_quarter.png";
    models::WardrobeVariant rain;
    rain.wardrobe_id = "雨中";
    rain.description = "湿透的白衬衫";
    lin.wardrobe.push_back(rain);
    a.characters["c_lin_wan"] = lin;

    models::Character chen;
    chen.char_id = "c_chen_mo";
    chen.name = "陈默";
    chen.appearance.identity = "三十出头男性";
    chen.appearance.body = "高瘦";
    chen.appearance.face = "短寸黑发，方脸";
    chen.appearance.attire = "黑色风衣";
    a.characters["c_chen_mo"] = chen;

    models::Character plain;
    plain.char_id = "c_plain";
    plain.name = "路人";
    plain.appearance.identity = "中年男性";
    plain.appearance.face = "圆脸";
    plain.appearance.attire = "工装";
    a.characters["c_plain"] = plain;

    models::Location rooftop;
    rooftop.location_id = "loc_rooftop";
    rooftop.name = "夜间天台";
    rooftop.space = "水泥地面，锈蚀护栏";
    rooftop.lighting = "夜间冷调顶光";
    rooftop.palette = "冷蓝";
    rooftop.ref_empty = "refs/loc_rooftop_empty.png";
    a.locations["loc_rooftop"] = rooftop;

    models::Location alley;
    alley.location_id = "loc_alley";
    alley.name = "小巷";
    alley.space = "狭窄砖墙";
    alley.lighting = "路灯昏黄";
    a.locations["loc_alley"] = alley;

    a.style.style_line = line;
    a.style.global_style = "电影感，浅景深，冷色调";
    a.style.negative_prompt = "低质量，模糊";
    a.style.aspect_ratio = "9:16";
    return a;
}

void check_text(const std::string& got, const std::string& want,
                const char* what) {
    if (got != want) {
        std::size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
        MESSAGE(what << " 第一处不同在字节 " << i);
        MESSAGE("期望 " << want);
        MESSAGE("实得 " << got);
    }
    CHECK(got == want);
}

}  // namespace

// ---------------------------------------------------------------------------
// 「和 Python 逐字节一致」那几条，2026-09-17 退役
// ---------------------------------------------------------------------------
//
// 语料是当年冻下来的 Python 答案，用来证明移植没走样。**Python 引擎
// 2026-09-10 就删了**，那个用途从那天起就没了；断言还在，实际效果变成
// 「提示词和 schema 永远改不动」。
//
// 这一天里它挡住了三处量出来的改进：`camera_move` 的分类式禁令（模型对每
// 一镜重新论证一遍，一场戏想十五万字）、从没被填过的 `visual_desc`
//（两个项目 76% 和 100% 是空的）、以及 schema 的排版。用户拍板退役。
//
// **换掉的不是"有守卫"，是"守卫钉的是什么"**：钉结构（字段在不在、必填掉
// 没掉）而不是钉字节。前者是真出事——模型按错的名字产出、或者整个略过一栏，
// 全程不报错；后者只是让人改不动一句话。
//
TEST_CASE("三张中文标签表一项都不能少") {
    // 表里少一项的话那一层拼出来是空的，而空的景别层意味着模型
    // 自己决定构图——同一章里景别会乱跳，而且看不出是为什么。
    for (const auto& kv : golden().at("shot_size_zh").items()) {
        CAPTURE(kv.key());
        models::ShotSize v{};
        from_json(json(kv.key()), v);
        CHECK(stages::shot_size_zh(v) == kv.value().get<std::string>());
    }
    for (const auto& kv : golden().at("angle_zh").items()) {
        CAPTURE(kv.key());
        models::CameraAngle v{};
        from_json(json(kv.key()), v);
        CHECK(stages::angle_zh(v) == kv.value().get<std::string>());
    }
    for (const auto& kv : golden().at("move_zh").items()) {
        CAPTURE(kv.key());
        models::CameraMove v{};
        from_json(json(kv.key()), v);
        CHECK(stages::move_zh(v) == kv.value().get<std::string>());
    }
}

TEST_CASE("分层顺序不能变") {
    // 提示词里靠前的词权重更高。顺序一变画面重心就跟着变，
    // 同一个角色在不同镜头里会显得不是同一个人。
    //
    // 这一条不比字节，比的是四层出现的**先后**——将来有人重构这个函数时，
    // 逐字节那条用例会报"第一处不同在第 N 字节"，看不出是顺序问题。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台，女子背对镜头";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.location_id = "loc_rooftop";
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);

    const std::string p = stages::PromptComposer(a).compose(s).positive;
    const std::size_t identity = p.find("二十七岁女性");        // 身份层
    const std::size_t place = p.find("水泥地面");                // 场景层
    const std::size_t camera = p.find("中景");                   // 镜头层
    const std::size_t style = p.find("电影感");                  // 风格层

    REQUIRE(identity != std::string::npos);
    REQUIRE(place != std::string::npos);
    REQUIRE(camera != std::string::npos);
    REQUIRE(style != std::string::npos);
    CHECK(identity < place);
    CHECK(place < camera);
    CHECK(camera < style);
}

TEST_CASE("换装只替换服装那一层") {
    // 换装时把整个外观块换掉的话，同一个角色换件衣服就变了张脸。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);

    const stages::PromptComposer composer(a);
    const std::string normal = composer.compose(s).positive;

    s.characters[0].wardrobe_state = "雨中";
    const std::string changed = composer.compose(s).positive;

    // 服装换了
    CHECK(normal.find("白色衬衫，深色西装裤") != std::string::npos);
    CHECK(changed.find("湿透的白衬衫") != std::string::npos);
    CHECK(changed.find("白色衬衫，深色西装裤") == std::string::npos);
    // 脸和身材一个字没动
    CHECK(changed.find("黑色长直发，鹅蛋脸，杏眼") != std::string::npos);
    CHECK(changed.find("偏瘦，中等身高") != std::string::npos);
    CHECK(changed.find("二十七岁女性") != std::string::npos);
}

TEST_CASE("同一个角色在不同镜头里的身份层逐字节相同") {
    // 这是整个一致性方案的根：身份层从资产库读出来，模型碰不到。
    // 两个镜头拿到的必须是同一个字符串，差一个字就是差一张脸。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    const stages::PromptComposer composer(a);

    const auto make = [](const std::string& id, models::ShotSize size,
                         const std::string& expr) {
        models::Shot s;
        s.shot_id = id;
        s.scene_id = "sc01";
        s.shot_size = size;
        s.first_frame_prompt = "随便什么描述 " + id;
        models::CharacterInShot in_shot;
        in_shot.char_id = "c_lin_wan";
        in_shot.expression = expr;
        s.characters.push_back(in_shot);
        return s;
    };

    const std::string a1 =
        composer.compose(make("sh001", models::ShotSize::CU, "落寞")).positive;
    const std::string a2 =
        composer.compose(make("sh002", models::ShotSize::LS, "克制")).positive;

    // 身份那一段在两个镜头里一模一样
    const std::string identity =
        a.characters.at("c_lin_wan").render_prompt(models::StyleLine::REALISTIC);
    CHECK(a1.find(identity) != std::string::npos);
    CHECK(a2.find(identity) != std::string::npos);
    // 而且是从**开头**开始的——身份层排第一
    CHECK(a1.rfind(identity, 0) == 0);
    CHECK(a2.rfind(identity, 0) == 0);
}

TEST_CASE("两条风格线的分隔符不一样") {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，
    // 用中文逗号。混用会让标签串里冒出中文标点，
    // 而那对 Danbooru 词表训练出来的模型是噪声。
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    // 这一段刻意带中文逗号：它是**内容**，不是分隔符，
    // 动漫线下也要原样留着——改它等于替用户改提示词。
    s.first_frame_prompt = "雨夜天台，女子背对镜头";

    // 别的地方不能有中文标点，否则分不清"分隔符"和"内容自带的逗号"。
    // 资产库里的 global_style 本来就写着"电影感，浅景深"——
    // 那些逗号是内容，不是这条用例要验的东西。
    const auto clean = [](models::StyleLine line) {
        models::AssetLibrary a = make_assets(line);
        a.style.global_style = "cinematic";
        a.style.negative_prompt = "lowres";
        return a;
    };

    const std::string real =
        stages::PromptComposer(clean(models::StyleLine::REALISTIC))
            .compose(s).positive;
    const std::string anime =
        stages::PromptComposer(clean(models::StyleLine::ANIME))
            .compose(s).positive;

    // 写实线两层之间是中文逗号
    CHECK(real == "中景，平视，雨夜天台，女子背对镜头，cinematic");
    // 动漫线是英文逗号加空格，一个中文标点都没有
    CHECK(anime == "中景, 平视, 雨夜天台，女子背对镜头, cinematic");
    // 注意 first_frame_prompt 里那个中文逗号原样留着——
    // 那是内容不是分隔符，改它等于替用户改提示词。
}

TEST_CASE("缺一层不会留下孤零零的分隔符") {
    // 不跳空的话缺一层就多一个"，，"，而那个双逗号会被模型当成停顿信号。
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    a.style.global_style.clear();

    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.first_frame_prompt.clear();
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;

    const std::string p = stages::PromptComposer(a).compose(s).positive;
    CHECK(p.find("，，") == std::string::npos);
    CHECK(p.rfind("，") != p.size() - 3);   // 不以分隔符结尾
    CHECK(p.rfind("，", 0) != 0);            // 也不以它开头
}

TEST_CASE("模型写的句号不会和分隔符撞成「。，」") {
    // 上一条的另一半，2026-09-13 才发现。大模型写完一段描述习惯点个句号，
    // 而下一层紧跟着接「，」，拼出来就是"。，"——和双逗号一样是个坏掉的
    // 分隔符。**而且它一直在那儿**：walk_c 的 first_frame_prompt 61/61
    // 以句号结尾，也就是每一张首帧的提示词里都有一个，从来没人看见。
    //
    // 运动描述那一栏以前是空的，所以这半边直到 motion_prompt 变成必填
    // 才露出来（新出的 51/52 条也以句号结尾）。
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);

    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.camera_move = models::CameraMove::PUSH_IN;
    s.first_frame_prompt = "雨点砸在积水里，霓虹灯反射其上。";
    s.motion_prompt = "雨势渐大，他缓缓抬头。";

    const stages::PromptComposer composer(a);
    const std::string pos = composer.compose(s).positive;
    const std::string motion = composer.motion_prompt(s);

    CHECK(pos.find("。，") == std::string::npos);
    CHECK(motion.find("。，") == std::string::npos);
    // 内容本身不能被削掉，只削贴着分隔符的那个句号。
    CHECK(pos.find("霓虹灯反射其上") != std::string::npos);
    CHECK(motion.find("他缓缓抬头") != std::string::npos);
    // 段落中间的句号是真的句子结构，一个都不能动。
    CHECK(pos.find("雨点砸在积水里，霓虹灯反射其上") != std::string::npos);

    SUBCASE("！和？是内容，不是断句，不许削") {
        models::Shot q = s;
        q.motion_prompt = "他猛地回头！";
        const std::string m = stages::PromptComposer(a).motion_prompt(q);
        CHECK(m.find("他猛地回头！") != std::string::npos);
    }
}

TEST_CASE("这一镜写了光，场景资产那句光就不拼：两句光同时在，模型两句都读") {
    // 2026-09-16 实测 ep04_sh001：场景资产写着「白天，散射光从窗户来」，
    // 这一镜的光是「夜晚…硬光」，两句都进了提示词，首帧出来是大白天。
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.location_id = "loc_rooftop";
    s.first_frame_prompt = "雨夜天台";
    const std::string loc_light = a.locations.at("loc_rooftop").lighting;
    REQUIRE_FALSE(loc_light.empty());

    SUBCASE("没写镜头光：场景那句光照旧") {
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.positive.find(loc_light) != std::string::npos);
    }
    SUBCASE("写了镜头光：只剩镜头那句") {
        s.lighting = "夜晚，路灯从画左上斜射，硬光";
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.positive.find(loc_light) == std::string::npos);
        CHECK(p.positive.find("夜晚，路灯从画左上斜射，硬光") != std::string::npos);
        // 空间和色板那两句还在，丢的只是光
        CHECK(p.positive.find(a.locations.at("loc_rooftop").space) != std::string::npos);
    }
}

TEST_CASE("时间码格式的运动描述：运镜词不再前插，角色动作并进第一段") {
    // 前插一次就是「镜头缓慢推近, 缓慢推近」（金语料里原来就长这样）；
    // 动作接在整串末尾，三段的镜头里所有动作都被读成最后一段的事。
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.camera_move = models::CameraMove::PUSH_IN;
    s.duration_s = 15.0;
    s.motion_prompt = "[0-5秒] 镜头缓慢推近半步，雨点砸在栏杆上 [5-10秒] 她抬头 [10-15秒] 手松开";
    models::CharacterInShot in;
    in.char_id = "c_lin_wan";
    in.action = "扶着护栏";
    s.characters = {in};
    const std::string m = stages::PromptComposer(a).motion_prompt(s);
    // 第一段：原句 + 动作；后面两段原样
    CHECK(m.find("[0-5秒] 镜头缓慢推近半步，雨点砸在栏杆上，扶着护栏") == 0);
    CHECK(m.find("[5-10秒] 她抬头") != std::string::npos);
    CHECK(m.find("[10-15秒] 手松开") != std::string::npos);
    // 动作不在末段
    CHECK(m.find("手松开，扶着护栏") == std::string::npos);
    // 运镜枚举词没有第二次出现
    CHECK(m.find("镜头缓慢推近，") == std::string::npos);
    CHECK(m.find(", 缓慢推近") == std::string::npos);
}

TEST_CASE("项目页「负向」框里用户自己加的词要进视频负向词，默认那串不进") {
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜";
    SUBCASE("只有默认串：视频负向词就是 [style].negative_video") {
        a.style.negative_prompt = stages::prompt::style::kNegativeBase;
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.negative_video == std::string(stages::prompt::style::kNegativeVideo));
        CHECK(p.negative_video.find("多余的手指") == std::string::npos);
    }
    SUBCASE("默认串后面用户加了词：那几个词进去，默认串不进") {
        a.style.negative_prompt =
            std::string(stages::prompt::style::kNegativeBase) + "，3D 渲染，卡通";
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.negative_video.find("3D 渲染，卡通") == 0);
        CHECK(p.negative_video.find("多余的手指") == std::string::npos);
        CHECK(p.negative_video.find(stages::prompt::style::kNegativeVideo) != std::string::npos);
    }
}

TEST_CASE("大特写不带身份层：物件特写里塞角色全身描述，出来的是人不是物件") {
    // 2026-09-13 q4_full sh004：分镜要床头柜手机特写，角色列表挂着男主，
    // 身份层排最前，出的首帧是拿手机站在床边的全身人像；身份层挪后、只留
    // 脸都不行，整个去掉才对。见 PromptComposer::compose 里那段。
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh004";
    s.scene_id = "sc01";
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.first_frame_prompt = "床头柜上的手机屏幕亮起，只能看到林晚模糊的手部阴影";
    models::CharacterInShot in;
    in.char_id = "c_lin_wan";
    s.characters = {in};

    s.location_id = "loc_rooftop";

    SUBCASE("ECU：没有名字、脸、衣着、场景，画面描述和风格还在；走基础文生图") {
        s.shot_size = models::ShotSize::ECU;
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.positive.find("鹅蛋脸") == std::string::npos);
        CHECK(p.positive.find("白色衬衫") == std::string::npos);
        CHECK(p.positive.find("锈蚀护栏") == std::string::npos);   // 场景层
        CHECK(p.positive.find("床头柜上的手机屏幕亮起") != std::string::npos);
        CHECK(p.positive.find("大特写") != std::string::npos);
        CHECK(p.positive.find("电影感") != std::string::npos);     // 风格层照旧
        // 不带身份参考图（带了就画成全身人像）。首帧那一族是 Edit 模型，
        // 一张参考图都没有会退化成彩噪，所以这一档要基础文生图权重；基础版
        // 没配时退回 Edit，那时至少带这个场景的空景图当编辑源。
        CHECK(p.base_model);
        CHECK(p.reference_images == std::vector<std::string>{"refs/loc_rooftop_empty.png"});
    }
    SUBCASE("ECU 没有场景：一张参考图都没有，但标了基础模型") {
        s.shot_size = models::ShotSize::ECU;
        s.location_id.reset();
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.base_model);
        CHECK(p.reference_images.empty());
    }
    SUBCASE("CU 及以上照旧带身份层和场景层") {
        // 2026-09-16 试过让 MCU / CU 不带空景图（想让近景收紧），实测
        // 人还是全身、屋子却换了——见 prompt_compose.cpp 里那段。改回来了。
        s.shot_size = models::ShotSize::CU;
        const auto p = stages::PromptComposer(a).compose(s);
        CHECK(p.positive.find("鹅蛋脸") != std::string::npos);
        CHECK(p.positive.find("锈蚀护栏") != std::string::npos);
        CHECK(p.reference_images.size() == 2);
    }
    SUBCASE("ECU 引用了未注册场景照样拦") {
        s.shot_size = models::ShotSize::ECU;
        s.location_id = "loc_nowhere";
        CHECK_THROWS(stages::PromptComposer(a).compose(s));
    }
    SUBCASE("ECU 引用了未注册角色照样拦") {
        s.shot_size = models::ShotSize::ECU;
        s.characters[0].char_id = "c_nobody";
        CHECK_THROWS(stages::PromptComposer(a).compose(s));
    }
}

TEST_CASE("哪几镜一张参考图都拿不到") {
    // **这一条挡的是"跑完一个钟头才发现是一章雪花"。**
    //
    // 首帧那一族是图像编辑模型，手上没有编辑源时退化成文生图，出来是彩色
    // 噪点；而闸门那条「不是空图」拦不住它（方差比真图还大）。所以 post_run
    // 在按下去那一刻就得拦，而拦的判据必须和 compose 真正挑参考图的那一套
    // 一模一样——`shots_without_refs` 因此是**调 compose 本身**，不另写一份。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);

    const auto shot = [](const std::string& id, const std::string& who,
                         const std::string& where, models::ShotSize size,
                         models::FacePose pose) {
        models::Shot s;
        s.shot_id = id;
        s.shot_size = size;
        if (!where.empty()) s.location_id = where;
        if (!who.empty()) {
            models::CharacterInShot in_shot;
            in_shot.char_id = who;
            in_shot.face_pose = pose;
            s.characters.push_back(in_shot);
        }
        return s;
    };

    const auto bare = [&](const std::vector<models::Shot>& v) {
        return stages::shots_without_refs(v, a);
    };

    SUBCASE("人没定妆、场景也没空景图 —— 拦") {
        // c_plain 一张参考图都没有，loc_alley 也没有空景图
        const auto out = bare({shot("s1", "c_plain", "loc_alley",
                                    models::ShotSize::MS,
                                    models::FacePose::FRONT)});
        CHECK(out == std::vector<std::string>{"s1"});
    }

    SUBCASE("人有定妆图 —— 放行") {
        const auto out = bare({shot("s1", "c_lin_wan", "loc_alley",
                                    models::ShotSize::MS,
                                    models::FacePose::FRONT)});
        CHECK(out.empty());
    }

    SUBCASE("人没有、但场景有空景图 —— 放行") {
        // 一张就够：编辑模型有编辑源就不会退化成文生图
        const auto out = bare({shot("s1", "c_plain", "loc_rooftop",
                                    models::ShotSize::MS,
                                    models::FacePose::FRONT)});
        CHECK(out.empty());
    }

    SUBCASE("只画了正面和侧面，背身镜头靠回退拿到侧面 —— 放行") {
        // ref_for_pose：back → ref_back（没有）→ ref_three_quarter（有）
        const auto out = bare({shot("s1", "c_lin_wan", "loc_alley",
                                    models::ShotSize::MS,
                                    models::FacePose::BACK)});
        CHECK(out.empty());
    }

    SUBCASE("一张都没画的人，背身镜头回退也拿不到 —— 拦") {
        const auto out = bare({shot("s1", "c_chen_mo", "loc_alley",
                                    models::ShotSize::MS,
                                    models::FacePose::BACK)});
        CHECK(out == std::vector<std::string>{"s1"});
    }

    SUBCASE("大特写不算 —— 那一档是故意不带参考图的") {
        // compose 里 insert_shot 那段：带上的话半个房间会被拉进一个特写里。
        // 算进来等于让有大特写的那一章永远出不来，而它没有"补一张图"的解法。
        const auto out = bare({shot("s1", "c_plain", "loc_alley",
                                    models::ShotSize::ECU,
                                    models::FacePose::FRONT)});
        CHECK(out.empty());
    }

    SUBCASE("引用了没注册的角色 —— 不算这一条，让出图那步去说") {
        const auto out = bare({shot("s1", "c_no_such", "loc_alley",
                                    models::ShotSize::MS,
                                    models::FacePose::FRONT)});
        CHECK(out.empty());
    }

    SUBCASE("按分镜表的顺序回，一次报全") {
        const auto out = bare({
            shot("s1", "c_lin_wan", "loc_rooftop", models::ShotSize::MS,
                 models::FacePose::FRONT),
            shot("s2", "c_plain", "loc_alley", models::ShotSize::MS,
                 models::FacePose::FRONT),
            shot("s3", "c_chen_mo", "loc_alley", models::ShotSize::CU,
                 models::FacePose::FRONT),
        });
        CHECK(out == std::vector<std::string>{"s2", "s3"});
    }
}
