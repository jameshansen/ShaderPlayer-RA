// Proxy d3d9.dll: the in-game half of the Hooks feature.
//
// Per frame, when a worker (ShaderPlayer) is alive:
//   1. capture the finished D3D9 backbuffer to system memory (GetRenderTargetData),
//   2. upload it into a shared D3D11 texture and hand it to the worker (keyed mutex),
//   3. take back the shaded result and paint it over the backbuffer as a fullscreen quad,
//   4. call the real Present.
// When no worker is alive, it just calls the real Present, so the game runs stock. The proxy owns
// all the durable resources; the worker attaches and detaches. See hooks/common/hookshared.h.
//
// Legacy D3D9 has no shareable surfaces, so capture and write-back both round-trip through system
// memory. Correct first; the D3D9Ex GPU-only path is the optimisation if frame rate needs it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../common/hookshared.h"

using Microsoft::WRL::ComPtr;

static void logf(const char *fmt, ...)
{
    static FILE *f = nullptr;
    if (!f) f = fopen("C:\\Users\\Home\\AppData\\Local\\Temp\\sp_hook.log", "a");
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fflush(f);
}

// ---- hook plumbing ---------------------------------------------------------
typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

static PFN_CreateDevice g_realCreateDevice = nullptr;
static PFN_Present g_realPresent = nullptr;
static PFN_Reset g_realReset = nullptr;
static bool g_devHooked = false;

static void *hookVtbl(void *obj, int index, void *hook)
{
    void **vtbl = *reinterpret_cast<void ***>(obj);
    DWORD old = 0;
    VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    void *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    return orig;
}

// ---- shared state ----------------------------------------------------------
static UINT g_bbW = 0, g_bbH = 0;
static D3DFORMAT g_bbFmt = D3DFMT_X8R8G8B8;
static IDirect3DDevice9 *g_dev = nullptr;   // the device we last saw present; a change means rebuild

static ComPtr<ID3D11Device> g_d11;
static ComPtr<ID3D11DeviceContext> g_d11ctx;
static HANDLE g_map = nullptr;
static hook::Ctrl *g_ctrl = nullptr;

static ComPtr<ID3D11Texture2D> g_inTex, g_outTex, g_outStage;
static ComPtr<IDXGIKeyedMutex> g_inMx, g_outMx;
static HANDLE g_inShare = nullptr, g_outShare = nullptr;
static uint32_t g_gen = 0;
static UINT g_capW = 0, g_capH = 0;   // what we capture at: backbuffer size, or the worker's src size

// D3D9 write-back resources (recreated on Reset because they live in DEFAULT/SYSTEMMEM pools).
static ComPtr<IDirect3DSurface9> g_capSurf;   // SYSTEMMEM, GetRenderTargetData target, capture size
static ComPtr<IDirect3DSurface9> g_capRT;      // DEFAULT RT, only when downscaling (StretchRect target)
static ComPtr<IDirect3DTexture9> g_d9sys;      // SYSTEMMEM, lockable staging for the shaded result
static ComPtr<IDirect3DTexture9> g_d9def;      // DEFAULT, what the quad samples
static ComPtr<IDirect3DStateBlock9> g_sb;

static void releaseD9SizeResources()
{
    g_capSurf.Reset(); g_capRT.Reset(); g_d9sys.Reset(); g_d9def.Reset(); g_sb.Reset();
}

static void ensureD11()
{
    if (g_d11) return;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                      &g_d11, nullptr, &g_d11ctx);
    // Seed the generation from our pid so a restarted game (e.g. Sonic Mania's fullscreen toggle
    // spawns a fresh process) never reuses the previous process's texture names. Without this the
    // worker keeps its stale textures and the new game sits in passthrough.
    g_gen = GetCurrentProcessId();
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(hook::Ctrl), hook::CTRL_NAME);
    if (g_map) {
        g_ctrl = static_cast<hook::Ctrl *>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::Ctrl)));
        // The mapping may have outlived the previous game (the worker holds it open), so reset the
        // proxy-owned fields. Leave the worker's heartbeat/session alone; freshness gates liveness.
        if (g_ctrl) { g_ctrl->magic = hook::MAGIC; g_ctrl->version = 1;
                      g_ctrl->texGen = 0; g_ctrl->width = 0; g_ctrl->height = 0; }
    }
    logf("proxy: D3D11 %p, ctrl %p, gen seed %u", g_d11.Get(), g_ctrl, g_gen);
}

// (Re)create the shared textures and publish the new generation. The input is the captured frame, so
// it carries the source resolution; the output is what we paint back, so it stays backbuffer size.
static void ensureShared(UINT capW, UINT capH, UINT w, UINT h)
{
    static UINT lastCapW = 0, lastCapH = 0;
    if (g_inTex && g_ctrl && g_ctrl->width == w && g_ctrl->height == h && lastCapW == capW && lastCapH == capH) return;
    lastCapW = capW; lastCapH = capH;

    g_inTex.Reset(); g_outTex.Reset(); g_outStage.Reset(); g_inMx.Reset(); g_outMx.Reset();
    if (g_inShare) { CloseHandle(g_inShare); g_inShare = nullptr; }
    if (g_outShare) { CloseHandle(g_outShare); g_outShare = nullptr; }
    ++g_gen;

    D3D11_TEXTURE2D_DESC d{};
    d.Width = capW; d.Height = capH; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;             // matches D3D9 X8R8G8B8 byte order, no swizzle
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    g_d11->CreateTexture2D(&d, nullptr, &g_inTex);
    d.Width = w; d.Height = h;
    g_d11->CreateTexture2D(&d, nullptr, &g_outTex);

    D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    g_d11->CreateTexture2D(&s, nullptr, &g_outStage);

    wchar_t nin[64], nout[64];
    hook::inName(nin, 64, g_gen); hook::outName(nout, 64, g_gen);
    ComPtr<IDXGIResource1> ri, ro;
    if (SUCCEEDED(g_inTex.As(&ri)))
        ri->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nin, &g_inShare);
    if (SUCCEEDED(g_outTex.As(&ro)))
        ro->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nout, &g_outShare);
    g_inTex.As(&g_inMx); g_outTex.As(&g_outMx);

    if (g_ctrl) { g_ctrl->width = w; g_ctrl->height = h; g_ctrl->texGen = g_gen; }
    logf("proxy: shared textures gen %u (in %ux%u, out %ux%u)", g_gen, capW, capH, w, h);
}

static bool acquired(HRESULT hr) { return hr == S_OK || hr == WAIT_ABANDONED; }

// Paint the shaded pixels (BGRA, srcPitch bytes/row) over the whole backbuffer as a textured quad.
static void writeback(IDirect3DDevice9 *dev, const void *pixels, UINT srcPitch)
{
    if (!g_d9sys) {
        dev->CreateTexture(g_bbW, g_bbH, 1, 0, g_bbFmt, D3DPOOL_SYSTEMMEM, &g_d9sys, nullptr);
        dev->CreateTexture(g_bbW, g_bbH, 1, 0, g_bbFmt, D3DPOOL_DEFAULT, &g_d9def, nullptr);
        dev->CreateStateBlock(D3DSBT_ALL, &g_sb);
    }
    if (!g_d9sys || !g_d9def || !g_sb) return;

    D3DLOCKED_RECT lr{};
    if (FAILED(g_d9sys->LockRect(0, &lr, nullptr, 0))) return;
    for (UINT y = 0; y < g_bbH; ++y)
        memcpy((uint8_t *)lr.pBits + y * lr.Pitch, (const uint8_t *)pixels + y * srcPitch, g_bbW * 4);
    g_d9sys->UnlockRect(0);
    dev->UpdateTexture(g_d9sys.Get(), g_d9def.Get());

    ComPtr<IDirect3DSurface9> oldRT, bb;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    g_sb->Capture();                                    // snapshot game state, restored below

    dev->SetRenderTarget(0, bb.Get());
    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr);
    dev->SetTexture(0, g_d9def.Get());
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    const float o = -0.5f, W = float(g_bbW), H = float(g_bbH);   // D3D9 half-texel alignment
    struct V { float x, y, z, rhw, u, v; };
    V quad[4] = {
        { o,     o,     0, 1, 0, 0 },
        { W + o, o,     0, 1, 1, 0 },
        { o,     H + o, 0, 1, 0, 1 },
        { W + o, H + o, 0, 1, 1, 1 },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));

    dev->SetRenderTarget(0, oldRT.Get());               // RT is not part of the state block
    g_sb->Apply();
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                             HWND hwnd, const RGNDATA *dirty)
{
    ensureD11();

    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return g_realPresent(dev, src, dst, hwnd, dirty);
    D3DSURFACE_DESC sd{}; bb->GetDesc(&sd);

    // Trust the live backbuffer, not the CreateDevice/Reset hooks, for size + device identity. A
    // fullscreen toggle can swap in a new device or resize without a Reset we see; deriving state
    // here catches all of it. On any change, drop the device-tied resources so they rebuild.
    if (dev != g_dev || sd.Width != g_bbW || sd.Height != g_bbH || sd.Format != g_bbFmt) {
        releaseD9SizeResources();
        logf("proxy: surface %ux%u fmt %d dev %p (was %ux%u dev %p)",
             sd.Width, sd.Height, int(sd.Format), (void *)dev, g_bbW, g_bbH, (void *)g_dev);
        g_dev = dev; g_bbW = sd.Width; g_bbH = sd.Height; g_bbFmt = sd.Format;
    }

    static int frame = 0; ++frame;
    const bool alive = g_ctrl && g_ctrl->workerSession &&
                       (GetTickCount64() - g_ctrl->workerHeartbeat < hook::HEARTBEAT_STALE_MS);
    static bool wasAlive = false;
    if (alive != wasAlive) { logf("proxy: worker %s (frame %d)", alive ? "ENGAGED" : "LOST", frame); wasAlive = alive; }

    if (alive && g_bbW && g_bbH) {
        // Capture at the worker's requested source resolution when it asks for one smaller than the
        // backbuffer. For an integer-upscaled game (Sonic Mania draws 424x240 into 1272x720) POINT
        // lands exactly on the original pixel centres, so this recovers the native image rather than
        // resampling it -- and it shrinks the readback by the same factor.
        // ponytail: POINT only. A non-integer scale would alias; switch to LINEAR if that ever shows up.
        UINT capW = g_bbW, capH = g_bbH;
        if (g_ctrl->srcWidth && g_ctrl->srcHeight && g_ctrl->srcWidth < g_bbW && g_ctrl->srcHeight < g_bbH) {
            capW = g_ctrl->srcWidth; capH = g_ctrl->srcHeight;
        }
        if (capW != g_capW || capH != g_capH) {
            g_capSurf.Reset(); g_capRT.Reset();
            g_capW = capW; g_capH = capH;
            logf("proxy: capture size -> %ux%u (backbuffer %ux%u)", capW, capH, g_bbW, g_bbH);
        }
        ensureShared(capW, capH, g_bbW, g_bbH);
        if (!g_capSurf) {
            dev->CreateOffscreenPlainSurface(capW, capH, g_bbFmt, D3DPOOL_SYSTEMMEM, &g_capSurf, nullptr);
            if (capW != g_bbW || capH != g_bbH)
                dev->CreateRenderTarget(capW, capH, g_bbFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g_capRT, nullptr);
        }

        IDirect3DSurface9 *capSrc = bb;
        if (g_capRT) {
            if (FAILED(dev->StretchRect(bb, nullptr, g_capRT.Get(), nullptr, D3DTEXF_POINT))) {
                // Driver refused the downscale. Drop the whole capture setup and skip this frame;
                // sizes rebuild next Present rather than feeding a mismatched surface to readback.
                g_capRT.Reset(); g_capSurf.Reset(); g_capW = g_capH = 0;
                capSrc = nullptr;
            } else {
                capSrc = g_capRT.Get();
            }
        }

        HRESULT grtd = g_capSurf && capSrc ? dev->GetRenderTargetData(capSrc, g_capSurf.Get()) : E_FAIL;
        if (FAILED(grtd)) {
            static HRESULT last = S_OK; if (grtd != last) { last = grtd; logf("proxy: GetRenderTargetData 0x%08lX", grtd); }
        } else {
            D3DLOCKED_RECT cap{};
            if (SUCCEEDED(g_capSurf->LockRect(&cap, nullptr, D3DLOCK_READONLY))) {
                if (g_inMx && acquired(g_inMx->AcquireSync(hook::IN_GAME, 16))) {
                    g_d11ctx->UpdateSubresource(g_inTex.Get(), 0, nullptr, cap.pBits, cap.Pitch, 0);
                    g_d11ctx->Flush();
                    g_inMx->ReleaseSync(hook::IN_WORKER);            // hand frame to worker
                }
                g_capSurf->UnlockRect();

                if (g_outMx && acquired(g_outMx->AcquireSync(hook::OUT_GAME, 34))) {
                    g_d11ctx->CopyResource(g_outStage.Get(), g_outTex.Get());
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(g_d11ctx->Map(g_outStage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                        writeback(dev, m.pData, m.RowPitch);
                        g_d11ctx->Unmap(g_outStage.Get(), 0);
                    }
                    g_outMx->ReleaseSync(hook::OUT_WORKER);          // return buffer to worker
                }
            }
        }
    }
    if ((frame % 300) == 0) logf("proxy: frame %d alive=%d %ux%u", frame, int(alive), g_bbW, g_bbH);
    bb->Release();
    return g_realPresent(dev, src, dst, hwnd, dirty);
}

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    releaseD9SizeResources();                            // DEFAULT-pool resources must go before Reset
    HRESULT hr = g_realReset(dev, pp);
    if (SUCCEEDED(hr) && pp) {
        g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
        if (pp->BackBufferFormat != D3DFMT_UNKNOWN) g_bbFmt = pp->BackBufferFormat;
        logf("proxy: Reset -> %ux%u", g_bbW, g_bbH);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type,
                                                  HWND focus, DWORD behavior,
                                                  D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **ppDev)
{
    HRESULT hr = g_realCreateDevice(self, adapter, type, focus, behavior, pp, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev && !g_devHooked) {
        g_devHooked = true;
        if (pp) {
            g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
            if (pp->BackBufferFormat != D3DFMT_UNKNOWN) g_bbFmt = pp->BackBufferFormat;
        }
        g_realReset = reinterpret_cast<PFN_Reset>(hookVtbl(*ppDev, 16, reinterpret_cast<void *>(&HookReset)));
        g_realPresent = reinterpret_cast<PFN_Present>(hookVtbl(*ppDev, 17, reinterpret_cast<void *>(&HookPresent)));
        logf("device created (%ux%u fmt %d), Present+Reset hooked", g_bbW, g_bbH, int(g_bbFmt));
    }
    return hr;
}

extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    static PFN_Create9 real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\d3d9.dll");
        HMODULE m = LoadLibraryA(path);
        real = reinterpret_cast<PFN_Create9>(GetProcAddress(m, "Direct3DCreate9"));
        logf("Direct3DCreate9: real runtime %s (fn=%p)", path, real);
    }
    IDirect3D9 *d3d = real ? real(sdk) : nullptr;
    if (d3d && !g_realCreateDevice) {
        g_realCreateDevice = reinterpret_cast<PFN_CreateDevice>(hookVtbl(d3d, 16, reinterpret_cast<void *>(&HookCreateDevice)));
        logf("IDirect3D9 created, CreateDevice hooked");
    }
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        logf("=== proxy attached to pid %lu ===", GetCurrentProcessId());
    }
    return TRUE;
}
