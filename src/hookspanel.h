#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class HookWorker;
class QCheckBox;
class QLabel;
class QListWidget;
class QTimer;

// Hooks tab: the games ShaderPlayer can take the picture from, each with a play button.
//
// Only apps that have a graphics runtime loaded are listed, because everything else is a text
// editor or a background service and offering to hook it is noise. Pressing play loads our hook DLL
// into that app the way overlays do it, via SetWindowsHookEx, so nothing is written into the app's
// folder and it takes effect immediately with no restart. Injection is one-way for the life of that
// process; the button after that just flips a shared flag, which the in-game Ctrl+Alt+S hotkey
// flips too.
//
// The list is a stack, not a sorted view: a newly launched game is appended at the bottom and
// nothing above it ever moves. Sorting by name meant a game reordering itself under the cursor
// between one refresh and the next, which is how you end up pressing play on the wrong thing.
class HooksPanel : public QWidget
{
    Q_OBJECT

public:
    explicit HooksPanel(QWidget *parent = nullptr);

    // Lets the panel report what the worker is doing and drive its two destination switches.
    void attachWorker(HookWorker *worker);

public slots:
    // The play/stop button on one row. Public because --hooktest drives it directly, which keeps
    // the automated test on exactly the path a click takes rather than a parallel copy of it.
    void toggle(quint32 pid);

private slots:
    void refresh();                  // re-enumerate, rebuild rows only when the set changed

private:
    struct Row { quint32 pid; QString exe; QString title; bool is64; };

    QString hookDir() const;                     // where the hook DLLs and the helper live
    bool inject(quint32 pid, bool is64);         // load the hook DLL into that process
    QList<Row> enumerate();                      // compatible processes, in stack order
    void rebuild(const QList<Row> &rows);
    void updateButtons();

    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_applyToGame = nullptr;
    QCheckBox *m_displayInApp = nullptr;
    QTimer *m_timer = nullptr;
    HookWorker *m_worker = nullptr;

    QList<quint32> m_order;                      // pids in the order they were first seen
    QHash<quint32, bool> m_compatible;           // pid -> has a graphics runtime loaded
    int m_pass = 0;                              // refresh counter, for cheap re-checks
};
