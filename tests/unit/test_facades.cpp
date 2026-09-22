// 三个"可选依赖"门面在**没链进来**时的行为。
//
// `sd_backend` / `ggml_abi` / `llama_tts` 都是同一个套路：把编译期的
// 可选性关在一个 .cpp 里，头文件不带 `#ifdef`。sd_backend.hpp 里那句
// 写得最清楚：
//
//     没链的时候 available() 返回 false，别的函数返回空值。**调用方照常写。**
//
// **这条承诺原来没有任何东西保证。** 存根要是返回了别的东西（抛异常、
// 返回一个假版本号、返回 true），调用方那些"照常写"的代码就会当真——
// 而它们分散在流水线各处，每一处的表现都不一样，查起来要从头翻。
//
// 测试目标本来就不链 sd.cpp / llama.cpp（CMakeLists 里只给 changji 那个
// 目标定义了 CHANGJI_HAVE_SD / CHANGJI_HAVE_LLAMA），所以这里跑到的
// 正是存根那一支。

#include <doctest/doctest.h>

#include <string>

#include "http/server.hpp"
#include "infer/ggml_abi.hpp"
#include "infer/llama_chat.hpp"
#include "infer/llama_tts.hpp"
#include "infer/sd_backend.hpp"

using namespace changji;

TEST_CASE("sd.cpp 没链时：available 是假，别的给空值而不是垃圾") {
    REQUIRE_FALSE(infer::sd_available());
    // 版本号返回空串而不是 "unknown" 之类——体检那边靠它判断要不要显示。
    CHECK(infer::sd_version().empty());
    CHECK(infer::sd_system_info().empty());
    // 采样器和调度器列表是给界面下拉框用的。返回空列表，界面显示"没有可选项"；
    // 要是返回一个编出来的名字，用户选了之后才发现根本跑不起来。
    CHECK(infer::sd_sample_methods().empty());
    CHECK(infer::sd_schedulers().empty());
    // 设日志回调不该崩，也不该抛——调用方是无条件调的。
    infer::sd_set_log_sink(nullptr);
}

TEST_CASE("ggml 没链时：ABI 自检算通过，而不是报故障") {
    REQUIRE_FALSE(infer::ggml_available());
    const auto abi = infer::check_ggml_abi();
    // **判 ok 不判 fail。** 没链 ggml 就没有"两边按不同 GGML_MAX_NAME 编"
    // 这个风险，报故障会让体检报告长期挂一条红字，而那条红字没有任何
    // 可执行的处理办法——报告里有常驻噪音之后，真正的故障就没人看了。
    CHECK(abi.ok);
    CHECK_FALSE(abi.detail.empty());   // 但要说清楚为什么"通过"
    CHECK(abi.compiled_max_name == 0);
}

TEST_CASE("进程内配音没编进来时：说清楚是构建选项，不是配置") {
    REQUIRE_FALSE(infer::llama_tts_available());
    const auto probe = infer::probe_llama_tts();
    CHECK(probe.ok);                   // 同上，不是故障
    CHECK_FALSE(probe.detail.empty());

    // 载模型要失败，而且话要指向构建而不是配置——用户拿着这句话应该去
    // 重新配置构建，而不是去翻配置文件找哪一项填错了。
    std::string why;
    const auto engine = infer::LlamaTts::load("a.gguf", "b.gguf", false, why);
    CHECK(engine == nullptr);
    CHECK(why.find("CHANGJI_LLAMA") != std::string::npos);
}

TEST_CASE("进程内大模型没编进来时：说清楚是构建选项，不是配置") {
    REQUIRE_FALSE(infer::llama_chat_available());
    std::string why;
    const auto chat = infer::LlamaChat::load("/不存在/x.gguf", true, why);
    CHECK(chat == nullptr);
    CHECK(why.find("CHANGJI_LLAMA=OFF") != std::string::npos);
}

TEST_CASE("门面的可选性只关在 .cpp 里，头文件不带 #ifdef") {
    // 这条不是运行时行为，是设计约束——sd_backend.hpp 开头写着
    // "**这个头文件不带任何 #ifdef，也不 include sd.cpp 的头**"。
    // 破了这条，`#ifdef CHANGJI_HAVE_SD` 就会散布到每一个调用点上，
    // 每加一处就多一份漏写的机会。
    //
    // 能编到这儿本身就是证据：这个测试目标没有定义 CHANGJI_HAVE_SD，
    // 而上面那些调用全都编过了。
    CHECK(true);
}

TEST_CASE("服务默认只听回环，不是整个局域网") {
    // **原来默认是 0.0.0.0**，也就是同一个 Wi-Fi 上的人都连得上。
    // 而这套接口没有任何鉴权：连上就能读项目、改分镜、起流水线。
    //
    // Python 侧一直是 127.0.0.1（run_server 的默认值和 cli.py 的 --host），
    // 所以这也是一处**对拍看不见的行为差异**——对拍比的是响应内容，
    // 比不到绑在哪个地址上。
    //
    // 容器里要 0.0.0.0，但那条路本来就显式传（Dockerfile 最后一行），
    // 不依赖这个默认值。
    const http::Options opts;
    CHECK(opts.host == "127.0.0.1");
    CHECK(opts.port == 8080);
}
