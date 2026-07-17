#include "shaderparamspanel.h"

#include "appdata.h"
#include "videowidget.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// Fold a path or a parameter name into a flat, injection-proof settings token. '/' and '\'
// are QSettings group separators, so a shader called "a/b" must not open a sub-group.
QString flatten(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QLatin1Char('|')).replace(QLatin1Char('/'), QLatin1Char('|'));
    return out;
}

QString formatValue(double v)
{
    return QString::number(v, 'g', 4);
}

// The factory defaults file: staged beside the exe by the build, with a dev-tree fallback.
QString defaultsFile()
{
    const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/defaults.ini");
    if (QFileInfo::exists(beside))
        return beside;
    return QStringLiteral(SHADERPLAYER_DEFAULTS);
}

const char *kFavOnlyKey = "shaderparams/favouritesOnly";

QString labelFor(const ShaderParam &p)
{
    return p.description.isEmpty() ? p.name : p.description;
}

// Slider resolution for a continuous parameter. Bounded so a preset declaring a gigantic range
// with a tiny step cannot ask for a billion-position slider.
int sliderSteps(const ShaderParam &p)
{
    const double range = double(p.maximum) - double(p.minimum);
    const double step = (p.step > 0.0f) ? double(p.step) : range / 100.0;
    if (step <= 0.0)
        return 1;
    return std::clamp(int(std::lround(range / step)), 1, 10000);
}

// A gold-when-on, grey-when-off star toggle. The icon is the same star.ico the shader list uses.
QToolButton *makeStar(bool on)
{
    static const QIcon star(QStringLiteral(":/icons/star.ico"));
    auto *b = new QToolButton;
    b->setCheckable(true);
    b->setChecked(on);
    b->setAutoRaise(true);
    b->setToolTip(QObject::tr("Favourite this setting"));
    b->setFixedWidth(24);
    auto paint = [b] {
        b->setIcon(star.pixmap(QSize(16, 16), 1.0, b->isChecked() ? QIcon::Normal : QIcon::Disabled));
    };
    paint();
    QObject::connect(b, &QToolButton::toggled, b, [paint](bool) { paint(); });
    return b;
}

} // namespace

ShaderParamsPanel::ShaderParamsPanel(VideoWidget *video, const QString &shaderRoot, QWidget *parent)
    : QWidget(parent), m_video(video), m_root(shaderRoot)
{
    seedFactoryDefaults();

    m_title = new QLabel(this);
    m_title->setWordWrap(true);

    m_reset = new QPushButton(tr("Reset to Default"), this);
    connect(m_reset, &QPushButton::clicked, this, &ShaderParamsPanel::resetToDefaults);

    auto *header = new QHBoxLayout;
    header->addWidget(m_title, 1);
    header->addWidget(m_reset);

    m_favOnly = new QCheckBox(tr("Favourites only"), this);
    m_favOnly->setChecked(settings()->value(QLatin1String(kFavOnlyKey), false).toBool());
    connect(m_favOnly, &QCheckBox::toggled, this, [this](bool on) {
        settings()->setValue(QLatin1String(kFavOnlyKey), on);
        rebuild();
    });

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(header);
    layout->addWidget(m_favOnly);
    layout->addWidget(m_scroll, 1);

    connect(m_video, &VideoWidget::shaderParamsChanged, this, &ShaderParamsPanel::rebuild);
    rebuild();
}

QString ShaderParamsPanel::shaderId() const
{
    const QString abs = m_video->preset();
    if (abs.isEmpty())
        return QString();
    return flatten(QDir(m_root).relativeFilePath(abs));
}

QString ShaderParamsPanel::valueKey(const QString &param) const
{
    return QStringLiteral("shaderparams/") + shaderId() + QLatin1Char('/') + flatten(param);
}

// Favourites live in a separate top-level group so "Reset to Default", which clears the value
// group, cannot touch them.
QString ShaderParamsPanel::favKey(const QString &param) const
{
    return QStringLiteral("shaderfav/") + shaderId() + QLatin1Char('/') + flatten(param);
}

bool ShaderParamsPanel::isFavourite(const QString &param) const
{
    return settings()->value(favKey(param), false).toBool();
}

void ShaderParamsPanel::setFavourite(const QString &param, bool on)
{
    if (on)
        settings()->setValue(favKey(param), true);
    else
        settings()->remove(favKey(param));
}

void ShaderParamsPanel::store(const QString &param, double value)
{
    settings()->setValue(valueKey(param), value);
}

void ShaderParamsPanel::rebuild()
{
    // Sort a copy alphabetically by visible label; the preset's own order is arbitrary.
    QList<ShaderParam> params = m_video->shaderParams();
    std::sort(params.begin(), params.end(), [](const ShaderParam &a, const ShaderParam &b) {
        return labelFor(a).compare(labelFor(b), Qt::CaseInsensitive) < 0;
    });

    const bool favOnly = m_favOnly->isChecked();
    const QString name = m_video->preset().isEmpty()
        ? tr("No shader selected")
        : QFileInfo(m_video->preset()).completeBaseName();
    m_title->setText(params.isEmpty()
        ? tr("<b>%1</b><br>No adjustable settings.").arg(name)
        : tr("<b>%1</b><br>%n setting(s)", nullptr, int(params.size())).arg(name));
    m_reset->setEnabled(!params.isEmpty());
    m_favOnly->setEnabled(!params.isEmpty());

    // A fresh container each time; setWidget deletes the previous one and all its controls.
    auto *form = new QWidget;
    auto *col = new QVBoxLayout(form);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);

    QSettings *s = settings();
    int shown = 0;
    for (const ShaderParam &p : params) {
        const bool fav = isFavourite(p.name);
        if (favOnly && !fav)
            continue;
        ++shown;

        // Start from the saved override if there is one, re-clamped -- a hand-edited settings
        // file cannot push a value outside the parameter's declared bounds.
        double value = p.value;
        const QString key = valueKey(p.name);
        if (s->contains(key)) {
            bool ok = false;
            const double saved = s->value(key).toDouble(&ok);
            if (ok && std::isfinite(saved)) {
                value = std::clamp(saved, double(p.minimum), double(p.maximum));
                m_video->setShaderParam(p.name, float(value));
            }
        }

        auto *row = new QWidget(form);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        auto *star = makeStar(fav);
        connect(star, &QToolButton::toggled, this, [this, name = p.name, favOnly](bool on) {
            setFavourite(name, on);
            if (favOnly && !on)
                rebuild(); // it just left the filtered view
        });
        rl->addWidget(star);

        const QString label = labelFor(p);

        // A 0/1 toggle reads best as a checkbox.
        if (p.minimum == 0.0f && p.maximum == 1.0f && p.step == 1.0f) {
            auto *box = new QCheckBox(label, row);
            box->setChecked(value >= 0.5);
            box->setToolTip(p.name);
            connect(box, &QCheckBox::toggled, this, [this, name = p.name](bool on) {
                const double v = on ? 1.0 : 0.0;
                m_video->setShaderParam(name, float(v));
                store(name, v);
            });
            rl->addWidget(box, 1);
            col->addWidget(row);
            continue;
        }

        auto *caption = new QLabel(label, row);
        caption->setToolTip(p.name);
        caption->setMinimumWidth(80);
        rl->addWidget(caption);

        // A fixed parameter (min == max) has nothing to drag; show it read-only.
        if (p.maximum <= p.minimum) {
            rl->addWidget(new QLabel(formatValue(value), row), 1, Qt::AlignRight);
            col->addWidget(row);
            continue;
        }

        const int steps = sliderSteps(p);
        const double lo = p.minimum, hi = p.maximum;
        auto toValue = [lo, hi, steps](int i) { return lo + (hi - lo) * double(i) / steps; };
        auto toSlider = [lo, hi, steps](double v) {
            return int(std::lround((v - lo) / (hi - lo) * steps));
        };

        auto *slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(0, steps);
        slider->setValue(std::clamp(toSlider(value), 0, steps));

        auto *readout = new QLabel(formatValue(value), row);
        readout->setMinimumWidth(48);
        readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        connect(slider, &QSlider::valueChanged, this, [this, name = p.name, readout, toValue](int i) {
            const double v = toValue(i);
            readout->setText(formatValue(v));
            m_video->setShaderParam(name, float(v));
            store(name, v);
        });

        rl->addWidget(slider, 1);
        rl->addWidget(readout);
        col->addWidget(row);
    }

    if (shown == 0 && !params.isEmpty())
        col->addWidget(new QLabel(tr("No favourite settings yet."), form));
    col->addStretch(1);

    m_scroll->setWidget(form);
}

void ShaderParamsPanel::seedFactoryDefaults()
{
    QSettings *s = settings();
    if (s->value(QStringLiteral("defaults/seeded"), false).toBool())
        return;
    // Mark seeded up front: a missing or broken file must not re-seed (and undo the user's
    // later edits) on every launch.
    s->setValue(QStringLiteral("defaults/seeded"), true);

    const QString path = defaultsFile();
    if (!QFileInfo::exists(path))
        return;
    QSettings def(path, QSettings::IniFormat);
    if (def.status() != QSettings::NoError)
        return;

    if (def.contains(QStringLiteral("ui/favouritesOnly")))
        s->setValue(QLatin1String(kFavOnlyKey), def.value(QStringLiteral("ui/favouritesOnly")).toBool());

    // Each [defaultN] group stars a set of parameters for one shader.
    for (const QString &g : def.childGroups()) {
        if (!g.startsWith(QLatin1String("default")))
            continue;
        def.beginGroup(g);
        const QString shader = def.value(QStringLiteral("shader")).toString();
        const QStringList favs = def.value(QStringLiteral("favourites")).toString()
                                     .split(QLatin1Char(','), Qt::SkipEmptyParts);
        def.endGroup();
        if (shader.isEmpty())
            continue;
        const QString id = flatten(shader);
        for (const QString &f : favs) {
            const QString param = f.trimmed();
            if (!param.isEmpty())
                s->setValue(QStringLiteral("shaderfav/") + id + QLatin1Char('/') + flatten(param), true);
        }
    }
}

void ShaderParamsPanel::resetToDefaults()
{
    const QString id = shaderId();
    if (id.isEmpty())
        return;

    // Drop only the value overrides for this shader; favourites live under shaderfav/ and stay.
    settings()->beginGroup(QStringLiteral("shaderparams/") + id);
    settings()->remove(QString());
    settings()->endGroup();

    for (const ShaderParam &p : m_video->shaderParams())
        m_video->setShaderParam(p.name, p.initial);

    rebuild();
}
