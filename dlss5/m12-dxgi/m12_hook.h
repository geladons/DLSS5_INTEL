// SPDX-License-Identifier: Apache-2.0
// m12_hook.h - COM vtable hooking for the m12 dxgi proxy.
//
// Hooks the CreateSwapChain* factory entries (10/15/16/24) and the swapchain
// Present(8)/Present1(22) entries via a PER-OBJECT vtable swap: each hooked
// COM object gets a private copy of its vtable with our entries, and only
// that object's vtable pointer is redirected. The dxgi.dll module image
// (shared .rdata vtable pages) is never written - in-place vtable patching
// is what trips RDR2's integrity check (int3 crash ~60 s in, 2026-09-30
// field test). Call-through uses per-object records of the original entries.
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
