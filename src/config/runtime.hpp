#pragma once

// 进程内的当前配置。
//
// 存在的理由：有两个接口能在运行期改配置——/api/connections 换机器，
// /api/settings 调参数。改完要立刻生效，所以不能到处传 const Settings&。
//
// 线程安全：读的地方拿一份**拷贝**而不是引用。配置结构不大（几十个字段），
// 拷贝的代价远小于"某个请求正读到一半，另一个请求把它改了"那种问题——
// 后者的表现是一次请求里前后两处读到不同的配置，而且完全不可复现。

#include <map>
#include <mutex>
#include <optional>

#include "config/settings.hpp"
#include "models/hardware.hpp"
#include "stages/limits.hpp"

namespace changji::config {

class Runtime {
public:
    /// 当前配置的一份拷贝。
    Settings snapshot() const;
    void replace(Settings s);

    /// 画质档位的**进程内**覆盖。
    ///
    /// 不写回配置文件是刻意的：档位是按显存推出来的，写死在配置里等于
    /// 把这台机器的显存刻进项目，换台机器就不对了。所以重启即失效。
    ///
    /// ---
    ///
    /// Python 那边有一个同名的 _TIER_OVERRIDES 字典，但它**只写不读**——
    /// 用户改了草稿分辨率，接口回「已应用 草稿宽度」，而那个值存进一个
    /// 没有任何地方读的字典里，实际什么都没发生。
    ///
    /// 这里让它真的生效。这是**有意的偏离**：不生效的话，这个开关是在
    /// 骗用户。详见方案里那一节。
    void set_tier_override(models::Tier tier, const models::TierSpec& spec);
    void clear_tier_overrides();

    /// 应用了覆盖之后的硬件画像。/api/hardware 和 /api/settings 都用它。
    models::HardwareProfile profile() const;

private:
    mutable std::mutex mu_;
    Settings settings_;
    std::map<models::Tier, models::TierSpec> tier_overrides_;
};

/// 全局单例。main() 起服务前先 replace 一次。
Runtime& runtime();

/// 这份设置下，单镜能出多长：模型自己认的 ∩ 这张卡跑得动 ∩ 内核跑得动
/// ∩ 这部电影要的（`[video].max_shot_s`），再盖上 `[models].video_max_frames`
/// 那几项手填的覆盖。**只有这一处在做这个决定**：Runtime::replace 调它，
/// 出片和拆分镜那几条按项目重读设置的路也调它。
stages::VideoLimits video_limits_for(const Settings& s);

/// `stages::set_video_limits(video_limits_for(s))`。
///
/// 出片（http/run.cpp）和拆分镜（batch / episodes / planning）每次都按
/// **项目**重读设置（`load_settings(项目目录)`），根本不经过 Runtime；
/// 而单镜上限现在是项目的属性，所以这几条路进门先调一次这个。全局那一份
/// 是可变的，同时跑两个项目时后进的盖前面的——一次运行只跑一个项目的
/// 出片，拆分镜那几处最多影响正在渲的那一章对帧数的**夹低**，不会放开。
void apply_video_limits(const Settings& s);

}  // namespace changji::config
