// 后台那件活的"当前 stream"和"取消令牌"。
//
// **这两样都是线程局部的**，而它们错了的表现都不报错：
//   stream 串了  → 思考推到上一件活的频道上，界面上张冠李戴；
//   令牌漏登记   → 界面上那个「停下」按下去没反应，而接口回的是 200。
// 所以单独钉一遍。

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>

#include "http/job_stream.hpp"

using namespace changji;

TEST_CASE("JobScope：当前 stream 挂上、出去就摘掉") {
    CHECK(http::current_stream().empty());
    {
        const http::JobScope a{"job-1"};
        CHECK(http::current_stream() == "job-1");
        {
            // 嵌套：里层出去之后要还原成外层那个，不是清空。
            // 一件活里再起一件（批量那条）就是这个形状。
            const http::JobScope b{"job-2"};
            CHECK(http::current_stream() == "job-2");
        }
        CHECK(http::current_stream() == "job-1");
    }
    CHECK(http::current_stream().empty());
}

TEST_CASE("JobScope：抛出去也要摘干净") {
    // 处理函数里抛异常的路径有好几条。漏一条的后果是下一件活把思考推到
    // 上一件的频道上，而那不报错。
    try {
        const http::JobScope s{"job-throw"};
        CHECK(http::current_stream() == "job-throw");
        throw std::runtime_error("砸了");
    } catch (const std::exception&) {
    }
    CHECK(http::current_stream().empty());
}

TEST_CASE("按 stream 把活停掉") {
    SUBCASE("在跑的那件停得掉") {
        const http::JobScope s{"job-a"};
        CHECK_FALSE(http::current_cancel().cancelled());
        CHECK(http::cancel_job("job-a"));
        CHECK(http::current_cancel().cancelled());
    }

    SUBCASE("找不到不是错——按下去那一刻可能刚好干完") {
        CHECK_FALSE(http::cancel_job("根本没这条"));
        CHECK_FALSE(http::cancel_job(""));
    }

    SUBCASE("活干完了就停不到了，也不崩") {
        // 令牌活在那条线程的栈上。表里那一条和 JobScope 成对加减，
        // 所以这儿不会是个悬空指针。
        { const http::JobScope s{"job-b"}; }
        CHECK_FALSE(http::cancel_job("job-b"));
    }

    SUBCASE("同一个 id 被重用：停的是现在这件") {
        // 用户连点两下、前一件还没退干净时就是这个形状。析构时按地址比过
        // 再删，所以后来那件的登记不会被前一件抹掉。
        const http::JobScope outer{"job-same"};
        {
            const http::JobScope inner{"job-same"};
            CHECK(http::cancel_job("job-same"));
            CHECK(http::current_cancel().cancelled());
        }
        // 里层退了，外层那条还登记着，而且是另一个令牌（没被停过）
        CHECK_FALSE(http::current_cancel().cancelled());
        CHECK(http::cancel_job("job-same"));
        CHECK(http::current_cancel().cancelled());
    }
}

TEST_CASE("没有 JobScope 时给个哑元，别让调用方自己 new") {
    // 同步那条路和单测直接调都落在这儿。要点是**不能崩**，
    // 也不能把别人那件活停掉。
    CHECK(http::current_stream().empty());
    auto& t = http::current_cancel();
    t.reset();
    CHECK_FALSE(t.cancelled());
}

TEST_CASE("两条线程各记各的，不串") {
    // 同时写两章是常态。串了的话 A 的思考会推到 B 的频道上。
    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread a([&] {
        const http::JobScope s{"t-a"};
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ok_a = http::current_stream() == "t-a";
    });
    std::thread b([&] {
        const http::JobScope s{"t-b"};
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ok_b = http::current_stream() == "t-b";
    });
    a.join();
    b.join();
    CHECK(ok_a);
    CHECK(ok_b);
    CHECK(http::current_stream().empty());
}

TEST_CASE("借外面那个令牌：批量那几条真正管事的是 JobProgress::token()") {
    // **这条钉的是"停下按了没反应"那一类。**
    //
    // 批量（写全片、展开正文、批量补分镜）的形状是：JobTable 起一条 job，
    // 循环里查 `p.cancelled()`、给大模型的是 `p.token()`；而 JobScope 是
    // 挂上去让思考流和顶栏那个「停下」找得到它的。
    //
    // 两者原来不是同一个令牌：`cancel_job` 点亮的是 JobScope 自带的那个，
    // 而循环和大模型读的是 job 的那个——**接口照回 {"stopped": true}，活
    // 一秒没停**。顶栏那块「AI 作业中」是从设定页点「批量补分镜」之后唯一
    // 看得见的出口（那一页自己的提示就写着去那儿看），按下去没反应就是
    // "点了运行之后取消不掉"。
    pipeline::CancelToken job;      // 相当于 JobProgress::token()
    {
        const http::JobScope scope{"write-batch", job};
        CHECK_FALSE(job.cancelled());
        CHECK(http::cancel_job("write-batch"));
        CHECK(job.cancelled());     // 点亮的必须是这一个
        // 借来的时候，线程上那个 current_cancel() 也要是同一个，
        // 不然大模型那条路（server.cpp 那几个 handler 读的就是它）
        // 和循环这条又分成两个。
        CHECK(http::current_cancel().cancelled());
    }
    // 出去之后表里不该还留着它
    CHECK_FALSE(http::cancel_job("write-batch"));

    // **对照组：自带令牌那个构造函数点的是它自己那个。** 这正是批量原来
    // 的样子——外面那个（循环和大模型真正在读的）一动不动。
    pipeline::CancelToken outside;
    {
        const http::JobScope own{"own-token"};
        CHECK(http::cancel_job("own-token"));
        CHECK(http::current_cancel().cancelled());   // 自带那个亮了
        CHECK_FALSE(outside.cancelled());            // 而外面那个没人碰
    }
}

// ---------------------------------------------------------------------------
// 信箱：连不上 WebSocket 的时候，同一批消息改成拿 HTTP 来取
// ---------------------------------------------------------------------------
//
// 这一层错了的表现都不报错，只是"界面上一直转着"：
//   没开就存    → 每件走 socket 的活白攒一份，思考一段上万字；
//   结果被丢掉  → 前端永远等不到 job_done；
//   销号太早    → 取到一半信箱没了，当成"连接断了"。

TEST_CASE("信箱：没开过就不存，取的时候明说没有") {
    http::job_progress("no-box", 1, 3, "一");
    const auto r = http::mail_take("no-box", 0);
    CHECK_FALSE(r.at("exists").get<bool>());
    CHECK(r.at("events").empty());
}

TEST_CASE("信箱：开了之后进度和思考都留底，取过的不再给") {
    http::mail_open("box-1");
    http::job_progress("box-1", 1, 3, "一");
    http::job_thinking("box-1", "先想想");

    auto r = http::mail_take("box-1", 0);
    CHECK(r.at("exists").get<bool>());
    CHECK(r.at("events").size() == 2);
    CHECK(r.at("events")[0].at("type") == "job_progress");
    CHECK(r.at("events")[0].at("job_id") == "box-1");
    CHECK(r.at("events")[1].at("text") == "先想想");
    CHECK_FALSE(r.at("done").get<bool>());
    const std::size_t next = r.at("next").get<std::size_t>();
    CHECK(next == 2);

    // 没有新的就该是空的——轮询是一秒一次，重发已取过的等于把那段思考
    // 再拼一遍，界面上会看见重复的字。
    r = http::mail_take("box-1", next);
    CHECK(r.at("events").empty());
    CHECK(r.at("next").get<std::size_t>() == next);

    http::job_thinking("box-1", "想好了");
    r = http::mail_take("box-1", next);
    CHECK(r.at("events").size() == 1);
    CHECK(r.at("events")[0].at("text") == "想好了");
}

TEST_CASE("信箱：结果送到就销号，再取是「没有这个信箱」") {
    http::mail_open("box-done");
    http::job_done("box-done", {{"shots", 7}});
    auto r = http::mail_take("box-done", 0);
    CHECK(r.at("done").get<bool>());
    CHECK(r.at("events").size() == 1);
    CHECK(r.at("events")[0].at("result").at("shots") == 7);
    // 送一次就够了。留着只会等着过期，而结果一份可能是整章剧本。
    r = http::mail_take("box-done", 0);
    CHECK_FALSE(r.at("exists").get<bool>());

    http::mail_open("box-err");
    http::job_error("box-err", "显存不够");
    r = http::mail_take("box-err", 0);
    CHECK(r.at("done").get<bool>());
    CHECK(r.at("events")[0].at("message") == "显存不够");
}

TEST_CASE("信箱：预览图不留底") {
    // 一张几十 KB、一步一张。存起来这个信箱就成了主要流量——
    // 理由和 job_preview 头上那句"只广播，不留底"是同一条。
    http::mail_open("box-prev");
    http::job_preview("box-prev", 3, "data:image/png;base64,AAAA");
    http::job_progress("box-prev", 3, 20, "");
    const auto r = http::mail_take("box-prev", 0);
    CHECK(r.at("events").size() == 1);
    CHECK(r.at("events")[0].at("type") == "job_progress");
}

TEST_CASE("信箱：攒太多从前面丢，但结果那条一定留着") {
    http::mail_open("box-flood");
    // 思考是一段一条推的。**丢头不丢尾**：尾巴上那条才是结果，
    // 丢掉它的话前端永远等不到 job_done，界面一直转着。
    for (std::size_t i = 0; i < http::kMailMaxEvents + 50; ++i) {
        http::job_thinking("box-flood", "想");
    }
    http::job_done("box-flood", {{"ok", true}});
    const auto r = http::mail_take("box-flood", 0);
    CHECK(r.at("dropped").get<std::size_t>() > 0);
    CHECK(r.at("events").size() <= http::kMailMaxEvents);
    CHECK(r.at("events").back().at("type") == "job_done");

    // 丢过之后，`since` 落在没了的那一段里：从现有的头上接着给，
    // 别越界、别把 next 倒回去。
    http::mail_open("box-drop");
    for (std::size_t i = 0; i < http::kMailMaxEvents + 10; ++i) {
        http::job_thinking("box-drop", "想");
    }
    const auto r2 = http::mail_take("box-drop", 0);
    CHECK(r2.at("events").size() <= http::kMailMaxEvents);
    CHECK(r2.at("next").get<std::size_t>() == http::kMailMaxEvents + 10);
}

TEST_CASE("信箱：各步自己那几种消息（story_token 这些）也要留底") {
    // 写大纲 / 写正文 / 改一段那三步有自己的消息形状，原来是直接
    // `ws::hub().broadcast` 的——也就是说没有 socket 的时候一个字都到不了，
    // 而它们恰恰是思考最久、最需要看见"它在动"的三步。
    http::mail_open("box-story");
    http::job_relay("box-story", {{"type", "story_token"},
                                  {"job_id", "box-story"},
                                  {"seq", 0},
                                  {"text", "雨砸在天台上。"}});
    http::job_relay("box-story", {{"type", "outline_progress"},
                                  {"job_id", "box-story"},
                                  {"raw_chars", 12}});
    const auto r = http::mail_take("box-story", 0);
    CHECK(r.at("events").size() == 2);
    CHECK(r.at("events")[0].at("text") == "雨砸在天台上。");
    CHECK(r.at("events")[1].at("raw_chars") == 12);
    // 这两种都不是收尾消息，别把信箱销早了——后面还有 job_done 要送。
    CHECK_FALSE(r.at("done").get<bool>());
    CHECK(http::mail_take("box-story", 2).at("exists").get<bool>());
}
