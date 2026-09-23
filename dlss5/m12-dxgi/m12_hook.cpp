// m12_hook.cpp - COM vtable hooking for the m12 dxgi proxy (see m12_hook.h).
#include "m12_hook.h"

#include "m12_dx12.h"
#include "m12_log.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdlib>
#include <map>
#include <vector>

namespace {

// One private vtable copy per hooked object.
struct VtableCopy {
    void **obj;         // hooked interface pointer (first member is the vtable)
    void **orig;        // original vtable
    std::vector<void *> copy;  // patched copy, installed at *obj
};

// Swapchain state shared by every vtable view of the same swapchain object.
struct SwapchainState {
    std::vector<VtableCopy *> views;
    M12Dx12Processor *proc;
    void *orig_present;
    void *orig_present1;
    bool is_d3d12;
};

CRITICAL_SECTION g_cs;
std::map<void *, SwapchainState *> g_by_object;   // object ptr -> state
std::map<void *, VtableCopy *> g_by_vtable_view;  // interface ptr -> copy

// Copies up to `count` vtable entries and installs the patched copy.
// Returns NULL if the object was already hooked.
VtableCopy *hook_object_vtable(void *obj, size_t count)
{
    void **vtable = *(void ***)obj;
    VtableCopy *vc = new VtableCopy();
    vc->obj = (void **)obj;
    vc->orig = vtable;
    vc->copy.assign(vtable, vtable + count);
    *(void ***)obj = vc->copy.data();
    return vc;
}

void patch_entry(VtableCopy *vc, size_t idx, void *fn) { vc->copy[idx] = fn; }

int live_every()
{
    static int every = -1;
    if (every < 0) {
        const char *env = getenv("M12_LIVE");
        every = env ? atoi(env) : 4;
        if (every <= 0) every = 4;
    }
    return every;
}

bool disabled()
{
    static int off = -1;
    if (off < 0) {
        const char *env = getenv("M12_DISABLE");
        off = (env && env[0] == '1') ? 1 : 0;
    }
    return off == 1;
}

SwapchainState *find_state(void *obj)
{
    std::map<void *, SwapchainState *>::iterator it = g_by_object.find(obj);
    return it == g_by_object.end() ? NULL : it->second;
}

HRESULT WINAPI hooked_Present(IDXGISwapChain *sc, UINT sync_interval, UINT flags)
{
    EnterCriticalSection(&g_cs);
    SwapchainState *st = find_state(sc);
    void *orig = st ? st->orig_present : NULL;
    if (st && st->proc && st->is_d3d12) st->proc->onPresent();
    LeaveCriticalSection(&g_cs);
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    return ((HRESULT(WINAPI *)(IDXGISwapChain *, UINT, UINT))orig)(sc, sync_interval,
                                                                   flags);
}

HRESULT WINAPI hooked_Present1(IDXGISwapChain1 *sc, UINT sync_interval, UINT flags,
                               const DXGI_PRESENT_PARAMETERS *params)
{
    EnterCriticalSection(&g_cs);
    SwapchainState *st = find_state(sc);
    void *orig = st ? st->orig_present1 : NULL;
    if (st && st->proc && st->is_d3d12) st->proc->onPresent();
    LeaveCriticalSection(&g_cs);
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    return ((HRESULT(WINAPI *)(IDXGISwapChain1 *, UINT, UINT,
                               const DXGI_PRESENT_PARAMETERS *))orig)(
        sc, sync_interval, flags, params);
}

// Hooks a swapchain object: Present always, Present1 on the IDXGISwapChain1
// view when the object exposes one. `queue` is non-NULL for D3D12 devices.
void attach_swapchain(IDXGISwapChain *sc, ID3D12CommandQueue *queue, UINT w, UINT h,
                      DXGI_FORMAT fmt)
{
    EnterCriticalSection(&g_cs);
    if (find_state(sc)) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    SwapchainState *st = new SwapchainState();
    st->proc = NULL;
    st->orig_present = NULL;
    st->orig_present1 = NULL;
    st->is_d3d12 = queue != NULL;
    if (queue)
        st->proc = new M12Dx12Processor(sc, queue, w, h, fmt, live_every());

    // Classic IDXGISwapChain view: entries IUnknown(3) + IDXGIObject(4) +
    // IDXGIDeviceSubObject(1) -> Present sits at index 8.
    VtableCopy *v1 = hook_object_vtable(sc, 40);
    st->orig_present = v1->orig[8];
    patch_entry(v1, 8, (void *)&hooked_Present);
    st->views.push_back(v1);
    g_by_vtable_view[sc] = v1;
    g_by_object[sc] = st;

    // IDXGISwapChain1 view (same object, different vtable): Present1 is 22.
    IDXGISwapChain1 *sc1 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 &&
        *(void ***)sc1 != v1->orig) {
        VtableCopy *v2 = hook_object_vtable(sc1, 48);
        st->orig_present1 = v2->orig[22];
        patch_entry(v2, 8, (void *)&hooked_Present);
        patch_entry(v2, 22, (void *)&hooked_Present1);
        st->orig_present = v2->orig[8];
        st->views.push_back(v2);
        g_by_vtable_view[sc1] = v2;
        g_by_object[sc1] = st;
        m12_logf("swapchain %p exposes IDXGISwapChain1 (vtable %p): Present1 hooked",
                 sc, *(void ***)sc1);
    }
    if (sc1) sc1->Release();
    LeaveCriticalSection(&g_cs);
}

// Common tail of the three CreateSwapChain* hooks: QI the pDevice for a
// D3D12 command queue (that is what DXGI takes for D3D12 swapchains) and
// attach to the returned swapchain.
void after_create_swapchain(IUnknown *device, IDXGISwapChain *sc,
                            DXGI_SWAP_CHAIN_DESC *desc)
{
    if (!device || !sc || !desc) return;
    ID3D12CommandQueue *queue = NULL;
    bool d3d12 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)));
    if (d3d12) {
        m12_logf("D3D12 swapchain %p %ux%u fmt %d attached (queue %p)", sc,
                 desc->BufferDesc.Width, desc->BufferDesc.Height,
                 (int)desc->BufferDesc.Format, queue);
    } else {
        m12_logf("non-D3D12 swapchain %p (%ux%u fmt %d) - v0 hook is DX12 only, "
                 "present untouched", sc, desc->BufferDesc.Width,
                 desc->BufferDesc.Height, (int)desc->BufferDesc.Format);
    }
    attach_swapchain(sc, d3d12 ? queue : NULL, desc->BufferDesc.Width,
                     desc->BufferDesc.Height, desc->BufferDesc.Format);
    if (queue) queue->Release();
}

HRESULT WINAPI hooked_CreateSwapChain(IDXGIFactory *factory, IUnknown *device,
                                      DXGI_SWAP_CHAIN_DESC *desc,
                                      IDXGISwapChain **pp_swapchain)
{
    m12_logf("CreateSwapChain called: factory %p device %p %ux%u fmt %d", factory,
             device, desc ? desc->BufferDesc.Width : 0,
             desc ? desc->BufferDesc.Height : 0, desc ? (int)desc->BufferDesc.Format : -1);
    EnterCriticalSection(&g_cs);  // reentrant: attach below re-acquires
    VtableCopy *vc = g_by_vtable_view[(void *)factory];
    void *orig = vc ? vc->orig[10] : NULL;
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                      IDXGISwapChain **))orig)(factory, device, desc,
                                                              pp_swapchain);
    m12_logf("CreateSwapChain orig returned 0x%08lx, sc %p", hr,
             pp_swapchain ? *pp_swapchain : NULL);
    if (SUCCEEDED(hr) && pp_swapchain && *pp_swapchain)
        after_create_swapchain(device, *pp_swapchain, desc);
    return hr;
}

HRESULT WINAPI hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *device,
                                             HWND hwnd,
                                             const DXGI_SWAP_CHAIN_DESC1 *desc1,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
                                             IDXGIOutput *restrict_to,
                                             IDXGISwapChain1 **pp_swapchain)
{
    EnterCriticalSection(&g_cs);  // reentrant: attach below re-acquires
    VtableCopy *vc = g_by_vtable_view[(void *)factory];
    void *orig = vc ? vc->orig[15] : NULL;
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory2 *, IUnknown *, HWND,
                                      const DXGI_SWAP_CHAIN_DESC1 *,
                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
                                      IDXGIOutput *, IDXGISwapChain1 **))orig)(
        factory, device, hwnd, desc1, fs, restrict_to, pp_swapchain);
    if (SUCCEEDED(hr) && pp_swapchain && *pp_swapchain && desc1) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = desc1->Width;
        desc.BufferDesc.Height = desc1->Height;
        desc.BufferDesc.Format = desc1->Format;
        after_create_swapchain(device, *pp_swapchain, &desc);
    }
    return hr;
}

HRESULT WINAPI hooked_CreateSwapChainForCoreWindow(IDXGIFactory2 *factory,
                                                   IUnknown *device, IUnknown *window,
                                                   IUnknown *surface,
                                                   const DXGI_SWAP_CHAIN_DESC1 *desc1,
                                                   IDXGIOutput *restrict_to,
                                                   IDXGISwapChain1 **pp_swapchain)
{
    EnterCriticalSection(&g_cs);  // reentrant: attach below re-acquires
    VtableCopy *vc = g_by_vtable_view[(void *)factory];
    void *orig = vc ? vc->orig[16] : NULL;
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory2 *, IUnknown *, IUnknown *,
                                      IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                      IDXGIOutput *, IDXGISwapChain1 **))orig)(
        factory, device, window, surface, desc1, restrict_to, pp_swapchain);
    if (SUCCEEDED(hr) && pp_swapchain && *pp_swapchain && desc1) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = desc1->Width;
        desc.BufferDesc.Height = desc1->Height;
        desc.BufferDesc.Format = desc1->Format;
        after_create_swapchain(device, *pp_swapchain, &desc);
    }
    return hr;
}

HRESULT WINAPI hooked_CreateSwapChainForComposition(IDXGIFactory2 *factory,
                                                    IUnknown *device,
                                                    const DXGI_SWAP_CHAIN_DESC1 *desc1,
                                                    IDXGIOutput *restrict_to,
                                                    IDXGISwapChain1 **pp_swapchain)
{
    EnterCriticalSection(&g_cs);  // reentrant: attach below re-acquires
    VtableCopy *vc = g_by_vtable_view[(void *)factory];
    void *orig = vc ? vc->orig[24] : NULL;
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory2 *, IUnknown *,
                                      const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *,
                                      IDXGISwapChain1 **))orig)(
        factory, device, desc1, restrict_to, pp_swapchain);
    if (SUCCEEDED(hr) && pp_swapchain && *pp_swapchain && desc1) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = desc1->Width;
        desc.BufferDesc.Height = desc1->Height;
        desc.BufferDesc.Format = desc1->Format;
        after_create_swapchain(device, *pp_swapchain, &desc);
    }
    return hr;
}

}  // namespace

void m12_hook_init()
{
    static LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        m12_logf("m12 dxgi proxy loaded (pid %lu, live every %d%s)", GetCurrentProcessId(),
                 live_every(), disabled() ? ", DISABLED" : "");
    }
}

void m12_hook_factory(void *factory)
{
    if (!factory || disabled()) return;
    EnterCriticalSection(&g_cs);
    if (g_by_vtable_view.find(factory) != g_by_vtable_view.end()) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    // 48 entries cover IDXGIFactory7 (RegisterAdaptersChangedEvent at 31)
    // with headroom for anything newer the runtime may dispatch through.
    VtableCopy *vc = hook_object_vtable(factory, 48);
    patch_entry(vc, 10, (void *)&hooked_CreateSwapChain);
    patch_entry(vc, 15, (void *)&hooked_CreateSwapChainForHwnd);
    patch_entry(vc, 16, (void *)&hooked_CreateSwapChainForCoreWindow);
    patch_entry(vc, 24, (void *)&hooked_CreateSwapChainForComposition);
    g_by_vtable_view[factory] = vc;

    // The same object may expose an IID_IDXGIFactory2 view with its own
    // vtable; hook that one too so QI'd callers are still intercepted.
    m12_logf("factory %p: main view hooked, querying Factory2 view", factory);
    IDXGIFactory2 *f2 = NULL;
    HRESULT qhr = ((IUnknown *)factory)->QueryInterface(IID_PPV_ARGS(&f2));
    m12_logf("factory %p: Factory2 QI hr=0x%08lx ptr=%p", factory, qhr, (void *)f2);
    if (SUCCEEDED(qhr) && f2 && (void *)f2 != factory) {
        if (g_by_vtable_view.find(f2) == g_by_vtable_view.end()) {
            VtableCopy *vc2 = hook_object_vtable(f2, 48);
            patch_entry(vc2, 10, (void *)&hooked_CreateSwapChain);
            patch_entry(vc2, 15, (void *)&hooked_CreateSwapChainForHwnd);
            patch_entry(vc2, 16, (void *)&hooked_CreateSwapChainForCoreWindow);
            patch_entry(vc2, 24, (void *)&hooked_CreateSwapChainForComposition);
            g_by_vtable_view[f2] = vc2;
            m12_logf("factory %p: IDXGIFactory2 view %p hooked too", factory, f2);
        }
    }
    if (f2) f2->Release();
    LeaveCriticalSection(&g_cs);
    m12_logf("factory %p hooked (vtable %p)", factory,
             g_by_vtable_view[factory]->orig);
}
