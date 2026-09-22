#pragma once

// 局域网上看见了谁，谁能用这台机器。
//
// **两件事分开：看见 ≠ 能用。**
//
// mDNS 一开，同一个网段里所有开着感知的机器互相都看得见——办公室、咖啡馆、
// 学校宿舍。看见就能派活的话，任何人只要打开同一个程序就能占用你的显卡。
// 所以**默认一个权限都不给**：对方出现在表上，仅此而已；要用得这头的人
// 在设置里逐台点开。
//
// 给的权限分两档，因为它们的后果不是一回事：
//
//   · **给用**——对方可以往这台机器上派活（出图、出片、配音）。
//     代价是显卡和电。
//   · **给挑模型参数**——对方还可以指定用哪一份权重、跑几步。
//     代价是"这台机器上跑的到底是什么"不再由这头说了算：一个 40 步 2K 的
//     请求能把一张卡占住几十分钟，而机主看不出是谁点的。
//
// 所以第二档是第一档之上再单独开一次，**默认也是关的**。
//
// ⚠️ **这个文件不碰网络，也不碰 mDNS。** 它只管"表里有谁、谁被允许了什么"
// ——这一族全是边界（同一台换了 IP、下线又上线、权限给了又收），而那些错
// 在界面上看不出来，只会表现成"某天他突然用不了了"。收成纯的才验得了。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::lan {

/// 看见的一台。
struct Peer {
    /// 那台机器自报的身份。**认这个，不认地址**——同一台机器换个网络就
    /// 换个 IP，认地址的话权限跟着丢，人得重新点一遍。
    std::string id;
    std::string name;     ///< 说给人看的名字
    std::string host;     ///< 这会儿在哪儿（IP 或者主机名）
    int port = 0;
    /// 最后一次看见它是什么时候（毫秒）。**不在表上删**——下线的那台
    /// 要留着，不然权限和它一起没了。
    std::int64_t seen_at = 0;
    bool online = false;

    // ---- 反过来那一半：**他给了我什么** ----
    //
    // 上面那个 `Grant` 是"我给了他什么"。这两件事是独立的：他让我用他的卡，
    // 不等于我也让他用我的。

    /// 他发给我的那张票。空 = 还没给，或者问不着。
    std::string their_ticket;
    /// 他那张卡这会儿忙成什么样（0~100）。**-1 = 不知道**：他没给我用、
    /// 问不着，或者那台机器压根报不出这个数（苹果的卡就报不出）。
    double their_gpu = -1;
    /// 他那张卡的显存（GB）。**用了多少 / 一共多少**，0 = 不知道。
    ///
    /// **和占用分开记**：显存这两个数苹果的机器报得出来，而占用报不出
    /// ——合成一个"知不知道"的话，那种机器上两样都显示不了。
    double their_vram_used = 0;
    double their_vram_total = 0;
};

/// 给出去的权限。**两档，默认都关。**
struct Grant {
    bool use = false;     ///< 可以往这台机器派活
    bool params = false;  ///< 还可以指定模型和步数
    /// 这一条配的那张票。**对方拿它来敲门**，不是拿 id。
    ///
    /// ⚠️ **id 不能当凭据。** 它就写在 mDNS 的 TXT 记录里，同一个网段上
    /// 谁都读得到——拿 id 放行等于"谁报得出名字谁就能用"。票是给这一条
    /// 授权现生的一串随机数，收回授权就作废。
    ///
    /// **它挡不住同网段里冒充 id 去要票的人**（要票那一步也只认 id）——
    /// 那要靠对着念一串码那种办法，不在这一轮里。它挡住的是网段外面：
    /// 票不在 mDNS 上，出了这个网段就问不到。
    std::string ticket;
};

/// 表。
class PeerBook {
public:
    /// 看见一台（mDNS 那头每发现一次就叫一下）。
    ///
    /// **已经在表上就更新地址和时间**，不新增一行——同一台机器会被反复
    /// 报上来（换网卡、换端口、服务重启）。
    void saw(const Peer& p);

    /// 记下"他给了我什么"（票、那张卡忙成什么样）。
    ///
    /// **和 `saw` 分开**：那一条是 mDNS 报上来的（地址、名字），这一条是
    /// 主动去问回来的，两头节奏不一样，混在一起写会互相把对方的字段抹掉。
    void theirs(const std::string& id, const std::string& ticket, double gpu,
                double vram_used, double vram_total);

    /// 那台不见了（mDNS 报的 remove）。**只标成离线，不从表上拿掉**：
    /// 拿掉的话给过的权限跟着没了，而人过一会儿开机回来还得再点一遍。
    void lost(const std::string& id);

    /// 给权限 / 收权限。**只认 id**（见 `Peer::id`）。
    ///
    /// **票由这儿现生**：`g.ticket` 空着就配一张新的；已经有票的那一条
    /// 再改权限时**票不变**——换票等于把对面正在跑的活踢下线，而人只是
    /// 想多给一档。
    void allow(const std::string& id, const Grant& g);

    /// 这张票是谁的，给了什么。**票对不上回一个全空的 Grant**。
    ///
    /// 收活那一关拿它判：`by_ticket(t).use` 才放行。
    Grant by_ticket(const std::string& ticket) const;
    Grant grant_of(const std::string& id) const;

    /// 对方能不能往这台机器派活。
    bool may_use(const std::string& id) const { return grant_of(id).use; }
    /// 对方能不能指定模型和步数。**必须先有 `use`**——光给第二档不给第一档
    /// 是个说不通的状态，而它能从"先给了两档、后来只收回第一档"这么来。
    bool may_pick(const std::string& id) const {
        const auto g = grant_of(id);
        return g.use && g.params;
    }

    /// 表上现在有谁。**在线的排前面**，然后按名字——人找的多半是眼前
    /// 还活着的那几台。
    std::vector<Peer> list() const;

    /// 权限那一份（要落盘的就是它）。
    nlohmann::json grants_json() const;
    void load_grants(const nlohmann::json& j);

private:
    std::map<std::string, Peer> seen_;
    std::map<std::string, Grant> grants_;
};

/// 这台机器自报的那个 id。**一台一份，换网络不变**——存在用户数据目录里，
/// 没有就现生一个。
std::string my_id(const std::filesystem::path& data_dir);

}  // namespace changji::lan
