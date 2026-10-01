# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.ui_media - the Media tab: offline DLSS 5 processing of photos and
# videos (no game needed). Layout mirrors ui.py / ui_games.py:
#
#   left    queue listbox ([PHOTO]/[VIDEO] tags) + add/remove/clear +
#           Process all / Stop
#   center  Before / After preview (PNG blobs through tk.PhotoImage;
#           "Preview frame" decodes frame 0 of a selected video)
#   right   the SAME knob set the in-game overlay exposes: blend +
#           sharpness sliders + the Phase30Panel (7 vendor knobs). Turning
#           a knob while a PHOTO is selected debounces ~350 ms and
#           reprocesses the loaded original on the worker - the live
#           "overlay for photos" the owner asked for.
#   footer  output dir picker, save-mode combobox (ask/always/subdir),
#           ffmpeg status + Download ffmpeg button.
#
# Threading (same rule as everywhere in m13): NO I/O on the Tk thread.
# Engine work, decodes and encodes run inside ctl.submit() jobs; results
# come back through ui.logq ("__call__", callable) marshalling.
# ============================================================================
import os
import time
import tkinter as tk
from tkinter import filedialog, ttk

from . import media
from .i18n import tr
from .media_img import downscale_nearest, png_b64
from .media_video import VideoJob
from .phase30 import Phase30Panel

BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
BORDER = "#2a3040"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

DEBOUNCE_MS = 350
PREV_W, PREV_H = 400, 330
PROGRESS_MIN_S = 0.25

_FILETYPES = [
    ("media", "*.png *.bmp *.jpg *.jpeg *.mp4 *.mkv *.webm *.avi *.mov "
              "*.m4v *.ts *.wmv"),
    ("photos", "*.png *.bmp *.jpg *.jpeg"),
    ("videos", "*.mp4 *.mkv *.webm *.avi *.mov *.m4v *.ts *.wmv"),
    ("all", "*.*"),
]


class MediaTab:
    """The notebook page. `ui` is ManagerUI (ctl, logq, log(), root)."""

    def __init__(self, ui, nb):
        self.ui = ui
        self.ctl = ui.ctl
        self.logq = ui.logq
        self.queue = media.MediaQueue()
        self.ffm = media.Ffmpeg()
        self.engine = None              # media.EngineSession, worker-owned
        self._sel = -1                  # listbox index of the selection
        self._orig = None               # (w, h, bgra) of the loaded photo
        self._orig_path = None
        self._knobs = {}                # last pushed engine knob dict
        self._deb_id = None
        self._before_img = None
        self._after_img = None
        self._prog_ts = 0.0
        self._init_knobs()
        self._build(nb)
        self._ffmpeg_refresh()

    # ------------------------------------------------------------ state ---
    def _init_knobs(self):
        """Seed the knob dict from config: the 7 phase30 keys plus the
        overlay's blend/sharpness. gain mirrors intensity (same convention
        as gamelaunch: DLSS5_GAIN == intensity)."""
        p30 = self.ctl.cfg.get_phase30()
        self._p30_init = p30
        self._knobs.update(p30)
        self._knobs["sharpness"] = float(self.ctl.cfg.get("sharpness")
                                         or 0.8)
        self._knobs["blend"] = float(self.ctl.cfg.get("blend") or 1.0)
        self._knobs["gain"] = float(p30.get("intensity", 1.0))

    def _call_ui(self, fn):
        """Marshal a callable back to the Tk thread (ui.py drains the
        ("__call__", fn) sentinel from logq)."""
        self.logq.put(("__call__", fn))

    # ------------------------------------------------------------ build ---
    def _build(self, nb):
        t = ttk.Frame(nb, padding=8)
        nb.add(t, text=tr("tab_media"))
        self.frame = t

        main = ttk.Frame(t)
        main.pack(fill="both", expand=True)

        self._build_queue_col(main)
        self._build_preview_col(main)
        self._build_knob_col(main)
        self._build_footer(t)

    def _build_queue_col(self, main):
        left = ttk.Frame(main)
        left.pack(side="left", fill="y", padx=(0, 8))

        listbox_wrap = ttk.Frame(left)
        listbox_wrap.pack(fill="both", expand=True)
        self.lst = tk.Listbox(listbox_wrap, width=34, bg=PANEL, fg=FG,
                              selectbackground=ACCENT,
                              selectforeground="#ffffff", relief="flat",
                              bd=4, highlightthickness=1,
                              highlightcolor=BORDER,
                              highlightbackground=BORDER,
                              activestyle="none", exportselection=False,
                              font=("Consolas", 9))
        sb = ttk.Scrollbar(listbox_wrap, orient="vertical",
                           command=self.lst.yview)
        self.lst.configure(yscrollcommand=sb.set)
        self.lst.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.lst.bind("<<ListboxSelect>>", self._on_select)

        btns = ttk.Frame(left)
        btns.pack(fill="x", pady=(4, 0))
        ttk.Button(btns, text=tr("media_add_files"),
                   command=self._add_files).pack(side="left")
        ttk.Button(btns, text=tr("media_add_folder"),
                   command=self._add_folder).pack(side="left", padx=4)
        ttk.Button(btns, text=tr("media_remove"),
                   command=self._remove_sel).pack(side="left", padx=4)
        ttk.Button(btns, text=tr("media_clear"),
                   command=self._clear).pack(side="left")

        proc = ttk.Frame(left)
        proc.pack(fill="x", pady=(6, 0))
        ttk.Button(proc, text=tr("media_process"), style="Accent.TButton",
                   command=self._process_all).pack(side="left")
        ttk.Button(proc, text=tr("media_stop"), style="Danger.TButton",
                   command=self._stop).pack(side="left", padx=6)
        self.status_lbl = ttk.Label(left, text="", style="Muted.TLabel",
                                    wraplength=270, justify="left")
        self.status_lbl.pack(anchor="w", pady=(6, 0))
        self.prog_lbl = ttk.Label(left, text="", style="Muted.TLabel")
        self.prog_lbl.pack(anchor="w")

    def _build_preview_col(self, main):
        center = ttk.Frame(main)
        center.pack(side="left", fill="both", expand=True, padx=(0, 8))
        boxes = ttk.Frame(center)
        boxes.pack(fill="both", expand=True)
        self.before_lbl, self.before_img_lbl = self._preview_box(
            boxes, tr("media_before"), side="left")
        self.after_lbl, self.after_img_lbl = self._preview_box(
            boxes, tr("media_after"), side="right")
        row = ttk.Frame(center)
        row.pack(fill="x", pady=(6, 0))
        self.prev_btn = ttk.Button(row, text=tr("media_preview_btn"),
                                   command=self._preview_frame)
        self.prev_btn.state(["disabled"])
        self.prev_btn.pack(side="left")
        ttk.Label(row, text=tr("media_engine_hint"),
                  style="Muted.TLabel").pack(side="left", padx=10)

    def _preview_box(self, parent, title, side):
        box = tk.Frame(parent, bg=PANEL2, highlightthickness=1,
                       highlightbackground=BORDER,
                       width=PREV_W + 8, height=PREV_H + 26)
        box.pack_propagate(False)
        box.pack(side=side, fill="both", expand=True,
                 padx=(0, 6) if side == "left" else (6, 0))
        cap = tk.Label(box, text=title, bg=PANEL2, fg=MUTED,
                       font=("Segoe UI", 8, "bold"))
        cap.pack(anchor="w", padx=6, pady=(4, 0))
        img = tk.Label(box, bg=PANEL2, fg=MUTED, text="-",
                       font=("Segoe UI", 9))
        img.pack(fill="both", expand=True, padx=4, pady=4)
        return cap, img

    def _build_knob_col(self, main):
        right = ttk.Frame(main)
        right.pack(side="right", fill="y")

        bs = tk.Frame(right, bg=BG)
        bs.pack(fill="x", pady=(0, 4))
        self.blend_var = tk.DoubleVar(value=self._knobs["blend"])
        self._add_slider(bs, tr("ov_blend"), self.blend_var, 0.0, 1.0,
                         "blend").pack(side="left", fill="x", expand=True,
                                       padx=(0, 6))
        self.sharp_var = tk.DoubleVar(value=self._knobs["sharpness"])
        self._add_slider(bs, tr("sharpness"), self.sharp_var, 0.0, 1.0,
                         "sharpness").pack(side="right", fill="x",
                                          expand=True, padx=(6, 0))

        wrap = tk.Frame(right, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        wrap.pack(fill="x", pady=(2, 4))
        self.p30 = Phase30Panel(
            wrap, initial_values=self._p30_init,
            on_change=self._knob_changed, on_reset=self._knobs_reset,
            bg=PANEL, fg=FG, accent=ACCENT, panel2=PANEL2, muted=MUTED,
            compact=True)
        self.p30.pack(fill="x", padx=8, pady=6)

    def _add_slider(self, parent, label, var, lo, hi, key):
        box = tk.Frame(parent, bg=BG)
        hdr = tk.Frame(box, bg=BG)
        hdr.pack(fill="x")
        tk.Label(hdr, text=label, bg=BG, fg=FG,
                 font=("Segoe UI", 8, "bold")).pack(side="left")
        val_lbl = tk.Label(hdr, text="%.2f" % var.get(), bg=BG, fg=ACCENT,
                           font=("Consolas", 8, "bold"))
        val_lbl.pack(side="right")

        def _upd(_v):
            v = round(float(var.get()), 3)
            val_lbl.config(text="%.2f" % v)
            self._knob_changed(key, v)

        tk.Scale(box, from_=lo, to=hi, resolution=0.05,
                 orient="horizontal", variable=var, command=_upd,
                 bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0,
                 bd=0, activebackground=ACCENT, showvalue=False,
                 sliderrelief="flat").pack(fill="x")
        return box

    def _build_footer(self, t):
        foot = ttk.Frame(t)
        foot.pack(fill="x", pady=(6, 0))
        ttk.Label(foot, text=tr("media_outdir")).pack(side="left")
        self.outdir_var = tk.StringVar(
            value=self.ctl.cfg.get("media_out_dir") or "")
        outdir_entry = ttk.Entry(foot, textvariable=self.outdir_var,
                               width=34)
        outdir_entry.pack(side="left", padx=4)
        outdir_entry.bind("<FocusOut>", lambda _e: self.ctl.cfg.set(
            "media_out_dir", self.outdir_var.get() or None))
        ttk.Button(foot, text=tr("browse"),
                   command=self._pick_outdir).pack(side="left")
        ttk.Label(foot, text=tr("media_save_mode")).pack(side="left",
                                                       padx=(10, 0))
        self._mode_labels = [tr("media_mode_ask"), tr("media_mode_always"),
                             tr("media_mode_subdir")]
        self._mode_keys = ["ask", "always", "subdir"]
        cur = self.ctl.cfg.get("media_save_mode") or "subdir"
        self.mode_var = tk.StringVar(
            value=self._mode_labels[self._mode_keys.index(cur)]
            if cur in self._mode_keys else self._mode_labels[2])
        self.mode_cb = ttk.Combobox(foot, textvariable=self.mode_var,
                                    values=self._mode_labels, width=26,
                                    state="readonly")
        self.mode_cb.pack(side="left", padx=4)
        self.mode_cb.bind("<<ComboboxSelected>>", self._mode_changed)

        self.ff_lbl = ttk.Label(foot, text="", style="Muted.TLabel")
        self.ff_lbl.pack(side="right")
        self.ff_btn = ttk.Button(foot, text=tr("media_dl_ffmpeg"),
                                 command=self._download_ffmpeg)
        # packed on demand by _ffmpeg_refresh()

    # ------------------------------------------------------- queue edits --
    def _add_files(self):
        paths_ = filedialog.askopenfilenames(
            title=tr("media_add_files"), filetypes=_FILETYPES)
        if not paths_:
            return
        n = self.queue.add_many(paths_)
        self._refresh_list()
        self._status("+%d" % n)

    def _add_folder(self):
        d = filedialog.askdirectory(title=tr("media_add_folder"))
        if not d:
            return
        n = self.queue.add_folder(d)
        self._refresh_list()
        self._status("+%d" % n)

    def _remove_sel(self):
        sel = self.lst.curselection()
        if not sel:
            return
        item = self.queue.items[sel[0]]
        self.queue.remove(item.path)
        self._sel = -1
        self._refresh_list()

    def _clear(self):
        self.queue.clear()
        self._sel = -1
        self._refresh_list()

    _STATUS_KEY = {"working": "media_status_work",
                   "done": "media_status_done",
                   "error": "media_status_error",
                   "skipped": "media_status_skip",
                   "cancelled": "media_status_cancel"}

    def _row_text(self, item):
        tag = tr("media_photo_tag") if item.kind == "photo" \
            else tr("media_video_tag")
        tail = ""
        if item.status != "queued":
            tail = "  [%s]" % tr(self._STATUS_KEY.get(item.status,
                                                    "media_status_error"))
        return "[%s] %s%s" % (tag, os.path.basename(item.path), tail)

    def _refresh_list(self):
        self.lst.delete(0, "end")
        for it in self.queue.items:
            self.lst.insert("end", self._row_text(it))

    def _refresh_row(self, item):
        try:
            idx = self.queue.items.index(item)
        except ValueError:
            return
        self.lst.delete(idx)
        self.lst.insert(idx, self._row_text(item))
        if idx == self._sel:
            self.lst.selection_set(idx)

    # --------------------------------------------------------- selection --
    def _selected_item(self):
        sel = self.lst.curselection()
        if not sel or sel[0] >= len(self.queue.items):
            return None
        return self.queue.items[sel[0]]

    def _on_select(self, _e=None):
        sel = self.lst.curselection()
        self._sel = sel[0] if sel else -1
        item = self._selected_item()
        if item is None:
            return
        self.prev_btn.state(
            ["!disabled"] if item.kind == "video" else ["disabled"])
        if item.kind == "photo":
            self._orig = None
            self._orig_path = None
            self._set_text(self.before_img_lbl, "...")
            self._set_text(self.after_img_lbl, "...")
            self.ctl.submit(self._job_load_photo, item.path)
        else:
            self._orig = None
            self._orig_path = None
            self._set_text(self.before_img_lbl, tr("media_video_tag"))
            self._set_text(self.after_img_lbl, "-")

    # --------------------------------------------------------- preview ----
    def _engine(self):
        """WORKER THREAD ONLY: lazily create the one EngineSession."""
        if self.engine is None:
            self.engine = media.EngineSession(cfg=self.ctl.cfg)
        return self.engine

    def engine_close(self):
        """Submitted as a worker job on app shutdown (ui.py _on_close);
        queued FIFO so in-flight media work finishes first."""
        if self.engine is not None:
            self.engine.close()
        return True, "media engine closed"

    def _job_load_photo(self, path):
        """Worker job: decode -> Before preview, then engine -> After."""
        try:
            w, h, bgra = media.PhotoIO.load(path, self.ffm)
        except Exception as e:
            self._call_ui(lambda: self._set_text(
                self.before_img_lbl, "load: %s" % e))
            return False, "load failed: %s" % e
        self._orig = (w, h, bgra)
        self._orig_path = path
        w2, h2, small = downscale_nearest(w, h, bgra, PREV_W, PREV_H)
        b64 = png_b64(w2, h2, small)
        self._call_ui(lambda: self._set_image(self.before_img_lbl, b64))
        return self._job_reprocess(path)

    def _job_reprocess(self, path):
        """Worker job: apply current knobs to the loaded original and show
        the processed frame as the After preview."""
        if self._orig is None or self._orig_path != path:
            return False, "preview: stale selection"
        w, h, bgra = self._orig
        try:
            eng = self._engine()
            eng.apply_knobs(self._knobs)
            out = eng.process(bgra, w, h)
        except Exception as e:
            self._call_ui(lambda: self._set_text(
                self.after_img_lbl, "engine: %s" % e))
            return False, "engine: %s" % e
        w2, h2, small = downscale_nearest(w, h, out, PREV_W, PREV_H)
        b64 = png_b64(w2, h2, small)
        self._call_ui(lambda: self._set_image(self.after_img_lbl, b64))
        return True, "preview: %dx%d" % (w, h)

    def _preview_frame(self):
        item = self._selected_item()
        if item is None or item.kind != "video":
            return
        self._set_text(self.before_img_lbl, "...")
        self._set_text(self.after_img_lbl, "...")
        self.ctl.submit(self._job_video_frame, item.path)

    def _job_video_frame(self, path):
        """Worker job: decode frame 0 of a video, process it, show both."""
        ffmpeg, ffprobe = self.ffm.resolve(), self.ffm.ffprobe()
        if not ffmpeg or not ffprobe:
            return False, tr("media_ffmpeg_missing")
        try:
            job = VideoJob(ffmpeg, ffprobe, path, "-", self._engine())
            w, h, frame = job.grab_frame(0)
        except Exception as e:
            return False, "preview: %s" % e
        w2, h2, small = downscale_nearest(w, h, frame, PREV_W, PREV_H)
        b64 = png_b64(w2, h2, small)
        self._call_ui(lambda: self._set_image(self.before_img_lbl, b64))
        try:
            eng = self._engine()
            eng.apply_knobs(self._knobs)
            out = eng.process(frame, w, h)
        except Exception as e:
            return False, "engine: %s" % e
        w2, h2, small = downscale_nearest(w, h, out, PREV_W, PREV_H)
        b64 = png_b64(w2, h2, small)
        self._call_ui(lambda: self._set_image(self.after_img_lbl, b64))
        return True, "frame0 %dx%d" % (w, h)

    def _set_image(self, lbl, b64):
        img = tk.PhotoImage(data=b64)
        lbl.config(image=img, text="")
        lbl.image = img                 # keep the ref alive
        if lbl is self.before_img_lbl:
            self._before_img = img
        else:
            self._after_img = img

    def _set_text(self, lbl, text):
        lbl.config(image="", text=text)
        lbl.image = None

    # ------------------------------------------------------------- knobs --
    def _knob_changed(self, key, val):
        self._knobs[key] = val
        if key == "intensity":
            self._knobs["gain"] = float(val)   # gain mirrors intensity
        if self._deb_id is not None:
            self.ui.root.after_cancel(self._deb_id)
        self._deb_id = self.ui.root.after(DEBOUNCE_MS,
                                          self._debounced_preview)

    def _knobs_reset(self):
        """Phase30Panel already reset its own vars; mirror the 7 vendor
        knobs into _knobs and reprocess. blend/sharpness stay untouched."""
        p30 = dict(self.p30.get_values())
        self._knobs.update(p30)
        self._knobs["gain"] = float(p30.get("intensity", 1.0))
        self._debounced_preview()

    def _debounced_preview(self):
        """~350 ms after the last knob move: reprocess the selected photo
        on the worker (the live 'overlay for photos')."""
        self._deb_id = None
        item = self._selected_item()
        if item is None or item.kind != "photo":
            return
        if self._orig is None or self._orig_path != item.path:
            return
        self.ctl.submit(self._job_reprocess, item.path)

    # ------------------------------------------------------------- batch --
    def _save_mode_key(self):
        try:
            return self._mode_keys[self._mode_labels.index(
                self.mode_var.get())]
        except ValueError:
            return "subdir"

    def _process_all(self):
        if not self.queue.items:
            self._status(tr("media_queue_empty"))
            return
        batch_dir = None
        if self._save_mode_key() == "ask":
            batch_dir = filedialog.askdirectory(
                title=tr("media_pick_out"))
            if not batch_dir:
                return
        self.queue.reset()
        for it in self.queue.items:
            if it.status != "done":
                it.status = "queued"
        self._refresh_list()
        self.ctl.submit(self._job_run_all, batch_dir)

    def _job_run_all(self, batch_dir):
        """Worker job: the whole queue, sequentially."""
        try:
            eng = self._engine()
        except Exception as e:
            return False, "engine: %s" % e
        policy = media.SavePolicy(self.ctl.cfg)
        knobs = dict(self._knobs)

        def prog(item, i, total):
            now = time.monotonic()
            if now - self._prog_ts < PROGRESS_MIN_S:
                return
            self._prog_ts = now
            name = os.path.basename(item.path)
            self._call_ui(lambda: self.prog_lbl.config(
                text=tr("media_progress", name, i, total)))

        def done(item):
            self._call_ui(lambda: self._refresh_row(item))

        ok, msg = self.queue.run_all(
            eng, policy, self.ffm, knobs=knobs, batch_dir=batch_dir,
            progress_cb=prog, item_cb=done,
            log=lambda m: self.ui.log("mgr", m))
        self._call_ui(lambda: self._batch_done(ok, msg))
        return ok, tr("media_done", msg)

    def _batch_done(self, ok, msg):
        self._refresh_list()
        self.prog_lbl.config(text="")
        self._status(tr("media_done", msg))

    def _stop(self):
        self.queue.cancel()
        self._status(tr("media_status_cancel"))

    # ------------------------------------------------------- settings row --
    def _pick_outdir(self):
        d = filedialog.askdirectory(title=tr("media_outdir"))
        if d:
            self.outdir_var.set(d)
            self.ctl.cfg.set("media_out_dir", d)

    def _mode_changed(self, _e=None):
        self.ctl.cfg.set("media_save_mode", self._save_mode_key())

    def _ffmpeg_refresh(self):
        exe = self.ffm.resolve()
        if exe:
            self.ff_lbl.config(text=tr("media_ffmpeg_ok", exe))
            try:
                self.ff_btn.pack_forget()
            except tk.TclError:
                pass
        else:
            self.ff_lbl.config(text=tr("media_ffmpeg_missing"))
            if not self.ff_btn.winfo_ismapped():
                self.ff_btn.pack(side="right", padx=6)

    def _download_ffmpeg(self):
        self.ff_btn.state(["disabled"])
        self.ff_lbl.config(text=tr("media_dl_busy"))
        self.ctl.submit(self._job_download_ffmpeg)

    def _job_download_ffmpeg(self):
        try:
            exe = self.ffm.download()
        except Exception as e:
            self._call_ui(lambda: self._ffmpeg_failed(str(e)))
            return False, tr("media_dl_fail", e)
        self._call_ui(self._ffmpeg_refresh)
        return True, tr("media_dl_done", exe)

    def _ffmpeg_failed(self, err):
        self.ff_lbl.config(text=tr("media_dl_fail", err))
        self.ff_btn.state(["!disabled"])

    # ------------------------------------------------------------- misc ---
    def _status(self, text):
        self.status_lbl.config(text=text)
