#pragma once

#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

// Hooks tab: lists running windowed apps and turns the shader on or off in the selected one.
//
// Enabling loads our hook DLL into the app the way overlays do it, via SetWindowsHookEx, so nothing
// is written into the app's folder and it takes effect immediately with no restart. Injection is
// one-way for the life of that process; the toggle after that just flips a shared flag, which the
// in-game Ctrl+Alt+S hotkey flips too.
class HooksPanel : public QWidget
{
    Q_OBJECT

public:
    explicit HooksPanel(QWidget *parent = nullptr);

private slots:
    void refresh();          // re-enumerate running apps, preserving the selection
    void updateButton();     // reflect the selected app's supported/injected state
    void toggleSelected();   // inject into, or toggle shading for, the selected app

private:
    QString hookDir() const;                     // where the hook DLLs and the helper live
    bool inject(quint32 pid, bool is64);         // load the hook DLL into that process

    quint32 selectedPid() const;
    QString selectedExe() const;

    QListWidget *m_list = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_timer = nullptr;
};
