# ============================================================================
# m13.launcher - GameLauncher: runs ONE game launch as explicit, timed,
# individually guarded steps (register -> resolve -> guard_running ->
# deploy -> daemon -> spawn). Synchronous; the UI calls it on a dedicated
# "m13-launch" thread via ctl.launch_game_async so a blocking step (UAC
# wait, daemon stop poll) never wedges the shared ActionWorker queue.
#
# Stage 25 decisions baked in:
#  - NO config mutation during launch (the old Witcher 3 dx11->dx12 sibling
#    switch rewrote games[] on every click - gone entirely).
#  - Re-entrancy guard with a stale take-over so a wedged launch cannot
#    block launches forever (STALE_GUARD_S).
#  - Every step logs `[launch] game=<name> step=<step> ok=<0|1> ms=<n>` to
#    stdout (lands in manager_err.log) and the optional log_fn.
# ============================================================================
import os
import threading
import time

from . import gamelaunch
from .deploy import Deployer
from .processes import find_pid

STALE_GUARD_S = 180.0


class GameLauncher:
    """Single responsibility: run one launch as explicit steps."""

    def __init__(self, ctl, log_fn=None, find_pid_fn=None,
                 clock=time.monotonic):
        self.ctl = ctl
        self.log_fn = log_fn
        self.find_pid = find_pid_fn or find_pid
        self._clock = clock
        self._lock = threading.Lock()
        self._busy = False
        self._busy_since = 0.0
        self._busy_token = 0

    def busy(self):
        with self._lock:
            return self._busy

    @staticmethod
    def launch_target(exe, meta=None):
        """Exe to actually spawn: the registered launch_exe wrapper when it
        exists, else the game exe itself (falls back if the wrapper is
        missing)."""
        meta = meta or {}
        target = meta.get("launch_exe") or exe
        if target != exe and not os.path.exists(target):
            if os.path.exists(exe):
                return exe
        return target if os.path.exists(target) else exe

    # ------------------------------------------------------------ logging --
    def _emit(self, msg):
        print(msg, flush=True)
        if self.log_fn:
            try:
                self.log_fn(msg)
            except Exception:
                pass

    def _log_step(self, game, step, ok, ms, detail):
        self._emit("[launch] game=%s step=%s ok=%d ms=%d %s"
                   % (game, step, 1 if ok else 0, ms, detail or ""))

    def _finish(self, game, ok, msg):
        self._emit("[launch] game=%s result=%s msg=%s"
                   % (game, "ok" if ok else "fail", msg))
        return ok, msg

    # --------------------------------------------------------------- guard --
    def _try_acquire(self):
        """-> (ok, detail, token). Token is per-launch: a stale take-over
        bumps the generation so the wedged launch's release() cannot clear
        the new owner's guard."""
        with self._lock:
            if self._busy:
                age = self._clock() - self._busy_since
                if age < STALE_GUARD_S:
                    return False, ("another launch is still in progress "
                                   "(%ds)" % int(age)), None
                self._emit("[launch] warn: stale launch guard (%ds) - "
                           "taking over" % int(age))
            self._busy = True
            self._busy_since = self._clock()
            self._busy_token += 1
        return True, "guard acquired", self._busy_token

    def _release(self, token):
        with self._lock:
            if token is not None and token == self._busy_token:
                self._busy = False

    # --------------------------------------------------------- step runner --
    def _timed(self, game, step, fn):
        t0 = self._clock()
        try:
            ok, detail = fn()
        except Exception as e:
            ok, detail = False, "step=%s: %s" % (step, e)
        ms = int((self._clock() - t0) * 1000)
        self._log_step(game, step, ok, ms, detail)
        return ok, detail

    # -------------------------------------------------------------- launch --
    def launch(self, exe, register=None):
        ctl = self.ctl
        game = os.path.splitext(os.path.basename(exe))[0] or exe
        token = None
        meta = None
        target = exe
        daemon_note = None

        def do_acquire():
            nonlocal token
            ok2, detail, token = self._try_acquire()
            return ok2, detail

        try:
            ok, msg = self._timed(game, "guard_busy", do_acquire)
            if not ok:
                return self._finish(game, False, msg)

            def do_register():
                games = ctl.cfg.get("games") or {}
                if exe in games or not isinstance(register, dict):
                    return True, ""
                return ctl.add_game(exe, register.get("mode"),
                                    launch_exe=register.get("launch_exe"))

            ok, msg = self._timed(game, "register", do_register)
            if not ok:
                return self._finish(game, False, msg)

            def do_resolve():
                nonlocal meta
                meta = (ctl.cfg.get("games") or {}).get(exe)
                if not meta:
                    return False, "add the game first"
                mode = meta.get("mode")
                try:
                    from . import gamescan
                    _arch, apis = gamescan.pe_info(exe)
                    if apis and mode not in apis and mode != "vulkan":
                        self._emit("[launch] game=%s warn=mode_mismatch "
                                   "configured=%s detected=%s"
                                   % (game, mode, ",".join(apis)))
                except Exception:
                    pass        # unreadable PE: keep the configured mode
                return True, "mode=%s arch=%s" % (mode, meta.get("arch"))

            ok, msg = self._timed(game, "resolve", do_resolve)
            if not ok:
                return self._finish(game, False, msg)

            def do_guard_running():
                nonlocal target
                target = self.launch_target(exe, meta)
                for cand in (os.path.basename(target), os.path.basename(exe)):
                    pid = self.find_pid(cand)
                    if pid is not None:
                        return False, "%s is already running (pid %s)" % (
                            cand, pid)
                own_base = os.path.basename(exe).lower()
                for other, om in (ctl.cfg.get("games") or {}).items():
                    if other == exe:
                        continue
                    base = os.path.basename(other)
                    if base.lower() == own_base:
                        continue
                    pid = self.find_pid(base)
                    if pid is not None:
                        return False, ("%s is still running - close it "
                                       "before starting %s"
                                       % (om.get("name") or base, game))
                return True, ""

            ok, msg = self._timed(game, "guard_running", do_guard_running)
            if not ok:
                return self._finish(game, False, msg)

            def do_deploy():
                d = Deployer(os.path.dirname(exe))
                if d.mode_status(meta["mode"], meta["arch"]) == "deployed":
                    return True, "already deployed"
                return ctl.game_deploy(exe)

            ok, msg = self._timed(game, "deploy", do_deploy)
            if not ok:
                return self._finish(game, False, msg)

            def do_daemon():
                nonlocal daemon_note
                if meta.get("arch") == "x86":
                    if ctl.daemon.running:
                        return True, "daemon already running"
                    ok2, m2 = ctl.daemon_start()
                    if not ok2 and "already" not in m2:
                        return False, m2
                    return True, m2
                if ctl.daemon.running:
                    ok2, m2 = ctl._ensure_daemon_stopped(log_fn=self._emit)
                    if ok2:
                        daemon_note = m2
                    return ok2, m2
                return True, "daemon not running"

            ok, msg = self._timed(game, "daemon", do_daemon)
            if not ok:
                return self._finish(game, False, msg)

            def do_spawn():
                ok2, m2 = gamelaunch.launch_game(
                    target, meta["mode"], scale=ctl.cfg.get("scale"),
                    blend=ctl.cfg.get("blend"), cfg=ctl.cfg)
                if ok2:
                    ctl.note_launched(exe, target)
                    if target != exe:
                        m2 = "%s (via %s)" % (m2, os.path.basename(target))
                return ok2, m2

            ok, msg = self._timed(game, "spawn", do_spawn)
            if ok and daemon_note:
                msg = "%s | %s" % (daemon_note, msg)
            return self._finish(game, ok, msg)
        finally:
            if token is not None:
                self._release(token)
