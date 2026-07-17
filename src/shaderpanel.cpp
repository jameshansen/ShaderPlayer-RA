#include "shaderpanel.h"

#include "appdata.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QProcess>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QTextStream>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int PathRole = Qt::UserRole + 1;   // relative path; absent on folder nodes

QString settingsKey(const QString &relative, const char *field)
{
    // '/' is a group separator in QSettings, and '\' upsets the INI writer, so fold the
    // path into a flat key.
    QString flat = relative;
    flat.replace(QLatin1Char('\\'), QLatin1Char('|')).replace(QLatin1Char('/'), QLatin1Char('|'));
    return QStringLiteral("shaderinfo/") + flat + QLatin1Char('/') + QLatin1String(field);
}

// Every pass a preset references, in order. shader0 is not always the interesting one --
// crt-hyllian's first pass is a LUT loader with no authorship in it.
QStringList passSources(const QString &presetPath)
{
    QFile f(presetPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    static const QRegularExpression re(
        QStringLiteral("^\\s*shader\\d+\\s*=\\s*\"?([^\"\\r\\n]+?)\"?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);

    QStringList out;
    const QDir base(QFileInfo(presetPath).path());
    QTextStream in(&f);
    while (!in.atEnd()) {
        const auto m = re.match(in.readLine());
        if (m.hasMatch())
            out.append(base.absoluteFilePath(m.captured(1).trimmed()));
    }
    return out;
}

// Best-effort: pull a human name and a URL out of a shader's licence header.
// Roughly half the .slang files carry authorship at all, so "unknown" is a normal answer --
// hence the manual override in the context menu.
ShaderInfo scrapeFile(const QString &path)
{
    ShaderInfo out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    // "Author: X" / "Copyright (C) 2014 TroggleMonkey <...>" / a bare "//   by Timothy Lottes".
    static const QRegularExpression author(
        QStringLiteral("Author[s]?\\s*[:\\-]\\s*([^<(\\r\\n]+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression copyright(
        QStringLiteral("Copyright\\s*(?:\\(C\\)|\\xA9)?\\s*[0-9,\\-\\s]*\\s*(?:by\\s+)?([^<(\\r\\n]+)"),
        QRegularExpression::CaseInsensitiveOption);
    // Anchored so "Ported by" and "modified by" don't masquerade as authorship.
    static const QRegularExpression byLine(
        QStringLiteral("^\\s*(?://+|\\*|/\\*)?\\s*by\\s+([A-Za-z][^<(\\r\\n]{2,40})$"));
    static const QRegularExpression url(QStringLiteral("(https?://[^\\s*\\\"'>\\)]+)"));
    static const QRegularExpression trailingJunk(QStringLiteral("[\\s/*\\-,.]+$"));

    QTextStream in(&f);
    // The header sits after the #version/uniform block in most of these, so read the lot.
    while (!in.atEnd()) {
        const QString text = in.readLine();

        if (out.author.isEmpty()) {
            auto m = author.match(text);
            if (!m.hasMatch())
                m = copyright.match(text);
            if (!m.hasMatch())
                m = byLine.match(text);
            if (m.hasMatch()) {
                QString name = m.captured(1).trimmed();
                name.remove(trailingJunk);
                if (!name.isEmpty() && name.length() < 60
                    && !name.contains(QLatin1String("Free Software Foundation"))
                    && !name.contains(QLatin1String("the "), Qt::CaseInsensitive))
                    out.author = name;
            }
        }
        if (out.website.isEmpty()) {
            const auto m = url.match(text);
            if (m.hasMatch()) {
                const QString link = m.captured(1);
                // gnu.org licence boilerplate is not a homepage, and comments often cite a
                // reference image rather than a project page.
                static const QRegularExpression image(
                    QStringLiteral("\\.(gif|png|jpe?g|bmp|svg)$"), QRegularExpression::CaseInsensitiveOption);
                if (!link.contains(QLatin1String("gnu.org")) && !image.match(link).hasMatch())
                    out.website = link;
            }
        }
        if (!out.author.isEmpty() && !out.website.isEmpty())
            break;
    }
    return out;
}

ShaderInfo scrape(const QString &presetPath)
{
    ShaderInfo out;
    for (const QString &pass : passSources(presetPath)) {
        const ShaderInfo i = scrapeFile(pass);
        if (out.author.isEmpty())
            out.author = i.author;
        if (out.website.isEmpty())
            out.website = i.website;
        if (!out.author.isEmpty() && !out.website.isEmpty())
            break;
    }
    return out;
}

QIcon defaultIcon()
{
    // ponytail: a stock monitor glyph stands in for the TV icon. Swap in real artwork when
    // the app has any; the per-shader override already works.
    return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

// A QLabel that shrinks with its card and ends in "..." rather than cutting mid-word.
class ElidingLabel : public QLabel
{
public:
    explicit ElidingLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(parent), m_full(text)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setToolTip(text);
        QLabel::setText(text);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QLabel::resizeEvent(e);
        QLabel::setText(fontMetrics().elidedText(m_full, Qt::ElideRight, width()));
    }

private:
    QString m_full;
};

} // namespace

// ---------------------------------------------------------------- ShaderCard

ShaderCard::ShaderCard(const QString &relative, const ShaderInfo &info, QWidget *parent)
    : QFrame(parent), m_relative(relative)
{
    setFrameShape(QFrame::StyledPanel);
    setContextMenuPolicy(Qt::CustomContextMenu);

    m_check = new QCheckBox(this);
    m_check->setChecked(true); // it is in the favourites list, so by definition
    m_check->setToolTip(tr("Remove from favourites"));
    connect(m_check, &QCheckBox::toggled, this, [this](bool on) {
        if (!on)
            emit unfavourited(m_relative);
    });

    auto *icon = new QLabel(this);
    const QIcon ic = info.iconPath.isEmpty() ? defaultIcon() : QIcon(info.iconPath);
    icon->setPixmap(ic.pixmap(40, 40));
    icon->setFixedSize(44, 44);
    icon->setAlignment(Qt::AlignCenter);

    auto *name = new ElidingLabel(QFileInfo(relative).completeBaseName(), this);
    QFont bold = name->font();
    bold.setBold(true);
    name->setFont(bold);
    name->setToolTip(relative);

    auto *by = new ElidingLabel(info.author.isEmpty() ? tr("Author unknown")
                                                      : tr("Written by %1").arg(info.author), this);
    by->setEnabled(false);

    auto *text = new QVBoxLayout;
    text->setSpacing(0);
    text->addWidget(name);
    text->addWidget(by);
    if (!info.website.isEmpty()) {
        // Link the host rather than the raw URL: it stays short and still says where it goes.
        const QString host = QUrl(info.website).host();
        auto *link = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>")
                                    .arg(info.website, host.isEmpty() ? tr("Website") : host), this);
        link->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        link->setOpenExternalLinks(true);
        link->setTextInteractionFlags(Qt::TextBrowserInteraction);
        link->setToolTip(info.website);
        text->addWidget(link);
    }

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 4, 4, 4);
    row->addWidget(m_check);
    row->addWidget(icon);
    row->addLayout(text, 1);
}

void ShaderCard::setActive(bool active)
{
    setFrameShadow(active ? QFrame::Sunken : QFrame::Raised);
    setBackgroundRole(active ? QPalette::Highlight : QPalette::Base);
    setAutoFillBackground(active);
}

void ShaderCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        emit clicked(m_relative);
    QFrame::mousePressEvent(e);
}

// --------------------------------------------------------------- ShaderPanel

ShaderPanel::ShaderPanel(const QString &root, QWidget *parent)
    : QWidget(parent), m_root(root)
{
    m_favMode = new QRadioButton(tr("Favourites"), this);
    m_allMode = new QRadioButton(tr("All"), this);
    m_favMode->setChecked(true);

    auto *modes = new QHBoxLayout;
    modes->addWidget(m_favMode);
    modes->addWidget(m_allMode);
    modes->addStretch(1);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter shaders..."));
    m_filter->setClearButtonEnabled(true);

    m_favList = new QListWidget(this);
    m_favList->setSelectionMode(QAbstractItemView::NoSelection);
    m_favList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_favList->setResizeMode(QListView::Adjust);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_favList);
    m_stack->addWidget(m_tree);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(modes);
    layout->addWidget(m_filter);
    layout->addWidget(m_stack);

    connect(m_filter, &QLineEdit::textChanged, this, &ShaderPanel::rebuild);
    connect(m_favMode, &QRadioButton::toggled, this, &ShaderPanel::rebuild);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ShaderPanel::treeContextMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        const QVariant rel = item->data(0, PathRole);
        if (!rel.isValid())
            return; // a folder node
        m_current = rel.toString();
        emit presetChosen(m_current.isEmpty() ? QString() : absolute(m_current));
    });

    const QStringList favs = settings()->value(QStringLiteral("shaders/favourites")).toStringList();
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

ShaderInfo ShaderPanel::info(const QString &relative)
{
    auto cached = m_info.constFind(relative);
    if (cached != m_info.constEnd())
        return *cached;

    ShaderInfo i;
    QSettings *s = settings();
    i.iconPath = s->value(settingsKey(relative, "icon")).toString();

    // A stored author (scraped once, or corrected by hand) wins; only scrape on first sight.
    if (s->contains(settingsKey(relative, "author"))) {
        i.author = s->value(settingsKey(relative, "author")).toString();
        i.website = s->value(settingsKey(relative, "website")).toString();
    } else {
        const ShaderInfo scraped = scrape(absolute(relative));
        i.author = scraped.author;
        i.website = scraped.website;
        s->setValue(settingsKey(relative, "author"), i.author);
        s->setValue(settingsKey(relative, "website"), i.website);
    }

    m_info.insert(relative, i);
    return i;
}

void ShaderPanel::saveInfo(const QString &relative, const ShaderInfo &i)
{
    QSettings *s = settings();
    s->setValue(settingsKey(relative, "author"), i.author);
    s->setValue(settingsKey(relative, "website"), i.website);
    s->setValue(settingsKey(relative, "icon"), i.iconPath);
    m_info.insert(relative, i);
}

void ShaderPanel::rebuild()
{
    const bool fav = m_favMode->isChecked();
    m_stack->setCurrentWidget(fav ? static_cast<QWidget *>(m_favList) : m_tree);
    if (fav)
        rebuildFavourites();
    else
        rebuildTree();
}

void ShaderPanel::rebuildFavourites()
{
    m_favList->clear();

    const QString needle = m_filter->text().trimmed();
    QStringList favs(m_favourites.begin(), m_favourites.end());
    favs.sort(Qt::CaseInsensitive);

    for (const QString &rel : favs) {
        if (!needle.isEmpty() && !rel.contains(needle, Qt::CaseInsensitive))
            continue;

        auto *card = new ShaderCard(rel, info(rel), m_favList);
        card->setActive(rel == m_current);
        connect(card, &ShaderCard::clicked, this, [this](const QString &r) {
            m_current = r;
            emit presetChosen(absolute(r));
            rebuildFavourites();
        });
        connect(card, &ShaderCard::unfavourited, this, [this](const QString &r) {
            setFavourite(r, false);
        });
        connect(card, &ShaderCard::customContextMenuRequested, this, [this, card, rel](const QPoint &p) {
            cardContextMenu(rel, card->mapToGlobal(p));
        });

        auto *item = new QListWidgetItem(m_favList);
        // Only the height matters; the view stretches the row to the viewport width.
        item->setSizeHint(QSize(0, card->sizeHint().height()));
        m_favList->setItemWidget(item, card);
    }

    if (m_favList->count() == 0) {
        auto *empty = new QListWidgetItem(
            m_favourites.isEmpty() ? tr("No favourites yet.\nSwitch to All and star a shader.")
                                   : tr("No favourites match the filter."),
            m_favList);
        empty->setFlags(Qt::NoItemFlags);
    }
}

void ShaderPanel::rebuildTree()
{
    const QString needle = m_filter->text().trimmed();
    m_tree->clear();

    auto *none = new QTreeWidgetItem(m_tree, { tr("None (no shader)") });
    none->setData(0, PathRole, QString());

    QStringList matches;
    for (const QString &rel : m_all)
        if (needle.isEmpty() || rel.contains(needle, Qt::CaseInsensitive))
            matches.append(rel);

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
        auto *leaf = new QTreeWidgetItem(parent, { QFileInfo(rel).completeBaseName() });
        leaf->setData(0, PathRole, rel);
        leaf->setToolTip(0, rel);
        // The checkbox is the star: tick it to favourite, untick to drop it.
        leaf->setCheckState(0, m_favourites.contains(rel) ? Qt::Checked : Qt::Unchecked);
        if (rel == m_current)
            m_tree->setCurrentItem(leaf);
    }

    allRoot->setExpanded(true);
    if (!needle.isEmpty() && matches.size() <= 200)
        m_tree->expandAll();

    // Connected after populating so setCheckState above doesn't fire it.
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) {
        const QVariant rel = item->data(0, PathRole);
        if (rel.isValid() && !rel.toString().isEmpty())
            setFavourite(rel.toString(), item->checkState(0) == Qt::Checked);
    }, Qt::UniqueConnection);
}

void ShaderPanel::setFavourite(const QString &relative, bool on)
{
    if (on == m_favourites.contains(relative))
        return;
    if (on)
        m_favourites.insert(relative);
    else
        m_favourites.remove(relative);

    settings()->setValue(QStringLiteral("shaders/favourites"),
                         QStringList(m_favourites.begin(), m_favourites.end()));
    if (m_favMode->isChecked())
        rebuildFavourites();
}

void ShaderPanel::setCurrentPreset(const QString &absolutePath)
{
    m_current = absolutePath.isEmpty() ? QString() : QDir(m_root).relativeFilePath(absolutePath);
    rebuild();
}

void ShaderPanel::revealInExplorer(const QString &relative)
{
    // /select, needs native separators and puts the file itself in view.
    const QString path = QDir::toNativeSeparators(absolute(relative));
    QProcess::startDetached(QStringLiteral("explorer.exe"), { QStringLiteral("/select,") + path });
}

void ShaderPanel::editAuthor(const QString &relative)
{
    ShaderInfo i = info(relative);
    bool ok = false;
    const QString author = QInputDialog::getText(this, tr("Edit Author"),
        tr("Author of %1:").arg(QFileInfo(relative).completeBaseName()),
        QLineEdit::Normal, i.author, &ok);
    if (!ok)
        return;
    i.author = author.trimmed();

    const QString site = QInputDialog::getText(this, tr("Edit Website"),
        tr("Website (optional):"), QLineEdit::Normal, i.website, &ok);
    if (ok)
        i.website = site.trimmed();

    saveInfo(relative, i);
    rebuild();
}

void ShaderPanel::changeIcon(const QString &relative)
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Change Icon"), QString(),
        tr("Images (*.png *.ico *.jpg *.jpeg *.bmp *.svg)"));
    if (file.isEmpty())
        return;
    ShaderInfo i = info(relative);
    i.iconPath = file;
    saveInfo(relative, i);
    rebuild();
}

void ShaderPanel::cardContextMenu(const QString &relative, const QPoint &globalPos)
{
    QMenu menu(this);
    QAction *icon = menu.addAction(tr("Change Icon..."));
    QAction *author = menu.addAction(tr("Edit Author..."));
    menu.addSeparator();
    QAction *reveal = menu.addAction(tr("Show Shader File Location..."));
    menu.addSeparator();
    QAction *unfav = menu.addAction(tr("Remove from favourites"));

    QAction *chosen = menu.exec(globalPos);
    if (chosen == icon)
        changeIcon(relative);
    else if (chosen == author)
        editAuthor(relative);
    else if (chosen == reveal)
        revealInExplorer(relative);
    else if (chosen == unfav)
        setFavourite(relative, false);
}

void ShaderPanel::treeContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item)
        return;
    const QVariant rel = item->data(0, PathRole);
    if (!rel.isValid() || rel.toString().isEmpty())
        return;
    const QString relative = rel.toString();

    QMenu menu(this);
    const bool fav = m_favourites.contains(relative);
    QAction *toggle = menu.addAction(fav ? tr("Remove from favourites")
                                         : tr("Add to favourites"));
    menu.addSeparator();
    QAction *reveal = menu.addAction(tr("Show Shader File Location..."));

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (chosen == toggle) {
        item->setCheckState(0, fav ? Qt::Unchecked : Qt::Checked); // itemChanged does the rest
    } else if (chosen == reveal) {
        revealInExplorer(relative);
    }
}
