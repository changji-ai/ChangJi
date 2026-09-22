#include "stages/json_extract.hpp"

#include "stages/json_partial.hpp"

#include <stdexcept>
#include <string>

#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::stages {

namespace {

/// 找 ``` 代码块里的内容，对应 Python 的
/// re.search(r"```(?:json)?\s*(.+?)```", text, re.S)。
///
/// 手写而不用 std::regex：std::regex 在长输入上会递归到爆栈，
/// 而这里的输入正是"大模型吐的一大段文本"，动辄几万字。
/// 这个模式本身简单到不值得为它冒那个险。
bool find_fenced(const std::string& text, std::string& out) {
    const std::string kFence = "```";
    const std::size_t open = text.find(kFence);
    if (open == std::string::npos) return false;

    std::size_t i = open + kFence.size();
    // (?:json)? —— 可有可无的语言标注
    if (text.compare(i, 4, "json") == 0) i += 4;
    // \s* 是贪婪的，但后面的 (.+?) 至少要一个字符
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
            text[i] == '\r' || text[i] == '\v' || text[i] == '\f')) {
        ++i;
    }
    if (i >= text.size()) return false;

    // (.+?) 非贪婪，所以取到**最近**的那个闭合围栏
    const std::size_t close = text.find(kFence, i + 1);
    if (close == std::string::npos) return false;

    out = text::strip_ws(text.substr(i, close - i));
    return true;
}

/// 找第一个括号平衡的片段。
///
/// 刻意不管字符串里的括号——Python 那边也不管。
/// 内容里有 "}" 的话两边都会失败，行为一致比"我这边更聪明"重要：
/// 对拍时一个能过一个不能过，查起来比两个都失败麻烦得多。
bool find_balanced(const std::string& text, char opener, char closer,
                   json& out) {
    const std::size_t start = text.find(opener);
    if (start == std::string::npos) return false;

    int depth = 0;
    for (std::size_t idx = start; idx < text.size(); ++idx) {
        if (text[idx] == opener) {
            ++depth;
        } else if (text[idx] == closer) {
            --depth;
            if (depth == 0) {
                json parsed = json::parse(text.substr(start, idx - start + 1),
                                          nullptr, /*allow_exceptions=*/false);
                if (parsed.is_discarded()) return false;  // 对应 Python 的 break
                out = std::move(parsed);
                return true;
            }
        }
    }
    return false;
}

/// 这一行里没被转义的 `"` 有几个。
int unescaped_quotes(const std::string& line) {
    int n = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') {
            ++i;
            continue;
        }
        if (line[i] == '"') ++n;
    }
    return n;
}

/// **补上一行末尾漏掉的闭引号。**
///
/// 2026-09-19 拿 Qwen3-4B 真写一章逮到的：一段正文以单引号的对白收尾——
///
///     "……她喉咙里卡着一句话，'我...我忘了给它上发条。',
///
/// 模型把 `'` 当成收口，**忘了写那个 `"`**。整份 8 KB 的 JSON 只坏在这一处，
/// nlohmann 报的却是下一行的「invalid control character」（那个换行被当成
/// 还在字符串里），前面三道一条都救不回来，整章作废。
///
/// 判据故意窄：一行**以 `"` 开头**（数组里的一段、或者 `"key": "` 后面那
/// 半截）、没转义的引号数是**奇数**、去掉尾部空白后**以 `,` 结尾**——正文
/// 里合法的一行引号必然成对，奇数就是漏了一个，而漏的只可能在末尾（开头那个
/// 在）。补在逗号前面。别的形状（漏在中间、漏了逗号）不猜，猜错比不猜糟：
/// 下游拿到一份形状对了内容错位的稿，解析不报错。
std::string close_unfinished_lines(const std::string& text) {
    std::string out;
    std::string line;
    bool changed = false;
    const auto fix = [&](std::string one) {
        const std::string head = text::strip_ws(one);
        if (head.size() >= 2 && head.front() == '"' && head.back() == ',' &&
            unescaped_quotes(one) % 2 == 1) {
            const std::size_t comma = one.rfind(',');
            one.insert(comma, "\"");
            changed = true;
        }
        out += one;
    };
    for (const char c : text) {
        if (c != '\n') {
            line += c;
            continue;
        }
        fix(line);
        out += '\n';
        line.clear();
    }
    fix(line);
    return changed ? out : std::string();
}

/// **把字符串里那些不该结束字符串的 `"` 转义掉。**
///
/// 2026-09-20 采蒸馏语料时逮到的：`[llm.effort] chapter = "low"` 下，
/// glm-5.3 把对白写成 ASCII 直引号而**不转义**——
///
///     "turn": ""她说你迟早会翻。"他说，"她比谁都懂你。""
///
/// 一个裸 `"` 就把整份 JSON 断在那儿。355 次正文调用里坏了 **70 次
/// （20%）**，而且全是 `finish_reason=stop`、字数正常的"好稿"——内容写完了，
/// 只是引号没转义。同一批提示词在 `high` 档只坏 1 次：**想得少，JSON 就
/// 写得糙**。
///
/// 判据：字符串里的 `"`，后面（跳过空白）不是 `,` `:` `]` `}` 之一，就是
/// 正文里的引号。合法 JSON 里字符串的收尾引号后面必然跟着这四个之一。
///
/// ⚠️ **它会猜错的那一种**：模型漏了两个字符串之间的逗号（`"a" "b"`）时，
/// 这一条会把它们并成一个字符串——解得开，但内容错了。所以和别的几道一样
/// **只在前面全败之后跑**（那时候的另一条路是整章作废），而且补出来的那份
/// 照样要过下游的 schema 校验和正文闸门。
std::string escape_stray_quotes(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 32);
    bool in_str = false;
    bool changed = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!in_str) {
            out += c;
            if (c == '"') in_str = true;
            continue;
        }
        if (c == '\\') {
            out += c;
            if (i + 1 < text.size()) out += text[++i];
            continue;
        }
        if (c == '"') {
            std::size_t j = i + 1;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' ||
                                       text[j] == '\r' || text[j] == '\n')) {
                ++j;
            }
            const bool terminator =
                j < text.size() && (text[j] == ',' || text[j] == ':' ||
                                    text[j] == ']' || text[j] == '}');
            if (terminator) {
                out += c;
                in_str = false;
            } else {
                out += "\\\"";
                changed = true;
            }
            continue;
        }
        out += c;
    }
    return changed ? out : std::string();
}

}  // namespace

json extract_json(const std::string& raw) {
    std::string t = text::strip_ws(raw);
    std::string fenced;
    if (find_fenced(t, fenced)) t = fenced;

    json direct = json::parse(t, nullptr, /*allow_exceptions=*/false);
    if (!direct.is_discarded()) return direct;

    json out;
    if (find_balanced(t, '{', '}', out)) return out;
    if (find_balanced(t, '[', ']', out)) return out;

    // **漏了闭引号的行补上再来一遍。** 只在上面三条全败之后跑，所以它只
    // 可能把"失败"变成"成功"；补出来的那份仍然要过下游的 schema 校验。
    if (const std::string closed = close_unfinished_lines(t); !closed.empty()) {
        json fixed = json::parse(closed, nullptr, /*allow_exceptions=*/false);
        if (!fixed.is_discarded()) return fixed;
        if (find_balanced(closed, '{', '}', out)) return out;
        if (find_balanced(closed, '[', ']', out)) return out;
    }

    // **正文里的裸引号转义掉再来一遍。** 同样只在上面几条全败之后跑。
    if (const std::string esc = escape_stray_quotes(t); !esc.empty()) {
        json fixed = json::parse(esc, nullptr, /*allow_exceptions=*/false);
        if (!fixed.is_discarded()) return fixed;
        if (find_balanced(esc, '{', '}', out)) return out;
        if (find_balanced(esc, '[', ']', out)) return out;
    }

    // **最后一道：把半截的补齐再试一次。**
    //
    // 上面三条全是"整份必须是完好的"：直接解、围栏里那段、第一个括号平衡
    // 的片段。模型写到一半被掐断（长度上限、连接抖一下）就一条都不成立，
    // 而那时候前面写好的东西其实都在。2026-09-16 实测：写一章十分钟，
    // 日志里 JSON 开头一切正常（scenes 数组、第一场的 where/pov/who 全齐），
    // 就因为结尾没闭合，整份作废、一个字不剩。
    //
    // `close_partial_json` 本来就是干这个的（写大纲那条边写边看的路在用）。
    // 它**从头扫**，所以只管"开头就是 JSON、结尾没写完"这一种；前面还带着
    // 「好的，这是结果：」之类前缀的那些，仍然归上面 find_balanced 管。
    //
    // 放在最后、只在前面全败之后跑：这条路原来百分之百是抛异常，所以它
    // 只可能把"失败"变成"成功"，不会改变任何一个本来就解得出来的结果。
    // 补出来的那份可能缺东西——**那交给下游的 schema 校验去判**，
    // 这儿只负责"能解出多少算多少"。
    if (const std::string closed = close_partial_json(t); !closed.empty()) {
        json salvaged = json::parse(closed, nullptr, /*allow_exceptions=*/false);
        // **补出来得有东西。** `{不平衡的括号` 这种补完是个空对象——它能解，
        // 但里面什么都没有，当成功往下游送比在这儿抛更糟：下一步会拿着一份
        // 空数据再报一个更难懂的错。语料里钉着这一条要抛。
        if (!salvaged.is_discarded() && !salvaged.empty() &&
            (salvaged.is_object() || salvaged.is_array())) {
            return salvaged;
        }
    }

    // 截 400 字符，和 Python 一致。全贴出来的话，一段几万字的模型输出
    // 会把日志和界面的错误框都撑爆。
    // **按字符截，不是按字节。** 这条消息会进任务快照再序列化成 JSON，
    // 字节截断落在半个汉字上时整个 /api/script/series 都回 500，进度就
    // 看不见了——2026-09-11 实跑撞上的就是这个。
    throw std::runtime_error("大模型输出里找不到合法 JSON：\n" + text::truncate_utf8(raw, 400));
}

}  // namespace changji::stages
