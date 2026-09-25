# ============================================================================
# m13.splash - the startup splash (owner asked for an easter egg at launch).
# A frameless mini-window with a pulsing neural-net doodle, rotating tongue
# -in-cheek loading lines, and a progress bar tied to the real bring-up.
# Pure decoration: it never blocks the autosetup running on the worker.
# ============================================================================
import math
import random
import tkinter as tk

from .i18n import splash_lines, tr

BG = "#0d1017"
ACCENT = "#5b8cff"
ACCENT2 = "#9a6bff"
FG = "#e6e9f0"
MUTED = "#8a91a5"

MIN_SHOW_MS = 3400


class Splash:
    """Centered frameless splash; call start(), then close() when ready."""

    W, H = 480, 300

    def __init__(self, root):
        self.root = root
        self.win = tk.Toplevel(root)
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg=BG, highlightthickness=1,
                           highlightbackground=ACCENT)
        sw = self.win.winfo_screenwidth()
        sh = self.win.winfo_screenheight()
        self.win.geometry("%dx%d+%d+%d" % (
            self.W, self.H, (sw - self.W) // 2, (sh - self.H) // 2))
        self._t = 0
        self._closed = False
        self._shown_at = None
        self._build()

    def _build(self):
        self.cv = tk.Canvas(self.win, width=self.W, height=150, bg=BG,
                            highlightthickness=0)
        self.cv.pack(pady=(18, 0))
        tk.Label(self.win, text="D L S S   5", bg=BG, fg=FG,
                 font=("Segoe UI", 22, "bold")).pack()
        tk.Label(self.win, text=tr("splash_subtitle"),
                 bg=BG, fg=MUTED, font=("Segoe UI", 9)).pack()
        self._lines = splash_lines()
        self.line = tk.Label(self.win, text=random.choice(self._lines), bg=BG,
                             fg=ACCENT, font=("Segoe UI", 9, "italic"))
        self.line.pack(pady=(8, 0))
        self.bar_bg = tk.Canvas(self.win, width=280, height=4, bg="#1f2430",
                                highlightthickness=0)
        self.bar_bg.pack(pady=(12, 0))
        self.bar = self.bar_bg.create_rectangle(0, 0, 0, 4, fill=ACCENT,
                                                width=0)
        # neural-net doodle: 3 layers of nodes + edges, animated alpha pulse
        rnd = random.Random(5)
        self.nodes = []
        layers = (4, 6, 3)
        xs = (90, 240, 390)
        for li, count in enumerate(layers):
            ys = [30 + i * (90 // max(1, count - 1)) for i in range(count)]
            for y in ys:
                x = xs[li] + rnd.randint(-8, 8)
                self.nodes.append((x, y + 8, rnd.uniform(0, math.pi * 2)))
        self.edges = []
        l1, l2, l3 = self.nodes[:4], self.nodes[4:10], self.nodes[10:]
        for a in l1:
            for b in l2:
                self.edges.append((a, b))
        for a in l2:
            for b in l3:
                self.edges.append((a, b))

    def start(self):
        import time
        self._shown_at = time.time()
        self._tick()

    def _tick(self):
        if self._closed:
            return
        self._t += 1
        t = self._t
        self.cv.delete("all")
        for a, b in self.edges:
            phase = (a[2] + b[2] + t * 0.06) % (math.pi * 2)
            glow = (math.sin(phase) + 1) / 2
            color = ACCENT if glow > 0.7 else "#26304a"
            self.cv.create_line(a[0], a[1], b[0], b[1], fill=color, width=1)
        for x, y, ph in self.nodes:
            r = 3.2 + 1.6 * math.sin(ph + t * 0.08)
            color = ACCENT2 if math.sin(ph + t * 0.08) > 0.5 else ACCENT
            self.cv.create_oval(x - r, y - r, x + r, y + r, fill=color,
                                outline="")
        if t % 26 == 0:
            self.line.config(text=random.choice(self._lines))
        progress = min(1.0, t / (MIN_SHOW_MS / 60.0))
        self.bar_bg.coords(self.bar, 0, 0, 280 * progress, 4)
        self.win.after(60, self._tick)

    def ready_to_close(self):
        import time
        return self._shown_at is not None and \
            (time.time() - self._shown_at) * 1000 >= MIN_SHOW_MS

    def close(self):
        self._closed = True
        try:
            self.win.destroy()
        except tk.TclError:
            pass
