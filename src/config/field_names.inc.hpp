// 字段的中文标签，当年从 web/server.py 的 _FIELD_NAMES 原样导出。那套 Python
// 早删了，**现在直接改这里**；今天的 tools/gen_prompts.py 管的是 prompts.toml
// （提示词），不生成这个文件。
// 报「已应用 min_frame_similarity」不如报「与首帧相似度下限」。
// 另外是配置段映射：哪个字段该写进配置文件的哪一节。

#pragma once

#include "util/say.hpp"

namespace changji::stages::prompt {

// 字段名 -> 标签。
//
// **表里存中文原文，翻译推迟到查表的那一行**（`config_api.cpp` 的
// `readable()` / `labels_for()` 里 `SAY()` 一下）。在这儿就地 `SAY()` 是
// 不行的：这是张 `constexpr` 表，静态初始化跑在 `speak()` 之前，语言会被
// 冻在建表那一刻。`SAY_NOOP()` 只是给抽取器看的记号，展开之后一个字符
// 都没变，见 `util/say.hpp`。
inline constexpr const char* kFieldNames[][2] = {
    {"base_url", SAY_NOOP("地址")},
    {"model", SAY_NOOP("模型名")},
    {"api_key", "api key"},
    {"temperature", SAY_NOOP("温度")},
    // 这一条是给 validate() 那句话用的：错误消息形如
    // "llm.call_log_max_mb 必须大于 0"，config_api 的 readable() 按**最后
    // 一段**（去掉 llm. 前缀）来查，所以这儿要的是不带前缀的名字。
    // 下面 llm_call_log_max_mb 那条是另一条路（改完的回执 labels_for，
    // 查的是整个键名），两条都要，和 temperature 一模一样。
    {"call_log_max_mb", SAY_NOOP("提示词日志上限 MB")},
    {"job_timeout_s", SAY_NOOP("单镜超时")},
    {"max_retries", SAY_NOOP("重试次数")},
    {"backend", SAY_NOOP("后端")},
    {"fps", SAY_NOOP("帧率")},
    {"crf", SAY_NOOP("画质 crf")},
    {"subtitle_font", SAY_NOOP("字幕字体")},
    {"subtitle_max_chars_per_line", SAY_NOOP("字幕单行字数")},
    {"subtitle_max_lines", SAY_NOOP("字幕行数")},
    {"scene_transition_s", SAY_NOOP("场景转场")},
    {"max_attempts_per_shot", SAY_NOOP("单镜最多重试")},
    {"min_pixel_std", SAY_NOOP("画面展布下限")},
    {"min_frame_similarity", SAY_NOOP("与首帧相似度下限")},
    {"max_audio_drift_s", SAY_NOOP("台词落点最大偏差")},
    {"target_lufs", SAY_NOOP("响度目标")},
    {"gates_enabled", SAY_NOOP("质量闸门")},
    {"fallback_on_exhausted", SAY_NOOP("重试超限降级")},
    {"draft_width", SAY_NOOP("草稿档宽")},
    {"draft_height", SAY_NOOP("草稿档高")},
    {"draft_steps", SAY_NOOP("草稿档步数")},
    {"final_width", SAY_NOOP("成片档宽")},
    {"final_height", SAY_NOOP("成片档高")},
    {"final_steps", SAY_NOOP("成片档步数")},
    {"llm_base_url", SAY_NOOP("大模型地址")},
    {"llm_model", SAY_NOOP("模型名")},
    {"llm_api_key", "api key"},
    {"llm_temperature", SAY_NOOP("温度")},
    {"llm_call_log", SAY_NOOP("提示词日志")},
    {"llm_call_log_max_mb", SAY_NOOP("提示词日志上限 MB")},
    {"tts_backend", SAY_NOOP("配音后端")},
    {"tts_base_url", SAY_NOOP("配音服务地址")},
    {"vram_gb_override", SAY_NOOP("显存覆盖")},
};

// 字段名 -> {配置节, 节内的键名}。
// 画质档位不在这里：它是按显存推出来的，写死在配置里等于
// 把这台机器的显存刻进项目，换台机器就不对了。
inline constexpr const char* kSettingSections[][3] = {
    {"fps", "assembly", "fps"},
    {"crf", "assembly", "crf"},
    {"subtitle_font", "assembly", "subtitle_font"},
    {"subtitle_max_chars_per_line", "assembly", "subtitle_max_chars_per_line"},
    {"subtitle_max_lines", "assembly", "subtitle_max_lines"},
    {"scene_transition_s", "assembly", "scene_transition_s"},
    {"max_attempts_per_shot", "gates", "max_attempts_per_shot"},
    {"min_pixel_std", "gates", "min_pixel_std"},
    {"min_frame_similarity", "gates", "min_frame_similarity"},
    {"max_audio_drift_s", "gates", "max_audio_drift_s"},
    {"target_lufs", "gates", "target_lufs"},
    {"fallback_on_exhausted", "gates", "fallback_on_exhausted"},
    {"gates_enabled", "gates", "enabled"},
};

}  // namespace changji::stages::prompt
