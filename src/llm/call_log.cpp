#include "llm/call_log.hpp"
#include "util/say.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <utility>

#include "http/job_stream.hpp"      // current_stream
#include "pipeline/activity.hpp"    // current_activity
#include "pipeline/task_board.hpp"  // task_facts
#include "util/paths.hpp"
#include "util/text.hpp"

// 记录器本体。形状和"为什么是这个形状"写在 call_log.hpp 上，这儿只补实现上
// 踩得着的几处。
//
// ⚠️ **`index.jsonl` 永不删。** 剪枝那一段（prune_locked）只删正文文件。
// 一行几百字节，而它是统计的唯一依据；删掉正文之后那些行还在，只是点不开
// 原文。做成"统计数据也跟着没了"的话，这个功能就只剩最近那几天能用。
// 这一条同时写在契约、settings.hpp 的注释和这儿——**它是最容易被下一个人
// "顺手优化"掉的一条**。
//
// ⚠️ **不许每次调用都扫全目录。** 两万份文件的一次 directory_iterator 是几十
// 毫秒，乘以一部电影几百次调用就是实打实的拖慢。所以一趟进程只扫一次
// （每个 root 一次），之后每写一次就把新文件加进那本进程内的账。

namespace changji::llm {
namespace {

namespace fs = std::filesystem;

/// 取不到项目时的文件夹名。**不是空串**：空串会让所有"没有项目"的调用直接
/// 落在 llm_log/ 根上，和项目子目录混在一起，扫描和剪枝都要多一层判断。
// ⚠️ **这是盘上的目录名，不是话。** 落盘、扫描、剪枝三处都按它找；
// 翻了的话换一次界面语言就换一个目录，旧的日志再也扫不到、也剪不掉。
constexpr const char* kNoProject = SAY_NEVER("_无项目");

/// 正文文件的后缀。**落盘、扫描、剪枝三处都从这一张表取**，别在别处再写一遍
/// 字面量：剪枝手里只有 id 和项目子目录名，那几条路径是 `dir / (id + 后缀)`
/// 现拼出来的（见 `Bundle`）。后缀散在两处的话，"加一种新文件"时漏掉这儿就
/// 等于那种文件永远剪不掉，而磁盘上限还以为自己管着
/// （CLAUDE.md「同一件事别在两处各写一遍」）。
///
/// 剪枝**一次剪一个 id 的全部文件**：半份记录（有提示词没回复）比没有更误导人。
constexpr const char* kPromptSuffix = ".prompt.txt";
constexpr const char* kReplySuffix = ".reply.txt";
constexpr const char* kThinkingSuffix = ".thinking.txt";
constexpr const char* kErrorSuffix = ".error.txt";
constexpr const char* kToolsSuffix = ".tools.json";
const char* const kBodySuffixes[] = {kPromptSuffix, kReplySuffix, kThinkingSuffix,
                                     kErrorSuffix, kToolsSuffix};

// ------------------------------------------------------------ 密钥第二道门

/// 从 `i` 起是不是这个词（不分大小写）。
bool ci_at(const std::string& s, std::size_t i, const char* word) {
    const std::size_t n = std::strlen(word);
    if (i + n > s.size()) return false;
    for (std::size_t k = 0; k < n; ++k) {
        const int a = std::tolower(static_cast<unsigned char>(s[i + k]));
        const int b = std::tolower(static_cast<unsigned char>(word[k]));
        if (a != b) return false;
    }
    return true;
}

/// 密钥那一串里会出现的字符。
bool token_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.';
}

/// `"api_key": "……"` 这一族：认出键名之后把值换掉。
/// 回新的下标；认不出回 `npos`（调用方照原样抄这一个字节）。
std::size_t redact_json_secret(const std::string& s, std::size_t i, std::string& out) {
    if (s[i] != '"') return std::string::npos;
    std::size_t j = i + 1;
    while (j < s.size() && s[j] != '"') ++j;
    if (j >= s.size()) return std::string::npos;
    const std::string key = s.substr(i + 1, j - i - 1);
    bool hit = false;
    for (const char* k : {"authorization", "api_key", "apikey", "access_token",
                          "secret", "token"}) {
        if (key.size() == std::strlen(k) && ci_at(key, 0, k)) { hit = true; break; }
    }
    if (!hit) return std::string::npos;

    std::size_t p = j + 1;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size() || s[p] != ':') return std::string::npos;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size() || s[p] != '"') return std::string::npos;
    std::size_t q = p + 1;
    while (q < s.size() && s[q] != '"') ++q;
    if (q >= s.size()) return std::string::npos;

    out += s.substr(i, j - i + 1);   // 键名连两个引号原样留着，不然读的人不知道被抹的是什么
    out += ": \"***\"";
    return q + 1;
}

/// 把看着像密钥的那几种形状抹掉。
///
/// **这是第二道门，不是第一道。** 第一道是「整个 headers 都不记，只记
/// base_url」（见 call_log.hpp 上那三条），密钥本来就到不了这儿。
///
/// 还是要有第二道：这一条破了是**不可逆**的——日志会被拷给别人看、会贴进
/// 工单，而拷的人不会先逐份读一遍。而对面回的错误体里确实会带着密钥的回显
/// （`explain_status` 把回包截一段塞进那句错误话里，那句话又原样落进
/// error.txt 和索引的 `error` 栏）。
///
/// 认三种形状：`Bearer xxx`、`sk-` 打头的长串、以及 JSON 里 authorization /
/// api_key / access_token 这几个键的值。**只抹值，键名和别的字一个不动**
/// ——抹多了就看不出当时到底哪儿不对了。
std::string redact_secrets(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (ci_at(s, i, "bearer ")) {
            // **`Bearer` 这个词原样留着，而且后面那串要够长才抹。**
            //
            // 连词带值一起吞的话，散文里那句 "the bearer of this token"
            // 会变成 "the *** this token"——`of` 没了，而这段字可能正是对面
            // 回的错误说明，是读日志的人唯一的线索。
            //
            // 长度这一刀和 `sk-` 那一支同一个判据：真实的 bearer 值是几十上百
            // 个字符，而被误伤的那些（of / this / token）都是几个字母的普通词。
            // **代价说清楚**：短到 11 个字符以内的值抹不掉。那种东西不是真密钥
            // （第一道门"整个 headers 都不记"本来就让真密钥到不了这儿，这儿是
            // 第二道），而反过来把句子吃缺是每次都会发生的。
            std::size_t j = i + 7;
            while (j < s.size() && token_char(s[j])) ++j;
            if (j - (i + 7) >= 12) {
                out += s.substr(i, 7);
                out += "***";
                i = j;
                continue;
            }
        }
        // **`sk-` 必须在词首。** 不判边界的话任何含 `sk-` 的词都进这一支，
        // 后面再跟够长度就整段没了：对面回的错误体里那句 `disk-usage-exceeded`
        // 会变成 `di***`，而**那段字正是研究「什么提示词换来一个 400」时唯一的
        // 线索**。同族的还有 `risk-control`、`task-management`。
        if ((i == 0 || !token_char(s[i - 1])) && s.compare(i, 3, "sk-") == 0) {
            std::size_t j = i + 3;
            while (j < s.size() && token_char(s[j])) ++j;
            if (j - i >= 12) {   // 短的那些是正常单词（"sk-cn" 之类），别误伤
                out += "***";
                i = j;
                continue;
            }
        }
        if (s[i] == '"') {
            const std::size_t next = redact_json_secret(s, i, out);
            if (next != std::string::npos) {
                i = next;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// ---------------------------------------------------------------- id 和时间

/// 进程内自增序号，从 1 起。
///
/// **同一毫秒内多次调用靠它不撞。** 批量那几条是并发的（一次补七章分镜、
/// 一章几十镜的配音），时间戳精确到毫秒也照样会撞在一起。
///
/// ⚠️ **只管本进程。** 两个 changji 实例（开第二个、`--doctor`、以后 worker
/// 也叫模型）在同一毫秒各开自己的第一次调用，拿到的是同一个 id，于是
/// `write_body` 的 `std::ios::trunc` 把对方的 `prompt.txt` 悄悄盖掉，
/// `index.jsonl` 里两行同 id。掺一段 pid 进去能兜住，但 id 的形状写在
/// docs/提示词日志.md 上（「末尾那个是进程内自增的序号」），要改就得两边一起改。
std::atomic<std::uint64_t> g_seq{1};

std::tm local_tm(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    ::localtime_s(&out, &t);
#else
    ::localtime_r(&t, &out);
#endif
    return out;
}

std::tm utc_tm(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    ::gmtime_s(&out, &t);
#else
    ::gmtime_r(&t, &out);
#endif
    return out;
}

/// 本地时间比 UTC 快多少秒。
///
/// **不走 `mktime` 那条路**：`mktime` 会按 `tm_isdst` 再猜一次夏令时，而我们
/// 手上这两个 tm 是同一个时刻的两种写法，差多少直接减出来就行，不用猜。
/// 跨日的那一下用 `tm_yday` 补（年末那天 yday 会从 365 跳到 0，所以判的是
/// "差 1 天"和"差一整年"两种形状）。
long utc_offset_seconds(const std::tm& l, const std::tm& g) {
    long off = (l.tm_hour - g.tm_hour) * 3600 + (l.tm_min - g.tm_min) * 60 +
               (l.tm_sec - g.tm_sec);
    const int dday = l.tm_yday - g.tm_yday;
    if (dday == 1 || dday < -1) off += 86400;
    else if (dday == -1 || dday > 1) off -= 86400;
    return off;
}

struct Stamp {
    std::string id;   ///< 20260918-174233-518-0007
    std::string at;   ///< 2026-09-18T17:42:33.518+08:00
};

/// 把一个时刻格成 `2026-09-18T17:42:33.518+08:00`。
std::string format_at(std::chrono::system_clock::time_point now) {
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % std::chrono::seconds(1);
    const std::tm l = local_tm(t);
    const std::tm g = utc_tm(t);
    const long off = utc_offset_seconds(l, g);
    const long off_abs = off < 0 ? -off : off;

    char at[80] = {};
    std::snprintf(at, sizeof(at), "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02ld:%02ld",
                  l.tm_year + 1900, l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min,
                  l.tm_sec, static_cast<int>(ms.count()), off < 0 ? '-' : '+',
                  off_abs / 3600, (off_abs % 3600) / 60);
    return at;
}

/// 现在几点。**闸门那一行走这条，而不是借 `make_stamp`**：裁决没有自己的 id
/// （它借 `call_id` 认人），借 `make_stamp` 的话每记一次就白烧一个 `g_seq`，
/// index.jsonl 里的 id 于是跳号——跳号看着正像"这儿少记了一次调用"，研究时
/// 第一反应是怀疑日志漏了。
std::string now_at() { return format_at(std::chrono::system_clock::now()); }

Stamp make_stamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % std::chrono::seconds(1);
    const std::tm l = local_tm(t);

    char id[64] = {};
    std::snprintf(id, sizeof(id), "%04d%02d%02d-%02d%02d%02d-%03d-%04llu",
                  l.tm_year + 1900, l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min,
                  l.tm_sec, static_cast<int>(ms.count()),
                  static_cast<unsigned long long>(g_seq.fetch_add(1)));

    // id 和 at 是**同一个时刻**的两种写法，别改成各取一次 now()。
    return Stamp{id, format_at(now)};
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

// ------------------------------------------------------------ 剪枝那本账

/// 账上的键：**(id, 项目子目录名)**，两样缺一不可。
///
/// ⚠️ **只按 id 做键是不够的。** `g_seq` 每个进程从 1 起（见文件头那段），
/// 两个 changji 实例同一毫秒各开第一次调用，两个项目目录里就各有一份同名的
/// `<id>.*`。只按 id 记的话：folder 被后扫到的那个盖掉、字节数却是**两份相加**
/// ——剪到它时只删掉一个目录里的文件，`total` 按两份减，**另一个目录那几份
/// 从此不在账上、也不会再被扫到**（一趟进程只扫一次），磁盘上永久多出这一份
/// 且不收敛。
///
/// **id 在前、folder 在后**，不能反过来：`scan_once_locked` 靠这张表的遍历
/// 顺序还原「上一趟进程里谁先谁后」，而那个先后只有 id 里的时间戳知道。
/// folder 在前的话就变成了目录名的字典序，跨项目的先后全乱。
using Key = std::pair<std::string, std::string>;

/// 一次记录记下的东西。
///
/// **只记字节数，不记路径。** 那几条路径完全可重建：`root / folder / (id + 后缀)`，
/// 而两样都在键里。原来每个 id 存 3-5 条**绝对路径**，按默认上限 1 GB、
/// 一次调用几十 KB 算，稳态三万多条常驻，一条路径百来字节——光这本账就是
/// 二十几 MB 的常驻内存，而它只在超上限那一下才被读一次。
struct Bundle {
    std::uintmax_t bytes = 0;
    std::uint64_t seq = 0;    ///< 入账顺序，0 = 还没入过账。见 `Ledger::put`
};

/// 一个 root 底下的账。
///
/// **剪最旧的 = 剪最早落盘的那一次，不是最早开工的那一次。** id 的时间戳是
/// **构造**那一刻的，而落盘在**析构**：一次流式拆分镜跑了 166 秒（真实日志里
/// 那一行 `"ms":166842.4`），这 166 秒里批量那几条写完了几十条几秒钟的短记录。
/// 按 id 排的话，超上限时 `begin()` 正是这次长的自己——7 KB 提示词加 13 KB
/// 思考当场被删，那几十条短的全留着，**最贵、最想看的那一次第一个被扔**。
/// 所以排序的钥匙是入账那一下发的自增号 `seq`。
///
/// 两张表得一起动，所以进出都收在 `put` / `drop_oldest` 里
/// （CLAUDE.md「同一件事别在两处各写一遍」）。
struct Ledger {
    std::mutex mu;
    bool scanned = false;
    std::map<Key, Bundle> by_key;                  ///< (id, 项目目录) → 这一次
    std::map<std::uint64_t, Key> order;            ///< 入账顺序 → 那个键
    std::uint64_t next_seq = 0;
    std::uintmax_t total = 0;

    /// 入账。**同一个 id 记第二遍是换掉，不是加一遍。**
    ///
    /// 会记第二遍不是假想：正文文件在拿到这把锁**之前**就写完了，所以本进程
    /// 第一次落盘时 `scan_once_locked` 从磁盘上看见的正是自己刚写下的那几个
    /// 文件；多线程那头更早一步——B 写完文件还没拿到锁，A 拿到锁扫了一圈把 B
    /// 的文件按 B 的 id 记进账，B 随后再记一遍自己。`+=` 的后果是 `total`
    /// **永久虚高**一份：配了 1 GB 实际留不到 1 GB，而且偏差一路往上漂。
    /// 后到的那一次是自己数出来的字节数（扫描还可能扫到写了一半的），最准，
    /// 直接盖掉。
    void put(const std::string& id, const std::string& folder, std::uintmax_t bytes) {
        const Key k{id, folder};
        Bundle& b = by_key[k];
        if (b.seq != 0) order.erase(b.seq);
        total -= std::min(total, b.bytes);
        b.bytes = bytes;
        b.seq = ++next_seq;
        order.emplace(b.seq, k);
        total += bytes;
    }

    /// 把最早落盘的那一次从账上划走，连它那几个文件一起删。
    /// 回 false = 账已经空了，剪无可剪。
    bool drop_oldest(const fs::path& root) {
        const auto o = order.begin();
        if (o == order.end()) return false;
        const Key k = o->second;
        const auto it = by_key.find(k);
        if (it != by_key.end()) {
            const fs::path dir = root / paths::from_utf8(k.second);
            for (const char* suffix : kBodySuffixes) {
                std::error_code ec;
                // 删不掉也照样从账上划走，不然会死循环。
                fs::remove(dir / paths::from_utf8(k.first + suffix), ec);
            }
            total -= std::min(total, it->second.bytes);
            by_key.erase(it);
        }
        order.erase(o);
        return true;
    }
};

/// 每个 root 一本账。
///
/// **按 root 分开**是为了可测：用例把 root 指到各自的临时目录，一本全局的账
/// 会让它们互相影响（一条用例写的字节数把另一条的上限顶掉）。生产上只有一个
/// root，多这一层不花什么。
Ledger& ledger_for(const fs::path& root) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Ledger>> all;
    const std::string key = paths::to_utf8(root);
    std::lock_guard lg(mu);
    auto it = all.find(key);
    if (it == all.end()) it = all.emplace(key, std::make_unique<Ledger>()).first;
    return *it->second;
}

/// 文件名里 id 那一截：`20260918-174233-518-0007.prompt.txt` → 前面那一段。
/// 认不出来（不是这几个后缀）回空串，扫描时跳过。
std::string id_of_body_file(const std::string& name) {
    for (const char* suffix : kBodySuffixes) {
        const std::string s = suffix;
        if (name.size() > s.size() &&
            name.compare(name.size() - s.size(), s.size(), s) == 0) {
            return name.substr(0, name.size() - s.size());
        }
    }
    return {};
}

/// 第一次落盘时扫一遍 root 下所有项目子目录，把已经在盘上的正文文件记进账。
/// **一趟进程只扫这一次**（见文件头那段警告）。失败就当没扫成，下次再说。
void scan_once_locked(Ledger& led, const fs::path& root) {
    if (led.scanned) return;
    led.scanned = true;   // 扫不动也算扫过：反复扫一个坏目录没有意义
    // 先收成一张按 id 排好的表，扫完再一条条入账：上一趟进程留下的那些理应排
    // 在这一趟前面，而它们之间只有 id 能排先后（落盘顺序早就不在了）。
    // 目录迭代出来的顺序是文件系统给的，和时间没关系。
    // 键是 (id, 项目目录)：id 在前，遍历出来就是时间序。**两个项目里同 id 的
    // 那两份要各算各的**，合成一条的话其中一份会从账上消失（见 `Key` 那段）。
    std::map<Key, std::uintmax_t> found;
    std::error_code ec;
    for (fs::directory_iterator dir(root, ec), end; !ec && dir != end;
         dir.increment(ec)) {
        if (!dir->is_directory(ec)) continue;
        const std::string folder = paths::to_utf8(dir->path().filename());
        std::error_code ec2;
        for (fs::directory_iterator f(dir->path(), ec2), fend; !ec2 && f != fend;
             f.increment(ec2)) {
            if (!f->is_regular_file(ec2)) continue;
            const std::string name = paths::to_utf8(f->path().filename());
            const std::string id = id_of_body_file(name);
            if (id.empty()) continue;   // index.jsonl 和别的东西不算
            std::error_code ec3;
            const auto size = fs::file_size(f->path(), ec3);
            if (ec3) continue;
            found[Key{id, folder}] += size;
        }
    }
    for (const auto& [k, bytes] : found) led.put(k.first, k.second, bytes);
}

/// 超了就从**最早落盘**的那一次开始删，一次删一个 id 的全部文件。
/// **`index.jsonl` 不在这本账里**，所以永远不会被删到。
void prune_locked(Ledger& led, const fs::path& root, std::uintmax_t max_bytes) {
    while (led.total > max_bytes && led.drop_oldest(root)) {
    }
}

// ------------------------------------------------------------------ 写文件

/// 写一个正文文件。**原样写，不加 BOM、不加尾换行、不 trim。**
/// 成功时把字节数交出去（给剪枝那本账记）。
bool write_body(const fs::path& p, const std::string& body, std::uintmax_t* wrote) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    f.flush();
    if (!f.good()) return false;
    *wrote = body.size();
    return true;
}

/// 往 index.jsonl 追加一行。
///
/// **这一处要串起来**：四个出入口可能在不同线程上同时记（批量那几条本来就是
/// 并发的），两条线程各自 append 的话行会串在一起，而串了的那一行 jq 解不动
/// ——统计那头看到的是"少了几次调用"，没人会往日志自己身上想。
std::mutex& index_mutex() {
    static std::mutex mu;
    return mu;
}

/// 项目目录绝对路径的**最后一段**。取不到就 `_无项目`。
///
/// 中文和空格照原样建目录（`from_utf8` / `to_utf8` 见 util/paths.hpp 那两段
/// 注释：MSVC 上 `path.string()` 走 ANSI 代码页，中文路径当场抛异常）。
std::string folder_name(const std::string& project) {
    if (project.empty()) return kNoProject;
    fs::path p = paths::from_utf8(project);
    std::string name = paths::to_utf8(p.filename());
    if (name.empty()) name = paths::to_utf8(p.parent_path().filename());
    return name.empty() ? std::string(kNoProject) : name;
}

/// 这条线程上最近一次调用的 id。**取走就清掉**，理由写在 call_log.hpp 上
/// `take_last_call_id()` 那一段（线程是池子里轮着用的）。
thread_local std::string g_last_id;

/// 这条线程上那件活的三样。**没有活时是空串，不是缺键**——对拍、后台批处理、
/// `--doctor` 都没有活，而统计那头按键取，缺键会让整列变成 null。
struct Facts {
    std::string project;
    std::string episode_id;
    std::string task;
};

Facts read_facts() {
    Facts f;
    if (auto* act = pipeline::current_activity()) {
        if (const auto facts = pipeline::task_facts(act->task().id())) {
            f.project = facts->project;
            f.episode_id = facts->episode_id;
            f.task = facts->title;
        }
    }
    return f;
}

}  // namespace

// ---------------------------------------------------------------- 自由函数

std::string render_prompt(const nlohmann::ordered_json& messages) {
    if (!messages.is_array()) return {};

    auto content_of = [](const nlohmann::ordered_json& m) -> std::string {
        const auto c = m.find("content");
        if (c == m.end()) return {};
        // 带 tool_calls 的那条 content 可以是 null（OpenAI 那套），别 dump 成
        // 字面的 "null" 塞进正文里。
        if (c->is_string()) return c->get<std::string>();
        if (c->is_null()) return {};
        return c->dump();
    };
    auto calls_of = [](const nlohmann::ordered_json& m) {
        const auto tc = m.find("tool_calls");
        if (tc == m.end() || !tc->is_array()) return nlohmann::ordered_json::array();
        return *tc;
    };

    // **一条 user、没有 tool_calls 就原样出。** complete 那两条走的就是这一支，
    // 出来的正是模型收到的那段字——不多一个抬头，才能直接和另一次的 diff。
    if (messages.size() == 1 && messages[0].is_object() &&
        messages[0].value("role", std::string()) == "user" &&
        calls_of(messages[0]).empty()) {
        return content_of(messages[0]);
    }

    std::string out;
    for (const auto& m : messages) {
        if (!m.is_object()) continue;
        const std::string role = m.value("role", std::string());
        const auto calls = calls_of(m);

        std::string head = "=== " + role;
        if (!calls.empty()) {
            std::string names;
            for (const auto& c : calls) {
                if (!c.is_object()) continue;
                const auto fn = c.find("function");
                const std::string n =
                    fn != c.end() && fn->is_object() ? fn->value("name", std::string())
                                                     : c.value("name", std::string());
                if (!names.empty()) names += ", ";
                names += n;
            }
            head += " (tool_calls: " + names + ")";
        } else if (role == "tool") {
            const std::string cid = m.value("tool_call_id", std::string());
            if (!cid.empty()) head += " (" + cid + ")";
        }
        head += " ===";

        if (!out.empty()) out += "\n\n";
        out += head;
        const std::string body = content_of(m);
        if (!body.empty()) out += "\n" + body;
        // 工具参数是 JSON 文本，不是模型说的话——单独几行跟在 content 后面，
        // 混进正文会让"模型这一轮说了什么"不再准。
        for (const auto& c : calls) {
            if (!c.is_object()) continue;
            const auto fn = c.find("function");
            std::string n, args;
            if (fn != c.end() && fn->is_object()) {
                n = fn->value("name", std::string());
                const auto a = fn->find("arguments");
                if (a != fn->end()) args = a->is_string() ? a->get<std::string>() : a->dump();
            } else {
                n = c.value("name", std::string());
            }
            out += "\n→ " + n;
            if (!args.empty()) out += " " + args;
        }
    }
    return out;
}

nlohmann::json extract_usage(const std::string& raw_body) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(raw_body);
    } catch (const std::exception&) {
        return nullptr;   // 不是 JSON（命令行那条的 stdout、一段 HTML 报错页）
    }
    if (!body.is_object()) return nullptr;
    const auto u = body.find("usage");
    if (u == body.end() || !u->is_object()) return nullptr;

    nlohmann::json out = nlohmann::json::object();
    for (const char* key : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
        const auto v = u->find(key);
        // 收 `is_number` 而不是 `is_number_integer`：有的网关把 token 数回成
        // 9130.0，按整数判会整条丢掉——而落盘的还是整数，口径不变。
        if (v != u->end() && v->is_number()) out[key] = v->get<std::int64_t>();
    }
    return out.empty() ? nlohmann::json(nullptr) : out;
}

CallLogOptions call_log_options(const config::LLMConfig& cfg) {
    CallLogOptions o;
    o.enabled = cfg.call_log;
    // 研究用的后门：把一批日志单独收到一个目录里跑对比，不必去动设置页。
    const std::string over = paths::env("CHANGJI_LLM_LOG_DIR");
    if (!over.empty()) o.root = paths::from_utf8(over);
    // **转换之前先夹一刀。** double → uintmax_t 装不下时是 UB，实测常得 0，
    // 于是 `prune_locked` 把刚写下的正文文件当场删光，而开关明明是开的、
    // index.jsonl 还在长——症状和"日志坏了"完全对不上。设置页上手滑多按几个
    // 0（`call_log_max_mb = 1e30`）就能撞上。上限取 9e18：uintmax_t 装得下，
    // 而它比任何一块盘都大，等于"不限"。
    constexpr double kCap = 9.0e18;
    const double bytes = cfg.call_log_max_mb * 1024.0 * 1024.0;
    if (!(bytes > 0.0)) o.max_bytes = 0;            // 负数、0、NaN 都落这儿
    else if (bytes >= kCap) o.max_bytes = static_cast<std::uintmax_t>(kCap);
    else o.max_bytes = static_cast<std::uintmax_t>(bytes);
    return o;
}

// ------------------------------------------------------------------ CallLog

struct CallLog::Impl {
    CallLogOptions opt;
    std::string id;
    std::string at;

    std::string backend;
    std::string kind;
    std::string schema_name;
    std::string model;
    double temperature = 0.0;
    std::string reasoning_effort;
    std::string endpoint;

    std::string project;      ///< 项目目录的绝对路径
    std::string episode_id;
    std::string task;
    std::string stream_id;

    std::string prompt;
    std::string reply;
    std::string thinking;
    std::string tools_json;
    int tools_count = 0;
    bool has_tools = false;

    std::string raw_body;     ///< 最后一次拿到的原始回包，备 error.txt 用
    bool had_response = false;
    int http_status = 0;
    std::string finish_reason;
    nlohmann::json usage = nullptr;

    bool ok = true;
    std::string error;

    std::chrono::steady_clock::time_point t0{};
    std::chrono::steady_clock::time_point first_token{};
    bool has_first = false;

    fs::path root() const {
        if (!opt.root.empty()) return opt.root;
        return paths::user_data_dir("changji") / "llm_log";
    }

    /// 项目目录绝对路径的**最后一段**。取不到就 `_无项目`。
    /// **闸门那一行也走同一个函数**——两份 jsonl 落在同一个子目录里才对得上。
    std::string folder() const { return folder_name(project); }
};

CallLog::CallLog(std::string backend, std::string kind, const Request& req,
                 CallLogOptions opt)
    : impl_(std::make_unique<Impl>()) {
    impl_->opt = std::move(opt);
    impl_->backend = std::move(backend);
    impl_->kind = std::move(kind);
    impl_->schema_name = req.schema_name;
    impl_->t0 = std::chrono::steady_clock::now();
    if (!impl_->opt.enabled) return;   // 不记就整个不动手

    const Stamp s = make_stamp();
    impl_->id = s.id;
    impl_->at = s.at;

    // **上下文在构造时取，不在析构时取。** 两样都是线程局部的，而构造和析构
    // 都在调用线程上；取早一点的好处是：Activity 的 title 会被 set_message 改
    // （排队那一层还会盖一层 note），而研究要的是"这次调用是为哪件活发的"
    // ——开工那一刻那句最准。
    //
    // ⚠️ `current_activity()` 可能是 nullptr：对拍、后台批处理、--doctor 都
    // 没有活。那时候 project / task / episode_id 是**空串，不是缺键**。
    const Facts f = read_facts();
    impl_->project = f.project;
    impl_->episode_id = f.episode_id;
    impl_->task = f.task;
    impl_->stream_id = http::current_stream();

    // **留个 id 给闸门。** 闸门在 `complete()` 回来之后才判，那会儿这个记录器
    // 已经析构了；它靠这一个串把裁决对回这一次调用。盖掉上一次留下的是对的
    // ——同一条线程上，上一次的产出早就判完（或者那一步压根没有闸门）。
    g_last_id = impl_->id;
}

void CallLog::set_endpoint(std::string endpoint) { impl_->endpoint = std::move(endpoint); }

void CallLog::set_model(std::string model, double temperature,
                        std::string reasoning_effort) {
    impl_->model = std::move(model);
    impl_->temperature = temperature;
    impl_->reasoning_effort = std::move(reasoning_effort);
}

// ⚠️ **下面这几个开头都要有那句 `if (!opt.enabled) return;`。** 契约上写的是
// 「不记就整个不动手」，而出入口那头不看开关、照旧一样样交进来：关掉之后
// `set_prompt_from_payload` 照样跑一遍 `render_prompt` 把 7 KB 提示词复制一份、
// `append_thinking` 照样在**收流回调里**把每一小段往一个字符串上接（拆分镜
// 那一步实测思考十几万字，整份在内存里攒着、最后被析构直接扔掉）、
// `note_response` 照样多解一遍整份回包（一次拆分镜回包 400 KB 上下）。
// `set_reply` / `set_thinking` 只是一次 move，照样挡：它们攥着的是几千到十几万
// 字，关着的时候没必要在内存里留到析构。真正不值一句的是那几个短串
// （endpoint / model / finish_reason / fail 的那句错误话）。

void CallLog::set_prompt(std::string prompt) {
    if (!impl_->opt.enabled) return;
    impl_->prompt = std::move(prompt);
}

void CallLog::set_prompt_from_payload(const nlohmann::ordered_json& payload) {
    if (!impl_->opt.enabled) return;
    if (!payload.is_object()) return;
    const auto m = payload.find("messages");
    if (m == payload.end() || !m->is_array()) return;
    impl_->prompt = render_prompt(*m);
}

void CallLog::set_tools(const nlohmann::ordered_json& tools) {
    if (!impl_->opt.enabled) return;
    if (!tools.is_array() || tools.empty()) return;
    impl_->has_tools = true;
    impl_->tools_count = static_cast<int>(tools.size());
    try {
        impl_->tools_json = tools.dump(2);
    } catch (const std::exception&) {
        impl_->tools_json.clear();   // 非法 UTF-8 也不该让这一次白记
    }
}

void CallLog::note_response(int http_status, const std::string& body) {
    if (!impl_->opt.enabled) return;
    impl_->had_response = true;
    impl_->http_status = http_status;
    impl_->raw_body = body;
    const auto u = extract_usage(body);
    if (!u.is_null()) impl_->usage = u;
}

void CallLog::set_finish_reason(std::string reason) {
    impl_->finish_reason = std::move(reason);
}

void CallLog::set_reply(std::string reply) {
    if (!impl_->opt.enabled) return;
    impl_->reply = std::move(reply);
}

void CallLog::set_thinking(std::string thinking) {
    if (!impl_->opt.enabled) return;
    impl_->thinking = std::move(thinking);
}

void CallLog::append_thinking(const std::string& piece) {
    if (!impl_->opt.enabled) return;
    impl_->thinking += piece;
}

std::size_t CallLog::thinking_chars() const { return text::utf8_len(impl_->thinking); }

void CallLog::mark_first_token() {
    if (impl_->has_first) return;
    impl_->has_first = true;
    impl_->first_token = std::chrono::steady_clock::now();
}

void CallLog::fail(const std::string& message, int http_status) {
    impl_->ok = false;
    impl_->error = message;
    // 传 0 不盖掉已经记下的状态码：对面好好地回了 200、是我们这头按 schema
    // 校验没过的那一种，账上要同时看得见 http_status=200 和 ok=false。
    if (http_status != 0) impl_->http_status = http_status;
}

const std::string& CallLog::id() const { return impl_->id; }

CallLog::~CallLog() {
    // **绝不抛。** 记不下就算了——本来会成功的那一次，不许因为记日志而失败。
    try {
        if (!impl_->opt.enabled) return;

        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - impl_->t0).count();

        const std::string folder = impl_->folder();
        const fs::path dir = impl_->root() / paths::from_utf8(folder);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec && !fs::is_directory(dir, ec)) return;   // 建不了目录就算了

        // ---- 正文文件 ----
        std::uintmax_t written_bytes = 0;
        auto emit = [&](const char* suffix, const std::string& body) {
            if (body.empty()) return;   // 空文件没有研究价值，也白占一个 inode
            const fs::path p = dir / paths::from_utf8(impl_->id + suffix);
            std::uintmax_t n = 0;
            if (write_body(p, body, &n)) written_bytes += n;
        };
        emit(kPromptSuffix, impl_->prompt);
        emit(kReplySuffix, impl_->reply);
        emit(kThinkingSuffix, impl_->thinking);
        if (impl_->has_tools) emit(kToolsSuffix, impl_->tools_json);
        if (!impl_->ok) {
            // 第一段是**翻给用户的那句**：研究时要能一眼看出当时用户看到的是
            // 什么。第二段是原始回包，**不截断**——explain_status 里那个 200 字
            // 的截断是给用户看的，研究要看全的。
            //
            // **两段都过一遍 `redact_secrets`**：`explain_status` 会把回包截一段
            // 塞进那句错误话里，而有的网关在错误体里把请求头回显出来。
            std::string body = redact_secrets(impl_->error);
            body += "\n\n---- 对面原始回包（";
            if (impl_->had_response && impl_->http_status != 0) {
                body += "HTTP " + std::to_string(impl_->http_status) + "）----\n";
                body += redact_secrets(impl_->raw_body);
            } else if (impl_->had_response && !impl_->raw_body.empty()) {
                // 命令行那条没有状态码（恒 0），但 stdout/stderr 是有的，
                // 而失败时那句「Failed to authenticate…」正在里面。
                body += "无状态码）----\n";
                body += redact_secrets(impl_->raw_body);
            } else {
                body += "无）----\n";
            }
            emit(kErrorSuffix, body);
        }

        // ---- index.jsonl ----
        //
        // 键的顺序由 nlohmann::json（非 ordered）按字典序排。**不要为了好看换成
        // ordered_json**：统计那头按键取，顺序无意义，而字典序稳定——一行行
        // diff 两天的日志时，列不会因为代码里换了个赋值顺序就全部错位。
        //
        // 每一段字都过 sanitize_utf8：命令行后端的 stdout 可能是 GBK，而
        // nlohmann 的 dump() 遇到非法 UTF-8 会抛（type_error.316）——那会让这
        // 一整行没了，而它恰恰是"出问题的那一次"。
        const auto san = [](const std::string& s) { return text::sanitize_utf8(s); };
        nlohmann::json j;
        j["id"] = impl_->id;
        j["at"] = impl_->at;
        j["backend"] = impl_->backend;
        j["kind"] = impl_->kind;
        j["schema_name"] = san(impl_->schema_name);
        j["model"] = san(impl_->model);
        j["temperature"] = impl_->temperature;
        j["reasoning_effort"] = san(impl_->reasoning_effort);
        j["endpoint"] = san(redact_secrets(impl_->endpoint));
        j["project"] = san(impl_->project);
        j["episode_id"] = san(impl_->episode_id);
        j["task"] = san(impl_->task);
        j["stream_id"] = san(impl_->stream_id);
        // **三个 _chars 数的是 UTF-8 的「字」，不是字节。** 这份东西是给人读的，
        // 「这条提示词五千八百字」和「一万七千字节」里只有前一个能和 token 数、
        // 和 CLAUDE.md 里那张 schema 字数表对得上。
        j["prompt_chars"] = text::utf8_len(impl_->prompt);
        j["reply_chars"] = text::utf8_len(impl_->reply);
        j["thinking_chars"] = thinking_chars();
        j["ms"] = round1(ms);
        j["finish_reason"] = san(impl_->finish_reason);
        j["ok"] = impl_->ok;
        j["http_status"] = impl_->http_status;
        j["error"] = san(redact_secrets(impl_->error));
        if (impl_->has_first) {
            j["ms_to_first"] = round1(std::chrono::duration<double, std::milli>(
                                          impl_->first_token - impl_->t0).count());
        }
        if (!impl_->usage.is_null()) j["usage"] = impl_->usage;
        if (impl_->has_tools) j["tools_count"] = impl_->tools_count;

        {
            std::lock_guard lg(index_mutex());
            std::ofstream f(dir / "index.jsonl", std::ios::binary | std::ios::app);
            if (f) {
                f << j.dump() << "\n";
            }
        }

        // ---- 剪枝 ----
        if (written_bytes > 0) {
            const fs::path root = impl_->root();
            Ledger& led = ledger_for(root);
            std::lock_guard lg(led.mu);
            // 扫描可能刚把上面那几个文件（本进程写的）也算了一遍，`put` 换掉
            // 而不是加一遍——见 Ledger::put 上那段。
            scan_once_locked(led, root);
            led.put(impl_->id, folder, written_bytes);
            prune_locked(led, root, impl_->opt.max_bytes);
        }
    } catch (...) {
        // 磁盘满、没权限、路径转换炸了——都到这儿为止。
    }
}

// ------------------------------------------------------------------ 闸门

std::string take_last_call_id() {
    std::string id;
    id.swap(g_last_id);   // **取走就清掉**，理由在 call_log.hpp 上
    return id;
}

void note_gate(const GateVerdict& v, const CallLogOptions& opt) {
    // **绝不抛。** 和 `CallLog::~CallLog` 同一条规矩：本来会成功的那一章，
    // 不许因为记账而失败。这一条尤其要紧——`note_gate` 的调用点里有一半在
    // `catch` 里（打回的那一支），在那儿再抛一次就把真正的那句错误话盖掉了，
    // 界面上显示的会是"磁盘满"而不是"整章几乎没有对白"。
    try {
        if (!opt.enabled) return;
        if (v.call_id.empty()) return;   // 没有调用的裁决不记，见 call_log.hpp

        const Facts f = read_facts();
        const fs::path root =
            opt.root.empty() ? paths::user_data_dir("changji") / "llm_log"
                             : opt.root;
        const fs::path dir = root / paths::from_utf8(folder_name(f.project));
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec && !fs::is_directory(dir, ec)) return;   // 建不了目录就算了

        // 键的顺序和 index.jsonl 同一个道理：`nlohmann::json`（非 ordered）
        // 按字典序排，两天的日志 diff 起来列不会错位。
        // 每一段字都过 `sanitize_utf8`：打回那句话里带着模型吐出来的原文
        // （"有一段原样写了两遍（「…」）"），而模型偶尔会吐出半个字——
        // `dump()` 遇到非法 UTF-8 会抛，那会让**这一行**整个没了，
        // 而它恰恰是"被打回的那一次"。
        const auto san = [](const std::string& x) { return text::sanitize_utf8(x); };
        nlohmann::json j;
        j["at"] = now_at();
        j["call_id"] = san(v.call_id);
        j["stage"] = san(v.stage);
        j["subject"] = san(v.subject);
        j["gate"] = san(v.gate);
        j["reason"] = san(redact_secrets(v.reason));
        j["attempt"] = v.attempt;
        j["attempts"] = v.attempts;
        j["revision"] = v.revision;
        j["strict"] = v.strict;
        j["ok"] = v.ok;
        // 这三样 index.jsonl 里也有。**故意重一份**：按项目、按章看闸门是最
        // 常问的两件事，重了这三个短串就不必每次都先 join 一次。
        j["project"] = san(f.project);
        j["episode_id"] = san(f.episode_id);
        j["task"] = san(f.task);

        {
            // **和 index.jsonl 共用一把锁。** 批量那几条本来就是并发的，两条
            // 线程各自 append 的话行会串在一起，而串了的那一行 jq 解不动。
            // 两个文件共一把锁的代价是一次几十微秒的等待，换的是不必再养一把
            // 锁、也不会有人只给新文件加锁而漏掉顺序。
            std::lock_guard lg(index_mutex());
            std::ofstream out(dir / "gates.jsonl",
                              std::ios::binary | std::ios::app);
            if (out) out << j.dump() << "\n";
        }
    } catch (...) {
    }
}

}  // namespace changji::llm
