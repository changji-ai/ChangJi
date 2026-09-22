// 永远跑的那种动画，必须自己说清什么时候跑，而且**不许靠 `parent`**。
//
// 2026-09-21 实撞，代价是 12% CPU：面板标题行右边那句「正在动…」，动画写的是
//
//     SequentialAnimation on opacity { loops: Animation.Infinite
//                                      running: parent.visible ... }
//
// 这个动画是挂在那个 `Text` 上的**属性值源**，而它里头的 `parent` 指的是
// **那个 Text 的爹**（标题行），不是 Text 自己。标题行是"面板一开就在"的，
// 于是这一下喘气从面板打开那一刻起就没停过——**哪怕那行字根本看不见**。
//
// 看不见为什么还费电：每改一次 `opacity`，Qt 都把整扇窗标脏
//（`setOpacity` → `maybeUpdate`），于是整窗按 60 帧重画。量出来的：
//
//     没开面板 0.83% ／ 开着故事那一格 12.0% ／ 开着镜头那一格 14.4%
//     改成 running: <那个 Text 的 id>.visible 之后，两档都回到 0.8%
//
// 所以钉两条：
//
//   一、`loops: Animation.Infinite` 的动画**必须有一条 `running:`**。
//       没有的话它从程序起来那一刻跑到关窗，谁也拦不住。
//   二、那条 `running:` **不许出现 `parent.`**。要指自己就起个 `id`。
//
// ⚠️ 这条只看**源码里怎么写的**，看不出"这个条件对不对"。它拦的是那一族
// 一眼看不出来、量了才知道的写法。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "desktop_sources.hpp"

namespace fs = std::filesystem;

namespace {

/// 一处有问题的：哪个文件、第几行、怎么了。
struct Bad {
    std::string file;
    int line = 0;
    std::string why;
};

/// 这一行是不是 `XxxAnimation on 某属性 {` 的开头。是的话回缩进宽度。
int opens_value_source(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    for (const char* k : {"SequentialAnimation", "ParallelAnimation", "NumberAnimation",
                          "PropertyAnimation", "ColorAnimation", "RotationAnimation"}) {
        const std::string name(k);
        if (line.compare(i, name.size(), name) != 0) continue;
        // 后面必须跟着 ` on `——`Behavior` 里那些不是属性值源，它们只在值
        // 变了的时候跑一下，不是"永远跑"。
        const std::size_t j = i + name.size();
        if (line.compare(j, 4, " on ") != 0) continue;
        if (line.find('{') == std::string::npos) continue;
        return static_cast<int>(i);
    }
    return -1;
}

int brace_delta(const std::string& line) {
    int d = 0;
    for (const char c : line) {
        if (c == '{') ++d;
        if (c == '}') --d;
    }
    return d;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("桌面端的 QML：永远跑的动画要说清什么时候跑，而且别靠 parent") {
    if (!changji_test::desktop_sources()) return;
    const fs::path dir{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    std::vector<Bad> bad;
    int forever = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".qml") continue;
        std::ifstream in(entry.path());
        REQUIRE(in.good());
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (opens_value_source(lines[i]) < 0) continue;
            // 数花括号找这一块的闭合。
            int depth = 0;
            std::size_t end = i;
            for (std::size_t j = i; j < lines.size(); ++j) {
                depth += brace_delta(lines[j]);
                end = j;
                if (depth == 0 && j > i) break;
            }

            bool endless = false;
            std::string running;
            for (std::size_t j = i; j <= end && j < lines.size(); ++j) {
                if (has(lines[j], "loops:") && has(lines[j], "Animation.Infinite")) {
                    endless = true;
                }
                const std::size_t at = lines[j].find("running:");
                // **只认这一块自己那一层的 `running:`**：里头嵌的那几段
                // `NumberAnimation` 不带这个属性，不会误判。
                if (at != std::string::npos && running.empty()) {
                    running = lines[j].substr(at + 8);
                }
            }
            if (!endless) continue;
            ++forever;

            if (running.empty()) {
                bad.push_back({entry.path().filename().string(),
                               static_cast<int>(i + 1),
                               "`loops: Animation.Infinite` 却没有 `running:`"
                               "——它会从程序起来一直跑到关窗"});
                continue;
            }
            if (has(running, "parent.")) {
                bad.push_back({entry.path().filename().string(),
                               static_cast<int>(i + 1),
                               "`running:` 里用了 `parent.`——属性值源里的 "
                               "`parent` 指的是「挂着它那个东西的爹」，不是它自己；"
                               "要指自己就起个 id"});
            }
        }
    }

    // 一个都没找着就是这条用例自己不认路了，不是"全都合规"。
    REQUIRE_MESSAGE(forever >= 4,
                    "只找着 " << forever << " 处永远跑的动画——这条用例八成是自己不认路了");

    for (const auto& b : bad) {
        CAPTURE(b.file);
        CAPTURE(b.line);
        CHECK_MESSAGE(false, b.file << ":" << b.line << " " << b.why);
    }
    CHECK(bad.empty());
}
