// 跨平台的标准目录。
//
// 这是 Python 侧 platformdirs 的等价物，必须逐字节对齐它的结果——
// C++ 后端要能读到 Python 后端写的同一个 config.toml，否则迁移期间
// 用户会看到两套后端各有一份配置，改了这边那边不认。
//
// 对齐目标是 platformdirs 的 user_config_dir(appname, appauthor=False)
// 和 user_data_dir(appname, appauthor=False)。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace changji::paths {

/// 用户全局配置目录。
/// Windows: %LOCALAPPDATA%\changji
/// macOS:   ~/Library/Application Support/changji
/// Linux:   $XDG_CONFIG_HOME/changji 或 ~/.config/changji
/// 家目录。Windows 读 USERPROFILE（退回 HOMEDRIVE+HOMEPATH），
/// 别的系统读 HOME。
///
/// **导出它是给测试用的**：语料里存的是相对家目录的路径（存绝对路径的话
/// 里面带着用户名，换台机器就对不上），而"家目录是哪个"必须和代码用的
/// 是同一个——测试自己再算一遍就可能算出别的来。
std::filesystem::path home_dir();

/// 这台机器上的几块盘：Windows 是每个盘符的根（`C:\`、`D:\`，连映射的网络盘），
/// macOS 是 `/` 加 `/Volumes` 底下挂着的，Linux 是 `/` 加 `/mnt`、`/media`、
/// `/run/media` 底下挂着的。**只回此刻进得去的**：光驱没放盘、网络盘断了的不回。
///
/// 给「挑一个目录」那条接口当起点：模型动辄几十 GB，人要挑的正是"哪块盘装得下"，
/// 而起点只有家目录的话，别的盘得一级一级往上翻、翻到根也翻不过去。
std::vector<std::filesystem::path> volume_roots();

std::filesystem::path user_config_dir(const std::string& app_name);

/// 用户数据目录。
/// Windows: %LOCALAPPDATA%\changji
/// macOS:   ~/Library/Application Support/changji
/// Linux:   $XDG_DATA_HOME/changji 或 ~/.local/share/changji
std::filesystem::path user_data_dir(const std::string& app_name);

/// 展开开头的 ~。配置里的路径允许用户写 ~/电影项目 这种形式。
std::filesystem::path expand_user(const std::string& raw);

/// 读环境变量。没有或为空返回空字符串。
/// Windows 上走宽字符再转回 UTF-8，否则中文路径会变成问号。
std::string env(const char* name);

/// 路径转 UTF-8 字符串。**任何时候都不要用 path.string() 代替它。**
///
/// 这是实测踩出来的：MSVC 上 fs::path 内部存宽字符，.string() 会用当前
/// ANSI 代码页做转换，遇到该代码页表示不了的字符就抛 std::system_error
/// （"No mapping for the Unicode character exists in the target multi-byte
/// code page"）。这台机器的代码页是 936（GBK），--doctor 就这样静默死掉——
/// 异常一路穿到 std::terminate，进程以 0xC0000409 消失，而那个错误码字面
/// 意思是"栈缓冲区溢出"，会把人往完全错误的方向带。
///
/// 项目目录本来就允许是 E:\AI电影\ 这种路径，用户名也可能是中文，
/// 所以这不是边缘情况。这个函数走 wstring → UTF-8，不经过 ANSI 代码页，
/// 永远不会失败。
/// 设一个环境变量（当前进程内）。
///
/// 工作进程用它设 `CUDA_VISIBLE_DEVICES`，**必须在建任何 ggml 上下文之前**
/// ——后端初始化时就把设备列表读走了，之后再设没用。
void set_env(const std::string& name, const std::string& value);

std::string to_utf8(const std::filesystem::path& p);

/// UTF-8 字符串转路径。**任何时候都不要用 fs::path(str) 代替它。**
///
/// 是 to_utf8 的反方向，坑也是对称的：MSVC 上 fs::path(std::string) 把窄字符串
/// 按当前 ANSI 代码页解释，而本项目里的 std::string 一律是 UTF-8。
/// UTF-8 的中文字节序列在 GBK 里往往是非法的，转换会抛 std::system_error。
///
/// 实测踩到的位置：proc::which() 里 `fs::path(dir)`，dir 来自 PATH 环境变量
/// （已经是 UTF-8）。只要 PATH 里有一个目录名带非 ASCII 字符，--doctor 就整个崩掉。
std::filesystem::path from_utf8(const std::string& s);

/// 这个进程自己的可执行文件路径。
///
/// 多卡时单进程要拉起每张卡一个工作进程，而**拉起的必须是自己这一份**——
/// 靠 PATH 找 "changji" 会找到别的构建（机器上常常有好几个 build- 目录），
/// 那时候症状是"工作进程行为和主进程对不上"，极难联想到是版本不同。
///
/// 取不到时回空（那时候多卡自动拉起会退回单进程，并说清原因）。
/// 两串路径指的是不是同一个地方。
///
/// **不能直接比字符串**（CLAUDE.md 第十一条）：macOS 上 `/var/folders/…`
/// 是 `/private/var/folders/…` 的软链，**两串字不一样而指的是同一个地方**。
/// 直接比的后果是"问了等于没问"——这一批明明在跑，快照回「没在跑」。
///
/// 存在不存在都答得出（走 `weakly_canonical`）：队列里那个项目可能刚被删掉，
/// 而"它是不是你问的那个"仍然要有答案。
///
/// 三处用它：参考图队列（`ref_gen.cpp`）、出片队列和「正在跑的是不是这部
/// 电影」（`run.cpp`）。**收成一处**——各写一遍的话，哪天改成认大小写、
/// 认尾斜杠，漏掉的那一处会静悄悄地答错。
bool same_dir(const std::string& a, const std::string& b);

/// 一个目录的**键**：`same_dir(a, b)` 为真的两串，键一样。
///
/// 要拿路径当 map 的键时用它（任务表按片子分槽、对话按片子分会话）——
/// 拿原串当键就是第十一条那个坑：同一部片子两种写法，两个槽、两个会话，
/// 该挡的没挡住。空串的键是空串；规范化不出来（盘符不在之类）就用原串。
std::string dir_key(const std::string& dir);

std::filesystem::path self_exe();

/// 命令行参数，**UTF-8 的**。
///
/// Windows 上 `char** argv` 是按当前 ANSI 代码页编的。一个中文路径
/// 从命令行传进来，直接当 UTF-8 用会得到一串乱码字节——
/// 拼进 URL 是 400，交给 fs::path 是异常，写进日志是问号。
/// 而这一切都不报错，只表现为"这个项目打不开"。
///
/// 这里从 GetCommandLineW 重新取一份宽字符的再转 UTF-8。
/// 其它平台 argv 本来就是 UTF-8，原样拷贝。
///
/// **每个 main() 的第一件事都该是调它。**
std::vector<std::string> utf8_args(int argc, char** argv);

}  // namespace changji::paths
