// m12_hook.h - COM vtable hooking for the m12 dxgi proxy.
//
// Hooks the CreateSwapChain* factory entries (10/15/16/24) and the swapchain
// Present(8)/Present1(22) entries by patching the ORIGINAL vtables IN PLACE;
// the object's vtable pointer is never touched (GTA5 Enhanced anti-tamper
// kills the process otherwise). Call-through uses per-vtable records of the
// original entries.
#pragma once

// Initializes the hook layer. Called from DllMain process attach and from
// the CreateDXGIFactory* forwarders (whichever comes first).
void m12_hook_init();

// Called by the m12_exports.cpp forwarders after a real CreateDXGIFactory*
// succeeded; patches the returned factory object's vtable in place and
// starts the owner hotkey thread (first call only).
void m12_hook_factory(void *factory);

// Hotkey full detach (CTRL+ALT+Q): restores every patched vtable in place,
// releases processors, makes the proxy inert for the process lifetime.
void m12_hook_restore_all();
