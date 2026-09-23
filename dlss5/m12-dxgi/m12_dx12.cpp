// m12_dx12.cpp - per-swapchain D3D12 present processor (see m12_dx12.h).
#include "m12_dx12.h"

#include "m12_client.h"
#include "m12_log.h"

#include <algorithm>
#include <cstdlib>

namespace {

// The daemon wire format is byte-order B,G,R,A (Vulkan B8G8R8A8, fmt 44).
// DXGI R8G8B8A8 backbuffers store bytes R,G,B,A -> swap channels 0 and 2.
void swizzle_rb(BYTE *px, std::size_t count)
{
    for (std::size_t i = 0; i < count; i += 4)
        std::swap(px[i], px[i + 2]);
}

// M12_DUMP=<path>: write the last processed frame (BGRA bytes) as a BMP.
// Kernel32 only; BMP stores rows bottom-up, BGRA directly.
void dump_bgra_bmp(const char *path, const std::vector<BYTE> &bgra, UINT w, UINT h)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof bfh + sizeof bih;
    bfh.bfSize = bfh.bfOffBits + (DWORD)bgra.size();
    bih.biSize = sizeof bih;
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h;  // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    DWORD n = 0;
    WriteFile(f, &bfh, sizeof bfh, &n, NULL);
    WriteFile(f, &bih, sizeof bih, &n, NULL);
    WriteFile(f, bgra.data(), (DWORD)bgra.size(), &n, NULL);
    CloseHandle(f);
}

}  // namespace

M12Dx12Processor::M12Dx12Processor(IDXGISwapChain *sc, ID3D12CommandQueue *queue,
                                   UINT w, UINT h, DXGI_FORMAT fmt, int live_every)
    : sc_(sc), fence_event_(NULL), fence_value_(0), width_(w), height_(h),
      fmt_(fmt), fmt_supported_(false), needs_rbswap_(false), row_pitch_(0),
      live_every_(live_every > 0 ? live_every : 1), present_count_(0),
      have_processed_(false)
{
    queue_ = queue;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device_));
    if (FAILED(hr)) {
        m12_logf("GetDevice failed (0x%08lx); swapchain %p passthrough", hr, sc);
        return;
    }
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        fmt_supported_ = true;
        needs_rbswap_ = true;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        fmt_supported_ = true;
        needs_rbswap_ = false;
        break;
    default:
        fmt_supported_ = false;
        m12_logf("unsupported backbuffer format %d on swapchain %p - frames pass "
                 "through untouched", (int)fmt, sc);
        break;
    }
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (SUCCEEDED(hr))
        fence_event_ = CreateEvent(NULL, FALSE, FALSE, NULL);
    m12_logf("processor attached: %ux%u fmt %d rb-swap %d live every %d", w, h,
             (int)fmt, needs_rbswap_ ? 1 : 0, live_every_);
}

M12Dx12Processor::~M12Dx12Processor()
{
    if (fence_event_) CloseHandle(fence_event_);
}

D3D12_RESOURCE_BARRIER M12Dx12Processor::transition(ID3D12Resource *res,
                                                    D3D12_RESOURCE_STATES from,
                                                    D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void M12Dx12Processor::releaseResources()
{
    readback_.Reset();
    upload_.Reset();
    list_.Reset();
    alloc_.Reset();
    processed_.clear();
    have_processed_ = false;
}

bool M12Dx12Processor::ensureResources()
{
    if (readback_ && upload_ && list_) return true;
    releaseResources();
    row_pitch_ = (UINT64(width_) * 4 + 255) & ~UINT64(255);
    const UINT64 buf_size = row_pitch_ * height_;

    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = buf_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                IID_PPV_ARGS(&readback_))))
        return false;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                NULL, IID_PPV_ARGS(&upload_))))
        return false;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&alloc_))))
        return false;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(),
                                          NULL, IID_PPV_ARGS(&list_))))
        return false;
    list_->Close();
    processed_.resize((std::size_t)width_ * height_ * 4);
    m12_logf("staging buffers %llu bytes (row pitch %llu)", buf_size, row_pitch_);
    return true;
}

bool M12Dx12Processor::waitIdle(DWORD ms)
{
    if (!fence_ || !fence_event_) return true;
    UINT64 value = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), value))) return false;
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fence_event_))) return false;
    return WaitForSingleObject(fence_event_, ms) == WAIT_OBJECT_0;
}

// Re-read the swapchain desc; frees staging buffers on a size/format change.
bool M12Dx12Processor::refreshDesc()
{
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(sc_->GetDesc(&desc))) return false;
    UINT w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
    DXGI_FORMAT f = desc.BufferDesc.Format;
    if (w != width_ || h != height_ || f != fmt_) {
        m12_logf("swapchain resized %ux%u fmt %d -> %ux%u fmt %d", width_, height_,
                 (int)fmt_, w, h, (int)f);
        width_ = w;
        height_ = h;
        fmt_ = f;
        releaseResources();
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            fmt_supported_ = true;
            needs_rbswap_ = true;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            fmt_supported_ = true;
            needs_rbswap_ = false;
            break;
        default:
            fmt_supported_ = false;
            m12_logf("unsupported backbuffer format %d - frames pass through", (int)f);
            break;
        }
    }
    return fmt_supported_;
}

// Backbuffer -> readback -> daemon -> processed_ (packed B,G,R,A bytes).
bool M12Dx12Processor::captureToCpu(ID3D12Resource *bb)
{
    if (!waitIdle(15000)) {
        m12_logf("capture: waitIdle timed out");
        return false;
    }
    alloc_->Reset();
    list_->Reset(alloc_.Get(), NULL);

    D3D12_RESOURCE_BARRIER bar = transition(bb, D3D12_RESOURCE_STATE_PRESENT,
                                            D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = fmt_;
    dst.PlacedFootprint.Footprint.Width = width_;
    dst.PlacedFootprint.Footprint.Height = height_;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = (UINT)row_pitch_;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

    bar = transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &bar);
    list_->Close();

    ID3D12CommandList *lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (!waitIdle(15000)) {
        m12_logf("capture: GPU copy timed out");
        return false;
    }

    BYTE *mapped = NULL;
    if (FAILED(readback_->Map(0, NULL, (void **)&mapped))) return false;
    std::vector<BYTE> packed((std::size_t)width_ * height_ * 4);    for (UINT y = 0; y < height_; y++)
        memcpy(packed.data() + (std::size_t)y * width_ * 4,
               mapped + (std::size_t)y * row_pitch_, (std::size_t)width_ * 4);
    readback_->Unmap(0, NULL);

    if (needs_rbswap_) swizzle_rb(packed.data(), packed.size());

    const uint32_t header[4] = {0x304E524Eu, width_, height_, 44u};
    std::vector<BYTE> reply(packed.size());
    if (m12_exchange(header, packed.data(), packed.size(), reply.data(),
                     reply.size()) != 0) {
        daemon_failures_++;
        if (daemon_failures_ <= 3 || daemon_failures_ % 100 == 0)
            m12_logf("daemon did not answer (%llu failures); frame unchanged",
                     daemon_failures_);
        return false;
    }
    daemon_failures_ = 0;
    processed_.swap(reply);
    have_processed_ = true;
    m12_logf("processed %ux%u", width_, height_);
    dump_processed();
    return true;
}

void M12Dx12Processor::dump_processed()
{
    static const char *path = getenv("M12_DUMP");
    if (!path || !have_processed_) return;
    dump_bgra_bmp(path, processed_, width_, height_);
}

// processed_ -> upload buffer -> backbuffer (leaves bb in PRESENT state).
bool M12Dx12Processor::blitToBackbuffer(ID3D12Resource *bb)
{
    BYTE *mapped = NULL;
    if (FAILED(upload_->Map(0, NULL, (void **)&mapped))) return false;
    if (needs_rbswap_) swizzle_rb(processed_.data(), processed_.size());
    for (UINT y = 0; y < height_; y++)
        memcpy(mapped + (std::size_t)y * row_pitch_,
               processed_.data() + (std::size_t)y * width_ * 4,
               (std::size_t)width_ * 4);
    if (needs_rbswap_) swizzle_rb(processed_.data(), processed_.size());
    upload_->Unmap(0, NULL);

    if (!waitIdle(15000)) {
        m12_logf("blit: waitIdle timed out");
        return false;
    }
    alloc_->Reset();
    list_->Reset(alloc_.Get(), NULL);

    D3D12_RESOURCE_BARRIER bar = transition(bb, D3D12_RESOURCE_STATE_PRESENT,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
    list_->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = bb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = fmt_;
    src.PlacedFootprint.Footprint.Width = width_;
    src.PlacedFootprint.Footprint.Height = height_;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = (UINT)row_pitch_;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

    bar = transition(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &bar);
    list_->Close();

    ID3D12CommandList *lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    // No fence wait: the app's real Present right after us orders the queue.
    return true;
}

void M12Dx12Processor::onPresent()
{
    if (!fmt_supported_ || !fence_) return;
    if (!refreshDesc() || width_ == 0 || height_ == 0) return;
    if (!ensureResources()) {
        m12_logf("ensureResources failed; frame unchanged");
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> bb;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
    UINT index = 0;
    if (SUCCEEDED(sc_->QueryInterface(IID_PPV_ARGS(&sc3))))
        index = sc3->GetCurrentBackBufferIndex();
    else if (FAILED(sc_->GetLastPresentCount(&index)))
        return;
    HRESULT hr = sc_->GetBuffer(index, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) return;

    present_count_++;
    // Live capture stride (every Nth present) plus a hard minimum interval:
    // an occluded flip window makes DXGI retry Present at full throttle,
    // which would otherwise fire a capture every few milliseconds.
    bool live = ((present_count_ - 1) % (UINT64)live_every_) == 0;
    static const UINT64 min_interval_ms = 250;
    UINT64 now = GetTickCount64();
    if (live && now - last_live_ms_ < min_interval_ms) live = false;
    if (live) {
        last_live_ms_ = now;
        captureToCpu(bb.Get());
    }
    if (have_processed_) blitToBackbuffer(bb.Get());
}
