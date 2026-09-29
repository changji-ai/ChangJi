#include "lan/peers.hpp"

#include "util/atomic_file.hpp"

#include <algorithm>
#include <fstream>
#include <random>
#include <system_error>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::lan {

namespace {
/// 现生一串。**三十二个十六进制字符**——它是凭据，比那个 id 要长一倍。
std::string fresh_ticket() {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string t;
    t.reserve(32);
    for (int i = 0; i < 32; ++i) t += kHex[rd() % 16];
    return t;
}
}  // namespace


void PeerBook::saw(const Peer& p) {
    if (p.id.empty()) return;
    Peer& slot = seen_[p.id];
    slot.id = p.id;
    // **名字空着就别把旧的盖掉。** mDNS 那头有几种回调只带地址不带 TXT，
    // 拿空串盖过去的话，表上那一行会突然变成没有名字的一台。
    if (!p.name.empty()) slot.name = p.name;
    // **地址变了，他给我的那张票作废。** id 写在 mDNS 的 TXT 里、谁都报得出：
    // 同网段有人拿他的 id 报一个自己的地址，票原来原样跟着搬过去——下一轮
    // 问状态、把他写进机器表时，票就交到冒名的那台手上了。真是他换了网络的
    // 话，下一轮照新地址再要一次，拿回来的还是同一张（那头不换票）。
    const bool moved = (!p.host.empty() && !slot.host.empty() && p.host != slot.host) ||
                       (p.port > 0 && slot.port > 0 && p.port != slot.port);
    if (moved) {
        slot.their_ticket.clear();
        slot.their_gpu = -1;
        slot.their_vram_used = 0;
        slot.their_vram_total = 0;
    }
    if (!p.host.empty()) slot.host = p.host;
    if (p.port > 0) slot.port = p.port;
    if (p.seen_at > slot.seen_at) slot.seen_at = p.seen_at;
    slot.online = true;
}

void PeerBook::theirs(const std::string& id, const std::string& ticket,
                      double gpu, double vram_used, double vram_total) {
    const auto it = seen_.find(id);
    if (it == seen_.end()) return;      // 没在表上就不凭空造一行
    // **票空着不覆盖。** 这一轮只是没问到（网络抖一下），
    // 拿空串盖过去的话下一轮又得重新要一次票。
    if (!ticket.empty()) it->second.their_ticket = ticket;
    it->second.their_gpu = gpu;
    it->second.their_vram_used = vram_used;
    it->second.their_vram_total = vram_total;
}

void PeerBook::lost(const std::string& id) {
    const auto it = seen_.find(id);
    if (it == seen_.end()) return;
    it->second.online = false;
    // **走了就把那张卡的读数抹掉。** 留着的话界面上一个离线的机器还亮着
    // 一个"忙 70%"，而那是十分钟前的事。票留着——他给的授权没收回。
    it->second.their_gpu = -1;
    it->second.their_vram_used = 0;
    it->second.their_vram_total = 0;
}

void PeerBook::allow(const std::string& id, const Grant& g) {
    if (id.empty()) return;
    // **两档都关就把这一条整个删掉。** 留一行全 false 的记录没有意义，
    // 而落盘那份会越攒越长——一年下来全是"曾经见过、什么都没给"的机器。
    if (!g.use && !g.params) {
        grants_.erase(id);      // 票跟着作废
        return;
    }
    Grant put = g;
    // **已经有票的那一条不换票。** 换票等于把对面正在跑的活踢下线，
    // 而人只是想多给一档（或者少给一档）。
    const auto old = grants_.find(id);
    if (old != grants_.end() && !old->second.ticket.empty()) {
        put.ticket = old->second.ticket;
    } else if (put.ticket.empty()) {
        put.ticket = fresh_ticket();
    }
    grants_[id] = put;
}

Grant PeerBook::grant_of(const std::string& id) const {
    const auto it = grants_.find(id);
    return it == grants_.end() ? Grant{} : it->second;
}

std::vector<Peer> PeerBook::list() const {
    std::vector<Peer> out;
    out.reserve(seen_.size());
    for (const auto& [_, p] : seen_) out.push_back(p);
    std::sort(out.begin(), out.end(), [](const Peer& a, const Peer& b) {
        if (a.online != b.online) return a.online;       // 在线的排前面
        if (a.name != b.name) return a.name < b.name;
        return a.id < b.id;
    });
    return out;
}

Grant PeerBook::by_ticket(const std::string& ticket) const {
    // **空票一律不认。** 没带那个头的请求会送一个空串进来，而
    // "空票配空票"会把所有人都放行——这一条正是那种一读就过、
    // 一想就出冷汗的写法。
    if (ticket.empty()) return {};
    // **逐字节比完再回，不提前短路**（同 infer::token_ok）：这是对局域网开着的门，
    // `==` 在第一个不同的字节就返回，按响应快慢能一个字节一个字节地猜出票来。
    const auto same = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        }
        return diff == 0;
    };
    Grant hit;
    bool found = false;
    for (const auto& [_, g] : grants_) {
        // 每一张都比：找到就停的话，比了几张也是时间差。
        if (same(g.ticket, ticket) && !found) {
            hit = g;
            found = true;
        }
    }
    return found ? hit : Grant{};
}

json PeerBook::grants_json() const {
    json out = json::object();
    for (const auto& [id, g] : grants_) {
        out[id] = {{"use", g.use}, {"params", g.params}, {"ticket", g.ticket}};
    }
    return out;
}

void PeerBook::load_grants(const json& j) {
    grants_.clear();
    if (!j.is_object()) return;
    for (const auto& [id, v] : j.items()) {
        if (!v.is_object()) continue;
        Grant g;
        g.use = v.value("use", false);
        g.params = v.value("params", false);
        g.ticket = v.value("ticket", std::string());
        if (!g.use && !g.params) continue;
        // **票丢了就补一张。** 老版本存下来的那一份没有这一栏，
        // 而没票的授权是放不了行的——补一张比让人重点一遍强。
        if (g.ticket.empty()) g.ticket = fresh_ticket();
        grants_[id] = g;
    }
}

std::string my_id(const fs::path& data_dir) {
    const fs::path f = data_dir / "lan_id";
    std::error_code ec;
    if (fs::exists(f, ec)) {
        std::ifstream in(f);
        std::string got;
        std::getline(in, got);
        // 掐头去尾。**读回来的那一行可能带换行**，而它要进 TXT 记录，
        // 带一个换行的 id 在对面看起来就是另一台机器。
        while (!got.empty() && (got.back() == '\n' || got.back() == '\r' ||
                                got.back() == ' ')) {
            got.pop_back();
        }
        if (!got.empty()) return got;
    }
    // 现生一个。**十六个十六进制字符**：够短，摆得进 TXT；够长，同一个
    // 网段里撞不上。
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string id;
    id.reserve(16);
    for (int i = 0; i < 16; ++i) id += kHex[rd() % 16];
    fs::create_directories(data_dir, ec);
    // 整份换：原地截断写到一半没了的话，下次读回来是空串，又现生一个——
    // 这台机器在别人表上就成了另一台，发给它的票全对不上。
    try {
        util::write_file_atomic(f, id + "\n", /*private_only=*/false);
    } catch (const std::exception&) {
        // 写不下也照样用这一个 id 跑完这一趟；下次起来再生一个。
    }
    return id;
}

}  // namespace changji::lan
