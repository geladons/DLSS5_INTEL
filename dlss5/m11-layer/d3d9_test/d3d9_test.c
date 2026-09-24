// Minimal 32-bit D3D9 present-loop test app for the DXVK + m11-layer path.
// Built x86; with DXVK's d3d9.dll next to the exe it becomes a 32-bit Vulkan
// process, so the 32-bit implicit layer (Wow6432Node) hands each present to
// m11d. C (not C++): COM calls go through lpVtbl explicitly.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "d3d9_nr_test";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc)) { printf("[FAIL] RegisterClass\n"); return 1; }
    HWND hwnd = CreateWindowA("d3d9_nr_test", "d3d9_nr_test", WS_OVERLAPPEDWINDOW,
                              100, 100, 816, 639, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("[FAIL] CreateWindow\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("[FAIL] Direct3DCreate9 (dxvk d3d9.dll missing?)\n"); return 1; }

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 800;
    pp.BackBufferHeight = 600;
    pp.BackBufferCount = 1;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                           D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr))
        hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("[FAIL] CreateDevice hr=0x%08lx\n", (long)hr); return 1; }
    printf("[ok] d3d9 device up (800x600 windowed), presenting 30 frames\n");
    fflush(stdout);

    for (int i = 0; i < 30; ++i) {
        float t = (float)i * 0.05f;
        Vtx v[3] = {
            { 400.0f + 200.0f * (float)sin(t),        150.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 64, 64) },
            { 200.0f,                              450.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(64, 255, 64) },
            { 600.0f,                              450.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(64, 64, 255) },
        };
        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(i % 255, 128, 255 - (i % 255)), 0.0f, 0);
        dev->lpVtbl->BeginScene(dev);
        dev->lpVtbl->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
        dev->lpVtbl->EndScene(dev);
        hr = dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) { printf("[warn] Present failed at %d hr=0x%08lx\n", i, (long)hr); break; }
        Sleep(33);
    }
    printf("presented 30 frames\n");
    fflush(stdout);
    dev->lpVtbl->Release(dev);
    d3d->lpVtbl->Release(d3d);
    DestroyWindow(hwnd);
    return 0;
}
