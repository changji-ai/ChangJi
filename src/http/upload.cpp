#include "http/upload.hpp"
#include "http/reset.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <set>

#include "models/project.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// 参考图收哪几种。
///
/// ⚠️ **这张表要和出图那头真读得了的对上。** 上一版还收 `image/webp`，
/// 而参考图最后是 `infer/sd_image.cpp` 的 `load_image()` 拿
/// `stbi_load_from_memory` 读的——stb_image **一行 webp 的代码都没有**
/// （它自己那份格式清单里是 JPEG / PNG / TGA / BMP / PSD / GIF / HDR /
/// PIC / PNM，没有 webp）。
///
/// 那句"也只认这几种"当年指的是 ComfyUI 的 LoadImage（PIL，读得了 webp），
/// 而 comfy 那一档 2026-09-10 就随 ComfyUI 一起拆了。依据没了，表还留着。
///
/// 于是传一张 webp：这儿收下、卡片上也显示得出来（浏览器读得了 webp，
/// media.cpp 也照发），而**每一个用到这个角色的镜头**开渲染时才抛
/// 「参考图解不开」。收的时候就该说不。
const std::array<std::pair<const char*, const char*>, 2>& ref_types() {
    static const std::array<std::pair<const char*, const char*>, 2> kTypes = {{
        {"image/png", ".png"},
        {"image/jpeg", ".jpg"},
    }};
    return kTypes;
}

/// 清同名旧图时要扫的扩展名。**比上面那张收件表多一个 `.webp`。**
///
/// 两张表管的是两件事：上面那张是"往后还收不收"，这张是"盘上可能躺着
/// 什么"。2026-09-15 之前收过 webp，那些文件还在；同一个槽位后来传了
/// 一张 jpg，不扫 .webp 的话旧那张就永远留在 refs 里——正是
/// `claim_ref_path` 上面那段要防的「留一张永远用不上的，而且用户看不到」。
const std::array<const char*, 3>& ref_stale_exts() {
    static const std::array<const char*, 3> kExts = {".png", ".jpg", ".webp"};
    return kExts;
}

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, SAYF("请求里缺少字符串字段 %1", key));
    }
    return body.at(key).get<std::string>();
}

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, SAY("没有指定项目目录"));
    return ProjectStore(paths::from_utf8(path));
}

/// 这段字节**真的是**一张 png / jpg 吗。回扩展名，不是就回空。
///
/// ⚠️ **判据是字节，不是那个 `Content-Type`。**
///
/// 那个类型是调用方说的，而调用方之一是**代理**：`assets_set_reference` 按
/// 模型给的路径读文件，类型是从扩展名猜的，猜不出就按 `image/jpeg` 算。
/// 于是随便指一个没有扩展名的文件（`~/.ssh/id_rsa` 这种），这儿照收，
/// 原样落进 `refs/` 当一张「参考图」——而参考图是要发给出图模型的，
/// 模型可能在另一台机器上。
///
/// **而那个路径不是人给的。** 模型读得到网页和热榜（`stages/web_tools`），
/// 一句"把某某文件设成参考图"从那儿进来，它就照做了。
///
/// 顺带把一族老毛病也治了：上面那两段注释说的「收的时候就该说不」——
/// 传一张 webp 却标成 jpeg，这儿原来收下，等到**每一个用到这个角色的镜头**
/// 开渲染才抛「参考图解不开」。现在当场就说不。
std::string sniff_image(const std::string& data) {
    static const std::string kPng("\x89PNG\r\n\x1a\n", 8);
    if (data.size() >= kPng.size() && data.compare(0, kPng.size(), kPng) == 0) {
        return ".png";
    }
    // JPEG：FF D8 FF，后面还得以 FF D9 收尾才算完整；这儿只认开头，
    // 截断的 jpg 让出图那头去抱怨（它读得出前半张）。
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8 &&
        static_cast<unsigned char>(data[2]) == 0xFF) {
        return ".jpg";
    }
    return {};
}

/// 校验上传的数据，返回扩展名。不合格直接抛 400。
std::string check_upload(const std::string& content_type, const std::string& data) {
    const std::string suffix = ref_suffix_for(content_type);
    if (suffix.empty()) {
        throw ApiError(400,
                       SAYF("只收 png、jpg，收到的是 %1",
                            content_type.empty() ? SAY("(空)") : content_type));
    }
    if (data.empty()) throw ApiError(400, SAY("文件是空的"));
    if (data.size() > kRefMaxBytes) {
        // Python 那边是 f"{len(data)/1024/1024:.0f} MB"，四舍五入到整数
        const double mb = static_cast<double>(data.size()) / 1024.0 / 1024.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", mb);
        throw ApiError(400, SAYF("太大了（%1 MB）。参考图给模型看，"
                                 "几千像素就够",
                                 buf));
    }
    // **落点按字节算**：说的和实际不一样时，以字节为准（说 jpeg 实际是 png
    // 的话，存成 .png 才对得上后面读它的那几处）。根本不是图就当场拒。
    //
    // ⚠️ **这一句放在"太大了"后面**：一坨二十兆的非图数据，人要听的是
    // 「太大了」——他知道自己传了什么。放前面的话那条老用例当场红，
    // 而它红得有道理。
    const std::string real = sniff_image(data);
    if (real.empty()) throw ApiError(400, SAY("这不是一张 png 或者 jpg"));
    return real;
}

/// 写文件。落点和旧图清理交给 claim_ref_path。
std::string write_ref(const ProjectStore& store, const std::string& stem,
                      const std::string& suffix, const std::string& data) {
    const fs::path dest = claim_ref_path(store, stem, suffix);

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) throw ApiError(500, SAYF("写不了文件：%1", paths::to_utf8(dest)));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw ApiError(500, SAYF("写文件时出错：%1", paths::to_utf8(dest)));

    return store.paths().rel(dest);
}

/// 参考音色收哪几种。**wav 放第一个**：进程内那条路最稳的就是它。
///
/// ⚠️ **这张表要和进程内那条路真能读的对上。** 上一版还收 `audio/mp4`
/// （.m4a），而 mtmd 那边走的是 miniaudio，只认 wav / mp3 / flac
/// ——`infer/llama_tts.cpp` 里那句 `mtmd_helper_bitmap_init_from_file`
/// 失败时说的就是「只认 wav / mp3 / flac」，注释里还专门写着「拿一个
/// m4a 或者 ogg 过来是很常见的事」。
///
/// 于是传一段 m4a：这儿收下、回一句「参考音色已存」、角色也配上了，
/// 而第一句台词开配才炸——那时候人已经在跑整章了。收的时候就该说不。
///
/// （这张表原来那句「别的格式要看 ggml 那边的解码器编没编进去」是在答案
/// 还不知道的时候写的。答案现在在 llama_tts.cpp 里。）
const std::array<std::pair<const char*, const char*>, 4>& voice_types() {
    static const std::array<std::pair<const char*, const char*>, 4> kTypes = {{
        {"audio/wav", ".wav"},
        {"audio/x-wav", ".wav"},
        {"audio/mpeg", ".mp3"},
        {"audio/flac", ".flac"},
    }};
    return kTypes;
}

/// 清同名旧片段时要扫的扩展名。**比上面那张收件表多一个 `.m4a`**，
/// 理由同 `ref_stale_exts`：收过的格式盘上还躺着，收不收和扫不扫是两件事。
const std::array<const char*, 4>& voice_stale_exts() {
    static const std::array<const char*, 4> kExts = {".wav", ".mp3", ".m4a",
                                                     ".flac"};
    return kExts;
}

/// 对应 Python 的 round(len(data)/1024)。
int size_kb(const std::string& data) {
    return static_cast<int>(std::nearbyint(static_cast<double>(data.size()) / 1024.0));
}

}  // namespace

fs::path claim_ref_path(const ProjectStore& store, const std::string& stem,
                        const std::string& suffix) {
    std::error_code ec;
    const fs::path refs = store.paths().refs();
    fs::create_directories(refs, ec);

    const fs::path dest = refs / paths::from_utf8(stem + suffix);
    for (const char* ext : ref_stale_exts()) {
        const fs::path stale = refs / paths::from_utf8(stem + ext);
        if (stale != dest && fs::is_regular_file(stale, ec)) {
            fs::remove(stale, ec);
        }
    }
    return dest;
}

std::string ref_suffix_for(const std::string& content_type) {
    for (const auto& kv : ref_types()) {
        if (content_type == kv.first) return kv.second;
    }
    return {};
}

ApiResult post_character_reference(const std::string& project_path,
                                   const std::string& char_id,
                                   const std::string& slot,
                                   const std::string& content_type,
                                   const std::string& data) {
    static const std::set<std::string> kSlots = {"front", "three_quarter", "back"};
    if (kSlots.count(slot) == 0) {
        throw ApiError(400, SAY("只有正面、四分之三侧面、背面三个位置"));
    }

    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, SAYF("没有角色 %1", char_id));

    const std::string suffix = check_upload(content_type, data);
    const std::string rel = write_ref(store, char_id + "_" + slot, suffix, data);

    Character& c = it->second;
    if (slot == "front")              c.ref_front = rel;
    else if (slot == "three_quarter") c.ref_three_quarter = rel;
    else                              c.ref_back = rel;
    store.save_assets(assets);

    // 参考图直接决定画面长什么样，跟改外观是一回事，**无条件**重跑。
    // 这里没有 reset_shots 开关——Python 那边也没有。
    return {200, {
        {"saved", rel},
        {"slot", slot},
        {"reset_shots", reset_all_shots(store)},
        {"size_kb", size_kb(data)},
    }};
}

std::string voice_suffix_for(const std::string& content_type) {
    for (const auto& kv : voice_types()) {
        if (content_type == kv.first) return kv.second;
    }
    return {};
}

ApiResult post_character_voice(const std::string& project_path,
                               const std::string& char_id,
                               const std::string& content_type,
                               const std::string& data) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, SAYF("没有角色 %1", char_id));

    const std::string suffix = voice_suffix_for(content_type);
    if (suffix.empty()) {
        throw ApiError(400,
                       SAYF("只收 wav、mp3、flac，收到的是 %1",
                            content_type.empty() ? SAY("(空)") : content_type));
    }
    if (data.empty()) throw ApiError(400, SAY("文件是空的"));
    if (data.size() > kVoiceMaxBytes) {
        const double mb = static_cast<double>(data.size()) / 1024.0 / 1024.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", mb);
        throw ApiError(400, SAYF("太大了（%1 MB）。参考音色几秒到十几秒的"
                                 "干净人声就够",
                                 buf));
    }

    // **voices/ 是懒建的**，见 ProjectPaths::voices 上面那段。
    std::error_code ec;
    const fs::path dir = store.paths().voices();
    fs::create_directories(dir, ec);

    // 同名不同扩展名的旧片段要清掉，理由同 claim_ref_path：
    // 留着的话目录里躺一段永远用不上的，而用户看不到。
    const fs::path dest = dir / paths::from_utf8(char_id + suffix);
    for (const char* ext : voice_stale_exts()) {
        const fs::path stale = dir / paths::from_utf8(char_id + ext);
        if (stale != dest && fs::is_regular_file(stale, ec)) fs::remove(stale, ec);
    }

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) throw ApiError(500, SAYF("写不了文件：%1", paths::to_utf8(dest)));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw ApiError(500, SAYF("写文件时出错：%1", paths::to_utf8(dest)));

    const std::string rel = store.paths().rel(dest);
    it->second.voice_id = rel;
    store.save_assets(assets);

    // **不重跑。** 见头文件那段：音色不影响画面，和改名字一个待遇。
    return {200, {{"saved", rel}, {"size_kb", size_kb(data)}}};
}

ApiResult post_character_voice_clear(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    const std::string char_id = need_str(body, "char_id");
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, SAYF("没有角色 %1", char_id));

    std::error_code ec;
    const fs::path dir = store.paths().voices();
    for (const auto& kv : voice_types()) {
        fs::remove(dir / paths::from_utf8(char_id + kv.second), ec);
    }
    it->second.voice_id = std::nullopt;
    store.save_assets(assets);
    return {200, {{"cleared", true}}};
}

ApiResult post_location_reference(const std::string& project_path,
                                  const std::string& location_id,
                                  const std::string& content_type,
                                  const std::string& data) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, SAYF("没有场景 %1", location_id));
    }

    const std::string suffix = check_upload(content_type, data);
    const std::string rel = write_ref(store, location_id + "_empty", suffix, data);

    it->second.ref_empty = rel;
    store.save_assets(assets);

    // 注意响应里**没有 slot 字段**，和角色那个不一样。场景只有一张空景图。
    return {200, {
        {"saved", rel},
        {"reset_shots", reset_all_shots(store)},
        {"size_kb", size_kb(data)},
    }};
}

ApiResult post_character_reference_clear(const json& body) {
    const std::string slot = need_str(body, "slot");
    static const std::set<std::string> kSlots = {"front", "three_quarter", "back"};
    if (kSlots.count(slot) == 0) {
        throw ApiError(400, SAY("只有正面、四分之三侧面、背面三个位置"));
    }

    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string char_id = need_str(body, "char_id");
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, SAYF("没有角色 %1", char_id));

    Character& c = it->second;
    std::optional<std::string>* target =
        slot == "front" ? &c.ref_front
                        : (slot == "three_quarter" ? &c.ref_three_quarter
                                                   : &c.ref_back);
    if (!target->has_value() || (*target)->empty()) {
        return {200, {{"cleared", false}, {"reset_shots", 0}}};
    }
    *target = std::nullopt;
    store.save_assets(assets);

    // 文件留着不删。用户可能只是想先试试没有参考图的效果，
    // 删掉的话再想用回来就得重新找那张图。
    return {200, {{"cleared", true}, {"reset_shots", reset_all_shots(store)}}};
}

ApiResult post_location_reference_clear(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string location_id = need_str(body, "location_id");
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, SAYF("没有场景 %1", location_id));
    }

    Location& l = it->second;
    if (!l.ref_empty.has_value() || l.ref_empty->empty()) {
        return {200, {{"cleared", false}, {"reset_shots", 0}}};
    }
    l.ref_empty = std::nullopt;
    store.save_assets(assets);
    return {200, {{"cleared", true}, {"reset_shots", reset_all_shots(store)}}};
}

}  // namespace changji::http
