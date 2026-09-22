#pragma once

// 配乐：一章一条器乐，由外部命令生成，装配时压在台词底下。
//
// 为什么是「一条命令」而不是进程内跑：能本地出像样配乐的模型
// （ACE-Step 1.5，Apache 2.0，<4 GB 显存，几秒一条）在 Python 里，
// 而且换起来勤——今天是它，明年不一定。我们只约定占位符
// （[sound].music_command，见 config::SoundConfig），包装脚本在
// tools/music_ace_step.py。
//
// 这一层要能在没有那条命令的机器上测死：描述怎么拼、文件放哪、
// 命令怎么展开，都是纯函数。

#include <filesystem>
#include <string>

#include "config/settings.hpp"
#include "models/project.hpp"

namespace changji::stages {

/// 这一章的配乐放哪：output/<episode_id><suffix>_music.wav。
///
/// `suffix` 是给**同一章的另一条片子**用的（预告：`_preview`）。不给它的
/// 话两条片子共用一个配乐文件，而这一层是"文件在就沿用"、ffmpeg 那头短了
/// 又不循环：预告先出一条两分钟的，整章再跑就沿用它，**后面几分钟静悄悄
/// 没有配乐**。
std::filesystem::path music_path_for(const models::ProjectPaths& paths,
                                     const std::string& episode_id,
                                     const std::string& suffix = "");

/// 给配乐模型的描述。器乐、无人声、按整部电影的调子和这一章的梗概。
///
/// 英文为主：配乐模型的训练语料是英文标签；梗概那一段照原文带上，
/// 它只是给情绪定调，不要求逐字理解。
std::string music_prompt(const models::Episode& ep,
                         const config::SoundConfig& sound, double seconds);

struct MusicOutcome {
    bool ok = false;
    /// 已有文件，没重新生成。
    bool reused = false;
    std::filesystem::path path;
    std::string error;
};

/// 保证这一章的配乐在：文件在就沿用（想重出就删掉它），不在就跑命令。
/// 命令没配返回 ok = false 并说清楚，不抛。
MusicOutcome ensure_music(const config::Settings& settings,
                          const models::ProjectPaths& paths,
                          const models::Episode& ep, double seconds,
                          const std::string& suffix = "");

}  // namespace changji::stages
