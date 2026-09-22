// 铺满一块**圆角** `Rectangle` 的图（或者视频），**必须自己把方角切掉**。
//
// 2026-09-21 用户报的：「图片/视频的圆角都不正常」。查出来不是"不正常"，
// 是压根没有——四处全是这么写的：
//
//     Rectangle { radius: 8; clip: true
//         Image { anchors.fill: parent; ... }
//     }
//
// 看着天经地义，可 **`clip: true` 按外接矩形裁，不按圆角裁**。图是不透明的，
// 四个方角原样盖在圆角上，那圈 `radius` 一个像素都露不出来。
//
// ⚠️ **这个错在代码里看不出来，在图上也不刺眼**——它长得就像"这儿本来就是
// 方角"。四处这么写了不知道多久，谁都没发现，直到有人把两块摆在一起比。
// 治它的是 `RoundMask.qml`（往外让一圈，拿边框把方角糊掉）。
//
// 所以钉一条：**一块 `Rectangle` 带着 `radius`，直接孩子里有铺满的
// `Image` / `VideoOutput`，那它的直接孩子里就得有一个 `RoundMask`。**
//
// 不适用的那一族（图不铺满、底下不是纯色）留给调用方自己判——真碰上了，
// 那时候该上的是 `MultiEffect` 的遮罩，而不是把这条用例删掉。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// 把注释切掉再看。
///
/// ⚠️ **这一步不能省。** 头一版忘了，于是这条用例**假绿了一次**：把
/// `RoundMask { … }` 那一句真删掉，它还是绿的——因为那一句上面那行注释里
/// 写着「见 `RoundMask.qml`」，一样被 `find` 认成"有"。一条只认字面的用例
/// 被一句注释骗过去，正是它自己该防的那一类。
std::string code_of(const std::string& line) {
    const auto cut = line.find("//");
    return cut == std::string::npos ? line : line.substr(0, cut);
}

bool has(const std::string& hay, const std::string& needle) {
    return code_of(hay).find(needle) != std::string::npos;
}

int depth_of(const std::string& line) {
    int d = 0;
    for (const char c : code_of(line)) {
        if (c == '{') ++d;
        if (c == '}') --d;
    }
    return d;
}

/// 一处漏了的：哪个文件、第几行。
struct Bare {
    std::string file;
    int line = 0;
};

}  // namespace

TEST_CASE("桌面端的 QML：铺满圆角块的图和视频，方角得自己切掉") {
    const fs::path dir{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    std::vector<Bare> bare;
    int checked = 0;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".qml") continue;
        std::ifstream in(entry.path());
        REQUIRE(in.good());
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!has(lines[i], "Rectangle {")) continue;

            // 这一块从这儿到配对的那个括号。
            int depth = 0;
            std::size_t j = i;
            bool own_radius = false;     ///< 这一块自己有没有 radius
            bool filled = false;         ///< 直接孩子里有没有铺满的图 / 视频
            bool masked = false;         ///< 直接孩子里有没有 RoundMask
            std::size_t kid_at = 0;      ///< 那个孩子的头一行，报错用
            std::string kid_head;        ///< 正在看的那个直接孩子是什么

            for (; j < lines.size(); ++j) {
                const int before = depth;
                depth += depth_of(lines[j]);

                // 深度 1 ＝ 这一块自己的身子。
                if (before == 1 && has(lines[j], "radius:")) own_radius = true;
                if (before == 1 && has(lines[j], "RoundMask")) masked = true;
                // 直接孩子的头一行也在深度 1 上（它自己的 `{` 还没算进去）。
                if (before == 1 && (has(lines[j], "Image {") ||
                                    has(lines[j], "VideoOutput {"))) {
                    kid_head = lines[j];
                    kid_at = j;
                }
                // 孩子的身子在深度 2 上。
                if (before == 2 && !kid_head.empty() &&
                    has(lines[j], "anchors.fill: parent")) {
                    filled = true;
                }
                if (before == 2 && depth == 1) kid_head.clear();   // 这个孩子完了

                if (depth == 0 && j > i) break;
            }

            if (!own_radius || !filled) continue;
            ++checked;
            if (!masked) {
                bare.push_back({entry.path().filename().string(),
                                static_cast<int>(kid_at + 1)});
            }
        }
    }

    // 一处都没找着就是这条用例自己不认路了，不是"全都合规"。
    REQUIRE_MESSAGE(checked >= 3,
                    "只找着 " << checked << " 处铺满圆角块的图——这条用例八成是自己不认路了");

    for (const auto& b : bare) {
        CAPTURE(b.file);
        CAPTURE(b.line);
        CHECK_MESSAGE(false,
                      b.file << ":" << b.line
                      << " 一张图铺满了一块圆角 Rectangle，却没有 RoundMask——"
                      "`clip: true` 按外接矩形裁，那圈圆角一个像素都露不出来，"
                      "而它看着就像'这儿本来就是方角'");
    }
    CHECK(bare.empty());
}
