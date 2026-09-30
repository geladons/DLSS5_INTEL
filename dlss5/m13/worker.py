# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.worker - the async action queue (moved out of controller.py) plus the
# watchdog that keeps it responsive. One ActionWorker thread runs every
# mutating action sequentially; the Tk thread never blocks on it.
#
# Stage 25 addition: a job that wedges (elevated deploy waiting on an
# invisible UAC prompt, a daemon stop poll) used to freeze every later
# action. WorkerSupervisor notices a job exceeding JOB_TIMEOUT_S, abandons
# the old worker (its result is still delivered when it finally unblocks)
# and starts a fresh worker on the SAME queue so queued jobs are not lost.
# ============================================================================
import queue
import threading
import time

# Above the 120 s elevated-copy timeout - a legitimately slow job must not
# be mistaken for a stuck one.
JOB_TIMEOUT_S = 150.0


class ActionWorker(threading.Thread):
    """One sequential worker for every mutating action (daemon start/stop,
    gain push, deploy, pause/resume). Keeps the Tk thread at 0 ms of I/O."""

    def __init__(self, on_result, q=None, clock=time.monotonic):
        super().__init__(daemon=True, name="m13-actions")
        self.on_result = on_result        # called from THIS thread
        self.q = q if q is not None else queue.Queue()
        self._clock = clock
        self._stop_ev = threading.Event()
        self._abandon = False
        self._job_lock = threading.Lock()
        self._cur = None                  # (name, started_monotonic) or None

    def submit(self, fn, args=()):
        self.q.put((fn, args))

    def stop(self):
        self._stop_ev.set()
        self.q.put((None, ()))

    def abandon(self):
        """Exit after the CURRENT job reports its result; the queue (which
        may be shared with a replacement worker) keeps its remaining jobs."""
        self._abandon = True
        self.q.put((None, ()))            # wake the loop if it is idle

    def current_job(self):
        """(job_name, started_monotonic) while a job runs, else None."""
        with self._job_lock:
            return self._cur

    def run(self):
        while not self._stop_ev.is_set():
            fn, args = self.q.get()
            if fn is None:
                if self._abandon or self._stop_ev.is_set():
                    break
                continue
            name = getattr(fn, "__qualname__", repr(fn))
            with self._job_lock:
                self._cur = (name, self._clock())
            try:
                ok, msg = fn(*args)
            except Exception as e:
                ok, msg = False, "error: %s" % e
            with self._job_lock:
                self._cur = None
            try:
                self.on_result(ok, msg)
            except Exception:
                pass
            if self._abandon:
                break


class WorkerSupervisor(threading.Thread):
    """Watchdog over ctl.worker: swaps in a fresh ActionWorker when the
    current job exceeds the timeout. check() is public so tests can drive
    it deterministically; run() just calls check() every period."""

    def __init__(self, ctl_like, timeout_s=JOB_TIMEOUT_S, period_s=5.0,
                 clock=time.monotonic, log_fn=print):
        super().__init__(daemon=True, name="m13-worker-watchdog")
        self.ctl = ctl_like
        self.timeout_s = timeout_s
        self.period_s = period_s
        self.clock = clock
        self.log_fn = log_fn
        self._stop_ev = threading.Event()

    def stop(self):
        self._stop_ev.set()

    def check(self):
        w = getattr(self.ctl, "worker", None)
        if w is None:
            return
        job = w.current_job()
        if not job:
            return
        name, started = job
        age = self.clock() - started
        if age <= self.timeout_s:
            return
        try:
            self.log_fn("[worker] job %s stuck for %ds; starting a fresh "
                        "worker (its result will still be logged)"
                        % (name, int(age)))
        except Exception:
            pass
        w.abandon()
        new = ActionWorker(w.on_result, q=w.q, clock=self.clock)
        new.start()
        self.ctl.worker = new

    def run(self):
        while not self._stop_ev.wait(self.period_s):
            try:
                self.check()
            except Exception as e:
                try:
                    self.log_fn("[worker] watchdog error: %s" % e)
                except Exception:
                    pass
