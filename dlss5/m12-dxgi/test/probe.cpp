// probe.cpp - minimal loader diagnostic for the m12 dxgi proxy.
// Loads the proxy from its staged directory and reports the exact error.
#include <windows.h>

#include <cstdio>

int main()
{
    const wchar_t *path = L"C:\\Users\\AI\\Desktop\\DLSS5_INTEL\\dlss5\\m12-dxgi\\test\\run\\dxgi.dll";
    HMODULE h = LoadLibraryW(path);
    DWORD err = GetLastError();
    printf("LoadLibrary -> %p, last error = %lu\n", (void *)h, err);
    if (h) {
        void *p = (void *)GetProcAddress(h, "CreateDXGIFactory1");
        printf("GetProcAddress(CreateDXGIFactory1) -> %p\n", p);
        FreeLibrary(h);
    }
    return 0;
}
