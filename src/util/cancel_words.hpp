#pragma once

// 「人按的停」留下的那句话。
//
// **界面靠这两个词认它**：webapp 的 composables/stopped-by-hand.js 里是
// `/已停下|已取消/`，认出来才不把它弹成红色报错——「人按的停不是失败」。
// 那边没有别的判据可用：停是从顶栏那块徽标按的，发请求的是另一个页面里的
// 另一个函数，两边碰不着面；取消也可能来自另一个标签页。
//
// 这条耦合治起来不便宜（HTTP 错误体和作业错误消息是两条通道，要各加一个
// 机器可读的标记）。**但它最坏的性质是会静默失效**——引擎这边换个说法，
// 界面那边一声不响地开始把取消显示成报错。收成常量，再用
// tests/unit/test_ref_gen.cpp 那条钉住"必须还含着那两个词"，改文案的人
// 当场就会看到红。
//
// 真要换词：连 stopped-by-hand.js 那个正则一起换，那边的用例也跟着。
//
// ⚠️ **「已手动停止。……」那一族不进这儿。** 那是作业级的停（整趟活儿停下
// 之后 error 里留的话），界面靠自己按停时立的那面旗子分辨，不走这个正则；
// 收在各自那一层上（`pipeline/jobs.hpp` 的 kRunStoppedMessage、
// `pipeline/film_join.hpp` 的 kFilmJoinStoppedMessage）。塞进这儿的话，
// stopped-by-hand.test.js 会把本文件里每一条 `const char*` 都拿去过一遍
// 那个正则——而「已手动停止」里没有那两个词，当场红。

#include <string>

namespace changji::util {

/// 出参考图那条：`ApiError(400, …)`。
inline constexpr const char* kStoppedOne = "已停下这一张";

/// 大模型那条：`LlmError(…)`。
inline constexpr const char* kCancelled = "已取消";

/// 界面那个正则认的两个词。用例拿它比对上面两句。
inline constexpr const char* kStopToken1 = "已停下";
inline constexpr const char* kStopToken2 = "已取消";

/// 这句话是不是「人按的停」。
///
/// **`stopped-by-hand.js` 那个正则的 C++ 这一份。** 引擎这头也要分这两种
/// ——整批出图停下时，报「出图失败」和报「停下了，还差 N 张」是两回事。
inline bool is_cancel_word(const std::string& message) {
    return message.find(kStopToken1) != std::string::npos ||
           message.find(kStopToken2) != std::string::npos;
}

}  // namespace changji::util
