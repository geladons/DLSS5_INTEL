# ============================================================================
# m13.gamelaunch - detached game launching with the injection env, and
# FILE-BASED pause/resume channels.
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
# ============================================================================
import os
import subprocess

from .processes import DETACHED

DX9_TRIGGER = os.path.join(os.environ.get("TEMP", "."), "m13_dx9_trigger.flag")
M12_PAUSE = os.path.join(os.environ.get("TEMP", "."), "m12_pause.flag")


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


# --------------------------------------------------------------------- DX9 --
def launch_dx9(exe_path, live_every=1):
    """Launch a DX9 game under DXVK + the m11 layer (managed live mode)."""
    _flag_set(DX9_TRIGGER, True)   # processing ON from the first present
    return _launch(exe_path, {
        "NR_LAYER_LIVE": str(live_every),
        "NR_LAYER_TRIGGER": DX9_TRIGGER,
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
    return _launch(exe_path)


def dx12_paused():
    return _flag_exists(M12_PAUSE)


def dx12_pause():
    _flag_set(M12_PAUSE, True)
    return True, "DX12 paused (proxy passthrough, full fps)"


def dx12_resume():
    _flag_set(M12_PAUSE, False)
    return True, "DX12 resumed (processing on)"
