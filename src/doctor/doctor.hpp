// 环境体检。
//
// 跑之前先把所有外部依赖查一遍，缺什么直接说清楚怎么补。
// 目标是让用户永远不会在跑到一半的时候才发现缺东西。
//
// 对齐 Python 侧 src/changji/doctor.py。响应结构体必须完全一致：
// 前端只认 can_run 和 checks 数组里的 name/level/detail，检查项本身可以增删。

#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <vector>

#include "config/settings.hpp"

namespace changji::doctor {

enum class Level { OK, WARN, FAIL };

/// 序列化用的字符串，必须是 "ok" / "warn" / "fail"——前端按这三个值上色。
/// 定义放在头里，不放 .cpp。
///
/// doctor.cpp 链 httplib（体检要去连推理服务和大模型），而这两个
/// 是纯函数，别的地方（比如 /api/connections 组装响应）用得到。
/// 放 .cpp 的话那些地方就得跟着链 httplib，单元测试目标也要——
/// 为两个 switch 拖进一个 HTTP 库不值得。
inline const char* to_string(Level level) {
    switch (level) {
        case Level::OK:   return "ok";
        case Level::WARN: return "warn";
        case Level::FAIL: return "fail";
    }
    return "fail";
}

struct Check {
    std::string name;
    Level level = Level::OK;
    std::string detail;
    std::string fix;  ///< 怎么补。空表示不需要动作

    /// 要补的是哪一组模型（`tts` / `image` / `video` / `llm` / `image_base`）。
    /// 空 = 这一项和模型无关。
    ///
    /// ⚠️ **这一栏是给界面用的，不是给人读的。**
    ///
    /// 那几条没过的项，`fix` 里写的都是「填 `[models].tts`」「检查 `[models]`
    /// 里的文件名」——**叫人去手改配置文件**，而产品里本来就有一整套挑模型、
    /// 下模型的窗。2026-09-17 实测一台什么都没装的机器：三条没过的项，三条
    /// 都在说配置键名，没有一条给得出"点这儿"。
    ///
    /// 有了这一栏，设置页就能摆一颗直接跳到那一组模型窗的按钮。**别让界面
    /// 去正则匹配 `fix` 里的中文**——那几句话一直在调，匹配挂了不会报错，
    /// 只会悄悄少一颗按钮（同 `Activity::queued` 那条：在跑还是在排给个字段，
    /// 别让前端去猜那句话）。
    std::string group;
};

struct Report {
    std::vector<Check> checks;

    /// 能不能跑完整流程。有任何一项 FAIL 就是不能。
    /// 有没有 FAIL。同样放在头里，理由见上面 to_string。
    bool can_run() const {
        for (const auto& c : checks) {
            if (c.level == Level::FAIL) return false;
        }
        return true;
    }
};

/// 跑一遍全部检查。
///
/// 注意这些检查是串行的，其中三项要发网络请求，各自超时 8 秒，
/// 最坏情况这个函数会阻塞二十多秒。Python 侧也是同样的行为。
/// TODO(阶段 2): 三个网络检查改成并发，把最坏情况压到单次超时。
Report run_checks(const config::Settings& settings);

/// 本机没编 sd.cpp 时，「出图后端」这一项该报哪一档。
///
/// **两样都派得出去才降级。** 只有一样的话另一样仍然没人能干，而出片这条
/// 链少哪一段都走不完——降成 WARN 等于把按钮点亮，让人跑二十分钟撞在
/// 后半截上。
///
/// 单拎出来只为能测：真正的判据（机器表里那几台在不在线、报了哪些能力）
/// 要一台真机器才答得出，而这一句"两个都要"是纯的。
Level sd_level_without_local(bool frame_elsewhere, bool video_elsewhere);

/// 体检报告的 JSON 形状。**只此一份。**
///
/// ⚠️ 2026-09-17 之前这段在两处各写了一遍（`http/server.cpp` 的 `to_json`
/// 和 `http/config_api.cpp` 的 `checks_json`），字段列表一模一样。加一栏
/// （`group`）就要改两处，而漏改一处**不会报错**——只是那一条路上的界面
/// 少一颗按钮。CLAUDE.md 第八条说的就是这个。
nlohmann::json to_json(const Report& r);

}  // namespace changji::doctor
