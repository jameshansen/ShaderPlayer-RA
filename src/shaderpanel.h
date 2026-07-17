#pragma once

#include <QFrame>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

// What we can discover about a preset. No .slangp carries author metadata, so this is
// scraped out of the shader source comments and can be corrected by hand.
struct ShaderInfo {
    QString author;
    QString website;
    QString iconPath;   // empty = default icon
};

// One row of the verbose favourites view: [icon] name / author / website. Being highlighted
// is what marks it active; unfavouriting lives in the context menu.
class ShaderCard : public QFrame
{
    Q_OBJECT

public:
    ShaderCard(const QString &relative, const ShaderInfo &info, QWidget *parent = nullptr);
    void setActive(bool active);

signals:
    void clicked(const QString &relative);

protected:
    void mousePressEvent(QMouseEvent *e) override;

private:
    QString m_relative;
};

// Right-hand Shaders tab: a verbose favourites list, or the full tree.
class ShaderPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ShaderPanel(const QString &root, QWidget *parent = nullptr);

    void setCurrentPreset(const QString &absolutePath);

signals:
    void presetChosen(const QString &absolutePath); // empty means passthrough

protected:
    // Catches clicks on the star before the tree turns them into a selection.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void scan();
    void rebuildTree();
    void rebuildFavourites();
    void rebuild();
    void setFavourite(const QString &relative, bool on);
    void treeContextMenu(const QPoint &pos);
    void cardContextMenu(const QString &relative, const QPoint &globalPos);
    void editAuthor(const QString &relative);
    void changeIcon(const QString &relative);
    void revealInExplorer(const QString &relative);

    ShaderInfo info(const QString &relative);      // scrapes and caches on first use
    void saveInfo(const QString &relative, const ShaderInfo &i);
    QString absolute(const QString &relative) const;

    QString m_root;
    QStringList m_all;                  // relative paths, sorted
    QSet<QString> m_favourites;
    QHash<QString, ShaderInfo> m_info;  // cache

    QRadioButton *m_favMode = nullptr;
    QRadioButton *m_allMode = nullptr;
    QLineEdit *m_filter = nullptr;
    QStackedWidget *m_stack = nullptr;
    QListWidget *m_favList = nullptr;
    QTreeWidget *m_tree = nullptr;
    QString m_current;                  // relative path
};
