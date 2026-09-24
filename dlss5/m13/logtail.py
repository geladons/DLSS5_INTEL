# ============================================================================
# m13.logtail - threaded line tails of the component logs into UI callbacks.
#
# Tailed sources (whatever exists):
#   work\_m11\m11d.log        daemon stdout (started via manager/_start_daemon)
#   %TEMP%\m12_dxgi.log       DX12 proxy (M12_LOG default)
#   %TEMP%\nr_layer_win.log   m11 Vulkan layer
#   docs\m8b-live.log         screen-mode overlay
# The UI passes a callback fired from a worker thread; marshal to the UI
# thread there (tkinter is not thread-safe).
# ============================================================================
import os
import threading
import time

from . import paths

M11D_LOG = os.path.join(paths.find("logs"), "m11d.log")
M12_LOG = os.path.join(os.environ.get("TEMP", "."), "m12_dxgi.log")
LAYER_LOG = os.path.join(os.environ.get("TEMP", "."), "nr_layer_win.log")
M8B_LOG = os.path.join(paths.find("logs"), "m8b-live.log")

DEFAULT_SOURCES = [
    ("m11d", M11D_LOG),
    ("m12", M12_LOG),
    ("layer", LAYER_LOG),
    ("m8blive", M8B_LOG),
]

POLL_S = 0.25


class LogTail(threading.Thread):
    """Follow one file; callback(tag, line) for each new line. Daemon thread."""

    def __init__(self, tag, path, callback, skip_backlog=True):
        super().__init__(daemon=True, name="logtail-" + tag)
        self.tag = tag
        self.path = path
        self.callback = callback
        self.skip_backlog = skip_backlog
        self._stop = threading.Event()

    def run(self):
        pos = None     # None = not positioned yet
        while not self._stop.is_set():
            try:
                size = os.path.getsize(self.path)
                if pos is None:
                    # tail semantics: only NEW lines, not the whole history
                    # (m8b-live.log can be many MB - flooding the UI at
                    # startup wedges it; see() per line is quadratic)
                    pos = size if self.skip_backlog else 0
                if size < pos:      # rotated/truncated -> restart at head
                    pos = 0
                if size > pos:
                    with open(self.path, "r", errors="replace") as f:
                        f.seek(pos)
                        data = f.read()
                        pos = f.tell()
                    for line in data.splitlines():
                        self.callback(self.tag, line)
            except OSError:
                pass               # file not there yet (component down)
            self._stop.wait(POLL_S)

    def stop(self):
        self._stop.set()


class LogHub:
    """Owns all tails; one callback(tag, line) for everything."""

    def __init__(self, callback, sources=DEFAULT_SOURCES):
        self.tails = [LogTail(tag, path, callback) for tag, path in sources]

    def start(self):
        for t in self.tails:
            t.start()

    def stop(self):
        for t in self.tails:
            t.stop()
