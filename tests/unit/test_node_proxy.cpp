// 转发到别的机器（infer/node_proxy）：不在机器表里的地址一律不转。
//
// 2026-09-25 审出来：`/api/nodes/setup?url=…` 是 GET，地址直接取自请求，而
// 转发时"不在配置里也照发、带全局口令"。于是一个 `?url=http://随便哪/`
// 就让引擎带着 `Authorization: Bearer <peer.token>` 去敲那个地址——那个口令
// 守着每一台工作进程的 `/task`。node_get / node_post 进门先问 proxy_target。

#include <doctest/doctest.h>

#include "config/settings.hpp"
#include "infer/node_proxy.hpp"

using namespace changji;

TEST_CASE("转发：不在机器表里的地址不转，口令一个字都不给") {
    config::Settings s;
    s.peer.token = "全局口令-不该出门";

    const auto t = infer::proxy_target(s, "http://attacker:1/");
    CHECK_FALSE(t.listed);
    CHECK(t.token.empty());
}

TEST_CASE("转发：登记了的照常转，带那一台自己的口令，没配就用全局的") {
    config::Settings s;
    s.peer.token = "全局口令";
    config::PeerNodeConfig own;
    own.url = "http://gpu-a:8080";
    own.token = "这台的口令";
    config::PeerNodeConfig shared;
    shared.url = "http://gpu-b:8080";
    s.peer.nodes = {own, shared};

    const auto a = infer::proxy_target(s, "http://gpu-a:8080");
    CHECK(a.listed);
    CHECK(a.token == "这台的口令");
    const auto b = infer::proxy_target(s, "http://gpu-b:8080");
    CHECK(b.listed);
    CHECK(b.token == "全局口令");
    // 逐字比：多一个斜杠、换个端口都不算同一台
    CHECK_FALSE(infer::proxy_target(s, "http://gpu-a:8081").listed);
}
