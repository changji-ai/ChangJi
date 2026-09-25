#pragma once

// 参考图上传：/api/character/reference /api/location/reference
// 以及对应的两个 /clear。
//
// 参考图是一致性最硬的手段：文字描述再细，模型每次也会重新想象一遍
// 那张脸；给一张图，它就照着画。所以传了图必须无条件重跑全片——
// 跟改外观是一回事。
//
// multipart 的解析留在路由层（那是 crow 的事），这里只收已经拆好的
// 字段和二进制数据，这样能不起服务就把校验和落盘逻辑测一遍。

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"  // ApiResult / ApiError / guard
#include "models/project.hpp"

namespace changji::http {

/// 参考图能传哪些格式。
///
/// 不是随便什么文件都往项目目录里塞，而且**出图那头只读得了这几种**
/// （`sd_image.cpp` 的 `load_image()` 走 stb_image）。返回扩展名，
/// 认不出来返回空串。
///
/// 原来这句写的是「ComfyUI 那边的 LoadImage 也只认这几种」——而 comfy
/// 那一档 2026-09-10 就随 ComfyUI 一起拆了，剩下的只有进程内这一条。
/// 两者能读的还不一样：PIL 读得了 webp，stb_image 读不了。
std::string ref_suffix_for(const std::string& content_type);

/// 单张上限 20MB。
///
/// 参考图是给模型看的，几千像素足够；传一张两百兆的原片进来
/// 只会把项目目录撑爆。
constexpr std::size_t kRefMaxBytes = 20u * 1024u * 1024u;

/// 在 refs/ 里给这个名字定一个落点，**先把这一格现在的图留底**
/// （`versions/refs/<名字>/`，见 stash_ref）。
///
/// 上传和生成都从这里过。新图写好之后调 `settle_ref_path` 清掉**同名不同
/// 扩展名**的旧图：同一个槽位先传过一张 jpg、再生成一张 png 的话，不清的话
/// refs 里会留一张永远用不上的——而且用户看不到，只有翻目录才发现。
///
/// ⚠️ **清旧图要在新图落地之后。** 2026-09-25 之前是在这儿（出图之前）就删：
/// 那一张画失败或者被停，角色那一栏还指着 jpg，而 jpg 已经没了。
std::filesystem::path claim_ref_path(const models::ProjectStore& store,
                                     const std::string& stem,
                                     const std::string& suffix);
/// 新图已经在 `dest` 了：清掉同名不同扩展名的旧图。
void settle_ref_path(const models::ProjectStore& store, const std::string& stem,
                     const std::filesystem::path& dest);

/// 每一格留几份旧图。
constexpr std::size_t kRefHistoryKeep = 8;
/// 这一格的旧图放在哪（`<项目>/versions/refs/<名字>/`）。
std::filesystem::path ref_history_dir(const models::ProjectStore& store,
                                      const std::string& stem);
/// 这一格留下来的旧图，新的在前。
std::vector<std::filesystem::path> ref_history(const models::ProjectStore& store,
                                               const std::string& stem);
/// 把这一格现在的图复制一份留底。和最近一份一模一样就不存；只留最近
/// `kRefHistoryKeep` 份。
void stash_ref(const models::ProjectStore& store, const std::string& stem);

/// POST /api/character/reference
///
/// slot 只能是 front / three_quarter / back。
ApiResult post_character_reference(const std::string& project_path,
                                   const std::string& char_id,
                                   const std::string& slot,
                                   const std::string& content_type,
                                   const std::string& data);

/// POST /api/location/reference —— 空景图，场景只有一张，没有 slot。
ApiResult post_location_reference(const std::string& project_path,
                                  const std::string& location_id,
                                  const std::string& content_type,
                                  const std::string& data);

/// 参考音色能传哪些格式。返回扩展名，认不出来返回空串。
///
/// 进程内配音把 `voice_id` 当**一段人声片段的路径**用（tts_backends.cpp
/// 里 `req.speaker_ref`），模型照着它的音色念。所以"选音色"这件事在这一版
/// 等于"给一段参考音频"——界面上那个空的文本框以前要求用户手打路径，
/// 而路径打错的表现只是配音失败。
std::string voice_suffix_for(const std::string& content_type);

/// 单段上限 8MB。参考音色几秒到十几秒就够，给一首歌进来没有意义。
constexpr std::size_t kVoiceMaxBytes = 8u * 1024u * 1024u;

/// POST /api/character/voice —— 传一段参考音色，存进 voices/。
///
/// **不触发重跑。** 和改音色那一栏一个待遇（editing_assets.cpp 里
/// kAppearance 不含 voice_id）：它不影响画面。要让新音色生效，在镜头墙上
/// 对那几镜点「配音」——2026-09-13 补了这个按钮。
ApiResult post_character_voice(const std::string& project_path,
                               const std::string& char_id,
                               const std::string& content_type,
                               const std::string& data);

/// POST /api/character/voice/clear —— 清掉这个角色的参考音色。
ApiResult post_character_voice_clear(const nlohmann::json& body);

/// POST /api/character/reference/clear
ApiResult post_character_reference_clear(const nlohmann::json& body);

/// POST /api/location/reference/clear
ApiResult post_location_reference_clear(const nlohmann::json& body);

}  // namespace changji::http
