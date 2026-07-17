#include "playlistpanel.h"

#include "appdata.h"
#include "videowidget.h"

#include <QAbstractItemModel>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QProcess>
#include <QTextStream>
#include <QVBoxLayout>

PlaylistPanel::PlaylistPanel(VideoWidget *video, QWidget *parent)
    : QWidget(parent), m_video(video)
{
    m_list = new QListWidget(this);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_list);

    connect(m_video, &VideoWidget::playlistChanged, this, &PlaylistPanel::refresh);

    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        m_video->playIndex(m_list->row(item));
    });

    // A drag reorders the view; push the same move into mpv, which is the source of truth.
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &, int start, int, const QModelIndex &, int dest) {
        if (m_updating)
            return;
        m_video->movePlaylistEntry(start, dest);
    });

    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_list->itemAt(pos);
        if (!item)
            return;
        const int row = m_list->row(item);

        QMenu menu(this);
        QAction *play = menu.addAction(tr("Play"));
        QAction *reveal = menu.addAction(tr("Show File Location..."));
        menu.addSeparator();
        QAction *remove = menu.addAction(tr("Remove from Playlist"));

        QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
        if (chosen == play) {
            m_video->playIndex(row);
        } else if (chosen == remove) {
            m_video->removePlaylistEntry(row);
        } else if (chosen == reveal) {
            const QString f = QDir::toNativeSeparators(m_video->playlist().value(row).filename);
            QProcess::startDetached(QStringLiteral("explorer.exe"), { QStringLiteral("/select,") + f });
        }
    });

    refresh();
}

void PlaylistPanel::refresh()
{
    const QList<PlaylistEntry> entries = m_video->playlist();

    // Rebuilding fires rowsMoved-adjacent signals; don't echo them back to mpv.
    m_updating = true;
    m_list->clear();
    for (const PlaylistEntry &e : entries) {
        auto *item = new QListWidgetItem(e.title, m_list);
        item->setToolTip(e.filename);
        if (e.current) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            m_list->setCurrentItem(item);
        }
    }
    m_updating = false;
}

void PlaylistPanel::save() const
{
    const QList<PlaylistEntry> entries = m_video->playlist();
    QFile f(playlistPath());
    if (entries.isEmpty()) {
        f.remove();
        return;
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning("cannot write playlist to %s", qUtf8Printable(playlistPath()));
        return;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "#EXTM3U\n";
    for (const PlaylistEntry &e : entries)
        out << e.filename << '\n';
}

void PlaylistPanel::restore()
{
    if (QFileInfo::exists(playlistPath()))
        m_video->loadList(playlistPath());
}
