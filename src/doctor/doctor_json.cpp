// 体检报告 → JSON。**单独一个文件，和 `infer/node_json.cpp` 是同一个理由。**
//
// `doctor.cpp` 为了探大模型服务 include 了 httplib，而测试目标那一列上面写着
// 「一个网络库都不链」——只要这段留在那儿，`changji_tests` 就链不起来
//（实测：undefined reference 到 doctor::to_json）。这一段是纯的
//（Report → JSON，不碰网络），所以搬出来。
#include "doctor/doctor.hpp"

namespace changji::doctor {

/// **定义放在这一半。** doctor.cpp 链 httplib，刻意不进测试目标
/// （CMakeLists 里那段注释：「各自的逻辑部分都已经拆到不依赖网络库的文件
/// 里了」）。这条是纯判断，属于拆出来的那一半。
Level sd_level_without_local(bool frame_elsewhere, bool video_elsewhere) {
    return frame_elsewhere && video_elsewhere ? Level::WARN : Level::FAIL;
}

nlohmann::json to_json(const Report& r) {
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& c : r.checks) {
        checks.push_back({
            {"name", c.name},
            {"level", to_string(c.level)},
            {"detail", c.detail},
            {"fix", c.fix},
            // 要补的是哪一组模型。界面据此摆一颗直接跳过去的按钮，
            // 不用去正则匹配 `fix` 里的中文。见 doctor.hpp 上那段。
            {"group", c.group},
        });
    }
    return {{"can_run", r.can_run()}, {"checks", checks}};
}

}  // namespace changji::doctor
