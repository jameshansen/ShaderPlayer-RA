#include "mainwindow.h"

#include "shaderpanel.h"
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
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
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
    auto *dock = new QDockWidget(tr("Shaders"), this);
    dock->setObjectName(QStringLiteral("shaderDock"));
    dock->setWidget(m_shaders);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    buildMenus();
    buildTransport();

    connect(m_shaders, &ShaderPanel::presetChosen, m_video, &VideoWidget::setPreset);
    connect(m_video, &VideoWidget::shaderFailed, this, [this](const QString &msg) {
        QMessageBox::warning(this, tr("Shader failed"),
                             tr("%1\n\nFalling back to no shader.").arg(msg));
        m_shaders->setCurrentPreset(QString());
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
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 4, 6, 4);

    auto button = [&](QStyle::StandardPixmap icon, const QString &tip, auto slot) {
        auto *b = new QToolButton(bar);
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, m_video, slot);
        layout->addWidget(b);
        return b;
    };

    button(QStyle::SP_MediaSkipBackward, tr("Previous"), &VideoWidget::stepBackward);
    m_playButton = button(QStyle::SP_MediaPlay, tr("Play/Pause"), &VideoWidget::togglePause);
    button(QStyle::SP_MediaSkipForward, tr("Next"), &VideoWidget::stepForward);

    m_seek = new QSlider(Qt::Horizontal, bar);
    m_seek->setRange(0, 0);
    layout->addWidget(m_seek, 1);

    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), bar);
    layout->addWidget(m_timeLabel);

    auto *volIcon = new QLabel(bar);
    volIcon->setPixmap(style()->standardIcon(QStyle::SP_MediaVolume).pixmap(16, 16));
    layout->addWidget(volIcon);

    m_volume = new QSlider(Qt::Horizontal, bar);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(90);
    layout->addWidget(m_volume);

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
        m_playButton->setIcon(style()->standardIcon(paused ? QStyle::SP_MediaPlay
                                                           : QStyle::SP_MediaPause));
    });
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

    if (persist)
        QSettings().setValue(QStringLiteral("shaders/sourceOverride"), size);
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
        QSettings().value(QStringLiteral("lastDir")).toString(), tr(kVideoFilter));
    if (!files.isEmpty())
        openFiles(files, append);
}

void MainWindow::openFiles(const QStringList &files, bool append)
{
    if (files.isEmpty())
        return;
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(files.first()).absolutePath());
    m_video->open(files, append);
}

void MainWindow::restoreSettings()
{
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(s.value(QStringLiteral("window/state")).toByteArray());

    m_volume->setValue(s.value(QStringLiteral("volume"), 100).toInt());

    const QSize override = s.value(QStringLiteral("shaders/sourceOverride")).toSize();
    applySourceOverride(override, false);

    const QString preset = s.value(QStringLiteral("shaders/lastPreset")).toString();
    if (!preset.isEmpty() && QFileInfo::exists(preset)) {
        m_video->setPreset(preset);
        m_shaders->setCurrentPreset(preset);
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/state"), saveState());
    s.setValue(QStringLiteral("volume"), m_volume->value());
    s.setValue(QStringLiteral("shaders/lastPreset"), m_video->preset());
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
