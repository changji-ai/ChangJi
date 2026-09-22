// 那张「机器 × 能力」的表，拼给界面的那一段（`nodes_json`）。
//
// 要钉死的是一件事：**灰格子那句"为什么干不了"必须一路传到界面。**
//
// 那句话是那台机器自己算的（`missing_for` 跑在它本地：缺哪个模型文件、
// 编没编进 sd.cpp，只有它知道），一路是
//
//     那台的 missing_for → 它的 /status 的 capabilities[].why
//       → 这边 refresh() 收进 NodeState::why → nodes_json 的 why
//       → 界面上悬停时那行字
//
// 断在任何一环，界面上就是一个灰格子、悬停只有「写文：干不了。」后面
// 什么都没有——而那个格子除了"灰"以外本来就什么都不说，**这句话是用户
// 唯一的线索**。实际就断过：refresh() 那个循环写的是
// `if (!item.value("able", false)) continue;`，why 连读都没读。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "infer/node_registry.hpp"

using namespace changji;
using namespace changji::infer;
using json = nlohmann::json;

namespace {

/// 从那张表里挑出某台某个能力那一格。
json cell(const json& table, const std::string& url, const std::string& cap) {
    for (const auto& row : table.at("nodes")) {
        if (row.at("url") != url) continue;
        for (const auto& c : row.at("capabilities")) {
            if (c.at("cap") == cap) return c;
        }
    }
    return json(nullptr);
}

}  // namespace

TEST_CASE("干不了的那一格，把原因带到界面") {
    NodeState n;
    n.url = "http://gpu-box:9001";
    n.name = "gpu-box";
    n.online = true;
    n.able = {Capability::Llm, Capability::Tts};
    n.why[Capability::Frame] = "这个二进制没编 sd.cpp，出不了图";
    n.why[Capability::Video] = "[models].video 没配，或者文件不在";

    const json t = nodes_json({n});

    const auto frame = cell(t, n.url, "frame");
    REQUIRE(frame.is_object());
    CHECK(frame.at("able") == false);
    CHECK(frame.at("why") == "这个二进制没编 sd.cpp，出不了图");

    CHECK(cell(t, n.url, "video").at("why") ==
          "[models].video 没配，或者文件不在");

    // 能干的那几格不带话：格子是实心的，没什么要解释。
    CHECK(cell(t, n.url, "llm").at("able") == true);
    CHECK(cell(t, n.url, "llm").at("why") == "");
}

TEST_CASE("整台连不上时，每一格都说那句连接错误") {
    // **别留白**：连不上的那台一个能力都报不上来，于是每一格都是灰的。
    // 不把 error 填进去的话，悬停五个格子看到的是五句「干不了。」，
    // 而真正的原因（口令不对／连不上／答的不是 JSON）只在行首那一小行。
    NodeState n;
    n.url = "http://gpu-box:9001";
    n.name = n.url;
    n.online = false;
    n.error = "口令不对。这台的 [peer].token 和你这边填的对不上";

    const json t = nodes_json({n});
    for (const auto& c : t.at("nodes")[0].at("capabilities")) {
        CAPTURE(c.at("cap"));
        CHECK(c.at("able") == false);
        CHECK(c.at("why") == n.error);
        // 连不上的一律不参与调度，哪怕没人关过它。
        CHECK(c.at("on") == false);
    }
}

TEST_CASE("那台自己给的话优先，轮不到拿连接错误顶") {
    // 在线但干不了：话是它自己算的，这边不许改写。
    NodeState n;
    n.url = "local";
    n.online = true;
    n.error = "";
    n.why[Capability::Tts] = "[models].tts_decoder 没配，或者文件不在";

    const json t = nodes_json({n});
    CHECK(cell(t, "local", "tts").at("why") ==
          "[models].tts_decoder 没配，或者文件不在");
    CHECK(t.at("nodes")[0].at("local") == true);
}

TEST_CASE("关掉的那一格：能干，只是不参与") {
    NodeState n;
    n.url = "local";
    n.online = true;
    n.able = {Capability::Frame};
    n.off = {Capability::Frame};

    const json t = nodes_json({n});
    const auto c = cell(t, "local", "frame");
    CHECK(c.at("able") == true);
    CHECK(c.at("off") == true);
    CHECK(c.at("on") == false);
    // **能干的没有"为什么干不了"**——有的话界面会把关掉的画成灰的。
    CHECK(c.at("why") == "");
}
