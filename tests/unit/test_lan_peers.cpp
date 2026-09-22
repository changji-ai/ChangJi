// 局域网上看见了谁、谁能用这台机器。
//
// 这一族全是边界，而**那些错在界面上看不出来**：同一台换了 IP、下线又上线、
// 权限给了又收——出事的表现是"某天他突然用不了了"，或者更糟，"某天他
// 突然能用了"。所以那张表收成纯的（`lan/peers.hpp` 不碰网络也不碰 mDNS），
// 在这儿逐条钉。

#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include "lan/peers.hpp"

namespace fs = std::filesystem;

namespace {
/// 一个没人用过的临时目录。**每次都不一样**——同一次跑里两条用例撞在
/// 同一个目录上的话，第二条读到的是第一条留下的 id。
fs::path temp_dir(const std::string& tag) {
    static std::random_device rd;
    const auto p = fs::temp_directory_path() /
                   ("changji_" + tag + "_" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}
}  // namespace

using changji::lan::Grant;
using changji::lan::Peer;
using changji::lan::PeerBook;

namespace {
Peer one(const std::string& id, const std::string& name, const std::string& host,
         int port = 8080, std::int64_t at = 1000) {
    Peer p;
    p.id = id; p.name = name; p.host = host; p.port = port; p.seen_at = at;
    return p;
}
}  // namespace

TEST_CASE("看见一台：同一个 id 只占一行，换了地址就跟着换") {
    PeerBook b;
    b.saw(one("aa", "小王的 Mac", "192.168.1.9"));
    b.saw(one("aa", "小王的 Mac", "10.0.0.5", 9090, 2000));
    const auto all = b.list();
    REQUIRE(all.size() == 1);
    CHECK(all[0].host == "10.0.0.5");
    CHECK(all[0].port == 9090);
    CHECK(all[0].seen_at == 2000);
}

TEST_CASE("看见一台：名字空着别把旧的盖掉") {
    // mDNS 有几种回调只带地址不带 TXT。拿空串盖过去的话，表上那一行会
    // 突然变成没有名字的一台。
    PeerBook b;
    b.saw(one("aa", "小王的 Mac", "192.168.1.9"));
    b.saw(one("aa", "", "192.168.1.9"));
    REQUIRE(b.list().size() == 1);
    CHECK(b.list()[0].name == "小王的 Mac");
}

TEST_CASE("下线：只标成离线，不从表上拿掉") {
    // 拿掉的话给过的权限跟着没了，而人过一会儿开机回来还得再点一遍。
    PeerBook b;
    b.saw(one("aa", "小王", "192.168.1.9"));
    b.allow("aa", Grant{true, false});
    b.lost("aa");

    REQUIRE(b.list().size() == 1);
    CHECK_FALSE(b.list()[0].online);
    CHECK(b.may_use("aa"));          // 权限还在
}

TEST_CASE("默认一个权限都不给") {
    // mDNS 一开，同一个网段里所有开着感知的机器互相都看得见。看见就能派活
    // 的话，任何人只要打开同一个程序就能占用你的显卡。
    PeerBook b;
    b.saw(one("aa", "谁", "192.168.1.9"));
    CHECK_FALSE(b.may_use("aa"));
    CHECK_FALSE(b.may_pick("aa"));
    // 压根没见过的那一台，问也是不给。
    CHECK_FALSE(b.may_use("没见过"));
}

TEST_CASE("挑模型参数：必须先给「能用」") {
    // 光给第二档不给第一档是个说不通的状态，而它真能这么来：先给了两档，
    // 后来只收回第一档。
    PeerBook b;
    b.saw(one("aa", "谁", "1.2.3.4"));
    b.allow("aa", Grant{true, true});
    CHECK(b.may_pick("aa"));

    b.allow("aa", Grant{false, true});   // 收回「能用」，「挑参数」忘了收
    CHECK_FALSE(b.may_use("aa"));
    CHECK_FALSE(b.may_pick("aa"));       // 跟着也不算数
}

TEST_CASE("两档都关就把那一条整个删掉") {
    // 留一行全 false 的记录没有意义，而落盘那份会越攒越长——一年下来全是
    // "曾经见过、什么都没给"的机器。
    PeerBook b;
    b.allow("aa", Grant{true, true});
    b.allow("bb", Grant{true, false});
    REQUIRE(b.grants_json().size() == 2);

    b.allow("aa", Grant{false, false});
    CHECK(b.grants_json().size() == 1);
    CHECK(b.grants_json().contains("bb"));
}

TEST_CASE("权限存下来再读回去，一模一样") {
    PeerBook a;
    a.allow("aa", Grant{true, true});
    a.allow("bb", Grant{true, false});
    const auto saved = a.grants_json();

    PeerBook b;
    b.load_grants(saved);
    CHECK(b.may_use("aa"));
    CHECK(b.may_pick("aa"));
    CHECK(b.may_use("bb"));
    CHECK_FALSE(b.may_pick("bb"));
}

TEST_CASE("权限那份读坏了不许当成「都给」") {
    // 文件被手改坏、版本对不上——那时候**一个都不给**才是安全的那一边。
    PeerBook b;
    b.load_grants(nlohmann::json("这不是一个对象"));
    CHECK_FALSE(b.may_use("aa"));
    b.load_grants(nlohmann::json{{"aa", "也不是对象"}});
    CHECK_FALSE(b.may_use("aa"));
    // 全 false 的那一条读进来也不留。
    b.load_grants(nlohmann::json{{"aa", {{"use", false}, {"params", false}}}});
    CHECK(b.grants_json().empty());
}

TEST_CASE("表上的顺序：在线的排前面，然后按名字") {
    PeerBook b;
    b.saw(one("c", "阿三", "1.1.1.3"));
    b.saw(one("a", "阿大", "1.1.1.1"));
    b.saw(one("b", "阿二", "1.1.1.2"));
    b.lost("a");
    const auto all = b.list();
    REQUIRE(all.size() == 3);
    CHECK(all[0].name == "阿三");    // 在线，按名字「阿三」< 「阿二」？看编码
    CHECK(all[2].name == "阿大");    // 离线的沉底
    CHECK_FALSE(all[2].online);
}

TEST_CASE("这台机器那个 id：生成一次，之后每次都一样") {
    const auto dir = temp_dir("lan_id");
    const auto a = changji::lan::my_id(dir);
    const auto b = changji::lan::my_id(dir);
    CHECK(a.size() == 16);
    CHECK(a == b);
    // **换个目录就是另一台。**
    CHECK(changji::lan::my_id(temp_dir("lan_id2")) != a);
}

// ---- 那张票 ----
//
// ⚠️ **id 不能当凭据。** 它就写在 mDNS 的 TXT 记录里，同一个网段上谁都
// 读得到——拿 id 放行等于"谁报得出名字谁就能用"。收活那一关认的是票。

TEST_CASE("给权限就配一张票，收回就作废") {
    PeerBook b;
    b.allow("aa", Grant{true, false});
    const auto t = b.grant_of("aa").ticket;
    CHECK(t.size() == 32);
    CHECK(b.by_ticket(t).use);

    b.allow("aa", Grant{false, false});
    CHECK_FALSE(b.by_ticket(t).use);     // 作废了
}

TEST_CASE("改权限不换票") {
    // 换票等于把对面正在跑的活踢下线，而人只是想多给一档。
    PeerBook b;
    b.allow("aa", Grant{true, false});
    const auto first = b.grant_of("aa").ticket;
    b.allow("aa", Grant{true, true});
    CHECK(b.grant_of("aa").ticket == first);
    CHECK(b.by_ticket(first).params);
}

TEST_CASE("两条授权两张票，互不通用") {
    PeerBook b;
    b.allow("aa", Grant{true, true});
    b.allow("bb", Grant{true, false});
    const auto ta = b.grant_of("aa").ticket;
    const auto tb = b.grant_of("bb").ticket;
    CHECK(ta != tb);
    CHECK(b.by_ticket(ta).params);
    CHECK_FALSE(b.by_ticket(tb).params);
}

TEST_CASE("空票一律不认") {
    // 没带那个头的请求会送一个空串进来，而"空票配空票"会把所有人都放行
    // ——这一条正是那种一读就过、一想就出冷汗的写法。
    PeerBook b;
    b.allow("aa", Grant{true, true});
    CHECK_FALSE(b.by_ticket("").use);
    CHECK_FALSE(b.by_ticket("随便编的").use);
}

TEST_CASE("票跟着权限一起存下来，读回去还认") {
    PeerBook a;
    a.allow("aa", Grant{true, true});
    const auto t = a.grant_of("aa").ticket;

    PeerBook b;
    b.load_grants(a.grants_json());
    CHECK(b.by_ticket(t).use);
    CHECK(b.by_ticket(t).params);
}

TEST_CASE("老版本存的那份没有票，读回来补一张") {
    // 没票的授权是放不了行的——补一张比让人重点一遍强。
    PeerBook b;
    b.load_grants(nlohmann::json{{"aa", {{"use", true}, {"params", false}}}});
    const auto t = b.grant_of("aa").ticket;
    CHECK(t.size() == 32);
    CHECK(b.by_ticket(t).use);
}
