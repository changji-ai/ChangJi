// 引擎说人话那一套（`util/say.hpp`）。
//
// 界面那 251 句走 Qt 的翻译，**引擎自己说的话走这儿**：体检报告、报错、
// 进度、命令行输出。2026-09-21 在繁體那张截图上看见的就是这件事——界面全是
// 繁體，而设置页底下那段体检报告还是简体。
//
// 这条用例钉的是三件，每一件坏起来都不报错：
//
//   · **查不到就原样返回**（漏翻的显示中文，不是空白、不是键名）；
//   · **说源语言时一个字都不换**；
//   · **带地区的退一步再找**（`pt_BR` 找不到就试 `pt`）——系统给的常常是
//     带地区的那一种，而我们不一定每个地区都备了一份。

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "util/chapter_word.hpp"
#include "util/say.hpp"

using namespace changji;

namespace {

/// 跑完把语言放回去。**别的用例可能也在看它**，而这是个进程级的开关。
struct ScopedLang {
    std::string old_ = i18n::spoken();
    explicit ScopedLang(const std::string& lang) { i18n::speak(lang); }
    ~ScopedLang() { i18n::speak(old_); }
};


/// 去掉 `//` 和 `/* */`，字符串里的照留。
///
/// 只给底下那条"读源码"的用例用，够用就行：它要数的是**真正的调用**，
/// 而注释里提到宏名是正当的。
std::string strip_comments(const std::string& src) {
    std::string out;
    for (std::size_t i = 0; i < src.size();) {
        if (src[i] == '"') {                       // 字符串：原样抄过去
            out += src[i++];
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < src.size()) out += src[i++];
                out += src[i++];
            }
            if (i < src.size()) out += src[i++];
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        out += src[i++];
    }
    return out;
}

}  // namespace

TEST_CASE("说人话：查不到就原样") {
    ScopedLang en("en");
    // 这一句不在任何一张表里。**回的必须是它自己**——回空串的话，
    // 界面上那一处就是一片空白，而"空白"和"这儿本来就没有"长得一样。
    CHECK(SAY("这一句谁都没翻过就是要看它原样回来") ==
          "这一句谁都没翻过就是要看它原样回来");
}

TEST_CASE("说人话：源语言一个字都不换") {
    for (const char* lang : {"", "zh_CN", "zh-CN", "zh"}) {
        CAPTURE(lang);
        ScopedLang zh(lang);
        CHECK(SAY("中文字体") == "中文字体");
        CHECK(SAY("未找到") == "未找到");
    }
}

TEST_CASE("说人话：换了语言就换那一句") {
    ScopedLang en("en");
    // 这几句在 `cpp/i18n/engine_en.json` 里。**改那个文件要重新跑
    // `gen_say.py`**，不跑的话这条用例先红——它钉的正是"表进没进二进制"。
    CHECK(SAY("中文字体") == "Chinese font");
    CHECK(SAY("未找到") == "not found");
}

TEST_CASE("说人话：带地区的退一步再找") {
    // `en_US` 没有单独一张表，该落到 `en` 上。系统给的常常是带地区的那一种。
    ScopedLang en("en_US");
    CHECK(SAY("中文字体") == "Chinese font");
    CHECK(i18n::spoken() == "en_US");   // 记的是人给的那个，不是退到的那个
}

TEST_CASE("说人话：不认识的语言就说源语言") {
    ScopedLang nope("xx_YY");
    CHECK(SAY("中文字体") == "中文字体");
}

// ---- 一句话两个读者 ----
//
// `agent/tools.cpp` 那几个 `*_read` 回的正文，模型要读，而 `/api/peek`
//（「就地看一眼」那五格）把**同一段话原样摆到界面上**。两个读者要两种语言，
// 而 `speak()` 是整个进程一个——所以"说给谁听"是个参数（`i18n::Audience`）。
//
// 这两条钉的是这件事的两半，都在**非中文**下跑，中文下它俩什么也证明不了。

TEST_CASE("两个读者：给模型那一份一个字节都不变，给人那一份才查表") {
    ScopedLang de("de");

    // 给模型：原样。**这是这套东西存在的理由**——翻了就是改了模型收到的
    // 提示词，而那是另一套东西在打架（见 util/say.hpp 开头）。
    CHECK(SAY_TO(i18n::Audience::model(), "还没有故事。") == "还没有故事。");
    CHECK(SAYF_TO(i18n::Audience::model(), "共 %1 章：\n", "3") == "共 3 章：\n");

    // 给人：查表。查得到就换了，换不掉说明这句话根本没进表。
    const std::string human = SAY_TO(i18n::Audience::human(), "还没有故事。");
    CHECK(human != "还没有故事。");
    CHECK(human == SAY("还没有故事。"));

    // `chapter_word` 是同一件事：`ep03` → 「第 3 章」，模型那份要中文。
    // **默认给模型**——它最早只有模型在用。
    CHECK(util::chapter_word("ep03") == "第 3 章");
    CHECK(util::chapter_word("ep03", i18n::Audience::model()) == "第 3 章");
    CHECK(util::chapter_word("ep03", i18n::Audience::human()) != "第 3 章");
}

TEST_CASE("两个读者：agent/tools.cpp 里不许出现光秃秃的 SAY(") {
    // **漏一处不报错。** 在那个文件里写一句 `SAY("…")`，编得过、跑得动，
    // 只是模型从此按界面语言收到提示词——而那是"让界面语言决定模型收到
    // 什么"，正是整套 `Audience` 要挡的事。看得见的症状要等到有人把界面
    // 切成德语、再让代理干一件活，才在输出质量上慢慢显出来。
    //
    // 做法同 `test_rail.cpp` 那条跨语言的用例：直接读源码。
    const std::filesystem::path p =
        std::filesystem::path(CHANGJI_AGENT_SRC_DIR) / "tools.cpp";   // 编进去的那一份
    std::ifstream in(p);
    REQUIRE_MESSAGE(in.good(), "读不到 " << p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    // ⚠️ **注释要剥掉。** 那个文件的文档注释里就写着一句 `SAY()`（解释
    // 「detail 那一半已经是当前界面语言了」的那一段）——不剥的话这条用例
    // 逮的是一句说明文字。2026-09-22 写完当场红在这儿。
    const std::string src = strip_comments(ss.str());

    // 只认**真正的调用**：`SAY(` / `SAYF(` 前面挨着字母的不算
    //（`SAY_TO(` 里没有 `SAY(`，但 `SAYF_TO(` 里也没有 `SAYF(`——
    // 这儿挡的是 `_` 之外真跟着括号的那一种）。
    for (const std::string& call : {std::string("SAY("), std::string("SAYF(")}) {
        std::size_t at = 0;
        while ((at = src.find(call, at)) != std::string::npos) {
            const bool glued =
                at > 0 && (std::isalnum(static_cast<unsigned char>(src[at - 1]))
                           || src[at - 1] == '_');
            CHECK_MESSAGE(glued,
                          "agent/tools.cpp 里出现了光秃秃的 " << call
                          << "——这个文件里的话是给模型看的，要翻得走 SAY_TO(to, …)");
            at += call.size();
        }
    }
}

// ---------------------------------------------------------------------------
// 带数的那一族（`SAYN` / `i18n::Plural`）
//
// 中文分不出「一章」和「几章」，所以十一份表里那一族一直是照 `n > 1` 写死的
// ——界面上一到 1 就露馅：`Wrote 1 chapters`、`1 Spuren`。这几条钉的是：
//
//   · **挑档的规矩照 CLDR**，几处反直觉的地方一条一条摆着（俄语 21 和 1
//     同档、11 和 5 同档；阿拉伯语 0 和 2 各自一档）；
//   · **十一种语言都给齐了自己那几档**——漏一档不会报错，会安静地退到
//     别的档上，于是俄语里 2 说成 5 的样子；
//   · **给模型那一份连档都不挑**。

TEST_CASE("带数的话：挑档按 CLDR，不是「1 和别的」") {
    using changji::i18n::Plural;
    using changji::i18n::plural_of;

    // 只有一档的那几种。
    for (const char* lang : {"zh_TW", "ja", "ko"}) {
        CAPTURE(std::string(lang));
        for (long long n : {0, 1, 2, 5, 11, 21, 100}) {
            CAPTURE(n);
            CHECK(plural_of(lang, n) == Plural::kOther);
        }
    }

    // 英德西：只有 1 是 one。
    for (const char* lang : {"en", "de", "es"}) {
        CAPTURE(std::string(lang));
        CHECK(plural_of(lang, 0) == Plural::kOther);
        CHECK(plural_of(lang, 1) == Plural::kOne);
        CHECK(plural_of(lang, 2) == Plural::kOther);
    }

    // 法葡：**0 跟 1 同档**。
    for (const char* lang : {"fr", "pt_BR"}) {
        CAPTURE(std::string(lang));
        CHECK(plural_of(lang, 0) == Plural::kOne);
        CHECK(plural_of(lang, 1) == Plural::kOne);
        CHECK(plural_of(lang, 2) == Plural::kOther);
    }

    // 印地语：**CLDR 说 0 跟 1 同档，Qt 说不是，这儿跟 Qt。**
    // 为什么跟 Qt（两半截的话摆在同一块屏上），见 util/say.cpp；
    // 跟上没跟上另有一条用例盯着（「同一个数得落在同一档上」）。
    CHECK(plural_of("hi", 0) == Plural::kOther);
    CHECK(plural_of("hi", 1) == Plural::kOne);
    CHECK(plural_of("hi", 2) == Plural::kOther);

    // 俄语三档，看个位，但 11–14 要挖掉。
    CHECK(plural_of("ru", 1) == Plural::kOne);
    CHECK(plural_of("ru", 21) == Plural::kOne);      // 21 和 1 同档
    CHECK(plural_of("ru", 2) == Plural::kFew);
    CHECK(plural_of("ru", 4) == Plural::kFew);
    CHECK(plural_of("ru", 22) == Plural::kFew);
    CHECK(plural_of("ru", 5) == Plural::kMany);
    CHECK(plural_of("ru", 11) == Plural::kMany);     // 11 反而和 5 同档
    CHECK(plural_of("ru", 12) == Plural::kMany);
    CHECK(plural_of("ru", 14) == Plural::kMany);
    CHECK(plural_of("ru", 0) == Plural::kMany);
    CHECK(plural_of("ru", 111) == Plural::kMany);    // 111 也是（末两位 11）
    CHECK(plural_of("ru", 101) == Plural::kOne);

    // 阿拉伯语六档全用得上。
    CHECK(plural_of("ar", 0) == Plural::kZero);
    CHECK(plural_of("ar", 1) == Plural::kOne);
    CHECK(plural_of("ar", 2) == Plural::kTwo);
    CHECK(plural_of("ar", 3) == Plural::kFew);
    CHECK(plural_of("ar", 10) == Plural::kFew);
    CHECK(plural_of("ar", 11) == Plural::kMany);
    CHECK(plural_of("ar", 99) == Plural::kMany);
    CHECK(plural_of("ar", 100) == Plural::kOther);
    CHECK(plural_of("ar", 101) == Plural::kOther);

    // 不认识的语言：一档，说得出话。**别空白**。
    CHECK(plural_of("xx", 1) == Plural::kOther);
}

TEST_CASE("带数的话：十一种语言都给齐了自己那几档") {
    using changji::i18n::has_form;
    using changji::i18n::plural_keys;
    using changji::i18n::plural_of;

    // ⚠️ **"该给哪几档"是跑出来的，不是另立的一张单子**：把 0…200 过一遍
    // `plural_of`，跑出来哪几档就得给哪几档。规矩变了，这条跟着变。
    int checked = 0;
    for (const std::string& lang : {"en", "zh_TW", "ja", "ko", "es", "fr",
                                    "de", "pt_BR", "ru", "ar", "hi"}) {
        CAPTURE(lang);
        const auto keys = plural_keys(lang);
        REQUIRE_MESSAGE(!keys.empty(), "engine_" << lang << ".json 里一句带数的都没有");
        for (const std::string& zh : keys) {
            CAPTURE(zh);
            for (long long n = 0; n <= 200; ++n) {
                const auto want = plural_of(lang, n);
                if (has_form(lang, zh, want)) continue;
                CAPTURE(n);
                FAIL_CHECK("这一档没给，界面上 " << n << " 会退到别的档去说");
                break;
            }
            ++checked;
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("带数的话：1 和 2 说出来不一样") {
    // 英语：最好认的那一种。
    {
        ScopedLang g("en");
        CHECK(SAYN("写完了 %n 章", 1) == "Wrote 1 chapter");
        CHECK(SAYN("写完了 %n 章", 2) == "Wrote 2 chapters");
        CHECK(SAYN("装配 %n 个镜头", 1) == "Assembling 1 shot");
        CHECK(SAYN("装配 %n 个镜头", 7) == "Assembling 7 shots");
    }
    // 俄语：三档，而且 21 跟着 1 走。
    {
        ScopedLang g("ru");
        CHECK(SAYN("装配 %n 个镜头", 1) == "Собираю 1 кадр");
        CHECK(SAYN("装配 %n 个镜头", 2) == "Собираю 2 кадра");
        CHECK(SAYN("装配 %n 个镜头", 5) == "Собираю 5 кадров");
        CHECK(SAYN("装配 %n 个镜头", 21) == "Собираю 21 кадр");
        CHECK(SAYN("装配 %n 个镜头", 11) == "Собираю 11 кадров");
    }
    // 阿拉伯语：0 和 2 各自一档，而且这两档**根本不带那个数**——正确的
    // 说法就是不带（«لقطة واحدة» 自己就是"一个"）。
    {
        ScopedLang g("ar");
        CHECK(SAYN("装配 %n 个镜头", 1) == "أجمّع لقطة واحدة");
        CHECK(SAYN("装配 %n 个镜头", 2) == "أجمّع لقطتين");
        CHECK(SAYN("装配 %n 个镜头", 3) == "أجمّع 3 لقطات");
        CHECK(SAYN("装配 %n 个镜头", 30) == "أجمّع 30 لقطة");
    }
    // 源语言：中文分不出档，那个数照样填进去。
    {
        ScopedLang g("");
        CHECK(SAYN("装配 %n 个镜头", 1) == "装配 1 个镜头");
        CHECK(SAYN("装配 %n 个镜头", 9) == "装配 9 个镜头");
    }
}

TEST_CASE("带数的话：给模型那一份一个字节都不变") {
    ScopedLang g("en");
    const auto to_model = i18n::Audience::model();
    const auto to_human = i18n::Audience::human();
    CHECK(SAYN_TO(to_model, "装配 %n 个镜头", 3) == "装配 3 个镜头");
    CHECK(SAYN_TO(to_human, "装配 %n 个镜头", 3) == "Assembling 3 shots");
}

TEST_CASE("带数的话：%n 换掉，%1 留给 fill") {
    // `count_in` 只认 `%n`。路径和百分号里真有 `%`，多吞一个就是另一个串。
    CHECK(i18n::count_in("%n 章 / %1 场", 3) == "3 章 / %1 场");
    CHECK(i18n::count_in("100%% 和 %note", 3) == "100%% 和 3ote");
    CHECK(i18n::count_in("没有空位", 3) == "没有空位");
    CHECK(i18n::count_in("结尾是 %", 3) == "结尾是 %");
}

TEST_CASE("带数的话：界面那半截和引擎这半截，同一个数得落在同一档上") {
    // **这一条是两套机制之间的接缝。** 镜头墙那一行的复数是 Qt 挑的档
    //（`<numerusform>`），底下进度条那一行是 `plural_of` 挑的——**同一块屏、
    // 同一个数**。挑法差一处，屏幕上就是一句单数一句复数，而且不报错。
    //
    // ⚠️ **对照表是问出来的，不是推的**：`cpp/i18n/qt_plural_forms.tsv`
    // 拿 `cpp/tools/plural_probe.cpp` 把编好的 `.qm` 装起来，n=0…130 一个
    // 一个问过 Qt。实测十一种语言里**十种和 CLDR 一处不差，只有印地语的 0
    // 不一样**（CLDR 算单数，Qt 算复数）——那一处跟了 Qt，因为 Qt 那套
    // 改不了。这条用例钉的就是"跟上了"。
    using changji::i18n::Plural;
    using changji::i18n::plural_of;

    const std::filesystem::path p =
        std::filesystem::path(CHANGJI_SRC_DIR).parent_path() / "i18n" / "qt_plural_forms.tsv";
    std::ifstream in(p);
    REQUIRE_MESSAGE(in.good(), "读不到 " << p.string()
                    << "（重新量：见 cpp/tools/plural_probe.cpp）");

    int langs = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string lang;
        int forms = 0;
        row >> lang >> forms;
        CAPTURE(lang);

        // **这种语言用得上哪几档，是跑 `plural_of` 跑出来的**，不另立单子。
        std::vector<Plural> used;
        for (long long n = 0; n <= 200; ++n) {
            const Plural c = plural_of(lang, n);
            if (std::find(used.begin(), used.end(), c) == used.end()) used.push_back(c);
        }
        std::sort(used.begin(), used.end(), [](Plural a, Plural b) {
            return static_cast<int>(a) < static_cast<int>(b);
        });
        CHECK_MESSAGE(static_cast<int>(used.size()) == forms,
                      "档数对不上：Qt 的 .ts 里 " << forms << " 档，plural_of 跑出来 "
                      << used.size() << " 档");

        int n = 0, want = 0, checked = 0;
        while (row >> want) {
            const auto it = std::find(used.begin(), used.end(), plural_of(lang, n));
            const int got = static_cast<int>(it - used.begin());
            if (got != want) {
                CAPTURE(n);
                FAIL_CHECK("Qt 把 " << n << " 放在第 " << want << " 档，plural_of 放在第 "
                           << got << " 档——同一块屏上会一句单数一句复数");
                break;
            }
            ++n;
            ++checked;
        }
        CHECK_MESSAGE(checked > 100, "这一行的数太少，量的时候是不是截断了");
        ++langs;
    }
    CHECK_MESSAGE(langs == 11, "对照表里该有十一种语言，读到 " << langs << " 种");
}
