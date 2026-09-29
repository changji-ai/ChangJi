#include "telemetry/telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include "util/atomic_file.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::telemetry {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::system_clock;

namespace {

// ---- 白名单：和 server/src/appstats.js 那几张表对得上（改一边要改另一边） ----

/// 任务账本上的种类（util/task_slot.hpp 那一套）。
const std::set<std::string, std::less<>> kTasks{
    "outline", "premise", "analyze", "write_one", "revise", "story_web", "understand", "bible", "script",
    "plan",    "tts",     "video",   "trailer",   "llm",    "say",       "run",        "write", "image",
};
/// 按天的计数。
const std::set<std::string, std::less<>> kCounts{
    "project_new", "chapter_written", "shots_planned", "shots_rendered", "film_minutes", "voice_lines",
    "refs_drawn",  "oneclick",        "preview",       "film_joined",    "llm_calls",
};
const std::set<std::string, std::less<>> kEvents{"installed", "chapter_written", "shot_rendered", "film_joined"};
/// 在这几种活上 = 在出片（出图、出片、配音、试听、前几分钟、出片那一整批）。
const std::set<std::string, std::less<>> kRendering{"video", "image", "tts", "say", "trailer", "run"};

constexpr auto kOnlineRetry = std::chrono::seconds(60);
constexpr auto kMinBeatGap = std::chrono::seconds(30);
constexpr auto kFirstReport = std::chrono::minutes(5);
constexpr auto kReportEvery = std::chrono::hours(24);
constexpr auto kReportRetry = std::chrono::hours(1);
constexpr auto kSaveEvery = std::chrono::seconds(60);
constexpr int kKeepDays = 45;
constexpr std::size_t kMaxEvents = 100;

struct Tally {
    double ok = 0, fail = 0, cancel = 0, ms = 0;
};
struct DayBook {
    std::map<std::string, double> c;
    std::map<std::string, Tally> t;
    std::map<std::string, double> g;
};

struct State {
    std::mutex mu;
    /// 能发、也记账。钩子先看它（不上锁），关着就当场回去。
    std::atomic<bool> on{false};
    std::string shell = "engine";
    /// 开着的活（心跳里的「在干什么」）。**关着也记**：只在内存里，一个字节不出去；不记的话
    /// 半路打开统计，那几件正在跑的活结账时减成负数。
    std::map<std::string, int> running;
    std::map<std::string, DayBook> days;
    std::deque<json> events;
    bool dirty = false;
    bool loaded = false;

    Options opt;
    bool have_opt = false;
    std::string id;
    Clock::time_point next_beat{}, last_beat{}, next_report{}, last_save{};
    std::string last_state;
    std::int64_t last_report_s = 0;

    std::thread th;
    std::condition_variable cv;
    bool stopping = false;
};

/// 永不析构：钩子在进程退出那一刻（静态析构期间）也可能被叫到。
State& st() {
    static State* s = new State;
    return *s;
}

Clock::time_point now_locked(const State& s) {
    return s.have_opt && s.opt.io.now ? s.opt.io.now() : Clock::now();
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

double round1(double x) { return std::round(x * 10.0) / 10.0; }

std::string os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "x64";
#endif
}

fs::path id_file(const State& s) { return s.opt.dir / "install_id"; }
fs::path book_file(const State& s) { return s.opt.dir / "usage.json"; }

bool valid_id(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// 安装编号：`<数据目录>/install_id`。`create` 为假时只读不生（设置页看原文那一下不该生出一个编号）。
/// 新生出来的顺带排一条「新装上」。
std::string ensure_id(State& s, bool create) {
    if (valid_id(s.id)) return s.id;
    if (!s.have_opt || s.opt.dir.empty()) return {};
    std::ifstream in(id_file(s), std::ios::binary);
    std::string got;
    if (in) std::getline(in, got);
    while (!got.empty() && std::isspace(static_cast<unsigned char>(got.back()))) got.pop_back();
    if (valid_id(got)) return s.id = got;
    if (!create) return {};
    std::random_device rd;
    std::ostringstream o;
    for (int i = 0; i < 4; ++i) o << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(rd());
    const std::string id = o.str();
    try {
        std::error_code ec;
        fs::create_directories(s.opt.dir, ec);
        util::write_file_atomic(id_file(s), id + "\n", false);
    } catch (...) {
        return {};   // 写不下就这一拍不发，下一拍再试
    }
    s.id = id;
    s.events.push_back(json{{"kind", "installed"}});
    return id;
}

std::string version_to_send(const std::string& v) { return release_version(v) ? v : "v0.0.0-dev"; }

json identity(State& s, bool create_id) {
    const std::string ver = version_to_send(s.opt.version);
    return json{
        {"v", 1},
        {"id", ensure_id(s, create_id)},
        {"ver", ver},
        {"ch", ver.find('-') == std::string::npos ? "release" : "beta"},
        {"shell", s.shell},
        {"os", os_name()},
        {"arch", arch_name()},
        {"gpu", s.opt.gpu_build.empty() ? "cpu" : s.opt.gpu_build},
        {"lang", lang_code(i18n::spoken())},
    };
}

json day_json(const std::string& day, const DayBook& d) {
    json c = json::object();
    for (const auto& [k, v] : d.c)
        if (v > 0) c[k] = k == "film_minutes" ? round1(v) : v;
    json t = json::object();
    for (const auto& [k, v] : d.t) {
        json x = json::object();
        if (v.ok > 0) x["ok"] = v.ok;
        if (v.fail > 0) x["fail"] = v.fail;
        if (v.cancel > 0) x["cancel"] = v.cancel;
        if (v.ms > 0) x["ms"] = std::round(v.ms);
        if (!x.empty()) t[k] = x;
    }
    json g = json::object();
    for (const auto& [k, v] : d.g)
        if (v > 0) g[k] = v;
    return json{{"day", day}, {"c", c}, {"t", t}, {"g", g}};
}

/// `hw`、`setup` 在锁外问好了再交进来：问显卡在满负荷时能挂好几秒，锁着的话出片线程结账全跟着等。
json report_json(State& s, bool create_id, json hw, json setup) {
    json j = identity(s, create_id);
    j["hw"] = hw.is_object() ? std::move(hw) : json::object();
    j["setup"] = setup.is_object() ? std::move(setup) : json::object();
    json days = json::array();
    for (const auto& [day, d] : s.days) days.push_back(day_json(day, d));
    j["days"] = days;
    return j;
}

/// 按天的账落盘 / 读回来（`<数据目录>/usage.json`）：崩了、关机了，今天的数不丢。
void save(State& s) {
    if (!s.have_opt || s.opt.dir.empty()) return;
    json days = json::object();
    for (const auto& [day, d] : s.days) {
        const json x = day_json(day, d);
        days[day] = {{"c", x["c"]}, {"t", x["t"]}, {"g", x["g"]}};
    }
    try {
        std::error_code ec;
        fs::create_directories(s.opt.dir, ec);
        util::write_file_atomic(book_file(s), json{{"days", days}, {"last_report", s.last_report_s}}.dump(), false);
        s.dirty = false;
        s.last_save = now_locked(s);
    } catch (...) {
        /* 写不下就下一拍再写 */
    }
}

void load(State& s, Clock::time_point now) {
    s.loaded = true;
    try {
        std::ifstream in(book_file(s), std::ios::binary);
        if (in) {
            const json j = json::parse(in, nullptr, false);
            if (j.is_object()) {
                s.last_report_s = j.value("last_report", std::int64_t{0});
                const std::string oldest = day_of(now - std::chrono::hours(24 * kKeepDays));
                if (j.contains("days") && j["days"].is_object()) {
                    for (const auto& [day, x] : j["days"].items()) {
                        if (day < oldest || !x.is_object()) continue;
                        DayBook& d = s.days[day];
                        for (const auto& [k, v] : x.value("c", json::object()).items())
                            if (v.is_number() && kCounts.count(k)) d.c[k] += v.get<double>();
                        for (const auto& [k, v] : x.value("t", json::object()).items()) {
                            if (!kTasks.count(k) || !v.is_object()) continue;
                            Tally& t = d.t[k];
                            t.ok += v.value("ok", 0.0);
                            t.fail += v.value("fail", 0.0);
                            t.cancel += v.value("cancel", 0.0);
                            t.ms += v.value("ms", 0.0);
                        }
                        for (const auto& [k, v] : x.value("g", json::object()).items())
                            if (v.is_number()) d.g[k] += v.get<double>();
                    }
                }
            }
        }
    } catch (...) {
        /* 坏了就从头记 */
    }
    const auto last = Clock::time_point(std::chrono::seconds(s.last_report_s));
    s.next_report = std::max(now + kFirstReport, last + kReportEvery);
    s.next_beat = now;
}

DayBook& today(State& s) { return s.days[day_of(now_locked(s))]; }

bool gate_code_ok(std::string_view c) {
    if (c.empty() || c.size() > 32 || !(c[0] >= 'a' && c[0] <= 'z')) return false;
    return std::all_of(c.begin(), c.end(), [](char x) { return (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '_'; });
}

void push_event(State& s, std::string_view kind, double minutes) {
    json e{{"kind", std::string(kind)}};
    if (kind == "film_joined" && minutes >= 0) e["minutes"] = round1(minutes);
    s.events.push_back(std::move(e));
    while (s.events.size() > kMaxEvents) s.events.pop_front();
}

/// POST 一趟（不上锁的时候调）。回 {状态码, 回包}。
std::pair<int, std::string> send(const Io& io, const std::string& base, const std::string& path, const json& body) {
    if (!io.post || base.empty()) return {0, ""};
    try {
        return io.post(base + path, body.dump());
    } catch (...) {
        return {0, ""};
    }
}

}  // namespace

// ---- 纯函数 ----

bool release_version(std::string_view v) {
    // v<数>.<数>.<数>，后面可以挂 -<分支>
    if (v.size() < 6 || v[0] != 'v') return false;
    std::size_t i = 1;
    for (int part = 0; part < 3; ++part) {
        const std::size_t start = i;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
        if (i == start) return false;
        if (part < 2) {
            if (i >= v.size() || v[i] != '.') return false;
            ++i;
        }
    }
    return i == v.size() || v[i] == '-';
}

std::string to_string(Block b) {
    switch (b) {
        case Block::env: return "env";
        case Block::dnt: return "dnt";
        case Block::local_build: return "local_build";
        case Block::none: break;
    }
    return "";
}

Block blocked(const std::function<std::string(const char*)>& env, std::string_view version) {
    const auto get = [&](const char* k) { return env ? lower(env(k)) : std::string(); };
    const std::string off = get("CHANGJI_TELEMETRY");
    if (off == "0" || off == "off" || off == "false" || off == "no") return Block::env;
    const std::string dnt = get("DO_NOT_TRACK");
    if (!dnt.empty() && dnt != "0" && dnt != "false") return Block::dnt;
    if (!release_version(version) && get("CHANGJI_TELEMETRY_URL").empty()) return Block::local_build;
    return Block::none;
}

std::string host_class(std::string_view base_url) {
    std::string u = lower(base_url);
    if (const auto p = u.find("://"); p != std::string::npos) u = u.substr(p + 3);
    const std::string host = u.substr(0, u.find('/'));
    if (host.empty()) return "";
    // 按域名的整段比：`api.z.ai` 是智谱，`xyz.ai` 不是（原来按后缀比，差一点就认错了）
    const auto ends = [&](std::string_view tail) {
        std::string_view h = host;
        if (const auto c = h.rfind(':'); c != std::string_view::npos && h.find(']') == std::string_view::npos) h = h.substr(0, c);
        if (h == tail) return true;
        return h.size() > tail.size() && h.substr(h.size() - tail.size()) == tail && h[h.size() - tail.size() - 1] == '.';
    };
    // 端口整个比：`:12345` 不是 LM Studio 的 `:1234`
    const auto port = [&](std::string_view p) { return host.size() > p.size() && host.compare(host.size() - p.size(), p.size(), p) == 0; };
    if (ends("bigmodel.cn") || ends("z.ai")) return "zhipu";
    if (ends("openai.com")) return "openai";
    if (ends("deepseek.com")) return "deepseek";
    if (ends("dashscope.aliyuncs.com")) return "qwen";
    if (ends("moonshot.cn") || ends("moonshot.ai")) return "moonshot";
    if (ends("openrouter.ai")) return "openrouter";
    if (ends("anthropic.com")) return "anthropic";
    if (ends("googleapis.com")) return "google";
    if (ends("siliconflow.cn") || ends("siliconflow.com")) return "siliconflow";
    if (ends("volces.com") || ends("volcengineapi.com")) return "volcengine";
    if (ends("minimax.chat") || ends("minimaxi.com") || ends("minimax.io")) return "minimax";
    if (ends("changji.xyz")) return "changji";
    if (port(":11434")) return "ollama";
    if (port(":1234")) return "lmstudio";
    // 局域网、回环：只说「局域网」，不说是哪一台
    const std::string h = host.substr(0, host.rfind(':') == std::string::npos || host[0] == '[' ? host.size() : host.rfind(':'));
    if (h == "localhost" || h.rfind("127.", 0) == 0 || h.rfind("10.", 0) == 0 || h.rfind("192.168.", 0) == 0 ||
        h.rfind("[::1]", 0) == 0 || (h.size() > 6 && h.substr(h.size() - 6) == ".local")) {
        return "lan";
    }
    if (h.rfind("172.", 0) == 0) {
        const int second = std::atoi(h.c_str() + 4);
        if (second >= 16 && second <= 31) return "lan";
    }
    return "other";
}

std::string gpu_vendor_of(std::string_view gpu_name) {
    const std::string n = lower(gpu_name);
    if (n.empty()) return "none";
    if (contains(n, "nvidia") || contains(n, "geforce") || contains(n, "quadro") || contains(n, "tesla") || contains(n, "rtx")) return "nvidia";
    if (contains(n, "amd") || contains(n, "radeon")) return "amd";
    if (contains(n, "intel") || contains(n, "arc ")) return "intel";
    if (contains(n, "apple") || n.rfind("m1", 0) == 0 || n.rfind("m2", 0) == 0 || n.rfind("m3", 0) == 0 || n.rfind("m4", 0) == 0) return "apple";
    return "other";
}

std::string clean_gpu_name(std::string_view gpu_name) {
    std::string s(gpu_name);
    // 括号里的附注（「（统一内存）」「(unified memory)」）不要：那是本地化的字，服务器那头也认不出
    for (const std::string_view open : {std::string_view("\xEF\xBC\x88"), std::string_view("(")}) {
        if (const auto p = s.find(open); p != std::string::npos) s = s.substr(0, p);
    }
    std::string out;
    for (unsigned char c : s) {
        const bool ok = std::isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-' || c == '+' || c == '/' || c == '@' || c == ',';
        out.push_back(ok && c < 0x80 ? static_cast<char>(c) : ' ');
    }
    // 空白收成一个、去头尾
    std::string squeezed;
    for (char c : out) {
        if (c == ' ' && (squeezed.empty() || squeezed.back() == ' ')) continue;
        squeezed.push_back(c);
    }
    while (!squeezed.empty() && squeezed.back() == ' ') squeezed.pop_back();
    return squeezed.substr(0, 80);
}

std::string lang_code(std::string_view spoken) {
    std::string s(spoken);
    if (const auto dot = s.find('.'); dot != std::string::npos) s = s.substr(0, dot);   // zh_CN.UTF-8
    std::replace(s.begin(), s.end(), '-', '_');
    if (s.empty() || s == "zh_CN" || s == "zh_Hans" || s == "zh") return "zh";
    if (s == "zh_TW" || s == "zh_Hant" || s == "zh_HK") return "zh_TW";
    if (s == "pt_BR") return "pt_BR";
    const std::string lang = lower(s.substr(0, s.find('_')));
    if (lang.size() < 2 || lang.size() > 3 || !std::all_of(lang.begin(), lang.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return "";
    return lang;
}

std::string model_name(std::string_view path) {
    std::string_view s = path;
    if (const auto p = s.find_last_of("/\\"); p != std::string_view::npos) s = s.substr(p + 1);
    if (s.empty() || s.size() > 100) return "";
    const bool ok = std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
    });
    return ok ? std::string(s) : "";
}

std::string state_of(const std::map<std::string, int>& running) {
    bool writing = false;
    for (const auto& [k, n] : running) {
        if (n <= 0) continue;
        if (kRendering.count(k)) return "rendering";
        writing = true;
    }
    return writing ? "writing" : "idle";
}

std::string day_of(Clock::time_point t) {
    const std::time_t tt = Clock::to_time_t(t);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

// ---- 钩子 ----

void task_begun(std::string_view kind) {
    if (!kTasks.count(kind)) return;
    State& s = st();
    std::lock_guard lg(s.mu);
    s.running[std::string(kind)] += 1;
}

void task_ended(std::string_view kind, std::string_view slot, bool begun, End end, double ms) {
    if (!kTasks.count(kind)) return;
    State& s = st();
    std::lock_guard lg(s.mu);
    if (begun) {
        auto it = s.running.find(std::string(kind));
        if (it != s.running.end() && --it->second <= 0) s.running.erase(it);
    }
    if (!s.on) return;
    DayBook& d = today(s);
    Tally& t = d.t[std::string(kind)];
    if (end == End::ok) {
        t.ok += 1;
        t.ms += std::max(0.0, ms);
    } else if (end == End::fail) {
        t.fail += 1;
    } else {
        t.cancel += 1;
    }
    s.dirty = true;
    if (end != End::ok) return;
    // 几种活做成了，顺带记一笔计数、出一件事（地图上闪那一下）
    if (kind == "write_one" || kind == "story_web") {
        d.c["chapter_written"] += 1;
        push_event(s, "chapter_written", -1);
    } else if (kind == "video") {
        d.c["shots_rendered"] += 1;
        push_event(s, "shot_rendered", -1);
    } else if (kind == "tts") {
        d.c["voice_lines"] += 1;
    } else if (kind == "image" && slot == "assets") {
        d.c["refs_drawn"] += 1;
    } else if (kind == "trailer") {
        d.c["preview"] += 1;
    }
}

void count(std::string_view key, double n) {
    State& s = st();
    if (!s.on || !kCounts.count(key) || !(n > 0)) return;
    std::lock_guard lg(s.mu);
    today(s).c[std::string(key)] += n;
    s.dirty = true;
}

void gate(std::string_view code) {
    State& s = st();
    if (!s.on || !gate_code_ok(code)) return;
    std::lock_guard lg(s.mu);
    DayBook& d = today(s);
    if (d.g.size() >= 40 && !d.g.count(std::string(code))) return;   // 服务器那头一天最多收 40 种
    d.g[std::string(code)] += 1;
    s.dirty = true;
}

void event(std::string_view kind, double minutes) {
    State& s = st();
    if (!s.on || !kEvents.count(kind)) return;
    std::lock_guard lg(s.mu);
    push_event(s, kind, minutes);
}

void set_shell(std::string shell) {
    State& s = st();
    std::lock_guard lg(s.mu);
    s.shell = shell == "desktop" ? "desktop" : "engine";
}

// ---- 发 ----

void tick_once() {
    State& s = st();
    Io io;
    std::string base;
    json beat, report;
    std::vector<json> evs;
    bool want_beat = false, want_report = false;
    Clock::time_point now;
    std::string state;
    {
        std::lock_guard lg(s.mu);
        if (!s.have_opt) return;
        io = s.opt.io;
        now = io.now ? io.now() : Clock::now();
        const bool conf = io.configured ? io.configured() : true;
        const bool on = conf && blocked(io.env, s.opt.version) == Block::none;
        s.on = on;
        if (!on) return;
        if (!s.loaded) load(s, now);
        base = io.endpoint ? io.endpoint() : std::string();
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (ensure_id(s, true).empty()) return;
        state = state_of(s.running);
        want_beat = now >= s.next_beat || (state != s.last_state && now - s.last_beat >= kMinBeatGap);
        if (want_beat) {
            beat = identity(s, true);
            beat["state"] = state;
        }
        for (int i = 0; i < 20 && !s.events.empty(); ++i) {
            json e = identity(s, true);
            e.update(s.events.front());
            evs.push_back(std::move(e));
            s.events.pop_front();
        }
        want_report = now >= s.next_report;
    }
    // 下面几趟网络都不上锁：钩子照样能记账

    if (want_beat) {
        const auto [code, body] = send(io, base, "/t/v1/beat", beat);
        std::lock_guard lg(s.mu);
        s.last_beat = now;
        if (code == 200) {
            s.last_state = state;
            int beat_s = 300;
            const json r = json::parse(body, nullptr, false);
            if (r.is_object() && r.contains("beat_s") && r["beat_s"].is_number()) beat_s = r["beat_s"].get<int>();
            s.next_beat = now + std::chrono::seconds(std::clamp(beat_s, 60, 3600));
        } else {
            s.next_beat = now + kOnlineRetry;
        }
    }

    for (std::size_t i = 0; i < evs.size(); ++i) {
        const auto [code, body] = send(io, base, "/t/v1/event", evs[i]);
        (void)body;
        if (code == 200 || code == 400) continue;   // 400 = 这一条服务器不认，重发也没用
        // 没发出去的放回队头，下一拍再发
        std::lock_guard lg(s.mu);
        for (std::size_t j = evs.size(); j-- > i;) {
            json back = json{{"kind", evs[j]["kind"]}};
            if (evs[j].contains("minutes")) back["minutes"] = evs[j]["minutes"];
            s.events.push_front(std::move(back));
        }
        while (s.events.size() > kMaxEvents) s.events.pop_back();
        break;
    }

    if (want_report) {
        const json hw = io.hw ? io.hw() : json::object();
        const json setup = io.setup ? io.setup() : json::object();
        json body;
        {
            std::lock_guard lg(s.mu);
            body = report_json(s, true, hw, setup);
        }
        const auto [code, reply] = send(io, base, "/t/v1/report", body);
        (void)reply;
        std::lock_guard lg(s.mu);
        if (code == 200) {
            // 发过的那几天定下来了：今天之前的删掉（今天的接着记，明天连同今天的累计再发一遍）
            const std::string d = day_of(now);
            for (auto it = s.days.begin(); it != s.days.end();) it = it->first < d ? s.days.erase(it) : std::next(it);
            s.last_report_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            s.next_report = now + kReportEvery;
            s.dirty = true;
        } else {
            s.next_report = now + kReportRetry;
        }
    }

    std::lock_guard lg(s.mu);
    // 太老的天扔掉（离线攒着的，最多留 45 天）
    const std::string oldest = day_of(now - std::chrono::hours(24 * kKeepDays));
    for (auto it = s.days.begin(); it != s.days.end();) it = it->first < oldest ? s.days.erase(it) : std::next(it);
    if (s.dirty && now - s.last_save >= kSaveEvery) save(s);
}

void stop() {
    State& s = st();
    std::thread th;
    {
        std::lock_guard lg(s.mu);
        s.stopping = true;
        th = std::move(s.th);
    }
    s.cv.notify_all();
    if (th.joinable()) th.join();
    std::lock_guard lg(s.mu);
    if (s.dirty && s.on) save(s);
}

void start(Options opt) {
    stop();
    State& s = st();
    {
        std::lock_guard lg(s.mu);
        s.opt = std::move(opt);
        s.have_opt = true;
        s.stopping = false;
        s.loaded = false;
        s.on = (s.opt.io.configured ? s.opt.io.configured() : true) && blocked(s.opt.io.env, s.opt.version) == Block::none;
        if (s.opt.tick.count() <= 0) return;   // 用例：不起线程，自己一拍一拍调 tick_once
        s.th = std::thread([&s] {
            for (;;) {
                {
                    std::unique_lock lk(s.mu);
                    s.cv.wait_for(lk, s.opt.tick, [&s] { return s.stopping; });
                    if (s.stopping) return;
                }
                try {
                    tick_once();
                } catch (...) {
                    /* 统计出了问题不许把引擎带倒 */
                }
            }
        });
    }
}

bool enabled() { return st().on; }

json status() {
    State& s = st();
    Io io;
    {
        std::lock_guard lg(s.mu);
        if (s.have_opt) io = s.opt.io;
    }
    const json hw = io.hw ? io.hw() : json::object();
    const json setup = io.setup ? io.setup() : json::object();
    std::lock_guard lg(s.mu);
    const bool conf = s.have_opt && s.opt.io.configured ? s.opt.io.configured() : true;
    const Block b = s.have_opt ? blocked(s.opt.io.env, s.opt.version) : Block::local_build;
    std::string base = s.have_opt && s.opt.io.endpoint ? s.opt.io.endpoint() : std::string();
    while (!base.empty() && base.back() == '/') base.pop_back();
    json beat, ev, report;
    if (s.have_opt) {
        beat = identity(s, false);
        beat["state"] = state_of(s.running);
        ev = identity(s, false);
        ev["kind"] = "shot_rendered";
        report = report_json(s, false, hw, setup);
    }
    return json{
        {"enabled", conf && b == Block::none},
        {"configured", conf},
        {"blocked", to_string(b)},
        {"endpoint", base},
        {"preview", {{"beat", beat}, {"event", ev}, {"report", report}}},
    };
}

void reset_for_tests() {
    stop();
    State& s = st();
    std::lock_guard lg(s.mu);
    s.on = false;
    s.shell = "engine";
    s.running.clear();
    s.days.clear();
    s.events.clear();
    s.dirty = false;
    s.loaded = false;
    s.opt = Options{};
    s.have_opt = false;
    s.id.clear();
    s.next_beat = s.last_beat = s.next_report = s.last_save = Clock::time_point{};
    s.last_state.clear();
    s.last_report_s = 0;
    s.stopping = false;
}

}  // namespace changji::telemetry
