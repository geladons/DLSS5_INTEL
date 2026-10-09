# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# test_launcher.py - Stage 25 Task C unit tests for GameLauncher,
# launch_game_async, Deployer sister-dir isolation and WorkerSupervisor.
# Runs fully offline: find_pid is injected, launch_game / deploy / daemon
# are patched - no real game, daemon or UI is touched.
# ============================================================================
import os
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13.config import Config
from m13.controller import M13Controller
from m13.launcher import GameLauncher, STALE_GUARD_S
from m13.worker import ActionWorker, WorkerSupervisor
from m13 import gamelaunch
from m13 import deploy as deploy_mod


def _write_exe(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("dummy")


class LauncherBase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.td = self._td.name
        self.addCleanup(self._td.cleanup)
        cfg_file = os.path.join(self.td, "test_cfg.json")
        self.cfg = Config(cfg_file)
        self.cfg.set("scale", 0.58)
        self.cfg.set("blend", 0.75)
        self.ctl = M13Controller(self.cfg)
        self.addCleanup(self.ctl.shutdown)
        self.logs = []
        self.pids = {}          # basename.lower() -> pid or None
        self.launcher = GameLauncher(
            self.ctl, log_fn=self.logs.append,
            find_pid_fn=lambda name: self.pids.get(name.lower()))

    def _mk(self, name="game.exe", mode="vulkan", arch="x64", sub=""):
        exe = os.path.join(self.td, sub, name)
        _write_exe(exe)
        self.cfg.set("games", dict(self.cfg.get("games") or {},
                                   **{exe: {"mode": mode, "arch": arch,
                                            "name": os.path.splitext(name)[0]}}))
        return exe

    def _patches(self, launch_ret=(True, "launched pid 1"), deploy_ret=(True, "deployed")):
        """Common patches: no real spawn, no real deploy writes."""
        return (patch("m13.gamelaunch.launch_game",
                      side_effect=self._fake_spawn(launch_ret)),
                patch.object(self.ctl, "game_deploy", return_value=deploy_ret))

    def _launch_p(self, ret=(True, "launched pid 1")):
        return patch("m13.gamelaunch.launch_game",
                     side_effect=self._fake_spawn(ret))

    def _fake_spawn(self, ret):
        """A successful fake spawn makes the spawned process 'exist': the
        launcher's post-spawn verify step polls find_pid for it."""
        def _se(target, *a, **kw):
            if ret[0]:
                self.pids[os.path.basename(target).lower()] = 4242
                # the game exe is what verify looks for - for wrapper
                # spawns register the child as well
                for g_exe, meta in (self.cfg.get("games") or {}).items():
                    if (meta.get("launch_exe")
                            and os.path.basename(meta["launch_exe"]).lower()
                            == os.path.basename(target).lower()):
                        self.pids[os.path.basename(g_exe).lower()] = 4243
            return ret
        return _se


class TestHappyPath(LauncherBase):
    def test_happy_path_x64(self):
        exe = self._mk("game_x64.exe", mode="dx11")
        pl, pd = self._patches()
        with pl as m_lg, pd, \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"), \
             patch.object(self.ctl, "note_launched",
                          wraps=self.ctl.note_launched) as m_note:
            ok, msg = self.launcher.launch(exe)
        self.assertTrue(ok, msg)
        steps = [l for l in self.logs if " step=" in l]
        order = [s.split("step=")[1].split(" ")[0] for s in steps]
        for want in ("resolve", "guard_running", "deploy", "daemon", "spawn"):
            self.assertIn(want, order)
        self.assertLess(order.index("resolve"), order.index("guard_running"))
        self.assertLess(order.index("guard_running"), order.index("deploy"))
        self.assertLess(order.index("deploy"), order.index("daemon"))
        self.assertLess(order.index("daemon"), order.index("spawn"))
        m_note.assert_called_once_with(exe, exe)
        m_lg.assert_called_once_with(exe, "dx11", scale=0.58, blend=0.75,
                                     cfg=self.ctl.cfg, launch_args=[])
        self.assertTrue(any("result=ok" in l for l in self.logs))

    def test_window_flag_appends_windowed_args(self):
        exe = self._mk("game_x64.exe", mode="dx11")
        games = self.ctl.cfg.get("games")
        games[exe]["window"] = True
        pl, pd = self._patches()
        with pl as m_lg, pd, \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ok, msg = self.launcher.launch(exe)
        self.assertTrue(ok, msg)
        self.assertEqual(m_lg.call_args.kwargs["launch_args"],
                         list(gamelaunch.WINDOW_ARGS))

    def test_launch_args_override_window(self):
        exe = self._mk("game_x64.exe", mode="dx11")
        games = self.ctl.cfg.get("games")
        games[exe]["window"] = True
        games[exe]["launch_args"] = "-novid -console"
        pl, pd = self._patches()
        with pl as m_lg, pd, \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ok, msg = self.launcher.launch(exe)
        self.assertTrue(ok, msg)
        self.assertEqual(m_lg.call_args.kwargs["launch_args"],
                         ["-novid", "-console"])


class TestGuards(LauncherBase):
    def test_same_game_running_refused(self):
        exe = self._mk("game_x64.exe")
        self.pids[os.path.basename(exe).lower()] = 4321
        with self._launch_p() as m_lg:
            ok, msg = self.launcher.launch(exe)
        self.assertFalse(ok)
        self.assertIn("already running", msg)
        m_lg.assert_not_called()

    def test_other_game_running_refused(self):
        exe = self._mk("newgame.exe")
        other = self._mk("other.exe")
        self.pids[os.path.basename(other).lower()] = 777
        with self._launch_p() as m_lg:
            ok, msg = self.launcher.launch(exe)
        self.assertFalse(ok)
        self.assertIn("other", msg)
        self.assertIn("still running", msg)
        m_lg.assert_not_called()

    def test_concurrent_launch_refused(self):
        exe = self._mk("game_x64.exe")
        gate = threading.Event()
        done = []

        def slow_launch(target, *a, **kw):
            gate.wait(10)
            self.pids[os.path.basename(target).lower()] = 9
            return True, "launched pid 9"

        with patch("m13.gamelaunch.launch_game", side_effect=slow_launch), \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            t = threading.Thread(target=lambda: done.append(
                self.launcher.launch(exe)), daemon=True)
            t.start()
            # wait until the first launch actually holds the guard
            for _ in range(200):
                if self.launcher.busy():
                    break
                time.sleep(0.01)
            self.assertTrue(self.launcher.busy())
            ok2, msg2 = self.launcher.launch(exe)
            self.assertFalse(ok2)
            self.assertIn("in progress", msg2)
            gate.set()
            t.join(10)
        self.assertTrue(done and done[0][0], done)
        self.assertFalse(self.launcher.busy())
        # third launch proceeds once the guard is released - the fake
        # spawned process is gone by then (game exited)
        self.pids.clear()
        with self._launch_p(), \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ok3, _ = self.launcher.launch(exe)
        self.assertTrue(ok3)

    def test_stale_guard_takeover(self):
        exe = self._mk("game_x64.exe")
        t = [1000.0]
        launcher = GameLauncher(self.ctl, log_fn=self.logs.append,
                                find_pid_fn=lambda n: self.pids.get(n.lower()),
                                clock=lambda: t[0])
        # occupy the guard, then jump the clock past STALE_GUARD_S
        ok, _, _tok = launcher._try_acquire()
        self.assertTrue(ok)
        t[0] += STALE_GUARD_S + 1
        with self._launch_p(), \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ok2, msg2 = launcher.launch(exe)
        self.assertTrue(ok2, msg2)
        self.assertTrue(any("stale" in l.lower() for l in self.logs))


class TestErrorsAndConfig(LauncherBase):
    def test_step_exception_tagged_and_recovers(self):
        exe = self._mk("game_x64.exe")
        with patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="clean"), \
             patch.object(self.ctl, "game_deploy",
                          side_effect=RuntimeError("boom")):
            ok, msg = self.launcher.launch(exe)
        self.assertFalse(ok)
        self.assertIn("step=deploy", msg)
        self.assertFalse(self.launcher.busy())
        with self._launch_p(), \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ok2, _ = self.launcher.launch(exe)
        self.assertTrue(ok2)

    def test_no_config_mutation_on_launch(self):
        # Witcher 3 style layout: both renderer dirs exist; only x64 is
        # registered. The launch must not touch the config at all.
        w3_dx11 = os.path.join(self.td, "The Witcher 3", "bin", "x64",
                               "witcher3.exe")
        w3_dx12 = os.path.join(self.td, "The Witcher 3", "bin", "x64_dx12",
                               "witcher3.exe")
        _write_exe(w3_dx11)
        _write_exe(w3_dx12)
        games_before = {w3_dx11: {"mode": "dx11", "arch": "x64",
                                  "name": "witcher3"}}
        self.cfg.set("games", dict(games_before))
        with self._launch_p() as m_lg, \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ok, msg = self.launcher.launch(w3_dx11)
        self.assertTrue(ok, msg)
        self.assertEqual(self.cfg.get("games"), games_before)
        m_lg.assert_called_once()
        self.assertEqual(m_lg.call_args[0][0], w3_dx11)

    def test_register_path(self):
        exe = os.path.join(self.td, "fresh.exe")
        _write_exe(exe)
        self.cfg.set("games", {})
        with self._launch_p() as m_lg, \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"), \
             patch.object(self.ctl, "add_game",
                          wraps=self.ctl.add_game) as m_add:
            ok, msg = self.launcher.launch(exe, register={"mode": "dx11"})
        m_add.assert_called_once_with(exe, "dx11", launch_exe=None)
        self.assertTrue(ok, msg)
        self.assertIn(exe, self.cfg.get("games") or {})
        m_lg.assert_called_once()


    def test_stale_takeover_keeps_guard(self):
        """k) stale take-over: the wedged launch's release must not clear
        the new owner's guard."""
        exe = self._mk("game_x64.exe")
        t = [0.0]
        launcher = GameLauncher(self.ctl, log_fn=self.logs.append,
                                find_pid_fn=lambda n: self.pids.get(n.lower()),
                                clock=lambda: t[0])
        gate_a = threading.Event()
        gate_b = threading.Event()
        calls = []
        res_a, res_b = [], []

        def slow_launch(target, *a, **kw):
            calls.append(1)
            # B may reach spawn before A (both race after their acquires), so
            # pick the gate by thread identity rather than call order.
            (gate_a if threading.current_thread() is ta else gate_b).wait(10)
            self.pids[os.path.basename(target).lower()] = 9
            return True, "launched"

        with patch("m13.gamelaunch.launch_game", side_effect=slow_launch), \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            ta = threading.Thread(
                target=lambda: res_a.append(launcher.launch(exe)),
                daemon=True)
            ta.start()
            for _ in range(200):
                if launcher.busy():
                    break
                time.sleep(0.01)
            self.assertTrue(launcher.busy())
            t[0] += STALE_GUARD_S + 1          # make A stale
            tb = threading.Thread(
                target=lambda: res_b.append(launcher.launch(exe)),
                daemon=True)
            tb.start()
            for _ in range(200):
                if len(calls) > 1:
                    break
                time.sleep(0.01)
            self.assertEqual(len(calls), 2)    # B took over and reached spawn
            gate_a.set()                        # wedged A finally returns
            ta.join(10)
            self.assertTrue(launcher.busy(), "B must still hold the guard")
            okc, msgc = launcher.launch(exe)
            self.assertFalse(okc)
            self.assertIn("in progress", msgc)
            gate_b.set()
            tb.join(10)
        self.assertTrue(res_a and res_a[0][0], res_a)
        self.assertTrue(res_b and res_b[0][0], res_b)
        self.assertFalse(launcher.busy())


class TestAsync(LauncherBase):
    def test_launch_game_async_busy(self):
        results = []
        self.ctl.worker = MagicMock()
        self.ctl.worker.on_result = lambda ok, msg: results.append((ok, msg))
        gate = threading.Event()

        def slow_launch(target, *a, **kw):
            gate.wait(10)
            self.pids[os.path.basename(target).lower()] = 5
            return True, "launched pid 5"

        exe = self._mk("game_x64.exe")
        with patch("m13.gamelaunch.launch_game", side_effect=slow_launch), \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"):
            self.ctl.launch_game_async(exe)
            for _ in range(200):
                if self.ctl.launcher.busy():
                    break
                time.sleep(0.01)
            self.assertTrue(self.ctl.launcher.busy())
            self.ctl.launch_game_async(exe)   # must report busy immediately
            gate.set()
            for _ in range(200):
                if not self.ctl.launcher.busy():
                    break
                time.sleep(0.01)
        self.assertTrue(any(r[0] is False and "already in progress" in r[1]
                            for r in results), results)


class TestWorkerSupervisor(unittest.TestCase):
    def test_stuck_job_swapped_and_result_delivered(self):
        results = []
        q_results = []
        ctl = MagicMock()
        stuck_gate = threading.Event()
        clock = [time.monotonic()]

        def stuck_job():
            stuck_gate.wait(10)
            return True, "stuck finished"

        worker = ActionWorker(lambda ok, msg: results.append((ok, msg)),
                              clock=lambda: clock[0])
        worker.start()
        ctl.worker = worker
        worker.submit(stuck_job)
        for _ in range(200):
            if worker.current_job():
                break
            time.sleep(0.01)
        self.assertIsNotNone(worker.current_job())

        logs = []
        sup = WorkerSupervisor(ctl, timeout_s=150.0, period_s=9999,
                               clock=lambda: clock[0], log_fn=logs.append)
        sup.check()                      # age ~0: nothing happens
        self.assertIs(ctl.worker, worker)
        clock[0] += 200.0                # jump past timeout
        sup.check()
        new_worker = ctl.worker
        self.assertIsNot(new_worker, worker)
        self.assertTrue(any("stuck" in l for l in logs))

        # a queued job runs on the new worker
        ran = threading.Event()
        new_worker.submit(lambda: (ran.set(), (True, "fresh ok"))[1])
        self.assertTrue(ran.wait(10))

        # the abandoned worker still delivers its result then exits
        stuck_gate.set()
        worker.join(10)
        self.assertFalse(worker.is_alive())
        self.assertTrue(any(m == "stuck finished" for _o, m in results))
        new_worker.stop()
        new_worker.join(10)


class TestDaemonShutdown(LauncherBase):
    """shutdown() must reap the m11d daemon when no x86 game consumes it
    (the manager-close leak), but leave it alone while an x86 game runs -
    killing it mid-game freezes that game's picture on the held frame."""

    def _fake_daemon(self):
        class _D:
            def __init__(self):
                self.pid = 4242
                self.stopped = 0
            @property
            def running(self):
                return self.stopped == 0
            def stop(self):
                self.stopped += 1
                return True, "stopped"
        return _D()

    def test_shutdown_stops_idle_daemon(self):
        self.ctl.daemon = self._fake_daemon()
        exe = self._mk("gtaiv.exe", mode="dx9", arch="x86")
        with patch("m13.controller.find_pid", return_value=None):
            self.ctl._daemon_shutdown()
        self.assertEqual(self.ctl.daemon.stopped, 1)

    def test_shutdown_keeps_daemon_while_x86_game_runs(self):
        self.ctl.daemon = self._fake_daemon()
        exe = self._mk("gtaiv.exe", mode="dx9", arch="x86")
        with patch("m13.controller.find_pid", return_value=777):
            self.ctl._daemon_shutdown()
        self.assertEqual(self.ctl.daemon.stopped, 0)

    def test_shutdown_no_daemon_is_noop(self):
        d = self._fake_daemon()
        d.stop()                     # already dead
        self.ctl.daemon = d
        self.ctl._daemon_shutdown()  # must not raise or double-stop
        self.assertEqual(d.stopped, 1)


class TestX86DaemonKnobs(LauncherBase):
    """x86 daemon-path launches push per-game knobs over NRCT: temporal is
    left at the daemon baseline (engine ON + the Stage-46 motion gate
    suppresses history on fast frames - pinned tone on near-static content,
    no stale-structure injection when things move). meta.temporal still
    forces the member either way."""

    def _x86_launch(self, meta_extra=None):
        exe = self._mk("gtaiv.exe", mode="dx9", arch="x86")
        if meta_extra:
            self.ctl.cfg.get("games")[exe].update(meta_extra)
        client = MagicMock()
        client.warmup.return_value = True
        self.ctl.client = client

        class _Daemon:
            pid = 4242
            running = True
            def stop(self):
                self.running = False
                return True, "stopped"
        self.ctl.daemon = _Daemon()
        pl, pd = self._patches()
        with pl, pd, \
             patch.object(deploy_mod.Deployer, "mode_status",
                          return_value="deployed"), \
             patch("m13.launcher._screen_extent", return_value=(800, 600)), \
             patch.dict(os.environ, {"D5C_LAUNCH_SETTLE_MS": "0"}):
            ok, msg = self.launcher.launch(exe)
        self.assertTrue(ok, msg)
        return client

    def test_x86_temporal_default_no_push(self):
        # No meta.temporal -> no NRCT push; the daemon's env baseline (ON +
        # motion gate) stands.
        client = self._x86_launch()
        client.set_temporal.assert_not_called()

    def test_x86_temporal_meta_true_reenables(self):
        client = self._x86_launch({"temporal": True})
        client.set_temporal.assert_called_once_with(True)

    def test_x86_temporal_meta_false_pushes(self):
        client = self._x86_launch({"temporal": False})
        client.set_temporal.assert_called_once_with(False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
