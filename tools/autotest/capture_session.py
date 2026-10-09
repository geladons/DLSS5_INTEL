#!/usr/bin/env python3
# ============================================================================
# capture_session.py - GTA IV automated capture/orchestration harness.
#
# One command = one reproducible test session:
#   1. write/restore commandline.txt (benchmark vs menu args)
#   2. arm the layer trigger/knob protocol, clear stale freeze flags
#   3. spawn m11d.exe daemon detached (skipped if --no-daemon or the port
#      already answers), poll 127.0.0.1:48990, send a warm-up frame
#   4. launch GTAIV.exe with the field-proven NR_LAYER_* env
#   5. wait for the game window, wait for a stable rect, park it at a fixed
#      geometry (keeper thread re-parks - the game recreates its window)
#   6. ddarec.exe records --dur seconds at --fps into a raw file
#      (two-pass on purpose: the stdout-pipe path splits writes above
#      ~60 fps and corrupts the rawvideo stream - gamerec.py comment)
#   7. ffmpeg.exe encodes a lossless-ish libx264 mp4; --keep-raw keeps the
#      (huge) raw capture too
#   8. manifest.json + capture.log in --out; all spawned processes killed,
#      commandline.txt restored byte-exact, one-line RESULT printed.
#
# INPUT: GTA IV ignores ALL injected keyboard/mouse input on this host
# (SendInput/PostMessage/mouse_event/UIA - verified 2026-10-03). The only
# synthetic input the game accepts is a ViGEmBus virtual X360 pad - see
# pad.py + --drive (field report 2026-10-05: menus AND gameplay driven
# end-to-end). There is still NO commandline arg to auto-load a savegame
# or skip the main menu - the complete commandline.txt vocabulary is
# graphics/memory/audio/input-method flags only (-benchmark is the single
# auto-run mode, it plays a fixed flythrough then self-exits).
#
# Timeline segmentation is NOT done here: we record continuously from
# window-stable and let the analyzer bucket phases (loading art ~static,
# menu animates, benchmark is a camera flight).
#
# Usage:
#   python capture_session.py --mode benchmark --dur 30 --fps 120 --out out\b1
#   python capture_session.py --mode menu --dur 60 --out out\m1
#   python capture_session.py --mode benchmark --native --out out\native1
#   python capture_session.py --mode benchmark --env NR_LAYER_RESIDUAL=1 \
#       --env NR_LAYER_RESIDUAL_FB=1 --fg 2 --out out\ab1
#   python capture_session.py --mode menu --dur 60 --pre-roll 150 \
#       --drive menu_play --out out\g1   (records REAL gameplay: the
#       virtual pad navigates menus during pre-roll, then wanders)
# ============================================================================
import argparse
import ctypes
import ctypes.wintypes
from ctypes import wintypes
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# ---- default paths (all overridable on the CLI) ---------------------------
GAME = r"D:\games\Grand Theft Auto IV\GTAIV.exe"
M11D = r"C:\Users\<owner>\Desktop\production\runtime\m11d\m11d.exe"
WEIGHTS_CANDIDATES = [
    r"C:\Users\<owner>\Desktop\production\weights\dlssnr-logical.safetensors",
    os.path.join(ROOT, "work", "mlxw", "dlssnr-logical.safetensors"),
]
DDAREC = os.path.join(ROOT, "dlss5", "m14-ddarec", "build", "Release", "ddarec.exe")
FFMPEG = r"C:\Users\<owner>\Desktop\production\tools\ffmpeg.exe"

TEMP = os.environ.get("TEMP", r"C:\Users\<owner>\AppData\Local\Temp")
KNOB_PATH = os.path.join(TEMP, "m13_phase30_knobs.json")
TRIG_FLAG = os.path.join(TEMP, "m13_dx9_trigger.flag")
STALE_FLAGS = ("m13_freeze.flag", "m13_reproc.flag", "m12_pause.flag")
LAYER_LOG = os.path.join(TEMP, "nr_layer_win.log")

PORT = 48990
MAGIC_FRAME = 0x304E524E         # 'NRN0' frame channel

# Menu: intro videos -> animated main menu, no input needed.
# Benchmark: -benchmark self-runs a flythrough then auto-exits the game.
MODE_ARGS = {
    "menu": "-windowed\r\n-width 800\r\n-height 600\r\n",
    # 720p variant: probes whether the menu-select raw-splice defect is
    # bound to the tiny 800x600 client or fires at any windowed extent.
    "menu720": "-windowed\r\n-width 1280\r\n-height 720\r\n",
    "benchmark": "-benchmark\r\n-windowed\r\n-width 800\r\n-height 600\r\n",
}

# Field-proven geometry (Temp/_field_fg2.py): 816x638 window = 800x600 client.
PARK_DEFAULT = (400, 200, 816, 638)

# Layer env: the _gta4_field2.cmd field set (framegen filled in by --fg).
def base_layer_env(port, fg):
    env = {
        "ENABLE_NR_LAYER": "1",
        "NR_LAYER_LIVE": "1",
        "NR_LAYER_DIAG": "1",
        "NR_LAYER_PORT": str(port),
        "NR_LAYER_ASYNC": "1",
        "NR_LAYER_TRIGGER": TRIG_FLAG,
        "NR_LAYER_FREEZE": os.path.join(TEMP, "m13_freeze.flag"),
        "NR_LAYER_REPROC": os.path.join(TEMP, "m13_reproc.flag"),
        "NR_LAYER_UI_MASK": "1",
        "DLSS5_SHARPNESS": "0.9", "NR_LAYER_SHARPNESS": "0.9",
        "DLSS5_TEMPORAL": "1", "NR_LAYER_TEMPORAL": "1",
        "NR_LAYER_SCALE": "0.58", "DLSS5_SCALE": "0.58",
        "DLSS5_BLEND": "1.0", "NR_LAYER_BLEND": "1.0",
        "DLSS5_GAIN": "1.300", "NR_LAYER_GAIN": "1.300",
        "DLSS5_INTENSITY": "1.300", "NR_LAYER_INTENSITY": "1.300",
        "DLSS5_LOCAL_TONE": "1.000", "NR_LAYER_LOCAL_TONE": "1.000",
        "DLSS5_LOCAL_STRUCTURE": "1.000", "NR_LAYER_LOCAL_STRUCTURE": "1.000",
        "DLSS5_SKIN_STRUCTURE": "-1.000", "NR_LAYER_SKIN_STRUCTURE": "-1.000",
        "DLSS5_AUTO_MASK": "0", "NR_LAYER_AUTO_MASK": "0",
        "DLSS5_STYLE_INDEX": "0", "NR_LAYER_STYLE_INDEX": "0",
        "DLSS5_COLOR_STRENGTH": "0.950", "NR_LAYER_COLOR_STRENGTH": "0.950",
        "DLSS5_FRAMEGEN": "1" if fg else "0",
        "NR_LAYER_FRAMEGEN": "1" if fg else "0",
        "DLSS5_FG_MODE": str(fg), "NR_LAYER_FG_MODE": str(fg),
    }
    return env

LAYER_PREFIXES = ("NR_LAYER", "ENABLE_NR", "DLSS5", "M12_", "D5C_")


class Logger(object):
    """Tee to console + <outdir>/capture.log."""
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8", errors="replace")

    def log(self, msg):
        line = "[%.3f] %s" % (time.time() - T0, msg)
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


T0 = time.time()
LOG = None

def log(msg):
    if LOG:
        LOG.log(msg)
    else:
        print(msg, flush=True)


# ---- win32 window layer lives in win32win.py -------------------------------
# (discovery by image/title, foreground activation, occluder minimize/restore)
from win32win import (  # noqa: E402
    u32, k32,
    game_window_rect, game_hwnd, game_hwnd_any, activate_foreground,
    root_window_at, occluder_ok, window_rect, minimize, restore,
    raise_normal_band, set_pos_top, exe_running,
)


# ---- daemon helpers (from Temp/_postfix_setup.py) -------------------------
def port_open(port, timeout=1.0):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=timeout).close()
        return True
    except OSError:
        return False


def wait_port(port, secs):
    deadline = time.time() + secs
    while time.time() < deadline:
        if port_open(port):
            return True
        time.sleep(0.5)
    return False


def daemon_warmup(port, w, h, timeout=120):
    """Push dummy NRN0 frames until the daemon returns a full reply - forces
    weights load + engine build so the game's first present lands on a warm
    daemon. Mirrors _postfix_setup.warmup()."""
    frame_len = w * h * 4
    payload = b"\x80" * frame_len
    hdr = struct.pack("<4I", MAGIC_FRAME, w, h, 44)
    deadline = time.time() + timeout
    n = 0
    while time.time() < deadline:
        n += 1
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=15.0) as s:
                s.settimeout(15.0)
                s.sendall(hdr)
                s.sendall(payload)
                got = 0
                while got < frame_len:
                    c = s.recv(min(65536, frame_len - got))
                    if not c:
                        break
                    got += len(c)
            if got == frame_len:
                log("[warm] full %dx%d reply after %d tries" % (w, h, n))
                return True
        except OSError:
            pass
        time.sleep(0.5)
    log("[warm] TIMEOUT after %d tries" % n)
    return False


# ---- layer activation protocol (from _gta4_field2.cmd / _field_fg2.py) ----
def arm_layer_protocol(fg):
    knob = {"intensity": 1.3, "local_tone": 1.0, "local_structure": 1.0,
            "skin_structure": -1.0, "auto_mask": False, "style_index": 0,
            "color_strength": 0.95, "gain": 1.3, "blend": 1.0,
            "scale": 0.58, "sharpness": 0.9, "frame_gen": int(bool(fg))}
    tmp = KNOB_PATH + ".tmp"
    with open(tmp, "w", encoding="ascii") as f:
        json.dump(knob, f, indent=2)
    os.replace(tmp, KNOB_PATH)
    for f in STALE_FLAGS:
        try:
            os.remove(os.path.join(TEMP, f))
        except OSError:
            pass
    open(TRIG_FLAG, "w").close()
    log("[layer] knobs json + trigger flag armed, stale freeze flags cleared")


def normalize_region(x, y, w, h):
    """Clamp to desktop + even dims (same rules gamerec/ddarec apply)."""
    desk_w = u32.GetSystemMetrics(0)
    desk_h = u32.GetSystemMetrics(1)
    if x < 0:
        w += x
        x = 0
    if y < 0:
        h += y
        y = 0
    if x + w > desk_w:
        w = desk_w - x
    if y + h > desk_h:
        h = desk_h - y
    return x, y, w & ~1, h & ~1


def tail_file(path, offset):
    """Bytes appended to path since offset; '' if the file shrank/vanished."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            end = f.tell()
            if end < offset:
                offset = 0
            f.seek(offset)
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def parse_args():
    ap = argparse.ArgumentParser(
        description="GTA IV automated capture session (daemon + layer + DDA "
                    "record + mp4 encode + manifest).")
    ap.add_argument("--mode", required=True, choices=sorted(MODE_ARGS),
                    help="menu = animated main menu; benchmark = auto flythrough")
    ap.add_argument("--dur", type=float, required=True,
                    help="record seconds")
    ap.add_argument("--pre-roll", type=float, default=0.0,
                    help="seconds to let the game run (window defended by "
                         "the keeper) AFTER the window stabilizes and BEFORE "
                         "recording starts - the way to capture the menu: "
                         "the game loads unrecorded, then we film it")
    ap.add_argument("--fps", type=int, default=120,
                    help="requested CFR rate (default 120; ddarec duplicates "
                         "on timeout so over-requesting is safe)")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--no-daemon", action="store_true",
                    help="do not spawn/wait/warm m11d (implies a native "
                         "session unless an external daemon is running)")
    ap.add_argument("--native", action="store_true",
                    help="strip every layer env var: native-reference capture "
                         "(implies --no-daemon for the spawn)")
    ap.add_argument("--fg", type=int, default=0, choices=(0, 1, 2),
                    help="framegen mode pushed to BOTH the daemon "
                         "(--framegen) and the layer env (default 0)")
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL",
                    help="extra env for the game process; repeatable. "
                         "KEY= (empty VAL) deletes a default")
    ap.add_argument("--park", default=None, metavar="x,y,w,h",
                    help="force a capture rect and defend it (default: use "
                         "the rect the game itself settles into - GTA IV "
                         "reverts foreign positions, so a forced rect is "
                         "best-effort only)")
    ap.add_argument("--no-park", action="store_true",
                    help="legacy alias of the default natural-rect mode")
    ap.add_argument("--no-window-mgmt", action="store_true",
                    help="do not touch the game window at all (no raise, no "
                         "occluder minimize) - only the trigger is re-armed; "
                         "isolates whether window management itself breaks "
                         "painting")
    ap.add_argument("--drive", default="none",
                    choices=("none", "menu_play", "wander"),
                    help="scripted X360-pad input during the session "
                         "(pad.py, needs ViGEmBus+vgamepad). menu_play = "
                         "legal->select->menu->PLAY->gameplay wander; "
                         "wander = gameplay motion only; none = no input. "
                         "Thread starts when the game window appears and "
                         "stops with the session; attach failure just "
                         "logs and skips")
    ap.add_argument("--keep-raw", action="store_true",
                    help="keep the raw BGRA capture (huge: w*h*4*fps*dur)")
    ap.add_argument("--no-warm", action="store_true",
                    help="skip the daemon warm-up frame")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--window-timeout", type=float, default=90.0)
    ap.add_argument("--settle-secs", type=float, default=1.5,
                    help="rect must be unchanged this long to count as stable")
    ap.add_argument("--crf", type=int, default=12,
                    help="libx264 crf for the mp4 (default 12, lossless-ish)")
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--m11d", default=M11D)
    ap.add_argument("--weights", default=None)
    ap.add_argument("--ddarec", default=DDAREC)
    ap.add_argument("--ffmpeg", default=FFMPEG)
    return ap.parse_args()


def main():
    global LOG
    a = parse_args()
    outdir = os.path.abspath(a.out)
    os.makedirs(outdir, exist_ok=True)
    LOG = Logger(os.path.join(outdir, "capture.log"))

    t_start = time.time()
    ts = {"t_start": t_start, "iso_start": time.strftime(
        "%Y-%m-%dT%H:%M:%S", time.localtime(t_start))}

    game_dir = os.path.dirname(a.game)
    cmdline_path = os.path.join(game_dir, "commandline.txt")
    m11d_dir = os.path.dirname(a.m11d)
    weights = a.weights or next(
        (p for p in WEIGHTS_CANDIDATES if os.path.exists(p)), WEIGHTS_CANDIDATES[0])

    manifest = {
        "tool": "capture_session.py",
        "mode": a.mode,
        "commandline_txt": MODE_ARGS[a.mode],
        "requested_fps": a.fps,
        "requested_dur_s": a.dur,
        "fg_mode": a.fg,
        "native": bool(a.native),
        "drive": a.drive,
        "park": None if a.no_park else a.park,
        "paths": {"game": a.game, "m11d": a.m11d, "weights": weights,
                  "ddarec": a.ddarec, "ffmpeg": a.ffmpeg, "outdir": outdir},
        "pids": {}, "timestamps": ts, "status": "starting",
    }

    # spawned-process bookkeeping for the finally sweep
    procs = {"daemon": None, "game": None, "rec": None, "enc": None}
    daemon_spawned = False
    stop_keeper = threading.Event()
    keeper_thread = [None]
    stop_drive = threading.Event()
    drive_thread = [None]
    minimized_hwnds = []          # windows hidden for the run; restored in finally
    layer_log_off = 0
    rec_region = None
    raw_path = os.path.join(outdir, "capture.raw")
    mp4_path = os.path.join(outdir, "capture.mp4")
    env_used = {}
    stage = "init"

    def set_stage(s):
        manifest["status"] = s

    def write_manifest():
        manifest["timestamps"]["t_end"] = time.time()
        manifest["timestamps"]["elapsed_s"] = round(time.time() - t_start, 3)
        tmp = os.path.join(outdir, "manifest.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        os.replace(tmp, os.path.join(outdir, "manifest.json"))

    def kill_proc(p, name):
        if not p:
            return
        try:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=5)
        except Exception as e:
            log("[kill] %s: %r" % (name, e))
        log("[kill] %s rc=%s" % (name, p.poll()))

    # ---- original commandline.txt stash -----------------------------------
    cmdline_orig = None
    try:
        with open(cmdline_path, "rb") as f:
            cmdline_orig = f.read()
    except OSError:
        cmdline_orig = None  # file did not exist -> delete on restore

    try:
        # ---------- init checks ----------
        set_stage("init")
        for label, p in (("game", a.game), ("ddarec", a.ddarec),
                         ("ffmpeg", a.ffmpeg)):
            if not os.path.exists(p):
                raise RuntimeError("%s not found: %s" % (label, p))
        use_daemon = not (a.no_daemon or a.native)
        if use_daemon:
            if not os.path.exists(a.m11d):
                raise RuntimeError("m11d not found: %s" % a.m11d)
            if not os.path.exists(weights):
                raise RuntimeError("weights not found: %s" % weights)
        if exe_running("GTAIV.exe"):
            raise RuntimeError("GTAIV.exe already running - refuse a "
                               "second instance")
        log("[init] mode=%s dur=%s fps=%d out=%s" % (a.mode, a.dur, a.fps, outdir))
        log("[init] game=%s" % a.game)
        log("[init] weights=%s" % weights)

        # rough free-space guard for the raw file
        park = None
        if a.park:
            park = tuple(int(v) for v in a.park.split(","))
            if len(park) != 4:
                raise RuntimeError("--park must be x,y,w,h")
        est_w, est_h = (park[2], park[3]) if park else (816, 638)
        need = int(est_w) * int(est_h) * 4 * a.fps * a.dur
        free = shutil.disk_usage(outdir).free
        if free < need:
            raise RuntimeError(
                "not enough free space for raw capture: need %.1f GB, have %.1f GB"
                % (need / 1e9, free / 1e9))
        if need > 2e9:
            log("[init] raw estimate %.1f GB (kept only with --keep-raw)"
                % (need / 1e9))

        # ---------- commandline.txt ----------
        set_stage("commandline")
        with open(cmdline_path, "wb") as f:
            f.write(MODE_ARGS[a.mode].encode("ascii"))
        log("[cmdline] wrote %s mode args to commandline.txt" % a.mode)
        manifest["commandline_orig_existed"] = cmdline_orig is not None

        # ---------- layer protocol ----------
        arm_layer_protocol(a.fg)
        try:
            layer_log_off = os.path.getsize(LAYER_LOG)
        except OSError:
            layer_log_off = 0

        # ---------- daemon ----------
        set_stage("daemon")
        daemon_preexisting = False
        if use_daemon:
            if port_open(a.port):
                daemon_preexisting = True
                log("[m11d] port %d already answers - using the running "
                    "daemon (will NOT kill it)" % a.port)
            else:
                daemon_log = os.path.join(outdir, "m11d.log")
                dargs = [a.m11d, "--port", str(a.port),
                         "--gain", "1.300", "--intensity", "1.300",
                         "--local-tone", "1.000", "--local-structure", "1.000",
                         "--skin-structure", "-1.000", "--auto-mask", "0",
                         "--style-index", "0", "--color-strength", "0.950",
                         "--scale", "0.58", "--framegen", str(a.fg),
                         "--log", daemon_log, "--weights", weights]
                procs["daemon"] = subprocess.Popen(
                    dargs, cwd=m11d_dir,
                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, close_fds=True)
                daemon_spawned = True
                log("[m11d] spawned pid=%d: %s"
                    % (procs["daemon"].pid, " ".join(dargs)))
                if not wait_port(a.port, 60):
                    raise RuntimeError("m11d port %d never came up" % a.port)
                ts["t_daemon_up"] = time.time()
                log("[m11d] port up at +%.1fs" % (time.time() - t_start))
            if not a.no_warm:
                ts["t_warm_start"] = time.time()
                daemon_warmup(a.port, 800, 600)
                ts["t_warm_done"] = time.time()
        else:
            log("[m11d] skipped (--no-daemon or --native)")
        manifest["daemon"] = {
            "used": use_daemon, "spawned": daemon_spawned,
            "preexisting": daemon_preexisting,
            "pid": procs["daemon"].pid if procs["daemon"] else None,
            "port": a.port}
        manifest["pids"]["daemon"] = manifest["daemon"]["pid"]

        # ---------- game env ----------
        env = dict(os.environ)
        if a.native:
            for k in list(env):
                if k.startswith(LAYER_PREFIXES):
                    del env[k]
            env_used = {}
        else:
            env_used = base_layer_env(a.port, a.fg)
        env_overrides = {}
        for kv in a.env:
            if "=" not in kv:
                raise RuntimeError("--env needs KEY=VAL, got %r" % kv)
            k, v = kv.split("=", 1)
            k = k.strip()
            if v == "":
                env_used.pop(k, None)
                env.pop(k, None)
                env_overrides[k] = None
            else:
                env_used[k] = v
                env_overrides[k] = v
        env.update(env_used)
        manifest["env_layer"] = env_used
        manifest["env_overrides"] = env_overrides

        # ---------- launch ----------
        set_stage("launch")
        procs["game"] = subprocess.Popen([a.game], cwd=game_dir, env=env)
        manifest["pids"]["game"] = procs["game"].pid
        ts["t_launch"] = time.time()
        log("[game] launched pid=%d" % procs["game"].pid)

        # ---------- window appear + stabilize ----------
        set_stage("window")
        rc = None
        deadline = time.time() + a.window_timeout
        while time.time() < deadline:
            rc = game_window_rect()
            if rc and rc[2] > 200:
                break
            if procs["game"].poll() is not None:
                raise RuntimeError("game exited before showing a window "
                                   "(rc=%s)" % procs["game"].poll())
            time.sleep(0.25)
        if not rc:
            raise RuntimeError("no GTA IV window within %.0fs"
                               % a.window_timeout)
        ts["t_window_seen"] = time.time()
        log("[win] appeared %s at +%.1fs" % (rc, time.time() - t_start))

        # ---------- keeper: defend the window from first sight ----------
        # GTA IV latches a DXGI occluded state if its window sits covered
        # during boot - the game keeps rendering internally but the window
        # never repaints again (frozen first screen). The keeper therefore
        # starts NOW, not after the settle wait: it keeps the game on top
        # of the normal z-band and minimizes whatever occludes it, around
        # the live rect until the record rect is frozen below.
        target_rect = [None]     # None = defend live rect; tuple = defend that
        win_state = {"had": True}

        def keeper():
            while not stop_keeper.is_set():
                # re-arm the trigger every tick: anything clearing it
                # mid-run would silently route every present native.
                try:
                    open(TRIG_FLAG, "w").close()
                except OSError:
                    pass
                if a.no_window_mgmt:
                    time.sleep(0.25)
                    continue
                cur = (game_hwnd(procs["game"].pid)
                       or game_hwnd_any())
                # window lifecycle telemetry: a run whose window vanished
                # (benchmark auto-exit, crash, mode switch) must be visible
                # in the log, not silently record the desktop
                if cur and not win_state["had"]:
                    log("[win] re-appeared hwnd=%s" % cur)
                elif not cur and win_state["had"]:
                    log("[win] LOST - no visible gtaiv window (game exited "
                        "or mode switch); proc_alive=%s"
                        % exe_running("GTAIV.exe"))
                win_state["had"] = bool(cur)
                if cur:
                    t = target_rect[0]
                    rect_now = window_rect(cur)
                    if t is not None and rect_now != t:
                        set_pos_top(cur, *t)
                        log("[park] re-parked hwnd=%s to target" % cur)
                        sx, sy, sw, sh = t
                    else:
                        raise_normal_band(cur)
                        sx, sy, sw, sh = (t if t is not None else rect_now)
                    # GTA IV's intro timers only tick while its window is
                    # FOREGROUND - a z-raise does not activate. Log only
                    # on a real transition (activate_foreground returns
                    # True every tick once held).
                    if (u32.GetForegroundWindow() != cur
                            and activate_foreground(cur)):
                        log("[park] game window activated hwnd=%s" % cur)
                    # clear occluders around the (live or frozen) rect
                    pts = [(sx + sw // 2, sy + sh // 2),
                           (sx + 6, sy + 6), (sx + sw - 6, sy + 6),
                           (sx + 6, sy + sh - 6), (sx + sw - 6, sy + sh - 6)]
                    for px, py in pts:
                        top = root_window_at(px, py)
                        if top and top != cur and occluder_ok(top):
                            minimize(top)
                            minimized_hwnds.append(top)
                            log("[park] minimized occluder hwnd=%s" % top)
                time.sleep(0.25)

        keeper_thread[0] = threading.Thread(target=keeper, daemon=True)
        keeper_thread[0].start()

        # ---------- drive thread: scripted X360 pad input (optional) -----
        # Starts NOW, with the window already appearing/defended: the
        # sequence clock counts from here (~+21 s post-launch on this
        # host), so early events land on the legal/intro screens and the
        # menu-arrival sweep brackets the (~110-200 s varying) main menu.
        # The keeper owns foreground state - pad presses need it and the
        # keeper already maintains it; this thread must ONLY call the pad.
        # All input failures are soft: no vgamepad/ViGEmBus -> logged skip,
        # never a failed capture.
        def drive():
            if a.drive == "none":
                return
            try:
                import pad
            except Exception as e:
                log("[drive] pad module unavailable: %r - skipped" % e)
                return
            ctl = pad.Pad(log=lambda m: log("[drive] " + m))
            if not ctl.attach():
                return
            events = [e for e in pad.SEQUENCES.get(a.drive, ())]
            log("[drive] %s: %d events over %.0fs"
                % (a.drive, len(events),
                   events[-1][0] if events else 0))
            try:
                ctl.play_sequence(events, stop=stop_drive)
            except Exception as e:
                log("[drive] sequence aborted: %r" % e)
            finally:
                ctl.release()

        if a.drive != "none":
            drive_thread[0] = threading.Thread(target=drive, daemon=True)
            drive_thread[0].start()
            ts["t_drive_start"] = time.time()
            log("[drive] thread started (t0 = sequence clock)")

        # stabilize: identical rect for settle_secs, THEN freeze it as the
        # record rect (--park overrides with a foreign rect, best-effort)
        stable_since = time.time()
        last = rc
        while time.time() - stable_since < a.settle_secs:
            if time.time() > deadline + 30:
                break
            time.sleep(0.25)
            cur = game_window_rect() or last
            if cur != last:
                last = cur
                stable_since = time.time()
        rc = last
        ts["t_window_stable"] = time.time()
        log("[win] stable rect=%s" % (rc,))

        target = park if park else tuple(int(v) for v in rc)
        target_rect[0] = target
        rec_region = normalize_region(*target)
        manifest["window_rect"] = list(rec_region)
        region = "%d,%d,%d,%d" % rec_region
        log("[rec] region=%s" % region)

        if a.pre_roll > 0:
            log("[pre-roll] letting the game run %.0fs before recording"
                % a.pre_roll)
            ts["t_preroll_start"] = time.time()
            stop_keeper.wait(a.pre_roll)
            ts["t_preroll_end"] = time.time()
            # the window may have moved during the wait - re-anchor the
            # capture rect on its CURRENT position (keeper re-defends it)
            if not park:
                cur = game_window_rect()
                if cur and tuple(int(v) for v in cur) != target:
                    target = tuple(int(v) for v in cur)
                    target_rect[0] = target
                    rec_region = normalize_region(*target)
                    manifest["window_rect"] = list(rec_region)
                    region = "%d,%d,%d,%d" % rec_region
                    log("[pre-roll] window moved -> new region=%s" % region)

        # ---------- record (two-pass via raw - pipe glitches >60fps) -------
        set_stage("record")
        rec_cmd = [a.ddarec, str(a.dur), str(a.fps), region, raw_path]
        log("[rec] %s" % " ".join(rec_cmd))
        ts["t_rec_start"] = time.time()
        rec_log = os.path.join(outdir, "ddarec.log")
        with open(rec_log, "wb") as dn, open(rec_log + ".err", "wb") as ef:
            procs["rec"] = subprocess.Popen(rec_cmd, stdout=dn, stderr=ef)
        try:
            procs["rec"].wait(timeout=a.dur * 2 + 90)
        except subprocess.TimeoutExpired:
            log("[rec] ddarec overran its window - killing")
            procs["rec"].kill()
            procs["rec"].wait(timeout=10)
        ts["t_rec_end"] = time.time()
        try:
            rec_err = open(rec_log + ".err", "rb").read()
        except OSError:
            rec_err = b""
        if rec_err:
            tail = rec_err.decode("utf-8", "replace").strip()[-500:]
            log("[rec] ddarec stderr tail: %s" % tail)
            manifest["ddarec_stderr_tail"] = tail
        manifest["ddarec_rc"] = procs["rec"].returncode
        if procs["rec"].returncode != 0 or not os.path.exists(raw_path):
            raise RuntimeError("ddarec failed rc=%s" % procs["rec"].returncode)
        raw_bytes = os.path.getsize(raw_path)
        log("[rec] raw %.1f MB" % (raw_bytes / 1e6))
        manifest["raw_bytes"] = raw_bytes
        manifest["raw_path"] = raw_path if a.keep_raw else None

        # ---------- encode ----------
        set_stage("encode")
        w, h = rec_region[2], rec_region[3]
        ff_cmd = [a.ffmpeg, "-y", "-hide_banner", "-loglevel", "warning",
                  "-f", "rawvideo", "-pix_fmt", "bgra",
                  "-s", "%dx%d" % (w, h), "-r", str(a.fps), "-i", raw_path,
                  "-c:v", "libx264", "-preset", "veryfast",
                  "-crf", str(a.crf), "-pix_fmt", "yuv420p", mp4_path]
        log("[enc] %s" % " ".join(ff_cmd))
        ff_log = os.path.join(outdir, "ffmpeg.log")
        with open(ff_log, "wb") as lf:
            procs["enc"] = subprocess.Popen(ff_cmd, stdout=lf,
                                            stderr=subprocess.STDOUT)
            procs["enc"].wait(timeout=max(300, a.dur * 10))
        manifest["ffmpeg_rc"] = procs["enc"].returncode
        if procs["enc"].returncode != 0 or not os.path.exists(mp4_path):
            raise RuntimeError("ffmpeg failed rc=%s (see ffmpeg.log)"
                               % procs["enc"].returncode)
        manifest["mp4_path"] = mp4_path
        manifest["mp4_bytes"] = os.path.getsize(mp4_path)
        log("[enc] mp4 %.1f MB -> %s"
            % (manifest["mp4_bytes"] / 1e6, mp4_path))
        if not a.keep_raw:
            try:
                os.remove(raw_path)
            except OSError:
                pass

        # ---------- layer serve evidence ----------
        seg = tail_file(LAYER_LOG, layer_log_off)
        if seg:
            seg_path = os.path.join(outdir, "nr_layer_win_segment.log")
            with open(seg_path, "w", encoding="utf-8") as f:
                f.write(seg)
            serves = len(re.findall(r"\b(aserv|astale|ares|arej|serv)", seg))
            manifest["layer_log_segment"] = seg_path
            manifest["layer_serve_lines"] = serves
            log("[layer] %d serve-class lines in new log segment" % serves)

        set_stage("done")
        write_manifest()
        log("RESULT ok mode=%s dur=%.0fs fps=%d region=%s mp4=%s "
            "mp4_bytes=%d daemon_pid=%s game_pid=%s serves=%s"
            % (a.mode, a.dur, a.fps, region, mp4_path,
               manifest["mp4_bytes"], manifest["pids"]["daemon"],
               manifest["pids"]["game"],
               manifest.get("layer_serve_lines")))

    except Exception as e:
        set_stage("fail:" + stage)
        manifest["error"] = "%r" % e
        try:
            write_manifest()
        except Exception:
            pass
        log("RESULT fail stage=%s err=%r" % (stage, e))
        log(traceback.format_exc())

    finally:
        stop_keeper.set()
        stop_drive.set()
        if drive_thread[0]:
            drive_thread[0].join(timeout=5)
        if keeper_thread[0]:
            keeper_thread[0].join(timeout=3)
        # put back every window we minimized for the capture
        for mh in minimized_hwnds:
            try:
                restore(mh)
            except Exception:
                pass
        if minimized_hwnds:
            log("[cleanup] restored %d occluder window(s)" % len(minimized_hwnds))
        kill_proc(procs["rec"], "ddarec")
        kill_proc(procs["enc"], "ffmpeg")
        kill_proc(procs["game"], "GTAIV.exe")
        if daemon_spawned:
            kill_proc(procs["daemon"], "m11d")
        else:
            log("[cleanup] daemon not spawned by us - left running")
        # restore commandline.txt byte-exact (delete if it did not exist)
        try:
            if cmdline_orig is None:
                if os.path.exists(cmdline_path):
                    os.remove(cmdline_path)
            else:
                with open(cmdline_path, "wb") as f:
                    f.write(cmdline_orig)
            log("[cleanup] commandline.txt restored")
        except OSError as e:
            log("[cleanup] commandline.txt restore failed: %r" % e)
        LOG.close()


if __name__ == "__main__":
    main()
