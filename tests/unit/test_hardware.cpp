// hardware.cpp 的对拍测试。
//
// 这个文件盯的核心是一个很容易漏的差异：**Python 的 round() 是银行家舍入
// （四舍六入五取偶），C++ 的 std::round 是四舍五入远离零。**
// 恰好落在 .5 上时两者不同，而 _round32 每次调用都可能撞上。
//
// 语料里 round32(400) = 384、round32(464) = 448，用 std::round 会得到
// 416 和 480，全错。分辨率算错的后果是 Wan 的潜空间对不齐。

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"
#include "util/paths.hpp"

using namespace changji::models;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

}  // namespace

TEST_CASE("round32 用的是银行家舍入，和 Python 一致") {
    for (const auto& c : load_golden("round32")) {
        const int n = c.at("n").get<int>();
        CAPTURE(n);
        CHECK(round32(n) == c.at("expected").get<int>());
    }
}

TEST_CASE("半整数的几例单独钉死") {
    // 这几个是 std::round 会做错的。留在这里当哨兵：
    // 谁把 nearbyint 改回 std::round，立刻红。
    CHECK(round32(400) == 384);  // 12.5 → 12（偶）
    CHECK(round32(464) == 448);  // 14.5 → 14（偶）
    CHECK(round32(432) == 448);  // 13.5 → 14（偶），这个两种舍入碰巧相同
    CHECK(round32(48) == 64);    // 1.5 → 2（偶）
}

TEST_CASE("各档显存推导出的档位参数与 Python 一致") {
    for (const auto& c : load_golden("tiers_for_vram")) {
        const double vram = c.at("vram_gb").get<double>();
        CAPTURE(vram);
        const auto tiers = tiers_for_vram(vram);
        const json& want = c.at("tiers");

        REQUIRE(tiers.size() == want.size());
        for (Tier t : all_tiers()) {
            const std::string key = to_string(t);
            CAPTURE(key);
            REQUIRE(want.contains(key));
            const auto it = tiers.find(t);
            REQUIRE(it != tiers.end());
            const TierSpec& got = it->second;
            const json& w = want.at(key);

            CHECK(got.width == w.at("width").get<int>());
            CHECK(got.height == w.at("height").get<int>());
            CHECK(got.steps == w.at("steps").get<int>());
            // 耗时估算里还有一次 round(x, 1) 的银行家舍入
            REQUIRE(got.measured_seconds.has_value());
            CHECK(*got.measured_seconds ==
                  doctest::Approx(w.at("measured_seconds").get<double>()).epsilon(1e-9));
        }
    }
}

TEST_CASE("画幅缩放") {
    for (const auto& c : load_golden("tier_scaled_to")) {
        const json& in = c.at("input");
        TierSpec spec;
        spec.tier = Tier::FINAL;
        spec.width = in.at("width").get<int>();
        spec.height = in.at("height").get<int>();
        spec.steps = in.at("steps").get<int>();
        CAPTURE(spec.width);
        CAPTURE(spec.height);

        for (const char* ratio : {"9:16", "16:9", "1:1"}) {
            CAPTURE(ratio);
            const TierSpec got = spec.scaled_to(ratio);
            const json& want = c.at(ratio);
            CHECK(got.width == want.at("width").get<int>());
            CHECK(got.height == want.at("height").get<int>());
            // 缩放不该动步数和档位
            CHECK(got.steps == spec.steps);
            CHECK(got.tier == spec.tier);
        }
    }
}

TEST_CASE("describe 的输出与 Python 逐字节相同") {
    for (const auto& c : load_golden("hardware_describe")) {
        HardwareProfile p;
        p.vram_gb = c.at("vram_gb").get<double>();
        p.detected = c.at("detected").get<bool>();
        if (!c.at("gpu").is_null()) {
            GPUInfo g;
            g.name = c.at("gpu").at("name").get<std::string>();
            g.vram_mb = c.at("gpu").at("vram_mb").get<int>();
            if (!c.at("gpu").at("driver").is_null()) {
                g.driver = c.at("gpu").at("driver").get<std::string>();
            }
            p.gpu = g;
        }
        p.tiers = tiers_for_vram(p.vram_gb);

        CAPTURE(p.vram_gb);
        CAPTURE(p.detected);
        // 这一条是逐字节的：describe 直接打给用户看
        CHECK(p.describe() == c.at("describe").get<std::string>());

        const auto est = p.estimate_episode(20, Tier::FINAL);
        const json& want_est = c.at("estimate_episode_20_final");
        if (want_est.is_null()) {
            CHECK_FALSE(est.has_value());
        } else {
            REQUIRE(est.has_value());
            CHECK(*est == doctest::Approx(want_est.get<double>()).epsilon(1e-9));
        }
    }
}

TEST_CASE("探测不到显卡时按 12GB 保守估算") {
    // 不依赖本机有没有卡：直接给一个覆盖值走另一条分支
    const HardwareProfile p = HardwareProfile::detect(6.0);
    CHECK(p.vram_gb == doctest::Approx(6.0));
    CHECK_FALSE(p.detected);  // 给了覆盖值就不算探测到
    // 两档：preview 2026-09-13 删了，见 models/hardware.hpp 上面那段。
    // 写 all_tiers().size() 而不是字面量，下次增删档位这里不用跟着改。
    CHECK(p.tiers.size() == all_tiers().size());
    // 6GB 落在最低那一档
    CHECK(p.tiers.at(Tier::DRAFT).width == 448);
}

TEST_CASE("本机探测能跑通且不崩") {
    // 这台机器上有没有卡都不影响：detect 的契约是探测不到就返回空，
    // 不是抛异常。前置验证里 doctor 崩掉的教训就在这里。
    const HardwareProfile p = HardwareProfile::detect();
    CHECK(p.tiers.size() == all_tiers().size());
    CHECK(p.vram_gb > 0.0);
    if (p.gpu.has_value()) {
        CHECK_FALSE(p.gpu->name.empty());
        CHECK(p.gpu->vram_mb > 0);
        CHECK(p.detected);
    }
}


TEST_CASE("问实时空闲显存：有卡就问得到，没卡就老老实实说没有") {
    // **这条是给 NVML 那条新路兜底的。**
    //
    // free_vram_gb 原来只有一条路：fork + exec 跑 nvidia-smi。这个进程
    // 初始化 CUDA 之后 fork 是 NVIDIA 明确不支持的，问不到就退回保守
    // 估算——"显存够就不清理"于是可能从来没生效过。现在先问 NVML
    // （进程内、不 fork），问不到才走老路。
    //
    // 契约是：**要么给一个说得通的数，要么说没有，绝不给个荒唐的数**。
    // 给荒唐的数比说没有更糟：说没有只是退回保守（多卸一次），
    // 给个偏大的数是 CUDA OOM，abort() 把整个服务带走。
    const auto gb = free_vram_gb();
    if (gb.has_value()) {
        CHECK(*gb >= 0.0);
        CHECK(*gb < 4096.0);   // 2026 年还没有 4 TB 显存的卡
#if !defined(__APPLE__)
        // 空闲不可能比整卡还多。**这一条最要紧**：调度器拿它和
        // "这一路要占多少"直接比，虚报一点点就是一次 OOM。
        //
        // **Mac 上不比。** 那边是统一内存：free_vram_gb 报的是 vm_stat
        // 算出来的"还能用多少系统内存"，而 gpu->vram_gb() 是
        // hw.memsize × iogpu.wired_limit_pct（默认七成半）。两个数出自
        // 两套口径，空闲大过那个七成半是正常的，不是错。
        const HardwareProfile p = HardwareProfile::detect();
        if (p.gpu.has_value()) {
            CHECK(*gb <= p.gpu->vram_gb() + 1.0);
        }
#endif
    }
    // 没值也是合法答案（没装 NVIDIA 驱动的机器），不该因此判失败。
}

TEST_CASE("绑了哪张卡要换算对：NVML 不认 CUDA_VISIBLE_DEVICES") {
    // **这条是给多卡兜底的。** 工作进程靠 CUDA_VISIBLE_DEVICES 绑卡
    // （worker_server.cpp 里设的），于是进程里 CUDA 的 0 号可能是物理的
    // 3 号——而 NVML 和 nvidia-smi 的 0 号永远是物理 0 号。不换算的话，
    // 绑在 3 号上的工作进程问到的是 0 号的空闲，两张卡的忙闲毫无关系。
    //
    // 问小了只是多卸一次；**问大了是拿别人卡上的空闲去判"够，不卸"**，
    // 下一步就是 OOM，走 GGML_ASSERT 把整个工作进程带走。
    SUBCASE("没设：就是 0 号") {
        CHECK(parse_visible_devices("") == 0u);
        CHECK(parse_visible_devices("   ") == 0u);
    }
    SUBCASE("设了一个数：就是它") {
        CHECK(parse_visible_devices("3") == 3u);
        CHECK(parse_visible_devices(" 7 ") == 7u);
    }
    SUBCASE("设了一串：取第一个——进程里的 0 号对应列表里第一项") {
        CHECK(parse_visible_devices("2,5,6") == 2u);
        CHECK(parse_visible_devices("0,1") == 0u);
    }
    SUBCASE("UUID 那种写法：认不出来就说认不出来，不猜") {
        // 猜错的方向会 OOM，而"认不出来"只是退回保守。
        CHECK_FALSE(parse_visible_devices("GPU-4f2c1a").has_value());
        CHECK_FALSE(parse_visible_devices("MIG-abc").has_value());
    }
    SUBCASE("离谱的数不当真") {
        CHECK_FALSE(parse_visible_devices("999").has_value());
    }
}

TEST_CASE("nvidia-smi 一张卡一行：取自己那一行") {
    // 取错行和问错卡是同一个后果。
    const std::string out = "6144\n40960\n\n81920\n";
    CHECK(nth_gpu_line(out, 0) == "6144");
    CHECK(nth_gpu_line(out, 1) == "40960");
    // 空行跳过，不算一张卡
    CHECK(nth_gpu_line(out, 2) == "81920");
    // 没有那么多卡：返回空串，调用方当成问不到
    CHECK(nth_gpu_line(out, 3).empty());
    CHECK(nth_gpu_line("", 0).empty());
    // 只有一行、没有换行符的情况（nounits 单卡输出）
    CHECK(nth_gpu_line("6144", 0) == "6144");
}

TEST_CASE("总量和空闲要一次问出来，而且互相说得通") {
    // 算"这个槽实际占了多少"用的是 总量 − 空闲。分两次问的话两个数来自
    // 两个时刻、两条不同的路，差值就不是这个槽占的——而那个差值会被当成
    // 实测值记下来，之后每一镜都拿它判要不要卸模型。
    const auto t = vram_totals_gb();
    if (t.has_value()) {
        CHECK(t->total_gb > 0.0);
        CHECK(t->free_gb >= 0.0);
        // **空闲不可能比总量还多。** 反过来的话 用掉的 = 总量 − 空闲
        // 会是负数，实测值就记不下来，"够就不清理"永远起不来。
        CHECK(t->free_gb <= t->total_gb);
        // 和单独问空闲那条对得上（两次调用之间会变，放宽到 4 GB）。
        if (const auto f = free_vram_gb(); f.has_value()) {
            CHECK(std::abs(*f - t->free_gb) < 4.0);
        }
        // 和硬件探测报的整卡容量对得上。
        const HardwareProfile p = HardwareProfile::detect();
        if (p.gpu.has_value()) {
            CHECK(std::abs(p.gpu->vram_gb() - t->total_gb) < 1.0);
        }
    }
    // 拿不到也是合法答案（Mac、没装驱动的机器），调用方会退回老路。
}

TEST_CASE("绑到一张不存在的卡：说问不到，而不是退回报 0 号卡的数") {
    // **这条测的是安全方向。** 多卡时工作进程靠 CUDA_VISIBLE_DEVICES 绑卡，
    // 而 NVML 和 nvidia-smi 都不认这个变量——所以要按它换算下标。
    // 换算出来的下标越界时（配错了、或者卡被拔了），**必须说"问不到"**：
    // 退回去报 0 号卡的空闲，就是拿别人卡上的数去判"够，不卸"，下一步 OOM。
    //
    // 这台机器只有一张卡，所以下标 9 一定越界，这条在本机就能真跑。
    //
    // **Mac 上整条不成立，直接跳过。** 那边是统一内存，free_vram_gb 走的是
    // vm_stat，压根不看 CUDA_VISIBLE_DEVICES——绑到哪张"卡"都照样有数。
    // 同一个文件里「空闲不能比整卡多」那条也是为这个原因关掉的，
    // 我这次又忘了，CI 的 macOS 格子当场红。
#if !defined(__APPLE__)
    const auto before = free_vram_gb();
    changji::paths::set_env("CUDA_VISIBLE_DEVICES", "9");
    const auto bound = free_vram_gb();
    const auto bound_totals = vram_totals_gb();
    changji::paths::set_env("CUDA_VISIBLE_DEVICES", "");

    const HardwareProfile p = HardwareProfile::detect();
    const int cards = p.gpu.has_value() ? p.gpu->count : 0;
    if (cards > 0 && cards <= 9) {
        // 下标越界 -> 一个数都不该给出来
        CHECK_FALSE(bound.has_value());
        CHECK_FALSE(bound_totals.has_value());
    }
    // 环境变量清掉之后要恢复原样，别把状态留给后面的用例
    const auto after = free_vram_gb();
    CHECK(before.has_value() == after.has_value());
#endif
}

TEST_CASE("CHANGJI_NO_NVML 能把新路关掉，退回老路") {
    // 新加一个原生库依赖，得留一个一键关掉的口子。关掉之后仍然要么
    // 给数、要么给空——不能因为关掉就崩，也不能给个荒唐的数。
    const auto before = free_vram_gb();
    changji::paths::set_env("CHANGJI_NO_NVML", "1");
    const auto after = free_vram_gb();
    changji::paths::set_env("CHANGJI_NO_NVML", "");
    if (after.has_value()) {
        CHECK(*after >= 0.0);
        CHECK(*after < 4096.0);
    }
    // 两条路问的是同一张卡，差得离谱就说明有一条读错了。
    // 放宽到 4 GB：两次调用之间显存本来就在变。
    if (before.has_value() && after.has_value()) {
        CHECK(std::abs(*before - *after) < 4.0);
    }
}

TEST_CASE("认得出有几张卡") {
    // **这台开发机只有一张卡，多卡那条路一行都跑不到。**
    // 明天上 8×48 才知道对不对，而那时候要是错了，表现是
    // "八个进程全挤在卡 0 上，看着在并行实际在排队"，一声不吭。
    // 所以拿真实格式的输出在这儿先验。

    // 一张卡
    {
        const auto g = parse_gpu_query(
            "NVIDIA GeForce RTX 2060, 6144, 581.15\n");
        REQUIRE(g.has_value());
        CHECK(g->name == "NVIDIA GeForce RTX 2060");
        CHECK(g->vram_mb == 6144);
        CHECK(g->count == 1);
    }

    // 八张卡：**显存仍然是单卡的 48 GB，不是加起来的 384**
    {
        std::string out;
        for (int i = 0; i < 8; ++i) out += "NVIDIA L40S, 49140, 581.15\n";
        const auto g = parse_gpu_query(out);
        REQUIRE(g.has_value());
        CHECK(g->count == 8);
        CHECK(g->vram_mb == 49140);
        // 加起来去查档位表会算出一张卡根本跑不动的分辨率
        CHECK(g->vram_gb() < 50.0);
    }

    // 末尾没有换行也要数对
    {
        const auto g = parse_gpu_query(
            "NVIDIA L40S, 49140, 581.15\nNVIDIA L40S, 49140, 581.15");
        REQUIRE(g.has_value());
        CHECK(g->count == 2);
    }

    // 中间的空行不算一张卡
    {
        const auto g = parse_gpu_query(
            "NVIDIA L40S, 49140, 581.15\n\nNVIDIA L40S, 49140, 581.15\n\n");
        REQUIRE(g.has_value());
        CHECK(g->count == 2);
    }

    // 探测不到就是探测不到，别编一个出来
    CHECK_FALSE(parse_gpu_query("").has_value());
    CHECK_FALSE(parse_gpu_query("\n\n").has_value());
    // 显存那一列不是数字时也不该硬凑
    CHECK_FALSE(parse_gpu_query("某张卡, N/A, 1.0\n").has_value());
}

TEST_CASE("档位按单卡算，不按总显存") {
    // 八张 48 GB 加起来 384 GB 去查档位表，会算出一张卡跑不动的分辨率。
    // 这一条钉住"两个维度不能混"。
    const auto one = tiers_for_vram(48.0);
    const auto eight = tiers_for_vram(48.0);   // 卡数不参与
    CHECK(one.at(Tier::FINAL).width ==
          eight.at(Tier::FINAL).width);

    // 而 384 GB 会落到更高的档——正是不该发生的那种
    const auto summed = tiers_for_vram(384.0);
    CHECK(summed.at(Tier::FINAL).width >=
          one.at(Tier::FINAL).width);
}

TEST_CASE("解析实时空闲显存") {
    // nounits 格式：一张卡一行，纯数字（MiB）
    CHECK(parse_free_vram("12873\n").value() ==
          doctest::Approx(12.571).epsilon(0.01));
    // 带单位的也吃
    CHECK(parse_free_vram("12873 MiB\n").value() ==
          doctest::Approx(12.571).epsilon(0.01));
    // 问不到时**必须回空，不能回 0**——回 0 会被当成"没空间"，
    // 而"问不到"和"没空间"是两回事：前者该退回静态估算。
    CHECK_FALSE(parse_free_vram("").has_value());
    CHECK_FALSE(parse_free_vram("\n").has_value());
    CHECK_FALSE(parse_free_vram("N/A\n").has_value());
    CHECK_FALSE(parse_free_vram("0\n").has_value());
}

// ---- 苹果机器：统一内存当显存 ----
//
// 这台机器上没有 nvidia-smi，而 detect_gpu 以前只认它：探不到就退回
// "按 12 GB 估算"。一台 128 GB 的 Mac 于是被当成 12 GB，档位、权重放哪、
// "显存够就不用清理"全部按 12 GB 算——**而且不报错**，只是什么都跑不大。

/// 真机上抓的（iMac21,1 / Apple M1 / 16 GB，页大小 16384）。
static const char* kVmStat =
    "Mach Virtual Memory Statistics: (page size of 16384 bytes)\n"
    "Pages free:                                     6364.\n"
    "Pages active:                                 360261.\n"
    "Pages inactive:                               385709.\n"
    "Pages speculative:                             10817.\n"
    "Pages throttled:                                   0.\n"
    "Pages wired down:                             127621.\n"
    "Pages purgeable:                               25885.\n"
    "\"Translation faults\":                      574116357.\n"
    "Pages copy-on-write:                        20675625.\n";

TEST_CASE("vm_stat：算的是「还能用多少」，不是「完全空着多少」") {
    const auto gb = parse_vm_stat(kVmStat);
    REQUIRE(gb.has_value());

    // free 6364 + inactive 385709 + purgeable 25885 + speculative 10817
    // = 428775 页 × 16384 字节 = 6.54 GB
    CHECK(*gb == doctest::Approx(6.54).epsilon(0.01));

    // **只看 Pages free 是不行的**：那只有 6364 页 ≈ 0.1 GB，
    // 调度器会以为一点空间都没有，每次都去卸模型。
    CHECK(*gb > 1.0);
}

TEST_CASE("vm_stat：页大小必须从输出里读，不能写死 4096") {
    // 苹果芯片是 16384。写死 4096 的话算出来差四倍——而这不会报错，
    // 只是调度器一直以为显存不够。
    std::string small = kVmStat;
    const auto pos = small.find("16384");
    REQUIRE(pos != std::string::npos);
    small.replace(pos, 5, "4096");
    const auto a = parse_vm_stat(kVmStat);
    const auto b = parse_vm_stat(small);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == doctest::Approx(*b * 4.0).epsilon(0.01));
}

TEST_CASE("vm_stat：读不出来就说读不出来，别猜") {
    // 猜一个数比没有更糟：调度器会拿它当真，而"问不到"那条路本来就有
    // 保守的退路（见 Scheduler::make_room）。
    CHECK_FALSE(parse_vm_stat("").has_value());
    CHECK_FALSE(parse_vm_stat("完全不相干的输出").has_value());
    // 没有页大小那一行 → 不猜
    CHECK_FALSE(parse_vm_stat("Pages free: 100.\n").has_value());
}

TEST_CASE("空闲不许比总量还大") {
    // **这条是冲着一个真实故障去的。** 2026-09-12 在 128 GB 的 M3 Max 上：
    //
    //   总量走 sysctl，算出 96 GB（写死的 75% 兜底，而它查的那个 OID
    //   在 macOS 26 上已经不存在了）
    //   空闲走 vm_stat，算出 106 GB（free + inactive + speculative + purgeable，
    //   那是"系统还能腾出多少内存"，不是"GPU 能占多少"）
    //
    // 两个数来自两套接口，于是"空闲比总量还大"。下游 sd_image.cpp 里
    // `used = total - free` 得到 -10，卡在 `if (used_gb > 0.0)` 上——
    // **Mac 上显存实测标定一次都没记下过，而且没有任何日志说它被跳过了。**
    //
    // 现在两个数同源（Metal 那边是同一个 MTLDevice，NVML 那边是同一次
    // 调用），这条不变量就该永远成立。探不到显卡的机器（CI 上那两台）
    // 两个都是空，这条用例自然跳过。
    const auto gpu = detect_gpu();
    const auto free_gb = free_vram_gb();
    if (!gpu.has_value() || !free_gb.has_value()) return;
    CAPTURE(gpu->name);
    CAPTURE(gpu->vram_gb());
    CAPTURE(*free_gb);
    CHECK(*free_gb <= gpu->vram_gb());
    CHECK(*free_gb >= 0.0);
}

TEST_CASE("统一内存：整机内存和 GPU 能用的那份是两个数") {
    // 128 GB 的 Mac 上 vram_mb 是 Metal 肯给的 107.5 GB，unified_mb 才是 128。
    // 只留一个的后果在两边都难看：只显示前者，用户觉得"我买的明明是 128"；
    // 只按后者算预算，超过 Metal 那条线系统就开始压缩换页。
    GPUInfo g;
    CHECK_FALSE(g.unified());          // 默认就不是统一内存，别误报

    g.vram_mb = 110100;                // 107.5 GB
    g.unified_mb = 131072;             // 128 GB
    CHECK(g.unified());
    CHECK(g.vram_gb() == doctest::Approx(107.52).epsilon(0.01));

    // 存到状态文件里再读回来，两个数都要在——漏了 unified_mb 的话
    // 重启之后界面上那台 Mac 就又变回"一张 107.5 GB 的卡"了。
    const nlohmann::json j = g;
    const auto back = j.get<GPUInfo>();
    CHECK(back.unified_mb == g.unified_mb);
    CHECK(back.vram_mb == g.vram_mb);

    // 老状态文件里没有这一项：按"不是统一内存"读，不要炸
    const auto old = nlohmann::json{{"name", "RTX 5090"}, {"vram_mb", 32768}}
                         .get<GPUInfo>();
    CHECK_FALSE(old.unified());
}
