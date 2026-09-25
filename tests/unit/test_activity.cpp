// 正在干的那些短活。
//
// 这一层的用处全在"不可见"上：2026-09-11 一次批量写作四章全挂在
// 「显存不够加载 LLM：「图像」正用着」，而顶栏 `jobs: []`、GPU 0%——
// 挡路的那张参考图整个没登记。所以这里钉的是**登记和划掉这对操作**：
// 登记漏了看不见活，划掉漏了留下一条永远在跑的幽灵记录，而那条记录会让
// 顶栏永远亮着，比不显示更糟。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "pipeline/activity.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/task_board.hpp"
#include "util/text.hpp"

#include <nlohmann/json.hpp>

using changji::pipeline::Activity;
using changji::pipeline::running_activities;
using changji::pipeline::running_work;
namespace pipeline = changji::pipeline;

TEST_CASE("没活的时候是空数组，不是 null") {
    // 前端直接 v-for 它。null 的话整块渲染不出来，而且不报错。
    CHECK(running_activities().is_array());
    CHECK(running_activities().empty());
}

TEST_CASE("构造登记，析构划掉") {
    {
        Activity a{"image", "/p/one", "", "正在画参考图"};
        const auto rows = running_activities();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0]["kind"] == "image");
        CHECK(rows[0]["project"] == "/p/one");
        CHECK(rows[0]["message"] == "正在画参考图");
    }
    // 出了作用域就得干净。**漏一条的代价是顶栏永远亮着**，而用户会以为
    // 引擎卡住了。
    CHECK(running_activities().empty());
}

TEST_CASE("抛异常也划得掉") {
    // 出图失败抛的是 ApiError，登记那条要是靠"跑到函数末尾"来清，
    // 一次失败就留一条幽灵。
    try {
        Activity a{"image", "/p", "", "画着"};
        REQUIRE(running_activities().size() == 1);
        throw std::runtime_error("出图失败");
    } catch (const std::exception&) {
    }
    CHECK(running_activities().empty());
}

TEST_CASE("行的形状和长跑任务那边一样") {
    Activity a{"revise", "/p", "ch01", "正在改这一段"};
    const auto row = running_activities()[0];
    // 前端一套模板画两边，少一个键就得在模板里到处判空
    for (const char* k :
         {"kind", "project", "episode_id", "stage", "current", "total",
          "message", "queued"}) {
        CAPTURE(k);
        CHECK(row.contains(k));
    }
    CHECK(row["episode_id"] == "ch01");
    CHECK(row["stage"] == "");
}

TEST_CASE("排队那句盖在原话上，清掉就露出原话") {
    Activity a{"image", "/p", "", "正在画参考图"};
    CHECK(running_activities()[0]["queued"] == false);

    a.set_note("排队中，前面还有 2 件");
    auto row = running_activities()[0];
    CHECK(row["queued"] == true);
    // 两句都要有：在排队，而且排的是哪件事
    CHECK(row["message"] == "排队中，前面还有 2 件：正在画参考图");

    a.set_note("");
    row = running_activities()[0];
    CHECK(row["queued"] == false);
    CHECK(row["message"] == "正在画参考图");
}

TEST_CASE("note_queued：写到当前线程那件活上") {
    Activity a{"image", "/p", "", "正在画参考图"};
    CHECK(changji::pipeline::current_activity() != nullptr);

    changji::pipeline::note_queued(2, "");
    CHECK(running_activities()[0]["message"] ==
          "排队中，前面还有 2 件：正在画参考图");

    // 轮到自己了但显存腾不动：点名挡路的那个槽
    changji::pipeline::note_queued(0, "LLM");
    CHECK(running_activities()[0]["message"] ==
          "排队中，等「LLM」用完：正在画参考图");

    // -1 = 不排了
    changji::pipeline::note_queued(-1, "");
    CHECK(running_activities()[0]["message"] == "正在画参考图");
}

TEST_CASE("没活在干的时候 note_queued 什么也不干，不崩") {
    // 调度器是从底下调上来的，而底下不知道上头有没有登记过活——
    // 命令行跑流水线时就一个都没有。
    CHECK(changji::pipeline::current_activity() == nullptr);
    changji::pipeline::note_queued(3, "图像");
    CHECK(running_activities().empty());
}

TEST_CASE("进度改得动，话也改得动") {
    Activity a{"image", "/p", "", "画着"};
    a.set_progress(3, 8);
    a.set_message("快好了");
    const auto row = running_activities()[0];
    CHECK(row["current"] == 3);
    CHECK(row["total"] == 8);
    CHECK(row["message"] == "快好了");
}

TEST_CASE("几件同时干，按开工顺序排") {
    // 顶栏那个列表两秒重画一次。顺序每次都跳的话，看着像有活在闪。
    Activity a{"outline", "/p", "", "一"};
    Activity b{"image", "/p", "", "二"};
    Activity c{"say", "/p", "", "三"};
    const auto rows = running_activities();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0]["message"] == "一");
    CHECK(rows[1]["message"] == "二");
    CHECK(rows[2]["message"] == "三");
}

TEST_CASE("中间那件干完了，剩下的顺序不变") {
    Activity a{"outline", "/p", "", "一"};
    {
        Activity b{"image", "/p", "", "二"};
        CHECK(running_activities().size() == 2);
    }
    Activity c{"say", "/p", "", "三"};
    const auto rows = running_activities();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]["message"] == "一");
    CHECK(rows[1]["message"] == "三");
}

TEST_CASE("多个线程同时登记划掉，不崩不漏") {
    // 出图、写作、朗读各在各的请求线程上，真会同时开工。
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([i] {
            for (int n = 0; n < 50; ++n) {
                Activity a{"image", "/p" + std::to_string(i), "", "干着"};
                a.set_progress(n, 50);
                (void)running_activities();
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(running_activities().empty());
}

TEST_CASE("全部在干的活：没长跑任务时就等于短活那份") {
    Activity a{"image", "/p", "", "画着"};
    const auto all = running_work();
    REQUIRE(all.size() == 1);
    CHECK(all[0]["kind"] == "image");
}

TEST_CASE("画的是哪一格参考图：target 跟着那一行走") {
    // 没有 WebSocket 的时候，设定页只能从 `/api/system` 那份表里认出
    // "这一格在画"（`refs` 那条固定频道是纯 socket 的）。少了这一栏，
    // 那几格从头到尾一动不动，而且不报错。
    Activity a{"image", "/p/one", "", "正在画参考图"};
    {
        const auto rows = running_activities();
        REQUIRE(rows.size() == 1);
        // 没说画哪一格之前是空串，不是 null——前端直接读它
        CHECK(rows[0]["target"] == "");
    }
    a.set_target("linyuan_front");
    a.set_progress(7, 20);
    const auto rows = running_activities();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["target"] == "linyuan_front");
    CHECK(rows[0]["current"] == 7);
    CHECK(rows[0]["total"] == 20);
}

TEST_CASE("长跑任务那几行也要有 target 这一栏") {
    // 顶栏和设定页都是一套代码画两边的列表，少一个键就得到处判空。
    // 同 `queued` 那一栏的理由。
    const auto all = running_work();
    CHECK(all.is_array());
    Activity a{"image", "/p/two", "", "正在画参考图"};
    for (const auto& row : running_work()) {
        CHECK(row.contains("target"));
    }
}

// ---------------------------------------------------------------------------
// 任务账本：排队中 → 正在做 → 做完的
// ---------------------------------------------------------------------------

TEST_CASE("任务账本：三个状态各归各位，耗时按自己那一段算") {
    // 用户 2026-09-17：「任务页面显示正在做的（已经用时…）、排队中的
    // （…取消图标按钮）、已经做完的（耗时）」。
    //
    // **耗时这一条钉得最紧**：出图那一批第一版是整批收尾时一起结账的，
    // 六张图的耗时于是一模一样（实测全是 96.9 秒），而真实是三十几秒到
    // 七十几秒不等。结账必须是"这一件干完那一下"。
    const std::string proj = "/tmp/任务账本用例";

    {
        pipeline::Task queued{"image", "画参考图 · 甲 正面", proj};
        pipeline::Task running{"tts", "配音 · sh001", proj};
        running.begin();

        const auto b = pipeline::task_board(proj);
        REQUIRE(b.at("queued").size() == 1);
        REQUIRE(b.at("running").size() == 1);
        CHECK(b.at("queued")[0].at("title") == "画参考图 · 甲 正面");
        CHECK(b.at("running")[0].at("title") == "配音 · sh001");
        // 排着的那一行要说得出"排了多久"——等得久的时候人想知道它是不是
        // 被忘了。
        CHECK(b.at("queued")[0].contains("waited"));
        // 正在做的不报"排了多久"：它已经不排了。
        CHECK_FALSE(b.at("running")[0].contains("waited"));
    }

    // 两件都出了作用域 = 都结账了。没 begin 过的那件记成"取消了"。
    const auto after = pipeline::task_board(proj);
    CHECK(after.at("queued").empty());
    CHECK(after.at("running").empty());
    REQUIRE(after.at("done").size() == 2);
    // **倒着报：最后结账那件在最上面。** 这两件是按声明的反序析构的
    //（`running` 声明在后、先析构），所以最后结账的是那件排着没开工的。
    CHECK(after.at("done")[0].at("title") == "画参考图 · 甲 正面");
    CHECK(after.at("done")[0].at("state") == "cancelled");
    CHECK(after.at("done")[1].at("title") == "配音 · sh001");
    CHECK(after.at("done")[1].at("state") == "done");
}

TEST_CASE("任务账本：人按的停不是失败") {
    const std::string proj = "/tmp/任务账本用例2";
    {
        pipeline::Task t{"image", "画参考图 · 乙 背面", proj};
        t.begin();
        CHECK_FALSE(t.cancelled());
        CHECK(pipeline::cancel_task(t.id()));
        CHECK(t.cancelled());
        // 底下那一层会把取消抛成一句「已停下这一张」，照样落进 error。
        t.fail("已停下这一张");
    }
    const auto b = pipeline::task_board(proj);
    REQUIRE(b.at("done").size() == 1);
    // **有 error 也算"停下了"，不是"失败"。** 报成红的话人会去找哪儿出错
    // 了，而什么都没出错——和 util/cancel_words.hpp 那条是同一件事。
    CHECK(b.at("done")[0].at("state") == "cancelled");
}

TEST_CASE("任务账本：上一级停了，每一件跟着停") {
    // 「停一件」和「停一整批」要能同时成立：任务页面上每一行有自己的叉，
    // 而整批那头还有一个停。没有 link 的话两者只能选一个。
    pipeline::CancelToken batch;
    pipeline::Task a{"image", "第一张", "/tmp/任务账本用例3"};
    pipeline::Task b{"image", "第二张", "/tmp/任务账本用例3"};
    a.token().link(&batch);
    b.token().link(&batch);

    CHECK(pipeline::cancel_task(a.id()));
    CHECK(a.cancelled());
    CHECK_FALSE(b.cancelled());   // 停一件不碰别的

    batch.request();
    CHECK(b.cancelled());         // 停整批，剩下的跟着停
}

TEST_CASE("任务账本：思考接起来存，取的时候是整份") {
    const std::string proj = "/tmp/任务账本用例4";
    std::uint64_t id = 0;
    {
        pipeline::Task t{"llm", "写第三章", proj};
        t.begin();
        id = t.id();
        t.append_thinking("先想");
        t.append_thinking("再想");
        // 账本那份**只报有没有**，不报正文——一次写作的思考几千字，而这份
        // 东西两秒推一次。正文点开才单独取。
        const auto b = pipeline::task_board(proj);
        REQUIRE(b.at("running").size() == 1);
        CHECK(b.at("running")[0].at("thinking") == true);
        CHECK(pipeline::task_thinking(id).text == "先想再想");
    }
    // 结完账也还找得到：页面上「做完的」那几行也能点开看。
    CHECK(pipeline::task_thinking(id).text == "先想再想");
}

TEST_CASE("任务账本：预计等多久按「这是哪一族」算，不按 kind") {
    // `image` 这一族里既有「画参考图 · …」也有「出首帧 · …」，两者的耗时
    // 不是一个量级。混一个桶算出来的中位数报给谁都不对。
    const std::string proj = "/tmp/任务账本用例5";
    {
        pipeline::Task ref{"image", "画参考图 · 甲 正面", proj};
        ref.begin();
    }
    {
        // 同一个 kind、另一族。**它没跑过，所以报不出预计**——报得出的话
        // 说明它借了参考图那一族的数。
        pipeline::Task frame{"image", "出首帧 · sh001", proj};
        const auto b = pipeline::task_board(proj);
        REQUIRE(b.at("queued").size() == 1);
        CHECK(b.at("queued")[0].at("eta") == 0.0);
    }
}

TEST_CASE("思考按段取：只给新增，断了一截要说得出来") {
    // 用户 2026-09-17：「在任务里思考没有实时显示最后内容」。整份重取的话，
    // 一件十五万字的活每一拍就要搬十五万字过去——页面卡在那儿，而人要的是
    // **最新那一段**，不是从头。
    const std::string proj = "/tmp/任务账本用例6";
    pipeline::Task t{"llm", "写第三章", proj};
    t.begin();
    t.append_thinking("第一段");
    const auto a = pipeline::task_thinking(t.id(), 0);
    CHECK(a.start == 0);
    CHECK(a.text == "第一段");
    CHECK(a.end == std::string("第一段").size());

    t.append_thinking("第二段");
    // 拿上一次的 end 当 from，只回新增的那一段。
    const auto b = pipeline::task_thinking(t.id(), a.end);
    CHECK(b.start == a.end);
    CHECK(b.text == "第二段");

    // 已经取到头了就回空，不重复给。
    const auto c = pipeline::task_thinking(t.id(), b.end);
    CHECK(c.text.empty());
    CHECK(c.start == c.end);

    // 问一个比现在还靠后的位置（不该发生，但别让它回出负数长度的段）
    const auto d = pipeline::task_thinking(t.id(), b.end + 999);
    CHECK(d.text.empty());
}

TEST_CASE("思考超了上限从头上截：截口落在字的边界上，读回来是合法的 UTF-8") {
    // 原来按字节截：六万多个汉字的思考，截口劈在一个字中间，留下来那一截开头是
    // 半个字——/api/task/thinking 序列化时 nlohmann 当场抛（2026-09-25 对话那头
    // 同一种截法撞上 `invalid UTF-8 byte at index 0`）。
    const std::string proj = "/tmp/任务账本用例截口";
    pipeline::Task t{"llm", "拆分镜", proj};
    t.begin();
    std::string chunk;
    for (int i = 0; i < 1000; ++i) chunk += "想";   // 3000 字节一段
    for (int i = 0; i < 67; ++i) t.append_thinking(chunk);   // 201000 字节：超了上限
    t.append_thinking("x");                                 // 再挪一个字节，截口落进字里
    const auto got = pipeline::task_thinking(t.id(), 0);
    REQUIRE_FALSE(got.text.empty());
    CHECK(got.text.rfind("想", 0) == 0);               // 开头是一整个字
    CHECK_NOTHROW((void)nlohmann::json(got.text).dump());

    std::string s = "ab想想";
    CHECK(changji::text::keep_tail_utf8(s, 4) == 5);   // 留 4 个字节会劈开第一个「想」：挪到下一个字
    CHECK(s == "想");
    std::string short_one = "想";
    CHECK(changji::text::keep_tail_utf8(short_one, 10) == 0);
    CHECK(short_one == "想");
}

// ---------------------------------------------------------------------------
// 账本那一行的叉，接到真正在干活的那个令牌上
//
// 2026-09-17 实测的坏样子：点了"结束这一件"，行上写着"正在停…"，而那条
// 大纲又跑了一分半，思考字数从三万三涨到三万五。断在两个不同的令牌上——
// 叉点的是 Activity 自带那个，大模型查的是外面传进来那个。
// ---------------------------------------------------------------------------

TEST_CASE("接上之后，停账本那一行就等于停手上的活") {
    pipeline::CancelToken worker;
    Activity act{"outline", "/tmp/p", "", "正在出大纲"};
    const pipeline::CancelLink link{worker, act};

    CHECK_FALSE(worker.cancelled());

    // 任务页面上那个叉点的就是这个。
    act.task().token().request();

    // 干活那头查的是自己手上这个 —— 现在它也该说停。
    CHECK(worker.cancelled());
}

TEST_CASE("Activity 走了之后，手上那个令牌不能再攥着它") {
    // ⚠️ 这条钉的是**野指针**，不是行为。
    //
    // 同步那一支的 worker 令牌是 script_route 里的 static thread_local，
    // 活得比 Activity 久得多。不摘钩的话它会攥着一个已经析构的令牌，
    // 下一个请求落到同一条线程上就是读已经释放的内存。
    pipeline::CancelToken worker;
    {
        Activity act{"outline", "/tmp/p", "", "正在出大纲"};
        const pipeline::CancelLink link{worker, act};
        CHECK_FALSE(worker.cancelled());
    }
    // 两个都没了，这一问不能碰到已经析构的那块。
    CHECK_FALSE(worker.cancelled());

    // 而且还能正常再用一次：自己被点亮还是算停。
    worker.request();
    CHECK(worker.cancelled());
}

TEST_CASE("没接的时候，停账本那一行停不了手上的活") {
    // 这就是 2026-09-17 那条大纲的处境，留着当对照。
    pipeline::CancelToken worker;
    Activity act{"outline", "/tmp/p", "", "正在出大纲"};

    act.task().token().request();
    CHECK_FALSE(worker.cancelled());
}

TEST_CASE("思考那个数报的是字，不是字节") {
    // 页面上那句写的是"想了 N 字"。std::string::size() 数字节，中文一个字
    // 三个字节——照字节报的话当场虚报三倍。
    // 2026-09-17 实测一条大纲：账本报 101817，真实 66277 个字。
    Activity act{"outline", "/tmp/p", "", "正在出大纲"};
    act.set_thinking("这一句十个字啊");   // 7 个字，21 字节

    // **这个数在账本那一份里，不在顶栏那一份里**：running_activities()
    // 报的是顶栏要的形状（没有 title 也没有 thinking_chars）。
    const auto board = pipeline::task_board("/tmp/p");
    bool seen = false;
    for (const auto& r : board.value("running", nlohmann::json::array())) {
        if (r.value("title", std::string{}) != "正在出大纲") continue;
        seen = true;
        CHECK(r.value("thinking", false));
        CHECK(r.value("thinking_chars", 0) == 7);   // 不是 21
    }
    CHECK(seen);
}

// ---------------------------------------------------------------------------
// 任务页面上长跑那一行的叉
//
// 短活那一族靠 CancelLink 接好了。长跑这一族的链接方向是**反的**：
// jobs.cpp 里 `act.task().token().link(&slot.token)` 管的是"顶栏按停也能停
// 这一行"，而任务页面上那一行的叉点的是行自己的令牌，长跑的 worker 查的是
// 槽上那个，从来不读行上这个。
//
// 2026-09-17 实测的坏样子：批量补分镜按了叉，行上写着"正在停…"，三十多分钟
// 一直跑到自己结束。长跑正是最需要能停的那一种。
// ---------------------------------------------------------------------------

TEST_CASE("长跑那一行的叉，按下去要真的停") {
    auto& jt = pipeline::jobs();
    std::atomic<bool> saw_cancel{false};
    std::atomic<bool> started{false};

    REQUIRE(jt.start(
        pipeline::JobKind::Write, "",
        [&](pipeline::JobProgress& p) {
            started = true;
            // 真实的长跑就是这个形状：每个检查点问一次 p.cancelled()。
            for (int i = 0; i < 2000; ++i) {
                if (p.cancelled()) {
                    saw_cancel = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        },
        "停了", "/tmp/cancel-long", "写全片正文"));

    // **等它登记上账本再找那一行。** 起线程和开 Activity 之间有个窗口，
    // 上来就查会偶发查不到（第一版就是这么飘的）。
    std::uint64_t row_id = 0;
    for (int i = 0; i < 400 && row_id == 0; ++i) {
        // ⚠️ **先落地再遍历。** `task_board()` 回的是临时对象，
        // `for (auto& r : task_board(...).at("running"))` 绑的是它内部的
        // 引用，而那个临时在整条表达式结束时就析构了——遍历的是悬空内存，
        // 表现是"行明明在账本里，却一条都数不出来"。
        const nlohmann::json board = pipeline::task_board("/tmp/cancel-long");
        for (const auto& r : board.at("running")) {
            if (r.value("title", std::string{}) == "写全片正文") {
                row_id = r.value("id", std::uint64_t{0});
            }
        }
        if (row_id == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(started);
    REQUIRE(row_id != 0);

    // 这一下就是任务页面上那个叉。
    CHECK(pipeline::cancel_task(row_id));

    for (int i = 0; i < 400 && !saw_cancel; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(saw_cancel);
    jt.wait_idle();
}

TEST_CASE("领到了不等于轮到了：等位置的那几件不算在跑") {
    // 用户 2026-09-20：「任务在做的明明只有一个，任务列表里和顶部任务都是
    // 显示 3 个」。出图出片那几路比真正的位置多开一路（infer::pool_lanes：
    // 跨机的每台再加一路，为的是拉产物那几十秒里把下一件先派出去），多出来
    // 那一路会先把下一镜领走、然后在池子上等。
    //
    // **「派出去了」不等于「正在画」**（CLAUDE.md 第九条）。实测那一屏：
    // sh002 的进度 0/6→1/6 在动，sh003 五分钟里一直是 0/0，而两行都在走秒。
    // **按项目过滤着看。** 这个文件里别的用例也往同一本账上记，不挑项目
    // 的话数到的是它们的行（`task_board("")` 回的是所有项目的）。
    const std::string proj = "/tmp/lane-vs-slot";
    pipeline::Task painting("video", "出片成片档 · ep01_sh002", proj);
    pipeline::Task waiting("video", "出片成片档 · ep01_sh003", proj);
    painting.begin(/*working=*/false);
    waiting.begin(/*working=*/false);

    const auto rows = [&proj] {
        const nlohmann::json b = pipeline::task_board(proj);
        std::map<std::string, nlohmann::json> by_title;
        for (const auto& r : b.at("running")) {
            by_title[r.at("title").get<std::string>()] = r;
        }
        return by_title;
    };

    // 刚领到：两件都还没开始画，那一行要说清在等什么
    CHECK(rows().at("出片成片档 · ep01_sh002").at("working") == false);
    CHECK_FALSE(rows().at("出片成片档 · ep01_sh003").at("note").get<std::string>().empty());

    // 对面开口了（读权重那一声也算）：这一件转正，等位置那句话跟着清掉
    painting.working_now();
    CHECK(rows().at("出片成片档 · ep01_sh002").at("working") == true);
    CHECK(rows().at("出片成片档 · ep01_sh002").at("note").get<std::string>().empty());
    // 另一件没动，还在等
    CHECK(rows().at("出片成片档 · ep01_sh003").at("working") == false);

    // 报进度同样算转正——两条路都要通（远端只报采样、本机先报准备）
    waiting.set_progress(1, 6);
    CHECK(rows().at("出片成片档 · ep01_sh003").at("working") == true);
}

TEST_CASE("长跑那一行是个壳，数「几件在跑」时不和它的孩子一起数") {
    // 用户报的那 3 件里，有一件是「预告 · ep01 · 前 30 秒」——它底下那两行
    // 就是它的孩子。判据在前端一处（api/labels.js 的 countRunning），引擎
    // 这头负责把 `long_job` 报出去，两份快照（任务账、顶栏那份）都得有。
    const std::string proj = "/tmp/long-job-shell";
    pipeline::Task shell("run", "预告 · ep01 · 前 30 秒", proj);
    shell.begin();
    shell.mark_long_job();

    const nlohmann::json b = pipeline::task_board(proj);
    bool found = false;
    for (const auto& r : b.at("running")) {
        if (r.at("title") == "预告 · ep01 · 前 30 秒") {
            found = true;
            CHECK(r.at("long_job") == true);
        }
    }
    CHECK(found);
}

TEST_CASE("退休的行不再问令牌，而活着的行照问") {
    // **顺序是要害。** `cancelling` 这一行原来写成「令牌被立起来了 && 还活着」
    // ——于是退休的行也要问一次令牌。而一件活的令牌可以挂在整批那个令牌上
    // （"停一件"和"停一整批"要同时成立），这一行却要在「做完的」里留两百条，
    // 比整批那个令牌（多半在栈上）活得久：拼这份 JSON 时顺着野指针往下问，
    // 整个引擎段错误。2026-09-20 写上面那两条用例时撞见的。
    //
    // 这一条钉的是**还活着的行照旧要问**——那才是这个字段的用处，而"退休的
    // 不问"如果顺手把它也关掉了，页面上那个叉按下去就再也不会变成"取消中"。
    const std::string proj = "/tmp/取消中用例";
    pipeline::CancelToken batch;
    pipeline::Task live{"image", "还在跑的", proj};
    live.token().link(&batch);
    live.begin();

    const auto row_of = [&proj](const char* bucket, const char* title) {
        const nlohmann::json b = pipeline::task_board(proj);
        for (const auto& r : b.at(bucket)) {
            if (r.at("title") == title) return r;
        }
        return nlohmann::json{};
    };

    CHECK(row_of("running", "还在跑的").at("cancelling") == false);
    batch.request();   // 整批停
    // 活着的行要跟着说"取消中"——按了叉之后它还在名单上待一会儿，
    // 不说的话看着像那个叉没按上。
    CHECK(row_of("running", "还在跑的").at("cancelling") == true);
}
