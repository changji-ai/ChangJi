// 跨模型显存调度的测试。
//
// 这里测的是**策略**，不是推理。加载和卸载都是假的回调，只记账。
//
// 之所以刻意做成这样：接上真模型之后，每个用例要读几个 GB、跑几分钟，
// 策略层的 bug 就没法反复撞了。而策略层恰恰是最容易出错的地方——
// 引用计数、驱逐顺序、加载失败后的回滚，这三样错了都不会当场报错，
// 只会在跑到一半时以 OOM 或者段错误的形式出现。

#include <doctest/doctest.h>

#include <optional>
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "infer/scheduler.hpp"
#include "util/say.hpp"

using namespace changji::infer;

namespace {

constexpr std::size_t GB = 1024ull * 1024 * 1024;

/// 记账用的假模型。
struct FakeModel {
    std::atomic<int> loads{0};
    std::atomic<int> unloads{0};
    std::atomic<bool> resident{false};
    bool fail_next_load = false;
};

SlotSpec spec_for(Slot slot, FakeModel& m, std::size_t bytes, int priority,
                  Residency r = Residency::Cached) {
    SlotSpec s;
    s.slot = slot;
    s.residency = r;
    s.vram_estimate = bytes;
    s.evict_priority = priority;
    s.load = [&m] {
        if (m.fail_next_load) {
            m.fail_next_load = false;
            throw std::runtime_error("假装加载失败");
        }
        ++m.loads;
        m.resident = true;
    };
    s.unload = [&m] {
        ++m.unloads;
        m.resident = false;
    };
    return s;
}

}  // namespace

TEST_CASE("借出时才加载，不借不加载") {
    Scheduler s;
    FakeModel llm;
    s.register_slot(spec_for(Slot::LLM, llm, 9 * GB, 0));

    // 注册不等于加载。九个 GB 的东西不该因为"登记了一下"就读进来。
    CHECK(llm.loads == 0);
    CHECK_FALSE(s.loaded(Slot::LLM));

    {
        auto lease = s.acquire(Slot::LLM);
        CHECK(lease.valid());
        CHECK(llm.loads == 1);
        CHECK(s.loaded(Slot::LLM));
        CHECK(s.lease_count(Slot::LLM) == 1);
    }
    // Cached 的槽还回来之后仍然留着
    CHECK(s.lease_count(Slot::LLM) == 0);
    CHECK(s.loaded(Slot::LLM));
    CHECK(llm.unloads == 0);
}

TEST_CASE("重复借用只加载一次") {
    Scheduler s;
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    auto a = s.acquire(Slot::Video);
    auto b = s.acquire(Slot::Video);
    auto c = s.acquire(Slot::Video);
    CHECK(video.loads == 1);
    CHECK(s.lease_count(Slot::Video) == 3);

    a.release();
    CHECK(s.lease_count(Slot::Video) == 2);
    CHECK(s.loaded(Slot::Video));
}

TEST_CASE("用完就放的槽在最后一个借用还回来时卸载") {
    // LLM 一章只用一次。留着纯粹占地方——它按定义不会再被用了，
    // 而那是九个 GB。
    Scheduler s;
    FakeModel llm;
    s.register_slot(spec_for(Slot::LLM, llm, 9 * GB, 0, Residency::Ephemeral));

    {
        auto a = s.acquire(Slot::LLM);
        auto b = s.acquire(Slot::LLM);
        CHECK(llm.resident);
        a.release();
        // 还有一个借用者，不能卸
        CHECK(llm.resident);
        CHECK(llm.unloads == 0);
    }
    CHECK_FALSE(llm.resident);
    CHECK(llm.unloads == 1);
}

TEST_CASE("装不下时按优先级驱逐") {
    Scheduler s;
    s.set_budget(6 * GB);

    FakeModel llm, image, video;
    // 优先级：数字小的先被驱逐。重新加载越贵的数字越大。
    s.register_slot(spec_for(Slot::LLM, llm, 2 * GB, 0));
    s.register_slot(spec_for(Slot::Image, image, 2 * GB, 5));
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    s.acquire(Slot::LLM).release();
    s.acquire(Slot::Image).release();
    CHECK(s.resident_bytes() == 4 * GB);

    // 再来 3 GB 就超了 6 GB，要驱逐。LLM 优先级最低，先走。
    {
        auto v = s.acquire(Slot::Video);
        CHECK_FALSE(s.loaded(Slot::LLM));
        CHECK(s.loaded(Slot::Image));   // 优先级更高，保住
        CHECK(s.loaded(Slot::Video));
        CHECK(llm.unloads == 1);
    }
    CHECK(s.resident_bytes() == 5 * GB);
}

TEST_CASE("正在被借用的槽绝不驱逐") {
    // 正在用的模型被抽走，表现是段错误，不是一个能读的报错。
    // 宁可这次加载失败。
    Scheduler s;
    s.set_budget(4 * GB);

    FakeModel llm, video;
    s.register_slot(spec_for(Slot::LLM, llm, 3 * GB, 0));    // 优先级最低
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    auto held = s.acquire(Slot::LLM);   // 一直拿着不放

    // LLM 虽然优先级最低，但正被借用，不能动。于是视频加载不进来。
    CHECK_THROWS_AS(s.acquire(Slot::Video), std::runtime_error);
    CHECK(s.loaded(Slot::LLM));
    CHECK_FALSE(s.loaded(Slot::Video));
    CHECK(llm.unloads == 0);

    // 放开之后就能加载了
    held.release();
    {
        auto v = s.acquire(Slot::Video);
        CHECK(s.loaded(Slot::Video));
        CHECK_FALSE(s.loaded(Slot::LLM));
    }
}

TEST_CASE("同优先级时最久没用的先走") {
    Scheduler s;
    s.set_budget(4 * GB);

    FakeModel a, b, c;
    s.register_slot(spec_for(Slot::LLM, a, 2 * GB, 5));
    s.register_slot(spec_for(Slot::Image, b, 2 * GB, 5));
    s.register_slot(spec_for(Slot::Video, c, 2 * GB, 5));

    s.acquire(Slot::LLM).release();     // 最早
    s.acquire(Slot::Image).release();
    // 再碰一下 LLM，让它变成"最近用过"
    s.acquire(Slot::LLM).release();

    {
        auto v = s.acquire(Slot::Video);
        // Image 是最久没用的，它先走，不是 LLM
        CHECK_FALSE(s.loaded(Slot::Image));
        CHECK(s.loaded(Slot::LLM));
    }
}

TEST_CASE("预算为零表示不限制") {
    Scheduler s;   // 没设预算
    FakeModel a, b, c, d;
    s.register_slot(spec_for(Slot::LLM, a, 100 * GB, 0));
    s.register_slot(spec_for(Slot::Image, b, 100 * GB, 1));
    s.register_slot(spec_for(Slot::Video, c, 100 * GB, 2));
    s.register_slot(spec_for(Slot::TTS, d, 100 * GB, 3));

    s.acquire(Slot::LLM).release();
    s.acquire(Slot::Image).release();
    s.acquire(Slot::Video).release();
    s.acquire(Slot::TTS).release();

    CHECK(s.loaded_slots().size() == 4);
    CHECK(s.resident_bytes() == 400 * GB);
}

TEST_CASE("加载失败要把账退干净") {
    // 退不干净的后果是：调度器以为那个槽加载着，
    // 后面的预算计算全错，而且永远不会再去加载它。
    Scheduler s;
    s.set_budget(6 * GB);
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));
    video.fail_next_load = true;

    CHECK_THROWS_AS(s.acquire(Slot::Video), std::runtime_error);
    CHECK_FALSE(s.loaded(Slot::Video));
    CHECK(s.lease_count(Slot::Video) == 0);
    CHECK(s.resident_bytes() == 0);

    SUBCASE("退干净之后还能重试") {
        auto lease = s.acquire(Slot::Video);
        CHECK(s.loaded(Slot::Video));
        CHECK(video.loads == 1);
    }
}

TEST_CASE("加载失败不会把被驱逐的槽自动加载回来") {
    // 自动回滚会把一次失败变成一串连锁加载，而且日志里看不出源头。
    Scheduler s;
    s.set_budget(4 * GB);
    FakeModel llm, video;
    s.register_slot(spec_for(Slot::LLM, llm, 3 * GB, 0));
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    s.acquire(Slot::LLM).release();
    CHECK(s.loaded(Slot::LLM));

    video.fail_next_load = true;
    CHECK_THROWS(s.acquire(Slot::Video));

    // LLM 已经被驱逐了，不会自己回来
    CHECK_FALSE(s.loaded(Slot::LLM));
    CHECK(llm.loads == 1);
    CHECK(llm.unloads == 1);
}

TEST_CASE("evict 不强卸正在用的") {
    Scheduler s;
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    auto held = s.acquire(Slot::Video);
    CHECK(s.evict(Slot::Video) == false);
    CHECK(s.loaded(Slot::Video));

    held.release();
    CHECK(s.evict(Slot::Video) == true);
    CHECK_FALSE(s.loaded(Slot::Video));

    // 已经卸了再卸一次不报错
    CHECK(s.evict(Slot::Video) == true);
    CHECK(video.unloads == 1);
}

TEST_CASE("evict_all 放掉所有没被借用的") {
    // 跑完一章要把显存交回去，别的程序还等着用。
    Scheduler s;
    FakeModel a, b, c;
    s.register_slot(spec_for(Slot::LLM, a, 1 * GB, 0));
    s.register_slot(spec_for(Slot::Image, b, 1 * GB, 1));
    s.register_slot(spec_for(Slot::Video, c, 1 * GB, 2));

    s.acquire(Slot::LLM).release();
    s.acquire(Slot::Image).release();
    auto held = s.acquire(Slot::Video);

    s.evict_all();
    CHECK_FALSE(s.loaded(Slot::LLM));
    CHECK_FALSE(s.loaded(Slot::Image));
    CHECK(s.loaded(Slot::Video));      // 还借着，不动
}

TEST_CASE("没注册的槽借不出来") {
    Scheduler s;
    CHECK_THROWS_AS(s.acquire(Slot::Video), std::runtime_error);
}

TEST_CASE("已加载的槽不能重新注册") {
    // 覆盖之后新的 unload 会去卸一个不是它加载的东西。
    // 让它响亮地失败，比留一个对不上的账好。
    Scheduler s;
    FakeModel a, b;
    s.register_slot(spec_for(Slot::Video, a, 1 * GB, 0));
    auto held = s.acquire(Slot::Video);
    CHECK_THROWS_AS(s.register_slot(spec_for(Slot::Video, b, 2 * GB, 0)),
                    std::runtime_error);

    held.release();
    s.evict(Slot::Video);
    CHECK_NOTHROW(s.register_slot(spec_for(Slot::Video, b, 2 * GB, 0)));
}

TEST_CASE("析构时把还加载着的都卸掉") {
    FakeModel llm, video;
    {
        Scheduler s;
        s.register_slot(spec_for(Slot::LLM, llm, 1 * GB, 0));
        s.register_slot(spec_for(Slot::Video, video, 1 * GB, 1));
        s.acquire(Slot::LLM).release();
        s.acquire(Slot::Video).release();
        CHECK(llm.resident);
        CHECK(video.resident);
    }
    CHECK_FALSE(llm.resident);
    CHECK_FALSE(video.resident);
}

TEST_CASE("Lease 可以移动") {
    Scheduler s;
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 1 * GB, 0,
                             Residency::Ephemeral));

    {
        Lease outer;
        {
            Lease inner = s.acquire(Slot::Video);
            CHECK(inner.valid());
            outer = std::move(inner);
            // 移走之后原来那个不该再归还一次——归还两次会把引用计数弄成负的，
            // 然后模型在还有人用的时候被卸掉
            CHECK_FALSE(inner.valid());
        }
        // inner 析构了，但借用还在 outer 手里
        CHECK(s.lease_count(Slot::Video) == 1);
        CHECK(video.resident);
    }
    CHECK(s.lease_count(Slot::Video) == 0);
    CHECK_FALSE(video.resident);   // Ephemeral，还回来就卸
}

TEST_CASE("多线程同时借同一个槽，只加载一次") {
    // 两条线程同时发现"没加载"然后都去加载，会读两遍几个 GB，
    // 而且第二次覆盖第一次的指针，第一次那份就泄漏了。
    for (int round = 0; round < 20; ++round) {
        CAPTURE(round);
        Scheduler s;
        FakeModel video;
        auto spec = spec_for(Slot::Video, video, 1 * GB, 0);
        // 让加载慢一点，把竞态窗口撑开
        spec.load = [&video] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ++video.loads;
            video.resident = true;
        };
        s.register_slot(std::move(spec));

        std::vector<std::thread> ts;
        std::atomic<int> ok{0};
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([&] {
                try {
                    auto lease = s.acquire(Slot::Video);
                    ++ok;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } catch (...) {
                }
            });
        }
        for (auto& t : ts) t.join();

        CHECK(ok.load() == 8);
        CHECK(video.loads == 1);
        CHECK(s.lease_count(Slot::Video) == 0);
    }
}

TEST_CASE("按阶段分批比按镜头串行少换很多次模型") {
    // 这条不是测代码，是把方案里那条结论钉下来：
    // 跑一章的正确顺序是"四十个镜头的首帧全出完，再统一出视频"，
    // 不是"一个镜头出首帧再出视频，然后下一个"。
    //
    // 预算只装得下一个模型时，两种顺序的换入换出次数差四十倍。
    constexpr int kShots = 40;

    auto count_swaps = [](bool batched) {
        Scheduler s;
        s.set_budget(3 * GB);      // 只装得下一个
        FakeModel image, video;
        s.register_slot(spec_for(Slot::Image, image, 3 * GB, 5));
        s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

        if (batched) {
            for (int i = 0; i < kShots; ++i) s.acquire(Slot::Image).release();
            for (int i = 0; i < kShots; ++i) s.acquire(Slot::Video).release();
        } else {
            for (int i = 0; i < kShots; ++i) {
                s.acquire(Slot::Image).release();
                s.acquire(Slot::Video).release();
            }
        }
        return image.loads + video.loads;
    };

    const int batched = count_swaps(true);
    const int serial = count_swaps(false);

    CHECK(batched == 2);              // 一共就加载两次
    CHECK(serial == kShots * 2);      // 每镜换两次
    CHECK(serial > batched * 30);
}

TEST_CASE("显存真空着的时候别瞎卸模型") {
    // vram_estimate 是按整份预算估的（"同时只装得下一个"），那是保守的：
    // 权重放内存时显存里只有计算缓冲，两个槽同时在也没事。只信估算的话，
    // 每次切阶段都要卸一个再装一个，而一次重装是几十秒到几分钟。
    Scheduler s;
    s.set_budget(10ull << 30);          // 预算 10 GB

    int image_unloads = 0;
    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = 8ull << 30;     // 估 8 GB
    img.load = [] {};
    img.unload = [&image_unloads] { ++image_unloads; };
    s.register_slot(img);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = 8ull << 30;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    { auto lease = s.acquire(Slot::Image); }   // 装上图像槽
    CHECK(image_unloads == 0);

    SUBCASE("卡上真的空着 20 GB：不该卸") {
        s.set_free_vram_probe([] { return std::optional<double>(20.0); });
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 0);
    }
    SUBCASE("卡上只剩 1 GB：照旧按估算卸") {
        s.set_free_vram_probe([] { return std::optional<double>(1.0); });
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 1);
    }
    SUBCASE("问不到就退回估算——**问不到不等于有空间**") {
        s.set_free_vram_probe([] { return std::optional<double>{}; });
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 1);
    }
    SUBCASE("没装探针时和以前一样") {
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 1);
    }
}

TEST_CASE("量过一个更小的活：按比例放大再判，别退回估算") {
    // 2026-09-13 q4_full 实见：73 帧的镜头量到 24 GB，下一镜 106 帧，
    // "量过的活不够大"于是退回估算，估算比卡上剩的 9.5 GB 还小，判成
    // 够、不卸，sd.cpp 当场 cannot make enough memory。
    Scheduler s;
    s.set_budget(10ull << 30);

    int image_unloads = 0;
    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = 8ull << 30;
    img.load = [] {};
    img.unload = [&image_unloads] { ++image_unloads; };
    s.register_slot(img);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = 8ull << 30;
    // 静态估算故意给个偏小的数（5 GB）：正是它把人骗了
    vid.live_vram = [] { return std::size_t(5ull << 30); };
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    { auto lease = s.acquire(Slot::Image); }
    // 出片在活=100 的镜头上量到 20 GB
    s.record_measured_vram(Slot::Video, 20ull << 30, 100);
    // 卡上剩 25 GB
    s.set_free_vram_probe([] { return std::optional<double>(25.0); });

    SUBCASE("这次的活不比量过的大：20 ≤ 25，不卸") {
        Scheduler::AcquireOptions opt;
        opt.work = 100;
        { auto lease = s.acquire(Slot::Video, opt); }
        CHECK(image_unloads == 0);
    }
    SUBCASE("这次的活大一半：20 × 1.5 = 30 > 25，要卸——不能退回那个 5 GB 的估算") {
        Scheduler::AcquireOptions opt;
        opt.work = 150;
        { auto lease = s.acquire(Slot::Video, opt); }
        CHECK(image_unloads == 1);
    }
}

TEST_CASE("槽装着、峰值也量过，中间别的槽装上来了：回头再借要重新问卡") {
    // 2026-09-13 q4_full sh004：出片槽独占时量到 24.9 GB；首帧阶段把
    // 16 GB 的图像模型装上；回头出片，槽装着、量过的活也够大，一次判断
    // 都不做，卡上只剩 9.5 GB，sd.cpp 当场 cannot make enough memory。
    Scheduler s;
    // 预算 20 GB：两个槽各估 8 GB，都装着时再借就超预算，走到"问卡"那一步
    // （静态账 ≤ 预算时 make_room 直接放行，那条路不问卡）。
    s.set_budget(20ull << 30);

    int image_unloads = 0;
    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = 8ull << 30;
    img.evict_priority = 1;
    img.load = [] {};
    img.unload = [&image_unloads] { ++image_unloads; };
    s.register_slot(img);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = 8ull << 30;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    Scheduler::AcquireOptions opt;
    opt.work = 100;
    double free_gb = 26.0;
    s.set_free_vram_probe([&free_gb] { return std::optional<double>(free_gb); });

    { auto v = s.acquire(Slot::Video, opt); }          // 出片槽装上、独占
    s.record_measured_vram(Slot::Video, 24ull << 30, 100);   // 量到 24 GB
    { auto i = s.acquire(Slot::Image); }               // 首帧阶段把图像装上
    CHECK(image_unloads == 0);

    free_gb = 9.5;   // 图像模型占掉了一大块
    { auto v = s.acquire(Slot::Video, opt); }          // 回头出片：槽是装着的
    // 24 > 9.5：必须把图像模型卸掉，不能因为"装着、量过"就放行
    CHECK(image_unloads == 1);
}

TEST_CASE("显存不够时，每个槽要给出各自的出路") {
    // 只说一句"显存不够"用户无从下手。配音和大模型都能换成外部服务
    // （改配置，不改代码），出图出片躲不掉、只能在放内存和降分辨率之间挑。
    SUBCASE("配音要点名外接 API 这条路") {
        const std::string m = out_of_vram_message(Slot::TTS);
        CHECK(m.find("tts") != std::string::npos);
        CHECK(m.find("base_url") != std::string::npos);
        CHECK(m.find("http") != std::string::npos);
    }
    SUBCASE("大模型同样") {
        const std::string m = out_of_vram_message(Slot::LLM);
        CHECK(m.find("llm") != std::string::npos);
        CHECK(m.find("OpenAI") != std::string::npos);
    }
    SUBCASE("出图出片给的是另外两条，别乱指外接服务") {
        for (const Slot s : {Slot::Image, Slot::Video}) {
            const std::string m = out_of_vram_message(s);
            CHECK(m.find("weights") != std::string::npos);
            CHECK(m.find("清晰度") != std::string::npos);
            // 这两个槽没有外部服务可换，别给假出路
            CHECK(m.find("base_url") == std::string::npos);
            // **清晰度不在设置页了**（2026-09-10 搬到项目页上）。指错地方
            // 的话用户会在设置页翻半天，而这句话是他此刻唯一的线索。
            // 那儿也不叫「画面」卡——项目页上是一行，标题写着「这部电影」。
            CHECK(m.find("项目页") != std::string::npos);
            CHECK(m.find("设置页") == std::string::npos);
            // **档位名要和下拉框里写的一样。** 界面上是「标准 544×928 · 快」；
            // 尺寸还跟着画幅翻（竖屏短边×长边，横屏反过来），所以消息里那
            // 三个数统一按竖屏写、另说一句横屏反过来。
            // 取值虽然还叫 "720p"（老项目的配置文件里存的就是它），但那个
            // 字符串不该出现在给人看的话里——2026-09-10 画幅改成 544×928
            // 之后，叫 720p 就是假的，用户会在那张卡上找一个不存在的选项。
            CHECK(m.find("标准") != std::string::npos);
            CHECK(m.find("544") != std::string::npos);
            CHECK(m.find("720p") == std::string::npos);
        }
    }
    SUBCASE("每一条都得先说清是哪个槽") {
        for (const Slot s : {Slot::LLM, Slot::Image, Slot::Video, Slot::TTS}) {
            CHECK(out_of_vram_message(s).find(to_string(s)) !=
                  std::string::npos);
        }
    }
    SUBCASE("一条都不许再提 comfy") {
        // ComfyUI 2026-09-10 拆了，comfy 现在连配置校验都过不去。
        // 把人指到一个不存在的取值上，比只说一句"显存不够"更糟。
        for (const Slot s : {Slot::LLM, Slot::Image, Slot::Video, Slot::TTS}) {
            CAPTURE(to_string(s));
            CHECK(out_of_vram_message(s).find("comfy") == std::string::npos);
            CHECK(out_of_vram_message(s).find("ComfyUI") == std::string::npos);
        }
    }
}

TEST_CASE("生产里 vram_estimate 就等于整份预算：老实数才救得回来") {
    // 上面那组用的是 8 GB 估值配 10 GB 预算，探针一问就够。
    // **生产不长这样**：register_sd_slots 把每个槽的 vram_estimate 都设成
    // 整份预算（整卡的九成），谁也凑不出第二份。那时候拿 vram_estimate
    // 去问"卡上够不够"，答案永远是不够——这条分支等于不存在，
    // 每次点出片照样把大模型卸掉。这一组盯的就是那个形状。
    Scheduler s;
    const std::size_t budget = 28ull << 30;   // 32 GB 卡的九成，约 28 GB
    s.set_budget(budget);

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;               // 和预算一样大，生产就是这样
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = budget;
    img.evict_priority = 5;
    img.load = [] {};
    img.unload = [] {};

    // 卡上真空着 23 GB（大模型占了 8 GB 左右）。
    SUBCASE("图像这一路真占 10.6 GB：够，不该卸大模型") {
        // Q4 图像模型放内存：缓冲 6.6 + 4.0，权重不常驻。
        img.live_vram = [] { return std::size_t(10ull << 30); };
        s.register_slot(img);
        { auto lease = s.acquire(Slot::LLM); }
        s.set_free_vram_probe([] { return std::optional<double>(23.0); });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 0);
    }
    SUBCASE("图像这一路真占 26.6 GB：不够，照卸") {
        img.live_vram = [] { return std::size_t(26ull << 30); };
        s.register_slot(img);
        { auto lease = s.acquire(Slot::LLM); }
        s.set_free_vram_probe([] { return std::optional<double>(23.0); });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("没给老实数：退回 vram_estimate，也就是老行为") {
        s.register_slot(img);                 // live_vram 没装
        { auto lease = s.acquire(Slot::LLM); }
        s.set_free_vram_probe([] { return std::optional<double>(23.0); });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
}

TEST_CASE("老实数只走问到卡那条路，静态那条一点不放松") {
    // 探针问不到（没有 nvidia-smi）的时候，老实数不许拿来当依据——
    // "问不到"不等于"有空间"。
    Scheduler s;
    const std::size_t budget = 28ull << 30;
    s.set_budget(budget);

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = budget;
    img.live_vram = [] { return std::size_t(10ull << 30); };     // 老实数很小
    img.evict_priority = 5;
    img.load = [] {};
    img.unload = [] {};
    s.register_slot(img);

    { auto lease = s.acquire(Slot::LLM); }
    SUBCASE("没装探针") {
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("装了但问不到") {
        s.set_free_vram_probe([] { return std::optional<double>{}; });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
}

TEST_CASE("配音显存不够时，抛出来的就是那条带出路的话") {
    // 消息内容上面测过了，这一条测的是**够不够得着**：
    // out_of_vram_message 只在 acquire 失败时抛，所以必须真有人借这个槽。
    // 2026-09-10 之前没人借——tts_backends 直接 LlamaTts::load 绕开了调度器，
    // 那段"改成 [tts].backend = http 接外部服务"的话是死代码，用户看不到。
    Scheduler s;
    s.set_budget(4ull << 30);

    SlotSpec big;
    big.slot = Slot::Video;
    big.vram_estimate = 4ull << 30;
    big.load = [] {};
    big.unload = [] {};
    s.register_slot(big);

    SlotSpec tts;
    tts.slot = Slot::TTS;
    tts.vram_estimate = 3ull << 30;
    tts.load = [] {};
    tts.unload = [] {};
    s.register_slot(tts);

    // 视频槽借着不放，腾不出地方。
    auto held = s.acquire(Slot::Video);
    std::string msg;
    try {
        auto lease = s.acquire(Slot::TTS);
        FAIL("显存不够却借到了");
    } catch (const std::exception& e) {
        msg = e.what();
    }
    CHECK(msg.find("base_url") != std::string::npos);
    CHECK(msg.find("http") != std::string::npos);
    // 别把人指到一个已经拆掉的取值上。
    CHECK(msg.find("comfy") == std::string::npos);
}


TEST_CASE("老实数是现问的，不是注册时定死的") {
    // 槽**一个进程只注册一次**（已加载的槽重新注册会抛），而模型可以在
    // 运行中被换掉——初始化页就能换。存成定值的话：开机时还没配模型，
    // 算出来只有计算缓冲那几 GB；用户下了一份 20 GB 的 fp8 之后，这个数
    // 还停在开机那一刻，调度器以为够、不腾地方，然后 CUDA OOM。
    // **估低了是崩**，这个方向最不能错。
    Scheduler s;
    const std::size_t budget = 28ull << 30;
    s.set_budget(budget);

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    // 注册时这个数很小（还没配模型），之后变大（换了个大模型）。
    std::size_t current = 10ull << 30;
    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = budget;
    img.live_vram = [&current] { return current; };
    img.evict_priority = 5;
    img.load = [] {};
    img.unload = [] {};
    s.register_slot(img);

    s.set_free_vram_probe([] { return std::optional<double>(23.0); });

    { auto lease = s.acquire(Slot::LLM); }
    { auto lease = s.acquire(Slot::Image); }
    CHECK(llm_unloads == 0);          // 10 GB ≤ 23 GB，够，不卸

    // 换了个大模型。注册没重来过，但下一次借槽要按新的数算。
    s.evict(Slot::Image);
    current = 26ull << 30;
    { auto lease = s.acquire(Slot::LLM); }
    { auto lease = s.acquire(Slot::Image); }
    CHECK(llm_unloads == 1);          // 26 GB > 23 GB，这回得卸
}

TEST_CASE("实测值一旦量到，就压过静态估算") {
    // 静态估算差得离谱：2026-09-11 在 96 GB 卡上实测，weights="cpu" 那一路
    // 算出来 14.6 GB、真实峰值 74 GB（差五倍）；te=cpu 那一路算 81.8 GB、
    // 实际超过 95.6 GB 直接 OOM 把进程带走。所以量到之后必须以量到的为准。
    Scheduler s;
    const std::size_t budget = 28ull << 30;
    s.set_budget(budget);

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = budget;
    img.live_vram = [] { return std::size_t(10ull << 30); };  // 估算说只要 10 GB
    img.evict_priority = 5;
    img.load = [] {};
    img.unload = [] {};
    s.register_slot(img);

    s.set_free_vram_probe([] { return std::optional<double>(23.0); });

    SUBCASE("没量过：按估算，10 ≤ 23，不卸") {
        { auto lease = s.acquire(Slot::LLM); }
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 0);
    }
    SUBCASE("量到真占 26 GB：估算说够也得卸") {
        s.record_measured_vram(Slot::Image, 26ull << 30);
        { auto lease = s.acquire(Slot::LLM); }
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
}

TEST_CASE("实测值只往上记，不往下调") {
    // 同一个槽不同镜头占用有出入（帧数、画幅、挂没挂 LoRA）。按最近一次
    // 记的话，一个小镜头会把上限拉低，紧接着一个大镜头就 OOM 了。
    Scheduler s;
    s.record_measured_vram(Slot::Video, 70ull << 30);
    CHECK(s.measured_vram(Slot::Video) == (70ull << 30));

    s.record_measured_vram(Slot::Video, 40ull << 30);   // 小的那一镜
    CHECK(s.measured_vram(Slot::Video) == (70ull << 30));  // 上限不该被拉低

    s.record_measured_vram(Slot::Video, 80ull << 30);   // 更大的
    CHECK(s.measured_vram(Slot::Video) == (80ull << 30));

    // 没量过的槽回 0，调用方据此退回保守估算
    CHECK(s.measured_vram(Slot::TTS) == 0);
    // 0 不记账——量不到的时候（没有 nvidia-smi）别把上限清成 0
    s.record_measured_vram(Slot::Video, 0);
    CHECK(s.measured_vram(Slot::Video) == (80ull << 30));
}

TEST_CASE("问不到卡的时候，用量到的推算空闲") {
    // nvidia-smi 不是永远问得到：free_vram_gb 是 fork + exec 去跑它的，
    // 而这个进程初始化 CUDA 之后映射着十几 GB，在这种进程里 fork 本来就是
    // NVIDIA 明确不支持的做法。以前问不到就一路走到驱逐，
    // "显存够就不清理"在真机上等于从来没生效过。
    Scheduler s;
    const std::size_t budget = 86ull << 30;
    s.set_budget(budget);
    s.set_total_vram(96ull << 30);
    s.set_free_vram_probe([] { return std::optional<double>{}; });  // 问不到

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    SUBCASE("两边都量过：96 − 15 = 81 够放 74，不卸") {
        s.record_measured_vram(Slot::LLM, 15ull << 30);
        s.record_measured_vram(Slot::Video, 74ull << 30);
        { auto lease = s.acquire(Slot::LLM); }
        { auto lease = s.acquire(Slot::Video); }
        CHECK(llm_unloads == 0);
    }
    SUBCASE("量到的放不下：照卸") {
        s.record_measured_vram(Slot::LLM, 15ull << 30);
        s.record_measured_vram(Slot::Video, 90ull << 30);   // 96 − 15 = 81 < 90
        { auto lease = s.acquire(Slot::LLM); }
        { auto lease = s.acquire(Slot::Video); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("装着的槽既没量过也估不出：不推算，保守驱逐") {
        // **这一条是安全底线。** 不知道别人占多少就敢算空闲的话，
        // 算出来的数会偏大，然后 OOM——而 OOM 走 GGML_ASSERT，
        // abort() 把整个服务带走。
        // 这里的 llm 没设 live_vram，所以连估都估不出来。
        s.record_measured_vram(Slot::Video, 74ull << 30);    // LLM 没量过
        { auto lease = s.acquire(Slot::LLM); }
        { auto lease = s.acquire(Slot::Video); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("不知道整卡多大：也不推算") {
        Scheduler s2;
        s2.set_budget(budget);
        s2.set_free_vram_probe([] { return std::optional<double>{}; });
        // 故意不调 set_total_vram
        int un = 0;
        SlotSpec a = llm;
        a.unload = [&un] { ++un; };
        s2.register_slot(a);
        s2.register_slot(vid);
        s2.record_measured_vram(Slot::LLM, 15ull << 30);
        s2.record_measured_vram(Slot::Video, 74ull << 30);
        { auto lease = s2.acquire(Slot::LLM); }
        { auto lease = s2.acquire(Slot::Video); }
        CHECK(un == 1);
    }
}

TEST_CASE("探针失灵时，没量过的槽用它自己的估算顶上") {
    // **为什么非要留这条估算的口子。**
    //
    // 大模型那份实测值是"装之前问一次显存、装完再问一次"的差值，问的还是
    // 同一个 nvidia-smi。探针问不到的时候，那两次也一样问不到，于是大模型
    // 永远拿不到实测值。只认实测的话，"问不到就推算"这条在"探针失灵"这个
    // 它唯一要救的场景里从来不会生效——出片每次都会把大模型踢掉。
    //
    // 估算只往高了用：把别人占的算大，推出来的空闲偏小，顶多多卸一次。
    const std::size_t budget = 86 * GB;
    int llm_unloads = 0;

    auto setup = [&](Scheduler& s, std::size_t llm_live) {
        s.set_budget(budget);
        s.set_total_vram(96 * GB);
        s.set_free_vram_probe([] { return std::optional<double>{}; });  // 问不到

        SlotSpec llm;
        llm.slot = Slot::LLM;
        llm.vram_estimate = budget;
        llm.evict_priority = 1;
        llm.load = [] {};
        llm.unload = [&llm_unloads] { ++llm_unloads; };
        if (llm_live > 0) llm.live_vram = [llm_live] { return llm_live; };
        s.register_slot(std::move(llm));

        SlotSpec vid;
        vid.slot = Slot::Video;
        vid.vram_estimate = budget;
        vid.evict_priority = 9;
        vid.load = [] {};
        vid.unload = [] {};
        s.register_slot(std::move(vid));

        s.record_measured_vram(Slot::Video, 74 * GB);
    };

    auto run = [](Scheduler& s) {
        { auto lease = s.acquire(Slot::LLM); }
        { auto lease = s.acquire(Slot::Video); }
    };

    SUBCASE("大模型估 20 GB：96 − 20 = 76，放得下 74，不卸") {
        Scheduler s;
        setup(s, 20 * GB);
        run(s);
        CHECK(llm_unloads == 0);
    }
    SUBCASE("大模型估 30 GB：96 − 30 = 66，放不下 74，照卸") {
        Scheduler s;
        setup(s, 30 * GB);
        run(s);
        CHECK(llm_unloads == 1);
    }
    SUBCASE("既没量过也估不出：还是保守驱逐") {
        Scheduler s;
        setup(s, 0);
        run(s);
        CHECK(llm_unloads == 1);
    }
    SUBCASE("量到了就按量到的算，不用估的那个") {
        // 估算说 30（会卸），实测说 15（不卸）。实测是证据，估算是模型，
        // 有证据就别再用模型——否则量得越准反而卸得越勤。
        Scheduler s;
        setup(s, 30 * GB);
        s.record_measured_vram(Slot::LLM, 15 * GB);
        run(s);
        CHECK(llm_unloads == 0);
    }
    SUBCASE("留痕里要标明空闲不是问来的") {
        Scheduler s;
        setup(s, 20 * GB);
        run(s);
        const auto d = s.last_room_decision();
        REQUIRE(d.valid);
        CHECK(d.probed == false);
        CHECK(d.kept == true);
        CHECK(d.free_seen == 76 * GB);
    }
}

TEST_CASE("在小画幅量到的数，不能拿去给大画幅背书") {
    // **这条是 2026-09-11 补的安全底线。**
    //
    // "量出来的数比估算准"这个设计是对的，但量到的数**不带画幅**。
    // 档位从 544×928（0.5 MP）到 2560×1440（3.7 MP）差七倍多，占用跟着
    // 画幅和帧数走。在 720p 量到 20 GB、下一镜切 2K 还按 20 GB 判
    // "够，不卸"，就是拿偏小的数去赌——赌输了是 CUDA OOM，走 GGML_ASSERT
    // 直接 abort()，整个服务没了，不是一条能读的报错。
    //
    // 所以量的时候记下量的是多大的活，用的时候只认"量过的活不小于这次的活"。
    const std::size_t budget = 86 * GB;
    const std::size_t small = 544ull * 928 * 81;    // 720p 档
    const std::size_t big = 2560ull * 1440 * 81;    // 2K 档

    int llm_unloads = 0;
    auto setup = [&](Scheduler& s) {
        s.set_budget(budget);
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        SlotSpec llm;
        llm.slot = Slot::LLM;
        llm.vram_estimate = budget;
        llm.evict_priority = 1;
        llm.load = [] {};
        llm.unload = [&llm_unloads] { ++llm_unloads; };
        s.register_slot(std::move(llm));
        SlotSpec vid;
        vid.slot = Slot::Video;
        vid.vram_estimate = budget;   // 保守估值 = 整份预算
        vid.evict_priority = 9;
        vid.load = [] {};
        vid.unload = [] {};
        s.register_slot(std::move(vid));
    };

    SUBCASE("量过的活更大：认，不卸") {
        Scheduler s;
        setup(s);
        s.record_measured_vram(Slot::Video, 20 * GB, big);
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, small); }
        CHECK(llm_unloads == 0);
        CHECK(s.last_room_decision().live_measured);
    }
    SUBCASE("活一样大：认") {
        Scheduler s;
        setup(s);
        s.record_measured_vram(Slot::Video, 20 * GB, small);
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, small); }
        CHECK(llm_unloads == 0);
    }
    SUBCASE("这次的活更大：不认，回到保守那条") {
        Scheduler s;
        setup(s);
        s.record_measured_vram(Slot::Video, 20 * GB, small);
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, big); }
        CHECK(llm_unloads == 1);
        CHECK_FALSE(s.last_room_decision().live_measured);
    }
    SUBCASE("老文件里的数不知道多大的活：这次说了大小就不认") {
        Scheduler s;
        setup(s);
        s.record_measured_vram(Slot::Video, 20 * GB);   // work 缺省 0
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, big); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("调用方没说这次多大：按老规矩认") {
        // 大模型、配音这些槽和画幅无关，不该因为这条新规矩变得更保守。
        Scheduler s;
        setup(s);
        s.record_measured_vram(Slot::Video, 20 * GB, small);
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video); }
        CHECK(llm_unloads == 0);
    }
}

TEST_CASE("实测值：字节数和活各记各的高水位") {
    Scheduler s;
    const std::size_t small = 544ull * 928 * 81;
    const std::size_t big = 2560ull * 1440 * 81;

    SUBCASE("先大活小占用、后小活大占用：两边都留最大的") {
        s.record_measured_vram(Slot::Video, 20 * GB, big);
        s.record_measured_vram(Slot::Video, 30 * GB, small);
        const auto m = s.all_measured().at(Slot::Video);
        CHECK(m.bytes == 30 * GB);
        // **work 不能跟着 bytes 被拉回去。** 拉回去的话，此后每一镜 2K
        // 都当没量过办，白卸一次大模型——而我们明明见过 2K 只用了 20 GB。
        CHECK(m.work == big);
    }
    SUBCASE("占用没长高但活更大：也要落盘") {
        int sunk = 0;
        s.set_measured_sink([&sunk](Slot, std::size_t) { ++sunk; });
        s.record_measured_vram(Slot::Video, 30 * GB, small);
        CHECK(sunk == 1);
        s.record_measured_vram(Slot::Video, 20 * GB, big);
        CHECK(sunk == 2);   // 字节数没长，但"罩得住多大的活"长了
        const auto m = s.all_measured().at(Slot::Video);
        CHECK(m.bytes == 30 * GB);
        CHECK(m.work == big);
    }
    SUBCASE("两样都没长：不惊动落盘") {
        int sunk = 0;
        s.record_measured_vram(Slot::Video, 30 * GB, big);
        s.set_measured_sink([&sunk](Slot, std::size_t) { ++sunk; });
        s.record_measured_vram(Slot::Video, 20 * GB, small);
        CHECK(sunk == 0);
    }
}

TEST_CASE("最近一次腾地方的判断要留痕，界面上读得到") {
    // 这个判断错了的表现是"该留的时候卸了"（慢）或者"该卸的时候没卸"
    // （CUDA OOM 把整个服务带走）。而以前只能登上机器看 stderr——
    // 2026-09-11 服务器连不上那几个钟头，这条线索彻底断了。
    Scheduler s;
    const std::size_t budget = 86ull << 30;
    s.set_budget(budget);

    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [] {};
    s.register_slot(llm);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    SUBCASE("还没判过的时候是空的，不能装作判过") {
        CHECK_FALSE(s.last_room_decision().valid);
    }

    SUBCASE("够、没卸：把依据一并记下") {
        s.record_measured_vram(Slot::Video, 74ull << 30);
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video); }
        const auto d = s.last_room_decision();
        REQUIRE(d.valid);
        CHECK(d.slot == Slot::Video);
        CHECK(d.kept);
        CHECK(d.evicted == 0);
        CHECK(d.probed);                       // 是问卡问来的
        CHECK(d.live == (74ull << 30));        // 用的是实测值
        CHECK(d.free_seen > 0);
    }

    SUBCASE("不够、卸了：记下卸了几个") {
        s.record_measured_vram(Slot::Video, 90ull << 30);
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video); }
        const auto d = s.last_room_decision();
        REQUIRE(d.valid);
        CHECK_FALSE(d.kept);
        CHECK(d.evicted >= 1);
    }

    SUBCASE("要多少这个数是量来的还是估的，也要记") {
        // **这一位决定上面那个"够"值不值得信。** 估算在 video 这一路
        // 算 14.6 GB、实测 74 GB，差五倍。拿估的判出"够，不卸"，
        // 下一步就可能是 CUDA OOM——那个直接 abort，服务整个没了。
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        SUBCASE("量过：标成真") {
            s.record_measured_vram(Slot::Video, 74ull << 30);
            { auto a = s.acquire(Slot::LLM); }
            { auto b = s.acquire(Slot::Video); }
            const auto d = s.last_room_decision();
            REQUIRE(d.valid);
            CHECK(d.kept);
            CHECK(d.live_measured);
        }
        SUBCASE("没量过、拿估算判的：标成假，界面上要能警告") {
            Scheduler s2;
            s2.set_budget(budget);
            s2.set_free_vram_probe([] { return std::optional<double>(80.0); });
            SlotSpec a2 = llm;
            s2.register_slot(a2);
            SlotSpec v2 = vid;
            v2.live_vram = [] { return 14ull << 30; };   // 估得偏小的那个数
            s2.register_slot(v2);
            { auto a = s2.acquire(Slot::LLM); }
            { auto b = s2.acquire(Slot::Video); }
            const auto d = s2.last_room_decision();
            REQUIRE(d.valid);
            CHECK(d.kept);                  // 估算说够
            CHECK_FALSE(d.live_measured);   // 但这个"够"不可信
        }
        SUBCASE("静态那条就够、根本没查过实测：也标成假") {
            Scheduler s3;
            s3.set_budget(budget);
            SlotSpec only = vid;
            only.vram_estimate = 10 * GB;
            s3.register_slot(only);
            s3.record_measured_vram(Slot::Video, 74ull << 30);
            { auto b = s3.acquire(Slot::Video); }
            const auto d = s3.last_room_decision();
            REQUIRE(d.valid);
            CHECK(d.kept);
            CHECK_FALSE(d.live_measured);   // 这一路压根没拿实测去比
        }
    }

    SUBCASE("问不到卡时也要记，并且标明空闲不是问来的") {
        s.set_total_vram(96ull << 30);
        s.record_measured_vram(Slot::LLM, 15ull << 30);
        s.record_measured_vram(Slot::Video, 74ull << 30);
        s.set_free_vram_probe([] { return std::optional<double>{}; });
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video); }
        const auto d = s.last_room_decision();
        REQUIRE(d.valid);
        CHECK(d.kept);
        CHECK_FALSE(d.probed);   // 推算出来的，不是问来的
    }
}

TEST_CASE("从头到尾走一遍用户要的那条路") {
    // **这条把散着的几件事串起来测一遍。** 单独看每一件都有用例了，
    // 但用户说的是一整串："默认加载 llm，点击出片清理掉大模型，如果内存
    // 够的就不用清理，根据实时的显存情况来判断"。中间任何两件叠在一起
    // 打架，分开测是看不出来的。
    //
    // 场景按 96 GB 卡摆：起服务先装上大模型，然后连出三镜。
    const std::size_t budget = 86 * GB;
    const std::size_t small = 544ull * 928 * 81;    // 标准档
    const std::size_t big = 2560ull * 1440 * 81;    // 2K 档

    int llm_unloads = 0, llm_loads = 0;
    Scheduler s;
    s.set_budget(budget);
    s.set_total_vram(96 * GB);
    // 卡上真实空闲：大模型占了 15，还剩 81。
    s.set_free_vram_probe([] { return std::optional<double>(81.0); });

    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;     // 静态估值 = 整份预算，和生产里一致
    llm.evict_priority = 1;
    llm.load = [&llm_loads] { ++llm_loads; };
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(std::move(llm));

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    // **出片槽故意不给 live_vram**，和生产里一致（估算被实测推翻过两次）。
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(std::move(vid));

    // 1）起服务：预热把大模型装上。这时候没别的槽，静态那条就够。
    { auto warm = s.acquire(Slot::LLM); }
    CHECK(llm_loads == 1);
    CHECK(s.loaded(Slot::LLM));

    // 2）第一镜出片：还没量过，没有估算可用 -> 保守，卸大模型。
    //    这一步"慢几十秒"是有意的：拿没验过的数赌一把的代价是 OOM。
    { auto v = s.acquire(Slot::Video, small); }
    CHECK(llm_unloads == 1);
    CHECK_FALSE(s.loaded(Slot::LLM));
    CHECK(s.room_note(Slot::Video) == "腾显存：卸了 1 个模型");

    // 3）这一镜跑起来量到了真实占用：74 GB @ 标准档。
    s.record_measured_vram(Slot::Video, 74 * GB, small);

    // 4）写下一章剧本，大模型装回来。
    { auto a = s.acquire(Slot::LLM); }
    CHECK(llm_loads == 2);

    // 5）再出一镜（还是标准档）：74 ≤ 81，**够，不卸**——用户要的那句。
    { auto v = s.acquire(Slot::Video, small); }
    CHECK(llm_unloads == 1);          // 还是 1，没再卸
    CHECK(s.loaded(Slot::LLM));
    CHECK(s.room_note(Slot::Video) == "显存够，没动别的模型");

    // 5b）**同一档再出一镜：这一镜也要重新问一次卡。** 以前槽装着、画幅
    //     罩得住就走快路（不重新腾地方），2026-09-13 栽了：中间首帧阶段把
    //     图像模型装上，回头出片一次判断都不做，sd.cpp 当场没内存。现在
    //     只要说了这次多大的活就重走 make_room——这里什么都没变，所以是
    //     "够，没动"，而且不会抄上一镜的结论（上一镜要是卸过也不会被算到
    //     这一镜头上）。
    { auto v = s.acquire(Slot::Video, small); }
    CHECK(llm_unloads == 1);
    CHECK(s.room_note(Slot::Video) == "显存够，没动别的模型");

    // 6）用户把画幅换成 2K：量过的活比这次小，那个数不算数 -> 回到保守。
    //    在标准档量到的 74 GB 拿去给 2K 判"够"，下一步就是显存爆掉。
    { auto v = s.acquire(Slot::Video, big); }
    CHECK(llm_unloads == 2);
    CHECK(s.room_note(Slot::Video) == "腾显存：卸了 1 个模型");
}

TEST_CASE("卸干净之后，上一次的判断就不该还挂着") {
    // 那条结论说的是"上一次要不要腾地方"。什么都没装着了还显示
    // "刚才卸了 1 个模型"，是在说一件已经不成立的事。
    //
    // 这也是测试唯一的复位钩子：不清的话，一个用例摆的局面会顺着全局
    // 单例漏给下一个用例——下一个要是逐字比对进度文案，就会被凭空多出来
    // 的"（腾显存：卸了 1 个模型）"挂掉，而且看不出是谁干的。
    Scheduler s;
    const std::size_t budget = 10 * GB;
    s.set_budget(budget);

    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [] {};
    s.register_slot(llm);
    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    { auto a = s.acquire(Slot::LLM); }
    { auto v = s.acquire(Slot::Video); }
    REQUIRE(s.last_room_decision().valid);
    REQUIRE_FALSE(s.room_note(Slot::Video).empty());

    s.evict_all();
    CHECK_FALSE(s.last_room_decision().valid);
    CHECK(s.room_note(Slot::Video).empty());
    CHECK(s.room_note(Slot::LLM).empty());
}

TEST_CASE("走快路的那一镜，不能照抄上一镜的结论") {
    // **接着上面那条串起来的用例继续挖。**
    //
    // 槽装着、画幅也罩得住时走快路（不重新腾地方）。那时候 last_decision_
    // 停在上一镜——上一镜要是卸过模型，这一镜的进度条上就会凭空多出一句
    // "腾显存：卸了 1 个模型"，而它什么都没卸。
    //
    // 用户看到的是每镜都在卸模型，于是回来问"不是说够就不清理吗"。
    const std::size_t budget = 86 * GB;
    const std::size_t small = 544ull * 928 * 81;

    int llm_unloads = 0;
    Scheduler s;
    s.set_budget(budget);
    s.set_free_vram_probe([] { return std::optional<double>(81.0); });

    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(std::move(llm));

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(std::move(vid));

    { auto a = s.acquire(Slot::LLM); }
    // 第一镜：没量过 -> 保守，卸掉大模型。
    { auto v = s.acquire(Slot::Video, small); }
    REQUIRE(llm_unloads == 1);
    REQUIRE(s.room_note(Slot::Video) == "腾显存：卸了 1 个模型");

    // 这一镜跑完量到了。
    s.record_measured_vram(Slot::Video, 74 * GB, small);

    // 第二镜：槽还装着、画幅也罩得住。**以前走快路什么都不干**，2026-09-13
    // 栽了（中间别的槽装上来，回头一次判断都不做就 OOM），现在只要说了
    // 这次多大的活就重新问一次卡：74 ≤ 81，够，没动。
    { auto v = s.acquire(Slot::Video, small); }
    CHECK(llm_unloads == 1);
    // **不能还是"卸了 1 个模型"**——那是上一镜的结论。这一镜真判过了，
    // 照实说"够"。
    CHECK(s.room_note(Slot::Video) == "显存够，没动别的模型");
    const auto d = s.last_room_decision();
    REQUIRE(d.valid);
    CHECK_FALSE(d.already_loaded);
    CHECK(d.kept);
    CHECK(d.evicted == 0);
}

TEST_CASE("腾不出地方时，先说清是被谁挡住的") {
    // **"再等等"和"这张卡太小"是两件完全不同的事。** 以前腾不出地方
    // 一律给同一段话（"别的槽正被借用着，或者这张卡确实太小"），用户
    // 没法判断该等还是该去降画幅、换小模型——后者是白折腾。
    //
    // 这个窗口 2026-09-11 之后更容易撞上：起服务时会在后台装大模型，
    // 那几十秒里它是借着的，"起服务 → 立刻点出片"正好落在里面。
    Scheduler s;
    const std::size_t budget = 10 * GB;
    s.set_budget(budget);

    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;   // 一个就占满，第二个一定要腾
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [] {};
    s.register_slot(llm);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    SUBCASE("挡路的正被借着：点名它，并且说等它干完再点一次") {
        auto held = s.acquire(Slot::LLM);   // 租约一直拿着
        try {
            auto v = s.acquire(Slot::Video);
            FAIL("装不下却没抛");
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("LLM") != std::string::npos);
            CHECK(msg.find("正用着") != std::string::npos);
            // 钉的是"让人等"这个意思，不是某一句话的措辞。原来那句写的是
            // "多半是它还在装（起服务时大模型会在后台装上）"——预装那条路
            // 后来整个去掉了（用的时候才装），那句话就成了往错方向支人。
            CHECK(msg.find("再点一次") != std::string::npos);
            // 通用那几条出路还得在后面，一直这样才是真的装不下。
            //
            // **锚在 `weights` 上，不锚措辞。** 原来钉的是「画面」，而那段
            // 通用出路后来改成了「把**清晰度**降一档」——话还在、意思没变，
            // 用例却红了。`[models].weights` 是配置键名，比任何一句中文稳。
            CHECK(msg.find("weights") != std::string::npos);
        }
    }
    SUBCASE("没人借着、纯粹是卡小：给的还是原来那段") {
        // 这里不留租约，LLM 装着但没被借——能驱逐，所以腾得出来。
        { auto a = s.acquire(Slot::LLM); }
        { auto v = s.acquire(Slot::Video); }   // 应该卸掉 LLM 之后成功
        CHECK(s.loaded(Slot::Video));
        CHECK_FALSE(s.loaded(Slot::LLM));
    }
}

TEST_CASE("腾显存的结论要能一句话挂到进度条上") {
    // **用户点完出片盯的是进度条**，而"卸没卸大模型"以前只在设置页上。
    // 一镜一句，就在他眼前。见 Scheduler::room_note。
    Scheduler s;
    const std::size_t budget = 86 * GB;
    s.set_budget(budget);

    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [] {};
    s.register_slot(llm);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = budget;
    vid.evict_priority = 9;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    SUBCASE("还没判过：什么都不说，别硬凑一句") {
        CHECK(s.room_note(Slot::Video).empty());
    }
    SUBCASE("卸了：说卸了几个") {
        s.record_measured_vram(Slot::Video, 90 * GB, 1);
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, 1); }
        CHECK(s.room_note(Slot::Video) == "腾显存：卸了 1 个模型");
    }
    SUBCASE("够、而且是量出来的：直说没动") {
        s.record_measured_vram(Slot::Video, 74 * GB, 1);
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, 1); }
        CHECK(s.room_note(Slot::Video) == "显存够，没动别的模型");
    }
    SUBCASE("够、但是按估算判的：**必须说出来**") {
        // 出片这一路的估算被实测推翻过两次，都是往小了错五倍，
        // 而判错的后果是 CUDA OOM 把整个服务带走。用户看到这句就知道
        // 这一镜是在没量过的情况下赌了一把。
        Scheduler s2;
        s2.set_budget(budget);
        s2.set_free_vram_probe([] { return std::optional<double>(80.0); });
        SlotSpec a2 = llm;
        s2.register_slot(a2);
        SlotSpec v2 = vid;
        v2.live_vram = [] { return 14ull << 30; };
        s2.register_slot(v2);
        { auto a = s2.acquire(Slot::LLM); }
        { auto b = s2.acquire(Slot::Video); }
        CHECK(s2.room_note(Slot::Video) == "显存够（按估算判的），没动别的模型");
    }
    SUBCASE("判的是别的槽：这个槽这儿不说话") {
        // 挂错地方会让人以为刚刚为这一镜卸过模型。
        s.record_measured_vram(Slot::Video, 74 * GB, 1);
        s.set_free_vram_probe([] { return std::optional<double>(80.0); });
        { auto a = s.acquire(Slot::LLM); }
        { auto b = s.acquire(Slot::Video, 1); }
        CHECK(s.room_note(Slot::Image).empty());
        CHECK(s.room_note(Slot::LLM).empty());
    }
}

TEST_CASE("显存宽裕时也要留痕，别让界面显示上一次的旧结论") {
    // 不记的话，界面上显示的还是更早那次——而那次很可能是"卸了"。
    // 用户看着以为刚才又卸了一回，实际这次根本没压力。
    Scheduler s;
    s.set_budget(100ull << 30);

    SlotSpec a;
    a.slot = Slot::Image;
    a.vram_estimate = 10ull << 30;   // 宽裕，静态那步就过
    a.load = [] {};
    a.unload = [] {};
    s.register_slot(a);

    CHECK_FALSE(s.last_room_decision().valid);
    { auto lease = s.acquire(Slot::Image); }
    const auto d = s.last_room_decision();
    REQUIRE(d.valid);
    CHECK(d.slot == Slot::Image);
    CHECK(d.kept);
    CHECK(d.evicted == 0);
}

TEST_CASE("两个槽不会同时往卡上装") {
    // **2026-09-11 服务被这件事整个干掉过一次。** 出片跑到成片那一步，
    // 另一边大模型正在装（8.4 GB 权重 + 6.4 GB KV），视频的文本编码器
    // 连 70 MiB 都要不到，sd.cpp 的 GGML_ASSERT 直接 abort()——不是异常，
    // 兜不住，在跑的任务全没了。
    //
    // 根子在 acquire 的形状：它标完 is_loaded 就放锁、再去调 load()。
    // 而 make_room 那一刻问到的空闲显存，在另一个槽装到一半时就不作数了。
    Scheduler s;
    s.set_budget(0);  // 不靠预算拦，就看两次 load 会不会叠在一起

    std::atomic<int> inflight{0};
    std::atomic<int> max_inflight{0};
    std::atomic<int> done{0};

    const auto slow_spec = [&](Slot slot) {
        SlotSpec spec;
        spec.slot = slot;
        spec.residency = Residency::Cached;
        spec.vram_estimate = GB;
        spec.load = [&] {
            const int now = ++inflight;
            // 记峰值。两个 load 只要有一瞬间叠上，这个数就会到 2。
            int prev = max_inflight.load();
            while (now > prev && !max_inflight.compare_exchange_weak(prev, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            --inflight;
            ++done;
        };
        spec.unload = [] {};
        return spec;
    };
    s.register_slot(slow_spec(Slot::LLM));
    s.register_slot(slow_spec(Slot::Video));

    std::thread a([&] { auto l = s.acquire(Slot::LLM); });
    std::thread b([&] { auto l = s.acquire(Slot::Video); });
    a.join();
    b.join();

    CHECK(done == 2);
    CHECK(max_inflight == 1);
}

// **槽名有两个：一个是键，一个是话。**
//
// `to_string(Slot)` 是键：实测显存那份文件按它分节（`serialize_measured_vram`），
// `/api/system` 回的 `slot` 字段也是它。翻了之后换一次语言，上一次量到的数
// 就再也认不出来——而表现只是"第一镜又卸了一次模型"，一句报错都没有。
//
// `slot_label(Slot)` 是话，跟着界面语言走。
//
// ⚠️ 上面「每一条都得先说清是哪个槽」那条**在中文下证明不了什么**：中文里
// 这两个函数回的是同一个字符串。真要证明得换一种语言跑。
TEST_CASE("多语言 · 槽名：键不动，话跟着换") {
    using namespace changji::infer;
    struct ScopedLang {
        std::string old_ = changji::i18n::spoken();
        explicit ScopedLang(const char* l) { changji::i18n::speak(l); }
        ~ScopedLang() { changji::i18n::speak(old_); }
    };

    for (const Slot s : {Slot::Image, Slot::Video, Slot::TTS}) {
        CAPTURE(to_string(s));
        const std::string key_zh = to_string(s);
        const std::string label_zh = slot_label(s);
        ScopedLang de("de");
        // 键一个字都不许动。
        CHECK(std::string(to_string(s)) == key_zh);
        // 话换了。
        CHECK(slot_label(s) != label_zh);
        // 那几段长话里说的是"话"，不是"键"——说成键的话德语用户会看到
        // 一句德文里嵌着两个汉字。
        const std::string m = out_of_vram_message(s);
        CAPTURE(m);
        CHECK(m.find(slot_label(s)) != std::string::npos);
        CHECK(m.find(key_zh) == std::string::npos);
    }
}
