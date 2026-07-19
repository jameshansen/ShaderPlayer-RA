#include "mainwindow.h"
#include "videowidget.h"
#ifdef SHADERPLAYER_HOOKS
#include "hookspanel.h"
#include "hookworker.h"
#endif

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

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#ifdef SHADERPLAYER_HOOKS
// The newest running process with this image name. Newest because a test run launches its stand-in
// game moments before asking, and a stale one left by a previous run would otherwise win.
static quint32 pidNamed(const QString &exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    quint32 best = 0;
    FILETIME bestStart{};
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (QString::fromWCharArray(pe.szExeFile).compare(exe, Qt::CaseInsensitive) != 0)
            continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h)
            continue;
        FILETIME start{}, dummy{};
        if (GetProcessTimes(h, &start, &dummy, &dummy, &dummy) &&
            CompareFileTime(&start, &bestStart) >= 0) {
            bestStart = start;
            best = pe.th32ProcessID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return best;
}
#endif

// Which module did we die in? An access violation inside libmpv means something very
// different from one inside our own code, and the log alone cannot tell them apart.
static void logModuleFor(void *addr, const char *label)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) || !mod) {
        qCritical("%s %p <unknown module>", label, addr);
        return;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char *base = strrchr(path, '\\');
    qCritical("%s %p  %s+0x%llx", label, addr, base ? base + 1 : path,
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr)
                                              - reinterpret_cast<uintptr_t>(mod)));
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS *info)
{
    qCritical("=== CRASH code=0x%08lX ===", info->ExceptionRecord->ExceptionCode);
    logModuleFor(info->ExceptionRecord->ExceptionAddress, "faulting address:");

    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Rip;      frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;   frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;   frame.AddrStack.Mode = AddrModeFlat;

    HANDLE proc = GetCurrentProcess();
    for (int i = 0; i < 24; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (!frame.AddrPC.Offset)
            break;
        logModuleFor(reinterpret_cast<void *>(frame.AddrPC.Offset), "  frame:");
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

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
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(crashHandler);

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
    QCommandLineOption hookTest(
        QStringList{ QStringLiteral("hooktest") },
        QStringLiteral("Hook the running process named <exe>, wait for its frames to arrive, then "
                       "report what came through and exit. 0 = frames arriving and animating, "
                       "5 = no frames, 6 = frames but frozen, 7 = frames upside down, "
                       "8 = no such process."),
        QStringLiteral("exe"));
    parser.addOption(hookTest);
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

                // Pausing must not stall the chain, or an animated preset freezes with the video.
                // mpv stops handing over frames here, so this is really asking whether anything
                // else is still driving paintGL.
                //
                // This used to be skipped once playback ended, back when an idle mpv meant paintGL
                // correctly drew nothing. The chain now runs continuously and falls back to the
                // no-signal screen, so there is no longer any state in which stalling is correct
                // and the check always applies.
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
#ifdef SHADERPLAYER_HOOKS
        // Drive the Hooks panel the way a click does, then read the pipeline's own pixels rather
        // than screenshotting the window. What this proves is the whole chain in one go: the DLL got
        // into the process, a backend claimed it, frames crossed the shared texture, and they came
        // out of the shader the right way up.
        if (parser.isSet(hookTest)) {
            const QString exe = parser.value(hookTest);
            QTimer::singleShot(2500, &w, [&w, exe] {
                const quint32 pid = pidNamed(exe);
                qInfo("HOOKTEST target: %s pid %u", qUtf8Printable(exe), pid);
                if (!pid) { qInfo("HOOKTEST result: FAIL (no such process)"); qApp->exit(8); return; }
                w.hooks()->toggle(pid);

                // Injection, the game's next present, the handoff and a repaint all have to happen
                // before there is anything to look at.
                QTimer::singleShot(7000, &w, [&w] {
                    const FrameStats a = w.video()->probeSource();
                    const FrameStats out = w.video()->probeOutput();
                    qInfo("HOOKTEST hookActive: %d", int(w.video()->hookActive()));
                    qInfo("HOOKTEST src: %s", qUtf8Printable(a.toString()));
                    qInfo("HOOKTEST out: %s", qUtf8Printable(out.toString()));
                    if (!w.video()->hookActive() || !a.valid || a.nonBlack < 0.05) {
                        qInfo("HOOKTEST result: FAIL (no frames from the game)");
                        qApp->exit(5);
                        return;
                    }
                    // The test game's marker square is in its top-left eighth, and its field colour
                    // cycles, so one sample answers orientation and two answer liveness.
                    if (a.topLeftBias <= 0.0) {
                        qInfo("HOOKTEST result: FAIL (frame is upside down)");
                        qApp->exit(7);
                        return;
                    }
                    const quint64 first = a.hash;
                    const size_t paintedBefore = w.video()->painted();
                    QTimer::singleShot(2000, &w, [&w, first, paintedBefore] {
                        const FrameStats b = w.video()->probeSource();
                        qInfo("HOOKTEST src again: %s", qUtf8Printable(b.toString()));
                        // The two rates that localise a slow picture: what the worker collected from
                        // the game, and what actually reached the screen. A gap between them is the
                        // display's fault, and both being low is the capture's.
                        qInfo("HOOKTEST capture: %.1f fps, painted: %.1f fps",
                              w.hookWorker() ? w.hookWorker()->fps() : 0.0,
                              double(w.video()->painted() - paintedBefore) / 2.0);
                        if (b.hash == first) {
                            qInfo("HOOKTEST result: FAIL (frames frozen)");
                            qApp->exit(6);
                            return;
                        }
                        qInfo("HOOKTEST result: PASS");
                        qApp->exit(0);
                    });
                });
            });
        }
#endif
        return app.exec();
    } catch (const std::exception &e) {
        QMessageBox::critical(nullptr, "ShaderPlayer-RA", e.what());
        return 1;
    }
}

