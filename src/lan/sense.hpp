#pragma once

// 局域网感知：往外报一声「这儿有一台」，同时听听还有谁。
//
// **默认关着**（用户说的）。开着才广播、才浏览——一个悄悄把自己报到整个
// 网段上的程序不该是默认行为，尤其在办公室和咖啡馆。
//
// ---
//
// **走系统自带的那一套，不引第三方。**
//
// macOS 上是 Bonjour（`<dns_sd.h>`，在 libSystem 里，什么都不用链）。
// **Linux 上是 Avahi 的 Bonjour 兼容层**（`libavahi-compat-libdnssd-dev`）
// ——函数名一样，所以 `sense.cpp` 两个平台共用一份；但**连接模型不一样**，
// 见那个文件里 `kSharedConn` 那一段。2026-09-22 在树莓派上真编真跑过：
// 两台互相看得见。
//
// **Windows 还没接**：那边要么装 Apple 的 Bonjour SDK（一个额外的安装包），
// 要么走 Win10 自带的 `DnsServiceRegister`（另一套 API，得另写一份）。
// 这条路上做半套比不做更糟，所以先空着——那时候 `supported()` 回 false，
// 界面照实说，**不摆一个开了没反应的开关**。
//
// ---
//
// ⚠️ **不要在引擎的线程上跑它。** Bonjour 那套是"给我一个 socket，
// 有事我通知你"，处理回调要自己转起来。这儿单开一条线程 select 那个 fd，
// 用一根自管道叫停——`DNSServiceRefDeallocate` 必须在同一条线程上做，
// 从外面直接释放会把正在回调里的那一次踩掉。
//
// ⚠️ **看见了也不等于能用。** 权限那一半在 `lan/peers.hpp`，
// 默认一个都不给。

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "lan/peers.hpp"

namespace changji::lan {

/// 这台机器上那一份。**进程里只有一个**。
class Sense {
public:
    static Sense& instance();

    /// 这个平台接上了没有。没接上时开关点不动，界面照实说。
    static bool supported();

    /// 把状态从盘上读回来（开没开、给过谁什么），并在开着时真的转起来。
    /// **进程起来时叫一次。**
    void boot(const std::filesystem::path& data_dir, int port);

    /// 开 / 关。**立刻生效，并且落盘**——它不是"这部电影怎么拍"，
    /// 是这台机器的一个状态，下次起来要照旧。
    void set_on(bool on);
    bool on() const;

    /// 这台机器自报的身份。
    std::string my_id() const;
    std::string my_name() const;

    /// 给权限 / 收权限。**落盘。**
    void allow(const std::string& id, const Grant& g);

    /// 表上现在有谁。
    std::vector<Peer> peers() const;
    Grant grant_of(const std::string& id) const;

    /// 某一台给我的那张票和它的地址。**给"拿它来算"那一下用**——
    /// 把它写进机器表时，票就是口令（那头的门认 `Bearer <票>`）。
    /// 没给过就回两个空串。
    std::pair<std::string, std::string> their_door(const std::string& id) const;

    /// 某一台在机器表上会长什么地址。**票给没给过都回**。
    ///
    /// 设置页要拿它比一句话：这一台**是不是已经在机器表上了**。
    /// ⚠️ **别在界面那头拼这个地址**：那就成了同一件事两处各写一遍
    ///（CLAUDE.md 第八条），而两处一分叉的表现是"明明在表上，页面上还
    /// 写着「拿它来算」"——按下去回 409，人只看见一个按不动的按钮。
    /// 问不着（不在表上、没有主机名或端口）回空串。
    std::string their_url(const std::string& id) const;

    /// 拿着这张票的，被允许了什么。**收活那一关拿它判**（见 `Grant::ticket`）。
    /// 票对不上回一个全空的 `Grant`。
    Grant by_ticket(const std::string& ticket) const;

    /// 回一整份给界面（`GET /api/lan` 就是它）。
    nlohmann::json to_json() const;

private:
    Sense() = default;
    ~Sense();
    Sense(const Sense&) = delete;
    Sense& operator=(const Sense&) = delete;

    /// 去问一圈：他们给我票了没有、那几张卡这会儿忙成什么样。
    ///
    /// **在那条 select 线程之外单开一条**：这几下是真发 HTTP 的，
    /// 一台连不上就要等超时，卡在 select 那条线程上会把 mDNS 的回调
    /// 一起堵住。
    void ask_around();
    void start();            ///< 真的开始广播 + 浏览
    void stop();             ///< 停下来并等那条线程收工
    void save() const;       ///< 落盘（开没开 + 权限）

    mutable std::mutex mu_;
    PeerBook book_;
    std::filesystem::path dir_;
    std::string id_;
    std::string name_;
    int port_ = 0;
    bool on_ = false;
    bool running_ = false;
    /// 转不起来时那句话（报不出去、听不见）。**咽下去的话界面一直显示
    /// "开着"，而整个网段上没人看得见这一台**，人只会以为"对方没开"。
    std::string trouble_;
    /// 去问一圈那条线程。
    std::thread asker_;
    bool asking_ = false;
    /// 上一次去敲某一台的门是什么时候（毫秒）。**被回绝过就退到一分钟
    /// 一次**——不退的话，一个二十台机器的网段上每三秒就是二十个注定 403
    /// 的请求，全网都在替我们白跑。
    std::map<std::string, std::int64_t> knocked_;

    struct Guts;             ///< 平台那一半（Bonjour 的几个 ref 和那条线程）
    std::unique_ptr<Guts> guts_;
};

}  // namespace changji::lan
