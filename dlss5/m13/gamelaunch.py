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

from .phase30 import phase30_env_dict, write_phase30_knobs

DX9_TRIGGER = os.path.join(os.environ.get("TEMP", "."), "m13_dx9_trigger.flag")
M12_PAUSE = os.path.join(os.environ.get("TEMP", "."), "m12_pause.flag")
FREEZE_FLAG = os.path.join(os.environ.get("TEMP", "."), "m13_freeze.flag")
REPROC_FLAG = os.path.join(os.environ.get("TEMP", "."), "m13_reproc.flag")


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


def _launch(exe_path, env_extra=None):
    """Game launch (cwd = game dir). Returns (ok, message)."""
    if not os.path.exists(exe_path):
        return False, "game exe not found: %s" % exe_path
    game_dir = os.path.dirname(exe_path)
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    p = subprocess.Popen([exe_path], cwd=game_dir,
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
def launch_game(exe_path, mode, scale=None, gain=None, blend=None, cfg=None, sharpness=None):
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

    fg_val = True
    if cfg is not None and hasattr(cfg, "get"):
        fg_val = bool(cfg.get("frame_gen"))
    elif isinstance(cfg, dict):
        fg_val = bool(cfg.get("frame_gen"))
    else:
        fg_env = os.environ.get("DLSS5_FRAMEGEN")
        if fg_env is not None:
            fg_val = (fg_env != "0")
        else:
            fg_val = True
    env["DLSS5_FRAMEGEN"] = "1" if fg_val else "0"
    env["NR_LAYER_FRAMEGEN"] = "1" if fg_val else "0"

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
            "intensity": cfg.get("intensity") or cfg.get("gain") or 1.6,
            "local_tone": cfg.get("local_tone") or 1.0,
            "local_structure": cfg.get("local_structure") or 1.5,
            "skin_structure": cfg.get("skin_structure") if cfg.get("skin_structure") is not None else 2.0,
            "auto_mask": True if cfg.get("auto_mask") is None else bool(cfg.get("auto_mask")),
            "style_index": cfg.get("style_index") or 0,
            "color_strength": cfg.get("color_strength") or 1.0,
        }
    if p30:
        env.update(phase30_env_dict(p30))
        write_phase30_knobs(p30)

    return _launch(exe_path, env)


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
