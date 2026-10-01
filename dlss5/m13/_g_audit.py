# _g_audit.py - Task G production bundle audit. Mirrors _build_production.py
# ARTIFACTS/PKG_FILES; reports sha1 mismatches and unexpected extra files.
# UNTRACKED helper - do not stage.
import hashlib
import os
import sys

_DLSS5 = r"C:\Users\<owner>\Desktop\DLSS5_INTEL\dlss5"
_PROD = r"C:\Users\<owner>\Desktop\production"

PKG_FILES = ["__init__.py", "config.py", "daemonctl.py", "deploy.py",
             "gamelaunch.py", "gamescan.py", "gamescache.py", "i18n.py",
             "icons.py", "launcher.py", "logtail.py", "overlay.py",
             "overlay_win32.py", "paths.py", "phase30.py", "processes.py",
             "screenmode.py", "splash.py", "controller.py", "ui.py",
             "ui_games.py", "worker.py", "m13.pyw", "icon.ico"]


def sha1(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


_M11D_DIR = os.path.join(_DLSS5, "m11d", "build", "Release")
if not os.path.exists(os.path.join(_M11D_DIR, "m11d.exe")):
    _M11D_DIR = os.path.join(_DLSS5, "m11d", "build-nmake")
_CHAIN_DLL = os.path.join(_DLSS5, "m11d", "build", "chain", "Release",
                          "d5c_chain.dll")
if not os.path.exists(_CHAIN_DLL):
    _CHAIN_DLL = os.path.join(_DLSS5, "m11d", "build-nmake", "chain",
                              "d5c_chain.dll")
if not os.path.exists(_CHAIN_DLL):
    _CHAIN_DLL = os.path.join(_M11D_DIR, "d5c_chain.dll")

ARTIFACTS = [
    (_M11D_DIR, ("runtime", "m11d"), ["m11d.exe", "*.spv"]),
    (os.path.join(_DLSS5, "m8b-live", "build", "Release"),
     ("runtime", "m8b"), ["m8blive.exe", "*.spv"]),
    (os.path.join(_DLSS5, "m11-layer", "build", "Release"),
     ("runtime", "layer", "x64"),
     ["nr_layer_win.dll", "VkLayer_dlssnr_win.json", "*.spv"]),
    (os.path.join(_DLSS5, "m11-layer", "x86"), ("runtime", "layer", "x86"),
     ["nr_layer_win32.dll", "VkLayer_dlssnr_win32.json"]),
    (os.path.join(_DLSS5, "m11-layer", "dxvk", "x32"), ("runtime", "dxvk", "x32"),
     ["d3d9.dll", "d3d11.dll", "d3d10core.dll"]),
    (os.path.join(_DLSS5, "m11-layer", "dxvk", "x64"), ("runtime", "dxvk", "x64"),
     ["d3d9.dll", "d3d11.dll", "d3d10core.dll", "dxgi.dll"]),
    (os.path.join(_DLSS5, "m12-dxgi", "build", "Release"), ("runtime", "m12"),
     ["m12_dxgi.dll", "*.spv"]),
]

print("chain dll source: %s" % _CHAIN_DLL)
print("=" * 78)

mism, match, missing = [], [], []
expected = {}   # dest dir -> set(names)


def expand(src_dir, patterns):
    names = []
    for pat in patterns:
        if pat.startswith("*."):
            names += [n for n in os.listdir(src_dir) if n.endswith(pat[1:])]
        else:
            names.append(pat)
    return names


for src_dir, rel, pats in ARTIFACTS:
    dst_dir = os.path.join(_PROD, *rel)
    exp = expected.setdefault(dst_dir, set())
    for name in expand(src_dir, pats):
        exp.add(name)
        s = os.path.join(src_dir, name)
        d = os.path.join(dst_dir, name)
        if not os.path.exists(s):
            missing.append((name, rel, "no build output"))
            continue
        if not os.path.exists(d):
            missing.append((name, rel, "missing in prod"))
            continue
        hs, hd = sha1(s), sha1(d)
        row = ("%s/%s" % ("/".join(rel), name), hs[:12], hd[:12])
        (match if hs == hd else mism).append(row)

# explicit chain dll copies
for rel in (("runtime", "m11d", "d5c_chain.dll"),
            ("runtime", "layer", "x64", "d5c_chain.dll"),
            ("runtime", "m12", "d5c_chain.dll")):
    d = os.path.join(_PROD, *rel)
    expected.setdefault(os.path.dirname(d), set()).add(rel[-1])
    hs = sha1(_CHAIN_DLL)
    hd = sha1(d) if os.path.exists(d) else "MISSING"
    row = ("/".join(rel), hs[:12], hd[:12] if len(hd) > 12 else hd)
    (match if hs == hd else mism).append(row)

# m13 package files
m13_dst = os.path.join(_PROD, "m13")
m13_exp = expected.setdefault(m13_dst, set())
for name in PKG_FILES:
    m13_exp.add(name)
    s = os.path.join(_DLSS5, "m13", name)
    d = os.path.join(m13_dst, name)
    if not os.path.exists(s):
        missing.append((name, "m13", "no source"))
        continue
    if not os.path.exists(d):
        missing.append((name, "m13", "missing in prod"))
        continue
    hs, hd = sha1(s), sha1(d)
    row = ("m13/%s" % name, hs[:12], hd[:12])
    (match if hs == hd else mism).append(row)

print("%-52s %-13s %-13s %s" % ("artifact", "build", "prod", "state"))
print("-" * 90)
for name, hs, hd in sorted(match):
    print("%-52s %-13s %-13s ok" % (name, hs, hd))
print("-" * 90)
for name, hs, hd in sorted(mism):
    print("%-52s %-13s %-13s MISMATCH" % (name, hs, hd))
for name, rel, why in missing:
    print("%-52s %-13s %-13s %s" % ("%s/%s" % ("/".join(rel) if isinstance(rel, tuple) else rel, name), "-", "-", why))

print("\n=== unexpected files in prod dirs ===")
for dst_dir, exp in sorted(expected.items()):
    if not os.path.isdir(dst_dir):
        print("  %s: DIR MISSING" % dst_dir)
        continue
    extra = sorted(set(os.listdir(dst_dir)) - exp)
    if extra:
        for e in extra:
            print("  %s: %s" % (os.path.relpath(dst_dir, _PROD), e))
    else:
        print("  %s: clean" % os.path.relpath(dst_dir, _PROD))

# stale artifacts scan
print("\n=== stale-file scan (hudmask/test_*/pycache/sister-dir) ===")
for root, dirs, files in os.walk(_PROD):
    rel = os.path.relpath(root, _PROD)
    for f in files:
        low = f.lower()
        if ("hudmask" in low or low.startswith("test_") or
                low.endswith((".pyc", ".obj", ".log"))):
            print("  STALE? %s" % os.path.join(rel, f))
    if "__pycache__" in dirs:
        print("  STALE? %s" % os.path.join(rel, "__pycache__"))

print("\n%d match, %d mismatch, %d missing" % (len(match), len(mism),
                                               len(missing)))
