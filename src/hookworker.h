#pragma once

#include <QList>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QThread>

#include "videowidget.h"   // ShaderParam

// The in-app half of the Hooks feature: what used to be hooks/worker/worker.exe, living on a
// background thread inside ShaderPlayer so the shader running in a hooked game is the shader
// showing in the app. It owns a private D3D11 device and a librashader D3D11 chain, entirely
// separate from VideoWidget's GL chain.
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
    void setState(const QString &preset, const QList<ShaderParam> &params, const QSize &source);

    // True while a hooked game is actually being shaded. Drives the panel's live indicator.
    bool shading() const { return m_shading; }

    void stop();

protected:
    void run() override;

private:
    QMutex m_lock;                  // guards the pending snapshot below
    QString m_preset;
    QList<ShaderParam> m_params;
    QSize m_source;
    bool m_dirty = true;

    volatile bool m_stop = false;
    volatile bool m_shading = false;
};
