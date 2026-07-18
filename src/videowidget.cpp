#include "videowidget.h"

#include "sourcelines.h"

#include <QApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QRect>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

// mpv resolves its own GL entry points through this.
void *getProcAddress(void *ctx, const char *name)
{
    Q_UNUSED(ctx);
    QOpenGLContext *gl = QOpenGLContext::currentContext();
    return gl ? reinterpret_cast<void *>(gl->getProcAddress(name)) : nullptr;
}

// librashader wants the same thing with a different signature.
const void *libraLoader(const char *name)
{
    QOpenGLContext *gl = QOpenGLContext::currentContext();
    return gl ? reinterpret_cast<const void *>(gl->getProcAddress(name)) : nullptr;
}

// libra_error_t is null on success; anything else carries a message worth reading.
std::string libraMessage(libra_error_t err, const char *what)
{
    std::string msg = std::string("librashader: ") + what;
    char *detail = nullptr;
    if (libra_error_write(err, &detail) == 0 && detail) {
        msg += ": ";
        msg += detail;
        libra_error_free_string(&detail);
    }
    return msg;
}

void checkLibra(libra_error_t err, const char *what)
{
    if (!err)
        return;
    std::string msg = libraMessage(err, what);
    libra_error_free(&err);
    throw std::runtime_error(msg);
}

// Scale src to fit inside outW x outH without distorting it, centred. The leftover is black bars.
// Centring is what makes the FLIP_Y convention irrelevant here: the margins are symmetric.
QRect fitRect(const QSize &src, int outW, int outH)
{
    const double aspect = double(src.width()) / double(src.height());
    int w = outW, h = int(qRound(outW / aspect));
    if (h > outH) {
        h = outH;
        w = int(qRound(outH * aspect));
    }
    return QRect((outW - w) / 2, (outH - h) / 2, w, h);
}

QString mpvString(mpv_handle *mpv, const QString &prop)
{
    char *v = nullptr;
    if (mpv_get_property(mpv, prop.toUtf8().constData(), MPV_FORMAT_STRING, &v) < 0 || !v)
        return QString();
    QString out = QString::fromUtf8(v);
    mpv_free(v);
    return out;
}

} // namespace

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        throw std::runtime_error("mpv_create failed");

    // vo=libmpv routes video into the render API instead of mpv's own window.
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    // Copy-back, not direct. render_gl.h: "Windows: ANGLE is required" for hardware decoding
    // through the GL render API, and Qt 6 dropped ANGLE, so we hand mpv a desktop WGL context
    // that the d3d11va interop cannot use. Plain auto-safe picks d3d11va regardless and dies
    // inside mpv's own decode thread on any file the GPU accepts. The copy-back modes need no
    // GL interop at all: the GPU still decodes, we just pay a frame copy back to RAM.
    mpv_set_option_string(m_mpv, "hwdec", "auto-copy-safe");
    // Without idle=yes mpv tears down once the playlist ends and the widget goes dead.
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "msg-level", "all=warn");

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("mpv_initialize failed");

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "track-list/count", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "dheight", MPV_FORMAT_INT64);
    // MPV_FORMAT_NONE fires on any playlist change, including a reorder that leaves the count alone.
    mpv_observe_property(m_mpv, 0, "playlist", MPV_FORMAT_NONE);

    // ponytail: fixed 60Hz rather than chasing the real refresh rate. Only runs while paused.
    m_paintTimer.setInterval(16);
    connect(&m_paintTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));

    mpv_request_log_messages(m_mpv, "info");
    mpv_set_wakeup_callback(m_mpv, [](void *ctx) {
        auto *self = static_cast<VideoWidget *>(ctx);
        QMetaObject::invokeMethod(self, &VideoWidget::pumpMpvEvents, Qt::QueuedConnection);
    }, this);
}

VideoWidget::~VideoWidget()
{
    // mpv's render context and the filter chain both need the GL context current to release.
    makeCurrent();
    if (m_mpvGl)
        mpv_render_context_free(m_mpvGl);
    releaseChain();
    releaseTargets();
    doneCurrent();

    if (m_mpv)
        mpv_terminate_destroy(m_mpv);
}

void VideoWidget::command(std::initializer_list<const char *> args)
{
    if (!m_mpv)
        return;
    std::vector<const char *> argv(args);
    argv.push_back(nullptr);
    mpv_command_async(m_mpv, 0, argv.data());
}

void VideoWidget::initializeGL()
{
    // Qt cannot carry an exception out of initializeGL through the event loop; it would
    // std::terminate with no message. Catch here and surface it instead.
    try {
        initializeGLOrThrow();
    } catch (const std::exception &e) {
        qCritical("ShaderPlayer init failed: %s", e.what());
        QMetaObject::invokeMethod(this, [msg = QString::fromUtf8(e.what())] {
            QMessageBox::critical(nullptr, "ShaderPlayer-RA", msg);
            qApp->quit();
        }, Qt::QueuedConnection);
    }
}

void VideoWidget::initializeGLOrThrow()
{
    initializeOpenGLFunctions();

    mpv_opengl_init_params glInit{ getProcAddress, nullptr };
    // ponytail: no ADVANCED_CONTROL. It makes mpv defer rendering to its own schedule and
    // requires driving mpv_render_context_update(); plain mode just renders when we ask.
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&m_mpvGl, m_mpv, params) < 0)
        throw std::runtime_error("mpv_render_context_create failed");

    // Fires on mpv's thread; hop to the GUI thread to repaint.
    mpv_render_context_set_update_callback(m_mpvGl, [](void *ctx) {
        auto *self = static_cast<VideoWidget *>(ctx);
        QMetaObject::invokeMethod(self, qOverload<>(&QOpenGLWidget::update), Qt::QueuedConnection);
    }, this);

    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &m_samplerUnits);

    m_glReady = true;
    rebuildChain();
}

void VideoWidget::releaseChain()
{
    if (m_chain) {
        libra_gl_filter_chain_free(&m_chain);
        m_chain = nullptr;
    }
}

// Pull the preset's tunables out and sanitize them. Everything downstream (the panel, the
// settings file round-trip) trusts this list, so this is the one place that has to be paranoid
// about a hand-crafted preset: absurd counts, NaN/Inf bounds, inverted ranges, junk names.
void VideoWidget::extractParams(libra_shader_preset_t preset)
{
    m_params.clear();

    libra_preset_param_list_t list{};
    libra_error_t err = libra_preset_get_runtime_params(&preset, &list);
    if (err) {
        libra_error_free(&err);
        return;
    }

    // Hard cap: a preset with a pathological parameter count can't make us build a runaway
    // number of widgets or churn through unbounded work here.
    constexpr uint64_t kMaxParams = 512;
    const uint64_t n = list.parameters ? qMin(list.length, kMaxParams) : 0;

    for (uint64_t i = 0; i < n; ++i) {
        const libra_preset_param_t &p = list.parameters[i];

        const QString name = p.name ? QString::fromUtf8(p.name) : QString();
        if (name.isEmpty() || name.size() > 256)
            continue; // no usable id, or a name long enough to be a nuisance as a settings key
        if (!std::isfinite(p.minimum) || !std::isfinite(p.maximum)
            || !std::isfinite(p.initial) || !std::isfinite(p.step))
            continue; // NaN/Inf bounds can't map to any sane control
        if (p.minimum > p.maximum)
            continue; // inverted range: refuse rather than guess the author's intent

        ShaderParam sp;
        sp.name = name;
        sp.description = p.description ? QString::fromUtf8(p.description).left(200) : QString();
        sp.minimum = p.minimum;
        sp.maximum = p.maximum;
        sp.step = (std::isfinite(p.step) && p.step > 0.0f) ? p.step : 0.0f;
        sp.initial = std::clamp(p.initial, p.minimum, p.maximum);
        sp.value = sp.initial;
        m_params.append(sp);
    }

    libra_preset_free_runtime_params(list);
}

void VideoWidget::rebuildChain()
{
    releaseChain();
    m_params.clear();
    if (m_presetPath.isEmpty()) {
        emit shaderParamsChanged(); // passthrough: the panel should empty itself
        return;
    }

    try {
        libra_shader_preset_t preset = nullptr;
        checkLibra(libra_preset_create(m_presetPath.toUtf8().constData(), &preset), "preset_create");

        // Must happen before filter_chain_create, which invalidates the preset handle.
        extractParams(preset);

        filter_chain_gl_opt_t opt{};
        opt.version = LIBRASHADER_CURRENT_VERSION;
        opt.glsl_version = 460;
        opt.use_dsa = true;
        opt.force_no_mipmaps = false;
        opt.disable_cache = false;

        libra_error_t err = libra_gl_filter_chain_create(&preset, libraLoader, &opt, &m_chain);
        libra_preset_free(&preset);
        checkLibra(err, "filter_chain_create");
    } catch (const std::exception &e) {
        // A bad preset falls back to passthrough rather than taking the app down.
        releaseChain();
        m_params.clear();
        m_presetPath.clear();
        qCritical("%s", e.what());
        emit shaderFailed(QString::fromUtf8(e.what()));
    }

    emit shaderParamsChanged();
}

void VideoWidget::setShaderParam(const QString &name, float value)
{
    if (!m_chain || !std::isfinite(value))
        return;
    for (ShaderParam &p : m_params) {
        if (p.name != name)
            continue;
        p.value = std::clamp(value, p.minimum, p.maximum);
        libra_error_t err = libra_gl_filter_chain_set_param(
            &m_chain, name.toUtf8().constData(), p.value);
        if (err)
            libra_error_free(&err); // e.g. name vanished between builds; not fatal
        update();
        return;
    }
}

void VideoWidget::setPreset(const QString &path)
{
    if (path == m_presetPath)
        return;
    m_presetPath = path;
    if (!m_glReady)
        return; // picked up by initializeGL
    makeCurrent();
    rebuildChain();
    doneCurrent();
    update();
}

void VideoWidget::setSourceLines(int lines)
{
    if (lines == m_forcedLines)
        return;
    m_forcedLines = lines;
    update();
}

// A line count plus the source's own aspect, not a fixed width and height. Pinning 480p on a 16:9
// video has to mean 854x480 and on a 4:3 video 640x480, otherwise the picture gets squeezed to fit
// a shape it never had. The width rule itself lives in sourcelines.h, shared with the hook DLL.
QSize VideoWidget::sourceSize() const
{
    const QSize full = videoSize();
    const int w = widthForLines(full.width(), full.height(), m_forcedLines);
    return w > 0 ? QSize(w, m_forcedLines) : full;
}

QSize VideoWidget::videoSize() const
{
    int64_t w = 0, h = 0;
    mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &w);
    mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &h);
    return QSize(int(w), int(h));
}

void VideoWidget::open(const QStringList &files, bool append)
{
    if (!append)
        setPaused(false); // opening a file is an explicit request to play it
    bool first = true;
    for (const QString &f : files) {
        const QByteArray path = f.toUtf8();
        command({ "loadfile", path.constData(), (append || !first) ? "append-play" : "replace" });
        first = false;
    }
}

QList<PlaylistEntry> VideoWidget::playlist() const
{
    int64_t count = 0;
    mpv_get_property(m_mpv, "playlist-count", MPV_FORMAT_INT64, &count);

    QList<PlaylistEntry> out;
    for (int64_t i = 0; i < count; ++i) {
        PlaylistEntry e;
        e.filename = mpvString(m_mpv, QString("playlist/%1/filename").arg(i));
        const QString title = mpvString(m_mpv, QString("playlist/%1/title").arg(i));
        e.title = title.isEmpty() ? QFileInfo(e.filename).fileName() : title;
        if (e.title.isEmpty())
            e.title = e.filename;
        e.current = mpvString(m_mpv, QString("playlist/%1/current").arg(i)) == QLatin1String("yes");
        out.append(e);
    }
    return out;
}

void VideoWidget::playIndex(int index)
{
    setPaused(false); // double-clicking an entry means play it, even after a paused restore
    const QByteArray v = QByteArray::number(index);
    command({ "playlist-play-index", v.constData() });
}

void VideoWidget::movePlaylistEntry(int from, int to)
{
    // mpv's playlist-move puts `from` in front of `to`, which is the same convention as
    // Qt's rowsMoved destination, so the indices pass straight through.
    const QByteArray f = QByteArray::number(from), t = QByteArray::number(to);
    command({ "playlist-move", f.constData(), t.constData() });
}

void VideoWidget::removePlaylistEntry(int index)
{
    const QByteArray v = QByteArray::number(index);
    command({ "playlist-remove", v.constData() });
}

void VideoWidget::loadList(const QString &m3uPath, bool autoplay)
{
    // Restoring at startup should bring the playlist back without launching into playback.
    // loadlist always makes entry 0 current and plays it, so pre-set pause to hold on frame 0;
    // any explicit play action (open, playIndex, the play button) clears it again.
    if (!autoplay)
        setPaused(true);
    const QByteArray p = m3uPath.toUtf8();
    command({ "loadlist", p.constData(), "replace" });
}

QList<SubtitleTrack> VideoWidget::subtitleTracks() const
{
    // Reading track-list through property paths avoids parsing an mpv_node tree.
    int64_t count = 0;
    mpv_get_property(m_mpv, "track-list/count", MPV_FORMAT_INT64, &count);

    QList<SubtitleTrack> tracks;
    for (int64_t i = 0; i < count; ++i) {
        if (mpvString(m_mpv, QString("track-list/%1/type").arg(i)) != QLatin1String("sub"))
            continue;

        SubtitleTrack t;
        t.id = mpvString(m_mpv, QString("track-list/%1/id").arg(i)).toLongLong();
        const QString lang = mpvString(m_mpv, QString("track-list/%1/lang").arg(i));
        const QString title = mpvString(m_mpv, QString("track-list/%1/title").arg(i));
        t.label = title.isEmpty() ? QString("Track %1").arg(t.id) : title;
        if (!lang.isEmpty())
            t.label += QString(" (%1)").arg(lang);
        tracks.append(t);
    }
    return tracks;
}

int64_t VideoWidget::currentSubtitleTrack() const
{
    // "auto" means mpv has not picked a track yet; it is not track 0 (ids start at 1), and
    // reading it as one left every entry in the Subtitles menu unchecked.
    const QString sid = mpvString(m_mpv, QStringLiteral("sid"));
    return (sid.isEmpty() || sid == QLatin1String("no") || sid == QLatin1String("auto"))
               ? -1 : sid.toLongLong();
}

void VideoWidget::setSubtitleTrack(int64_t id)
{
    const QByteArray v = (id < 0) ? QByteArray("no") : QByteArray::number(qlonglong(id));
    mpv_set_property_string(m_mpv, "sid", v.constData());
}

void VideoWidget::togglePause()   { command({ "cycle", "pause" }); }
void VideoWidget::stepForward()   { command({ "playlist-next", "weak" }); }
void VideoWidget::stepBackward()  { command({ "playlist-prev", "weak" }); }

void VideoWidget::setPaused(bool paused)
{
    int flag = paused ? 1 : 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void VideoWidget::seekTo(double seconds)
{
    const QByteArray v = QByteArray::number(seconds, 'f', 3);
    command({ "seek", v.constData(), "absolute" });
}

void VideoWidget::seekRelative(double seconds)
{
    const QByteArray v = QByteArray::number(seconds, 'f', 3);
    command({ "seek", v.constData(), "relative" });
}

void VideoWidget::setVolume(int volume)
{
    double v = volume;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    Q_UNUSED(e);
    emit doubleClicked();
}

void VideoWidget::pumpMpvEvents()
{
    while (m_mpv) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;

        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *p = static_cast<mpv_event_property *>(ev->data);
            const QLatin1String name(p->name);
            if (name == QLatin1String("playlist")) {
                emit playlistChanged();
                break;
            }
            if (!p->data) {
                if (name == QLatin1String("track-list/count"))
                    emit tracksChanged();
                break;
            }
            if (name == QLatin1String("time-pos"))
                emit positionChanged(*static_cast<double *>(p->data));
            else if (name == QLatin1String("duration"))
                emit durationChanged(*static_cast<double *>(p->data));
            else if (name == QLatin1String("pause")) {
                const bool paused = *static_cast<int *>(p->data) != 0;
                if (paused)
                    m_paintTimer.start();
                else
                    m_paintTimer.stop(); // mpv's update callback takes over again
                emit pausedChanged(paused);
            }
            else if (name == QLatin1String("media-title"))
                emit titleChanged(QString::fromUtf8(*static_cast<char **>(p->data)));
            else if (name == QLatin1String("track-list/count"))
                emit tracksChanged();
            else if (name == QLatin1String("dwidth") || name == QLatin1String("dheight"))
                emit videoSizeChanged(videoSize());
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *m = static_cast<mpv_event_log_message *>(ev->data);
            qDebug("[mpv/%s] %s", m->prefix, QByteArray(m->text).trimmed().constData());
            break;
        }
        case MPV_EVENT_END_FILE: {
            auto *e = static_cast<mpv_event_end_file *>(ev->data);
            if (e->reason == MPV_END_FILE_REASON_ERROR)
                qCritical("[mpv] load failed: %s", mpv_error_string(e->error));
            break;
        }
        default:
            break;
        }
    }
}

QString FrameStats::toString() const
{
    if (!valid)
        return QStringLiteral("<no frame>");
    return QString::asprintf("%dx%d nonblack=%.3f mean=(%d,%d,%d)",
                             width, height, nonBlack, meanR, meanG, meanB);
}

FrameStats VideoWidget::probe(GLuint fbo, int w, int h)
{
    FrameStats st;
    if (!fbo || w <= 0 || h <= 0 || !m_glReady)
        return st;

    makeCurrent();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    std::vector<unsigned char> px(size_t(w) * size_t(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    doneCurrent();

    quint64 sumR = 0, sumG = 0, sumB = 0, lit = 0;
    quint64 hash = 1469598103934665603ULL; // FNV-1a
    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        sumR += r; sumG += g; sumB += b;
        if (int(r) + int(g) + int(b) > 24) // ignore near-black noise
            ++lit;
        hash = (hash ^ r) * 1099511628211ULL;
        hash = (hash ^ g) * 1099511628211ULL;
        hash = (hash ^ b) * 1099511628211ULL;
    }

    st.valid = true;
    st.width = w; st.height = h;
    st.nonBlack = double(lit) / double(n);
    st.meanR = int(sumR / n); st.meanG = int(sumG / n); st.meanB = int(sumB / n);
    st.hash = hash;
    return st;
}

FrameStats VideoWidget::probeSource()
{
    return m_painted ? probe(m_srcFbo, m_srcW, m_srcH) : FrameStats{};
}

FrameStats VideoWidget::probeOutput()
{
    if (!m_painted)
        return {};
    // With no chain the source is the output.
    return m_chain ? probe(m_outFbo, m_outW, m_outH) : probe(m_srcFbo, m_srcW, m_srcH);
}

void VideoWidget::releaseTargets()
{
    if (m_srcFbo) { glDeleteFramebuffers(1, &m_srcFbo); m_srcFbo = 0; }
    if (m_outFbo) { glDeleteFramebuffers(1, &m_outFbo); m_outFbo = 0; }
    if (m_srcTex) { glDeleteTextures(1, &m_srcTex); m_srcTex = 0; }
    if (m_outTex) { glDeleteTextures(1, &m_outTex); m_outTex = 0; }
    m_srcW = m_srcH = m_outW = m_outH = 0;
}

void VideoWidget::resizeTargets(int srcW, int srcH, int outW, int outH)
{
    if (srcW == m_srcW && srcH == m_srcH && outW == m_outW && outH == m_outH)
        return;

    releaseTargets();

    auto makeTarget = [&](GLuint &fbo, GLuint &tex, int w, int h) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // librashader samples through its own sampler objects, which may use mipmap min
        // filters. A single-level texture with default MAX_LEVEL=1000 is incomplete under
        // such a sampler and reads as opaque black; clamping MAX_LEVEL keeps it complete.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    };

    makeTarget(m_srcFbo, m_srcTex, srcW, srcH);
    makeTarget(m_outFbo, m_outTex, outW, outH);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());

    m_srcW = srcW; m_srcH = srcH;
    m_outW = outW; m_outH = outH;
}

void VideoWidget::paintGL()
{
    if (!m_mpvGl)
        return;

    const QSize src = sourceSize();
    const qreal dpr = devicePixelRatioF();
    const int outW = int(width() * dpr), outH = int(height() * dpr);

    // Nothing loaded (or mpv is idle): just show black.
    if (src.isEmpty() || outW <= 0 || outH <= 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    // The chain renders at the letterboxed size, not the widget size, so the shader never sees
    // a stretched frame and the bars stay outside it.
    const QRect dst = fitRect(src, outW, outH);
    resizeTargets(src.width(), src.height(), dst.width(), dst.height());

    // 1. mpv decodes and renders the frame (including subtitles) into m_srcFbo.
    mpv_opengl_fbo fbo{ int(m_srcFbo), src.width(), src.height(), GL_RGBA8 };
    // mpv's FBO origin convention comes out inverted relative to how we blit into the widget.
    int flip = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flip },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    mpv_render_context_render(m_mpvGl, params);

    // 2. librashader runs the preset from m_srcTex into m_outTex.
    GLuint readFbo = m_srcFbo;
    int readW = src.width(), readH = src.height();
    if (m_chain) {
        libra_image_gl_t in{ m_srcTex, GL_RGBA8, uint32_t(src.width()), uint32_t(src.height()) };
        libra_image_gl_t out{ m_outTex, GL_RGBA8, uint32_t(dst.width()), uint32_t(dst.height()) };
        libra_viewport_t vp{ 0.0f, 0.0f, uint32_t(dst.width()), uint32_t(dst.height()) };
        libra_error_t err = libra_gl_filter_chain_frame(&m_chain, m_frame++, in, out, &vp, nullptr, nullptr);
        if (err) {
            static bool once = false;
            if (!once) { once = true; qCritical("%s", libraMessage(err, "filter_chain_frame").c_str()); }
            libra_error_free(&err);
            return;
        }
        // librashader samples through its own sampler objects and leaves them bound. mpv's
        // render API documents that it expects GL state at standard defaults, and a stray
        // sampler overrides the texture parameters on mpv's own textures -- which makes them
        // mipmap-incomplete and renders the next frame black. Only bites when mpv has to
        // scale (i.e. source override != native size), since the unscaled path never samples.
        for (GLint unit = 0; unit < m_samplerUnits; ++unit)
            glBindSampler(unit, 0);

        readFbo = m_outFbo;
        readW = dst.width(); readH = dst.height();
    }

    // 3. Blit the result onto the widget, centred. Clearing first is what paints the bars.
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, defaultFramebufferObject());
    glBlitFramebuffer(0, 0, readW, readH,
                      dst.x(), dst.y(), dst.x() + dst.width(), dst.y() + dst.height(),
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    ++m_painted;
}
