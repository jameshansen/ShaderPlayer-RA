#include "hookworker.h"

#include <QElapsedTimer>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <librashader.h>   // LIBRA_RUNTIME_D3D11 comes from the build

#include "../hooks/common/hookshared.h"

using Microsoft::WRL::ComPtr;

namespace {

bool acquired(HRESULT hr) { return hr == S_OK || hr == WAIT_ABANDONED; }   // dead peer hands us the lock

}

HookWorker::HookWorker(QObject *parent) : QThread(parent) {}

HookWorker::~HookWorker()
{
    stop();
}

void HookWorker::stop()
{
    if (!isRunning()) return;
    m_stop = true;
    wait(3000);
}

void HookWorker::setState(const QString &preset, const QList<ShaderParam> &params, int sourceLines)
{
    QMutexLocker guard(&m_lock);
    if (preset == m_preset && sourceLines == m_sourceLines && params.size() == m_params.size()) {
        bool same = true;
        for (int i = 0; same && i < params.size(); ++i)
            same = params[i].name == m_params[i].name && params[i].value == m_params[i].value;
        if (same) return;
    }
    m_preset = preset;
    m_params = params;
    m_sourceLines = sourceLines;
    m_dirty = true;
}

void HookWorker::run()
{
    // Ask for a 1ms scheduler tick for as long as this thread runs. This loop polls for new frames,
    // and without it Sleep(1) rounds up to Windows' default 15.6ms -- close enough to a 60Hz frame
    // that the poll and the game beat against each other instead of keeping up with it.
    timeBeginPeriod(1);
    struct EndPeriod { ~EndPeriod() { timeEndPeriod(1); } } endPeriod;

    // Wait for a hooked game before touching the GPU. No hook running means no control block, and
    // there is no reason for a video player to hold a D3D11 device it will never use.
    HANDLE map = nullptr;
    while (!m_stop && !map) {
        map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, hook::CTRL_NAME);
        if (!map) msleep(250);
    }
    if (!map) return;
    auto *ctrl = static_cast<hook::Ctrl *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::Ctrl)));
    if (!ctrl) { CloseHandle(map); return; }

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<ID3D11Device1> dev1;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx)) || FAILED(dev.As(&dev1))) {
        UnmapViewOfFile(ctrl); CloseHandle(map); return;
    }

    ctrl->workerSession = GetCurrentProcessId();

    libra_d3d11_filter_chain_t chain = nullptr;
    QString builtFrom;                  // preset the current chain came from; empty = passthrough

    // Per-generation resources. Captured frames arrive through shared memory; the shaded result goes
    // back through a real GPU texture, because the game has to draw it.
    HANDLE frameMap = nullptr;
    hook::FrameHeader *hdr = nullptr;
    const uint8_t *frameSlots = nullptr;
    uint32_t fw = 0, fh = 0, fpitch = 0, slotBytes = 0;

    ComPtr<ID3D11Texture2D> chainIn, outTex;
    ComPtr<ID3D11ShaderResourceView> inSrv;
    ComPtr<ID3D11RenderTargetView> outRtv;
    ComPtr<IDXGIKeyedMutex> outMx;

    uint32_t curGen = 0, lastSeq = 0;
    size_t frame = 0;
    QElapsedTimer rate, report;
    int delivered = 0, idle = 0, overtaken = 0;

    auto dropGeneration = [&] {
        if (hdr) { UnmapViewOfFile(hdr); hdr = nullptr; }
        if (frameMap) { CloseHandle(frameMap); frameMap = nullptr; }
        frameSlots = nullptr; fw = fh = fpitch = slotBytes = 0;
        chainIn.Reset(); outTex.Reset(); inSrv.Reset(); outRtv.Reset(); outMx.Reset();
        lastSeq = 0;
    };

    while (!m_stop) {
        const bool inApp = m_displayInApp;
        const bool toGame = m_applyToGame;
        ctrl->applyToGame = toGame ? 1 : 0;

        // Pick up whatever the app is showing. Rebuilding the chain here, between frames, is the
        // only point where swapping it out cannot race a libra_..._frame call.
        if (m_dirty) {
            QString preset; QList<ShaderParam> params; int sourceLines;
            {
                QMutexLocker guard(&m_lock);
                preset = m_preset; params = m_params; sourceLines = m_sourceLines;
                m_dirty = false;
            }
            // Only the apply-to-game route needs a chain of our own. Showing in the app means
            // VideoWidget's GL chain does the shading, and building a second D3D11 copy of the same
            // preset would be pure waste -- a heavy preset like crt-royale is not cheap to compile.
            if (preset != builtFrom || (toGame && !chain)) {
                if (chain) { libra_d3d11_filter_chain_free(&chain); chain = nullptr; }
                builtFrom.clear();
                if (toGame && !preset.isEmpty()) {
                    libra_shader_preset_t sp = nullptr;
                    if (!libra_preset_create(preset.toUtf8().constData(), &sp)) {
                        filter_chain_d3d11_opt_t opt{}; opt.version = LIBRASHADER_CURRENT_VERSION;
                        // create consumes the preset; freeing it afterwards panics inside librashader (H0.2).
                        if (!libra_d3d11_filter_chain_create(&sp, dev.Get(), &opt, &chain))
                            builtFrom = preset;
                        else
                            chain = nullptr;
                    }
                }
            }
            if (chain) {
                for (const ShaderParam &p : params) {
                    libra_error_t e = libra_d3d11_filter_chain_set_param(&chain, p.name.toUtf8().constData(), p.value);
                    if (e) libra_error_free(&e);   // preset without that param; nothing to do
                }
            }
            ctrl->srcLines = uint32_t(sourceLines > 0 ? sourceLines : 0);
        }

        // Nothing to do for: both destinations off, or the only one on is apply-to-game and there is
        // no shader to apply. Stopping the heartbeat is what puts the hook back into passthrough,
        // so this is also how the game gets left alone.
        if (!inApp && !chain) { m_shading = false; msleep(100); continue; }

        ctrl->workerHeartbeat = GetTickCount64();
        const uint32_t gen = ctrl->texGen;
        if (gen == 0) { m_shading = false; msleep(4); continue; }   // hook has not published yet

        if (gen != curGen) {
            dropGeneration();
            wchar_t nframe[64];
            hook::frameName(nframe, 64, gen);
            frameMap = OpenFileMappingW(FILE_MAP_READ, FALSE, nframe);
            if (frameMap)
                hdr = static_cast<hook::FrameHeader *>(MapViewOfFile(frameMap, FILE_MAP_READ, 0, 0, 0));
            if (!hdr) { dropGeneration(); msleep(4); continue; }    // hook may be mid-recreate; retry
            fw = hdr->width; fh = hdr->height; fpitch = hdr->pitch; slotBytes = hdr->slotBytes;
            if (!fw || !fh) { dropGeneration(); msleep(4); continue; }
            frameSlots = reinterpret_cast<const uint8_t *>(hdr) + sizeof(hook::FrameHeader);

            // The shaded-result texture only exists while apply-to-game is on, and so does the
            // chain's input, which is filled from shared memory each frame.
            if (toGame) {
                wchar_t nout[64];
                hook::outName(nout, 64, gen);
                if (SUCCEEDED(dev1->OpenSharedResourceByName(nout, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                              __uuidof(ID3D11Texture2D), (void **)outTex.GetAddressOf()))) {
                    dev->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv);
                    outTex.As(&outMx);
                }
                D3D11_TEXTURE2D_DESC d{};
                d.Width = fw; d.Height = fh; d.MipLevels = 1; d.ArraySize = 1;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &chainIn)))
                    dev->CreateShaderResourceView(chainIn.Get(), nullptr, &inSrv);
            }
            curGen = gen;
        }

        // Read side of the seqlock. Nothing here can block the game: the worst case is noticing we
        // were overtaken mid-copy, and then we simply take the next frame instead.
        const uint32_t s = hdr->seq;
        if (s == lastSeq) {                             // no new frame yet
            ++idle;
            msleep(1);
            continue;
        }
        const uint8_t *src = frameSlots + size_t(s & 1) * slotBytes;

        QImage img(int(fw), int(fh), QImage::Format_RGB32);
        for (uint32_t y = 0; y < fh; ++y)
            memcpy(img.scanLine(int(y)), src + size_t(y) * fpitch, fpitch);
        MemoryBarrier();
        if (hdr->seq != s) {                            // writer lapped us; this copy may be torn
            ++overtaken;
            lastSeq = hdr->seq;
            continue;
        }
        lastSeq = s;

        if (toGame && chain && inSrv && outRtv && outMx) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(chainIn.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                for (uint32_t y = 0; y < fh; ++y)
                    memcpy(static_cast<uint8_t *>(m.pData) + size_t(y) * m.RowPitch,
                           img.constScanLine(int(y)), fpitch);
                ctx->Unmap(chainIn.Get(), 0);
            }
            // Zero timeout: if the game has not taken the last shaded frame yet, skip this one
            // rather than stall. The game's own acquire is non-blocking for the same reason.
            if (acquired(outMx->AcquireSync(hook::OUT_WORKER, 0))) {
                libra_viewport_t vp{ 0.0f, 0.0f, ctrl->width, ctrl->height };
                libra_error_t e = libra_d3d11_filter_chain_frame(&chain, ctx.Get(), frame++, inSrv.Get(),
                                                                 outRtv.Get(), &vp, nullptr, nullptr);
                if (e) libra_error_free(&e);
                ctx->Flush();
                ctrl->workerHeartbeat = GetTickCount64();
                outMx->ReleaseSync(hook::OUT_GAME);     // hand result to game
            }
        }

        m_shading = true;
        if (inApp)
            emit frameReady(img);

        if (!rate.isValid()) { rate.start(); report.start(); }
        if (++delivered >= 30) {                        // often enough to be live, rarely enough to be free
            m_fps = delivered * 1000.0 / double(qMax(qint64(1), rate.restart()));
            delivered = 0;
        }
        if (report.elapsed() >= 5000) {
            qInfo("HOOKWORKER %.1f fps, %d idle polls, %d overtaken", m_fps, idle, overtaken);
            idle = overtaken = 0;
            report.restart();
        }
    }

    m_shading = false;
    m_fps = 0.0;
    ctrl->workerSession = 0;
    ctrl->srcLines = 0;                                 // do not leave a stale size for the next worker
    dropGeneration();
    if (chain) libra_d3d11_filter_chain_free(&chain);
    UnmapViewOfFile(ctrl);
    CloseHandle(map);
}
