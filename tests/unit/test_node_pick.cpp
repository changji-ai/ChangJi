// 「这个能力派给哪台」的三层过滤。
//
// 挑错了都不报错：挑了台干不了的是那一镜失败，该挑的没挑是那台一直闲着
// 而界面上看不出为什么。所以这一段要能反复撞。
//
// 最要紧的一条是最后那组：**一台都派不出去时，得说清是哪一层拦的**。
// 「连不上」「你自己关的」「它干不了」这三种，用户要做的事完全不同——
// 等一等／去表上打开／去装模型。

#include <doctest/doctest.h>

#include "infer/node_pick.hpp"
#include "util/say.hpp"

using namespace changji::infer;

namespace {

NodeState node(std::string url, bool online, std::set<Capability> able,
               std::set<Capability> off = {}, bool busy = false) {
    NodeState n;
    n.url = std::move(url);
    n.name = n.url;
    n.online = online;
    n.able = std::move(able);
    n.off = std::move(off);
    n.busy = busy;
    return n;
}

}  // namespace

TEST_CASE("三层：在线、没被关、自己说能干") {
    const std::vector<NodeState> nodes = {
        node("a", true, {Capability::Frame, Capability::Video}),
        node("b", false, {Capability::Frame}),                      // 连不上
        node("c", true, {Capability::Frame}, {Capability::Frame}),  // 你关的
        node("d", true, {Capability::Llm}),                         // 干不了
    };
    const auto cands = candidates_for(nodes, Capability::Frame);
    REQUIRE(cands.size() == 1);
    CHECK(cands.front()->url == "a");
}

TEST_CASE("空闲的优先") {
    const std::vector<NodeState> nodes = {
        node("busy", true, {Capability::Video}, {}, /*busy=*/true),
        node("idle", true, {Capability::Video}),
    };
    CHECK(pick_for(nodes, Capability::Video) == "idle");
}

TEST_CASE("全忙也要回一台，去排队而不是判失败") {
    // 一镜一两分钟等得起；当场失败的话，用户得到的是一章里随机几镜没了。
    const std::vector<NodeState> nodes = {
        node("x", true, {Capability::Video}, {}, true),
        node("y", true, {Capability::Video}, {}, true),
    };
    const auto got = pick_for(nodes, Capability::Video);
    REQUIRE(got.has_value());
    CHECK(*got == "x");
}

TEST_CASE("一台候选都没有才回空") {
    const std::vector<NodeState> nodes = {
        node("a", true, {Capability::Llm}),
    };
    CHECK_FALSE(pick_for(nodes, Capability::Video).has_value());
}

TEST_CASE("钉死某台 = 把别台关掉，走的是同一套代码") {
    // 「全自动」和「钉死某台」不该是两条路——分成两条，迟早在某个边角上
    // 行为不一致，而那种不一致查起来要先怀疑调度、再怀疑配置。
    std::vector<NodeState> nodes = {
        node("a", true, {Capability::Video}),
        node("b", true, {Capability::Video}),
    };
    CHECK(candidates_for(nodes, Capability::Video).size() == 2);

    nodes[0].off.insert(Capability::Video);   // 表上把 a 的出片关掉
    const auto cands = candidates_for(nodes, Capability::Video);
    REQUIRE(cands.size() == 1);
    CHECK(cands.front()->url == "b");
    CHECK(pick_for(nodes, Capability::Video) == "b");
}

TEST_CASE("派不出去时说清是哪一层拦的") {
    SUBCASE("一台都没登记") {
        const std::string why = why_no_node({}, Capability::Video);
        CHECK(why.find("没有任何一台") != std::string::npos);
    }
    SUBCASE("连不上") {
        const std::vector<NodeState> nodes = {
            node("a", false, {Capability::Video}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("连不上") != std::string::npos);
    }
    SUBCASE("你自己关的") {
        const std::vector<NodeState> nodes = {
            node("a", true, {Capability::Video}, {Capability::Video}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("关掉") != std::string::npos);
    }
    SUBCASE("它自己说干不了") {
        const std::vector<NodeState> nodes = {
            node("a", true, {Capability::Llm}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("干不了") != std::string::npos);
    }
    SUBCASE("三种都有就三种都说") {
        const std::vector<NodeState> nodes = {
            node("a", false, {Capability::Video}),
            node("b", true, {Capability::Video}, {Capability::Video}),
            node("c", true, {Capability::Llm}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("连不上") != std::string::npos);
        CHECK(why.find("关掉") != std::string::npos);
        CHECK(why.find("干不了") != std::string::npos);
    }
}

TEST_CASE("那句话里带着能力的中文名") {
    // 它会直接显示给用户，"video 这一步派不出去"没人看得懂。
    const std::string why = why_no_node({}, Capability::Tts);
    CHECK(why.find("配音") != std::string::npos);
}

TEST_CASE("一台报几个槽，派活就占几个位置") {
    // 池按下标记忙闲，同一个 url 出现两次就是两条独立通道——
    // 一台双卡机两张卡一起干靠的就是这一句。
    auto two = node("http://box:8080", true, {Capability::Video});
    two.slots = 2;
    auto one = node("http://solo:8080", true, {Capability::Video});
    auto local = node("local", true, {Capability::Video});
    local.slots = 3;   // 本机那条不走这儿，几个槽都不算
    const std::vector<NodeState> nodes = {two, local, one};
    const auto eps = remote_slots_for(nodes, Capability::Video);
    REQUIRE(eps.size() == 3);
    CHECK(eps[0] == "http://box:8080");
    CHECK(eps[1] == "http://box:8080");
    CHECK(eps[2] == "http://solo:8080");
}

TEST_CASE("槽数报 0 按 1 算，一台在线的机器不能因此从池里消失") {
    auto n = node("http://box:8080", true, {Capability::Frame});
    n.slots = 0;
    CHECK(remote_slots_for({n}, Capability::Frame).size() == 1);
}

// ---------------------------------------------------------------------------
// 答得慢 ≠ 没了
// ---------------------------------------------------------------------------
//
// 2026-09-20 实测撞到的：那台 L20 正渲着我们派过去的活，`/status` 答得比
// 探活超时（3 秒）还慢——边渲边答要 2.4 秒，读权重那一分钟干脆答不上来。
// 于是机器表判它离线，接着体检说"出图后端没编进来"（本机确实没有），
// 镜头页三颗按钮全灰，出片被拒。**人什么都没干，只是那台正在替他干活。**

TEST_CASE("接得上但答得慢：先按上一次问到的算") {
    using namespace std::chrono;
    CHECK(changji::infer::coast_on_last_ok(changji::infer::ProbeMiss::Slow, true, seconds(30)));
    CHECK(changji::infer::coast_on_last_ok(changji::infer::ProbeMiss::Slow, true, seconds(119)));
}

TEST_CASE("慢过两分钟就别按上一次算了：真死的机器也得掉出去") {
    using namespace std::chrono;
    // 探活每 15 秒一拍，两分钟是连丢七八拍。而读权重那一分钟（实测远端
    // 59 秒）撑得过去——两头都照顾到。
    CHECK_FALSE(changji::infer::coast_on_last_ok(changji::infer::ProbeMiss::Slow, true, seconds(121)));
}

TEST_CASE("被拒不给宽限：那台真没了，按上一次算会一直往死地址派活") {
    using namespace std::chrono;
    CHECK_FALSE(changji::infer::coast_on_last_ok(changji::infer::ProbeMiss::Refused, true, seconds(1)));
}

TEST_CASE("答了但答得不对，一律照实报") {
    using namespace std::chrono;
    // 口令不对、起错了模式（完整服务的 /status 回的是那张网页）——这些
    // 等多久都不会自己好，而那句话正是用户唯一的线索。按上一次算等于
    // 把它藏起来。
    CHECK_FALSE(changji::infer::coast_on_last_ok(changji::infer::ProbeMiss::Answered, true, seconds(1)));
}

TEST_CASE("从来没问到过，也就没有「上一次」可按") {
    using namespace std::chrono;
    CHECK_FALSE(changji::infer::coast_on_last_ok(changji::infer::ProbeMiss::Slow, false, seconds(1)));
}

TEST_CASE("派不出去那句话：一台的时候动词也得跟着变") {
    // **这一条是实地看见的。** 2026-09-22 真读 `/api/nodes` 的回包，英语下
    // 一台时出来的是：
    //
    //     1 machine say they can’t do it (…)
    //
    // 数换成了单数，动词还是复数——因为那句话原来是 `%n 台` 加上一个**碎片**
    // （「自己说干不了」），而碎片在别的语言里自己带着动词，`%n` 管不到它。
    // 现在三种理由各是一整句（见 node_pick.cpp 那段），这条钉的就是这件事。
    //
    // ⚠️ **中文查不出这个毛病**：中文的动词不随数变，一台和三台一个样。
    // 所以这条用例必须**换一种语言说**。
    struct Back {
        std::string was = changji::i18n::spoken();
        ~Back() { changji::i18n::speak(was); }
    } back;
    changji::i18n::speak("en");

    SUBCASE("一台自己说干不了：says it") {
        const std::vector<NodeState> nodes = {node("a", true, {Capability::Llm})};
        const std::string why = why_no_node(nodes, Capability::Video);
        CAPTURE(why);
        CHECK(why.find("1 machine says it can’t do it") != std::string::npos);
        CHECK(why.find("say they") == std::string::npos);
    }
    SUBCASE("两台自己说干不了：say they") {
        const std::vector<NodeState> nodes = {node("a", true, {Capability::Llm}),
                                              node("b", true, {Capability::Llm})};
        const std::string why = why_no_node(nodes, Capability::Video);
        CAPTURE(why);
        CHECK(why.find("2 machines say they can’t do it") != std::string::npos);
    }
    SUBCASE("一台连不上：is，两台：are") {
        const std::vector<NodeState> one = {node("a", false, {Capability::Video})};
        CHECK(why_no_node(one, Capability::Video).find("1 machine can’t be reached")
              != std::string::npos);
        const std::vector<NodeState> two = {node("a", false, {Capability::Video}),
                                            node("b", false, {Capability::Video})};
        CHECK(why_no_node(two, Capability::Video).find("2 machines can’t be reached")
              != std::string::npos);
    }
    SUBCASE("一台被关掉：is turned off") {
        const std::vector<NodeState> nodes = {
            node("a", true, {Capability::Video}, {Capability::Video})};
        const std::string why = why_no_node(nodes, Capability::Video);
        CAPTURE(why);
        CHECK(why.find("1 machine is turned off") != std::string::npos);
        CHECK(why.find("machines are turned off") == std::string::npos);
    }
}
