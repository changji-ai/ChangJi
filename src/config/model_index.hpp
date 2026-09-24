#pragma once

// 模型目录里**实际有**哪些模型文件。
//
// **为什么要有这一层。** 配置里写的是一串相对模型目录的名字
// （`[models].image = "Qwen_Image-Q8_0.gguf"`），而文件不一定就在那串
// 拼出来的位置上：
//
//   · 2026-09-24 起新下的按类型放进子目录（`llm/` `video/` `image/` `tts/`，
//     见 `setup::type_dir`），老版本下的平铺在目录根上；
//   · 人自己拷进来的，爱放哪放哪（`D:\models\wan\…`、`unet/…`）。
//
// 只认"那串拼上去"的话，这两种都是**缺模型**——界面上那一组亮红，人再点一次
// 下载，几十 GB 重下一遍，而那份文件明明就在同一个目录里。
//
// 所以：拼出来的那个位置上没有，就**按文件名**在整个模型目录里找
// （`ModelsConfig::resolve` 走这儿）；设置页「索引」那一下列的也是这一份。
//
// ⚠️ **只按文件名认，不按内容认。** 同名的两份（一份下全了、一份截断的）
// 按名字分不出来——要大小的地方（下载器判"已经有了"、设置页判"齐了"）
// 传 `bytes`，只认大小恰好对上的那份。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace changji::config {

/// 目录里的一个模型文件。
struct ModelFile {
    /// 相对模型目录，`/` 分隔，UTF-8。
    std::string rel;
    std::uint64_t bytes = 0;
};

struct ModelIndex {
    std::vector<ModelFile> files;
    /// 没扫完（目录太深、东西太多）。**要说出来**：没扫完的一份被当成全部的话，
    /// 界面上会说"这个目录里没有模型"，而人把整个盘指成了模型目录。
    bool truncated = false;
};

/// 按扩展名看算不算模型文件（`.gguf` `.safetensors` …，大小写不论）。
/// 下到一半的 `.part`、aria2 的控制文件、日志都不算。
bool is_model_file(const std::filesystem::path& p);

/// 扫一遍这个目录。
///
/// **有缓存**：同一个目录几秒内只扫一次——`resolve` 在出片、体检、`/status`
/// 里一直被调，每次都扫一遍盘不值得。`fresh` 为真时不看缓存（设置页那颗
/// 「索引」按钮：人按它就是因为刚往里放了东西）。
///
/// 扫得有边：跳过 `.` 开头的目录（`.git`、`.cache`），深度和条数都封顶，
/// 封到顶就把 `truncated` 置上。目录不存在回一份空的，不抛。
ModelIndex index_models(const std::filesystem::path& dir, bool fresh = false);

/// 按名字找一个模型文件，回绝对路径；找不到回空。
///
/// 先看 `dir / name` 本身；那儿没有，再在索引里找**文件名相同**的
/// （`name` 带的子目录不算数：`loras/x.safetensors` 挪到 `video/x.safetensors`
/// 照样认）。几份同名时，路径和 `name` 一模一样的优先，其次浅的，再其次按字母。
///
/// `bytes` 大于 0 时只认大小恰好是它的那份。
std::optional<std::filesystem::path> find_model(const std::filesystem::path& dir,
                                                const std::string& name,
                                                std::uint64_t bytes = 0);

/// 盘上变了（下完一个、人挪了目录）：扔掉缓存，下次问的时候重扫。
void forget_model_index();

}  // namespace changji::config
