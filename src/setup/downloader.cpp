#include "setup/downloader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#include "config/model_index.hpp"
#include "setup/catalog.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

namespace changji::setup {

namespace {

/// 盯着下载器的间隔。**别调到 200 毫秒以下**：每次都要 stat 一个文件、
/// 读一段日志，而这条循环在整个下载期间一直转。
constexpr int kPollMs = 400;

/// 一个文件最多重来几次。网络抖一下是常态，每次都从断点续，
/// 所以重试很便宜；真正下不动的（地址没了、盘满了）三次也救不回来。
constexpr int kMaxAttempts = 3;


std::uint64_t file_size_or_zero(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

/// 读文件末尾的一段。日志会长到几 MB，每次全读一遍太浪费。
std::string tail_of(const fs::path& p, std::size_t bytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end <= 0) return {};
    const auto want = static_cast<std::streamoff>(bytes);
    const std::streamoff from = end > want ? end - want : std::streamoff(0);
    in.seekg(from, std::ios::beg);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return out;
}

/// 日志里最后几行，报错时带给用户。
///
/// **必须带上。** 只说"下载失败"的话，用户看不出是地址 404、盘满了、
/// 还是证书过期——这三种的下一步动作完全不同。
std::string last_lines(const std::string& text, int count) {
    std::string flat = text;
    for (char& c : flat) {
        if (c == '\r') c = '\n';
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= flat.size()) {
        const auto nl = flat.find('\n', start);
        const auto end = nl == std::string::npos ? flat.size() : nl;
        std::string line = flat.substr(start, end - start);
        // 去掉两头的空白和 aria2 画的那些分隔线
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        if (!line.empty() && line.find_first_not_of("=-*") != std::string::npos) {
            lines.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    std::string out;
    const std::size_t from =
        lines.size() > static_cast<std::size_t>(count) ? lines.size() - count : 0;
    for (std::size_t i = from; i < lines.size(); ++i) {
        if (!out.empty()) out += "\n";
        out += lines[i];
    }
    return out;
}

}  // namespace

/// 同时下几个。
///
/// **原来是一个一个下的**，理由写在 downloader.hpp 上：瓶颈是带宽不是并发。
/// 那句话对一半——对的是"带宽跑满之后并发没用"，不对的是**单连接常常
/// 跑不满带宽**：hf-mirror 一条连接 1～2 MB/s，而这台机器的出口远不止。
/// 用户 2026-09-12：「将模型改成同时下载而不是一个一个下」。
///
/// 分两档，因为两个下载器的并发是两回事：
///   curl    单连接。并发几个就是快几倍，这一档给 4。
///   aria2c  自己已经开了 8 条连接（`-x 8`），再乘上去就是 32 条——
///           源站那边多半开始限速甚至掐连接，所以只给 2。
///
/// **代价要说清楚**：同时下的时候，「现在在下什么」不再是一个文件，
/// 单个文件的进度也不再是单调推进的（谁快谁慢看源站脸色）。界面上
/// 每一项各自有自己的进度条，总进度仍然是所有项的和。
///
/// 压回一个一个下：`CHANGJI_DOWNLOAD_PARALLEL=1`。上限 8——再多就只是
/// 给源站添乱，而且断点续传的文件句柄也多得没必要。
std::size_t parallel_lanes(std::size_t item_count, const std::string& tool) {
    std::size_t lanes = tool == "aria2c" ? 2 : 4;
    const std::string env = paths::env("CHANGJI_DOWNLOAD_PARALLEL");
    if (!env.empty()) {
        try {
            const long v = std::stol(env);
            if (v >= 1) lanes = static_cast<std::size_t>(std::min<long>(v, 8));
        } catch (const std::exception&) {
            // 写错了就按默认来。这地方不值得让整轮下载停下。
        }
    }
    return std::max<std::size_t>(1, std::min(lanes, item_count));
}

const char* to_string(ItemState v) {
    switch (v) {
        case ItemState::Pending: return "pending";
        case ItemState::Running: return "running";
        case ItemState::Done: return "done";
        case ItemState::Present: return "present";
        case ItemState::Failed: return "failed";
        case ItemState::Canceled: return "canceled";
    }
    return "pending";
}

const char* to_string(RunState v) {
    switch (v) {
        case RunState::Idle: return "idle";
        case RunState::Running: return "running";
        case RunState::Done: return "done";
        case RunState::Failed: return "failed";
        case RunState::Canceled: return "canceled";
    }
    return "idle";
}

std::optional<std::uint64_t> parse_human_bytes(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const std::size_t num_start = i;
    while (i < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[i])) != 0 || text[i] == '.')) {
        ++i;
    }
    if (i == num_start) return std::nullopt;
    double value = 0.0;
    try {
        value = std::stod(text.substr(num_start, i - num_start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    std::string unit;
    while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
        unit += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        ++i;
    }
    // aria2 用的是 1024 进制（KiB/MiB/GiB）。裸数字就是字节。
    double mul = 1.0;
    if (unit == "kib" || unit == "k" || unit == "kb") mul = 1024.0;
    else if (unit == "mib" || unit == "m" || unit == "mb") mul = 1024.0 * 1024;
    else if (unit == "gib" || unit == "g" || unit == "gb") mul = 1024.0 * 1024 * 1024;
    else if (unit == "tib" || unit == "t") mul = 1024.0 * 1024 * 1024 * 1024;
    else if (!unit.empty() && unit != "b") return std::nullopt;
    if (value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(value * mul);
}

std::optional<Aria2Progress> parse_aria2_progress(const std::string& text) {
    // 最后一条 `[#xxxxxx 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]`。
    // 取最后一条而不是第一条：日志是追加的，前面那些是几秒钟以前的。
    const auto open = text.rfind("[#");
    if (open == std::string::npos) return std::nullopt;
    const auto close = text.find(']', open);
    if (close == std::string::npos) return std::nullopt;
    const std::string line = text.substr(open, close - open + 1);

    // 空格分段。gid 那一段跳过。
    const auto slash = line.find('/');
    if (slash == std::string::npos) return std::nullopt;
    const auto space_before = line.rfind(' ', slash);
    if (space_before == std::string::npos) return std::nullopt;
    const auto paren = line.find('(', slash);

    Aria2Progress p;
    const auto got = parse_human_bytes(line.substr(space_before + 1, slash - space_before - 1));
    if (!got.has_value()) return std::nullopt;
    p.downloaded = *got;
    if (paren != std::string::npos) {
        const auto total = parse_human_bytes(line.substr(slash + 1, paren - slash - 1));
        if (total.has_value()) p.total = *total;
    }
    const auto dl = line.find("DL:");
    if (dl != std::string::npos) {
        const auto end = line.find(' ', dl);
        const auto len = end == std::string::npos ? std::string::npos : end - dl - 3;
        const auto speed = parse_human_bytes(line.substr(dl + 3, len));
        if (speed.has_value()) p.speed_bps = static_cast<double>(*speed);
    }
    return p;
}

std::string pick_tool() {
    // aria2c 排前面：这些镜像单连接常常只有 1～2 MB/s，八连接能到
    // 10 MB/s 以上，43 GB 那一档差的是五个小时。
    if (proc::which("aria2c").has_value()) return "aria2c";
    if (proc::which("curl").has_value()) return "curl";
    return {};
}

json Snapshot::to_json() const {
    json items_json = json::array();
    for (const auto& it : items) {
        items_json.push_back({{"group", it.group},
                              {"option", it.option},
                              {"name", it.name},
                              {"path", it.path},
                              {"note", it.note},
                              {"total", it.total},
                              {"downloaded", it.downloaded},
                              {"speedBps", it.speed_bps},
                              {"state", to_string(it.state)},
                              {"error", it.error}});
    }
    return {{"state", to_string(state)},
            {"items", items_json},
            {"error", error},
            {"tool", tool},
            {"source", source},
            {"dir", dir},
            {"total", total},
            {"downloaded", downloaded},
            {"speedBps", speed_bps},
            {"etaSeconds", eta_seconds}};
}

std::size_t adopt_finished_parts(const fs::path& models_dir) {
    std::size_t n = 0;
    for (const auto& g : catalog()) {
        for (const auto& o : g.options) {
            // 替换档（小一档的编码器、VAE）也要看：人挑了它、下到一半被杀，
            // 盘上躺着的正是那一份。
            std::vector<FileSpec> files = o.files;
            for (const auto& alt : o.alts) {
                files.insert(files.end(), alt.choices.begin(), alt.choices.end());
            }
            for (const auto& f : files) {
                if (f.bytes == 0) continue;
                // 老位置和新位置都看：这一版之前下的平铺在根上，之后的进了类型目录。
                const std::string where[] = {f.name, download_rel(g.key, f)};
                for (const auto& rel : where) {
                    const fs::path dest = models_dir / paths::from_utf8(rel);
                    const fs::path part =
                        dest.parent_path() / (paths::to_utf8(dest.filename()) + ".part");
                    if (file_size_or_zero(part) != f.bytes) continue;
                    if (file_size_or_zero(dest) == f.bytes) {
                        // 正式的那份也全：`.part` 是多余的，扔掉
                        std::error_code rm;
                        fs::remove(part, rm);
                        continue;
                    }
                    std::error_code mv;
                    fs::rename(part, dest, mv);
                    if (!mv) ++n;
                }
            }
        }
    }
    if (n > 0) config::forget_model_index();
    return n;
}

Downloader& Downloader::instance() {
    static Downloader d;
    return d;
}

Downloader::~Downloader() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Downloader::running() const { return running_.load(); }

Snapshot Downloader::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
}

bool Downloader::start(std::vector<Item> items, const fs::path& dir,
                       const std::string& source,
                       std::function<void(const Item&)> on_item_done) {
    if (running_.exchange(true)) return false;
    // 上一轮的线程还挂着就先收掉。**不能不 join**：worker_ 被赋值时
    // 如果还是 joinable 的，std::thread 的赋值运算符会直接 terminate。
    if (worker_.joinable()) worker_.join();
    cancel_ = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = Snapshot{};
        snap_.state = RunState::Running;
        snap_.tool = pick_tool();
        snap_.source = source;
        snap_.dir = paths::to_utf8(dir);
        for (const auto& it : items) {
            ItemProgress p;
            p.group = it.group;
            p.option = it.option;
            p.name = it.file.name;
            p.path = it.rel.empty() ? it.file.name : it.rel;
            p.note = it.file.note;
            p.total = it.file.bytes;
            snap_.items.push_back(std::move(p));
            snap_.total += it.file.bytes;
        }
    }

    worker_ = std::thread(&Downloader::run, this, std::move(items), dir,
                          std::move(on_item_done));
    return true;
}

void Downloader::cancel() { cancel_ = true; }

void Downloader::run(std::vector<Item> items, fs::path dir,
                     std::function<void(const Item&)> on_item_done) {
    std::string fatal;
    bool canceled = false;

    if (pick_tool().empty()) {
        // **说清楚装什么，而不是"没有下载器"。** 这台机器上一个都没有是
        // 罕见情况，碰上的人多半也不知道该装哪个。
        fatal =
            SAY("这台机器上没找到下载器。装一个就行：\n"
                "  Windows：winget install aria2.aria2（或者用系统自带的 curl，"
                "重开一个终端让 PATH 生效）\n"
                "  Debian/Ubuntu：apt-get install -y aria2\n"
                "  macOS：brew install aria2");
    } else {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            fatal = SAYF("建不了模型目录 %1：%2", paths::to_utf8(dir),
                         ec.message());
        }
    }

    if (fatal.empty()) {
        // **几路一起下。** 几路怎么定的见 parallel_lanes。
        //
        // 派活走一个原子下标，不是按线程切片：文件大小差着两个数量级
        // （605 MB 到 66 GB），切片会让某一路早早空转，而另一路还在啃
        // 那个最大的。谁先空出来谁拿下一个。
        const std::size_t lanes = parallel_lanes(items.size(), pick_tool());
        std::atomic<std::size_t> next{0};
        std::atomic<bool> any_canceled{false};
        // **on_item_done 要串起来。** 它做的是把这一项写回用户配置
        // （逐行编辑同一个 TOML），两路同时写会把文件写花——而那种坏
        // 表现是"下完之后配置里少了一项"，谁也想不到是并发写的。
        std::mutex done_mu;

        const auto lane = [&] {
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= items.size()) return;
                if (cancel_) { any_canceled = true; return; }
                const fs::path dest = dir / paths::from_utf8(
                    items[i].rel.empty() ? items[i].file.name : items[i].rel);
                if (!fetch_one(items[i], dir, dest, i)) {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (snap_.items[i].state == ItemState::Canceled) {
                        any_canceled = true;
                    }
                    continue;  // 一个失败不拦住别的：能下多少是多少
                }
                if (on_item_done) {
                    std::lock_guard<std::mutex> lock(done_mu);
                    on_item_done(items[i]);
                }
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(lanes - 1);
        for (std::size_t t = 1; t < lanes; ++t) pool.emplace_back(lane);
        lane();   // 这个线程自己也算一路，别光站着看
        for (auto& th : pool) th.join();
        canceled = any_canceled;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!fatal.empty()) {
        snap_.state = RunState::Failed;
        snap_.error = fatal;
    } else if (canceled || cancel_) {
        snap_.state = RunState::Canceled;
        for (auto& it : snap_.items) {
            if (it.state == ItemState::Pending || it.state == ItemState::Running) {
                it.state = ItemState::Canceled;
            }
        }
    } else {
        const bool any_failed =
            std::any_of(snap_.items.begin(), snap_.items.end(),
                        [](const ItemProgress& p) { return p.state == ItemState::Failed; });
        snap_.state = any_failed ? RunState::Failed : RunState::Done;
        if (any_failed) {
            snap_.error = SAY("有文件没下下来，看下面每一项的说明。"
                              "重来一次会从断点接着下。");
        }
    }
    snap_.speed_bps = 0.0;
    snap_.eta_seconds = -1.0;
    running_ = false;
}

bool Downloader::fetch_one(const Item& item, const fs::path& dir, const fs::path& dest,
                           std::size_t index) {
    const auto set = [&](auto&& fn) {
        std::lock_guard<std::mutex> lock(mu_);
        fn(snap_.items[index]);
        // 总量每次跟着重算。分开算的话会出现"明细都下完了、总数还差一截"。
        std::uint64_t sum = 0;
        for (const auto& it : snap_.items) sum += it.downloaded;
        snap_.downloaded = sum;
    };

    const std::uint64_t want = item.file.bytes;
    // 盘上已经有一份对得上的就跳过。**这就是"重来一次不会重下"**：
    // 用户中途取消、或者上一轮有几个失败了，再点一次只补缺的那几个。
    //
    // **在整个模型目录里找，不只看要落的那个位置。** 这一版之前下的平铺在根上，
    // 人自己拷进来的哪儿都有——只看 `<类型>/` 底下的话，那几份全要重下一遍，
    // 几十 GB。大小要恰好对上才算（截断的同名文件不算）。
    // 旁边还躺着 aria2 的控制文件的不算：那是老版本直接写在正式名字上、按全长
    // 预分配好的半截（同 setup_api 的 download_in_progress）。
    const auto in_progress = [](const fs::path& p) {
        fs::path ctrl = p;
        ctrl += ".aria2";
        std::error_code ec;
        return fs::exists(ctrl, ec);
    };
    if (want > 0) {
        if (const auto have = config::find_model(dir, item.file.name, want);
            have && !in_progress(*have)) {
            std::string at = paths::to_utf8(have->lexically_relative(dir));
            std::replace(at.begin(), at.end(), '\\', '/');
            set([&](ItemProgress& p) {
                p.state = ItemState::Present;
                p.path = at;
                p.downloaded = want;
                p.speed_bps = 0.0;
            });
            return true;
        }
    }

    std::error_code ec;
    // **下载途中不占正式的文件名，先写 `<名字>.part`，校验过了再改名。**
    //
    // 原来是直接 `-o dest`：curl 创建文件的那一瞬间，盘上就有了一个叫
    // 最终名字的文件。而"这台能不能出片"判的是
    // `std::filesystem::exists`（node_status.cpp → capability.cpp 的
    // `has()`），**只问在不在、不问全不全**——于是一个 17.5 GB 的权重
    // 刚下了几十兆，这台就开始对外宣称自己会出片，调度器照着派真镜头
    // 过来，加载时炸在"读不对"上，而人看到的是"模型坏了"。
    //
    // 实测：MiniMax-H3 那一组下到 15%（fl2va 2.6 GB / 17.5 GB），
    // /api/nodes 里这台的 able 已经是 ['llm','video','assemble']。
    //
    // 改名是同一个目录内的 rename，原子的；`.part` 留着也照样能续传
    // （curl -C - 接着它写），断一次不会重头来。
    const fs::path part =
        dest.parent_path() / (paths::to_utf8(dest.filename()) + ".part");

    // **老位置上下了一半的，挪过来接着下。** 这一版之前是平铺在根上下的
    // （`<目录>/<名字>.part`），现在落到 `<类型>/` 底下。不挪的话新位置从 0 开始，
    // 老位置那半截（可能几十 GB）就白下了，还一直占着盘。
    // 同一个盘里改名，不搬字节。aria2 的控制文件跟着走，不然它认不出续传。
    if (const fs::path old = dir / paths::from_utf8(item.file.name); old != dest) {
        const fs::path old_part =
            old.parent_path() / (paths::to_utf8(old.filename()) + ".part");
        if (file_size_or_zero(part) == 0) {
            fs::path from;
            if (file_size_or_zero(old_part) > 0) {
                from = old_part;
            } else if (const auto n = file_size_or_zero(old); n > 0 && n < want) {
                // 更老的版本连 `.part` 都没有，半截就躺在正式名字上。
                from = old;
            }
            if (!from.empty()) {
                std::error_code mk;
                fs::create_directories(part.parent_path(), mk);
                std::error_code mv;
                fs::rename(from, part, mv);
                if (!mv) {
                    fs::path ctrl_from = from;
                    ctrl_from += ".aria2";
                    fs::path ctrl_to = part;
                    ctrl_to += ".aria2";
                    std::error_code cm;
                    if (fs::exists(ctrl_from, cm)) fs::rename(ctrl_from, ctrl_to, cm);
                }
            }
        }
    }

    // `.part` 已经下全了、只差改名（上一轮父进程被杀，见
    // adopt_finished_parts）：改个名就是了，一个字节都不用再下。
    if (want > 0 && file_size_or_zero(part) == want) {
        std::error_code mv;
        fs::rename(part, dest, mv);
        if (!mv) {
            config::forget_model_index();
            set([&](ItemProgress& p) {
                p.state = ItemState::Present;
                p.downloaded = want;
                p.speed_bps = 0.0;
            });
            return true;
        }
    }

    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        set([&](ItemProgress& p) {
            p.state = ItemState::Failed;
            p.error = SAYF("建不了目录 %1：%2",
                           paths::to_utf8(dest.parent_path()), ec.message());
        });
        return false;
    }

    const std::string tool = pick_tool();
    // ⚠️ **日志必须一个文件一份。**
    //
    // 原来是整个目录共用一个 `.changji-download.log`——一个一个下的时候
    // 没问题，几路一起下就成了几个进程往同一个文件里交替写，而
    // parse_aria2_progress 读的是末尾那几 KB：**A 文件的进度条会跳成
    // B 文件的进度**，而下载本身是好的，从现象完全看不出是日志串了。
    const fs::path log =
        dest.parent_path() / paths::from_utf8("." + paths::to_utf8(dest.filename()) + ".log");

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (cancel_) {
            set([](ItemProgress& p) { p.state = ItemState::Canceled; });
            return false;
        }

        // 日志每次清掉。留着的话上一次的进度行会被当成这一次的——
        // 表现是进度条一开始就停在上次断掉的位置不动。
        fs::remove(log, ec);

        // 旧版本（或这一版修好之前）留下的半截文件就躺在正式名字上。
        // 挪到 `.part` 去：续传接得上，而那个会骗人的 `exists` 当场消失。
        if (const auto here = file_size_or_zero(dest); here > 0 && here != want) {
            std::error_code mv;
            fs::rename(dest, part, mv);
            if (mv) fs::remove(dest, mv);
        }

        // **比应有的还大就只能重下。**
        //
        // `-C -` 是**追加**式续传：它从当前文件长度往后接。文件一旦被写
        // 过头，续传每一次都只会让它更大或不变，`got == want` 永远不成立
        // ——重试三次是纯粹空转，最后报一句「试了 3 次都没下全」，而实际
        // 是下多了，人照着这句话去查网络，方向正好反的。
        //
        // 怎么会写过头：两个 curl 同时写同一个 `.part`。2026-09-15 实测
        // 撞到——反复重启 worker 时，`setsid` 派生的旧 curl 成了孤儿还在
        // 写，而新一轮又给同一个文件开了一个。Qwen_Image-Q8_0.gguf 因此
        // 变成 21840951840 字节，比应有的 21761817120 多了 79 MB。
        if (want > 0) {
            if (const auto here = file_size_or_zero(part); here > want) {
                std::error_code rm;
                fs::remove(part, rm);
                set([](ItemProgress& p) {
                    p.downloaded = 0;
                    p.error = SAY("盘上那份比应有的还大（多半是上一轮有两个"
                                  "下载同时写它），续传接不回来，这一次"
                                  "从头下。");
                });
            }
        }

        std::vector<std::string> args;
        if (tool == "aria2c") {
            args = {"-x", "8", "-s", "8", "-k", "4M",
                    "--file-allocation=none", "--auto-file-renaming=false",
                    "--allow-overwrite=true", "--continue=true",
                    "--max-tries=3", "--retry-wait=5",
                    "--console-log-level=warn", "--summary-interval=1",
                    "-d", paths::to_utf8(dest.parent_path()),
                    "-o", paths::to_utf8(part.filename()), item.url};
        } else {
            // curl 是单连接，文件大小就是真进度，不用解析输出。
            args = {"-L", "--fail", "--retry", "3", "--retry-delay", "5",
                    "-C", "-", "-o", paths::to_utf8(part), item.url};
        }

        const proc::ProcHandle h = proc::spawn(tool, args, log);
        if (h == 0) {
            set([&](ItemProgress& p) {
                p.state = ItemState::Failed;
                p.error = SAYF("起不来 %1。检查它在不在 PATH 上。", tool);
            });
            return false;
        }

        set([](ItemProgress& p) {
            p.state = ItemState::Running;
            p.error.clear();
        });

        // 盯着它。速度优先用下载器自己报的（aria2 的 DL:），
        // 拿不到就自己按文件大小的增量算。
        auto last_at = clock_type::now();
        std::uint64_t last_bytes = file_size_or_zero(part);
        double smoothed = 0.0;
        bool killed = false;

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            if (cancel_) {
                proc::kill_spawned(h, 2000);
                killed = true;
                break;
            }
            const bool still = proc::alive(h);

            std::uint64_t now_bytes = file_size_or_zero(part);
            double speed = 0.0;
            if (tool == "aria2c") {
                const auto parsed = parse_aria2_progress(tail_of(log, 4096));
                if (parsed.has_value()) {
                    // **用它报的字节数，不用文件大小。** 多连接下载会往
                    // 文件中间写，表观大小随时可能跳到接近全长，
                    // 那样进度条会先冲到 99% 再原地不动。
                    now_bytes = parsed->downloaded;
                    speed = parsed->speed_bps;
                }
            }
            const auto now = clock_type::now();
            if (speed <= 0.0) {
                const double dt =
                    std::chrono::duration<double>(now - last_at).count();
                if (dt > 0.0 && now_bytes >= last_bytes) {
                    speed = static_cast<double>(now_bytes - last_bytes) / dt;
                }
            }
            last_at = now;
            last_bytes = now_bytes;
            // 平滑一下。瞬时速度抖得厉害，界面上那个数字会闪到看不清。
            smoothed = smoothed <= 0.0 ? speed : smoothed * 0.7 + speed * 0.3;

            const double shown = smoothed;
            const std::uint64_t bytes = now_bytes;
            set([&](ItemProgress& p) {
                p.downloaded = bytes;
                p.speed_bps = shown;
            });
            {
                std::lock_guard<std::mutex> lock(mu_);
                // **总速度是在跑的那几路之和**，不是这一路的。
                // 写成这一路的话，四路一起下时界面上显示的是其中随便
                // 一路的速度，而剩下的时间按它算——报出来的数比实际慢
                // 三四倍，用户会以为并发没生效。
                double sum = 0.0;
                for (const auto& it : snap_.items) {
                    if (it.state == ItemState::Running) sum += it.speed_bps;
                }
                snap_.speed_bps = sum;
                snap_.eta_seconds =
                    sum > 1.0 && snap_.total > snap_.downloaded
                        ? static_cast<double>(snap_.total - snap_.downloaded) / sum
                        : -1.0;
            }

            if (!still) break;
        }

        if (killed) {
            set([](ItemProgress& p) {
                p.state = ItemState::Canceled;
                p.speed_bps = 0.0;
            });
            return false;
        }

        const std::uint64_t got = file_size_or_zero(part);
        // **字节数对不上就算没下完。** 下载器退出码为 0 也可能留下截断的
        // 文件（连接被中间设备掐断、镜像返回了一个错误页）。
        // 而截断的权重加载时报的是"读不对"，指向完全错误的方向。
        if (want == 0 || got == want) {
            // 字节数对上了才把它摆到正式名字上——**在这之前，任何人问
            // "这台有没有这个模型"，答案都必须是没有。**
            std::error_code mv;
            fs::rename(part, dest, mv);
            if (mv) {
                set([&](ItemProgress& p) {
                    p.state = ItemState::Failed;
                    p.speed_bps = 0.0;
                    p.error = SAYF("下全了，但改名失败：%1", mv.message());
                });
                return false;
            }
            // 索引里那份是改名之前扫的，扔掉：下一次 `resolve` 要看得见它。
            config::forget_model_index();
            set([&](ItemProgress& p) {
                p.state = ItemState::Done;
                p.downloaded = want == 0 ? got : want;
                p.speed_bps = 0.0;
            });
            return true;
        }

        const std::string detail = last_lines(tail_of(log, 4096), 4);
        if (attempt == kMaxAttempts) {
            set([&](ItemProgress& p) {
                p.state = ItemState::Failed;
                p.speed_bps = 0.0;
                // **多了和少了是两件事，别都说成"没下全"。**
                // 少了是网络断在半路；多了是这个文件被写过头（两个下载
                // 同时写），那时候人该做的是删掉重下，不是去查网络。
                const bool over = got > want;
                p.error =
                    SAYN("下了 %n 字节，应该是 %1。", static_cast<long long>(got),
                         std::to_string(want)) +
                    (over ? SAYN("比应有的多 %n 字节——这一份坏了，删掉 .part "
                                 "重下。",
                                 static_cast<long long>(got - want))
                          : SAYF("试了 %1 次都没下全。",
                                 std::to_string(kMaxAttempts))) +
                    (detail.empty() ? std::string{}
                                    : SAYF("\n下载器最后说：\n%1", detail));
            });
            return false;
        }
        set([&](ItemProgress& p) {
            p.error = SAYF("第 %1 次没下全，接着续传。",
                           std::to_string(attempt));
        });
    }
    return false;
}

}  // namespace changji::setup
