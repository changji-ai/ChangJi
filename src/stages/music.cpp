#include "stages/music.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "util/cmdline.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::stages {

fs::path music_path_for(const models::ProjectPaths& paths,
                        const std::string& episode_id,
                        const std::string& suffix) {
    return paths.output() /
           paths::from_utf8(episode_id + suffix + "_music.wav");
}

std::string music_prompt(const models::Episode& ep,
                         const config::SoundConfig& sound, double seconds) {
    std::string out = "cinematic instrumental film score, no vocals";
    const std::string style = text::strip_ws(sound.music_style);
    if (!style.empty()) {
        out += ", " + style;
    } else {
        out += ", tense and emotional, minimal strings and piano, slow build";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(seconds)));
    out += ", about " + std::string(buf) + " seconds";
    // 梗概只取一段：它是定调用的，不是歌词。
    std::string synopsis = text::collapse_ws(ep.synopsis);
    if (synopsis.empty()) synopsis = text::collapse_ws(ep.title);
    if (!synopsis.empty()) {
        // 按字符截，别把一个汉字切成半个。
        std::string cut;
        std::size_t chars = 0;
        for (std::size_t i = 0; i < synopsis.size() && chars < 120;) {
            const unsigned char c = static_cast<unsigned char>(synopsis[i]);
            std::size_t len = 1;
            if (c >= 0xF0) len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;
            cut += synopsis.substr(i, len);
            i += len;
            ++chars;
        }
        out += ". story: " + cut;
    }
    return out;
}

MusicOutcome ensure_music(const config::Settings& settings,
                          const models::ProjectPaths& paths,
                          const models::Episode& ep, double seconds,
                          const std::string& suffix) {
    MusicOutcome out;
    out.path = music_path_for(paths, ep.episode_id, suffix);
    std::error_code ec;
    if (fs::is_regular_file(out.path, ec) && fs::file_size(out.path, ec) > 0) {
        out.ok = true;
        out.reused = true;
        return out;
    }
    if (text::strip_ws(settings.sound.music_command).empty()) {
        out.error = "全局配置里没有 [sound].music_command，不知道拿什么生成配乐";
        return out;
    }
    fs::create_directories(out.path.parent_path(), ec);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d",
                  std::max(5, static_cast<int>(std::lround(seconds))));
    const std::map<std::string, std::string> vars = {
        {"prompt", music_prompt(ep, settings.sound, seconds)},
        {"seconds", buf},
        {"out", paths::to_utf8(out.path)},
    };
    const auto argv = util::expand_command(settings.sound.music_command, vars);
    const auto r = util::run_command(argv, settings.sound.music_timeout_s);
    if (!r.ok) {
        out.error = "配乐命令失败：" + r.error;
        return out;
    }
    if (!fs::is_regular_file(out.path, ec) || fs::file_size(out.path, ec) == 0) {
        out.error = "配乐命令跑完了，但没有写出 " + paths::to_utf8(out.path);
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace changji::stages
