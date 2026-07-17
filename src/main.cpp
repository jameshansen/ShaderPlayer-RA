#include "mainwindow.h"
#include "videowidget.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

// A WIN32-subsystem app has no console, so qDebug/qCritical would vanish. Tee them to a file.
static void logToFile(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static QFile log(QCoreApplication::applicationDirPath() + "/shaderplayer.log");
    static bool opened = log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    if (!opened)
        return;
    static const char *names[] = { "DEBUG", "WARN", "CRIT", "FATAL", "INFO" };
    QTextStream(&log) << names[qBound(0, int(type), 4)] << ": " << msg << '\n';
    log.flush();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ShaderPlayer"));
    app.setApplicationName(QStringLiteral("ShaderPlayer-RA"));
    qInstallMessageHandler(logToFile);

    // Request 4.6 core so librashader can use its DSA path and shader cache.
    QSurfaceFormat fmt;
    fmt.setVersion(4, 6);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument("files", "Video files to play.", "[files...]");
    QCommandLineOption selfTest(
        QStringList{ QStringLiteral("selftest") },
        QStringLiteral("Play for <seconds>, report what the pipeline actually rendered, then "
                       "exit. 0 = image, 2 = black/blank, 3 = never rendered."),
        QStringLiteral("seconds"));
    parser.addOption(selfTest);
    parser.process(app);

    try {
        MainWindow w;
        w.show();
        if (parser.positionalArguments().isEmpty())
            w.restorePlaylist();   // nothing asked for: pick up where we left off
        else
            w.openFiles(parser.positionalArguments());

        if (parser.isSet(selfTest)) {
            const int ms = int(parser.value(selfTest).toDouble() * 1000);
            QTimer::singleShot(ms, &w, [&w] {
                const FrameStats src = w.video()->probeSource();
                const FrameStats out = w.video()->probeOutput();
                qInfo("SELFTEST src: %s", qUtf8Printable(src.toString()));
                qInfo("SELFTEST out: %s", qUtf8Printable(out.toString()));
                qInfo("SELFTEST playlist: %lld entries", qlonglong(w.video()->playlist().size()));
                qInfo("SELFTEST preset: %s",
                      w.video()->preset().isEmpty() ? "<none>" : qUtf8Printable(w.video()->preset()));
                if (!src.valid || !out.valid) {
                    qInfo("SELFTEST result: FAIL (nothing rendered)");
                    qApp->exit(3);
                } else if (out.nonBlack < 0.01) {
                    qInfo("SELFTEST result: FAIL (output is black)");
                    qApp->exit(2);
                } else {
                    qInfo("SELFTEST result: PASS");
                    qApp->exit(0);
                }
            });
        }
        return app.exec();
    } catch (const std::exception &e) {
        QMessageBox::critical(nullptr, "ShaderPlayer-RA", e.what());
        return 1;
    }
}
