// sphook.dll: the in-game half of the Hooks feature.
//
// Windows loads this into the game for us (see hooks/common/hookinject.h). It is not a proxy DLL and
// nothing gets written into the game's folder; it arrives at runtime and attaches to a game that is
// already running.
//
// Per frame, when capture is enabled and a worker (ShaderPlayer) is alive:
//   1. capture the finished backbuffer, downscaling to the worker's requested source resolution so
//      a CRT mask sees the game's native pixels instead of its upscaled ones,
//   2. upload it into a shared D3D11 texture and hand it to the worker (keyed mutex),
//   3. normally stop there -- ShaderPlayer shades the frame and shows it in its own window, which
//      is what scales a mismatched game resolution to fit. When the user asks for the picture to
//      stay in the game's window instead, take the shaded result back and paint it over the
//      backbuffer, which only the D3D9 backend can do.
//
// Otherwise it just calls the real present, so the game runs stock.
//
// Three backends, because a game is whichever of these it happens to be:
//
//   D3D9   thunk-detour d3d9.dll's Present/Reset entry points. Capture and write-back.
//   DXGI   patch IDXGISwapChain's vtable, which dxgi.dll shares across every swap chain in the
//          process, so one patch covers D3D10, D3D11 and any D3D12 game presenting through it.
//   GL     patch the import tables so a game's call to SwapBuffers/wglSwapBuffers lands here.
//
// All three hook at once and the first one to actually present claims the pipeline, so nothing has
// to guess which API a game uses -- and a game that loads d3d9.dll but renders with D3D11 sorts
// itself out on the first frame.
//
// Patches are never removed and the DLL pins itself: unloading patched code while a render thread
// might be inside it is not survivable. Disabling flips a flag instead.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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
    // Stamp every line with the process and the time. Several hooked processes append to this one
    // file and each holds it open for its lifetime, so a test script's attempt to delete it between
    // runs silently fails -- which is how a stale run's numbers get read as the current one's.
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u pid %lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId());
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fflush(f);
}

// ---- patching ---------------------------------------------------------------

// Redirect a d3d9.dll entry point to our own function, returning what to call for the original.
//
// Patching the device vtable does not work for D3D9: it gives each device its own heap-allocated
// vtable, so patching one device's table leaves every other device, including the game's,
// untouched. The shared thing is the code the tables point at, inside d3d9.dll, so that is what we
// patch. (DXGI is the opposite -- see patchVtable.)
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

// Swap one slot of a COM vtable. dxgi.dll keeps a single vtable per interface in its own data, so
// patching the slot hooks every swap chain in the process, present and future, without touching a
// single byte of code. No disassembly, no trampoline, and nothing to get wrong.
static void *patchVtable(void **vtbl, int index, void *hook)
{
    if (!vtbl) return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
    void *original = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    return original;
}

// Point every import-table slot in the process that resolves to `real` at `hook` instead.
//
// Matching on the address rather than the function name is what makes this short: no parsing of
// import name tables, no special case for bound imports (where the name table is absent), and it
// finds the call whether the game imports SwapBuffers itself or a middleware DLL like SDL2 does it
// on the game's behalf.
//
// This is the safe way to hook OpenGL. wglSwapBuffers is a real function with a real prologue, not
// a jmp thunk, so detouring the code would mean length-disassembling it; the import table is a
// table of pointers and rewriting one is just a store.
static int patchIat(void *real, void *hook)
{
    if (!real || real == hook) return 0;
    int patched = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{ sizeof(me) };
    for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me)) {
        auto *base = reinterpret_cast<uint8_t *>(me.modBaseAddr);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size) continue;

        auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
        for (; imp->Name; ++imp) {
            auto **slot = reinterpret_cast<void **>(base + imp->FirstThunk);
            for (; *slot; ++slot) {
                if (*slot != real) continue;
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) continue;
                *slot = hook;
                VirtualProtect(slot, sizeof(void *), old, &old);
                ++patched;
            }
        }
    }
    CloseHandle(snap);
    return patched;
}

// ---- shared state ----------------------------------------------------------
static ComPtr<ID3D11Device> g_d11;
static ComPtr<ID3D11DeviceContext> g_d11ctx;
static HANDLE g_map = nullptr;
static hook::Ctrl *g_ctrl = nullptr;

static ComPtr<ID3D11Texture2D> g_outTex, g_outStage;
static ComPtr<IDXGIKeyedMutex> g_outMx;
static HANDLE g_outShare = nullptr;

// The captured-frame ring (see hookshared.h). Written every frame, read by ShaderPlayer.
static HANDLE g_frameMap = nullptr;
static hook::FrameHeader *g_frameHdr = nullptr;
static uint8_t *g_frameSlots = nullptr;
static uint32_t g_gen = 0;
static uint32_t g_api = hook::API_NONE;   // whichever backend presented first

static bool acquired(HRESULT hr) { return hr == S_OK || hr == WAIT_ABANDONED; }

// Frame accounting, reported periodically from shouldCapture. QueryPerformanceCounter rather than
// GetTickCount64: the costs being measured here are single-digit milliseconds and the tick counter
// only moves every 15.6ms, which reports them as either nothing or one whole tick.
static uint64_t g_presents = 0, g_mark = 0;
static double g_readMs = 0, g_acquireMs = 0;
static uint32_t g_submits = 0;

static double perfMs()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
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
        if (g_ctrl) { g_ctrl->magic = hook::MAGIC; g_ctrl->version = 2;
                      g_ctrl->texGen = 0; g_ctrl->width = 0; g_ctrl->height = 0;
                      g_ctrl->capWidth = 0; g_ctrl->capHeight = 0; g_ctrl->api = hook::API_NONE; }
    }
    logf("hook: D3D11 %p, ctrl %p, gen seed %u", g_d11.Get(), g_ctrl, g_gen);
}

// (Re)create the shared textures and publish the new generation. The input is the captured frame, so
// it carries the source resolution; the output stays backbuffer size because that is what the
// write-back path paints.
static void ensureShared(UINT capW, UINT capH, UINT w, UINT h)
{
    // applyToGame is part of the condition because the output texture is only built when it is on,
    // so switching it on has to rebuild rather than find the texture missing every frame afterwards.
    static UINT lastCapW = 0, lastCapH = 0;
    static uint32_t lastApply = 0;
    const uint32_t apply = g_ctrl ? g_ctrl->applyToGame : 0;
    if (g_frameHdr && g_ctrl && g_ctrl->width == w && g_ctrl->height == h &&
        lastCapW == capW && lastCapH == capH && lastApply == apply) return;
    lastCapW = capW; lastCapH = capH; lastApply = apply;

    g_outTex.Reset(); g_outStage.Reset(); g_outMx.Reset();
    if (g_outShare) { CloseHandle(g_outShare); g_outShare = nullptr; }
    if (g_frameHdr) { UnmapViewOfFile(g_frameHdr); g_frameHdr = nullptr; g_frameSlots = nullptr; }
    if (g_frameMap) { CloseHandle(g_frameMap); g_frameMap = nullptr; }
    ++g_gen;

    // The captured frames go through plain shared memory. Two slots so the reader is never looking
    // at the one being written.
    const size_t bytes = hook::frameMapBytes(capW, capH);
    wchar_t nframe[64];
    hook::frameName(nframe, 64, g_gen);
    g_frameMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    DWORD(uint64_t(bytes) >> 32), DWORD(bytes & 0xFFFFFFFF), nframe);
    if (g_frameMap) {
        g_frameHdr = static_cast<hook::FrameHeader *>(MapViewOfFile(g_frameMap, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
        if (g_frameHdr) {
            g_frameHdr->seq = 0;
            g_frameHdr->width = capW; g_frameHdr->height = capH;
            g_frameHdr->pitch = capW * 4;
            g_frameHdr->slotBytes = uint32_t(size_t(capW) * capH * 4);
            g_frameSlots = reinterpret_cast<uint8_t *>(g_frameHdr) + sizeof(hook::FrameHeader);
        }
    }

    // The output texture stays a real shared GPU texture: the write-back path needs the shaded
    // result back on the GPU to draw it, so system memory would help nothing there.
    if (apply) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;         // matches D3D9 X8R8G8B8 byte order, no swizzle
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        g_d11->CreateTexture2D(&d, nullptr, &g_outTex);

        D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        g_d11->CreateTexture2D(&s, nullptr, &g_outStage);

        wchar_t nout[64];
        hook::outName(nout, 64, g_gen);
        ComPtr<IDXGIResource1> ro;
        if (SUCCEEDED(g_outTex.As(&ro)))
            ro->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nout, &g_outShare);
        g_outTex.As(&g_outMx);
    }

    if (g_ctrl) { g_ctrl->width = w; g_ctrl->height = h;
                  g_ctrl->capWidth = capW; g_ctrl->capHeight = capH; g_ctrl->texGen = g_gen; }
    logf("hook: shared frames gen %u (%ux%u, %zu bytes), out texture %s",
         g_gen, capW, capH, bytes, g_outTex ? "yes" : "no");
}

// The capture size for a given backbuffer: the worker's requested line count, at the backbuffer's
// own aspect. 240 lines of Sonic Mania's 1272x720 is 424 wide, which is precisely what it renders
// internally before upscaling, so this recovers the native image rather than resampling it.
static void captureSize(UINT bbW, UINT bbH, UINT &capW, UINT &capH)
{
    capW = bbW; capH = bbH;
    const uint32_t lines = g_ctrl ? g_ctrl->srcLines : 0;
    if (lines && lines < bbH) {
        capH = lines;
        capW = UINT(widthForLines(int(bbW), int(bbH), int(lines)));
    }
    if (!capW) capW = bbW;
}

// Nearest-neighbour BGRA resample, optionally flipping vertically (OpenGL reads bottom-up) and
// swapping red and blue (DXGI backbuffers are as often RGBA as BGRA).
//
// Every backend except D3D9 downscales here rather than on the GPU, because it rides along with a
// readback we are already paying for. Nearest is the right filter and not a shortcut: games upscale
// their native picture by an integer factor, so nearest lands on the original pixel centres exactly
// and anything smoother would blur pixels that were never blurred.
static void resampleBgra(const uint8_t *src, UINT srcPitch, UINT srcW, UINT srcH,
                         uint8_t *dst, UINT dstPitch, UINT dstW, UINT dstH, bool flipV, bool swapRB)
{
    for (UINT y = 0; y < dstH; ++y) {
        UINT sy = dstH == srcH ? y : UINT(uint64_t(y) * srcH / dstH);
        if (flipV) sy = srcH - 1 - sy;
        const uint8_t *srow = src + size_t(sy) * srcPitch;
        uint8_t *drow = dst + size_t(y) * dstPitch;
        if (dstW == srcW && !swapRB) {
            memcpy(drow, srow, size_t(dstW) * 4);
            continue;
        }
        for (UINT x = 0; x < dstW; ++x) {
            const uint8_t *p = srow + size_t(dstW == srcW ? x : UINT(uint64_t(x) * srcW / dstW)) * 4;
            drow[x * 4 + 0] = swapRB ? p[2] : p[0];
            drow[x * 4 + 1] = p[1];
            drow[x * 4 + 2] = swapRB ? p[0] : p[2];
            drow[x * 4 + 3] = 255;
        }
    }
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

// Ctrl+Alt+S, edge-triggered, and only while this game has focus so the combination stays free for
// whatever else the user has running. Polled from present because we are already here once a frame;
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

// The one question every backend asks at the top of its present hook: should this frame be
// captured? Enabled, a live worker, and no other hooked game already holding the pipeline.
static bool shouldCapture(uint32_t api)
{
    ensureD11();
    pollToggle();
    if (!g_ctrl || !g_ctrl->enabled || !g_ctrl->workerSession) return false;
    if (GetTickCount64() - g_ctrl->workerHeartbeat >= hook::HEARTBEAT_STALE_MS) return false;
    if (!claimPipeline()) return false;

    // First backend through here owns the process. A game can have several graphics runtimes loaded
    // without rendering through all of them, and capturing from two of them would interleave two
    // different pictures into one texture.
    if (g_api != api) {
        if (g_api != hook::API_NONE) return false;
        g_api = api;
        g_ctrl->api = api;
        logf("hook: %s claimed the pipeline in pid %lu", hook::apiName(api), GetCurrentProcessId());
    }

    // Where the game's frame time goes while we are capturing. "Why is this slow" splits into the
    // game's own present rate, the cost of reading its backbuffer, and the wait to hand it over,
    // and only this side can see any of them.
    ++g_presents;
    if (g_presents % 120 == 0) {
        const uint64_t t = GetTickCount64();
        if (g_mark)
            logf("hook: %s %.1f present/s, %u submitted, read %.1fms, acquire %.1fms over %llums",
                 hook::apiName(api), 120000.0 / double(t - g_mark), g_submits,
                 g_readMs, g_acquireMs, t - g_mark);
        g_mark = t; g_submits = 0; g_readMs = 0; g_acquireMs = 0;
    }
    return true;
}

// Hand a captured frame, already at capture size and BGRA, to the worker.
//
// This runs inside the game's Present, so it does exactly one thing that can cost time: a memcpy of
// the frame. There is nothing to acquire, nothing to wait for, and no way for ShaderPlayer to slow
// the game down from here -- which is the entire reason the transport is shared memory and not a
// shared texture.
static void submitFrame(const void *bgra, UINT pitch, UINT capW, UINT capH, UINT bbW, UINT bbH)
{
    ensureShared(capW, capH, bbW, bbH);
    if (!g_frameHdr || !g_frameSlots) return;

    const double t0 = perfMs();
    // Write the slot the reader is not in. It reads `seq & 1`; the next completed frame is the
    // other one, and bumping seq afterwards is what publishes it.
    const uint32_t next = g_frameHdr->seq + 1;
    uint8_t *dst = g_frameSlots + size_t(next & 1) * g_frameHdr->slotBytes;
    const UINT dstPitch = g_frameHdr->pitch;
    if (pitch == dstPitch) {
        memcpy(dst, bgra, size_t(dstPitch) * capH);
    } else {
        for (UINT y = 0; y < capH; ++y)
            memcpy(dst + size_t(y) * dstPitch, static_cast<const uint8_t *>(bgra) + size_t(y) * pitch, dstPitch);
    }
    // The pixels must be visible before the sequence number that advertises them.
    MemoryBarrier();
    g_frameHdr->seq = next;
    g_acquireMs += perfMs() - t0;
    ++g_submits;
}

// ---- D3D9 backend ----------------------------------------------------------
typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

static PFN_Present g_realPresent = nullptr;
static PFN_Reset g_realReset = nullptr;

static UINT g_bbW = 0, g_bbH = 0;
static D3DFORMAT g_bbFmt = D3DFMT_X8R8G8B8;
static IDirect3DDevice9 *g_dev = nullptr;   // the device we last saw present; a change means rebuild
static UINT g_capW = 0, g_capH = 0;

// D3D9 write-back resources (recreated on Reset because they live in DEFAULT/SYSTEMMEM pools).
static ComPtr<IDirect3DSurface9> g_capSurf;   // SYSTEMMEM, GetRenderTargetData target, capture size
static ComPtr<IDirect3DSurface9> g_capRT;     // DEFAULT RT, only when downscaling (StretchRect target)
static ComPtr<IDirect3DTexture9> g_d9sys;     // SYSTEMMEM, lockable staging for the shaded result
static ComPtr<IDirect3DTexture9> g_d9def;     // DEFAULT, what the quad samples
static ComPtr<IDirect3DStateBlock9> g_sb;

static void releaseD9SizeResources()
{
    g_capSurf.Reset(); g_capRT.Reset(); g_d9sys.Reset(); g_d9def.Reset(); g_sb.Reset();
}

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

// Take the shaded frame back from the worker and paint it into the game's own window. Only D3D9
// does this; every other backend leaves the game stock and lets ShaderPlayer do the showing.
static void writebackFromWorker(IDirect3DDevice9 *dev)
{
    // Zero timeout for the same reason as the capture side: if the shaded frame is not ready, the
    // game presents its own unshaded one rather than waiting. Painting last frame's shading a frame
    // late is what a dropped frame looks like here, and it beats stalling the render thread.
    if (!g_outMx || !acquired(g_outMx->AcquireSync(hook::OUT_GAME, 0))) return;
    g_d11ctx->CopyResource(g_outStage.Get(), g_outTex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_d11ctx->Map(g_outStage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        writeback(dev, m.pData, m.RowPitch);
        g_d11ctx->Unmap(g_outStage.Get(), 0);
    }
    g_outMx->ReleaseSync(hook::OUT_WORKER);             // return buffer to worker
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                             HWND hwnd, const RGNDATA *dirty)
{
    const bool capture = shouldCapture(hook::API_D3D9);

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

    if (capture && g_bbW && g_bbH) {
        // D3D9 is the one backend that downscales on the GPU rather than during the readback: the
        // readback is a full stall here either way, so shrinking first makes it a smaller one.
        UINT capW, capH;
        captureSize(g_bbW, g_bbH, capW, capH);
        if (capW != g_capW || capH != g_capH) {
            g_capSurf.Reset(); g_capRT.Reset();
            g_capW = capW; g_capH = capH;
            logf("hook: capture size -> %ux%u (backbuffer %ux%u)", capW, capH, g_bbW, g_bbH);
        }
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
                submitFrame(cap.pBits, cap.Pitch, capW, capH, g_bbW, g_bbH);
                g_capSurf->UnlockRect();
                if (g_ctrl->applyToGame) writebackFromWorker(dev);
            }
        }
    }
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

// Patch IDirect3DDevice9's entry points using a device of our own, purely to read the addresses out
// of its vtable. What we patch is the code those point at, which every device in the process shares,
// so the game's device is hooked without us ever holding a pointer to it.
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
    // Spell the back buffer out rather than leaving it UNKNOWN. A HAL device is what we actually
    // want -- it is the one whose vtable entries point at the code a real game's device uses -- and
    // asking for an unspecified 0x0 back buffer is enough to make CreateDevice refuse with
    // E_INVALIDARG on some drivers. It then fell through to NULLREF, whose entry points are a
    // different implementation, so the patch landed somewhere the game never calls.
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    pp.BackBufferCount = 1;
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
        void **vtbl = *reinterpret_cast<void ***>(dev);
        void *presentEntry = vtbl[17];
        void *resetEntry = vtbl[16];

        g_realReset = reinterpret_cast<PFN_Reset>(detourThunk(resetEntry, reinterpret_cast<void *>(&HookReset)));
        g_realPresent = reinterpret_cast<PFN_Present>(detourThunk(presentEntry, reinterpret_cast<void *>(&HookPresent)));

        // 64-bit d3d9.dll does not use jmp thunks for these -- the entry point is the real function,
        // so detourThunk correctly refuses it. Fall back to patching the vtable slot, the way the
        // DXGI backend does. Whether that reaches the game's device depends on this d3d9 build
        // sharing one vtable across devices, which 32-bit d3d9 does not; if it does not, we have
        // patched a table belonging to a device we are about to release and simply stay passthrough.
        // Nothing here can break the game either way, and 64-bit D3D9 games are rare enough that
        // this is the right amount of effort to spend on them.
        if (!g_realPresent) {
            g_realPresent = reinterpret_cast<PFN_Present>(patchVtable(vtbl, 17, reinterpret_cast<void *>(&HookPresent)));
            g_realReset = reinterpret_cast<PFN_Reset>(patchVtable(vtbl, 16, reinterpret_cast<void *>(&HookReset)));
            logf("hook: d3d9 fell back to a vtable patch (present real %p)", (void *)g_realPresent);
        }
        ok = g_realPresent != nullptr;
        logf("hook: Present entry %p -> real %p, Reset entry %p -> real %p, ok=%d",
             presentEntry, (void *)g_realPresent, resetEntry, (void *)g_realReset, int(ok));
        dev->Release();   // after the patch: on the fallback path vtbl belongs to this device
    } else {
        logf("hook: could not create a dummy D3D9 device");
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
    return ok;
}

// ---- DXGI backend ----------------------------------------------------------
//
// Covers D3D10, D3D11 and D3D12, because they all present through IDXGISwapChain and dxgi.dll uses
// one vtable for the lot. Capture reads the backbuffer with the *game's* device -- ours cannot touch
// another device's resources -- and hands the pixels over through system memory, the same route the
// D3D9 path takes.
//
// ponytail: staging readback per frame. The zero-copy version is a shared NT-handle texture on the
// game's own device, but only when that device is D3D11 and on the same adapter as ours. Add it if
// the frame cost ever shows up; correct on every game beats fast on some.

typedef HRESULT(STDMETHODCALLTYPE *PFN_DxgiPresent)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_DxgiPresent1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_DxgiResize)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PFN_DxgiPresent g_realDxgiPresent = nullptr;
static PFN_DxgiPresent1 g_realDxgiPresent1 = nullptr;
static PFN_DxgiResize g_realDxgiResize = nullptr;

static ComPtr<ID3D11Texture2D> g_dxgiStage, g_dxgiResolve;   // live on the *game's* device
static ComPtr<ID3D11Device> g_dxgiDev;
static ComPtr<ID3D11DeviceContext> g_dxgiCtx;
static UINT g_dxgiW = 0, g_dxgiH = 0;
static DXGI_FORMAT g_dxgiFmt = DXGI_FORMAT_UNKNOWN;
static std::vector<uint8_t> g_dxgiScratch;

// Is this a plain 8-bit-per-channel backbuffer we can read as BGRA, and does it need a red/blue
// swap to get there? Anything else (10-bit HDR, float) we decline rather than show wrong colours.
static bool bgraLike(DXGI_FORMAT f, bool &swapRB)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        swapRB = false; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        swapRB = true; return true;
    default:
        return false;
    }
}

static void captureDxgi(IDXGISwapChain *sc)
{
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)bb.GetAddressOf())) || !bb) {
        static bool once = false;
        if (!once) { once = true; logf("hook: DXGI backbuffer is not a D3D11 texture (D3D12 game?); passthrough"); }
        return;
    }
    ComPtr<ID3D11Device> dev;
    bb->GetDevice(&dev);
    if (!dev) return;

    D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
    bool swapRB = false;
    if (!bgraLike(bd.Format, swapRB)) {
        static DXGI_FORMAT moaned = DXGI_FORMAT_UNKNOWN;
        if (moaned != bd.Format) { moaned = bd.Format; logf("hook: DXGI format %d unsupported; passthrough", int(bd.Format)); }
        return;
    }

    if (dev.Get() != g_dxgiDev.Get() || bd.Width != g_dxgiW || bd.Height != g_dxgiH || bd.Format != g_dxgiFmt) {
        g_dxgiStage.Reset(); g_dxgiResolve.Reset();
        g_dxgiDev = dev; g_dxgiCtx.Reset(); dev->GetImmediateContext(&g_dxgiCtx);
        g_dxgiW = bd.Width; g_dxgiH = bd.Height; g_dxgiFmt = bd.Format;
        logf("hook: DXGI backbuffer %ux%u fmt %d samples %u", bd.Width, bd.Height, int(bd.Format), bd.SampleDesc.Count);
    }
    if (!g_dxgiCtx) return;

    if (!g_dxgiStage) {
        D3D11_TEXTURE2D_DESC s = bd;
        s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.MiscFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_dxgiStage))) return;
        // An MSAA backbuffer cannot be copied straight to a staging texture; it has to be resolved
        // down to a single sample first. Only the older non-flip swap chains can be MSAA at all.
        if (bd.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC r = s;
            r.Usage = D3D11_USAGE_DEFAULT; r.CPUAccessFlags = 0;
            if (FAILED(dev->CreateTexture2D(&r, nullptr, &g_dxgiResolve))) { g_dxgiStage.Reset(); return; }
        }
    }

    if (g_dxgiResolve) {
        g_dxgiCtx->ResolveSubresource(g_dxgiResolve.Get(), 0, bb.Get(), 0, bd.Format);
        g_dxgiCtx->CopyResource(g_dxgiStage.Get(), g_dxgiResolve.Get());
    } else {
        g_dxgiCtx->CopyResource(g_dxgiStage.Get(), bb.Get());
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_dxgiCtx->Map(g_dxgiStage.Get(), 0, D3D11_MAP_READ, 0, &m))) return;

    UINT capW, capH;
    captureSize(bd.Width, bd.Height, capW, capH);
    const UINT pitch = capW * 4;
    g_dxgiScratch.resize(size_t(pitch) * capH);
    resampleBgra(static_cast<const uint8_t *>(m.pData), m.RowPitch, bd.Width, bd.Height,
                 g_dxgiScratch.data(), pitch, capW, capH, false, swapRB);
    g_dxgiCtx->Unmap(g_dxgiStage.Get(), 0);

    submitFrame(g_dxgiScratch.data(), pitch, capW, capH, bd.Width, bd.Height);
}

static HRESULT STDMETHODCALLTYPE HookDxgiPresent(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    // DXGI_PRESENT_TEST does not present anything; capturing on it would hand the worker the same
    // frame twice and, worse, do it while the game is only asking whether it may present at all.
    if (!(flags & DXGI_PRESENT_TEST) && shouldCapture(hook::API_DXGI)) captureDxgi(sc);
    return g_realDxgiPresent(sc, interval, flags);
}

static HRESULT STDMETHODCALLTYPE HookDxgiPresent1(IDXGISwapChain1 *sc, UINT interval, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS *pp)
{
    if (!(flags & DXGI_PRESENT_TEST) && shouldCapture(hook::API_DXGI)) captureDxgi(sc);
    return g_realDxgiPresent1(sc, interval, flags, pp);
}

static HRESULT STDMETHODCALLTYPE HookDxgiResize(IDXGISwapChain *sc, UINT count, UINT w, UINT h,
                                                DXGI_FORMAT fmt, UINT flags)
{
    // Our staging textures hold a reference to the buffers being resized, and ResizeBuffers fails
    // outright while anything still does. Drop them and let the next capture rebuild.
    g_dxgiStage.Reset(); g_dxgiResolve.Reset();
    g_dxgiW = g_dxgiH = 0;
    return g_realDxgiResize(sc, count, w, h, fmt, flags);
}

static bool hookDXGI()
{
    if (!GetModuleHandleA("dxgi.dll")) return false;

    HWND wnd = CreateWindowExW(0, L"STATIC", L"sp", WS_OVERLAPPED, 0, 0, 1, 1,
                               nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1; sd.BufferDesc.Height = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> sc; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr) || !sc) {
        logf("hook: dummy swap chain failed 0x%08lX", (unsigned long)hr);
        if (wnd) DestroyWindow(wnd);
        return false;
    }

    // The dummy exists only to read the vtable address. dxgi.dll shares that table across every
    // swap chain it hands out, so patching a slot hooks the game's chain too, including one it has
    // not created yet.
    void **vtbl = *reinterpret_cast<void ***>(sc.Get());
    g_realDxgiPresent = reinterpret_cast<PFN_DxgiPresent>(patchVtable(vtbl, 8, reinterpret_cast<void *>(&HookDxgiPresent)));
    g_realDxgiResize = reinterpret_cast<PFN_DxgiResize>(patchVtable(vtbl, 13, reinterpret_cast<void *>(&HookDxgiResize)));

    // IDXGISwapChain1::Present1 is a different slot on a derived interface, and a modern game that
    // uses a flip-model chain calls it instead. Same trick, one interface further down.
    ComPtr<IDXGISwapChain1> sc1;
    if (SUCCEEDED(sc.As(&sc1))) {
        void **v1 = *reinterpret_cast<void ***>(sc1.Get());
        g_realDxgiPresent1 = reinterpret_cast<PFN_DxgiPresent1>(patchVtable(v1, 22, reinterpret_cast<void *>(&HookDxgiPresent1)));
    }

    logf("hook: DXGI Present -> real %p, Present1 -> real %p, ResizeBuffers -> real %p",
         (void *)g_realDxgiPresent, (void *)g_realDxgiPresent1, (void *)g_realDxgiResize);

    sc.Reset(); ctx.Reset(); dev.Reset();
    if (wnd) DestroyWindow(wnd);
    return g_realDxgiPresent != nullptr;
}

// ---- OpenGL backend --------------------------------------------------------
//
// glReadPixels off the back buffer, which is the one capture every GL implementation supports and
// needs no extension, no context sharing and no knowledge of what the game drew.
//
// ponytail: a synchronous readback, so it stalls the GL pipeline once a frame. The unstalled version
// is a pair of pixel buffer objects read one frame late. Worth doing only if a real game turns out
// to care; this already sits after the game's last draw call.

typedef BOOL(WINAPI *PFN_SwapBuffers)(HDC);
typedef void(WINAPI *PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
typedef void(WINAPI *PFN_glReadBuffer)(unsigned);
typedef void(WINAPI *PFN_glPixelStorei)(unsigned, int);

static PFN_SwapBuffers g_realSwapBuffers = nullptr;      // gdi32!SwapBuffers
static PFN_SwapBuffers g_realWglSwap = nullptr;          // opengl32!wglSwapBuffers
static PFN_glReadPixels g_glReadPixels = nullptr;
static PFN_glReadBuffer g_glReadBuffer = nullptr;
static PFN_glPixelStorei g_glPixelStorei = nullptr;
static std::vector<uint8_t> g_glFull, g_glScratch;

enum : unsigned { GL_BACK_ = 0x0405, GL_BGRA_ = 0x80E1, GL_UNSIGNED_BYTE_ = 0x1401,
                  GL_PACK_ALIGNMENT_ = 0x0D05 };

static void captureGL(HDC dc)
{
    if (!g_glReadPixels) return;
    HWND wnd = WindowFromDC(dc);
    RECT rc{};
    if (!wnd || !GetClientRect(wnd, &rc)) return;
    const UINT w = UINT(rc.right - rc.left), h = UINT(rc.bottom - rc.top);
    if (!w || !h) return;

    const size_t need = size_t(w) * h * 4;
    if (g_glFull.size() < need) g_glFull.resize(need);
    const double t0 = perfMs();
    if (g_glReadBuffer) g_glReadBuffer(GL_BACK_);
    if (g_glPixelStorei) g_glPixelStorei(GL_PACK_ALIGNMENT_, 4);   // 4 bytes/pixel: already aligned
    g_glReadPixels(0, 0, int(w), int(h), GL_BGRA_, GL_UNSIGNED_BYTE_, g_glFull.data());
    g_readMs += perfMs() - t0;

    UINT capW, capH;
    captureSize(w, h, capW, capH);
    const UINT pitch = capW * 4;
    g_glScratch.resize(size_t(pitch) * capH);
    // GL's origin is bottom-left, so the rows come back upside down relative to everything else in
    // the pipeline. The flip rides along with the resample rather than costing a second pass.
    resampleBgra(g_glFull.data(), w * 4, w, h, g_glScratch.data(), pitch, capW, capH, true, false);
    submitFrame(g_glScratch.data(), pitch, capW, capH, w, h);
}

// Both entry points land here. A game calls one or the other, but gdi32!SwapBuffers can tail into
// opengl32!wglSwapBuffers inside the runtime, so guard against capturing one frame twice.
static BOOL WINAPI HookSwapBuffers(HDC dc)
{
    static thread_local bool inside = false;
    if (!inside) {
        inside = true;
        if (shouldCapture(hook::API_GL)) captureGL(dc);
        inside = false;
    }
    return g_realSwapBuffers ? g_realSwapBuffers(dc) : FALSE;
}

static BOOL WINAPI HookWglSwapBuffers(HDC dc)
{
    static thread_local bool inside = false;
    if (!inside) {
        inside = true;
        if (shouldCapture(hook::API_GL)) captureGL(dc);
        inside = false;
    }
    return g_realWglSwap ? g_realWglSwap(dc) : FALSE;
}

static bool hookGL()
{
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;

    g_glReadPixels = reinterpret_cast<PFN_glReadPixels>(GetProcAddress(gl, "glReadPixels"));
    g_glReadBuffer = reinterpret_cast<PFN_glReadBuffer>(GetProcAddress(gl, "glReadBuffer"));
    g_glPixelStorei = reinterpret_cast<PFN_glPixelStorei>(GetProcAddress(gl, "glPixelStorei"));
    if (!g_glReadPixels) { logf("hook: opengl32 has no glReadPixels?"); return false; }

    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    auto realSwap = gdi ? reinterpret_cast<void *>(GetProcAddress(gdi, "SwapBuffers")) : nullptr;
    auto realWgl = reinterpret_cast<void *>(GetProcAddress(gl, "wglSwapBuffers"));

    const int a = patchIat(realSwap, reinterpret_cast<void *>(&HookSwapBuffers));
    const int b = patchIat(realWgl, reinterpret_cast<void *>(&HookWglSwapBuffers));
    if (a) g_realSwapBuffers = reinterpret_cast<PFN_SwapBuffers>(realSwap);
    if (b) g_realWglSwap = reinterpret_cast<PFN_SwapBuffers>(realWgl);
    if (a || b) logf("hook: OpenGL IAT patched (%d SwapBuffers, %d wglSwapBuffers)", a, b);
    return a || b;
}

// ---- attach ----------------------------------------------------------------

// Runs off the loader lock. Creating a graphics device inside DllMain would deadlock.
//
// Keeps trying, because which runtime a game has loaded changes as it starts up: a launcher may be
// pure GDI for a second, load d3d11 when it opens its window, and only pull in a middleware DLL that
// imports SwapBuffers later still. Each backend is idempotent, so re-running costs a module lookup,
// and the loop stops as soon as a frame has actually gone through one of them.
static DWORD WINAPI attachThread(LPVOID)
{
    logf("hook: attach thread running in pid %lu", GetCurrentProcessId());

    bool announced = false;
    bool d9 = false, dxgi = false, gl = false;
    for (int i = 0; i < 300 && g_api == hook::API_NONE; ++i) {
        if (!d9) d9 = hookD3D9();
        if (!dxgi) dxgi = hookDXGI();
        // GL is re-patched every pass whether or not it took: a DLL loaded after we patched brings
        // its own unpatched import table with it, and only another pass catches it.
        gl = hookGL() || gl;

        if ((d9 || dxgi || gl) && !announced) {
            announced = true;
            logf("hook: backends ready (d3d9=%d dxgi=%d gl=%d)", int(d9), int(dxgi), int(gl));
            // Tell the injector we are in. It unhooks as soon as it sees this.
            wchar_t evName[128];
            _snwprintf(evName, 128, L"%s_%lu", hook::LOADED_EVENT, GetCurrentProcessId());
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
            if (ev) SetEvent(ev);   // deliberately leaked: it doubles as the "already injected" marker
        }
        Sleep(200);
    }
    if (!announced)
        logf("hook: no supported graphics API found in pid %lu", GetCurrentProcessId());
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
