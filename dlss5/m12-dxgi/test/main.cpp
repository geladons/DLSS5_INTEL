// m12_test main.cpp - DX12 validation app for the m12 dxgi proxy.
//
// Renders a known byte-exact gradient, presents N frames through a normal
// CreateDXGIFactory1 + CreateSwapChain path (the proxy dxgi.dll in this
// exe's directory intercepts it), then reads back the last presented
// backbuffer and compares it against:
//   expected9  - the gradient of live frame 9 (what the daemon processed
//                and the proxy re-blit on frames 10..12)
//   expected12 - the gradient the game itself drew last
// Proxy active  : diff(readback, g9) small (~2/255), diff(readback, g12) big.
// Proxy inactive: diff(readback, g12) == 0.
// Writes readback.bmp / expected9.bmp / expected12.bmp next to the exe.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define LOG(...)                         \
    do {                                 \
        printf(__VA_ARGS__);             \
        printf("\n");                    \
        fflush(stdout);                  \
    } while (0)

using Microsoft::WRL::ComPtr;

static const UINT W = 960, H = 540, FRAMES = 12, BUFFERS = 2;
static const UINT LIVE_EVERY = 4;  // keep in sync with M12_LIVE at run time

// Per-frame solid color. `bgra` goes to the clear call; the comparison
// buffers keep the READBACK byte order (DXGI R8G8B8A8 = R,G,B,A in memory).
static void frame_color(UINT frame, BYTE bgra[4])
{
    bgra[0] = (BYTE)(64 + frame * 8);   // B
    bgra[1] = (BYTE)(128 + frame * 4);  // G
    bgra[2] = (BYTE)(frame * 16);       // R
    bgra[3] = 255;
}

static std::vector<BYTE> solid_frame(UINT frame)
{
    BYTE c[4];
    frame_color(frame, c);
    std::vector<BYTE> buf((std::size_t)W * H * 4);
    for (size_t i = 0; i < buf.size(); i += 4) {
        buf[i] = c[2];      // R
        buf[i + 1] = c[1];  // G
        buf[i + 2] = c[0];  // B
        buf[i + 3] = c[3];  // A
    }
    return buf;
}

static bool save_bmp(const char *path, const std::vector<BYTE> &rgba_mem)
{
    FILE *f = NULL;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof bfh + sizeof bih;
    bfh.bfSize = bfh.bfOffBits + (DWORD)rgba_mem.size();
    bih.biSize = sizeof bih;
    bih.biWidth = W;
    bih.biHeight = -(LONG)H;  // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    fwrite(&bfh, sizeof bfh, 1, f);
    fwrite(&bih, sizeof bih, 1, f);
    // input is readback byte order (R,G,B,A); BMP wants B,G,R,A
    std::vector<BYTE> out(rgba_mem.size());
    for (size_t i = 0; i < rgba_mem.size(); i += 4) {
        out[i] = rgba_mem[i + 2];
        out[i + 1] = rgba_mem[i + 1];
        out[i + 2] = rgba_mem[i];
        out[i + 3] = rgba_mem[i + 3];
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

static double mean_abs_diff(const std::vector<BYTE> &a, const std::vector<BYTE> &b)
{
    if (a.size() != b.size() || a.empty()) return -1.0;
    double acc = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        acc += abs((int)a[i] - (int)b[i]) + abs((int)a[i + 1] - (int)b[i + 1]) +
               abs((int)a[i + 2] - (int)b[i + 2]);
    return acc / 3.0 / ((double)a.size() / 4.0);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main()
{
    LOG("m12_test: %ux%u, %u frames, live every %u\n", W, H, FRAMES, LIVE_EVERY);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"m12_test";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"m12_test", WS_OVERLAPPEDWINDOW,
                                64, 64, W + 16, H + 39, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { LOG("FAIL: window\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    LOG("window topmost - visible in the stream for ~10 s\n");

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        LOG("FAIL: CreateDXGIFactory1\n");
        return 1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); i++) {
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device))))
            break;
        adapter.Reset();
    }
    if (!device && FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device)))) {
        LOG("FAIL: D3D12CreateDevice\n");
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        LOG("FAIL: CreateCommandQueue\n");
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = BUFFERS;
    scd.BufferDesc.Width = W;
    scd.BufferDesc.Height = H;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Windowed = TRUE;
    LOG("calling CreateSwapChain\n");
    ComPtr<IDXGISwapChain> swapchain;
    if (FAILED(factory->CreateSwapChain(queue.Get(), &scd, &swapchain))) {
        LOG("FAIL: CreateSwapChain\n");
        return 1;
    }
    LOG("CreateSwapChain ok\n");
    ComPtr<IDXGISwapChain3> sc3_render;
    swapchain->QueryInterface(IID_PPV_ARGS(&sc3_render));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = BUFFERS;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_base = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    ComPtr<ID3D12Resource> targets[BUFFERS];
    for (UINT i = 0; i < BUFFERS; i++) {
        swapchain->GetBuffer(i, IID_PPV_ARGS(&targets[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_base;
        h.ptr += (std::size_t)i * rtv_step;
        device->CreateRenderTargetView(targets[i].Get(), NULL, h);
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), NULL,
                              IID_PPV_ARGS(&list));

    std::vector<BYTE> expected[FRAMES];
    for (UINT f = 0; f < FRAMES; f++) {
        expected[f] = solid_frame(f);
        BYTE c[4];
        frame_color(f, c);
        float color[4] = {c[2] / 255.0f, c[1] / 255.0f, c[0] / 255.0f, 1.0f};
        UINT idx = 0;
        if (sc3_render) idx = sc3_render->GetCurrentBackBufferIndex();
        else swapchain->GetLastPresentCount(&idx);
        alloc->Reset();
        list->Reset(alloc.Get(), NULL);
        D3D12_RESOURCE_BARRIER bar = {};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = targets[idx].Get();
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &bar);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_base;
        rtv.ptr += (std::size_t)idx * rtv_step;
        list->ClearRenderTargetView(rtv, color, 0, NULL);
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &bar);
        list->Close();
        ID3D12CommandList *lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        swapchain->Present(0, 0);  // the proxy hooks here; no vsync wait
        LOG("presented frame %u\n", f);
    }

    // Read back the last presented backbuffer.
    ComPtr<IDXGISwapChain3> sc3;
    UINT cur = 0;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))))
        cur = sc3->GetCurrentBackBufferIndex();
    else
        swapchain->GetLastPresentCount(&cur);
    UINT last = (cur + BUFFERS - 1) % BUFFERS;
    const UINT row_pitch = (W * 4 + 255) & ~255u;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf = {};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = (std::size_t)row_pitch * H;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
                                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                    IID_PPV_ARGS(&readback));
    alloc->Reset();
    list->Reset(alloc.Get(), NULL);
    D3D12_RESOURCE_BARRIER bar = {};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = targets[last].Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = targets[last].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &bar);
    list->Close();
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, evt);
    WaitForSingleObject(evt, 8000);

    BYTE *mapped = NULL;
    readback->Map(0, NULL, (void **)&mapped);
    std::vector<BYTE> got((std::size_t)W * H * 4);
    for (UINT y = 0; y < H; y++)
        memcpy(got.data() + (std::size_t)y * W * 4,
               mapped + (std::size_t)y * row_pitch, (std::size_t)W * 4);
    readback->Unmap(0, NULL);

    save_bmp("readback.bmp", got);
    save_bmp("expected_last_live.bmp", expected[FRAMES - LIVE_EVERY]);
    save_bmp("expected_last_drawn.bmp", expected[FRAMES - 1]);
    LOG("mean|d|(readback, frame %u processed) = %.3f /255\n", FRAMES - LIVE_EVERY,
           mean_abs_diff(got, expected[FRAMES - LIVE_EVERY]));
    LOG("mean|d|(readback, frame %u drawn)     = %.3f /255\n", FRAMES - 1,
           mean_abs_diff(got, expected[FRAMES - 1]));
    LOG("m12_test done\n");
    return 0;
}
