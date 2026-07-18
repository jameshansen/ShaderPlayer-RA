// Standalone 64-bit hook worker. Opens the proxy's shared textures, runs a librashader D3D11 chain
// over each captured frame, and stamps a heartbeat so the proxy knows to engage. This is the code
// that will later move into ShaderPlayer.exe driven by the live preset; kept standalone for now so
// it iterates against a running game without rebuilding the Qt app.
//
// Preset path is argv[1] (default crt-royale). Run it while the proxied game is running.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#define LIBRA_RUNTIME_D3D11
#include <librashader.h>

#include "../common/hookshared.h"

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

#define HR(call) do { HRESULT _hr = (call); if (FAILED(_hr)) { \
    std::fprintf(stderr, "FAIL %s -> 0x%08lX\n", #call, (unsigned long)_hr); return 2; } } while (0)

static bool acquired(HRESULT hr) { return hr == S_OK || hr == WAIT_ABANDONED; } // dead peer hands us the lock

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered so a redirected log is readable while running
    setvbuf(stderr, nullptr, _IONBF, 0);
    const char *presetPath = argc > 1 ? argv[1]
        : "E:\\Sync\\James Hansen\\ShaderPlayer-RA\\ShaderPlayer-RA\\extern\\slang-shaders\\crt\\crt-royale.slangp";

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    ComPtr<ID3D11Device1> dev1; HR(dev.As(&dev1));

    // Build the chain once. librashader adapts to the per-frame view sizes, so it does not care that
    // we do not yet know the game's resolution here.
    libra_shader_preset_t preset = nullptr;
    if (libra_preset_create(presetPath, &preset)) { std::fprintf(stderr, "preset_create failed\n"); return 3; }
    filter_chain_d3d11_opt_t opt{}; opt.version = LIBRASHADER_CURRENT_VERSION;
    libra_d3d11_filter_chain_t chain = nullptr;
    if (libra_d3d11_filter_chain_create(&preset, dev.Get(), &opt, &chain)) {
        std::fprintf(stderr, "filter_chain_create failed\n"); return 3;   // create consumes preset; no free (see H0.2)
    }
    std::printf("worker: chain built from %s\n", presetPath);

    // Open (or wait for) the control block.
    HANDLE map = nullptr;
    for (int i = 0; i < 100000 && !map; ++i) {
        map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, hook::CTRL_NAME);
        if (!map) Sleep(50);
    }
    if (!map) { std::fprintf(stderr, "no control block\n"); return 4; }
    auto *ctrl = static_cast<hook::Ctrl *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::Ctrl)));
    if (!ctrl) { std::fprintf(stderr, "map view failed\n"); return 4; }

    ctrl->workerSession = GetCurrentProcessId();
    std::printf("worker: attached, session %llu\n", (unsigned long long)ctrl->workerSession);

    ComPtr<ID3D11Texture2D> inTex, outTex;
    ComPtr<ID3D11ShaderResourceView> inSrv;
    ComPtr<ID3D11RenderTargetView> outRtv;
    ComPtr<IDXGIKeyedMutex> inMx, outMx;
    uint32_t curGen = 0;
    size_t frame = 0;

    for (;;) {
        ctrl->workerHeartbeat = GetTickCount64();
        const uint32_t gen = ctrl->texGen;
        if (gen == 0) { Sleep(4); continue; }          // proxy has not made textures yet

        if (gen != curGen) {                            // (re)open shared textures for this generation
            inTex.Reset(); outTex.Reset(); inSrv.Reset(); outRtv.Reset(); inMx.Reset(); outMx.Reset();
            wchar_t nin[64], nout[64];
            hook::inName(nin, 64, gen); hook::outName(nout, 64, gen);
            if (FAILED(dev1->OpenSharedResourceByName(nin, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                        __uuidof(ID3D11Texture2D), (void **)inTex.GetAddressOf())) ||
                FAILED(dev1->OpenSharedResourceByName(nout, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                        __uuidof(ID3D11Texture2D), (void **)outTex.GetAddressOf()))) {
                Sleep(4); continue;                     // proxy may be mid-recreate; retry
            }
            if (FAILED(dev->CreateShaderResourceView(inTex.Get(), nullptr, &inSrv)) ||
                FAILED(dev->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv)) ||
                FAILED(inTex.As(&inMx)) || FAILED(outTex.As(&outMx))) {
                Sleep(4); continue;
            }
            curGen = gen;
            std::printf("worker: opened textures gen %u (%ux%u)\n", gen, ctrl->width, ctrl->height);
        }

        if (!acquired(inMx->AcquireSync(hook::IN_WORKER, 100))) continue;   // no frame ready; loop, heartbeat
        if (!acquired(outMx->AcquireSync(hook::OUT_WORKER, 100))) { inMx->ReleaseSync(hook::IN_GAME); continue; }

        // Stamp either side of the shade: one heavy frame must not look like a dead worker.
        ctrl->workerHeartbeat = GetTickCount64();
        libra_viewport_t vp{ 0.0f, 0.0f, ctrl->width, ctrl->height };
        libra_error_t e = libra_d3d11_filter_chain_frame(&chain, ctx.Get(), frame++, inSrv.Get(), outRtv.Get(),
                                                         &vp, nullptr, nullptr);
        if (e) { std::printf("worker: frame error\n"); libra_error_free(&e); }
        ctx->Flush();
        ctrl->workerHeartbeat = GetTickCount64();
        if ((frame % 300) == 0) std::printf("worker: shading gen %u frame %zu\n", curGen, frame);

        outMx->ReleaseSync(hook::OUT_GAME);             // hand result to game
        inMx->ReleaseSync(hook::IN_GAME);               // let game write the next frame
    }
    // unreachable in v1 (Ctrl-C to stop); chain/map cleaned up by process exit
}
