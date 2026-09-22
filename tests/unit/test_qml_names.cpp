// 桌面端的 QML 文件名，不许和 import 进来的 Qt 类型撞名。
//
// 撞了会怎样：**同名的本地类型把 Qt 那个顶掉，而且一个告警都不报。**
// 2026-09-21 实撞——设置那张 sheet 一开始叫 `Settings.qml`，而 `Main.qml`
// 里 `import QtCore` 之后还有一处
//
//     Settings { id: prefs
//         property real peekWidth: 0
//         property bool sideOpen: true
//         property real sideWidth: 200
//     }
//
// 那是 QSettings，存侧栏宽度、面板宽度、上下对半那几个数的。被顶掉之后
// `prefs` 变成一个普通 `Item`——**那几行 `property` 声明照样合法**，所以
// QML 那层的错误处理（`qInstallMessageHandler`，见 main.cpp）一声不响，
// 界面上也什么都不少。真实后果是那几个数从此再也不落盘：拖宽了侧栏，
// 关掉再开又回到 200，而没有任何地方说过一句。
//
// 判据落在效果上（[[gui-verify-visible-and-measure]]）：把 plist 里的
// `sideWidth` 写成 333 再开，量出来的侧栏还是 200。改名之后是 333。
//
// ⚠️ **这张表是地板，不是天花板。** 它只列了这几个模块里"像是会被拿来当
// 文件名"的类型，列不全；但底下那条「模块没在表里就红」保证它不会因为有人
// 加了个新 import 而悄悄缩水——加 import 的那个人会被逼着把类型名补上来。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// 模块 → 它导出的、像是会被拿来当文件名的类型。
const std::map<std::string, std::vector<std::string>>& taken() {
    static const std::map<std::string, std::vector<std::string>> t = {
        {"QtCore", {"Settings", "SystemPalette", "FileSelector", "StandardPaths",
                    "Timer", "Instantiator", "Binding", "Connections"}},
        {"QtQuick", {"Item", "Rectangle", "Text", "TextInput", "TextEdit", "Image",
                     "AnimatedImage", "BorderImage", "Row", "Column", "Grid", "Flow",
                     "Repeater", "Loader", "Flickable", "MouseArea", "HoverHandler",
                     "TapHandler", "DragHandler", "PinchHandler", "ListView",
                     "GridView", "PathView", "Canvas", "Shortcut", "Timer",
                     "Connections", "Binding", "State", "Transition", "Behavior",
                     "Component", "FontLoader", "Gradient", "Accessible", "Window"}},
        {"QtQuick.Controls.Basic",
         {"Button", "Label", "TextField", "TextArea", "ScrollView", "ScrollBar",
          "Slider", "Switch", "CheckBox", "RadioButton", "ComboBox", "SpinBox",
          "Dialog", "Popup", "Menu", "MenuItem", "MenuBar", "ToolBar", "ToolTip",
          "ToolButton", "TabBar", "TabButton", "StackView", "SwipeView", "SplitView",
          "Page", "Pane", "Frame", "Control", "BusyIndicator", "ProgressBar",
          "Drawer", "Tumbler", "DelayButton", "RoundButton", "GroupBox"}},
        {"QtQuick.Layouts",
         {"RowLayout", "ColumnLayout", "GridLayout", "StackLayout", "Layout"}},
        {"QtQuick.Window", {"Window", "Screen"}},
        {"QtQuick.Effects", {"MultiEffect", "RectangularShadow"}},
        {"QtQuick.Dialogs", {"FileDialog", "FolderDialog", "MessageDialog",
                              "ColorDialog", "FontDialog"}},
        {"QtMultimedia",
         {"MediaPlayer", "VideoOutput", "AudioOutput", "AudioInput", "MediaDevices",
          "Video", "CaptureSession", "Camera", "SoundEffect", "MediaRecorder"}},
        // 自己这个模块：import 的是它自己，当然不算撞名。
        {"Changji", {}},
    };
    return t;
}

}  // namespace

TEST_CASE("桌面端的 QML：文件名不许盖掉 import 进来的 Qt 类型") {
    const fs::path dir{CHANGJI_DESKTOP_QML_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "读不到 " << dir.string());

    std::set<std::string> mine;        // 这个模块自己的文件名
    std::set<std::string> imported;    // 被 import 进来的模块
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".qml") continue;
        mine.insert(entry.path().stem().string());
        std::ifstream in(entry.path());
        REQUIRE(in.good());
        for (std::string line; std::getline(in, line);) {
            if (line.compare(0, 7, "import ") != 0) continue;
            std::string mod = line.substr(7);
            // 砍掉版本号、`as X`、行尾的空白
            const std::size_t sp = mod.find(' ');
            if (sp != std::string::npos) mod = mod.substr(0, sp);
            while (!mod.empty() && (mod.back() == '\r' || mod.back() == ' ')) mod.pop_back();
            if (!mod.empty()) imported.insert(mod);
        }
    }

    // 一个文件都没找着就是这条用例自己不认路了，不是"全都合规"。
    REQUIRE_MESSAGE(mine.size() > 8, "只找着 " << mine.size() << " 个 qml 文件——这条用例八成是自己不认路了");
    REQUIRE_MESSAGE(imported.count("QtQuick") == 1, "连 QtQuick 都没 import？这条用例八成是自己不认路了");

    // 表里没有的模块：**当场红**。不红的话，以后谁加一个新 import，这条
    // 守卫就静悄悄少管一片，而"少管了哪一片"没有任何地方看得出来。
    for (const auto& mod : imported) {
        CHECK_MESSAGE(taken().count(mod) == 1,
                      "import 了 " << mod << " 而 test_qml_names.cpp 的表里没有它——"
                      "把这个模块里像是会被拿来当文件名的类型补进去");
    }

    for (const auto& mod : imported) {
        const auto it = taken().find(mod);
        if (it == taken().end()) continue;
        for (const auto& type : it->second) {
            CHECK_MESSAGE(mine.count(type) == 0,
                          "qml/" << type << ".qml 和 " << mod << " 里的 " << type
                                 << " 撞名了：本地这个会把那个顶掉，而且一声不响——"
                                 "换个名字（比如 " << type << "Sheet.qml）");
        }
    }
}
