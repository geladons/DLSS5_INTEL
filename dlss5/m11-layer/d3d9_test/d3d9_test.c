// Minimal 32-bit D3D9Ex present/reset-loop test app for the DXVK + m11-layer
// path. Built x86; with DXVK's d3d9.dll next to the exe it becomes a 32-bit
// Vulkan process, so the 32-bit implicit layer hands each present to m11d.
// C (not C++): COM calls go through lpVtbl explicitly.
//
// Mirrors the GTA IV startup death: windowed presents, then fullscreen
// Reset, presents, then a second fullscreen Reset (the point where GTA IV
// went "Device lost"). Progress lines go to stdout and to dbgout.
// Env: D3D9T_FULLSCREEN=0 skips the fullscreen phases (windowed resets only).
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;
typedef IDirect3D9* (WINAPI *Create9Fn)(UINT);
typedef HRESULT (WINAPI *Create9ExFn)(UINT, IDirect3D9Ex**);

static int g_fullscreen = 1;

static void note(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    printf("%s\n", buf);
    fflush(stdout);
    OutputDebugStringA("[d3d9t] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

static int present_frames(IDirect3DDevice9 *dev, int n, int tag)
{
    for (int i = 0; i < n; ++i) {
        float t = (float)(tag * 100 + i) * 0.05f;
        Vtx v[3] = {
            { 400.0f + 200.0f * (float)sin(t),        150.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 64, 64) },
            { 200.0f,                              450.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(64, 255, 64) },
            { 600.0f,                              450.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(64, 64, 255) },
        };
        HRESULT hr = dev->lpVtbl->TestCooperativeLevel(dev);
        if (hr == D3DERR_DEVICELOST) { note("[FAIL] tag=%d frame=%d device lost", tag, i); return -1; }
        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB((tag * 37 + i) % 255, 128, 255 - ((tag * 53 + i) % 255)), 0.0f, 0);
        dev->lpVtbl->BeginScene(dev);
        dev->lpVtbl->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
        dev->lpVtbl->EndScene(dev);
        hr = dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) { note("[FAIL] tag=%d frame=%d Present hr=0x%08lx", tag, i, (long)hr); return -1; }
        Sleep(33);
    }
    note("[ok] tag=%d presented %d frames", tag, n);
    return 0;
}

static int reset_dev(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp, int w, int h,
                     int windowed, int tag)
{
    ZeroMemory(pp, sizeof *pp);
    pp->Windowed = windowed;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->BackBufferWidth = w;
    pp->BackBufferHeight = h;
    pp->BackBufferCount = 2;
    pp->hDeviceWindow = GetShellWindow(); /* unused; keep non-null */
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!windowed) pp->FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
    note("[..] tag=%d Reset -> %dx%d windowed=%d", tag, w, h, windowed);
    HRESULT hr = dev->lpVtbl->Reset(dev, pp);
    if (FAILED(hr)) { note("[FAIL] tag=%d Reset hr=0x%08lx", tag, (long)hr); return -1; }
    note("[ok] tag=%d Reset done", tag);
    return 0;
}

int main(void)
{
    const char *fs = getenv("D3D9T_FULLSCREEN");
    if (fs && !strcmp(fs, "0")) g_fullscreen = 0;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "d3d9_nr_test";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc)) { note("[FAIL] RegisterClass"); return 1; }
    HWND hwnd = CreateWindowA("d3d9_nr_test", "d3d9_nr_test",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 816, 639, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { note("[FAIL] CreateWindow"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) { note("[FAIL] LoadLibrary d3d9.dll"); return 1; }
    Create9ExFn create_ex = (Create9ExFn)GetProcAddress(d3d9, "Direct3DCreate9Ex");
    Create9Fn create9 = (Create9Fn)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!create_ex) note("[warn] Direct3DCreate9Ex missing; falling back to Direct3DCreate9");

    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *dev = NULL;

    if (create_ex) {
        IDirect3D9Ex *d3dex = NULL;
        HRESULT hr = create_ex(D3D_SDK_VERSION, &d3dex);
        if (FAILED(hr) || !d3dex) { note("[FAIL] Direct3DCreate9Ex hr=0x%08lx", (long)hr); return 1; }
        ZeroMemory(&pp, sizeof pp);
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth = 800;
        pp.BackBufferHeight = 600;
        pp.BackBufferCount = 1;
        pp.hDeviceWindow = hwnd;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        hr = d3dex->lpVtbl->CreateDeviceEx(d3dex, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                           D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL,
                                           (IDirect3DDevice9Ex**)&dev);
        if (FAILED(hr))
            hr = d3dex->lpVtbl->CreateDeviceEx(d3dex, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL,
                                               (IDirect3DDevice9Ex**)&dev);
        d3dex->lpVtbl->Release(d3dex);
        if (FAILED(hr) || !dev) { note("[FAIL] CreateDeviceEx hr=0x%08lx", (long)hr); return 1; }
    } else {
        IDirect3D9 *d3d = create9(D3D_SDK_VERSION);
        if (!d3d) { note("[FAIL] Direct3DCreate9"); return 1; }
        ZeroMemory(&pp, sizeof pp);
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth = 800;
        pp.BackBufferHeight = 600;
        pp.BackBufferCount = 1;
        pp.hDeviceWindow = hwnd;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                               D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
        d3d->lpVtbl->Release(d3d);
        if (FAILED(hr) || !dev) { note("[FAIL] CreateDevice hr=0x%08lx", (long)hr); return 1; }
    }
    note("[ok] device up (Ex=%d) windowed 800x600", create_ex ? 1 : 0);

    if (present_frames(dev, 5, 1)) goto out;

    /* windowed reset: different size */
    if (reset_dev(dev, &pp, 1024, 768, 1, 2)) goto out;
    if (present_frames(dev, 5, 2)) goto out;

    if (g_fullscreen) {
        /* fullscreen reset, then same-size fullscreen reset: the GTA IV death */
        if (reset_dev(dev, &pp, 1920, 1080, 0, 3)) goto out;
        if (present_frames(dev, 5, 3)) goto out;
        if (reset_dev(dev, &pp, 1920, 1080, 0, 4)) goto out;
        if (present_frames(dev, 5, 4)) goto out;
        if (reset_dev(dev, &pp, 800, 600, 1, 5)) goto out;
        if (present_frames(dev, 5, 5)) goto out;
    } else {
        if (reset_dev(dev, &pp, 800, 600, 1, 3)) goto out;
        if (present_frames(dev, 5, 3)) goto out;
    }

    note("[done] all phases passed");
out:
    if (dev) dev->lpVtbl->Release(dev);
    DestroyWindow(hwnd);
    return 0;
}
