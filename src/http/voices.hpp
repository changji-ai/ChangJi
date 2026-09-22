#pragma once
// GET /api/voices —— 服务端有哪些参考音色。
//
// **现在两条配音后端都没有服务端清单**，这个接口的全部工作就是把这件事
// 说清楚。以前走 ComfyUI 时它是个下拉框（装了哪些插件就有哪些选项），
// ComfyUI 拆掉之后剩下的两条是：
//
//   local —— 进程内跑。音色是用户自己给的一段参考音频，没有清单。
//   http  —— 外部服务。它有没有音色列表是那个服务自己的事，我们不代问。
//
// **接口保留而不是删掉**：设定页的「人物」那一格打开时会拉它（这一页
// 2026-09-11 之前叫「角色页」），删了前端要跟着改，
// 而它现在恰好承担了"告诉用户音色怎么配"这件事——那句说明比一个
// 空下拉框有用。
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "stages/tts_backends.hpp"

namespace changji::http {

/// 列音色。
///
/// **任何一步失败都回 200 加一个 error 字段**，不回错误码：这个接口是
/// 设定页的「人物」那一格打开时顺带拉的，回 500 的话整个页面会弹错误框，
/// 而用户可能根本没打算配音。
///
/// `backend` 是 `[tts].backend` 的值，用来决定那句说明怎么写——
/// 说错了比不说更糟：让用 local 的人去查一个他根本没在用的服务。
ApiResult get_voices(const std::string& path, const std::string& backend);


/// POST /api/voice/take —— 摇一段音色试听。
///
/// body: `{project, seed?, text?}`。不给 seed 就随机一个并在回包里带上
/// ——用户要是喜欢这一摇，得能把它存下来。
ApiResult post_voice_take(const nlohmann::json& body);

/// POST /api/voice/save —— 把某个种子摇出来的音色存成参考音频。
///
/// body: `{project, seed, name, char_id?}`。存进 `voices/<名字>.wav`，
/// 给了 char_id 就顺手挂到那个角色的 voice_id 上。
///
/// **存的就是刚才听的那一段**——直接把 `.take_<seed>.wav` 拷过去。
///
/// 原来是"按同一个种子重出一段更长的"（参考音频越长克隆越稳）。那是错的：
/// 音色是 (种子, 文本) 的函数，换文本就换人——实测 1789 号从 112 Hz 变成
/// 205 Hz、3313 号从 137 变成 224。拷贝还是**结构上**保证了"存的就是听的"，
/// 不靠"同种子同文本确定性"这个经验性质（换一版权重就未必还在）。
/// 手里没有那一段才退回重出（比如有人不经试听直接调接口）。
ApiResult post_voice_save(const nlohmann::json& body);

/// 摇音色的临时文件：`voices/` 里除了 `keep` 之外所有 `.take*`。
///
/// 摇是随手点的，一次点几十下，不清的话 voices/ 里会堆一地；
/// 所以每摇一次就把上一次那个删掉，任何时候至多留一个。
///
/// **前缀是 `.take` 而不是 `.take_`**：上一版落在固定的 `.take.wav` 上，
/// 只认带下划线的话那一个永远删不掉，在老项目里躺一辈子。
///
/// 导出来是因为**这是一段删文件的代码**，前缀写错就是删错东西。
/// 返回删掉了几个。
int sweep_old_takes(const std::filesystem::path& voices_dir,
                    const std::filesystem::path& keep);

}  // namespace changji::http
