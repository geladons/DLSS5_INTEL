// m12_dx12.h - per-swapchain D3D12 present processor for the m12 dxgi proxy.
//
// Called from the Present hook before the real Present. On live frames it
// copies the backbuffer to a readback buffer on the game's command queue,
// ships the pixels to the m11d daemon over TCP and writes the reply back
// into the backbuffer. On the frames in between it re-blits the last
// processed frame so the screen never shows the unprocessed game image.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

#include <cstdint>
#include <vector>

class M12Dx12Processor {
public:
    M12Dx12Processor(IDXGISwapChain *sc, ID3D12CommandQueue *queue, UINT w, UINT h,
                     DXGI_FORMAT fmt, int live_every);
    ~M12Dx12Processor();

    // Runs the capture/blit pipeline for the current backbuffer.
    void onPresent();

private:
    bool ensureResources();
    void releaseResources();
    bool captureToCpu(ID3D12Resource *bb);
    bool blitToBackbuffer(ID3D12Resource *bb);
    bool waitIdle(DWORD ms);
    bool refreshDesc();
    void dump_processed();

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *res,
                                             D3D12_RESOURCE_STATES from,
                                             D3D12_RESOURCE_STATES to);

    IDXGISwapChain *sc_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_;
    UINT64 fence_value_;

    UINT width_, height_;
    DXGI_FORMAT fmt_;
    bool fmt_supported_;   // FALSE: HDR/odd format -> pass through untouched
    bool needs_rbswap_;    // TRUE: R8G8B8A8 backbuffer, bytes go R,G,B,A
    UINT64 row_pitch_;     // 256-aligned byte row pitch of the staging buffers
    int live_every_;
    UINT64 present_count_;
    UINT64 last_live_ms_;
    UINT64 daemon_failures_;
    bool have_processed_;
    std::vector<BYTE> processed_;  // last daemon reply, packed w*h*4 BGRA
};
