// 32-bit Vulkan instance probe: loads vulkan-1.dll directly and creates a
// bare instance, so we can see (via VK_LOADER_DEBUG=all) whether implicit
// layers activate in a non-DXVK 32-bit process. Links nothing: everything
// via GetProcAddress. ASCII only.
#include <windows.h>
#include <stdio.h>

typedef int (WINAPI *CreateInstanceFn)(const void*, const void*, void*);

int main(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("[FAIL] no vulkan-1.dll\n"); return 1; }
    CreateInstanceFn ci = (CreateInstanceFn)GetProcAddress(vk, "vkCreateInstance");
    if (!ci) { printf("[FAIL] no vkCreateInstance\n"); return 1; }
    struct App { unsigned type; const void* next; const char* name; unsigned appver;
                 const char* eng; unsigned engver; unsigned api; } app;
    struct InstInfo { unsigned type; const void* next; unsigned flags; const void* app;
                      unsigned lc; const void* l; unsigned ec; const void* e; } ii;
    ZeroMemory(&app, sizeof app); ZeroMemory(&ii, sizeof ii);
    app.type = 0; app.name = "vk32probe"; app.api = 0;
    ii.type = 1; ii.app = &app;
    void* inst = 0;
    int r = ci(&ii, 0, &inst);
    printf("[ok] vkCreateInstance -> %d, instance=%p\n", r, inst);
    fflush(stdout);
    return r == 0 ? 0 : 2;
}
