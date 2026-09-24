# ============================================================================
# m13.paths - runtime artifact locations. Two supported layouts:
#
#  PRODUCTION (self-contained bundle, what we ship to the owner):
#    <bundle>\M13.cmd
#    <bundle>\m13\                 this package (py files only)
#    <bundle>\runtime\m11d\        m11d.exe + *.spv
#    <bundle>\runtime\layer\x64\   nr_layer_win.dll + manifest
#    <bundle>\runtime\layer\x86\   nr_layer_win32.dll + manifest
#    <bundle>\runtime\dxvk\x32\    d3d9.dll (ONLY this for DX9 games)
#    <bundle>\runtime\m12\         m12_dxgi.dll
#    <bundle>\runtime\m8b\         m8blive.exe + *.spv
#    <bundle>\logs\                manager/daemon logs
#    <bundle>\weights\             the owner's proprietary .safetensors
#
#  DEV (in-tree): the same artifacts spread across dlss5\ as built by cmake.
#
# find() prefers the production layout, then falls back to dev - so this
# same code runs unchanged from the repo and from the bundle.
# ============================================================================
import os

_PKG = os.path.dirname(os.path.abspath(__file__))      # ...\m13
BUNDLE = os.path.dirname(_PKG)                          # bundle or dlss5

# The repo root: when the package lives at dlss5\m13 the repo root is one
# level up; inside a bundle there is no dlss5 child so BUNDLE is the root.
if os.path.basename(BUNDLE).lower() == "dlss5":
    REPO = os.path.dirname(BUNDLE)
else:
    REPO = BUNDLE

_PROD = {
    "m11d_exe":   ("runtime", "m11d", "m11d.exe"),
    "m8b_exe":    ("runtime", "m8b", "m8blive.exe"),
    "dxvk_d3d9":  ("runtime", "dxvk", "x32", "d3d9.dll"),
    "m12_proxy":  ("runtime", "m12", "m12_dxgi.dll"),
    "layer_x64":  ("runtime", "layer", "x64", "VkLayer_dlssnr_win.json"),
    "layer_x86":  ("runtime", "layer", "x86", "VkLayer_dlssnr_win32.json"),
    "logs":       ("logs",),
}
_DEV = {
    "m11d_exe":   ("dlss5", "m11d", "build-nmake", "m11d.exe"),
    "m8b_exe":    ("dlss5", "m8b-live", "build", "Release", "m8blive.exe"),
    "dxvk_d3d9":  ("dlss5", "m11-layer", "dxvk", "x32", "d3d9.dll"),
    "m12_proxy":  ("dlss5", "m12-dxgi", "build", "Release", "m12_dxgi.dll"),
    # the REGISTERED x64 manifest lives next to the built dll
    "layer_x64":  ("dlss5", "m11-layer", "build", "Release",
                   "VkLayer_dlssnr_win.json"),
    "layer_x86":  ("dlss5", "m11-layer", "x86", "VkLayer_dlssnr_win32.json"),
    "logs":       ("work", "_m11"),
}


def find(key):
    """Absolute path to a runtime artifact (production first, dev fallback).
    Returned even if missing - callers surface 'file not found' themselves."""
    for root, table in ((BUNDLE, _PROD), (REPO, _DEV)):
        cand = os.path.join(root, *table[key])
        if key == "logs":
            return cand            # dirs are created on demand
        if os.path.exists(cand):
            return cand
    return os.path.join(REPO, *_DEV[key])


def find_all(key):
    """All existing candidates (own layout first) - used to de-duplicate
    Vulkan layer registrations between dev and production manifests."""
    out = []
    for root, table in ((BUNDLE, _PROD), (REPO, _DEV)):
        cand = os.path.join(root, *table[key])
        if os.path.exists(cand) and cand not in out:
            out.append(cand)
    return out
