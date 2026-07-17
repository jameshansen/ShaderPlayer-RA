#include "shaderpanel.h"

#include <QDir>
#include <QDirIterator>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int PathRole = Qt::UserRole + 1;   // relative path, absent on folder nodes
}

ShaderPanel::ShaderPanel(const QString &root, QWidget *parent)
    : QWidget(parent), m_root(root)
{
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter shaders..."));
    m_filter->setClearButtonEnabled(true);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree);

    connect(m_filter, &QLineEdit::textChanged, this, &ShaderPanel::rebuild);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ShaderPanel::showContextMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        const QVariant rel = item->data(0, PathRole);
        if (!rel.isValid())
            return; // a folder node
        m_current = rel.toString();
        emit presetChosen(m_current.isEmpty() ? QString() : absolute(m_current));
    });

    QSettings s;
    const QStringList favs = s.value(QStringLiteral("shaders/favourites")).toStringList();
    m_favourites = QSet<QString>(favs.begin(), favs.end());

    scan();
    rebuild();
}

QString ShaderPanel::absolute(const QString &relative) const
{
    return QDir(m_root).absoluteFilePath(relative);
}

void ShaderPanel::scan()
{
    m_all.clear();
    QDir root(m_root);
    QDirIterator it(m_root, { QStringLiteral("*.slangp") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        m_all.append(root.relativeFilePath(it.next()));
    m_all.sort(Qt::CaseInsensitive);
}

void ShaderPanel::rebuild()
{
    const QString needle = m_filter->text().trimmed();
    m_tree->clear();

    // "None" is how you get back to unfiltered video.
    auto *none = new QTreeWidgetItem(m_tree, { tr("None (no shader)") });
    none->setData(0, PathRole, QString());

    auto addLeaf = [&](QTreeWidgetItem *parent, const QString &rel) {
        auto *leaf = new QTreeWidgetItem(parent, { QFileInfo(rel).completeBaseName() });
        leaf->setData(0, PathRole, rel);
        leaf->setToolTip(0, rel);
        if (rel == m_current)
            m_tree->setCurrentItem(leaf);
        return leaf;
    };

    QStringList matches;
    for (const QString &rel : m_all)
        if (needle.isEmpty() || rel.contains(needle, Qt::CaseInsensitive))
            matches.append(rel);

    if (!m_favourites.isEmpty()) {
        auto *favRoot = new QTreeWidgetItem(m_tree, { tr("★ Favourites") });
        for (const QString &rel : matches)
            if (m_favourites.contains(rel))
                addLeaf(favRoot, rel);
        favRoot->setExpanded(true);
    }

    auto *allRoot = new QTreeWidgetItem(m_tree, { tr("All shaders") });

    // Fold each relative path into folder nodes, creating them on first use.
    QHash<QString, QTreeWidgetItem *> folders;
    for (const QString &rel : matches) {
        QTreeWidgetItem *parent = allRoot;
        const QString dir = QFileInfo(rel).path();
        if (dir != QLatin1String(".")) {
            QString sofar;
            for (const QString &part : dir.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                sofar = sofar.isEmpty() ? part : sofar + QLatin1Char('/') + part;
                auto found = folders.constFind(sofar);
                if (found == folders.constEnd())
                    found = folders.insert(sofar, new QTreeWidgetItem(parent, { part }));
                parent = *found;
            }
        }
        addLeaf(parent, rel);
    }

    // A filter is only useful if you can see what it matched.
    allRoot->setExpanded(true);
    if (!needle.isEmpty() && matches.size() <= 200)
        m_tree->expandAll();
}

void ShaderPanel::setCurrentPreset(const QString &absolutePath)
{
    m_current = absolutePath.isEmpty() ? QString() : QDir(m_root).relativeFilePath(absolutePath);
    rebuild();
}

void ShaderPanel::toggleFavourite(QTreeWidgetItem *item)
{
    const QVariant rel = item->data(0, PathRole);
    if (!rel.isValid() || rel.toString().isEmpty())
        return;

    const QString path = rel.toString();
    if (m_favourites.contains(path))
        m_favourites.remove(path);
    else
        m_favourites.insert(path);

    QSettings s;
    s.setValue(QStringLiteral("shaders/favourites"), QStringList(m_favourites.begin(), m_favourites.end()));
    rebuild();
}

void ShaderPanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item)
        return;
    const QVariant rel = item->data(0, PathRole);
    if (!rel.isValid() || rel.toString().isEmpty())
        return;

    QMenu menu(this);
    const bool fav = m_favourites.contains(rel.toString());
    QAction *toggle = menu.addAction(fav ? tr("Remove from favourites") : tr("Add to favourites"));
    if (menu.exec(m_tree->viewport()->mapToGlobal(pos)) == toggle)
        toggleFavourite(item);
}
