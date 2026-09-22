#pragma once

// 把接口里那些"要干一两分钟"的活挪出 Crow 的 I/O 线程。
//
// **为什么需要。** Crow 一条 I/O 线程管着一批连接：handler 在它上面跑多久，
// 落在同一条线程上的连接就干等多久。2026-09-11 实测：写一章的五十几秒里，
// 落到被占住那条线程上的请求会卡满二十多秒，而顶栏、镜头墙、任务进度全在
// 那几个接口上。WebSocket 更惨——它是长连接，一旦落在那条上，顶栏那块表
// 会冻到这一章写完。
//
// ⚠️ **这一层只负责把活挪走，不负责把响应发回去。**
//
// 试过"活挪走、干完在那条线程上 `res.end()`"（Crow README 里的异步写法），
// 不行：那一串最后是 `asio::async_write`，而 socket 属于某条 I/O 线程的
// io_context，Crow 的 Connection 没有 strand。两边同时动一个 socket，
// 轻则响应发不出（客户端等满四十几秒拿到 000，服务端日志里 200 明明打了），
// 重则把进程带走。详见 server.hpp 里 concurrency 那段。
//
// 所以用法只有一种：**接口当场回一句"开始了"**，活在这儿干，进度和结果
// 走 WebSocket。响应始终由 Crow 自己那条线程发出。
//
// 池子是**有界**的。无界地开线程，一把点下去几十个请求就是几十条线程，
// 每条还都在等显存；有界的话多出来的排在队列里等。

#include <cstddef>
#include <functional>

namespace changji::http {

/// 后台干活的池子。进程一份。
class Offload {
public:
    /// 全局那一个。第一次用时起线程。
    static Offload& instance();

    /// 排一件活。池子满了就在队列里等。
    void post(std::function<void()> fn);

    /// 停下来：不再收新活，等在干的干完。优雅关停时调。
    /// **可能要等上几分钟**（写一章就是这个量级），那是对的——
    /// 不等就是在活干到一半的时候把它用的东西拆了。
    void stop();

    /// 现在有几件在干、几件在排。给诊断和测试用。
    std::size_t busy() const;
    std::size_t queued() const;

    /// 开几条线程。
    ///
    /// **不是按核数定的**：这些活基本不吃 CPU（GPU 在算，这条线程在等），
    /// 而且它们还要再排一次显存的队。八条是"一个人手快能同时点出几件"
    /// 的余量；再多也只是让更多人一起等显存。
    static constexpr std::size_t kThreads = 8;

private:
    Offload();
    ~Offload();
    Offload(const Offload&) = delete;
    Offload& operator=(const Offload&) = delete;

    struct Impl;
    Impl* impl_;
};

}  // namespace changji::http
