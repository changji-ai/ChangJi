// 顶栏那三个小表的数据源。真去读 /proc、问 NVML 的那些不在这儿测——
// 跑不跑得动看机器；这里测的是把文本变成数的那几个纯函数。

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "util/sysstat.hpp"

using namespace changji::sysstat;

TEST_CASE("sysstat: /proc/stat 第一行，idle 含 iowait，total 是前八列") {
    const std::string text =
        "cpu  1000 20 300 5000 100 10 5 0 0 0\n"
        "cpu0 500 10 150 2500 50 5 2 0 0 0\n";
    auto t = parse_proc_stat(text);
    REQUIRE(t.has_value());
    CHECK(t->idle == 5100);
    CHECK(t->total == 1000 + 20 + 300 + 5000 + 100 + 10 + 5 + 0);
}

TEST_CASE("sysstat: /proc/stat 不是 cpu 打头或者列不够就当没读到") {
    CHECK_FALSE(parse_proc_stat("").has_value());
    CHECK_FALSE(parse_proc_stat("intr 1 2 3\n").has_value());
    CHECK_FALSE(parse_proc_stat("cpu 1 2\n").has_value());
}

TEST_CASE("sysstat: /proc/meminfo 取 MemTotal 和 MemAvailable，单位 GB") {
    const std::string text =
        "MemTotal:       67108864 kB\n"
        "MemFree:         1000000 kB\n"
        "MemAvailable:   33554432 kB\n"
        "Buffers:          123456 kB\n";
    auto m = parse_meminfo(text);
    REQUIRE(m.has_value());
    CHECK(m->first == doctest::Approx(64.0));
    CHECK(m->second == doctest::Approx(32.0));
    CHECK_FALSE(parse_meminfo("MemTotal: 1 kB\n").has_value());
}

TEST_CASE("sysstat: cgroup 上限，max 和 2^63 那种都算没设") {
    CHECK_FALSE(parse_cgroup_limit("max\n").has_value());
    CHECK_FALSE(parse_cgroup_limit("").has_value());
    CHECK_FALSE(parse_cgroup_limit("9223372036854771712\n").has_value());
    CHECK_FALSE(parse_cgroup_limit("abc").has_value());
    auto v = parse_cgroup_limit("2147483648\n");
    REQUIRE(v.has_value());
    CHECK(*v == 2147483648ULL);
}

TEST_CASE("sysstat: nvidia-smi 一张卡一行，MiB 换成 GB，[N/A] 留 -1") {
    const std::string out =
        "0, NVIDIA GeForce RTX 5090, 82, 31500, 32607\n"
        "1, NVIDIA L20, [N/A], 0, 46068\n"
        "\n";
    auto g = parse_nvidia_smi(out);
    REQUIRE(g.size() == 2);
    CHECK(g[0].index == 0);
    CHECK(g[0].name == "NVIDIA GeForce RTX 5090");
    CHECK(g[0].util_percent == 82);
    CHECK(g[0].vram_used_gb == doctest::Approx(31500.0 / 1024));
    CHECK(g[0].vram_total_gb == doctest::Approx(32607.0 / 1024));
    CHECK(g[1].index == 1);
    CHECK(g[1].util_percent == -1);
    CHECK(g[1].vram_used_gb == doctest::Approx(0));
}

TEST_CASE("sysstat: to_json 的形状，前端照这个读") {
    Load l;
    l.cpu_percent = 12.5;
    l.mem_used_gb = 8;
    l.mem_total_gb = 64;
    GpuLoad g;
    g.index = 0;
    g.name = "X";
    g.util_percent = 50;
    g.vram_used_gb = 1;
    g.vram_total_gb = 2;
    l.gpus.push_back(g);
    auto j = to_json(l);
    CHECK(j["cpu_percent"] == 12.5);
    CHECK(j["mem_total_gb"] == 64);
    REQUIRE(j["gpus"].size() == 1);
    CHECK(j["gpus"][0]["util_percent"] == 50);
    CHECK(j["gpus"][0]["name"] == "X");
}

// ---------------------------------------------------------------------------
// 后台采样
// ---------------------------------------------------------------------------
//
// **这一组是补一个实测出来的洞。** 2026-09-11 在服务器上量到：空闲时
// `/api/system` 1 毫秒，大模型一开始生成就变成 13.4 秒，还有一次超过 20 秒
// ——而同一时刻 `/api/health` 始终是 0.6 毫秒。卡在问 NVML 上：显卡满负荷时
// 驱动会把这一下挂住。后果不是"慢一点"：每两秒推一条的那条 WebSocket 用的
// 是同一个采样，它一停，前端八秒的看门狗就把整块表清掉——**表恰恰在最该看
// 的时候空掉**。

TEST_CASE("没起采样器时 latest 当场采，和 sample 一个形状") {
    // 命令行和单元测试走这条。
    const Load l = latest();
    CHECK(l.mem_total_gb >= 0.0);
    // 当场采的不算旧
    CHECK(l.age_s == doctest::Approx(0.0));
}

TEST_CASE("起了采样器之后，读缓存是立刻返回的") {
    start_sampler();
    // 等它采上第一份
    for (int i = 0; i < 100 && latest().age_s == 0.0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // **要点是"不管采样多慢，读都是快的"。** 这里量的是读，不是采。
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        (void)latest();
    }
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took < std::chrono::milliseconds(200));

    stop_sampler();
}

TEST_CASE("停了再起没事，重复起也没事") {
    // 起服务、优雅关停、测试里反复调——都不该留下第二条线程。
    start_sampler();
    start_sampler();   // 已经在跑，应该当场返回
    stop_sampler();
    stop_sampler();    // 没在跑，也不该卡住
    start_sampler();
    stop_sampler();
    CHECK(true);   // 跑到这儿没卡死、没崩就是过了
}

TEST_CASE("age_s 进 JSON：界面靠它把「读不动」和「真没在动」分开") {
    Load l;
    l.age_s = 12.5;
    const auto j = to_json(l);
    REQUIRE(j.contains("age_s"));
    CHECK(j.at("age_s").get<double>() == doctest::Approx(12.5));
}
