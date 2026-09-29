#pragma once

#include <filesystem>
#include <string>

namespace changji::util {

/// 整份换一个文件：写到旁边一个独一份的临时文件、写全了（fsync）再换名。
/// 别人任何时候看到的要么是旧的一整份，要么是新的一整份。
///
/// - `private_only`：临时文件**一生下来就是 0600**（O_EXCL），不是先按默认
///   权限建、写完再 chmod——中间那一下同机别的账号读得到。放口令、票、密钥的
///   文件（config.toml 的机器表口令、lan.json 里发出去的票）用它。
/// - 目标是个链接（dotfiles 仓库那种）就写到**它指着的那个文件**上：直接换名
///   会把链接本身换成一个普通文件，链过去的那份从此不再更新，一声不响。
///
/// 写不全、换不了名就抛 `std::runtime_error`，临时文件收掉。
/// Windows 上没有对应的简单权限做法：文件落在用户自己的目录里，照旧。
void write_file_atomic(const std::filesystem::path& target,
                       const std::string& data, bool private_only);

}  // namespace changji::util
