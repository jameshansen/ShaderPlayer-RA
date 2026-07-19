#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QTimer>

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <librashader.h>

struct SubtitleTrack {
    int64_t id;
    QString label;
};

struct PlaylistEntry {
    QString title;      // media-title if mpv knows one, else the file name
    QString filename;
    bool current = false;
};

// One tunable exposed by a preset. Everything here is sanitized before it reaches the UI:
// finite bounds, minimum <= maximum, a usable name. A malicious preset cannot smuggle NaN
// ranges or a name long enough to be a problem past extractParams().
struct ShaderParam {
    QString name;           // the librashader parameter id; also the settings sub-key
    QString description;    // human label, truncated
    float minimum = 0.0f;
    float maximum = 1.0f;
    float initial = 0.0f;   // the preset's default, i.e. what "Reset to Default" restores
    float step = 0.0f;      // 0 means "no sensible step", the panel derives one
    float value = 0.0f;     // current live value
};

// What actually came out of a render stage. Enough to answer "is it black?" without a human
// squinting at a screenshot.
struct FrameStats {
    bool valid = false;
    int width = 0, height = 0;
    double nonBlack = 0.0;      // fraction of pixels above the black threshold
    int meanR = 0, meanG = 0, meanB = 0;
    // Content fingerprint. Means are useless for spotting an animated shader -- crt-royale's
    // interlacing swaps which lines are real vs interpolated and barely moves the average --
    // so comparing two frames needs the actual pixels.
    quint64 hash = 0;
    // Mean luma of the picture's top-left eighth minus its bottom-left eighth. Only the hook test
    // reads it, and only to answer one question: is the frame the right way up? A captured game
    // frame passes through three conventions that each have an opinion about which row is first
    // (OpenGL's readback, D3D's texture, the widget's blit), and a sign is the cheapest thing that
    // notices when they disagree.
    double topLeftBias = 0.0;

    QString toString() const;
};

// The video surface: mpv decodes into a source texture, librashader runs the preset over it,
// the result is blitted to the widget. Playback state lives in mpv; this wraps it.
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget() override;

    // Empty path means passthrough (no shader). Safe to call before or after GL init.
    void setPreset(const QString &path);
    QString preset() const { return m_presetPath; }

    // The current preset's tunable parameters, sanitized. Empty when there is no shader.
    const QList<ShaderParam> &shaderParams() const { return m_params; }
    // Set one by name. Ignores unknown names and non-finite values, and clamps to the
    // parameter's own bounds, so nothing the UI (or a tampered settings file) sends can push
    // the chain outside what the preset declared.
    void setShaderParam(const QString &name, float value);

    // The resolution the shader chain sees, expressed as a line count because that is what a CRT
    // mask actually cares about; the width follows the source's aspect ratio. 0 means the video's
    // native size. This is the knob that keeps mask geometry stable across sources.
    void setSourceLines(int lines);
    int sourceLines() const { return m_forcedLines; }
    QSize sourceSize() const;

    // How often the chain is redrawn. The shader runs continuously, so this is its clock whether or
    // not anything is playing. 24 / 50 / 60 are the ones the UI offers.
    void setRefreshRate(int hz);
    int refreshRate() const { return m_refreshHz; }

    // False when nothing is loaded, i.e. the chain is being fed the no-signal screen.
    bool hasSignal() const;

    // The video's actual display size, ignoring any source override. This is what the scale
    // buttons size the window against, so 2x means twice the real picture, not twice the
    // shader's working resolution.
    QSize videoSize() const;

    // A frame captured from a hooked game, which takes over from mpv as the picture the chain is
    // fed. Everything downstream is unchanged, which is the point: the game gets the preset, the
    // parameters and the aspect-preserving letterbox the video already gets, so a game rendering at
    // a resolution nothing like the window still lands on screen at the right shape.
    //
    // Frames stop arriving when the game exits or the user turns the hook off; the picture reverts
    // to mpv (or the no-signal screen) on its own once they go stale.
    void setHookFrame(const QImage &frame);
    bool hookActive() const;

    void open(const QStringList &files, bool append = false);
    QList<SubtitleTrack> subtitleTracks() const;
    int64_t currentSubtitleTrack() const;

    // mpv owns the playlist; these just drive it.
    QList<PlaylistEntry> playlist() const;
    void playIndex(int index);
    void movePlaylistEntry(int from, int to);
    void removePlaylistEntry(int index);
    void loadList(const QString &m3uPath, bool autoplay = true);

    // Read back what mpv produced, and what the shader chain produced from it. Probing both
    // is what separates "mpv decoded nothing" from "the chain ate the frame".
    FrameStats probeSource();
    FrameStats probeOutput();

    // Frames actually blitted. Watching this tick while paused is how the selftest proves the
    // chain hasn't stalled.
    size_t painted() const { return m_painted; }

public slots:
    void togglePause();
    void setPaused(bool paused);
    void stepForward();
    void stepBackward();
    void seekTo(double seconds);
    void seekRelative(double seconds);
    void setVolume(int volume);
    void setSubtitleTrack(int64_t id); // -1 disables subtitles

signals:
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void pausedChanged(bool paused);
    void tracksChanged();
    void playlistChanged();
    void titleChanged(const QString &title);
    void shaderFailed(const QString &message);
    void doubleClicked();
    void videoSizeChanged(const QSize &size);
    void shaderParamsChanged();   // the parameter set was rebuilt (preset changed)

protected:
    void initializeGL() override;
    void paintGL() override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private slots:
    void pumpMpvEvents();

private:
    void initializeGLOrThrow();
    void rebuildChain();
    void extractParams(libra_shader_preset_t preset);   // sanitize preset params into m_params
    void releaseChain();
    void resizeTargets(int srcW, int srcH, int outW, int outH);
    void releaseTargets();
    void command(std::initializer_list<const char *> args);
    FrameStats probe(GLuint fbo, int w, int h);

    QString m_presetPath;
    QList<ShaderParam> m_params;
    int m_forcedLines = 0;   // 0 = native

    // The hooked game's latest frame, and how long ago it arrived. Held rather than uploaded on
    // arrival because the upload needs the GL context, which only paintGL is inside.
    QImage m_hookFrame;
    bool m_hookDirty = false;
    QElapsedTimer m_hookAge;

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_mpvGl = nullptr;
    libra_gl_filter_chain_t m_chain = nullptr;
    bool m_glReady = false;

    GLuint m_srcFbo = 0, m_srcTex = 0;   // mpv renders here
    GLuint m_outFbo = 0, m_outTex = 0;   // librashader renders here, we blit from it
    int m_srcW = 0, m_srcH = 0, m_outW = 0, m_outH = 0;

    // Drives the chain continuously: mpv only hands us frames while playing, so without this an
    // animated preset would freeze whenever playback did, and show nothing at all with no file
    // loaded. Runs for the life of the widget.
    QTimer m_paintTimer;
    int m_refreshHz = 60;

    size_t m_frame = 0;
    size_t m_painted = 0;                // frames actually blitted; 0 means nothing ever drew
    GLint m_samplerUnits = 0;            // texture units to clear samplers from after the chain
};
