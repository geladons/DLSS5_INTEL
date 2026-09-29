# ============================================================================
# m13.hudmask - per-game HUD mask regions for the Vulkan present layer.
#
# HUD layouts, fonts and text positions differ per game and per screen mode,
# so the layer carries NO compiled-in regions. The manager owns the mask:
# before launching a layer-mode game it writes the game's region list to
# %TEMP%\m13_hudmask.cfg; the layer reads it at instance creation and
# hot-reloads it on change (nr_layer_win.c: load_hud_mask_config /
# hud_cfg_tick). No file -> no positional masking at all.
#
# Region source order:
#   1. per-game override: config.json games[exe]["hudmask"] (user-tunable)
#   2. built-in preset for the exe basename (a STARTING point, not gospel)
#   3. nothing (mask off)
#
# Region dict: {"shape": "rect"|"ellipse", "x":.., "y":.., "w":.., "h":..}
# coords normalized 0..1. rect: x,y = top-left, w,h = size.
# ellipse: x,y = center, w,h = radii.
# ============================================================================
import os

HUDMASK_FLAG = os.path.join(os.environ.get("TEMP", "."), "m13_hudmask.cfg")

PRESETS = {
    "gtaiv.exe": [
        {"shape": "ellipse", "x": 0.08, "y": 0.90, "w": 0.065, "h": 0.10},
        {"shape": "rect", "x": 0.82, "y": 0.00, "w": 0.18, "h": 0.26},
        {"shape": "rect", "x": 0.18, "y": 0.89, "w": 0.64, "h": 0.11},
        {"shape": "rect", "x": 0.00, "y": 0.00, "w": 0.32, "h": 0.15},
    ],
}


def _clamp01(v):
    try:
        v = float(v)
    except (TypeError, ValueError):
        return 0.0
    if v < 0.0:
        return 0.0
    if v > 1.0:
        return 1.0
    return v


def validate(regions):
    out = []
    if not isinstance(regions, (list, tuple)):
        return out
    for r in regions:
        if not isinstance(r, dict):
            continue
        shape = r.get("shape")
        if shape not in ("rect", "ellipse"):
            continue
        out.append({"shape": shape,
                    "x": _clamp01(r.get("x")), "y": _clamp01(r.get("y")),
                    "w": _clamp01(r.get("w")), "h": _clamp01(r.get("h"))})
    return out


def resolve(exe_path, cfg=None):
    games = {}
    if cfg is not None and hasattr(cfg, "get"):
        games = cfg.get("games") or {}
    elif isinstance(cfg, dict):
        games = cfg.get("games") or {}
    gmeta = games.get(exe_path) or {}
    if isinstance(gmeta, dict) and "hudmask" in gmeta:
        return validate(gmeta.get("hudmask"))
    key = os.path.basename(exe_path or "").lower()
    return validate(PRESETS.get(key, []))


def render(regions):
    lines = []
    for r in validate(regions):
        lines.append("%s %.4f %.4f %.4f %.4f" % (
            r["shape"], r["x"], r["y"], r["w"], r["h"]))
    return "\n".join(lines) + ("\n" if lines else "")


def write_regions(regions, path=None):
    path = path or HUDMASK_FLAG
    regions = validate(regions)
    if not regions:
        try:
            os.remove(path)
        except OSError:
            pass
        return path
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii") as f:
        f.write(render(regions))
    os.replace(tmp, path)
    return path


def apply(exe_path, cfg=None, path=None):
    return write_regions(resolve(exe_path, cfg), path=path)