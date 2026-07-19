#include "hookspanel.h"

#include "hookworker.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include "../hooks/common/hookinject.h"

namespace {

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

// Has this process loaded a graphics runtime we have a backend for? That is the honest definition of
// "compatible": a window alone means nothing (Notepad has one), and the module list is the same
// thing the hook DLL itself looks at when it decides what to patch.
//
// SNAPMODULE32 is what lets a 64-bit ShaderPlayer see a 32-bit game's modules. Without it every
// 32-bit game reads as incompatible, which is most of the ones worth hooking.
bool hasGraphicsRuntime(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;   // usually a higher-integrity process
    static const wchar_t *wanted[] = { L"d3d9.dll", L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"opengl32.dll" };
    bool found = false;
    MODULEENTRY32W me{ sizeof(me) };
    for (BOOL more = Module32FirstW(snap, &me); more && !found; more = Module32NextW(snap, &me))
        for (const wchar_t *w : wanted)
            if (_wcsicmp(me.szModule, w) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

struct Found { QList<QPair<quint32, QString>> procs; };   // pid -> window title

// One entry per process that owns a visible, titled, top-level window: the set of apps a user could
// plausibly want to shade, before the compatibility filter narrows it further.
BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp)
{
    auto *out = reinterpret_cast<Found *>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    for (const auto &p : out->procs) if (p.first == pid) return TRUE;
    std::wstring t(size_t(len) + 1, L'\0');
    GetWindowTextW(hwnd, t.data(), len + 1);
    out->procs.append({ quint32(pid), QString::fromWCharArray(t.c_str()) });
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

bool alive(quint32 pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool yes = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return yes;
}

} // namespace

HooksPanel::HooksPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Games ShaderPlayer can take the picture from. Press play on one and it "
                               "appears here, through the current shader.\n"
                               "In game, Ctrl+Alt+S toggles it.\n"
                               "Single-player / offline games only, hooking multiplayer titles trips anti-cheat."), this);
    hint->setWordWrap(true);
    lay->addWidget(hint);

    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(24, 24));
    m_list->setSelectionMode(QAbstractItemView::NoSelection);   // the play button is the only control
    lay->addWidget(m_list, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    // Two destinations, not a choice between them: one capture can feed both, and seeing the shaded
    // picture here while the game shows it too is a reasonable thing to want.
    m_applyToGame = new QCheckBox(tr("Apply to Game"), this);
    m_applyToGame->setToolTip(tr("Paint the shaded picture back over the game's own window.\n"
                                 "Direct3D 9 games only."));
    connect(m_applyToGame, &QCheckBox::toggled, this, [this](bool on) {
        if (m_worker) m_worker->setApplyToGame(on);
    });
    lay->addWidget(m_applyToGame);

    m_displayInApp = new QCheckBox(tr("Display Output in ShaderPlayer"), this);
    m_displayInApp->setChecked(true);
    m_displayInApp->setToolTip(tr("Show the game here, through the current shader. This is what fits a "
                                  "game rendering at a different resolution to this window, and it "
                                  "works for every graphics API."));
    connect(m_displayInApp, &QCheckBox::toggled, this, [this](bool on) {
        if (m_worker) m_worker->setDisplayInApp(on);
    });
    lay->addWidget(m_displayInApp);

    m_timer = new QTimer(this);
    // Slow, because a pass now costs a module snapshot per candidate process and the rows only
    // change when a game starts or exits. The live button state is refreshed on the same tick.
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &HooksPanel::refresh);
    m_timer->start();
    refresh();
}

void HooksPanel::attachWorker(HookWorker *worker)
{
    m_worker = worker;
    if (!m_worker) return;
    m_worker->setApplyToGame(m_applyToGame->isChecked());
    m_worker->setDisplayInApp(m_displayInApp->isChecked());
}

QList<HooksPanel::Row> HooksPanel::enumerate()
{
    Found found;
    EnumWindows(&enumProc, reinterpret_cast<LPARAM>(&found));
    const DWORD self = GetCurrentProcessId();
    ++m_pass;

    QHash<quint32, Row> live;
    for (const auto &p : found.procs) {
        if (p.first == self) continue;                  // don't offer to hook ourselves
        const QString exe = exePathForPid(p.first);
        if (exe.isEmpty()) continue;

        // A game that has just launched may not have created its device yet, so a "no" is provisional
        // and gets asked again every few passes. A "yes" never becomes a no, so it is cached for good.
        auto it = m_compatible.find(p.first);
        if (it == m_compatible.end() || (!it.value() && m_pass % 3 == 0))
            it = m_compatible.insert(p.first, hasGraphicsRuntime(p.first));
        if (!it.value()) continue;

        live.insert(p.first, Row{ p.first, exe, p.second, is64BitProcess(p.first) });
    }

    // Stack order: keep everything already on screen where it is, drop what has exited, append what
    // is new at the bottom.
    QList<Row> rows;
    for (int i = m_order.size() - 1; i >= 0; --i)
        if (!live.contains(m_order[i])) m_order.removeAt(i);
    for (quint32 pid : std::as_const(m_order))
        rows.append(live.value(pid));
    for (const auto &pid : live.keys())
        if (!m_order.contains(pid)) { m_order.append(pid); rows.append(live.value(pid)); }

    // Forget processes that have exited, so the cache does not grow for the life of the session and
    // a recycled pid is judged on its own modules rather than the previous tenant's.
    for (auto it = m_compatible.begin(); it != m_compatible.end(); ) {
        if (alive(it.key())) ++it;
        else it = m_compatible.erase(it);
    }
    return rows;
}

void HooksPanel::rebuild(const QList<Row> &rows)
{
    m_list->clear();
    QFileIconProvider icons;
    for (const Row &r : rows) {
        const QFileInfo fi(r.exe);

        auto *row = new QWidget(m_list);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(4, 2, 4, 2);

        auto *play = new QToolButton(row);
        play->setObjectName(QStringLiteral("play"));    // how updateButtons() finds it again
        play->setAutoRaise(true);
        play->setIconSize(QSize(20, 20));
        play->setProperty("pid", r.pid);
        play->setProperty("is64", r.is64);
        connect(play, &QToolButton::clicked, this, [this, pid = r.pid] { toggle(pid); });
        h->addWidget(play);

        auto *icon = new QLabel(row);
        icon->setPixmap(icons.icon(fi).pixmap(24, 24));
        h->addWidget(icon);

        auto *text = new QLabel(QStringLiteral("%1  -  %2").arg(fi.fileName(), r.title), row);
        text->setToolTip(QStringLiteral("%1\nPID %2 (%3-bit)").arg(r.exe).arg(r.pid).arg(r.is64 ? 64 : 32));
        h->addWidget(text, 1);

        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, r.pid);
        item->setSizeHint(row->sizeHint());
        m_list->setItemWidget(item, row);
    }
}

void HooksPanel::updateButtons()
{
    hook::Ctrl *c = ctrl();
    const quint32 shading = c && c->enabled ? c->hookedPid : 0;

    for (int i = 0; i < m_list->count(); ++i) {
        auto *w = m_list->itemWidget(m_list->item(i));
        auto *play = w ? w->findChild<QToolButton *>(QStringLiteral("play")) : nullptr;
        if (!play) continue;
        const bool on = play->property("pid").toUInt() == shading && shading != 0;
        play->setIcon(on ? style()->standardIcon(QStyle::SP_MediaStop) : QIcon(QStringLiteral(":/icons/play.ico")));
        play->setToolTip(on ? tr("Stop") : tr("Play through ShaderPlayer"));
    }

    if (!shading) {
        m_status->setText(tr("Nothing hooked."));
        return;
    }
    const QString name = QFileInfo(exePathForPid(shading)).fileName();
    const QString via = QString::fromLatin1(hook::apiName(c->api));
    if (m_worker && m_worker->shading())
        m_status->setText(tr("%1 via %2, %3x%4 at %5 fps. Ctrl+Alt+S in game toggles it.")
                              .arg(name, via).arg(c->capWidth).arg(c->capHeight)
                              .arg(m_worker->fps(), 0, 'f', 0));
    else
        m_status->setText(tr("%1 is hooked, waiting for a frame.").arg(name));
}

void HooksPanel::refresh()
{
    const QList<Row> rows = enumerate();

    // Only touch the widgets when the set of processes actually changed. Rebuilding every tick
    // destroys the button under the pointer, which is what made the list feel like it was jumping.
    bool changed = rows.size() != m_list->count();
    for (int i = 0; !changed && i < rows.size(); ++i)
        changed = m_list->item(i)->data(Qt::UserRole).toUInt() != rows[i].pid;
    if (changed)
        rebuild(rows);

    updateButtons();
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
        QMessageBox::warning(this, tr("Play through ShaderPlayer"),
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
        QMessageBox::warning(this, tr("Play through ShaderPlayer"),
                             tr("sphelper32.exe is missing, so 32-bit apps cannot be hooked."));
        return false;
    }
    QProcess p;
    p.start(helper, { QString::number(pid), native });
    return p.waitForFinished(10000) && p.exitCode() == 0;
}

void HooksPanel::toggle(quint32 pid)
{
    hook::Ctrl *c = ctrl();
    if (!c) {
        QMessageBox::warning(this, tr("Hooks"), tr("Could not open the shared control block."));
        return;
    }

    if (c->enabled && c->hookedPid == pid) {   // already playing this one: stop
        c->enabled = 0;
        updateButtons();
        return;
    }

    bool is64 = true;
    for (int i = 0; i < m_list->count(); ++i) {
        auto *w = m_list->itemWidget(m_list->item(i));
        auto *play = w ? w->findChild<QToolButton *>(QStringLiteral("play")) : nullptr;
        if (play && play->property("pid").toUInt() == pid) { is64 = play->property("is64").toBool(); break; }
    }

    if (!hook::alreadyInjected(pid) && !inject(pid, is64)) {
        QMessageBox::warning(this, tr("Play through ShaderPlayer"),
            tr("Could not hook %1.\n\nSome apps run at a higher integrity level than ShaderPlayer, "
               "which blocks this. Anti-cheat also blocks it, by design.")
                .arg(QFileInfo(exePathForPid(pid)).fileName()));
        return;
    }

    c->hookedPid = pid;   // hand the pipeline to the app the user just picked
    c->enabled = 1;
    updateButtons();
}
