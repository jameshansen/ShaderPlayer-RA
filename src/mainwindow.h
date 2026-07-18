#pragma once

#include <QList>
#include <QMainWindow>

class VideoWidget;
class ShaderPanel;
class ShaderParamsPanel;
class PlaylistPanel;
class HooksPanel;
class HookWorker;
class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;
class QSlider;
class QToolButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openFiles(const QStringList &files, bool append = false);
    void restorePlaylist();
    VideoWidget *video() const { return m_video; }

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

public slots:
    void setFullscreen(bool on);
    void toggleFullscreen();
    void scaleTo(int factor);   // resize so the video shows at factor x its pixel size

private:
    void buildMenus();
    void buildTransport();
    void buildResolutionBar();
    void rebuildSubtitleMenu();
    void applySourceLines(int lines, bool persist = true);
    void updateSourceLabel();
    void promptOpen(bool append);
    void restoreSettings();

    VideoWidget *m_video = nullptr;
    ShaderPanel *m_shaders = nullptr;
    ShaderParamsPanel *m_params = nullptr;
    PlaylistPanel *m_playlist = nullptr;
    HooksPanel *m_hooks = nullptr;
    HookWorker *m_hookWorker = nullptr;   // shades hooked games with the same preset as the video

    QMenu *m_subtitleMenu = nullptr;
    QList<QToolButton *> m_sourceButtons;   // one per line count; the checked one is active
    QLabel *m_sourceLabel = nullptr;        // what the current pick resolves to for this video
    QAction *m_fullscreenAction = nullptr;
    QDockWidget *m_libraryDock = nullptr;
    QByteArray m_preFullscreenState;   // dock layout to put back on the way out

    QToolButton *m_playButton = nullptr;
    QSlider *m_seek = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_timeLabel = nullptr;

    double m_duration = 0.0;
    bool m_seeking = false;
    int m_scaleFactor = 2;      // last chosen video scale; startup applies it once
    bool m_scaledOnce = false;  // guards the one-time startup snap against later files
};
