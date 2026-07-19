#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QThread>

#include "videowidget.h"   // ShaderParam

// The in-app half of the Hooks feature: a background thread inside ShaderPlayer that talks to the
// hook DLL living in the game.
//
// Two destinations, independently switchable, because one capture can feed both:
//
//   Display in app     Emit the game's frames. VideoWidget draws them through the same GL chain the
//                      video path uses, so the shader, the parameters and the letterboxing are
//                      literally the ones on screen -- which is what puts a game at the right scale
//                      when its resolution is not the window's. Works for every graphics API,
//                      because collecting a frame is easy and painting one back into a live game
//                      is not.
//
//   Apply to game      Run a librashader D3D11 chain of our own and hand the shaded frame back for
//                      the hook to paint over the game's backbuffer. Only D3D9 games can do this.
//
// With both on the frame is collected once and used twice, which costs one extra readback of an
// already small texture. With neither on the thread stops stamping its heartbeat and the hook drops
// straight back to presenting the game untouched.
//
// Wiring is a poll, not a signal: MainWindow calls setState() on a timer with whatever VideoWidget
// currently has. That covers every way a preset or parameter can change (panel, menu, settings
// restore) without threading a signal through each call site, and the thread picks changes up at
// the top of its loop where rebuilding a chain is safe.
class HookWorker : public QThread
{
    Q_OBJECT

public:
    explicit HookWorker(QObject *parent = nullptr);
    ~HookWorker() override;

    // Safe to call from the GUI thread at any rate; only differences do any work.
    void setState(const QString &preset, const QList<ShaderParam> &params, int sourceLines);

    // The two destinations. See the class comment; both may be on, and both off means passthrough.
    void setDisplayInApp(bool on) { m_displayInApp = on; }
    void setApplyToGame(bool on) { m_applyToGame = on; }

    // True while frames are actually flowing from a hooked game. Drives the panel's live indicator.
    bool shading() const { return m_shading; }

    // Frames per second collected from the game, averaged over the last second. Read from the GUI
    // thread for the panel's status line, and the number that says whether a slow picture is the
    // capture's fault or the display's.
    double fps() const { return m_fps; }

    void stop();

signals:
    // One captured game frame, at whatever size the hook captured it. Queued to the GUI thread,
    // where VideoWidget takes it as its source picture.
    void frameReady(const QImage &frame);

protected:
    void run() override;

private:
    QMutex m_lock;                  // guards the pending snapshot below
    QString m_preset;
    QList<ShaderParam> m_params;
    int m_sourceLines = 0;
    bool m_dirty = true;

    // Written on the worker thread, read on the GUI thread purely to display. An aligned double is
    // not torn by any machine we run on, and a stale reading of a rate meter is worth nothing less
    // than a fresh one, so this does not need a lock.
    volatile double m_fps = 0.0;

    volatile bool m_stop = false;
    volatile bool m_shading = false;
    volatile bool m_displayInApp = true;
    volatile bool m_applyToGame = false;
};
