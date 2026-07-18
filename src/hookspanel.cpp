#include "hookspanel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../hooks/common/hookinject.h"

namespace {

struct Proc { quint32 pid; QString exe; QString title; bool is64; };

QString exePathForPid(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
    QString path;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) path = QString::fromWCharArray(buf, int(n));
    CloseHandle(h);
    return path;
}

// Which hook DLL a process needs. A 32-bit process on 64-bit Windows runs under WOW64, which is
// what IsWow64Process2 reports through its process-machine argument.
bool is64BitProcess(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN, nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    const bool ok = IsWow64Process2(h, &processMachine, &nativeMachine);
    CloseHandle(h);
    return ok && processMachine == IMAGE_FILE_MACHINE_UNKNOWN;   // not under WOW64, so native 64-bit
}

// One entry per process that owns a visible, titled, top-level window: that is the set of apps a
// user would actually want to shade, and it keeps background services out of the list.
BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp)
{
    auto *out = reinterpret_cast<QList<Proc> *>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    for (const Proc &p : *out) if (p.pid == pid) return TRUE;
    QString exe = exePathForPid(pid);
    if (exe.isEmpty()) return TRUE;
    std::wstring t(size_t(len) + 1, L'\0');
    GetWindowTextW(hwnd, t.data(), len + 1);
    out->append({ quint32(pid), exe, QString::fromWCharArray(t.c_str()), is64BitProcess(pid) });
    return TRUE;
}

// The control block, created here so the enabled flag exists before any game is hooked. The hook DLL
// opens the same named mapping from inside the game.
hook::Ctrl *ctrl()
{
    static hook::Ctrl *c = [] () -> hook::Ctrl * {
        HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(hook::Ctrl), hook::CTRL_NAME);
        if (!m) return nullptr;
        return static_cast<hook::Ctrl *>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::Ctrl)));
    }();
    return c;
}

} // namespace

HooksPanel::HooksPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Running apps. Pick one and turn the shader on.\n"
                               "In game, Ctrl+Alt+S toggles it.\n"
                               "Single-player / offline games only, hooking multiplayer titles trips anti-cheat."), this);
    hint->setWordWrap(true);
    lay->addWidget(hint);

    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(24, 24));
    lay->addWidget(m_list, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    m_toggle = new QPushButton(tr("Turn Shader On"), this);
    m_toggle->setEnabled(false);
    lay->addWidget(m_toggle);

    connect(m_list, &QListWidget::currentItemChanged, this, &HooksPanel::updateButton);
    connect(m_toggle, &QPushButton::clicked, this, &HooksPanel::toggleSelected);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);   // also refreshes the live state, so the in-game hotkey shows up here
    connect(m_timer, &QTimer::timeout, this, &HooksPanel::refresh);
    m_timer->start();
    refresh();
}

void HooksPanel::refresh()
{
    const quint32 keep = selectedPid();

    QList<Proc> procs;
    EnumWindows(&enumProc, reinterpret_cast<LPARAM>(&procs));
    std::sort(procs.begin(), procs.end(), [](const Proc &a, const Proc &b) {
        return QFileInfo(a.exe).fileName().toLower() < QFileInfo(b.exe).fileName().toLower();
    });

    const DWORD self = GetCurrentProcessId();
    QFileIconProvider icons;
    m_list->blockSignals(true);
    m_list->clear();
    for (const Proc &p : procs) {
        if (p.pid == self) continue;                    // don't offer to hook ourselves
        QFileInfo fi(p.exe);
        auto *item = new QListWidgetItem(icons.icon(fi), QStringLiteral("%1  -  %2").arg(fi.fileName(), p.title));
        item->setData(Qt::UserRole, p.pid);
        item->setData(Qt::UserRole + 1, p.exe);
        item->setData(Qt::UserRole + 2, p.is64);
        item->setToolTip(QStringLiteral("%1\nPID %2 (%3-bit)").arg(p.exe).arg(p.pid).arg(p.is64 ? 64 : 32));
        m_list->addItem(item);
        if (p.pid == keep) m_list->setCurrentItem(item);
    }
    m_list->blockSignals(false);
    updateButton();
}

quint32 HooksPanel::selectedPid() const
{
    auto *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toUInt() : 0;
}

QString HooksPanel::selectedExe() const
{
    auto *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole + 1).toString() : QString();
}

QString HooksPanel::hookDir() const
{
#ifdef SHADERPLAYER_HOOK_DIR
    const QString dev = QStringLiteral(SHADERPLAYER_HOOK_DIR);
    if (QFile::exists(dev + "/sphook32.dll")) return dev;   // dev tree
#endif
    return QCoreApplication::applicationDirPath();          // installed: they ship beside the exe
}

// 64-bit targets we can hook ourselves. 32-bit ones need a 32-bit process to call SetWindowsHookEx,
// which is all sphelper32.exe is.
bool HooksPanel::inject(quint32 pid, bool is64)
{
    const QString dll = hookDir() + (is64 ? "/sphook64.dll" : "/sphook32.dll");
    if (!QFile::exists(dll)) {
        QMessageBox::warning(this, tr("Turn shader on"),
                             tr("%1 is missing. Build hooks/inject or reinstall.").arg(QFileInfo(dll).fileName()));
        return false;
    }
    const QString native = QDir::toNativeSeparators(dll);
    if (is64) {
        const std::wstring path = native.toStdWString();
        return hook::injectInto(pid, path.c_str());
    }

    const QString helper = hookDir() + "/sphelper32.exe";
    if (!QFile::exists(helper)) {
        QMessageBox::warning(this, tr("Turn shader on"), tr("sphelper32.exe is missing, so 32-bit apps cannot be hooked."));
        return false;
    }
    QProcess p;
    p.start(helper, { QString::number(pid), native });
    return p.waitForFinished(10000) && p.exitCode() == 0;
}

void HooksPanel::updateButton()
{
    const quint32 pid = selectedPid();
    if (!pid) {
        m_toggle->setEnabled(false);
        m_toggle->setText(tr("Turn Shader On"));
        m_status->clear();
        return;
    }
    const QFileInfo fi(selectedExe());
    hook::Ctrl *c = ctrl();
    const bool injected = hook::alreadyInjected(pid);
    const bool on = c && c->enabled && c->hookedPid == pid;

    m_toggle->setEnabled(true);
    m_toggle->setText(on ? tr("Turn Shader Off in %1").arg(fi.fileName())
                         : tr("Turn Shader On in %1").arg(fi.fileName()));

    if (on)
        m_status->setText(tr("Shading %1. Ctrl+Alt+S in game toggles it.").arg(fi.fileName()));
    else if (injected)
        m_status->setText(tr("%1 is hooked and idle.").arg(fi.fileName()));
    else
        m_status->setText(tr("%1 is not hooked yet. Turning the shader on hooks it, no restart needed.").arg(fi.fileName()));
}

void HooksPanel::toggleSelected()
{
    const quint32 pid = selectedPid();
    if (!pid) return;
    auto *item = m_list->currentItem();
    const bool is64 = item->data(Qt::UserRole + 2).toBool();
    const QFileInfo fi(selectedExe());

    hook::Ctrl *c = ctrl();
    if (!c) {
        QMessageBox::warning(this, tr("Hooks"), tr("Could not open the shared control block."));
        return;
    }

    if (c->enabled && c->hookedPid == pid) {   // already shading this one: just switch it off
        c->enabled = 0;
        updateButton();
        return;
    }

    if (!hook::alreadyInjected(pid) && !inject(pid, is64)) {
        QMessageBox::warning(this, tr("Turn shader on"),
            tr("Could not hook %1.\n\nSome apps run at a higher integrity level than ShaderPlayer, "
               "which blocks this. Anti-cheat also blocks it, by design.").arg(fi.fileName()));
        return;
    }

    c->hookedPid = pid;   // hand the pipeline to the app the user just picked
    c->enabled = 1;
    updateButton();
}
