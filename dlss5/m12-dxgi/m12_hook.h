// m12_hook.h - COM vtable hooking for the m12 dxgi proxy.
//
// The proxy hooks the three CreateSwapChain* entry points by giving each
// DXGI factory object a private copy of its vtable (entries 10/15/16/24).
// When a D3D12 swapchain shows up (pDevice is an ID3D12CommandQueue) the
// swapchain object gets its own patched vtable (Present/Present1) that runs
// the capture/blit pipeline before calling through to the original entries.
#pragma once

// Initializes the hook layer. Called from DllMain process attach and from
// the CreateDXGIFactory* forwarders (whichever comes first).
void m12_hook_init();

// Called by the m12_exports.cpp forwarders after a real CreateDXGIFactory*
// succeeded; patches the returned factory object's vtable copy.
void m12_hook_factory(void *factory);
