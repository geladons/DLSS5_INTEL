# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.gamelaunch - detached game launching with the injection env, and
# FILE-BASED pause/resume/freeze channels.
#
# The manager NEVER injects input into games (keybd_event/PostMessage failed
# in the GTA IV menu - docs/HANDOFF_GTA4_DX9.md). All runtime control is
# files/processes/hotkeys-owned-by-the-layer:
#  - DX9 (m11 layer in live+trigger mode): processing runs while the trigger
#    flag EXISTS (nr_layer_win.c: on = !trigger_path || access==0). Pause =
#    delete the flag (native fps passthrough), resume = recreate it.
#  - DX12 (m12-dxgi proxy): %TEMP%\m12_pause.flag - while it EXISTS every
#    frame passes through untouched (full fps). Pause = create, resume =
#    delete. The proxy owns CTRL+ALT+X/Q itself.
#  - FREEZE (photo mode, both paths): %TEMP%\m13_freeze.flag - while it
#    EXISTS the layer/proxy holds the raw frame captured at freeze time and
#    re-blits its processed result: the picture stands still while the game
#    keeps running. The layer learns the flag paths from NR_LAYER_FREEZE /
#    NR_LAYER_REPROC env (manager-launched games); the m12 proxy reads the
#    fixed %TEMP% names directly (works for externally started games too).
#  - REPROCESS: bumping %TEMP%\m13_reproc.flag's mtime makes the layer/proxy
#    re-send the SAME raw frame to the daemon - after an NRCT gain push the
#    frozen picture updates from the untouched original (live knobs).
# ============================================================================
import os
import subprocess

from . import deploy
from . import paths
from .phase30 import phase30_env_dict, write_phase30_knobs

DX9_TRIGGER = os.path.join(os.environ.get("TEMP", "."), "m13_dx9_trigger.flag")
M12_PAUSE = os.path.join(os.environ.get("TEMP", "."), "m12_pause.flag")
FREEZE_FLAG = os.path.join(os.environ.get("TEMP", "."), "m13_freeze.flag")
REPROC_FLAG = os.path.join(os.environ.get("TEMP", "."), "m13_reproc.flag")

# Best-effort windowed launch: appended when a game entry sets "window"
# and no explicit launch_args exist. Common flags; a game that ignores
# them just launches as usual (the UI labels this "try").
WINDOW_ARGS = ("-windowed", "-window")


def _cfg_value(cfg, key, default):
    """cfg may be a Config, a plain dict (tests), or None."""
    if cfg is not None and hasattr(cfg, "get"):
        v = cfg.get(key)
        if v is not None:
            return v
    return default


def _flag_exists(path):
    return os.path.exists(path)


def _flag_set(path, present):
    if present:
        with open(path, "w") as f:
            f.write("m13")
    else:
        try:
            os.remove(path)
        except OSError:
            pass


# -------------------------------------------- engine env (weights/shaders) --
# The in-process d5c_chain.dll (loaded by the m12 dxgi proxy for dx12 and by
# nr_layer_win.dll for dx11/vulkan) resolves weights + shaders from the ENV
# FIRST - chain_c_api.cpp consults DLSS5_WEIGHTS_PATH / NR_LAYER_WEIGHTS and
# DLSS5_SHADER_DIR / NR_LAYER_SHADERS - and only then probes dirs relative to
# its own module dir. Those probes fail when the consumer dll runs injected
# inside a game folder, so the env keys are mandatory for every managed
# launch. On the x86 daemon path (dx9) they are simply unused: daemon_start
# passes --weights to m11d.exe itself.
def weights_path_for(cfg=None):
    """Absolute path of the .safetensors the chain must load: the
    configured "weights_path" when set, else the first *.safetensors in
    a known weights dir (same rule as ctl._autodetect_weights), else
    the canonical filename in the bundle dir (a missing file surfaces
    as the chain's own 'weights not found' error, same as today).
    paths.find("weights") returns the bundle dir even in the dev tree
    (dir keys return the first candidate unconditionally), so the dev
    location work\\mlxw is probed as well."""
    if cfg is not None and hasattr(cfg, "get"):
        p = cfg.get("weights_path")
        if p:
            return p
    dirs = [paths.find("weights"),
            os.path.join(paths.REPO, "work", "mlxw")]
    seen = set()
    for wdir in dirs:
        if not wdir or wdir in seen:
            continue
        seen.add(wdir)
        try:
            names = [n for n in os.listdir(wdir)
                     if n.endswith(".safetensors")]
        except OSError:
            continue
        if not names:
            continue
        if "dlssnr-logical.safetensors" in names:
            return os.path.join(wdir, "dlssnr-logical.safetensors")
        return os.path.join(wdir, sorted(names)[0])
    return os.path.join(dirs[0], "dlssnr-logical.safetensors")


def shader_dir_for(mode=None, arch="x64"):
    """Directory holding the *.spv set this mode's in-process d5c_chain
    consumer loads:
      dx12            -> dirname of the deployed d5c_chain.dll source
                         (mode_dll_source -> runtime\\m12 / the dev m12
                         build dir - both ship the full spv set)
      dx9/dx11/vulkan -> dirname of the x64 layer manifest
                         (runtime\\layer\\x64 ships spv next to the
                         d5c_chain.dll nr_layer_win.dll loads; the x86
                         daemon path ignores the env entirely)
      mode=None       -> first valid dir across consumers (env seeding)
    Returns None when no candidate holds gemm.spv - never point the
    chain at a dir without shaders (its own modDir probe is a better
    fallback than a wrong explicit dir)."""
    m12_dir = os.path.dirname(
        deploy.mode_dll_source("dx12", arch, "d5c_chain.dll"))
    layer_dir = os.path.dirname(deploy.MANIFEST_X64)
    m11d_dir = os.path.dirname(paths.find("m11d_exe"))
    if mode == "dx12":
        cands = (m12_dir, layer_dir, m11d_dir)
    elif mode in ("dx9", "dx11", "vulkan"):
        cands = (layer_dir, m12_dir, m11d_dir)
    else:
        cands = (m12_dir, layer_dir, m11d_dir)
    seen = set()
    for d in cands:
        if not d or d in seen:
            continue
        seen.add(d)
        if os.path.exists(os.path.join(d, "gemm.spv")):
            return d
    return None


def write_runtime_paths(cfg=None):
    """Drop %LOCALAPPDATA%\\DLSS5Manager\\runtime_paths.txt - the shared
    fallback the in-process chain reads when a game was spawned outside
    our env (Steam client, Rockstar/CDPR launchers, a manual
    double-click). chain_c_api consults it AFTER env vars and
    module-relative candidates, so the file can never outrank an
    explicit env."""
    from . import config as _config
    w = weights_path_for(cfg)
    s = shader_dir_for()
    if not w or not s:
        return False
    out = os.path.join(_config.CONFIG_DIR, "runtime_paths.txt")
    try:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        tmp = out + ".tmp"
        with open(tmp, "w", encoding="ascii") as f:
            f.write("WEIGHTS=%s\nSHADER_DIR=%s\n" % (w, s))
        os.replace(tmp, out)
        return True
    except OSError:
        return False


def engine_env(cfg=None, mode=None, arch="x64"):
    """The env pairs above as a dict ready for env.update / os.environ.
    mode=None picks the first valid shader dir (process-env seeding)."""
    out = {}
    w = weights_path_for(cfg)
    if w:
        out["DLSS5_WEIGHTS_PATH"] = w
        out["NR_LAYER_WEIGHTS"] = w
    s = shader_dir_for(mode, arch)
    if s:
        out["DLSS5_SHADER_DIR"] = s
        out["NR_LAYER_SHADERS"] = s
    return out


def _launch(exe_path, env_extra=None, args=None):
    """Game launch (cwd = game dir). Returns (ok, message).
    args: extra command-line tokens appended after the exe (e.g. the
    best-effort windowed flags in WINDOW_ARGS or a per-game launch_args
    string the owner set)."""
    if not os.path.exists(exe_path):
        return False, "game exe not found: %s" % exe_path
    game_dir = os.path.dirname(exe_path)
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    p = subprocess.Popen([exe_path] + list(args or []), cwd=game_dir,
                         stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL,
                         close_fds=True,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                         env=env)
    return True, "launched pid %d" % p.pid


# ------------------------------------------------------------ freeze flags --
def freeze_set(present):
    """Photo mode on/off (shared by the layer and the m12 proxy)."""
    _flag_set(FREEZE_FLAG, present)


def freeze_active():
    return _flag_exists(FREEZE_FLAG)


def reset_runtime_flags():
    """Restore unpaused defaults after the last managed game exits."""
    for path, present in ((DX9_TRIGGER, True), (M12_PAUSE, False),
                          (FREEZE_FLAG, False), (REPROC_FLAG, False)):
        try:
            _flag_set(path, present)
        except OSError:
            pass


def reproc_bump():
    """Ask the layer/proxy to re-send the held raw frame (after a gain push).
    Writing fresh content updates the mtime the layer watches."""
    try:
        with open(REPROC_FLAG, "w") as f:
            f.write(str(os.getpid()) + " " + repr(__import__("time").time()))
    except OSError:
        pass


# --------------------------------------------------------------------- DX9 --
def launch_dx9(exe_path, live_every=1, scale=None, gain=None, blend=None, cfg=None, sharpness=None):
    """Launch a DX9 game under DXVK + the m11 layer (managed live mode)."""
    return launch_game(exe_path, "dx9", scale=scale, gain=gain, blend=blend, cfg=cfg, sharpness=sharpness)


def dx9_paused():
    """True = passthrough (trigger flag absent)."""
    return not _flag_exists(DX9_TRIGGER)


def dx9_pause():
    _flag_set(DX9_TRIGGER, False)
    return True, "DX9 paused (native frames, full fps)"


def dx9_resume():
    _flag_set(DX9_TRIGGER, True)
    return True, "DX9 resumed (processing on)"


# -------------------------------------------------------------------- DX12 --
def launch_dx12(exe_path, scale=None, gain=None, blend=None, cfg=None, sharpness=None):
    """Launch a DX12 game with the m12-dxgi proxy active."""
    return launch_game(exe_path, "dx12", scale=scale, gain=gain, blend=blend, cfg=cfg, sharpness=sharpness)


def dx12_paused():
    return _flag_exists(M12_PAUSE)


def dx12_pause():
    _flag_set(M12_PAUSE, True)
    return True, "DX12 paused (proxy passthrough, full fps)"


def dx12_resume():
    _flag_set(M12_PAUSE, False)
    return True, "DX12 resumed (processing on)"


# -------------------------------------------------------- generic by mode --
# dx9/dx11/vulkan all ride the m11 Vulkan layer -> they share the trigger
# flag (for Vulkan games the layer self-loads, no dll deploy). dx12 rides
# the m12 proxy -> its own pause flag.
def launch_game(exe_path, mode, scale=None, gain=None, blend=None, cfg=None, sharpness=None, launch_args=None):
    if mode not in ("dx9", "dx11", "vulkan", "dx12"):
        return False, "unknown mode: %s" % mode

    env = {}
    if mode in ("dx9", "dx11", "vulkan"):
        _flag_set(DX9_TRIGGER, True)   # processing ON from the first present
        env.update({
            "ENABLE_NR_LAYER": "1",
            "NR_LAYER_LIVE": "1",
            "NR_LAYER_TRIGGER": DX9_TRIGGER,
            "NR_LAYER_FREEZE": FREEZE_FLAG,
            "NR_LAYER_REPROC": REPROC_FLAG,
            "NR_LAYER_UI_MASK": "1",
        })
    elif mode == "dx12":
        _flag_set(M12_PAUSE, False)    # make sure we start un-paused
        # DX12 uses m12_dxgi.dll proxy + in-process d5c_chain.dll.
        # Disable the Vulkan presentation layer to avoid layer collision with d5c_chain's Vulkan context.
        env.update({
            "ENABLE_NR_LAYER": "0",
        })

    freeze_set(False)                  # never launch into a frozen frame
    sharp_val = 0.8
    if cfg is not None and hasattr(cfg, "get"):
        sharp_val = cfg.get("sharpness") or 0.8
    elif isinstance(cfg, dict):
        sharp_val = cfg.get("sharpness") or 0.8
    elif sharpness is not None:
        sharp_val = sharpness
    env["DLSS5_SHARPNESS"] = str(sharp_val)
    env["NR_LAYER_SHARPNESS"] = str(sharp_val)

    fg_val = False
    meta = {}
    if cfg is not None and hasattr(cfg, "get"):
        meta = (cfg.get("games") or {}).get(exe_path) or {}
    elif isinstance(cfg, dict):
        meta = (cfg.get("games") or {}).get(exe_path) or {}
    # Per-game "framegen"/"fg" flag overrides the global switch. Measured on
    # the x86 daemon path (GTA IV, DDA-verified): residual-transfer FG only
    # transfers detail for motion <= ~4 px/frame; real gameplay motion emits
    # near-raw generated frames, and the P/G alternation pulses at ~13 Hz -
    # the reported judder + mouse-trail ghosts. Disabled per-game there.
    if isinstance(meta, dict) and ("framegen" in meta or "fg" in meta):
        fg_val = bool(meta.get("framegen", meta.get("fg")))
    elif cfg is not None and hasattr(cfg, "get"):
        fg_val = bool(cfg.get("frame_gen"))
    elif isinstance(cfg, dict):
        fg_val = bool(cfg.get("frame_gen"))
    else:
        fg_env = os.environ.get("DLSS5_FRAMEGEN")
        if fg_env is not None:
            fg_val = (fg_env != "0")
    env["DLSS5_FRAMEGEN"] = "1" if fg_val else "0"
    env["NR_LAYER_FRAMEGEN"] = "1" if fg_val else "0"
    if mode == "dx12":
        # m12 present cadence: FG on -> process every 2nd present and
        # residual-transfer the one between (1:2, x64-layer parity); FG
        # off -> every present is honestly processed (the old default of
        # live-every-4 repeated/interpolated 75% of frames, which read as
        # permanent smear on Metro).
        env["M12_LIVE"] = "2" if fg_val else "1"

    # FG runs in-process inside the x64 engine; on the x86 TCP/daemon path
    # the daemon's resident engine answers NRN2 transfer requests instead
    # (degrades to a full process until a prev pair exists).
    if fg_val and cfg is not None and hasattr(cfg, "get"):
        meta = (cfg.get("games") or {}).get(exe_path) or {}
        if isinstance(meta, dict) and meta.get("arch") == "x86":
            print("[launch] framegen on x86: daemon residual-transfer path",
                  flush=True)

    if scale is not None:
        s_val = str(scale)
        env["NR_LAYER_SCALE"] = s_val
        env["DLSS5_SCALE"] = s_val
        env["M12_SCALE"] = s_val
    if gain is not None:
        g_val = str(gain)
        env["NR_LAYER_GAIN"] = g_val
        env["DLSS5_GAIN"] = g_val
        env["M12_GAIN"] = g_val
        env["NR_LAYER_INTENSITY"] = g_val
        env["DLSS5_INTENSITY"] = g_val
        env["M12_INTENSITY"] = g_val
    if blend is not None:
        b_val = str(blend)
        env["NR_LAYER_BLEND"] = b_val
        env["DLSS5_BLEND"] = b_val
        env["M12_BLEND"] = b_val

    p30 = None
    if cfg is not None and hasattr(cfg, "get_phase30"):
        p30 = cfg.get_phase30(exe_path)
    elif cfg is not None and hasattr(cfg, "get"):
        p30 = {
            "intensity": cfg.get("intensity", cfg.get("gain", 1.0)),
            "local_tone": cfg.get("local_tone", 1.0),
            "local_structure": cfg.get("local_structure", 1.0),
            "skin_structure": cfg.get("skin_structure", -1.0),
            "auto_mask": bool(cfg.get("auto_mask", False)),
            "style_index": cfg.get("style_index", 0),
            "color_strength": cfg.get("color_strength", 1.0),
        }
    if p30:
        env.update(phase30_env_dict(p30))
        # The knob file is the live channel for an in-process game (its env
        # never changes after spawn): seed it with the same values this
        # launch puts into the env so the two agree from the first present.
        blob = dict(p30)
        blob["gain"] = float(gain if gain is not None
                             else _cfg_value(cfg, "gain", 1.0))
        blob["blend"] = float(blend if blend is not None
                              else _cfg_value(cfg, "blend", 1.0))
        blob["scale"] = float(scale if scale is not None
                              else _cfg_value(cfg, "scale", 1.0))
        blob["sharpness"] = float(sharp_val)
        blob["frame_gen"] = 1 if fg_val else 0
        write_phase30_knobs(blob)

    # Weights + shader locations for the in-process d5c_chain (see the
    # engine_env block above) - every injection mode, unconditionally.
    arch = "x64"
    if cfg is not None and hasattr(cfg, "get"):
        gmeta = (cfg.get("games") or {}).get(exe_path) or {}
        if isinstance(gmeta, dict) and gmeta.get("arch"):
            arch = gmeta["arch"]
    ee = engine_env(cfg, mode, arch)
    env.update(ee)
    print("[launch] game=%s env_weights=%s env_shaders=%s"
          % (os.path.basename(exe_path),
             ee.get("DLSS5_WEIGHTS_PATH") or "(unset)",
             ee.get("DLSS5_SHADER_DIR") or "(unset)"), flush=True)

    return _launch(exe_path, env, args=launch_args)


def mode_paused(mode):
    if mode == "dx12":
        return dx12_paused()
    return dx9_paused()                  # layer modes share the trigger


def mode_pause(mode):
    if mode == "dx12":
        return dx12_pause()
    ok, _ = dx9_pause()
    return ok, "%s paused (passthrough, full fps)" % mode.upper()


def mode_resume(mode):
    if mode == "dx12":
        return dx12_resume()
    ok, _ = dx9_resume()
    return ok, "%s resumed (processing on)" % mode.upper()
