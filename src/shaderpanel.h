#pragma once

#include <QWidget>
#include <QSet>
#include <QStringList>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

// Right-hand bar: every .slangp under the shader root, plus a favourites section.
// Paths are held relative to the root so the settings survive moving the install.
class ShaderPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ShaderPanel(const QString &root, QWidget *parent = nullptr);

    void setCurrentPreset(const QString &absolutePath);

signals:
    void presetChosen(const QString &absolutePath); // empty means passthrough

private:
    void scan();
    void rebuild();
    void toggleFavourite(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    QString absolute(const QString &relative) const;

    QString m_root;
    QStringList m_all;      // relative paths, sorted
    QSet<QString> m_favourites;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QString m_current;      // relative path
};
