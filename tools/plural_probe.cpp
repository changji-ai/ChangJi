// **去问 Qt 本人：编好的 `.qm` 里，哪个数落到哪一档。**
//
// 界面那半截的复数是 Qt 挑的档（`<numerusform>`），引擎那半截是我们自己挑的
// （`util/say.cpp` 的 `plural_of`）。两边的话**摆在同一块屏上**，挑法差一处
// 就是半新半旧的样子，而且不报错。所以挑法不能靠推——这个小程序把编好的
// `.qm` 装起来，一个数一个数问过去，问出来的写进 `cpp/i18n/qt_plural_forms.tsv`，
// `test_say.cpp` 拿它盯着 `plural_of`。
//
// 实测的结论：**十一种语言里十种和 CLDR 一处不差，只有印地语的 0 不一样**
// （CLDR 算单数，Qt 算复数）。跟 Qt，理由写在 say.cpp 那一段。
//
// 怎么跑（这台 Mac 上，Qt 是 brew 装的）：
//
//     QT=$(brew --prefix qt)
//     clang++ -std=c++17 cpp/tools/plural_probe.cpp -o /tmp/plural_probe \
//         -I"$QT/lib/QtCore.framework/Headers" -F"$QT/lib" -framework QtCore \
//         -Wl,-rpath,"$QT/lib"
//     /tmp/plural_probe desktop/i18n ShotWall '%1 · %n 镜，还没出片' en:0,1,2,5
//
// ⚠️ **拿来问的那一句，每一档的字得互不相同**——两档一样的话问出来分不清
// 落在哪一档。`%1 · %n 镜，还没出片` 是挑过的，十一种语言里各档都不一样。
//
// ⚠️ **不进构建。** 它只在"要重新量一遍对照表"的时候手跑一次，编它要 Qt 的
// 头文件，而引擎那一半的测试是不链 Qt 的。
#include <QCoreApplication>
#include <QTranslator>
#include <QTextStream>
#include <QStringList>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const QString dir = QString::fromLocal8Bit(argv[1]);
    const QString ctx = QString::fromLocal8Bit(argv[2]);
    const QString src = QString::fromUtf8(argv[3]);
    for (int i = 4; i < argc; ++i) {
        const QStringList bits = QString::fromLocal8Bit(argv[i]).split(':');
        const QString lang = bits[0];
        QTranslator tr;
        if (!tr.load(dir + "/changji_" + lang + ".qm")) { out << lang << " 装不上\n"; continue; }
        app.installTranslator(&tr);
        out << "== " << lang << "\n";
        for (const QString& s : bits[1].split(',')) {
            const int n = s.toInt();
            out << "   n=" << n << "\t"
                << QCoreApplication::translate(ctx.toUtf8().constData(),
                                               src.toUtf8().constData(), nullptr, n)
                << "\n";
        }
        app.removeTranslator(&tr);
    }
    out.flush();
    return 0;
}
