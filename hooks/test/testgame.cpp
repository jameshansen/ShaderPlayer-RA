// A stand-in game, so the hook can be tested without owning the game.
//
//   testgame.exe d3d9 | dxgi | gl
//
// Each mode opens a window and presents an animated frame through that graphics API and nothing
// else. That is the whole contract the hook cares about: something loaded d3d9.dll / dxgi.dll /
// opengl32.dll and is calling its present function on a loop. Sonic Mania proves the real thing
// works; this proves all three backends work, in seconds, unattended, with no Steam and no game.
//
// What it draws is chosen to be checkable by machine rather than pretty:
//
//   the field   cycles through hue over about two seconds, so two probes taken a moment apart
//               differ. A static image cannot tell "frames are flowing" from "one frame arrived
//               and then everything froze".
//   the marker  a bright square in the TOP-LEFT eighth of the window. OpenGL reads back
//               bottom-up and the others do not, so a backend that forgets to flip puts the
//               marker at the bottom and the test can say so.
//
// ponytail: clears and scissor rectangles, no geometry, no shaders. Nothing about the capture path
// cares how the pixels got into the backbuffer, and this way each backend is about thirty lines.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <gl/GL.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

static const int kW = 640, kH = 480;

// Field colour at time t, cycling so consecutive frames differ. Deliberately never near black: the
// probe's "is this blank" check would not be able to tell a working capture from a dead one.
static void fieldColour(double t, float &r, float &g, float &b)
{
    r = float(0.35 + 0.3 * std::sin(t * 3.1));
    g = float(0.45 + 0.3 * std::sin(t * 3.1 + 2.1));
    b = float(0.55 + 0.3 * std::sin(t * 3.1 + 4.2));
}

static HWND makeWindow(const wchar_t *title)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sp_testgame";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT r{ 0, 0, kW, kH };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

// True until the window is closed. Pumping messages is not optional: the injector hangs its hook off
// this thread's message queue, so a game that never pumps never gets the DLL.
static bool pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

static double now()
{
    static const ULONGLONG start = GetTickCount64();
    return double(GetTickCount64() - start) / 1000.0;
}

static int runD3D9(HWND hwnd)
{
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { fprintf(stderr, "Direct3DCreate9 failed\n"); return 1; }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = kW; pp.BackBufferHeight = kH;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    ComPtr<IDirect3DDevice9> dev;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        fprintf(stderr, "CreateDevice failed\n"); d3d->Release(); return 1;
    }
    printf("testgame: d3d9 %dx%d\n", kW, kH); fflush(stdout);

    while (pump()) {
        float r, g, b; fieldColour(now(), r, g, b);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET,
                   D3DCOLOR_COLORVALUE(r, g, b, 1.0f), 1.0f, 0);
        D3DRECT marker{ 0, 0, kW / 8, kH / 8 };
        dev->Clear(1, &marker, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 0), 1.0f, 0);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    d3d->Release();
    return 0;
}

static int runDxgi(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = kW; sd.BufferDesc.Height = kH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // the swapped-channel case, on purpose
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> sc; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx))) {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed\n"); return 1;
    }
    ComPtr<ID3D11Texture2D> bb;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)bb.GetAddressOf())) ||
        FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) {
        fprintf(stderr, "no render target view\n"); return 1;
    }
    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx.As(&ctx1);                                       // ClearView, for the marker rectangle
    printf("testgame: dxgi %dx%d\n", kW, kH); fflush(stdout);

    while (pump()) {
        float r, g, b; fieldColour(now(), r, g, b);
        const float field[4] = { r, g, b, 1.0f };
        ctx->ClearRenderTargetView(rtv.Get(), field);
        if (ctx1) {
            const float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
            D3D11_RECT marker{ 0, 0, kW / 8, kH / 8 };
            ctx1->ClearView(rtv.Get(), yellow, &marker, 1);
        }
        sc->Present(1, 0);
    }
    return 0;
}

static int runGL(HWND hwnd)
{
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{ sizeof(pfd), 1 };
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { fprintf(stderr, "no pixel format\n"); return 1; }

    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { fprintf(stderr, "no GL context\n"); return 1; }
    printf("testgame: gl %dx%d\n", kW, kH); fflush(stdout);

    while (pump()) {
        RECT cr{}; GetClientRect(hwnd, &cr);
        const int w = cr.right, h = cr.bottom;
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        float r, g, b; fieldColour(now(), r, g, b);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // GL's origin is bottom-left, so a top-left marker is a HIGH y here. Getting this wrong in
        // the test game would hide exactly the flip bug the marker exists to catch.
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, h - h / 8, w / 8, h / 8);
        glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(dc);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "d3d9";
    wchar_t title[64];
    _snwprintf_s(title, 64, L"SP Test Game (%S)", mode);
    HWND hwnd = makeWindow(title);
    if (!hwnd) return 1;

    if (!strcmp(mode, "d3d9")) return runD3D9(hwnd);
    if (!strcmp(mode, "dxgi")) return runDxgi(hwnd);
    if (!strcmp(mode, "gl"))   return runGL(hwnd);
    fprintf(stderr, "usage: testgame.exe d3d9|dxgi|gl\n");
    return 2;
}
