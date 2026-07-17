#include "mainwindow.h"
#include "videowidget.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QMessageBox>
#include <QSet>
#include <QStyleHints>
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

    // Dark suits a video player, and it keeps the chrome from glowing next to the picture.
    // The style has to change too: windowsvista (Qt's default on Windows 10) hardcodes light
    // colours and ignores the scheme outright. Fusion paints every indicator itself, which is
    // the point -- windows11 honours the scheme but draws menu arrows and check marks as
    // glyphs from Segoe Fluent Icons, a font that only ships with Windows 11, so on Windows 10
    // they come out as empty boxes.
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    QApplication::setStyle(QStringLiteral("Fusion"));

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
                       "exit. 0 = image, 2 = black/blank, 3 = never rendered, "
                       "4 = chain stalled while paused."),
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
                    return;
                }
                if (out.nonBlack < 0.01) {
                    qInfo("SELFTEST result: FAIL (output is black)");
                    qApp->exit(2);
                    return;
                }

                // Pausing must not stall the chain, or an animated preset freezes with the
                // video. mpv stops handing over frames here, so this is really asking whether
                // anything else is still driving paintGL.
                // Only meaningful with a file still loaded: once the playlist ends mpv goes
                // idle and paintGL correctly draws nothing, which is not a stall.
                if (w.video()->sourceSize().isEmpty()) {
                    qInfo("SELFTEST paused repaints: skipped (playback ended, nothing to shade)");
                    qInfo("SELFTEST result: PASS");
                    qApp->exit(0);
                    return;
                }

                w.video()->setPaused(true);
                const size_t before = w.video()->painted();
                QTimer::singleShot(500, &w, [&w, before] {
                    const size_t frames = w.video()->painted() - before;
                    qInfo("SELFTEST paused repaints: %lld in 500ms", qlonglong(frames));

                    // Repainting is not the same as animating. Fingerprint a run of paused
                    // frames: crt-royale's field bob alternates 0/1, so two samples could land
                    // on the same parity by luck -- sample enough to make that decisive.
                    auto *seen = new QList<quint64>;
                    auto *left = new int(12);
                    auto *t = new QTimer(&w);
                    t->setInterval(40);
                    QObject::connect(t, &QTimer::timeout, &w, [&w, seen, left, t, frames] {
                        seen->append(w.video()->probeOutput().hash);
                        if (--*left > 0)
                            return;
                        t->stop();
                        // Print the run as letters: ABABAB is a field bob still alternating,
                        // AAABBB is one transition that then settled.
                        QList<quint64> distinct;
                        QString pattern;
                        for (quint64 h : *seen) {
                            if (!distinct.contains(h))
                                distinct.append(h);
                            pattern += QChar('A' + int(distinct.indexOf(h)));
                        }
                        qInfo("SELFTEST paused output: %lld distinct in 12 samples (%s) seq=%s",
                              qlonglong(distinct.size()), distinct.size() > 1 ? "advancing" : "static",
                              qUtf8Printable(pattern));
                        // Generous: a heavy preset at 4K only manages ~13 here, and the point
                        // is to catch a dead timer (0), not to benchmark the GPU.
                        const bool stalled = frames < 5;
                        delete seen; delete left;
                        if (stalled) {
                            qInfo("SELFTEST result: FAIL (chain stalled while paused)");
                            qApp->exit(4);
                        } else {
                            qInfo("SELFTEST result: PASS");
                            qApp->exit(0);
                        }
                    });
                    t->start();
                });
            });
        }
        return app.exec();
    } catch (const std::exception &e) {
        QMessageBox::critical(nullptr, "ShaderPlayer-RA", e.what());
        return 1;
    }
}

