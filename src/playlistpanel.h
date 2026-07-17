#pragma once

#include <QWidget>

class VideoWidget;
class QListWidget;

// mpv already keeps a playlist, so this mirrors mpv's rather than maintaining a second copy:
// the view reflects mpv, and edits are sent back as playlist-move / playlist-remove.
class PlaylistPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PlaylistPanel(VideoWidget *video, QWidget *parent = nullptr);

    void save() const;      // write the current playlist to disk as m3u8
    void restore();         // load it back into mpv

private:
    void refresh();

    VideoWidget *m_video = nullptr;
    QListWidget *m_list = nullptr;
    bool m_updating = false;   // guards the mirror against our own edits
};
