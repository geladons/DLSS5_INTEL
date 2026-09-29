// m12_dx12.cpp - per-swapchain D3D12 present processor (see m12_dx12.h).
#include "m12_dx12.h"

#include "m12_client.h"
#include "m12_log.h"
#include "chain_c_api.h"

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

// ----------------------------------------------------------------------------
// In-process DLSS 5 engine integration (Stage 3.2: TCP elimination)
// ----------------------------------------------------------------------------
static HMODULE g_chain_dll = NULL;
static void *g_engine = NULL;
static PFN_dlss5_create pfn_dlss5_create = NULL;
static PFN_dlss5_destroy pfn_dlss5_destroy = NULL;
static PFN_dlss5_process_frame pfn_dlss5_process_frame = NULL;
static PFN_dlss5_set_scale pfn_dlss5_set_scale = NULL;
static PFN_dlss5_set_gain pfn_dlss5_set_gain = NULL;
static PFN_dlss5_set_blend pfn_dlss5_set_blend = NULL;
static PFN_dlss5_interpolate_frame pfn_dlss5_interpolate_frame = NULL;
static PFN_dlss5_set_framegen pfn_dlss5_set_framegen = NULL;
static PFN_dlss5_get_framegen pfn_dlss5_get_framegen = NULL;
static int g_inprocess_attempted = 0;
static int g_inprocess_active = 0;
static LONG g_proc_count = 0;
static CRITICAL_SECTION g_inproc_cs;
static LONG g_inproc_cs_init = 0;

static bool is_framegen_enabled()
{
    const char *fg = getenv("M12_FRAMEGEN");
    if (!fg) fg = getenv("DLSS5_FRAMEGEN");
    if (fg) {
        if (fg[0] == '1' || !strcmp(fg, "true") || !strcmp(fg, "on")) return true;
        if (fg[0] == '0' || !strcmp(fg, "false") || !strcmp(fg, "off")) return false;
    }
    if (g_engine && pfn_dlss5_get_framegen) {
        return pfn_dlss5_get_framegen(g_engine) == 1;
    }
    return false;
}

static void ensure_inprocess_cs()
{
    if (InterlockedCompareExchange(&g_inproc_cs_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_inproc_cs);
    }
}

static void ensure_inprocess()
{
    ensure_inprocess_cs();
    EnterCriticalSection(&g_inproc_cs);
    if (g_inprocess_attempted) {
        LeaveCriticalSection(&g_inproc_cs);
        return;
    }
    g_inprocess_attempted = 1;

    const char *inproc_env = getenv("M12_INPROCESS");
    if (!inproc_env) inproc_env = getenv("NR_LAYER_INPROCESS");
    if (inproc_env && (inproc_env[0] == '0' || !strcmp(inproc_env, "false") || !strcmp(inproc_env, "OFF"))) {
        m12_logf("[m12_dxgi] in-process mode disabled via M12_INPROCESS=0");
        LeaveCriticalSection(&g_inproc_cs);
        return;
    }

    HMODULE hDll = NULL;
    const char *custom_dll = getenv("M12_CHAIN_DLL");
    if (!custom_dll) custom_dll = getenv("NR_LAYER_CHAIN_DLL");
    if (custom_dll && custom_dll[0]) {
        hDll = LoadLibraryA(custom_dll);
        if (hDll) {
            m12_logf("[m12_dxgi] loaded d5c_chain.dll from env: %s", custom_dll);
        }
    }

    char mod_path[MAX_PATH] = {0};
    HMODULE hThis = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ensure_inprocess, &hThis) && hThis) {
        if (GetModuleFileNameA(hThis, mod_path, sizeof(mod_path))) {
            char *slash = strrchr(mod_path, '\\');
            if (!slash) slash = strrchr(mod_path, '/');
            if (slash) {
                *(slash + 1) = '\0';
            }
        }
    }

    if (!hDll && mod_path[0]) {
        char candidate[MAX_PATH];
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%sd5c_chain.dll", mod_path);
        if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
            hDll = LoadLibraryA(candidate);
            if (hDll) {
                m12_logf("[m12_dxgi] loaded d5c_chain.dll from module dir: %s", candidate);
            }
        }
    }

    if (!hDll) {
        const char *candidates[] = {
            "dlss5\\chain\\build\\Release\\d5c_chain.dll",
            "dlss5\\m11d\\build\\Release\\d5c_chain.dll",
            "dlss5\\m11-layer\\build\\Release\\d5c_chain.dll",
            "dlss5\\m12-dxgi\\build\\Release\\d5c_chain.dll",
            "..\\..\\chain\\build\\Release\\d5c_chain.dll",
            "..\\..\\m11d\\build\\Release\\d5c_chain.dll",
            "..\\..\\m11-layer\\build\\Release\\d5c_chain.dll",
            "..\\..\\build\\Release\\d5c_chain.dll",
            "..\\..\\..\\dlss5\\chain\\build\\Release\\d5c_chain.dll",
            "..\\..\\..\\dlss5\\m11d\\build\\Release\\d5c_chain.dll",
            "..\\..\\..\\dlss5\\m11-layer\\build\\Release\\d5c_chain.dll",
            "..\\..\\..\\dlss5\\m12-dxgi\\build\\Release\\d5c_chain.dll"
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
                hDll = LoadLibraryA(candidates[i]);
                if (hDll) {
                    m12_logf("[m12_dxgi] loaded d5c_chain.dll from candidate: %s", candidates[i]);
                    break;
                }
            }
            if (mod_path[0]) {
                char rel_cand[MAX_PATH];
                _snprintf_s(rel_cand, sizeof(rel_cand), _TRUNCATE, "%s%s", mod_path, candidates[i]);
                if (GetFileAttributesA(rel_cand) != INVALID_FILE_ATTRIBUTES) {
                    hDll = LoadLibraryA(rel_cand);
                    if (hDll) {
                        m12_logf("[m12_dxgi] loaded d5c_chain.dll from module-relative: %s", rel_cand);
                        break;
                    }
                }
            }
        }
    }

    if (!hDll) {
        hDll = LoadLibraryA("d5c_chain.dll");
        if (hDll) {
            m12_logf("[m12_dxgi] loaded d5c_chain.dll from standard search");
        }
    }

    if (!hDll) {
        m12_logf("[m12_dxgi] d5c_chain.dll not found; falling back to TCP socket");
        LeaveCriticalSection(&g_inproc_cs);
        return;
    }

    pfn_dlss5_create = (PFN_dlss5_create)GetProcAddress(hDll, "dlss5_create");
    pfn_dlss5_destroy = (PFN_dlss5_destroy)GetProcAddress(hDll, "dlss5_destroy");
    pfn_dlss5_process_frame = (PFN_dlss5_process_frame)GetProcAddress(hDll, "dlss5_process_frame");
    pfn_dlss5_set_scale = (PFN_dlss5_set_scale)GetProcAddress(hDll, "dlss5_set_scale");
    pfn_dlss5_set_gain = (PFN_dlss5_set_gain)GetProcAddress(hDll, "dlss5_set_gain");
    pfn_dlss5_set_blend = (PFN_dlss5_set_blend)GetProcAddress(hDll, "dlss5_set_blend");
    pfn_dlss5_interpolate_frame = (PFN_dlss5_interpolate_frame)GetProcAddress(hDll, "dlss5_interpolate_frame");
    pfn_dlss5_set_framegen = (PFN_dlss5_set_framegen)GetProcAddress(hDll, "dlss5_set_framegen");
    pfn_dlss5_get_framegen = (PFN_dlss5_get_framegen)GetProcAddress(hDll, "dlss5_get_framegen");

    if (!pfn_dlss5_create || !pfn_dlss5_destroy || !pfn_dlss5_process_frame) {
        m12_logf("[m12_dxgi] d5c_chain.dll missing exports; falling back to TCP");
        FreeLibrary(hDll);
        LeaveCriticalSection(&g_inproc_cs);
        return;
    }

    g_chain_dll = hDll;

    const char *weights = getenv("M12_WEIGHTS");
    if (!weights) weights = getenv("DLSS5_WEIGHTS_PATH");
    if (!weights) weights = getenv("NR_LAYER_WEIGHTS");

    const char *shaders = getenv("M12_SHADERS");
    if (!shaders) shaders = getenv("DLSS5_SHADER_DIR");
    if (!shaders) shaders = getenv("NR_LAYER_SHADERS");

    const char *scale_s = getenv("M12_SCALE");
    if (!scale_s) scale_s = getenv("NR_LAYER_SCALE");

    const char *gain_s = getenv("M12_GAIN");
    if (!gain_s) gain_s = getenv("NR_LAYER_GAIN");

    const char *blend_s = getenv("M12_BLEND");
    if (!blend_s) blend_s = getenv("NR_LAYER_BLEND");

    float scale = scale_s ? (float)atof(scale_s) : 1.0f;
    float gain = gain_s ? (float)atof(gain_s) : 1.0f;
    float blend = blend_s ? (float)atof(blend_s) : 1.0f;

    char weights_buf[MAX_PATH] = {0};
    if (!weights) {
        const char *cand_w[] = {
            "work\\mlxw\\dlssnr-logical.safetensors",
            "..\\..\\work\\mlxw\\dlssnr-logical.safetensors",
            "..\\..\\..\\work\\mlxw\\dlssnr-logical.safetensors",
            "..\\..\\..\\..\\work\\mlxw\\dlssnr-logical.safetensors"
        };
        for (size_t i = 0; i < sizeof(cand_w) / sizeof(cand_w[0]); i++) {
            if (GetFileAttributesA(cand_w[i]) != INVALID_FILE_ATTRIBUTES) {
                weights = cand_w[i];
                break;
            }
            if (mod_path[0]) {
                char probe[MAX_PATH];
                _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s%s", mod_path, cand_w[i]);
                if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                    _snprintf_s(weights_buf, sizeof(weights_buf), _TRUNCATE, "%s%s", mod_path, cand_w[i]);
                    weights = weights_buf;
                    break;
                }
            }
        }
    }

    char shaders_buf[MAX_PATH] = {0};
    if (!shaders) {
        const char *cand_sdirs[] = {
            ".\\",
            "dlss5\\m11d\\build\\Release\\",
            "dlss5\\chain\\build\\Release\\",
            "dlss5\\m8b-live\\shaders\\",
            "..\\..\\m11d\\build\\Release\\",
            "..\\..\\chain\\build\\Release\\",
            "..\\..\\..\\dlss5\\m11d\\build\\Release\\"
        };
        for (size_t i = 0; i < sizeof(cand_sdirs) / sizeof(cand_sdirs[0]); i++) {
            char probe[MAX_PATH];
            _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%sgemm.spv", cand_sdirs[i]);
            if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                shaders = cand_sdirs[i];
                break;
            }
            if (mod_path[0]) {
                _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s%sgemm.spv", mod_path, cand_sdirs[i]);
                if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                    _snprintf_s(shaders_buf, sizeof(shaders_buf), _TRUNCATE, "%s%s", mod_path, cand_sdirs[i]);
                    shaders = shaders_buf;
                    break;
                }
            }
        }
    }

    g_engine = pfn_dlss5_create(weights, shaders, scale, gain, blend);
    if (!g_engine) {
        m12_logf("[m12_dxgi] dlss5_create failed; falling back to TCP");
        FreeLibrary(g_chain_dll);
        g_chain_dll = NULL;
        LeaveCriticalSection(&g_inproc_cs);
        return;
    }

    const char *fg_env = getenv("M12_FRAMEGEN");
    if (!fg_env) fg_env = getenv("DLSS5_FRAMEGEN");
    if (fg_env && pfn_dlss5_set_framegen && g_engine) {
        int enable = (fg_env[0] == '1' || !strcmp(fg_env, "true") || !strcmp(fg_env, "on")) ? 1 : 0;
        pfn_dlss5_set_framegen(g_engine, enable);
    }

    g_inprocess_active = 1;
    m12_logf("[m12_dxgi] in-process DLSS 5 active (TCP bypassed, scale=%.2f gain=%.2f blend=%.2f)",
             scale, gain, blend);
    LeaveCriticalSection(&g_inproc_cs);
}

}  // namespace

void m12_dx12_shutdown_inprocess()
{
    ensure_inprocess_cs();
    EnterCriticalSection(&g_inproc_cs);
    if (g_engine && pfn_dlss5_destroy) {
        pfn_dlss5_destroy(g_engine);
        g_engine = NULL;
    }
    if (g_chain_dll) {
        FreeLibrary(g_chain_dll);
        g_chain_dll = NULL;
    }
    pfn_dlss5_create = NULL;
    pfn_dlss5_destroy = NULL;
    pfn_dlss5_process_frame = NULL;
    pfn_dlss5_set_scale = NULL;
    pfn_dlss5_set_gain = NULL;
    pfn_dlss5_set_blend = NULL;
    pfn_dlss5_interpolate_frame = NULL;
    pfn_dlss5_set_framegen = NULL;
    pfn_dlss5_get_framegen = NULL;
    g_inprocess_active = 0;
    g_inprocess_attempted = 0;
    LeaveCriticalSection(&g_inproc_cs);
}

M12Dx12Processor::M12Dx12Processor(IDXGISwapChain *sc, ID3D12CommandQueue *queue,
                                   UINT w, UINT h, DXGI_FORMAT fmt, int live_every)
    : sc_(sc), fence_event_(NULL), fence_value_(0), width_(w), height_(h),
      fmt_(fmt), fmt_supported_(false), needs_rbswap_(false), row_pitch_(0),
      live_every_(live_every > 0 ? live_every : 1), present_count_(0),
      present_trace_(0), have_processed_(false), paused_(false)
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
    InterlockedIncrement(&g_proc_count);
    ensure_inprocess();
    m12_logf("processor attached: %ux%u fmt %d rb-swap %d live every %d", w, h,
             (int)fmt, needs_rbswap_ ? 1 : 0, live_every_);
}

M12Dx12Processor::~M12Dx12Processor()
{
    if (fence_event_) CloseHandle(fence_event_);
    if (InterlockedDecrement(&g_proc_count) <= 0) {
        g_proc_count = 0;
        m12_dx12_shutdown_inprocess();
    }
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

// Backbuffer -> readback -> raw_ (packed B,G,R,A bytes, no daemon yet).
bool M12Dx12Processor::grabRaw(ID3D12Resource *bb)
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
    raw_.resize((std::size_t)width_ * height_ * 4);
    for (UINT y = 0; y < height_; y++)
        memcpy(raw_.data() + (std::size_t)y * width_ * 4,
               mapped + (std::size_t)y * row_pitch_, (std::size_t)width_ * 4);
    readback_->Unmap(0, NULL);

    if (needs_rbswap_) swizzle_rb(raw_.data(), raw_.size());
    return true;
}

// SEH helper functions (free of C++ objects with destructors to avoid C2712)
static int safe_process_frame(PFN_dlss5_process_frame pfn, void *engine, uint32_t w, uint32_t h,
                              const uint8_t *in, uint8_t *out, long frame)
{
    __try {
        return pfn(engine, w, h, in, out, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -999;
    }
}

static int safe_interpolate_frame(PFN_dlss5_interpolate_frame pfn, void *engine, uint32_t w, uint32_t h,
                                  const uint8_t *a, const uint8_t *b, uint8_t *out, float alpha)
{
    __try {
        return pfn(engine, w, h, a, b, out, alpha);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -999;
    }
}

// raw_ -> in-process DLSS 5 (or daemon fallback) -> processed_.
// Re-sending the same raw_ with a new daemon gain is exactly the "live knob on a frozen frame" reprocess.
bool M12Dx12Processor::sendRaw()
{
    if (raw_.empty()) return false;
    ensure_inprocess();

    if (g_inprocess_active && pfn_dlss5_process_frame && g_engine) {
        const char *gain_s = getenv("M12_GAIN");
        if (!gain_s) gain_s = getenv("NR_LAYER_GAIN");
        if (gain_s && pfn_dlss5_set_gain) {
            pfn_dlss5_set_gain(g_engine, (float)atof(gain_s));
        }

        std::vector<BYTE> reply(raw_.size());
        int r = safe_process_frame(pfn_dlss5_process_frame, g_engine, width_, height_,
                                   raw_.data(), reply.data(),
                                   (long)present_count_);
        if (r == 0) {
            processed_.swap(reply);
            have_processed_ = true;
            m12_logf("[m12_dxgi] in-process processed %ux%u (frame %llu)",
                     width_, height_, present_count_);
            dump_processed();
            return true;
        } else {
            m12_logf("[m12_dxgi] in-process dlss5_process_frame failed (%d); falling back to TCP", r);
        }
    }

    const uint32_t header[4] = {0x304E524Eu, width_, height_, 44u};
    std::vector<BYTE> reply(raw_.size());
    if (m12_exchange(header, raw_.data(), raw_.size(), reply.data(),
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

// Backbuffer -> readback -> daemon -> processed_ (packed B,G,R,A bytes).
bool M12Dx12Processor::captureToCpu(ID3D12Resource *bb)
{
    return grabRaw(bb) && sendRaw();
}

void M12Dx12Processor::dump_processed()
{
    static const char *path = getenv("M12_DUMP");
    if (!path || !have_processed_) return;
    dump_bgra_bmp(path, processed_, width_, height_);
}

void M12Dx12Processor::log_present_entry(int which, UINT flags)
{
    present_trace_++;
    if (present_trace_ <= 5 || present_trace_ % 60 == 0)
        m12_logf("present%s #%llu on sc %p flags 0x%x", which ? "1" : "",
                 present_trace_, sc_, flags);
}

void M12Dx12Processor::log_present_exit(int which)
{
    if (present_trace_ <= 5)
        m12_logf("present%s #%llu done", which ? "1" : "", present_trace_);
}

// buf -> upload buffer -> backbuffer (leaves bb in PRESENT state).
bool M12Dx12Processor::blitToBackbuffer(ID3D12Resource *bb, std::vector<BYTE> &buf)
{
    BYTE *mapped = NULL;
    if (FAILED(upload_->Map(0, NULL, (void **)&mapped))) return false;
    if (needs_rbswap_) swizzle_rb(buf.data(), buf.size());
    for (UINT y = 0; y < height_; y++)
        memcpy(mapped + (std::size_t)y * row_pitch_,
               buf.data() + (std::size_t)y * width_ * 4,
               (std::size_t)width_ * 4);
    if (needs_rbswap_) swizzle_rb(buf.data(), buf.size());
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

bool M12Dx12Processor::blitToBackbuffer(ID3D12Resource *bb)
{
    return blitToBackbuffer(bb, processed_);
}

// %TEMP%\m12_pause.flag: cheap runtime pause switch. While the flag exists
// every frame passes through untouched (full fps, zero GPU/TCP work).
static bool temp_flag_set(const char *name)
{
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return false;
    if (lstrlenA(path) + lstrlenA(name) + 1 > MAX_PATH) return false;
    lstrcatA(path, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool M12Dx12Processor::pauseFlagSet()
{
    return temp_flag_set("m12_pause.flag");
}

// %TEMP%\m13_freeze.flag: m13 overlay photo mode. While it exists the held
// RAW frame's processed result is re-blitted every present (the picture
// stands still; the game keeps running underneath).
bool M12Dx12Processor::freezeFlagSet()
{
    return temp_flag_set("m13_freeze.flag");
}

// %TEMP%\m13_reproc.flag: mtime bump = the manager pushed a new gain to the
// daemon; re-send the held raw frame so the frozen picture updates.
bool M12Dx12Processor::reprocChanged()
{
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return false;
    if (lstrlenA(path) + 16 > MAX_PATH) return false;
    lstrcatA(path, "m13_reproc.flag");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        reproc_seen_ = false;
        return false;
    }
    if (!reproc_seen_
        || fa.ftLastWriteTime.dwLowDateTime != reproc_stamp_.dwLowDateTime
        || fa.ftLastWriteTime.dwHighDateTime != reproc_stamp_.dwHighDateTime) {
        reproc_stamp_ = fa.ftLastWriteTime;
        reproc_seen_ = true;
        return true;
    }
    return false;
}

void M12Dx12Processor::onPresent()
{
    if (pauseFlagSet()) {
        if (!paused_) {
            paused_ = true;
            m12_logf("processing PAUSED (m12_pause.flag) - passthrough");
        }
        return;
    }
    if (paused_) {
        paused_ = false;
        have_processed_ = false;  // drop the stale frame, show live game again
        m12_logf("processing RESUMED");
    }
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

    // Freeze (photo mode): hold the frame captured when the flag appeared,
    // reprocess it from the raw original whenever the knob turns.
    if (freezeFlagSet()) {
        if (!raw_held_ || raw_.size() != (std::size_t)width_ * height_ * 4) {
            if (grabRaw(bb.Get())) {
                raw_held_ = true;
                sendRaw();
                m12_logf("frame frozen; knob turns reprocess the original");
            }
        } else if (reprocChanged()) {
            sendRaw();
        }
        if (have_processed_) blitToBackbuffer(bb.Get());
        return;
    }
    if (raw_held_) {
        raw_held_ = false;
        reproc_seen_ = false;
        m12_logf("freeze released - live processing continues");
    }

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
        if (have_processed_) blitToBackbuffer(bb.Get());
    } else {
        bool interpolated = false;
        ensure_inprocess();
        if (is_framegen_enabled() && g_engine && pfn_dlss5_interpolate_frame && have_processed_ &&
            processed_.size() == (std::size_t)width_ * height_ * 4) {
            if (grabRaw(bb.Get()) && !raw_.empty() && raw_.size() == (std::size_t)width_ * height_ * 4) {
                std::size_t needed = (std::size_t)width_ * height_ * 4;
                if (interp_.size() < needed) interp_.resize(needed);
                int r = safe_interpolate_frame(pfn_dlss5_interpolate_frame, g_engine, width_, height_,
                                                processed_.data(), raw_.data(),
                                                interp_.data(), 0.5f);
                if (r == 0) {
                    static UINT64 s_fg_count = 0;
                    s_fg_count++;
                    if (s_fg_count <= 5 || s_fg_count % 60 == 0) {
                        m12_logf("[m12_dxgi] framegen interpolated %ux%u (frame %llu, fg #%llu)",
                                 width_, height_, present_count_, s_fg_count);
                    }
                    blitToBackbuffer(bb.Get(), interp_);
                    interpolated = true;
                } else {
                    m12_logf("[m12_dxgi] dlss5_interpolate_frame failed (%d); falling back to stale frame", r);
                }
            }
        }
        if (!interpolated && have_processed_) {
            blitToBackbuffer(bb.Get());
        }
    }
}
