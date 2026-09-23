// m12_exports.cpp - export surface of the m12 dxgi proxy.
//
// CreateDXGIFactory/1/2 are forwarded to the system dxgi.dll (loaded by its
// fully qualified system32 path, so it can never resolve to this proxy
// again) and the returned factory object is vtable-hooked (m12_hook.cpp).
// The remaining 19 exports are internal DXGI helpers that games never call;
// they are exposed so GetProcAddress succeeds and answer with a safe failure.
#include "m12_hook.h"
#include "m12_log.h"

#include <windows.h>

namespace {

HMODULE real_module()
{
    static HMODULE mod = NULL;
    if (!mod) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return NULL;
        wcscat_s(path, MAX_PATH, L"\\dxgi.dll");
        mod = LoadLibraryW(path);
        if (!mod)
            m12_logf("FATAL: cannot load system dxgi.dll (%lu)", GetLastError());
    }
    return mod;
}

void *real_proc(const char *name)
{
    HMODULE mod = real_module();
    return mod ? (void *)GetProcAddress(mod, name) : NULL;
}

void stub_called(const char *name)
{
    static LONG warned = 0;
    if (InterlockedIncrement(&warned) < 6)
        m12_logf("stub export called: %s (ansering failure)", name);
}

}  // namespace

extern "C" {

HRESULT WINAPI CreateDXGIFactory(const IID &riid, void **ppv)
{
    typedef HRESULT(WINAPI * FnT)(const IID &, void **);
    static FnT fn = (FnT)real_proc("CreateDXGIFactory");
    if (!fn) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = fn(riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) m12_hook_factory(*ppv);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(const IID &riid, void **ppv)
{
    typedef HRESULT(WINAPI * FnT)(const IID &, void **);
    static FnT fn = (FnT)real_proc("CreateDXGIFactory1");
    if (!fn) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = fn(riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) m12_hook_factory(*ppv);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, const IID &riid, void **ppv)
{
    typedef HRESULT(WINAPI * FnT)(UINT, const IID &, void **);
    static FnT fn = (FnT)real_proc("CreateDXGIFactory2");
    if (!fn) return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = fn(flags, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) m12_hook_factory(*ppv);
    return hr;
}

// ---- internal DXGI helpers: present so GetProcAddress finds them --------

HRESULT WINAPI ApplyCompatResolutionQuirking(void *, const WCHAR *, void *, void *)
{
    stub_called("ApplyCompatResolutionQuirking");
    return E_NOTIMPL;
}

int WINAPI CompatString(UINT, UINT, const void *)
{
    stub_called("CompatString");
    return 0;
}

int WINAPI CompatValue(UINT, UINT *)
{
    stub_called("CompatValue");
    return 0;
}

HRESULT WINAPI DXGID3D10CreateDevice(HMODULE, void *, void *, UINT, void *, void **)
{
    stub_called("DXGID3D10CreateDevice");
    return E_NOTIMPL;
}

HRESULT WINAPI DXGID3D10CreateLayeredDevice(void *, void *, void *, void *, void **)
{
    stub_called("DXGID3D10CreateLayeredDevice");
    return E_NOTIMPL;
}

SIZE_T WINAPI DXGID3D10GetLayeredDeviceSize(const void *, UINT)
{
    stub_called("DXGID3D10GetLayeredDeviceSize");
    return 0;
}

HRESULT WINAPI DXGID3D10RegisterLayers(const void *, UINT)
{
    stub_called("DXGID3D10RegisterLayers");
    return E_NOTIMPL;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    stub_called("DXGIDeclareAdapterRemovalSupport");
    return E_NOTIMPL;
}

void WINAPI DXGIDisableVBlankVirtualization()
{
    stub_called("DXGIDisableVBlankVirtualization");
}

HRESULT WINAPI DXGIDumpJournal(void *)
{
    stub_called("DXGIDumpJournal");
    return E_NOTIMPL;
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT, const IID &, void **)
{
    stub_called("DXGIGetDebugInterface1");
    return E_NOINTERFACE;
}

HRESULT WINAPI DXGIReportAdapterConfiguration(void *)
{
    stub_called("DXGIReportAdapterConfiguration");
    return E_NOTIMPL;
}

HRESULT WINAPI PIXBeginCapture(DWORD, int, void *)
{
    stub_called("PIXBeginCapture");
    return E_NOTIMPL;
}

HRESULT WINAPI PIXEndCapture(DWORD)
{
    stub_called("PIXEndCapture");
    return E_NOTIMPL;
}

int WINAPI PIXGetCaptureState(DWORD)
{
    stub_called("PIXGetCaptureState");
    return 0;
}

HRESULT WINAPI SetAppCompatStringPointer(SIZE_T, const WCHAR *)
{
    stub_called("SetAppCompatStringPointer");
    return E_NOTIMPL;
}

HRESULT WINAPI UpdateHMDEmulationStatus(const WCHAR *, UINT)
{
    stub_called("UpdateHMDEmulationStatus");
    return E_NOTIMPL;
}

}  // extern "C"
