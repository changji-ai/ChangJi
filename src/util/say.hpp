#pragma once

// **引擎说人话的时候说哪国话。**
//
// 界面那 251 句走的是 Qt 的 `qsTr`（`desktop/i18n/`）。可**引擎自己也说
// 话**：体检报告、报错、任务进度、命令行输出——那些是 C++ 里的字面量，
// Qt 够不着。2026-09-21 在繁體那张截图上看见的就是这件事：界面全是繁體，
// 而设置页底下那段体检报告还是简体。
//
// 这儿是那半截的机制。用法：
//
//     return {SAY("中文字体"), Level::WARN, SAY("没找到") + " " + name, ""};
//
// ---
//
// ⚠️ **不是所有中文字面量都该包起来。** 引擎里带中文的字面量有两千多条，
// 而它们是**两类完全不同的东西**：
//
//   给人看的   体检报告、报错、进度、命令行输出         ← 包
//   给模型看的 规则表、schema 描述、工具说明、提示词    ← **一个字都不许动**
//
// 后者一翻就是在改提示词。这个项目里「改一句描述」和「改约束本身」的区别是
// 有血的教训的（CLAUDE.md 第二条、第五条、第六条）——`stages/` 和
// `agent/tools.cpp` 里那几百条属于后者，包上去等于让界面语言决定模型收到
// 什么，那是另一套东西在打架。
//
// 所以这儿**不做全局替换，一处一处手包**，包的时候顺便判一次"这句话是说给
// 谁听的"。
//
// ⚠️ **查不到就原样返回。** 漏翻的那几句显示中文，而不是显示一个键名、
// 也不是空白——空白和"这儿本来就没有"长得一样，而那正是最难查的一类。

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace changji::i18n {

/// 这一趟说哪国话。`""` 或者 `zh_CN` = 原样（源语言就是简体中文）。
///
/// 桌面端在起引擎之前按界面语言设一次；网页那一套按配置或者
/// `Accept-Language` 设。**设一次管整个进程**——不按请求分，因为引擎里
/// 那些话很多是在后台线程上生成的（跑一章的进度、闸门退回的理由），
/// 那时候手边没有"是谁在问"。
void speak(std::string_view lang);

/// 这会儿说的是哪国话。
std::string spoken();

/// 把这一句中文换成当前语言的那一句。**查不到就原样返回。**
const std::string& say(std::string_view zh);

/// 把 `%1` `%2` … 换成给的那几段。
///
/// **和界面那半截同一套记号**（Qt 的 `QString::arg`）：两边一个写法，
/// 翻译的人不用记两套，也不用在两套之间换脑子。
std::string fill(std::string_view pattern, const std::vector<std::string>& args);

/// 一句带空位的话。见下面 `SAYF` 那段。
template <typename... A>
std::string sayf(std::string_view zh, A&&... a) {
    return fill(say(zh), {std::string(std::forward<A>(a))...});
}

/// 一句里嵌着个数的话，**「一」和「几」说法不一样**。
///
/// 英语 `1 chapter` / `2 chapters`，俄语一句话有三档（`1 глава` / `2 главы` /
/// `5 глав`），阿拉伯语有六档。而中文没有这回事——源语言分不出的东西，**翻
/// 译的时候就分不回来**，于是十一份表里那一族全是照 `n > 1` 写死的，界面上
/// 一到 1 就露馅：`1 Spuren`、`Wrote 1 chapters`。
///
/// 这几档的名字和挑法都是 CLDR 的（Qt 的 `<numerusform>` 同一套），挑法写在
/// `plural_of()` 里。**只有这十一种语言**，不是通用实现。
enum class Plural { kZero, kOne, kTwo, kFew, kMany, kOther };

/// 这个数在这种语言里算哪一档。
///
/// ⚠️ **不是"1 和别的"那么简单**：俄语 21 和 1 同档、22 和 2 同档、11 反而
/// 和 5 同档；阿拉伯语 0 自己一档、2 自己一档。所以挑法按语言写，不共用。
Plural plural_of(std::string_view lang, long long n);

/// 按当前语言和这个数挑出该说的那一档。**挑不着就退回原话**（同 `say`）。
const std::string& say_n(std::string_view zh, long long n);

/// 把 `%n` 换成这个数本身。**`%1`…`%9` 不动**——那几个是 `fill` 的活。
std::string count_in(std::string_view pattern, long long n);

/// 这一句、这种语言，这一档给了没有。**守卫用**：见
/// `test_say.cpp` 的「每一句带 %n 的话，十一种语言都给齐了自己那几档」。
bool has_form(std::string_view lang, std::string_view zh, Plural p);

/// 这种语言的表里所有带 `%n` 的键。守卫用。
std::vector<std::string> plural_keys(std::string_view lang);

/// 一句带数的话。见下面 `SAYN`。
template <typename... A>
std::string sayn(std::string_view zh, long long n, A&&... a) {
    // **先填 `%n` 再填 `%1`**：反过来的话，某个参数里恰好带着 `%n` 两个字符
    // 就会被当成这个数吃掉。`fill` 只走一趟、填进去的不再扫，所以这个顺序
    // 下数字是安全的。
    return fill(count_in(say_n(zh, n), n), {std::string(std::forward<A>(a))...});
}

/// 这一段话说给谁听。
///
/// ⚠️ **有几段话有两个读者。** `agent/tools.cpp` 里那几个 `*_read` 回的
/// 正文，模型要读；而 `/api/peek`（「就地看一眼」那五格）把**同一段话原样
/// 摆到界面上**。模型那一份必须是中文原话——翻了就是改了模型收到的东西，
/// 而那正是上面那条"给模型看的一个字都不许动"要挡的事。
///
/// **不能靠 `speak()` 临时拨一下再拨回来**：它是**整个进程一个**（见上面
/// 那段），后台线程这会儿正拿着它生成进度和闸门的话，拨过去就把别人正在
/// 说的那一句也拨歪了，而且不报错。
///
/// 所以把"说给谁听"写成一个参数，一路传下去——同一份渲染，两个出口：
///
///     std::string render(const json& body, i18n::Audience to);
///     ...
///     out += SAYF_TO(to, "共 %1 章：", std::to_string(n));
///
/// `Audience::model()` 原样返回（一个字节都不变），`Audience::human()` 查表。
class Audience {
 public:
    /// 给模型看：原样，不查表。
    static Audience model() { return Audience(false); }
    /// 给人看：查表。
    static Audience human() { return Audience(true); }

    bool is_human() const { return human_; }

    std::string operator()(std::string_view zh) const {
        return human_ ? say(zh) : std::string(zh);
    }

    template <typename... A>
    std::string f(std::string_view zh, A&&... a) const {
        return fill((*this)(zh), {std::string(std::forward<A>(a))...});
    }

    /// 带数的那一种。模型那一份**连档都不挑**——原话就是原话。
    template <typename... A>
    std::string n(std::string_view zh, long long count, A&&... a) const {
        std::string one = human_ ? say_n(zh, count) : std::string(zh);
        return fill(count_in(one, count), {std::string(std::forward<A>(a))...});
    }

 private:
    explicit Audience(bool human) : human_(human) {}
    bool human_ = false;
};

}  // namespace changji::i18n

/// 包一句给人看的话。写起来短，读起来一眼看得出"这句是给人看的"。
#define SAY(zh) ::changji::i18n::say(zh)

/// 一句中间要填东西的话。**别拿 `+` 拼**：
///
///     "配置里 [models.pick]." + key + " 写的是「" + v + "」，这一组里没有"
///
/// 这么写出来的不是一句话，是三个半截。翻译的人拿到「写的是「」和「，这一
/// 组里没有」两块碎片，**既不知道中间填什么，也改不了语序**——而好几种语言
/// 里语序就是不一样的（德语动词跑到最后、阿拉伯语整句从右往左）。
/// 一句话一个键，空位写成 `%1` `%2`：
///
///     SAYF("配置里 [models.pick].%1 写的是「%2」，这一组里没有这一档", key, v)
#define SAYF(zh, ...) ::changji::i18n::sayf(zh, __VA_ARGS__)

/// 一句里嵌着个数、而「一」和「几」说法不一样的话。见 `i18n::Plural`。
///
/// **数写成 `%n`**，别的空位还是 `%1` `%2`（和界面那半截 Qt 的
/// `tr("%n …", "", n)` 一个记号）：
///
///     SAYF("写完了 %1 章", std::to_string(done))   // 英语下 1 章也说 chapters
///     SAYN("写完了 %n 章", done)                    // 一档一句，各说各的
///
/// ⚠️ **别拿 `%1` 当那个数。** 挑哪一档是按 `n` 算的，而 `%1` 只是一段文本
/// ——写成 `SAYN("写完了 %1 章", done, std::to_string(done))` 也能出对的字，
/// 可十一份表里那一族就得同时维护两个空位，抄漏一个就错位。
///
/// ⚠️ **表里那一族的值是个对象，不是一句话**：
///
///     "写完了 %n 章": { "one": "Wrote %n chapter", "other": "Wrote %n chapters" }
///
/// 哪几种语言该给哪几档，`gen_say.py` 建表的时候按 `plural_of` 自己数，
/// 给漏了当场不让过——**不另立一张单子**。
// **写成 `(...)` 是有原因的**：这一族常常只有两个参数（`SAYN(zh, n)`），
// 而 `#define SAYN(zh, n, ...)` 在 C++17 里那样调就多一个逗号——`__VA_OPT__`
// 要 C++20，这个工程是 17（见 `cpp/CMakeLists.txt` 那段为什么）。
#define SAYN(...) ::changji::i18n::sayn(__VA_ARGS__)

/// 只登记，不翻译：**句子写在这儿，翻译推迟到用它的那一行**。
///
/// 表要建在静态存储期的数组上（`static const char* kSlotNames[]` 这种）
/// 时用它。在那儿直接 `SAY()` 是不行的——静态初始化跑在 `speak()` 之前，
/// 语言会被冻在建表的那一刻。所以表里存中文原文，用的那一行再
/// `SAY(kSlotNames[i])`。
///
/// 那样一来句子就**从抽取器眼皮底下溜掉了**：`cpp/tools/say_scan.py` 看的是
/// `SAY(` 后面跟没跟一个字面量，而那儿跟的是个变量。包上这个宏，它就数得到
/// ——展开之后一个字符都没变，纯粹是给工具看的记号。Qt 那边的 `QT_TR_NOOP`
/// 是同一件事。
///
///     static const char* kSlotNames[] = {SAY_NOOP("正面"), SAY_NOOP("背面")};
///     ...
///     label = name + " " + SAY(kSlotNames[i]);   // 这一行才真翻
#define SAY_NOOP(zh) zh

/// 判过了：**这句中文不是界面**，一个字都不许翻。
///
/// 展开之后一个字符都没变，和 `SAY_NOOP` 一样纯粹是个记号——区别在于
/// `SAY_NOOP` 说的是"表里存原文，晚点再翻"，这个说的是"永远不翻"。
///
/// 给的是**混着两种东西的文件**用的：整份都不该翻的（`util/text.cpp` 的
/// 切章词表、`models/character.cpp` 的性别词表）走 `cpp/tools/say_scan.py`
/// 里的 `NOT_UI`，一个文件一条理由；而 `http/batch.cpp` 那种一半是界面、
/// 一半是解析器的，判断得落在**那一行**上，否则尺子量出来的"还欠多少"
/// 永远混着一批本来就不该动的。
///
/// ⚠️ **包上它之前先想清楚坏起来什么样。** 这几类是真判过的：
///   · 解析器的词表（比的是磁盘上或用户粘进来的中文，翻了匹配不上，
///     而且不报错）；
///   · 接口/存盘的键（`to_string(Slot)`、`fit` 那几个取值，翻了就是改了
///     接口，golden 逐字钉着）；
///   · 走 stderr 的日志（桌面端没有日志格）。
#define SAY_NEVER(zh) zh

/// 一句**两个读者**的话。见 `i18n::Audience`。
///
/// 和 `SAY()` 的区别只在于翻不翻由参数决定，不由进程的全局语言决定：
/// 同一行代码，模型那一遍拿到中文原话，人那一遍拿到自己的语言。
///
///     return SAY_TO(to, "还没有故事。");
#define SAY_TO(to, zh) (to)(zh)

/// 带空位的那一种。规矩同 `SAYF`：**一句话一个键，别拿 `+` 拼**。
///
///     return SAYF_TO(to, "%1 · %2 字", util::chapter_word(ep, to), n);
#define SAYF_TO(to, zh, ...) (to).f(zh, __VA_ARGS__)

/// 两个读者、又带个数的那一种。规矩同 `SAYN`：**数写成 `%n`**。
#define SAYN_TO(to, ...) (to).n(__VA_ARGS__)
