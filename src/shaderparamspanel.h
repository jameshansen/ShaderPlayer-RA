#pragma once

#include <QWidget>

class VideoWidget;
class QCheckBox;
class QLabel;
class QPushButton;
class QScrollArea;

// The dynamic settings panel: one control per parameter the current shader exposes, generated
// on the fly from VideoWidget::shaderParams(), sorted alphabetically. Values persist per-shader
// in settings.ini and apply live. Any parameter can be starred; the star survives a reset, and
// a filter shows only starred ones. The generation trusts nothing it isn't given -- the
// parameter list is already sanitized by VideoWidget, and anything read back from the settings
// file is re-clamped here.
class ShaderParamsPanel : public QWidget
{
    Q_OBJECT

public:
    ShaderParamsPanel(VideoWidget *video, const QString &shaderRoot, QWidget *parent = nullptr);

private:
    void rebuild();                 // regenerate the controls for the current shader
    void resetToDefaults();         // drop saved values, restore initials; keeps favourites
    void seedFactoryDefaults();     // first-run: apply the bundled defaults.ini
    void store(const QString &param, double value);   // persist one value override
    bool isFavourite(const QString &param) const;
    void setFavourite(const QString &param, bool on);
    QString shaderId() const;       // stable per-shader settings id, empty when no shader
    QString valueKey(const QString &param) const;
    QString favKey(const QString &param) const;

    VideoWidget *m_video = nullptr;
    QString m_root;
    QLabel *m_title = nullptr;
    QPushButton *m_reset = nullptr;
    QCheckBox *m_favOnly = nullptr;
    QScrollArea *m_scroll = nullptr;
};
