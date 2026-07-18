#include "hookworker.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
    // Wait for a hooked game before touching the GPU. No proxy running means no control block, and
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

    ComPtr<ID3D11Texture2D> inTex, outTex;
    ComPtr<ID3D11ShaderResourceView> inSrv;
    ComPtr<ID3D11RenderTargetView> outRtv;
    ComPtr<IDXGIKeyedMutex> inMx, outMx;
    uint32_t curGen = 0;
    size_t frame = 0;

    while (!m_stop) {
        // Pick up whatever the app is showing. Rebuilding the chain here, between frames, is the
        // only point where swapping it out cannot race a libra_..._frame call.
        if (m_dirty) {
            QString preset; QList<ShaderParam> params; int sourceLines;
            {
                QMutexLocker guard(&m_lock);
                preset = m_preset; params = m_params; sourceLines = m_sourceLines;
                m_dirty = false;
            }
            if (preset != builtFrom) {
                if (chain) { libra_d3d11_filter_chain_free(&chain); chain = nullptr; }
                builtFrom.clear();
                if (!preset.isEmpty()) {
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

        // No shader selected means passthrough: stop the heartbeat and the proxy leaves the game alone.
        if (!chain) { m_shading = false; msleep(100); continue; }

        ctrl->workerHeartbeat = GetTickCount64();
        const uint32_t gen = ctrl->texGen;
        if (gen == 0) { m_shading = false; msleep(4); continue; }   // proxy has not made textures yet

        if (gen != curGen) {                            // (re)open shared textures for this generation
            inTex.Reset(); outTex.Reset(); inSrv.Reset(); outRtv.Reset(); inMx.Reset(); outMx.Reset();
            wchar_t nin[64], nout[64];
            hook::inName(nin, 64, gen); hook::outName(nout, 64, gen);
            if (FAILED(dev1->OpenSharedResourceByName(nin, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                        __uuidof(ID3D11Texture2D), (void **)inTex.GetAddressOf())) ||
                FAILED(dev1->OpenSharedResourceByName(nout, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                        __uuidof(ID3D11Texture2D), (void **)outTex.GetAddressOf()))) {
                msleep(4); continue;                    // proxy may be mid-recreate; retry
            }
            if (FAILED(dev->CreateShaderResourceView(inTex.Get(), nullptr, &inSrv)) ||
                FAILED(dev->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv)) ||
                FAILED(inTex.As(&inMx)) || FAILED(outTex.As(&outMx))) {
                msleep(4); continue;
            }
            curGen = gen;
        }

        if (!acquired(inMx->AcquireSync(hook::IN_WORKER, 100))) continue;   // no frame ready; loop, heartbeat
        if (!acquired(outMx->AcquireSync(hook::OUT_WORKER, 100))) { inMx->ReleaseSync(hook::IN_GAME); continue; }

        // Stamp either side of the shade: one heavy frame must not look like a dead worker.
        ctrl->workerHeartbeat = GetTickCount64();
        libra_viewport_t vp{ 0.0f, 0.0f, ctrl->width, ctrl->height };
        libra_error_t e = libra_d3d11_filter_chain_frame(&chain, ctx.Get(), frame++, inSrv.Get(), outRtv.Get(),
                                                         &vp, nullptr, nullptr);
        if (e) libra_error_free(&e);
        ctx->Flush();
        ctrl->workerHeartbeat = GetTickCount64();
        m_shading = true;

        outMx->ReleaseSync(hook::OUT_GAME);             // hand result to game
        inMx->ReleaseSync(hook::IN_GAME);               // let game write the next frame
    }

    m_shading = false;
    ctrl->workerSession = 0;
    ctrl->srcLines = 0;                                 // do not leave a stale size for the next worker
    if (chain) libra_d3d11_filter_chain_free(&chain);
    UnmapViewOfFile(ctrl);
    CloseHandle(map);
}
