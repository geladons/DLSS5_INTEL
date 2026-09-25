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

from .processes import DETACHED

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
    """Detached game launch (cwd = game dir). Returns (ok, message)."""
    if not os.path.exists(exe_path):
        return False, "game exe not found: %s" % exe_path
    game_dir = os.path.dirname(exe_path)
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    log = os.path.join(game_dir, "_m13_game.log")
    lf = open(log, "a", buffering=1)
    p = subprocess.Popen([exe_path], cwd=game_dir, stdout=lf,
                         stderr=subprocess.STDOUT, close_fds=True,
                         creationflags=DETACHED, env=env)
    return True, "launched pid %d (log: %s)" % (p.pid, log)


# ------------------------------------------------------------ freeze flags --
def freeze_set(present):
    """Photo mode on/off (shared by the layer and the m12 proxy)."""
    _flag_set(FREEZE_FLAG, present)


def freeze_active():
    return _flag_exists(FREEZE_FLAG)


def reproc_bump():
    """Ask the layer/proxy to re-send the held raw frame (after a gain push).
    Writing fresh content updates the mtime the layer watches."""
    try:
        with open(REPROC_FLAG, "w") as f:
            f.write(str(os.getpid()) + " " + repr(__import__("time").time()))
    except OSError:
        pass


# --------------------------------------------------------------------- DX9 --
def launch_dx9(exe_path, live_every=1):
    """Launch a DX9 game under DXVK + the m11 layer (managed live mode)."""
    _flag_set(DX9_TRIGGER, True)   # processing ON from the first present
    freeze_set(False)              # never launch into a frozen frame
    return _launch(exe_path, {
        "NR_LAYER_LIVE": str(live_every),
        "NR_LAYER_TRIGGER": DX9_TRIGGER,
        "NR_LAYER_FREEZE": FREEZE_FLAG,
        "NR_LAYER_REPROC": REPROC_FLAG,
    })


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
def launch_dx12(exe_path):
    """Launch a DX12 game with the m12-dxgi proxy active (M12_LIVE default
    4 comes from the proxy itself; no env needed)."""
    _flag_set(M12_PAUSE, False)    # make sure we start un-paused
    freeze_set(False)
    return _launch(exe_path)


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
def launch_game(exe_path, mode):
    if mode in ("dx9", "dx11", "vulkan"):
        return launch_dx9(exe_path)      # same layer env + trigger flag
    if mode == "dx12":
        return launch_dx12(exe_path)
    return False, "unknown mode: %s" % mode


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
