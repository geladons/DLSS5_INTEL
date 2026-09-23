// m12_hook.cpp - COM vtable hooking for the m12 dxgi proxy.
//
// The proxy hooks the three CreateSwapChain* entry points and the swapchain
// Present/Present1 entry points by patching the ORIGINAL vtables IN PLACE
// (VirtualProtect, write, restore). The object's vtable pointer is left
// untouched - this is the ReShade-style approach and it matters: some games
// (GTA5 Enhanced, anti-tamper) kill the process if a COM object's vtable
// pointer deviates from the module's known vtable, which is what the
// per-object copy approach triggers.
//
// Lookup scheme:
//   g_vtables: vtable ptr -> record of original entries (for call-through)
//   g_objects: interface ptr -> SwapchainState (processor); unknown objects
//              pass through to the original entry.
#pragma once
#include "m12_hook.h"

#include "m12_dx12.h"
#include "m12_hotkeys.h"
#include "m12_log.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace {

struct SwapchainState {
    M12Dx12Processor *proc;
};

CRITICAL_SECTION g_cs;
std::map<void *, std::map<int, void *>> g_orig;    // vtable -> idx -> orig fn
std::map<void *, SwapchainState *> g_objects;       // interface ptr -> state
bool g_restored = false;   // CTRL+ALT+Q detach: never patch again

void *orig_entry(void *obj, int idx)
{
    std::map<void *, std::map<int, void *>>::iterator vt = g_orig.find(*(void ***)obj);
    if (vt == g_orig.end()) return NULL;
    std::map<int, void *>::iterator e = vt->second.find(idx);
    return e == vt->second.end() ? NULL : e->second;
}

// Patches the given entries of the object's CURRENT vtable in place.
// Idempotent per vtable. Returns FALSE if the page could not be opened.
bool patch_in_place(void *obj, int idx0, void *fn0, int idx1, void *fn1, int idx2,
                    void *fn2, int idx3, void *fn3)
{
    void **vt = *(void ***)obj;
    if (g_restored) return false;
    if (g_orig.find(vt) != g_orig.end()) return true;
    DWORD old_protect = 0;
    if (!VirtualProtect(vt, 64 * sizeof(void *), PAGE_READWRITE, &old_protect)) {
        m12_logf("VirtualProtect failed on vtable %p (err %lu); cannot hook in place",
                 vt, GetLastError());
        return false;
    }
    std::map<int, void *> &rec = g_orig[vt];
    struct {
        int idx;
        void *fn;
    } patches[4] = {{idx0, fn0}, {idx1, fn1}, {idx2, fn2}, {idx3, fn3}};
    for (int i = 0; i < 4; i++) {
        if (!patches[i].fn) continue;
        rec[patches[i].idx] = vt[patches[i].idx];
        vt[patches[i].idx] = patches[i].fn;
    }
    DWORD ignored = 0;
    VirtualProtect(vt, 64 * sizeof(void *), old_protect, &ignored);
    m12_logf("vtable %p patched in place (%u entries)", vt, (unsigned)rec.size());
    return true;
}

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

HRESULT WINAPI hooked_Present(IDXGISwapChain *sc, UINT sync_interval, UINT flags)
{
    EnterCriticalSection(&g_cs);
    std::map<void *, SwapchainState *>::iterator st = g_objects.find(sc);
    void *orig = orig_entry(sc, 8);
    if (st != g_objects.end() && st->second->proc) {
        st->second->proc->log_present_entry(0, flags);
        st->second->proc->onPresent();
        st->second->proc->log_present_exit(0);
    }
    LeaveCriticalSection(&g_cs);
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    return ((HRESULT(WINAPI *)(IDXGISwapChain *, UINT, UINT))orig)(sc, sync_interval,
                                                                   flags);
}

HRESULT WINAPI hooked_Present1(IDXGISwapChain1 *sc, UINT sync_interval, UINT flags,
                               const DXGI_PRESENT_PARAMETERS *params)
{
    EnterCriticalSection(&g_cs);
    std::map<void *, SwapchainState *>::iterator st = g_objects.find(sc);
    void *orig = orig_entry(sc, 22);
    if (st != g_objects.end() && st->second->proc) {
        st->second->proc->log_present_entry(1, flags);
        st->second->proc->onPresent();
        st->second->proc->log_present_exit(1);
    }
    LeaveCriticalSection(&g_cs);
    if (!orig) return DXGI_ERROR_INVALID_CALL;
    return ((HRESULT(WINAPI *)(IDXGISwapChain1 *, UINT, UINT,
                               const DXGI_PRESENT_PARAMETERS *))orig)(
        sc, sync_interval, flags, params);
}

void attach_swapchain(IDXGISwapChain *sc, ID3D12CommandQueue *queue, UINT w, UINT h,
                      DXGI_FORMAT fmt)
{
    EnterCriticalSection(&g_cs);
    if (g_objects.find(sc) != g_objects.end()) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    SwapchainState *st = new SwapchainState();
    st->proc = queue ? new M12Dx12Processor(sc, queue, w, h, fmt, live_every()) : NULL;
    g_objects[sc] = st;

    // Present sits at entry 8 of every IDXGISwapChainN view; Present1 at 22
    // of the IDXGISwapChain1 view. Patch each distinct vtable in place and
    // register every view pointer so any of them reaches the processor.
    patch_in_place(sc, 8, (void *)&hooked_Present, 0, NULL, 0, NULL, 0, NULL);
    g_objects[sc] = st;
    IDXGISwapChain1 *sc1 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        if (*(void ***)sc1 != *(void ***)sc)
            patch_in_place(sc1, 8, (void *)&hooked_Present, 22,
                           (void *)&hooked_Present1, 0, NULL, 0, NULL);
        g_objects[sc1] = st;
    }
    IDXGISwapChain3 *sc3 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
        if (*(void ***)sc3 != *(void ***)sc && *(void ***)sc3 != *(void ***)sc1)
            patch_in_place(sc3, 8, (void *)&hooked_Present, 0, NULL, 0, NULL, 0, NULL);
        g_objects[sc3] = st;
    }
    if (sc1) sc1->Release();
    if (sc3) sc3->Release();
    LeaveCriticalSection(&g_cs);
}

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
    EnterCriticalSection(&g_cs);  // reentrant: attach below re-acquires
    void *orig = orig_entry(factory, 10);
    if (!orig) {
        LeaveCriticalSection(&g_cs);
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                      IDXGISwapChain **))orig)(factory, device, desc,
                                                              pp_swapchain);
    if (SUCCEEDED(hr) && pp_swapchain && *pp_swapchain)
        after_create_swapchain(device, *pp_swapchain, desc);
    LeaveCriticalSection(&g_cs);
    return hr;
}

HRESULT WINAPI hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *device,
                                             HWND hwnd,
                                             const DXGI_SWAP_CHAIN_DESC1 *desc1,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
                                             IDXGIOutput *restrict_to,
                                             IDXGISwapChain1 **pp_swapchain)
{
    EnterCriticalSection(&g_cs);
    void *orig = orig_entry(factory, 15);
    if (!orig) {
        LeaveCriticalSection(&g_cs);
        return DXGI_ERROR_INVALID_CALL;
    }
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
    LeaveCriticalSection(&g_cs);
    return hr;
}

HRESULT WINAPI hooked_CreateSwapChainForCoreWindow(IDXGIFactory2 *factory,
                                                   IUnknown *device, IUnknown *window,
                                                   IUnknown *surface,
                                                   const DXGI_SWAP_CHAIN_DESC1 *desc1,
                                                   IDXGIOutput *restrict_to,
                                                   IDXGISwapChain1 **pp_swapchain)
{
    EnterCriticalSection(&g_cs);
    void *orig = orig_entry(factory, 16);
    if (!orig) {
        LeaveCriticalSection(&g_cs);
        return DXGI_ERROR_INVALID_CALL;
    }
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
    LeaveCriticalSection(&g_cs);
    return hr;
}

HRESULT WINAPI hooked_CreateSwapChainForComposition(IDXGIFactory2 *factory,
                                                    IUnknown *device,
                                                    const DXGI_SWAP_CHAIN_DESC1 *desc1,
                                                    IDXGIOutput *restrict_to,
                                                    IDXGISwapChain1 **pp_swapchain)
{
    EnterCriticalSection(&g_cs);
    void *orig = orig_entry(factory, 24);
    if (!orig) {
        LeaveCriticalSection(&g_cs);
        return DXGI_ERROR_INVALID_CALL;
    }
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
    LeaveCriticalSection(&g_cs);
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
    if (g_restored) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    // 10 CreateSwapChain, 15 ForHwnd, 16 ForCoreWindow, 24 ForComposition.
    patch_in_place(factory, 10, (void *)&hooked_CreateSwapChain, 15,
                   (void *)&hooked_CreateSwapChainForHwnd, 16,
                   (void *)&hooked_CreateSwapChainForCoreWindow, 24,
                   (void *)&hooked_CreateSwapChainForComposition);
    LeaveCriticalSection(&g_cs);
    m12_hotkeys_ensure_started();
}

void m12_hook_restore_all()
{
    EnterCriticalSection(&g_cs);
    unsigned n_vt = (unsigned)g_orig.size();
    unsigned n_obj = (unsigned)g_objects.size();
    for (std::map<void *, std::map<int, void *>>::iterator vt = g_orig.begin();
         vt != g_orig.end(); ++vt) {
        DWORD old_protect = 0;
        if (!VirtualProtect(vt->first, 64 * sizeof(void *), PAGE_READWRITE,
                            &old_protect))
            continue;
        for (std::map<int, void *>::iterator e = vt->second.begin();
             e != vt->second.end(); ++e)
            ((void **)vt->first)[e->first] = e->second;
        DWORD ignored = 0;
        VirtualProtect(vt->first, 64 * sizeof(void *), old_protect, &ignored);
    }
    for (std::map<void *, SwapchainState *>::iterator o = g_objects.begin();
         o != g_objects.end(); ++o) {
        if (o->second) {
            delete o->second->proc;
            delete o->second;
        }
    }
    g_objects.clear();
    g_orig.clear();
    g_restored = true;
    LeaveCriticalSection(&g_cs);
    m12_logf("proxy detached: %u vtables restored, %u objects released", n_vt,
             n_obj);
}
