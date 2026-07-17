#include "mainwindow.h"

#include "appdata.h"
#include "playlistpanel.h"
#include "shaderpanel.h"
#include "shaderparamspanel.h"
#include "videowidget.h"

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
QIcon tintLed(QPixmap pm)
{
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), kLed);
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
QIcon ledIcon(QStyle *style, QStyle::StandardPixmap sp, int size = 22)
{
    return tintLed(style->standardIcon(sp).pixmap(size, size));
}

// Same treatment for a bundled SVG. QIcon renders it through the svg icon plugin; currentColor
// resolves to black, which SourceIn then replaces with LED green.
QIcon ledSvg(const QString &path, int size = 22)
{
    return tintLed(QIcon(path).pixmap(size, size));
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

    auto *tabs = new QTabWidget(this);
    tabs->addTab(m_shaders, tr("Shaders"));
    tabs->addTab(m_playlist, tr("Playlist"));

    m_libraryDock = new QDockWidget(tr("Library"), this);
    m_libraryDock->setObjectName(QStringLiteral("shaderDock"));
    m_libraryDock->setWidget(tabs);
    addDockWidget(Qt::RightDockWidgetArea, m_libraryDock);

    m_params = new ShaderParamsPanel(m_video, shaderRoot(), this);
    m_paramsDock = new QDockWidget(tr("Shader Settings"), this);
    m_paramsDock->setObjectName(QStringLiteral("paramsDock"));
    m_paramsDock->setWidget(m_params);
    addDockWidget(Qt::RightDockWidgetArea, m_paramsDock);
    splitDockWidget(m_libraryDock, m_paramsDock, Qt::Vertical); // stacked under the library

    buildMenus();
    buildTransport();

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
        if (!m_scaledOnce && !s.isEmpty()) {
            m_scaledOnce = true;
            scaleTo(m_scaleFactor);
        }
    });
    connect(m_video, &VideoWidget::tracksChanged, this, &MainWindow::rebuildSubtitleMenu);
    connect(m_video, &VideoWidget::titleChanged, this, [this](const QString &t) {
        setWindowTitle(t.isEmpty() ? QStringLiteral("ShaderPlayer-RA")
                                   : QStringLiteral("%1 - ShaderPlayer-RA").arg(t));
    });

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
    QAction *showParams = m_paramsDock->toggleViewAction();
    showParams->setText(tr("Show Shader &Settings"));
    view->addAction(showParams);
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

    QMenu *shader = menuBar()->addMenu(tr("S&hader"));
    QMenu *res = shader->addMenu(tr("&Source Resolution"));
    m_sourceGroup = new QActionGroup(this);
    m_sourceGroup->setExclusive(true);

    // The whole point of the app: pin what resolution the shader chain sees, so a CRT mask
    // looks identical no matter what the source happens to be.
    struct Preset { const char *label; QSize size; };
    static const Preset presets[] = {
        { QT_TR_NOOP("Native (source size)"), QSize() },
        { QT_TR_NOOP("256 x 224 (NES / SNES)"), QSize(256, 224) },
        { QT_TR_NOOP("320 x 240 (240p)"), QSize(320, 240) },
        { QT_TR_NOOP("640 x 480 (480p)"), QSize(640, 480) },
        { QT_TR_NOOP("1280 x 720 (720p)"), QSize(1280, 720) },
        { QT_TR_NOOP("1920 x 1080 (1080p)"), QSize(1920, 1080) },
    };
    for (const Preset &p : presets) {
        QAction *a = res->addAction(tr(p.label));
        a->setCheckable(true);
        a->setData(p.size);
        m_sourceGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, p] { applySourceOverride(p.size); });
    }
    res->addSeparator();
    m_customSourceAction = res->addAction(tr("&Custom..."));
    m_customSourceAction->setCheckable(true);
    m_sourceGroup->addAction(m_customSourceAction);
    connect(m_customSourceAction, &QAction::triggered, this, &MainWindow::promptCustomSourceSize);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About"), this, [this] {
        QMessageBox::about(this, tr("About ShaderPlayer-RA"),
            tr("<h3>ShaderPlayer-RA</h3>"
               "<p>Video playback through RetroArch shaders.</p>"
               "<p>Shaders by the <a href='https://github.com/libretro/slang-shaders'>libretro</a> "
               "project, rendered with "
               "<a href='https://github.com/SnowflakePowered/librashader'>librashader</a>. "
               "Playback by <a href='https://mpv.io'>mpv</a>.</p>"));
    });
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

        // Offset 0 turns a drop shadow into a halo -- the cheapest honest glow going.
        auto *glow = new QGraphicsDropShadowEffect(b);
        glow->setColor(kLed);
        glow->setOffset(0, 0);
        glow->setBlurRadius(14);
        b->setGraphicsEffect(glow);

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

    auto glowGreen = [](QWidget *w, int blur) {
        auto *g = new QGraphicsDropShadowEffect(w);
        g->setColor(kLed);
        g->setOffset(0, 0);
        g->setBlurRadius(blur);
        w->setGraphicsEffect(g);
    };

    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), bar);
    m_timeLabel->setFont(segmentFont(24));
    glowGreen(m_timeLabel, 10);
    layout->addWidget(m_timeLabel);

    auto *volIcon = new QLabel(bar);
    volIcon->setPixmap(ledIcon(style(), QStyle::SP_MediaVolume, 16).pixmap(16, 16));
    // A label has no background of its own, so here the halo hugs the glyph rather than a
    // rounded rect -- the speaker actually bleeds light.
    glowGreen(volIcon, 10);
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
        glowGreen(b, 14);
        connect(b, &QToolButton::clicked, this, [this, factor] { scaleTo(factor); });
        layout->addWidget(b);
    }

    auto *fs = new QToolButton(bar);
    fs->setIcon(ledSvg(QStringLiteral(":/icons/arrows-fullscreen.svg"), 18));
    fs->setToolTip(tr("Fullscreen"));
    glowGreen(fs, 14);
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
    connect(m_video, &VideoWidget::pausedChanged, this, [this](bool paused) {
        m_playButton->setIcon(ledIcon(style(), paused ? QStyle::SP_MediaPlay
                                                      : QStyle::SP_MediaPause));
    });
}

void MainWindow::scaleTo(int factor)
{
    m_scaleFactor = factor;
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

void MainWindow::applySourceOverride(const QSize &size, bool persist)
{
    m_video->setSourceOverride(size);

    // Keep the radio buttons honest, including when a custom size is restored at startup.
    bool matched = false;
    for (QAction *a : m_sourceGroup->actions()) {
        if (a == m_customSourceAction)
            continue;
        if (a->data().toSize() == size) {
            a->setChecked(true);
            matched = true;
        }
    }
    if (!matched && size.isValid()) {
        m_customSourceAction->setChecked(true);
        m_customSourceAction->setText(tr("&Custom (%1 x %2)...").arg(size.width()).arg(size.height()));
    }

    if (persist) {
        settings()->setValue(QStringLiteral("shaders/sourceOverride"), size);
        // The window is sized in multiples of the source, so a deliberate source change should
        // re-snap to keep the current factor exact. Guarded on persist, so the startup restore
        // (which lets the one-time videoSizeChanged snap handle it) doesn't fight geometry.
        scaleTo(m_scaleFactor);
    }
}

void MainWindow::promptCustomSourceSize()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Custom Source Resolution"));

    const QSize current = m_video->sourceOverride().isValid() ? m_video->sourceOverride()
                                                              : m_video->sourceSize();
    auto *w = new QSpinBox(&dlg);
    w->setRange(16, 8192);
    w->setValue(current.width() > 0 ? current.width() : 320);
    auto *h = new QSpinBox(&dlg);
    h->setRange(16, 8192);
    h->setValue(current.height() > 0 ? current.height() : 240);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto *form = new QFormLayout(&dlg);
    form->addRow(tr("Width:"), w);
    form->addRow(tr("Height:"), h);
    form->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted)
        applySourceOverride(QSize(w->value(), h->value()));
    else
        applySourceOverride(m_video->sourceOverride()); // resync the checkmarks
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

    const QSize override = s->value(QStringLiteral("shaders/sourceOverride")).toSize();
    applySourceOverride(override, false);

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
