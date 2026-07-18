// sphook.dll: the in-game half of the Hooks feature.
//
// Windows loads this into the game for us (see hooks/common/hookinject.h). It is not a proxy DLL and
// nothing gets written into the game's folder; it arrives at runtime and attaches to a game that is
// already running.
//
// Because it arrives late, it cannot hook Direct3DCreate9 on the way past. Instead it creates its
// own throwaway D3D9 device and patches that device's vtable, which is the same vtable every
// IDirect3DDevice9 from the same d3d9.dll shares, so the game's live device picks up the patch.
//
// Per frame, when shading is enabled and a worker (ShaderPlayer) is alive:
//   1. capture the finished D3D9 backbuffer to system memory (GetRenderTargetData), downscaling to
//      the worker's requested source resolution first when it asked for one,
//   2. upload it into a shared D3D11 texture and hand it to the worker (keyed mutex),
//   3. take back the shaded result and paint it over the backbuffer as a fullscreen quad,
//   4. call the real Present.
// Otherwise it just calls the real Present, so the game runs stock.
//
// The vtable patch is never removed and the DLL pins itself: unloading patched code while a render
// thread might be inside it is not survivable. Disabling flips a flag instead.
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
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../common/hookshared.h"
#include "../../src/sourcelines.h"

using Microsoft::WRL::ComPtr;

static void logf(const char *fmt, ...)
{
    static FILE *f = nullptr;
    if (!f) {
        char path[MAX_PATH];
        if (!GetEnvironmentVariableA("TEMP", path, MAX_PATH)) return;
        strcat_s(path, "\\sp_hook.log");
        f = fopen(path, "a");
    }
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fflush(f);
}

// ---- hook plumbing ---------------------------------------------------------
typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

static PFN_Present g_realPresent = nullptr;
static PFN_Reset g_realReset = nullptr;

// Redirect a d3d9.dll entry point to our own function, returning what to call for the original.
//
// Patching the device vtable does not work here: D3D9 gives each device its own heap-allocated
// vtable, so patching one device's table leaves every other device, including the game's, untouched.
// The shared thing is the code the tables point at, inside d3d9.dll, so that is what we patch.
//
// These entry points are five-byte `jmp rel32` thunks. Replacing one is unusually safe, because we
// are overwriting exactly one instruction whose meaning we fully know: read the displacement to find
// where it went, and that address becomes the trampoline. Nothing has to be disassembled or
// relocated, which is the part of detouring that goes wrong. Anything that is not a thunk we refuse
// to touch rather than guess at instruction boundaries.
//
// If another overlay (Steam, Discord) got here first, its hook is what the thunk points at now, so
// we chain onto it instead of fighting over the slot.
//
// ponytail: thunk-shaped entry points only. If a d3d9 build ever turns up with a real prologue here,
// that is the point to vendor MinHook rather than hand-roll length disassembly.
static void *detourThunk(void *target, void *hook)
{
    auto *p = reinterpret_cast<uint8_t *>(target);
    if (!p || p[0] != 0xE9) {
        logf("hook: %p is not a jmp thunk (first byte %02X); refusing to patch", target, p ? p[0] : 0);
        return nullptr;
    }
    int32_t rel = 0;
    memcpy(&rel, p + 1, 4);
    void *original = p + 5 + rel;

    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    // ponytail: a render thread could in principle be mid-instruction here. The window is a single
    // 4-byte store during injection; suspending every thread first is the belt-and-braces version.
    const int32_t newRel = int32_t(reinterpret_cast<uint8_t *>(hook) - (p + 5));
    memcpy(p + 1, &newRel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return original;
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
    // Seed the generation from our pid so a restarted game never reuses the previous process's
    // texture names. Without this the worker keeps its stale textures and the new game sits in
    // passthrough.
    g_gen = GetCurrentProcessId();
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(hook::Ctrl), hook::CTRL_NAME);
    if (g_map) {
        g_ctrl = static_cast<hook::Ctrl *>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::Ctrl)));
        // The mapping may have outlived a previous game (ShaderPlayer holds it open), so reset the
        // fields this side owns. Leave the worker's heartbeat/session and the enabled flag alone.
        if (g_ctrl) { g_ctrl->magic = hook::MAGIC; g_ctrl->version = 1;
                      g_ctrl->texGen = 0; g_ctrl->width = 0; g_ctrl->height = 0; }
    }
    logf("hook: D3D11 %p, ctrl %p, gen seed %u", g_d11.Get(), g_ctrl, g_gen);
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
    logf("hook: shared textures gen %u (in %ux%u, out %ux%u)", g_gen, capW, capH, w, h);
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

// Ctrl+Alt+S, edge-triggered, and only while this game has focus so the combination stays free for
// whatever else the user has running. Polled from Present because we are already here once a frame;
// a keyboard hook or RegisterHotKey would be more machinery for the same result.
static void pollToggle()
{
    DWORD fpid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fpid);
    if (fpid != GetCurrentProcessId()) return;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)
                   && (GetAsyncKeyState(hook::TOGGLE_VK) & 0x8000);
    static bool was = false;
    if (down && !was && g_ctrl) {
        g_ctrl->enabled = g_ctrl->enabled ? 0 : 1;
        logf("hook: hotkey -> enabled=%u", g_ctrl->enabled);
    }
    was = down;
}

// Only one game drives the pipeline at a time. First hooked process to present claims it; a claim
// left behind by a game that has since exited is taken over.
static bool claimPipeline()
{
    const uint32_t self = GetCurrentProcessId();
    if (g_ctrl->hookedPid == self) return true;
    if (g_ctrl->hookedPid != 0) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_ctrl->hookedPid);
        if (h) {
            const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
            CloseHandle(h);
            if (alive) return false;                    // someone else is genuinely using it
        }
    }
    g_ctrl->hookedPid = self;
    logf("hook: claimed pipeline for pid %u", self);
    return true;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                             HWND hwnd, const RGNDATA *dirty)
{
    ensureD11();
    pollToggle();

    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return g_realPresent(dev, src, dst, hwnd, dirty);
    D3DSURFACE_DESC sd{}; bb->GetDesc(&sd);

    // Trust the live backbuffer, not the CreateDevice/Reset hooks, for size + device identity. A
    // fullscreen toggle can swap in a new device or resize without a Reset we see; deriving state
    // here catches all of it. On any change, drop the device-tied resources so they rebuild.
    if (dev != g_dev || sd.Width != g_bbW || sd.Height != g_bbH || sd.Format != g_bbFmt) {
        releaseD9SizeResources();
        logf("hook: surface %ux%u fmt %d dev %p (was %ux%u dev %p)",
             sd.Width, sd.Height, int(sd.Format), (void *)dev, g_bbW, g_bbH, (void *)g_dev);
        g_dev = dev; g_bbW = sd.Width; g_bbH = sd.Height; g_bbFmt = sd.Format;
    }

    static int frame = 0; ++frame;
    const bool alive = g_ctrl && g_ctrl->enabled && g_ctrl->workerSession &&
                       (GetTickCount64() - g_ctrl->workerHeartbeat < hook::HEARTBEAT_STALE_MS) &&
                       claimPipeline();
    static bool wasAlive = false;
    if (alive != wasAlive) { logf("hook: shading %s (frame %d)", alive ? "ON" : "OFF", frame); wasAlive = alive; }

    if (alive && g_bbW && g_bbH) {
        // Capture at the worker's requested source resolution when it asks for one smaller than the
        // backbuffer. For an integer-upscaled game (Sonic Mania draws 424x240 into 1272x720) POINT
        // lands exactly on the original pixel centres, so this recovers the native image rather than
        // resampling it -- and it shrinks the readback by the same factor.
        // ponytail: POINT only. A non-integer scale would alias; switch to LINEAR if that ever shows up.
        //
        // Width comes from the backbuffer's own aspect, by the same rule the video path uses.
        // 240 lines of 1272x720 is 424, which is Sonic Mania's true internal size.
        UINT capW = g_bbW, capH = g_bbH;
        const uint32_t lines = g_ctrl->srcLines;
        if (lines && lines < g_bbH) {
            capW = UINT(widthForLines(int(g_bbW), int(g_bbH), int(lines)));
            capH = lines;
        }
        if (capW != g_capW || capH != g_capH) {
            g_capSurf.Reset(); g_capRT.Reset();
            g_capW = capW; g_capH = capH;
            logf("hook: capture size -> %ux%u (backbuffer %ux%u)", capW, capH, g_bbW, g_bbH);
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
            static HRESULT last = S_OK; if (grtd != last) { last = grtd; logf("hook: GetRenderTargetData 0x%08lX", grtd); }
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
    if ((frame % 300) == 0) logf("hook: frame %d alive=%d %ux%u", frame, int(alive), g_bbW, g_bbH);
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
        logf("hook: Reset -> %ux%u", g_bbW, g_bbH);
    }
    return hr;
}

// ---- attach ----------------------------------------------------------------

// Patch IDirect3DDevice9's vtable using a device of our own. Every device created from the same
// d3d9.dll shares one vtable, so the game's device is hooked the moment ours is patched, without us
// ever needing a pointer to it.
static bool hookD3D9()
{
    HMODULE d9 = GetModuleHandleA("d3d9.dll");
    if (!d9) return false;
    auto create = reinterpret_cast<PFN_Create9>(GetProcAddress(d9, "Direct3DCreate9"));
    if (!create) return false;
    IDirect3D9 *d3d = create(D3D_SDK_VERSION);
    logf("hook: Direct3DCreate9 -> %p", (void *)d3d);
    if (!d3d) return false;

    HWND wnd = CreateWindowExW(0, L"STATIC", L"sp", WS_OVERLAPPED, 0, 0, 1, 1,
                               nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;

    IDirect3DDevice9 *dev = nullptr;
    // NULLREF is the fallback: it needs no real adapter resources, so it still works when the game
    // holds an exclusive fullscreen device, and its vtable is the same one.
    const D3DDEVTYPE types[] = { D3DDEVTYPE_HAL, D3DDEVTYPE_NULLREF };
    for (D3DDEVTYPE t : types) {
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, t, wnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
        logf("hook: CreateDevice type %d -> 0x%08lX dev %p", int(t), (unsigned long)hr, (void *)dev);
        if (SUCCEEDED(hr) && dev) break;
    }
    bool ok = false;
    if (dev) {
        // The dummy exists only to read the entry-point addresses out of its vtable. What we patch
        // is the code those point at, which every device in the process shares.
        void **vtbl = *reinterpret_cast<void ***>(dev);
        void *presentEntry = vtbl[17];
        void *resetEntry = vtbl[16];
        dev->Release();

        g_realReset = reinterpret_cast<PFN_Reset>(detourThunk(resetEntry, reinterpret_cast<void *>(&HookReset)));
        g_realPresent = reinterpret_cast<PFN_Present>(detourThunk(presentEntry, reinterpret_cast<void *>(&HookPresent)));
        ok = g_realPresent != nullptr;
        logf("hook: Present entry %p -> real %p, Reset entry %p -> real %p, ok=%d",
             presentEntry, (void *)g_realPresent, resetEntry, (void *)g_realReset, int(ok));
    } else {
        logf("hook: could not create a dummy D3D9 device");
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
    return ok;
}

// Runs off the loader lock. Creating a D3D9 device inside DllMain would deadlock.
static DWORD WINAPI attachThread(LPVOID)
{
    // The game may still be starting up; wait for its graphics runtime to appear.
    for (int i = 0; i < 300 && !GetModuleHandleA("d3d9.dll"); ++i) Sleep(100);
    logf("hook: attach thread running, d3d9=%p dxgi=%p opengl32=%p",
         (void *)GetModuleHandleA("d3d9.dll"), (void *)GetModuleHandleA("dxgi.dll"),
         (void *)GetModuleHandleA("opengl32.dll"));

    if (!hookD3D9()) {
        // ponytail: D3D9 only so far. dxgi.dll and opengl32.dll go here, same shape: find the
        // module, patch its present entry point, reuse everything below it.
        logf("hook: no supported graphics API found in pid %lu", GetCurrentProcessId());
        return 0;
    }

    // Tell the injector we are in. It unhooks as soon as it sees this.
    wchar_t evName[128];
    _snwprintf(evName, 128, L"%s_%lu", hook::LOADED_EVENT, GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    if (ev) SetEvent(ev);   // deliberately leaked: it doubles as the "already injected" marker
    return 0;
}

// The hook procedure exists only so SetWindowsHookEx has somewhere to point. Windows loading this
// DLL into the game is the entire point; the messages themselves are of no interest to us.
extern "C" __declspec(dllexport) LRESULT CALLBACK SpHookProc(int code, WPARAM w, LPARAM l)
{
    return CallNextHookEx(nullptr, code, w, l);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);

        // The injector has to load us too, just to get an HMODULE to pass to SetWindowsHookEx.
        // That is not a game, so do nothing there.
        if (GetEnvironmentVariableW(L"SP_HOOK_INJECTOR", nullptr, 0) > 0) return TRUE;

        // Pin ourselves. The injector unhooks as soon as we are loaded, and Windows would otherwise
        // unload us -- taking the code the game's vtable now points at with it.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &self);

        logf("=== sphook attached to pid %lu ===", GetCurrentProcessId());
        CloseHandle(CreateThread(nullptr, 0, &attachThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
