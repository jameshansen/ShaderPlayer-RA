#pragma once

#include <QMainWindow>

class VideoWidget;
class ShaderPanel;
class PlaylistPanel;
class QAction;
class QActionGroup;
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

private:
    void buildMenus();
    void buildTransport();
    void rebuildSubtitleMenu();
    void applySourceOverride(const QSize &size, bool persist = true);
    void promptCustomSourceSize();
    void promptOpen(bool append);
    void restoreSettings();

    VideoWidget *m_video = nullptr;
    ShaderPanel *m_shaders = nullptr;
    PlaylistPanel *m_playlist = nullptr;

    QMenu *m_subtitleMenu = nullptr;
    QActionGroup *m_sourceGroup = nullptr;
    QAction *m_customSourceAction = nullptr;

    QToolButton *m_playButton = nullptr;
    QSlider *m_seek = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_timeLabel = nullptr;

    double m_duration = 0.0;
    bool m_seeking = false;
};
