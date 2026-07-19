#include "mainwindow.h"

#include "appdata.h"
#include "playlistpanel.h"
#include "shaderpanel.h"
#include "shaderparamspanel.h"
#include "videowidget.h"
#ifdef SHADERPLAYER_HOOKS
#include "hookspanel.h"
#include "hookworker.h"
#endif

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHash>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>

namespace {

// Installed builds keep the shaders beside the exe; dev builds use the checkout.
QString shaderRoot()
{
    const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/shaders");
    if (QFileInfo::exists(beside))
        return beside;
    return QStringLiteral(SHADERPLAYER_SHADER_DIR);
}

QString formatTime(double seconds)
{
    if (seconds < 0 || !qIsFinite(seconds))
        seconds = 0;
    const qint64 total = qint64(seconds);
    const qint64 h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    return h > 0 ? QString::asprintf("%lld:%02lld:%02lld", h, m, s)
                 : QString::asprintf("%lld:%02lld", m, s);
}

const char *kVideoFilter =
    "Video files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.wmv *.flv *.ts *.mpg *.mpeg);;All files (*)";

const QColor kLed(0x4a, 0xff, 0x6a);
// Active state. Every control that has an on/off or a current-mode reading lights cyan when it is
// the live one: the play button while playing, the current scale, the current line count, the
// current refresh rate. Green is idle, cyan is "this is what you are getting".
const QColor kLedActive(0x4a, 0xe8, 0xff);

// Left-click jumps the handle to the click point, then hands off to the normal drag machinery.
// The SH_Slider_AbsoluteSetButtons style hint would do this in one line, but QStyleSheetStyle
// (active because the transport bar carries a stylesheet) swallows it, so do it by hand.
class ClickSlider : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && maximum() > minimum()) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                         QStyle::SC_SliderGroove, this);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                         QStyle::SC_SliderHandle, this);
            const int pos = int(e->position().x()) - groove.x() - handle.width() / 2;
            const int span = groove.width() - handle.width();
            setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), pos, span));
            // The handle now sits under the cursor, so the base class starts a drag from here
            // and still emits sliderPressed/sliderReleased for the seek logic.
        }
        QSlider::mousePressEvent(e);
    }
};

// FNV-nothing: just tint a glyph LED green. SourceIn keeps the glyph's alpha and paints every
// lit pixel green, which is how a monochrome icon becomes an LED without per-icon artwork.
QIcon tintLed(QPixmap pm, const QColor &c = kLed)
{
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), c);
    p.end();
    return QIcon(pm);
}

// A bundled font, registered once per resource. An empty family (load failure) falls back to
// the default, which still shows the text -- just not in the intended typeface.
QFont bundledFont(const QString &resource, int px)
{
    static QHash<QString, QString> families;
    auto it = families.constFind(resource);
    if (it == families.constEnd()) {
        const int id = QFontDatabase::addApplicationFont(resource);
        it = families.insert(resource,
            id >= 0 ? QFontDatabase::applicationFontFamilies(id).value(0) : QString());
    }
    QFont f(*it);
    f.setPixelSize(px);
    return f;
}

QFont segmentFont(int px) { return bundledFont(QStringLiteral(":/font/lcddot_tr.ttf"), px); }

// Recolour a stock icon to LED green. A stylesheet cannot reach inside a QIcon, so tint the
// pixmap: SourceIn keeps the glyph's alpha and replaces every colour under it.
QIcon ledIcon(QStyle *style, QStyle::StandardPixmap sp, int size = 22, const QColor &c = kLed)
{
    return tintLed(style->standardIcon(sp).pixmap(size, size), c);
}

// Same treatment for a bundled SVG. QIcon renders it through the svg icon plugin; currentColor
// resolves to black, which SourceIn then replaces with LED green.
QIcon ledSvg(const QString &path, int size = 22)
{
    return tintLed(QIcon(path).pixmap(size, size));
}

// Swap a control's halo between idle green and active cyan. The glow is a QGraphicsDropShadowEffect
// rather than anything a stylesheet can reach, so :checked cannot do this part on its own.
void setGlowActive(QWidget *w, bool active)
{
    if (auto *g = qobject_cast<QGraphicsDropShadowEffect *>(w->graphicsEffect()))
        g->setColor(active ? kLedActive : kLed);
}

// Give a widget its halo. Offset 0 turns a drop shadow into a glow -- the cheapest honest one going.
QGraphicsDropShadowEffect *addGlow(QWidget *w, int blur)
{
    auto *g = new QGraphicsDropShadowEffect(w);
    g->setColor(kLed);
    g->setOffset(0, 0);
    g->setBlurRadius(blur);
    w->setGraphicsEffect(g);
    return g;
}

// A checkable control whose halo tracks its own checked state.
void glowOnCheck(QAbstractButton *b)
{
    QObject::connect(b, &QAbstractButton::toggled, b, [b](bool on) { setGlowActive(b, on); });
}

// Recessed well: the gradient runs dark-at-top to lighter-at-bottom and the bevel is dark on
// top, light on the bottom -- the reverse of a raised button, which is what reads as inset.
const char *kTransportStyle = R"(
QToolButton {
    border: 1px solid #0d0d0d;
    border-top-color: #050505;
    border-bottom-color: #3a3a3a;
    border-radius: 5px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #121212, stop:1 #262626);
    padding: 5px;
    color: #4aff6a;
}
QLabel { color: #4aff6a; }
QToolButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1a1a1a, stop:1 #303030);
}
QToolButton:pressed {
    background: #0c0c0c;
    border-top-color: #000000;
    border-bottom-color: #202020;
}
/* The active mode: current resolution, refresh rate or scale. Only the text changes colour, and
   the halo goes cyan with it, so a lit button reads as a lit LED rather than a filled-in block.
   The well it sits in stays exactly as it is. */
QToolButton:checked {
    color: #4ae8ff;
}

/* Same recessed well as the buttons, so the sliders read as cut into the same panel.
   sub-page is the travelled part, which makes the groove light up as it fills. */
QSlider::groove:horizontal {
    height: 6px;
    border: 1px solid #0d0d0d;
    border-top-color: #050505;
    border-bottom-color: #3a3a3a;
    border-radius: 3px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #0e0e0e, stop:1 #222222);
}
QSlider::sub-page:horizontal {
    border: 1px solid #0d0d0d;
    border-radius: 3px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1f8f35, stop:1 #4aff6a);
}
QSlider::handle:horizontal {
    width: 10px;
    margin: -4px 0;
    border: 1px solid #072d10;
    border-radius: 5px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #b6ffc6, stop:1 #35d451);
}
QSlider::handle:horizontal:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #d8ffe1, stop:1 #4aff6a);
}
)";

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ShaderPlayer-RA"));
    setAcceptDrops(true);
    resize(1280, 800);

    m_video = new VideoWidget(this);
    setCentralWidget(m_video);

    m_shaders = new ShaderPanel(shaderRoot(), this);
    m_playlist = new PlaylistPanel(m_video, this);
    m_params = new ShaderParamsPanel(m_video, shaderRoot(), this);

    // Picking a shader and tuning it are one job, so the settings sit under the list in the same
    // tab rather than in a separate tool window. The splitter lets either half take the space.
    auto *shaderTab = new QSplitter(Qt::Vertical, this);
    shaderTab->addWidget(m_shaders);
    shaderTab->addWidget(m_params);
    shaderTab->setStretchFactor(0, 3);
    shaderTab->setStretchFactor(1, 2);
    shaderTab->setChildrenCollapsible(false);

    // Everything lives in the Library dock: one place, three tabs.
    auto *tabs = new QTabWidget(this);
    tabs->addTab(shaderTab, tr("Shaders"));
    tabs->addTab(m_playlist, tr("Playlist"));
#ifdef SHADERPLAYER_HOOKS
    m_hooks = new HooksPanel(this);
    tabs->addTab(m_hooks, tr("Hooks"));

    // Feed the hook worker whatever the video path is currently showing, so the shader in a hooked
    // game follows the app. Polling covers every route a preset or slider can change without
    // wiring a signal into each of them, and the worker only acts on differences.
    m_hookWorker = new HookWorker(this);
    auto *hookSync = new QTimer(this);
    connect(hookSync, &QTimer::timeout, this, [this] {
        m_hookWorker->setState(m_video->preset(), m_video->shaderParams(), m_video->sourceLines());
    });
    hookSync->start(200);

    // The game's picture arrives here and goes in as the video surface's source, so it comes out
    // the far end of the same shader chain, at the same scale, in the same window.
    connect(m_hookWorker, &HookWorker::frameReady, this, [this](const QImage &frame) {
        const bool wasPlaying = m_video->hookActive();
        m_video->setHookFrame(frame);
        // A game has taken the screen; nothing is watching the video behind it, and leaving it
        // playing means its audio carries on over the game's.
        if (!wasPlaying)
            m_video->setPaused(true);
    });
    m_hooks->attachWorker(m_hookWorker);
    m_hookWorker->start();
#endif

    m_libraryDock = new QDockWidget(tr("Library"), this);
    m_libraryDock->setObjectName(QStringLiteral("shaderDock"));
    m_libraryDock->setWidget(tabs);
    addDockWidget(Qt::RightDockWidgetArea, m_libraryDock);

    buildMenus();
    buildTransport();
    buildResolutionBar();

    connect(m_shaders, &ShaderPanel::presetChosen, m_video, &VideoWidget::setPreset);
    connect(m_video, &VideoWidget::shaderFailed, this, [this](const QString &msg) {
        QMessageBox::warning(this, tr("Shader failed"),
                             tr("%1\n\nFalling back to no shader.").arg(msg));
        m_shaders->setCurrentPreset(QString());
    });
    connect(m_video, &VideoWidget::doubleClicked, this, &MainWindow::toggleFullscreen);
    // First video to report a size snaps to the default scale; later files leave the window
    // where the user put it. Explicit scale-button clicks always apply, startup or not.
    connect(m_video, &VideoWidget::videoSizeChanged, this, [this](const QSize &s) {
        // A new video can change the aspect, so what "480p" resolves to changes with it.
        updateSourceLabel();
        if (!m_scaledOnce && !s.isEmpty()) {
            m_scaledOnce = true;
            scaleTo(m_scaleFactor);
        }
    });
    connect(m_video, &VideoWidget::tracksChanged, this, &MainWindow::rebuildSubtitleMenu);
    // The title bar stays "ShaderPlayer-RA". It used to follow whatever mpv was playing, which
    // changes on its own as a playlist advances and reads as a distraction rather than information;
    // the playlist panel is where you look to find out what is playing.

    restoreSettings();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open..."), QKeySequence::Open, this, [this] { promptOpen(false); });
    file->addAction(tr("&Add to Playlist..."), this, [this] { promptOpen(true); });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *play = menuBar()->addMenu(tr("&Playback"));
    play->addAction(tr("&Play/Pause"), Qt::Key_Space, m_video, &VideoWidget::togglePause);
    play->addSeparator();
    play->addAction(tr("&Previous"), QKeySequence(Qt::CTRL | Qt::Key_Left), m_video, &VideoWidget::stepBackward);
    play->addAction(tr("&Next"), QKeySequence(Qt::CTRL | Qt::Key_Right), m_video, &VideoWidget::stepForward);
    play->addSeparator();
    play->addAction(tr("Back 10s"), Qt::Key_Left, this, [this] { m_video->seekRelative(-10); });
    play->addAction(tr("Forward 10s"), Qt::Key_Right, this, [this] { m_video->seekRelative(10); });

    QMenu *view = menuBar()->addMenu(tr("&View"));
    // toggleViewAction is already checkable and tracks the dock: closing it via the dock's own
    // X unticks this, and reticking brings it back. No manual sync needed.
    QAction *showLibrary = m_libraryDock->toggleViewAction();
    showLibrary->setText(tr("Show &Library"));
    view->addAction(showLibrary);
    view->addSeparator();

    m_fullscreenAction = view->addAction(tr("&Fullscreen"), Qt::Key_F11, this,
                                         &MainWindow::toggleFullscreen);
    m_fullscreenAction->setCheckable(true);
    // The menu bar is hidden in fullscreen, so its actions can't be relied on for the way
    // back out. These live on the window instead.
    addAction(m_fullscreenAction);

    auto *escape = new QAction(this);
    escape->setShortcut(Qt::Key_Escape);
    connect(escape, &QAction::triggered, this, [this] { setFullscreen(false); });
    addAction(escape);

    m_subtitleMenu = menuBar()->addMenu(tr("&Subtitles"));
    rebuildSubtitleMenu();

    // Source Resolution used to be a submenu here. It is the control the whole app exists for, so
    // it now gets its own always-visible strip of buttons instead (see buildResolutionBar).

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About"), this, [this] {
        QMessageBox::about(this, tr("About ShaderPlayer-RA"),
            tr("<h3>ShaderPlayer-RA</h3>"
               "<p>Version 0.0.1.0</p>"
               "<p>Video playback through RetroArch shaders.</p>"
               "<p>Shaders by the <a href='https://github.com/libretro/slang-shaders'>libretro</a> "
               "project, rendered with "
               "<a href='https://github.com/SnowflakePowered/librashader'>librashader</a>. "
               "Playback by <a href='https://mpv.io'>mpv</a>.</p>"));
    });
}

// The Source Resolution strip: what resolution the shader chain sees, which is the whole point of
// the app, so it lives on screen rather than three levels into a menu.
//
// Line counts, not fixed sizes. A CRT mask cares about scanlines, and the width has to follow the
// source's aspect or the picture gets squeezed: 480p is 854x480 on a 16:9 video and 640x480 on a
// 4:3 one. The readout on the right shows what the current pick actually resolves to.
void MainWindow::buildResolutionBar()
{
    auto *bar = new QWidget(this);
    bar->setStyleSheet(QLatin1String(kTransportStyle));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(8);

    struct Choice { const char *label; int lines; const char *tip; };
    static const Choice choices[] = {
        { QT_TR_NOOP("Native"), 0,    QT_TR_NOOP("Shade at the source's own resolution") },
        { "224p",               224,  QT_TR_NOOP("Shade at 224 lines (NES / SNES)") },
        { "240p",               240,  QT_TR_NOOP("Shade at 240 lines (Genesis / arcade)") },
        { "360p",               360,  QT_TR_NOOP("Shade at 360 lines") },
        { "480p",               480,  QT_TR_NOOP("Shade at 480 lines (NTSC)") },
        { "576p",               576,  QT_TR_NOOP("Shade at 576 lines (PAL)") },
        { "720p",               720,  QT_TR_NOOP("Shade at 720 lines") },
        { "1080p",              1080, QT_TR_NOOP("Shade at 1080 lines") },
    };
    for (const Choice &c : choices) {
        auto *b = new QToolButton(bar);
        b->setText(tr(c.label));
        b->setFont(segmentFont(24));
        b->setToolTip(tr(c.tip));
        b->setCheckable(true);
        b->setProperty("lines", c.lines);
        addGlow(b, 14);
        glowOnCheck(b);
        connect(b, &QToolButton::clicked, this, [this, lines = c.lines] { applySourceLines(lines); });
        layout->addWidget(b);
        m_sourceButtons.append(b);
    }

    m_sourceLabel = new QLabel(bar);
    m_sourceLabel->setFont(segmentFont(24));
    addGlow(m_sourceLabel, 10);
    layout->addWidget(m_sourceLabel);

    // Refresh rate: how fast the chain is redrawn, which matters because the shader now runs all
    // the time rather than only when mpv produces a frame.
    auto *sep = new QLabel(QStringLiteral("|"), bar);
    sep->setFont(segmentFont(24));
    addGlow(sep, 8);
    layout->addWidget(sep);

    for (int hz : { 24, 50, 60 }) {
        auto *b = new QToolButton(bar);
        b->setText(QStringLiteral("%1Hz").arg(hz));
        b->setFont(segmentFont(24));
        b->setToolTip(tr("Run the shader at %1 Hz").arg(hz));
        b->setCheckable(true);
        b->setProperty("hz", hz);
        addGlow(b, 14);
        glowOnCheck(b);
        connect(b, &QToolButton::clicked, this, [this, hz] { applyRefreshRate(hz); });
        layout->addWidget(b);
        m_refreshButtons.append(b);
    }

    layout->addStretch(1);   // everything above stays left-justified

    auto *dock = new QDockWidget(this);
    dock->setObjectName(QStringLiteral("resolutionDock"));
    dock->setTitleBarWidget(new QWidget(dock));   // no title bar; it is a control strip
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(bar);
    addDockWidget(Qt::TopDockWidgetArea, dock);
}

void MainWindow::buildTransport()
{
    auto *bar = new QWidget(this);
    bar->setStyleSheet(QLatin1String(kTransportStyle));
    auto *layout = new QHBoxLayout(bar);
    // The glow is drawn outside the button's own rect, so leave it room to spill.
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(8);

    auto button = [&](QStyle::StandardPixmap icon, const QString &tip, auto slot) {
        auto *b = new QToolButton(bar);
        b->setIcon(ledIcon(style(), icon));
        b->setToolTip(tip);
        b->setAutoRaise(false); // the stylesheet supplies the look
        addGlow(b, 14);
        connect(b, &QToolButton::clicked, m_video, slot);
        layout->addWidget(b);
        return b;
    };

    button(QStyle::SP_MediaSkipBackward, tr("Previous"), &VideoWidget::stepBackward);
    m_playButton = button(QStyle::SP_MediaPlay, tr("Play/Pause"), &VideoWidget::togglePause);
    button(QStyle::SP_MediaSkipForward, tr("Next"), &VideoWidget::stepForward);

    m_seek = new ClickSlider(Qt::Horizontal, bar);
    m_seek->setRange(0, 0);
    layout->addWidget(m_seek, 1);

    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), bar);
    m_timeLabel->setFont(segmentFont(24));
    addGlow(m_timeLabel, 10);
    layout->addWidget(m_timeLabel);

    auto *volIcon = new QLabel(bar);
    volIcon->setPixmap(ledIcon(style(), QStyle::SP_MediaVolume, 16).pixmap(16, 16));
    // A label has no background of its own, so here the halo hugs the glyph rather than a
    // rounded rect -- the speaker actually bleeds light.
    addGlow(volIcon, 10);
    layout->addWidget(volIcon);

    m_volume = new QSlider(Qt::Horizontal, bar);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(90);
    layout->addWidget(m_volume);

    // Scale controls, right of the volume: snap the window so the video shows at 1x/2x/3x its
    // pixel size, plus a fullscreen toggle. Same LED font and glow as the time readout.
    for (int factor : { 1, 2, 3 }) {
        auto *b = new QToolButton(bar);
        b->setText(QStringLiteral("%1x").arg(factor));
        b->setFont(segmentFont(24));
        b->setToolTip(tr("Scale video to %1x").arg(factor));
        b->setCheckable(true);
        b->setProperty("factor", factor);
        addGlow(b, 14);
        glowOnCheck(b);
        b->setChecked(factor == m_scaleFactor);   // lit before the first scaleTo ever runs
        connect(b, &QToolButton::clicked, this, [this, factor] { scaleTo(factor); });
        layout->addWidget(b);
        m_scaleButtons.append(b);
    }

    auto *fs = new QToolButton(bar);
    fs->setIcon(ledSvg(QStringLiteral(":/icons/arrows-fullscreen.svg"), 18));
    fs->setToolTip(tr("Fullscreen"));
    addGlow(fs, 14);
    connect(fs, &QToolButton::clicked, this, &MainWindow::toggleFullscreen);
    layout->addWidget(fs);

    auto *dock = new QDockWidget(this);
    dock->setObjectName(QStringLiteral("transportDock"));
    dock->setTitleBarWidget(new QWidget(dock)); // no title bar; it is a control strip
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setWidget(bar);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    // Don't fight the user's drag by rewriting the slider under their thumb.
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_seeking = false;
        m_video->seekTo(m_seek->value() / 1000.0);
    });
    connect(m_volume, &QSlider::valueChanged, m_video, &VideoWidget::setVolume);
    // Persist as it changes, not just on close: a crash or a kill would otherwise lose it.
    connect(m_volume, &QSlider::valueChanged, this, [](int v) {
        settings()->setValue(QStringLiteral("volume"), v);
    });

    connect(m_video, &VideoWidget::durationChanged, this, [this](double d) {
        m_duration = d;
        m_seek->setRange(0, int(d * 1000));
    });
    connect(m_video, &VideoWidget::positionChanged, this, [this](double p) {
        if (!m_seeking)
            m_seek->setValue(int(p * 1000));
        m_timeLabel->setText(formatTime(p) + QStringLiteral(" / ") + formatTime(m_duration));
    });
    // Lit cyan while playing, idle green while paused, glyph and halo together.
    connect(m_video, &VideoWidget::pausedChanged, this, [this](bool paused) {
        m_playButton->setIcon(ledIcon(style(), paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause,
                                      22, paused ? kLed : kLedActive));
        setGlowActive(m_playButton, !paused);
    });
}

void MainWindow::scaleTo(int factor)
{
    m_scaleFactor = factor;
    for (QToolButton *b : m_scaleButtons)
        b->setChecked(b->property("factor").toInt() == factor);
    // Scale against the shader source, not the raw video: an integer multiple of the source is
    // what keeps crt-royale's scanlines and mask clean, and it honours the Source Resolution
    // override so 2x of a pinned 320x240 is a true 640x480.
    const QSize src = m_video->sourceSize();
    if (src.isEmpty())
        return; // nothing loaded yet; the pending factor applies when a video arrives

    if (isFullScreen())
        setFullscreen(false);
    if (isMaximized())
        showNormal();

    // Resize the window by the difference between the video widget's current size and the size
    // that shows the picture at factor x. The chrome (menu, docks) rides along unchanged.
    resize(size() - m_video->size() + src * factor);
}

void MainWindow::toggleFullscreen()
{
    setFullscreen(!isFullScreen());
}

void MainWindow::setFullscreen(bool on)
{
    if (on == isFullScreen())
        return;

    if (on) {
        // saveState/restoreState round-trips which docks the user actually had open, so
        // leaving fullscreen doesn't resurrect ones they'd closed.
        m_preFullscreenState = saveState();
        for (QDockWidget *d : findChildren<QDockWidget *>())
            d->hide();
        menuBar()->hide();
        showFullScreen();
    } else {
        showNormal();
        menuBar()->show();
        if (!m_preFullscreenState.isEmpty())
            restoreState(m_preFullscreenState);
    }
    m_fullscreenAction->setChecked(on);
}

void MainWindow::rebuildSubtitleMenu()
{
    m_subtitleMenu->clear();
    auto *group = new QActionGroup(m_subtitleMenu);
    group->setExclusive(true);

    const int64_t current = m_video->currentSubtitleTrack();

    QAction *none = m_subtitleMenu->addAction(tr("None"));
    none->setCheckable(true);
    none->setChecked(current < 0);
    group->addAction(none);
    connect(none, &QAction::triggered, this, [this] { m_video->setSubtitleTrack(-1); });

    const QList<SubtitleTrack> tracks = m_video->subtitleTracks();
    if (!tracks.isEmpty())
        m_subtitleMenu->addSeparator();

    for (const SubtitleTrack &t : tracks) {
        QAction *a = m_subtitleMenu->addAction(t.label);
        a->setCheckable(true);
        a->setChecked(t.id == current);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, id = t.id] { m_video->setSubtitleTrack(id); });
    }
}

void MainWindow::applySourceLines(int lines, bool persist)
{
    m_video->setSourceLines(lines);

    for (QToolButton *b : m_sourceButtons)
        b->setChecked(b->property("lines").toInt() == lines);
    updateSourceLabel();

    if (persist) {
        settings()->setValue(QStringLiteral("shaders/sourceLines"), lines);

        // Pick a scale that gives a usable window. Anything up to 480 lines is a small picture and
        // wants doubling to be watchable; 576p and above already fills enough of the screen that 2x
        // would overflow most displays. Keyed on the effective height rather than the button, so
        // Native follows the same rule from whatever the video actually is.
        const int effective = m_video->sourceSize().height();
        if (effective > 0)
            m_scaleFactor = effective <= 480 ? 2 : 1;

        // The window is sized in multiples of the source, so a deliberate source change should
        // re-snap. Guarded on persist, so the startup restore (which lets the one-time
        // videoSizeChanged snap handle it) doesn't fight geometry.
        scaleTo(m_scaleFactor);
    }
}

void MainWindow::applyRefreshRate(int hz, bool persist)
{
    m_video->setRefreshRate(hz);
    for (QToolButton *b : m_refreshButtons)
        b->setChecked(b->property("hz").toInt() == hz);
    if (persist)
        settings()->setValue(QStringLiteral("video/refreshRate"), hz);
}

// Spell out what the chosen line count actually works out to for the video that is loaded, since
// that is the part a fixed "480p" label cannot tell you: the width is whatever the aspect gives.
void MainWindow::updateSourceLabel()
{
    if (!m_sourceLabel)
        return;
    const QSize src = m_video->sourceSize();
    m_sourceLabel->setText(src.isEmpty() ? QString()
                                         : QStringLiteral("%1 x %2").arg(src.width()).arg(src.height()));
}

void MainWindow::promptOpen(bool append)
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, append ? tr("Add to Playlist") : tr("Open"),
        settings()->value(QStringLiteral("lastDir")).toString(), tr(kVideoFilter));
    if (!files.isEmpty())
        openFiles(files, append);
}

void MainWindow::openFiles(const QStringList &files, bool append)
{
    if (files.isEmpty())
        return;
    // Only real paths have a directory; taking absolutePath() of "av://..." yields nonsense.
    const QString first = files.first();
    if (!first.contains(QStringLiteral("://")))
        settings()->setValue(QStringLiteral("lastDir"), QFileInfo(first).absolutePath());
    m_video->open(files, append);
}

void MainWindow::restorePlaylist()
{
    m_playlist->restore();
}

void MainWindow::restoreSettings()
{
    QSettings *s = settings();
    restoreGeometry(s->value(QStringLiteral("window/geometry")).toByteArray());
    const bool hadLayout = restoreState(s->value(QStringLiteral("window/state")).toByteArray());

    // First run only: widen the right column 20px past its natural default. Queued so it runs
    // once the docks have been laid out at their content width; a saved layout is left alone.
    if (!hadLayout) {
        QMetaObject::invokeMethod(this, [this] {
            resizeDocks({ m_libraryDock }, { m_libraryDock->width() + 20 }, Qt::Horizontal);
        }, Qt::QueuedConnection);
    }

    m_volume->setValue(s->value(QStringLiteral("volume"), 100).toInt());

    // Carry over the old width-and-height setting by taking its height, so an existing install does
    // not silently drop back to Native on first run of this version.
    int lines = s->value(QStringLiteral("shaders/sourceLines"), -1).toInt();
    if (lines < 0)
        lines = s->value(QStringLiteral("shaders/sourceOverride")).toSize().height();
    applySourceLines(std::max(0, lines), false);
    applyRefreshRate(s->value(QStringLiteral("video/refreshRate"), 60).toInt(), false);

    QString preset = s->value(QStringLiteral("shaders/lastPreset")).toString();
    // First run defaults to crt-royale, matching the favourite seeded in ShaderPanel. Guarded
    // on the key so deliberately choosing "no shader" is remembered as empty, not re-seeded.
    if (!s->contains(QStringLiteral("shaders/lastPreset"))) {
        const QString royale = shaderRoot() + QStringLiteral("/crt/crt-royale.slangp");
        if (QFileInfo::exists(royale))
            preset = royale;
    }
    if (!preset.isEmpty() && QFileInfo::exists(preset)) {
        m_video->setPreset(preset);
        m_shaders->setCurrentPreset(preset);
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    // Quitting fullscreen would otherwise persist a hidden menu bar and no docks.
    setFullscreen(false);

    QSettings *s = settings();
    s->setValue(QStringLiteral("window/geometry"), saveGeometry());
    s->setValue(QStringLiteral("window/state"), saveState());
    s->setValue(QStringLiteral("shaders/lastPreset"), m_video->preset());
    s->sync();
    m_playlist->save();
#ifdef SHADERPLAYER_HOOKS
    m_hookWorker->stop();   // drops the heartbeat, so any hooked game returns to stock immediately
#endif
    QMainWindow::closeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    QStringList files;
    for (const QUrl &url : e->mimeData()->urls())
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    openFiles(files);
}
