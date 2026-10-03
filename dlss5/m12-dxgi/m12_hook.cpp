// SPDX-License-Identifier: Apache-2.0
// m12_hook.cpp - COM vtable hooking for the m12 dxgi proxy.
//
// Hooking strategy: per-object VTable SWAP. For every hooked COM object a
// private copy of its vtable is allocated (VTABLE_COPY_SLOTS entries), the
// hook entries are written into the COPY, and only then the object's vtable
// pointer is redirected at the copy. The module image (dxgi.dll .rdata) is
// NEVER modified.
//
// Why: RDR2's integrity check (int3 in RDR2.exe ~60 s in, fault offset
// 0x25b520c, deterministic) fires only when the shared factory vtable pages
// are patched in place - a scanner can trivially hash the module's read-only
// pages. The object-level swap leaves every byte of dxgi.dll untouched; the
// only deviation is a single pointer inside one heap object, which is what
// ReShade-style proxies do and what survives the common vtable-content
// checks. (The older comment claimed per-object copies are what anti-tamper
// detects - the 2026-09-30 field test on RDR2 proved the opposite.)
//
// Lookup scheme:
//   g_orig:    copy-vtable ptr -> idx -> original fn (call-through)
//   g_obj_vt:  object ptr -> original vtable ptr (for restore)
//   g_objects: interface ptr -> SwapchainState (processor); unknown objects
//              pass through to the original entry.
#pragma once
#include "m12_hook.h"

#include "m12_dx12.h"
#include "m12_hotkeys.h"
#include "m12_log.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace {

struct SwapchainState {
    M12Dx12Processor *proc;
};

CRITICAL_SECTION g_cs;
std::map<void *, std::map<int, void *>> g_orig;    // copy vtable -> idx -> orig fn
std::map<void *, SwapchainState *> g_objects;       // interface ptr -> state
std::map<void *, void **> g_obj_vt;                 // object ptr -> orig vtable
std::map<void *, void **> g_obj_copy;               // object ptr -> copy vtable
bool g_restored = false;   // CTRL+ALT+Q detach: never patch again

// Vtables we touch (IDXGIFactoryN, IDXGISwapChainN) have <= ~30 entries; a
// fixed 64-entry copy covers any current version. Entries beyond the real
// vtable size are copied verbatim from the following image data and are
// simply never invoked through our hooks.
enum { VTABLE_COPY_SLOTS = 64 };

void *orig_entry(void *obj, int idx)
{
    std::map<void *, std::map<int, void *>>::iterator vt = g_orig.find(*(void ***)obj);
    if (vt == g_orig.end()) return NULL;
    std::map<int, void *>::iterator e = vt->second.find(idx);
    return e == vt->second.end() ? NULL : e->second;
}

// Swap-and-hook: if the object still points at its ORIGINAL vtable, build the
// private copy and redirect the object at it; then install fn at idx inside
// the copy (idempotent per idx). The module image is never written to.
//
// Address-reuse hazard: g_obj_copy is keyed by raw COM pointer. When a hooked
// object is released its address can be handed to a NEW COM object, whose
// vptr then points at an original (read-only image) vtable while the stale
// map entry claims the copy is already installed. Writing a hook into that
// original vtable faults with 0xC0000005 - observed in the field as a crash
// inside this module on Witcher 3 DX12 (factory create/release churn during
// adapter probing). Detect the mismatch by comparing the live vptr with the
// recorded copy and evict the stale bookkeeping before hooking.
bool hook_entry(void *obj, int idx, void *fn)
{
    if (!obj || !fn || g_restored || idx < 0 || idx >= VTABLE_COPY_SLOTS)
        return false;
    void **cur = *(void ***)obj;
    std::map<void *, void **>::iterator cp = g_obj_copy.find(obj);
    if (cp != g_obj_copy.end() && cur != cp->second) {
        g_orig.erase(cp->second);
        g_obj_copy.erase(cp);
        g_obj_vt.erase(obj);
        m12_logf("stale hook entry evicted for object %p (address reused)",
                 obj);
    }
    if (g_obj_copy.find(obj) == g_obj_copy.end()) {
        if (g_obj_vt.find(obj) != g_obj_vt.end())
            return true;      // already swapped on this object; cur == copy
        void **copy = new (std::nothrow) void *[VTABLE_COPY_SLOTS];
        if (!copy) {
            m12_logf("vtable copy alloc failed for object %p", obj);
            return false;
        }
        memcpy(copy, cur, VTABLE_COPY_SLOTS * sizeof(void *));
        g_obj_vt[obj] = cur;
        g_obj_copy[obj] = copy;
        *reinterpret_cast<void ***>(obj) = copy;
        cur = copy;
    }
    std::map<int, void *> &rec = g_orig[cur];
    if (rec.find(idx) != rec.end()) return true;
    rec[idx] = cur[idx];
    cur[idx] = fn;
    return true;
}

bool patch_object(void *obj, int idx0, void *fn0, int idx1, void *fn1, int idx2,
                  void *fn2, int idx3, void *fn3)
{
    if (!obj || g_restored) return false;
    struct {
        int idx;
        void *fn;
    } patches[4] = {{idx0, fn0}, {idx1, fn1}, {idx2, fn2}, {idx3, fn3}};
    bool ok = true;
    for (int i = 0; i < 4; i++) {
        if (patches[i].fn) {
            if (!hook_entry(obj, patches[i].idx, patches[i].fn)) ok = false;
        }
    }
    return ok;
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
    std::map<void *, SwapchainState *>::iterator old = g_objects.find(sc);
    if (old != g_objects.end()) {
        // A live entry means the recorded swapchain still carries our copy
        // vtable. If the vptr moved off the copy, the old COM object was
        // released and this address was reused for a new swapchain - drop
        // the dead state and attach fresh instead of skipping it.
        std::map<void *, void **>::iterator cp = g_obj_copy.find(sc);
        if (cp != g_obj_copy.end() && *(void ***)sc == cp->second) {
            LeaveCriticalSection(&g_cs);
            return;
        }
        m12_logf("stale swapchain state evicted for %p (address reused)", sc);
        delete old->second->proc;
        delete old->second;
        g_objects.erase(old);
    }
    SwapchainState *st = new SwapchainState();
    st->proc = queue ? new M12Dx12Processor(sc, queue, w, h, fmt, live_every()) : NULL;
    g_objects[sc] = st;

    // Present sits at entry 8 of every IDXGISwapChainN view; Present1 at 22
    // of the IDXGISwapChain1 view. Swap a private vtable copy into each
    // distinct object and register every view pointer so any of them
    // reaches the processor.
    patch_object(sc, 8, (void *)&hooked_Present, 0, NULL, 0, NULL, 0, NULL);
    g_objects[sc] = st;
    IDXGISwapChain1 *sc1 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        if (*(void ***)sc1 != *(void ***)sc)
            patch_object(sc1, 8, (void *)&hooked_Present, 22,
                         (void *)&hooked_Present1, 0, NULL, 0, NULL);
        g_objects[sc1] = st;
    }
    IDXGISwapChain3 *sc3 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
        if (*(void ***)sc3 != *(void ***)sc && *(void ***)sc3 != *(void ***)sc1)
            patch_object(sc3, 8, (void *)&hooked_Present, 0, NULL, 0, NULL, 0, NULL);
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
    m12_logf("CreateSwapChain entry (%ux%u)", desc ? desc->BufferDesc.Width : 0,
             desc ? desc->BufferDesc.Height : 0);
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                      IDXGISwapChain **))orig)(factory, device, desc,
                                                              pp_swapchain);
    m12_logf("CreateSwapChain returned 0x%lx", (unsigned long)hr);
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
    m12_logf("CreateSwapChainForHwnd entry (%ux%u)", desc1 ? desc1->Width : 0,
             desc1 ? desc1->Height : 0);
    HRESULT hr = ((HRESULT(WINAPI *)(IDXGIFactory2 *, IUnknown *, HWND,
                                      const DXGI_SWAP_CHAIN_DESC1 *,
                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
                                      IDXGIOutput *, IDXGISwapChain1 **))orig)(
        factory, device, hwnd, desc1, fs, restrict_to, pp_swapchain);
    m12_logf("CreateSwapChainForHwnd returned 0x%lx", (unsigned long)hr);
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

HRESULT WINAPI hooked_Factory_QueryInterface(IUnknown *obj, const IID &riid,
                                             void **ppv);

// QI results that are factories must get the full factory hook; every other
// interface gets only its slot-0 QueryInterface swapped so factory views
// reached THROUGH it (e.g. IDXGIObject view -> IDXGIFactoryN) still route
// back into this function. Patching factory slots onto a non-factory vtable
// would mis-wire that object's real methods.
bool is_factory_iid(const IID &riid)
{
    // IID_IDXGIFactory5/6/7 are declared by dxgi1_6.h but the GUIDs live in a
    // dxgi.lib newer than this toolchain's - defined here from the public
    // SDK values (stable ABI constants).
    static const IID kFactory5 = {0x7632e1f5, 0xee65, 0x4dca,
                                  {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
    static const IID kFactory6 = {0xc1b6694f, 0xff09, 0x44a9,
                                  {0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}};
    static const IID kFactory7 = {0xa4966eed, 0x76db, 0x44da,
                                  {0x84, 0xc1, 0xee, 0x9a, 0x7a, 0xfb, 0x20, 0xa8}};
    static const IID *ids[] = {
        &IID_IDXGIFactory,  &IID_IDXGIFactory1, &IID_IDXGIFactory2,
        &IID_IDXGIFactory3, &IID_IDXGIFactory4, &kFactory5,
        &kFactory6,         &kFactory7,
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        if (IsEqualIID(riid, *ids[i])) return true;
    return false;
}

void hook_object_qi(void *obj)
{
    if (!obj || disabled()) return;
    patch_object(obj, 0, (void *)&hooked_Factory_QueryInterface, 0, NULL, 0,
                 NULL, 0, NULL);
}

HRESULT WINAPI hooked_Factory_QueryInterface(IUnknown *obj, const IID &riid, void **ppv)
{
    void *orig = orig_entry(obj, 0);
    if (!orig) return E_NOINTERFACE;
    HRESULT hr = ((HRESULT(WINAPI *)(IUnknown *, const IID &, void **))orig)(obj, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (is_factory_iid(riid))
            m12_hook_factory(*ppv);
        else
            hook_object_qi(*ppv);
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
    if (g_restored) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    // 0 QueryInterface, 10 CreateSwapChain, 15 ForHwnd, 16 ForCoreWindow, 24 ForComposition.
    patch_object(factory, 0, (void *)&hooked_Factory_QueryInterface, 10,
                 (void *)&hooked_CreateSwapChain, 15,
                 (void *)&hooked_CreateSwapChainForHwnd, 16,
                 (void *)&hooked_CreateSwapChainForCoreWindow);
    hook_entry(factory, 24, (void *)&hooked_CreateSwapChainForComposition);
    LeaveCriticalSection(&g_cs);
    m12_hotkeys_ensure_started();
}

void m12_hook_restore_all()
{
    EnterCriticalSection(&g_cs);
    unsigned n_vt = (unsigned)g_obj_vt.size();
    unsigned n_obj = (unsigned)g_objects.size();
    // Point every hooked object back at its original vtable. The private
    // copies are intentionally leaked (bytes, and a racing QI may still be
    // running through one); the process sees only the unmodified image.
    for (std::map<void *, void **>::iterator it = g_obj_vt.begin();
         it != g_obj_vt.end(); ++it)
        *reinterpret_cast<void ***>(it->first) = it->second;
    for (std::map<void *, SwapchainState *>::iterator o = g_objects.begin();
         o != g_objects.end(); ++o) {
        if (o->second) {
            delete o->second->proc;
            delete o->second;
        }
    }
    g_objects.clear();
    g_obj_vt.clear();
    g_obj_copy.clear();
    g_orig.clear();
    g_restored = true;
    LeaveCriticalSection(&g_cs);
    m12_logf("proxy detached: %u objects restored, %u processors released",
             n_vt, n_obj);
}
