#pragma once

// 把一段现成的文本切成章节。
//
// 三个入口里的第二条：用户手里已经有小说/剧本/大纲，直接粘进来，不必让
// 模型再编一遍。**这一步不碰大模型**——切章节是机械活，靠标题行和段落
// 边界就能做，而且做得比模型稳：模型切同一段文本两次结果会不一样。
//
// ⚠️ 全程按 UTF-8 字符算，不按字节。

#include <cstddef>
#include <string>
#include <vector>

#include "models/story.hpp"

namespace changji::stages {

/// 一章的目标篇幅。没有标题行时按这个大小在段落边界上切。
///
/// 三千字大约合三四分钟片长（按一秒消化 15 字算）。切得太碎，章节列表
/// 会长到没法看；切得太大，一章要拍十几分钟，写正文、排分镜都跟着变重。
inline constexpr int kImportTargetChars = 3000;

/// 一章最多留多少个候选位置。
///
/// 段落边界全都当候选，长章节能攒出上千个——读一遍正文（story_analyze）
/// 每次要在里面找位置对得上的那一条，而且这些点还要存进 story.json。
/// 多到一定程度就等距抽稀，反正相邻两个段落边界差不了几个字。
inline constexpr std::size_t kImportMaxHooks = 400;

/// 把粘进来的文本切成章节。
///
/// 两种切法，按有没有标题行自动选：
///
/// 1. **认出标题行**（第三章、楔子、## 标题、Chapter 3）就按它切，
///    标题当章名。这是作者自己分的章，比任何启发式都准。
/// 2. 一个标题都认不出来，就按 target_chars 在**段落边界**上切，
///    章名取那一章的头一句。
///
/// 两种切法都会把章节内的**段落边界登记成候选位置**（Hook，text 为空）。
/// 不登记的话这一章里程序认得出来的位置就只有章界一个，读一遍正文
///（story_analyze）把「这儿悬着什么」挂上来时没有一条现成的可对，每一条
/// 都得新加。段落边界不是真正的钩子——真钩子要读懂剧情才找得出来——但它
/// 至少落在段与段之间，不在半句话中间。
std::vector<models::Chapter> split_pasted(const std::string& text,
                                          int target_chars = kImportTargetChars);

/// 把一段正文里的段落边界登记成候选位置（Hook，text 留空）。
///
/// 粘贴导入和逐章展开都要用：只要一章有了正文，它的候选位置就该按段落
/// 重算一遍。不登记的话这一章里只有章界一条，读一遍正文时标出来的那些
/// 说法一条都对不上现成的位置。
///
/// 多到 kImportMaxHooks 就等距抽稀——相邻两个段落边界差不了几个字。
std::vector<models::Hook> paragraph_hooks(const std::string& text);

/// 每个段落边界在正文里的字符位置（第 i 个 = 第 i+1 段的起始字符）。
///
/// `paragraph_hooks` 抽稀之后的那一份不能拿来数段落——逐章展开要靠「第几段
/// 结束就是第几场结束」把场的位置数出来，抽掉几个边界就全错位了。所以这里
/// 把没抽稀的原始那份单独给出来。
///
/// 传进来的文本要先 strip_ws，和 `paragraph_hooks` 里那一步保持一致。
std::vector<int> paragraph_breaks(const std::string& body);

}  // namespace changji::stages
