#include "util/say.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace changji::i18n {

namespace {

/// 各语言那几张表。**生成的**，见 `cpp/tools/gen_say.py`：
/// `cpp/i18n/engine_<语言>.json` → 这个头文件。
#include "util/say_tables.inc.hpp"

std::mutex& lock() {
    static std::mutex m;
    return m;
}

/// 这会儿用哪一张表。**空指针 = 说源语言**（简体中文，也就是原样）。
const Table*& current() {
    static const Table* t = nullptr;
    return t;
}

std::string& current_lang() {
    static std::string s;
    return s;
}

}  // namespace

void speak(std::string_view lang) {
    std::lock_guard<std::mutex> g(lock());
    current_lang().assign(lang);
    current() = nullptr;
    if (lang.empty() || lang == "zh_CN" || lang == "zh-CN" || lang == "zh") return;
    for (const auto& one : kTables) {
        if (lang == one.lang) { current() = &one; return; }
    }
    // ⚠️ **退一步再找一次**：`pt_BR` 没有就试 `pt`，`zh_TW` 没有就试 `zh`
    // ——系统给的常常是带地区的那一种，而我们不一定每个地区都备了一份。
    const auto cut = lang.find('_');
    if (cut == std::string_view::npos) return;
    const auto base = lang.substr(0, cut);
    for (const auto& one : kTables) {
        if (base == one.lang) { current() = &one; return; }
    }
}

std::string spoken() {
    std::lock_guard<std::mutex> g(lock());
    return current_lang();
}

const std::string& say(std::string_view zh) {
    // 原样那一档：**连查都不查**。源语言是简体中文，说中文的人走的是这条路，
    // 一次哈希都不该花。
    const Table* t = nullptr;
    {
        std::lock_guard<std::mutex> g(lock());
        t = current();
    }
    if (t == nullptr) {
        // 回一个引用，所以得有个地方存。按原文缓存，一句只存一次。
        static std::mutex m;
        static std::unordered_map<std::string, std::string> keep;
        std::lock_guard<std::mutex> g(m);
        auto it = keep.find(std::string(zh));
        if (it == keep.end()) it = keep.emplace(std::string(zh), std::string(zh)).first;
        return it->second;
    }
    const auto it = t->rows.find(zh);
    if (it == t->rows.end()) {
        // 查不到就原样（同上，得有个地方存）。
        static std::mutex m;
        static std::unordered_map<std::string, std::string> keep;
        std::lock_guard<std::mutex> g(m);
        auto k = keep.find(std::string(zh));
        if (k == keep.end()) k = keep.emplace(std::string(zh), std::string(zh)).first;
        return k->second;
    }
    return it->second;
}

/// 这个数在这种语言里算哪一档。
///
/// ⚠️ **规矩是 CLDR 的，一条一条抄下来的，不是想出来的。** 几条反直觉的，
/// 写在这儿免得有人"顺手简化"：
///   · 俄语 **21 和 1 同档**（`одна глава`）、**11 和 5 同档**（`глав`）
///     ——看的是个位，但 11–14 要挖掉；
///   · 阿拉伯语 **0 自己一档**、**2 自己一档**；
///   · 法语和葡语 **0 跟 1 同档**（`0 chapitre`），英语德语西语不是。
///
/// 只认这十一种。**认不出的一律 `kOther`**：那正是"只有一档"的语言
/// （中日韩）该走的路，也是漏了一种语言时最不坏的样子——一句话，不是空白。
Plural plural_of(std::string_view lang, long long n) {
    std::string_view base = lang;
    if (const auto cut = base.find('_'); cut != std::string_view::npos) {
        base = base.substr(0, cut);
    }
    const long long v = n < 0 ? -n : n;
    const long long m10 = v % 10;
    const long long m100 = v % 100;

    if (base == "ru") {
        if (m10 == 1 && m100 != 11) return Plural::kOne;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return Plural::kFew;
        return Plural::kMany;
    }
    if (base == "ar") {
        if (v == 0) return Plural::kZero;
        if (v == 1) return Plural::kOne;
        if (v == 2) return Plural::kTwo;
        if (m100 >= 3 && m100 <= 10) return Plural::kFew;
        if (m100 >= 11 && m100 <= 99) return Plural::kMany;
        return Plural::kOther;
    }
    // 法语、葡语：0 和 1 一档。
    if (base == "fr" || base == "pt") {
        return (v == 0 || v == 1) ? Plural::kOne : Plural::kOther;
    }
    // ⚠️ **印地语跟 Qt，不跟 CLDR。** CLDR 说 `i = 0 or n = 1` 算单数，
    // 也就是 0 跟 1 同档；Qt 不是，Qt 的印地语是"只有 1"。这两半截的话
    // **摆在同一块屏上**（镜头墙那一行是 Qt 的，进度条那一行是这儿的），
    // 一个说「0 अध्याय जोड़ा गया」一个说「जोड़े गए」就是半新半旧的样子，
    // 而 Qt 那套我改不了。所以跟 Qt。
    //
    // 这不是猜的：`cpp/i18n/qt_plural_forms.tsv` 是拿 `cpp/tools/plural_probe.cpp`
    // 真去问编好的 `.qm` 问出来的，十一种语言 n=0…130 逐个对过——
    // **只有印地语的 0 这一处两家不一样**，别的十种一处不差。
    // `test_say.cpp` 那条守卫盯着这份对照表。
    if (base == "en" || base == "de" || base == "es" || base == "hi") {
        return v == 1 ? Plural::kOne : Plural::kOther;
    }
    // 中日韩：只有一档。
    return Plural::kOther;
}

namespace {

std::size_t slot(Plural p) { return static_cast<std::size_t>(p); }

/// 挑不着这一档就往回退：**先 other，再 many，再随便哪一档有字的**。
///
/// 退到哪一档都比空白强——空白和"这儿本来就没有"长得一样，那是最难查的
/// 一类（同 `say` 那条）。全空就当这个键不存在，交回给调用方退回原话。
const std::string* pick(const Forms& f, Plural p) {
    static const Plural kFallback[] = {Plural::kOther, Plural::kMany, Plural::kOne,
                                       Plural::kFew,   Plural::kTwo,  Plural::kZero};
    if (!f[slot(p)].empty()) return &f[slot(p)];
    for (const Plural q : kFallback) {
        if (!f[slot(q)].empty()) return &f[slot(q)];
    }
    return nullptr;
}

const Table* table_for(std::string_view lang) {
    for (const auto& one : kTables) {
        if (lang == one.lang) return &one;
    }
    return nullptr;
}

}  // namespace

const std::string& say_n(std::string_view zh, long long n) {
    const Table* t = nullptr;
    std::string lang;
    {
        std::lock_guard<std::mutex> g(lock());
        t = current();
        lang = current_lang();
    }
    if (t != nullptr) {
        const auto it = t->plurals.find(zh);
        if (it != t->plurals.end()) {
            if (const std::string* one = pick(it->second, plural_of(lang, n))) return *one;
        }
        // 带 `%n` 的键不在复数表里的话，再按普通那张表查一次——**一句话从
        // `SAYF` 改成 `SAYN` 的那一刻，十一份表还是老样子**，这时候退回那
        // 一句老译文（里头是 `%1`）比退回中文强得多。
    }
    return say(zh);
}

std::string count_in(std::string_view pattern, long long n) {
    const std::string num = std::to_string(n);
    std::string out;
    out.reserve(pattern.size() + num.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size() && pattern[i + 1] == 'n') {
            out += num;
            ++i;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

bool has_form(std::string_view lang, std::string_view zh, Plural p) {
    const Table* t = table_for(lang);
    if (t == nullptr) return false;
    const auto it = t->plurals.find(zh);
    if (it == t->plurals.end()) return false;
    return !it->second[slot(p)].empty();
}

std::vector<std::string> plural_keys(std::string_view lang) {
    std::vector<std::string> out;
    const Table* t = table_for(lang);
    if (t == nullptr) return out;
    for (const auto& kv : t->plurals) out.emplace_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::string fill(std::string_view pattern, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        // **只认 `%1`…`%9`。** `%` 后面不是那几个数字的照原样抄过去
        // ——路径和百分比里真有 `%`，吞掉就成了另一个串。
        if (pattern[i] != '%' || i + 1 >= pattern.size()
            || pattern[i + 1] < '1' || pattern[i + 1] > '9') {
            out += pattern[i];
            continue;
        }
        const std::size_t k = static_cast<std::size_t>(pattern[i + 1] - '1');
        // 给少了的时候**把空位原样留着**，不填空串：空串和"这儿本来就没有"
        // 长得一样，而 `%2` 明摆着是漏了一段，看一眼就知道找谁。
        if (k < args.size()) out += args[k];
        else { out += pattern[i]; out += pattern[i + 1]; }
        ++i;
    }
    return out;
}

}  // namespace changji::i18n
